// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/RebuildStandard.h"

#include <algorithm>
#include <csignal>
#include <map>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Core/Orca/DiscLayout.h"
#include "Core/Orca/DiscReport.h"
#include "Core/Orca/StandardBlob.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WIABlob.h"
#include "OrcaDiscTool/OutputFile.h"

namespace Orca::RebuildStandard
{
namespace
{
volatile std::sig_atomic_t s_cancelled = 0;

void Cancel(int)
{
  s_cancelled = 1;
}

class SignalScope
{
public:
  SignalScope() : m_previous(std::signal(SIGTERM, Cancel)) { s_cancelled = 0; }
  ~SignalScope()
  {
    std::signal(SIGTERM, m_previous);
    s_cancelled = 0;
  }

private:
  using Handler = void (*)(int);
  Handler m_previous;
};

std::optional<std::string> ReadJson(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file || file.GetSize() > 32 * 1024 * 1024)
    return {};
  std::string json(static_cast<size_t>(file.GetSize()), '\0');
  if (!file.ReadBytes(json.data(), json.size()))
    return {};
  return json;
}
}  // namespace

bool IsCancelled()
{
  return s_cancelled != 0;
}

bool VerifyRaw(DiscIO::BlobReader& expected, const DiscIO::Volume& actual, std::string* error)
{
  if (expected.GetDataSize() != actual.GetDataSize())
  {
    *error = "rebuilt disc logical size mismatch";
    return false;
  }
  std::vector<u8> expected_bytes(DiscIO::VolumeWii::GROUP_TOTAL_SIZE),
      actual_bytes(expected_bytes.size());
  for (u64 offset = 0; offset < expected.GetDataSize();)
  {
    if (IsCancelled())
    {
      *error = "cancelled";
      return false;
    }
    const u64 size = std::min<u64>(expected_bytes.size(), expected.GetDataSize() - offset);
    if (!expected.Read(offset, size, expected_bytes.data()) ||
        !actual.Read(offset, size, actual_bytes.data(), DiscIO::PARTITION_NONE))
    {
      *error = fmt::format("cannot verify rebuilt disc at offset {}", offset);
      return false;
    }
    const auto mismatch =
        std::mismatch(expected_bytes.begin(), expected_bytes.begin() + size, actual_bytes.begin());
    if (mismatch.first != expected_bytes.begin() + size)
    {
      *error = fmt::format("rebuilt disc byte mismatch at offset {}",
                           offset + std::distance(expected_bytes.begin(), mismatch.first));
      return false;
    }
    offset += size;
  }
  return true;
}

int Run(const std::string& image, const std::string& layout_path,
        const std::optional<std::string>& reference_path, const std::string& output, std::FILE* out,
        const std::function<void(u64 done, u64 total)>& progress)
{
  SignalScope signals;
  const auto fail = [&](std::string_view reason) {
    fmt::print(out, "{}\n", DiscReport::FailureLine(reason));
    return 1;
  };
  if (File::Exists(output))
    return fail("output already exists");
  const auto json = ReadJson(layout_path);
  if (!json)
    return fail("cannot read layout (maximum 32 MiB)");
  std::string error;
  const auto layout = DiscLayout::Parse(*json, &error);
  if (!layout)
    return fail(error);
  std::optional<DiscReport::Reference> reference;
  if (reference_path)
  {
    const auto ref_json = ReadJson(*reference_path);
    if (!ref_json)
      return fail("cannot read reference (maximum 32 MiB)");
    reference = DiscReport::ParseReference(*ref_json, &error);
    if (!reference)
      return fail(error);
    reference->partition_offset.reset();
  }
  fmt::print(stderr, "PROGRESS 0 1000\n");
  auto blob = StandardBlob::Create(image, *layout, &error);
  if (!blob)
  {
    if (error.starts_with("DIFF:"))
    {
      fmt::print(out, "{}\n", error);
      return 2;
    }
    return fail(error);
  }
  if (IsCancelled())
    return fail("cancelled");
  // Compare the normalized view: boot layout fields and FST have already been regenerated.
  auto view = DiscIO::CreateDisc(blob->CopyReader(), false);
  if (!view)
    return fail("cannot open rebuilt view");
  const auto normalized = DiscReport::Generate(*view, &error, RebuildStandard::IsCancelled);
  if (!normalized)
    return fail(error);
  if (reference)
  {
    // FST is derived from layout metadata, not copied from the rebuilt source.
    auto expected = *reference;
    auto actual = *normalized;
    std::erase_if(expected.files, [](auto& file) { return file.path == "sys/fst.bin"; });
    std::erase_if(actual.files, [](auto& file) { return file.path == "sys/fst.bin"; });
    const auto differences = DiscReport::Compare(actual, expected);
    if (!differences.empty())
    {
      fmt::print(out, "{}\n", DiscReport::VerdictLine(expected, differences));
      return 2;
    }
  }
  if (!blob->ComputeH3(&error))
    return fail(error);
  // A fresh volume must see the updated TMD/H3 rather than the lazy caches above.
  view = DiscIO::CreateDisc(blob->CopyReader(), false);
  if (!view || IsCancelled())
    return fail("cancelled or cannot open rebuilt view");
  const auto computed = DiscReport::Generate(*view, &error, RebuildStandard::IsCancelled);
  if (!computed)
    return fail(error);
  if (computed->header.h3_sha1 != layout->h3_sha1 ||
      computed->header.tmd_content_sha1 != layout->tmd_content_sha1)
  {
    if (!reference)
    {
      fmt::print(out, "DIFF: {} differs from standard layout\n",
                 computed->header.h3_sha1 != layout->h3_sha1 ? "H3 SHA-1" : "TMD content hash");
      return 2;
    }
    return fail(computed->header.h3_sha1 != layout->h3_sha1 ?
                    "rebuilt disc H3 SHA-1 does not match the standard layout" :
                    "rebuilt disc TMD content hash does not match the standard layout");
  }
  if (!reference)
    reference = DiscReport::Reference{layout->id, computed->files, std::nullopt};
  fmt::print(stderr, "PROGRESS 300 1000\n");
  const auto partial = DiscTool::OutputFile::Create(output);
  if (!partial)
    return fail("cannot reserve rebuilt output");
  const bool converted = DiscIO::ConvertToWIAOrRVZ(
      blob.get(), image, partial->Path(), true, DiscIO::WIARVZCompressionType::Zstd, 5, 128 * 1024,
      [&](const std::string&, float fraction) {
        const u64 done = 300 + static_cast<u64>(std::clamp(fraction, 0.0f, 1.0f) * 600);
        fmt::print(stderr, "PROGRESS {} 1000\n", done);
        if (progress)
          progress(done, 1000);
        return !IsCancelled();
      },
      view.get());
  if (!converted || IsCancelled())
    return fail(IsCancelled() ? "cancelled" : "cannot write rebuilt RVZ");
  fmt::print(stderr, "PROGRESS 900 1000\n");
  auto reopened = DiscIO::CreateDisc(partial->Path(), false);
  if (!reopened)
    return fail("cannot reopen rebuilt RVZ");
  if (!VerifyRaw(*blob, *reopened, &error))
    return fail(error);
  const auto verified = DiscReport::Generate(*reopened, &error, RebuildStandard::IsCancelled);
  if (IsCancelled())
    return fail("cancelled");
  if (!verified)
    return fail(error);
  if (verified->header.partition_offset != layout->partition_offset)
    return fail("rebuilt disc partitionOffset does not match the standard layout");
  if (verified->header.h3_sha1 != layout->h3_sha1)
    return fail("rebuilt disc H3 SHA-1 does not match the standard layout");
  if (verified->header.tmd_content_sha1 != layout->tmd_content_sha1)
    return fail("rebuilt disc TMD content hash does not match the standard layout");
  const auto differences = DiscReport::Compare(*verified, *reference);
  if (!differences.empty())
    return fail(fmt::format("rebuilt disc file hash/size/set mismatch: {}", differences.front()));
  // Close readers before publishing/cleanup (required on Windows).
  reopened.reset();
  if (!partial->Publish(output))
    return fail("cannot publish rebuilt RVZ; destination may already exist");
  fmt::print(stderr, "PROGRESS 1000 1000\n");
  fmt::print(out, "PASS: rebuilt {} to the standard layout\n", layout->id);
  return 0;
}

std::optional<int> RunCommandLine(int argc, char* argv[])
{
  bool requested = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string_view arg(argv[i]);
    if (arg == "--")
      break;
    const auto option = arg.substr(0, arg.find('='));
    requested |= option == "--disc-layout" || option == "--rebuild-standard" ||
                 option == "--layout" || option == "--out" || option == "--disc-report";
  }
  if (!requested)
    return {};
  Common::RegisterMsgAlertHandler(
      [](const char*, const char*, bool, Common::MsgType) { return false; });
  const auto fail = [](const char* reason) {
    fmt::print(stdout, "{}\n", DiscReport::FailureLine(reason));
    return 1;
  };
  std::map<std::string, std::string> options;
  for (int i = 1; i < argc; ++i)
  {
    const std::string_view arg(argv[i]);
    const size_t equals = arg.find('=');
    const std::string option(arg.substr(0, equals));
    if (option != "--disc-layout" && option != "--rebuild-standard" && option != "--layout" &&
        option != "--out" && option != "--against" && option != "--disc-report")
      return fail("unrecognized standalone disc option");
    if (options.contains(option))
      return fail("duplicate standalone disc option");
    std::string value;
    if (equals != std::string_view::npos)
      value = arg.substr(equals + 1);
    else if (++i < argc && argv[i][0] != '-')
      value = argv[i];
    if (value.empty())
      return fail("standalone disc option needs a path");
    options.emplace(option, std::move(value));
  }
  std::optional<std::string> reference;
  if (options.contains("--against"))
    reference = options.at("--against");
  if (options.contains("--disc-report"))
  {
    if (options.size() != (reference ? 2u : 1u))
      return fail("--disc-report accepts only an image and optional --against reference");
    return DiscReport::Run(options.at("--disc-report"), reference, stdout);
  }
  if (!options.contains("--out"))
    return fail("standalone disc command requires --out");
  if (options.contains("--disc-layout"))
  {
    if (options.size() != 2)
      return fail("--disc-layout accepts only an image and --out layout.json");
    SignalScope signals;
    return DiscLayout::Run(options.at("--disc-layout"), options.at("--out"), stdout);
  }
  if (!options.contains("--rebuild-standard") || !options.contains("--layout"))
    return fail("--rebuild-standard requires an image, --layout and --out");
  return Run(options.at("--rebuild-standard"), options.at("--layout"), reference,
             options.at("--out"), stdout);
}
}  // namespace Orca::RebuildStandard
