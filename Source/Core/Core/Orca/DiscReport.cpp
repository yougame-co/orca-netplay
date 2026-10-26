// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/DiscReport.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/Crypto/SHA1.h"
#include "Common/IOFile.h"
#include "Core/Orca/SafeJson.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace Orca::DiscReport
{
namespace
{
constexpr u64 MAX_JSON_INTEGER = (u64{1} << 53) - 1;
constexpr u64 MAX_REFERENCE_BYTES = 32 * 1024 * 1024;

struct FileRange
{
  std::string path;
  u64 offset;
  u64 size;
};

std::string LowerSha1(std::string sha1);

std::string OneLine(std::string_view text)
{
  std::string result(text);
  for (char& c : result)
  {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
      c = ' ';
  }
  return result;
}

bool ValidPath(std::string_view path)
{
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find('\0') != std::string_view::npos)
    return false;
  while (!path.empty())
  {
    const size_t slash = path.find('/');
    const auto component = path.substr(0, slash);
    if (component.empty() || component == "." || component == "..")
      return false;
    if (slash == std::string_view::npos)
      break;
    path.remove_prefix(slash + 1);
  }
  return true;
}

bool CollectFiles(const DiscIO::FileInfo& directory, const std::string& parent,
                  std::vector<FileRange>* files)
{
  for (const auto& child : directory)
  {
    const std::string name = child.GetName();
    if (name.find('/') != std::string::npos || !ValidPath(name))
      return false;
    const std::string path = parent + name;
    if (child.IsDirectory())
    {
      if (!CollectFiles(child, path + '/', files))
        return false;
    }
    else
    {
      files->push_back({path, child.GetOffset(), child.GetSize()});
    }
  }
  return true;
}
}  // namespace

std::optional<std::string> Detail::HashRange(const DiscIO::Volume& volume,
                                          const DiscIO::Partition& partition, std::string_view path,
                                          u64 offset, u64 size)
{
  if (offset > MAX_JSON_INTEGER || size > MAX_JSON_INTEGER - offset)
    return std::nullopt;
  auto context = Common::SHA1::CreateContext();
  std::vector<u8> buffer(static_cast<size_t>(std::min<u64>(1024 * 1024, size)));
  for (u64 position = 0; position < size;)
  {
    const size_t length = static_cast<size_t>(std::min<u64>(buffer.size(), size - position));
    if (!volume.Read(offset + position, length, buffer.data(), partition))
      return std::nullopt;
    // Disc tools can rename a title without changing the game. Mask only that field.
    if (path == "sys/boot.bin" && position == 0 && length >= 0x60)
      std::fill(buffer.begin() + 0x20, buffer.begin() + 0x60, 0);
    context->Update(buffer.data(), length);
    position += length;
  }
  return LowerSha1(Common::SHA1::DigestToString(context->Finish()));
}

namespace
{
std::optional<u64> DolSize(const DiscIO::Volume& volume, const DiscIO::Partition& partition,
                           u64 offset)
{
  // Include the header, and add section sizes as u64 to avoid wrapping malformed DOLs.
  u64 end = 0x100;
  for (u64 i = 0; i < 18; ++i)
  {
    const auto start = volume.ReadSwapped<u32>(offset + i * 4, partition);
    const auto size = volume.ReadSwapped<u32>(offset + 0x90 + i * 4, partition);
    if (!start || !size)
      return std::nullopt;
    if (*size != 0)
      end = std::max(end, static_cast<u64>(*start) + *size);
  }
  return end;
}

std::optional<u64> Integer(const picojson::value& value)
{
  if (!value.is<double>())
    return std::nullopt;
  const double number = value.get<double>();
  if (!std::isfinite(number) || number < 0 || number > MAX_JSON_INTEGER ||
      number != std::floor(number))
    return std::nullopt;
  return static_cast<u64>(number);
}

bool ValidSha1(std::string_view sha1)
{
  return sha1.size() == 40 && std::all_of(sha1.begin(), sha1.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
         });
}

std::string LowerSha1(std::string sha1)
{
  for (char& c : sha1)
  {
    if (c >= 'A' && c <= 'F')
      c += 'a' - 'A';
  }
  return sha1;
}
}  // namespace

std::optional<Report> Generate(const std::string& image, std::string* error)
{
  error->clear();
  const auto fail = [&](std::string reason) -> std::optional<Report> {
    *error = std::move(reason);
    return std::nullopt;
  };
  const auto volume = DiscIO::CreateVolume(image);
  if (!volume || !DiscIO::IsDisc(volume->GetVolumeType()))
    return fail("cannot open a GameCube or Wii disc image");

  const bool wii = volume->GetVolumeType() == DiscIO::Platform::WiiDisc;
  const auto partition = volume->GetGamePartition();
  if (wii && partition == DiscIO::PARTITION_NONE)
    return fail("game partition is missing");

  Report report;
  report.header.id = volume->GetGameID(partition);
  const auto revision = volume->GetRevision(partition);
  if (report.header.id.size() != 6 || !revision)
    return fail("cannot read disc header");
  report.header.revision = *revision;
  if (wii)
  {
    report.header.partition_offset = partition.offset;
    const auto& tmd = volume->GetTMD(partition);
    if (!tmd.IsValid())
      return fail("cannot read game partition TMD");
    const auto contents = tmd.GetContents();
    if (contents.size() != 1)
      return fail("game partition TMD must contain one content");
    report.header.tmd_content_sha1 = LowerSha1(Common::SHA1::DigestToString(contents[0].sha1));
    const auto h3_offset = volume->ReadSwappedAndShifted(
        partition.offset + DiscIO::WII_PARTITION_H3_OFFSET_ADDRESS, DiscIO::PARTITION_NONE);
    if (!h3_offset)
      return fail("cannot read game partition H3 offset");
    report.header.h3_sha1 =
        Detail::HashRange(*volume, DiscIO::PARTITION_NONE, "h3", partition.offset + *h3_offset,
                          DiscIO::WII_PARTITION_H3_SIZE);
    if (!report.header.h3_sha1)
      return fail("cannot read game partition H3 table");
  }

  const auto* fs = volume->GetFileSystem(partition);
  if (!fs)
    return fail("cannot read game partition filesystem");
  const auto dol_offset = DiscIO::GetBootDOLOffset(*volume, partition);
  const auto dol_size = dol_offset ? DolSize(*volume, partition, *dol_offset) : std::nullopt;
  const auto apploader_size = DiscIO::GetApploaderSize(*volume, partition);
  const auto fst_offset = DiscIO::GetFSTOffset(*volume, partition);
  const auto fst_size = DiscIO::GetFSTSize(*volume, partition);
  if (!dol_offset || !dol_size || !apploader_size || !fst_offset || !fst_size)
    return fail("cannot read system file locations");
  std::vector<FileRange> files = {
      {"sys/boot.bin", DiscIO::DISCHEADER_ADDRESS, DiscIO::DISCHEADER_SIZE},
      {"sys/bi2.bin", DiscIO::BI2_ADDRESS, DiscIO::BI2_SIZE},
      {"sys/apploader.img", DiscIO::APPLOADER_ADDRESS, *apploader_size},
      {"sys/main.dol", *dol_offset, *dol_size},
      {"sys/fst.bin", *fst_offset, *fst_size},
  };
  if (!CollectFiles(fs->GetRoot(), "", &files))
    return fail("invalid file path in game partition");
  std::set<std::string> paths;
  for (const auto& file : files)
  {
    if (!paths.insert(file.path).second)
      return fail("duplicate file path: " + file.path);
  }
  // Physical order keeps the full-disc read sequential even when FST order is different.
  std::sort(files.begin(), files.end(),
            [](const FileRange& a, const FileRange& b) { return a.offset < b.offset; });
  for (const auto& file : files)
  {
    const auto sha1 = Detail::HashRange(*volume, partition, file.path, file.offset, file.size);
    if (!sha1)
      return fail("cannot read " + file.path);
    report.files.push_back({file.path, file.size, *sha1});
  }
  std::sort(report.files.begin(), report.files.end(),
            [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
  return report;
}

std::optional<Reference> ParseReference(std::string_view json, std::string* error)
{
  error->clear();
  const auto fail = [&](const char* reason) -> std::optional<Reference> {
    *error = reason;
    return std::nullopt;
  };
  picojson::value root;
  if (json.size() > MAX_REFERENCE_BYTES || !Json::ParseWholeJson(json, &root).empty() ||
      !root.is<picojson::object>())
    return fail("invalid reference JSON");
  const auto& name = root.get("name");
  const auto& files = root.get("files");
  if (!name.is<std::string>() || name.get<std::string>().empty() || !files.is<picojson::array>())
    return fail("reference needs a name and files array");
  Reference reference;
  reference.name = name.get<std::string>();
  if (root.contains("partitionOffset"))
  {
    reference.partition_offset = Integer(root.get("partitionOffset"));
    if (!reference.partition_offset)
      return fail("invalid reference partitionOffset");
  }
  std::set<std::string> paths;
  for (const auto& file : files.get<picojson::array>())
  {
    if (!file.is<picojson::object>())
      return fail("invalid reference file entry");
    const auto& path = file.get("path");
    const auto& sha1 = file.get("sha1");
    const auto size = Integer(file.get("size"));
    if (!path.is<std::string>() || !ValidPath(path.get<std::string>()) || !size ||
        !sha1.is<std::string>() || !ValidSha1(sha1.get<std::string>()))
      return fail("reference file needs a path, integer size and SHA-1 hex digest");
    if (!paths.insert(path.get<std::string>()).second)
      return fail("duplicate reference file path");
    reference.files.push_back({path.get<std::string>(), *size, LowerSha1(sha1.get<std::string>())});
  }
  return reference;
}

std::vector<std::string> Compare(const Report& report, const Reference& reference)
{
  std::map<std::string, const FileEntry*> actual;
  for (const auto& file : report.files)
    actual.emplace(file.path, &file);
  std::set<std::string> differences;
  for (const auto& expected : reference.files)
  {
    const auto it = actual.find(expected.path);
    if (it == actual.end())
    {
      differences.insert(expected.path);
      continue;
    }
    if (it->second->size != expected.size || it->second->sha1 != expected.sha1)
      differences.insert(expected.path);
    actual.erase(it);
  }
  for (const auto& [path, file] : actual)
    differences.insert(path);
  if (reference.partition_offset && reference.partition_offset != report.header.partition_offset)
    differences.insert("partitionOffset");
  return {differences.begin(), differences.end()};
}

std::string HeaderLine(const Header& header)
{
  picojson::object object;
  object["id"] = picojson::value(header.id);
  object["revision"] = picojson::value(static_cast<double>(header.revision));
  if (header.tmd_content_sha1)
    object["tmdContentSha1"] = picojson::value(*header.tmd_content_sha1);
  if (header.h3_sha1)
    object["h3Sha1"] = picojson::value(*header.h3_sha1);
  if (header.partition_offset)
    object["partitionOffset"] = picojson::value(static_cast<double>(*header.partition_offset));
  return picojson::value(std::move(object)).serialize();
}

std::string FileLine(const FileEntry& file)
{
  return picojson::value(picojson::object{{"path", picojson::value(file.path)},
                                          {"size", picojson::value(static_cast<double>(file.size))},
                                          {"sha1", picojson::value(file.sha1)}})
      .serialize();
}

std::string VerdictLine(const Reference& reference, const std::vector<std::string>& differences)
{
  if (differences.empty())
    return "PASS: files match " + OneLine(reference.name);
  std::string paths;
  for (size_t i = 0; i < std::min<size_t>(5, differences.size()); ++i)
  {
    if (i != 0)
      paths += ", ";
    paths += OneLine(differences[i]);
  }
  return fmt::format("DIFF: {} files differ ({})", differences.size(), paths);
}

std::string FailureLine(std::string_view reason)
{
  return "FAIL: " + OneLine(reason);
}

int Run(const std::string& image, const std::optional<std::string>& reference_path, std::FILE* out)
{
  const auto fail = [&](std::string_view error) {
    fmt::print(out, "{}\n", FailureLine(error));
    return 1;
  };
  std::string error;
  std::optional<Reference> reference;
  if (reference_path)
  {
    File::IOFile file(*reference_path, "rb");
    if (!file || file.GetSize() > MAX_REFERENCE_BYTES)
      return fail("cannot read reference (maximum 32 MiB)");
    std::string json(static_cast<size_t>(file.GetSize()), '\0');
    if (!file.ReadBytes(json.data(), json.size()))
      return fail("cannot read reference");
    reference = ParseReference(json, &error);
    if (!reference)
      return fail(error);
  }
  const auto report = Generate(image, &error);
  if (!report)
    return fail(error);
  fmt::print(out, "{}\n", HeaderLine(report->header));
  for (const auto& file : report->files)
    fmt::print(out, "{}\n", FileLine(file));
  if (!reference)
    reference = Reference{report->header.id, report->files, std::nullopt};
  const auto differences = Compare(*report, *reference);
  fmt::print(out, "{}\n", VerdictLine(*reference, differences));
  return differences.empty() ? 0 : 2;
}
}  // namespace Orca::DiscReport
