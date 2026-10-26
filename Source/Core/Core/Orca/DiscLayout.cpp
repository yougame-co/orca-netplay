// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/DiscLayout.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/Crypto/AES.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Swap.h"
#include "Core/Orca/DiscReport.h"
#include "Core/Orca/RebuildStandard.h"
#include "Core/Orca/SafeJson.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeWii.h"
#include "OrcaDiscTool/OutputFile.h"

namespace Orca::DiscLayout
{
namespace
{
using Wii = DiscIO::VolumeWii;
constexpr u64 MAX_DISC_SIZE = 0x400000000ULL;
constexpr u64 MAX_FST_SIZE = 32 * 1024 * 1024;

picojson::value Number(u64 n)
{
  return picojson::value(static_cast<double>(n));
}

picojson::value RangeJson(const Range& range)
{
  return picojson::value(
      picojson::object{{"offset", Number(range.offset)}, {"size", Number(range.size)}});
}

bool ReadNumber(const picojson::value& value, u64* result)
{
  if (!value.is<double>())
    return false;
  const double n = value.get<double>();
  if (!std::isfinite(n) || n < 0 || n > MAX_DISC_SIZE || n != std::floor(n))
    return false;
  *result = static_cast<u64>(n);
  return true;
}

bool ReadRange(const picojson::value& value, Range* range)
{
  return value.is<picojson::object>() && ReadNumber(value.get("offset"), &range->offset) &&
         ReadNumber(value.get("size"), &range->size);
}

bool ReadString(const picojson::value& value, std::string* result)
{
  if (!value.is<std::string>())
    return false;
  *result = value.get<std::string>();
  return true;
}

bool IsHash(const std::string& hash)
{
  return hash.size() == 40 && std::all_of(hash.begin(), hash.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool Fits(const Range& range, u64 end)
{
  return range.offset <= end && range.size <= end - range.offset;
}

void Write32(std::vector<u8>* bytes, size_t offset, u64 n)
{
  const u32 be = Common::swap32(static_cast<u32>(n));
  std::memcpy(bytes->data() + offset, &be, sizeof(be));
}

std::string DigestHex(const Common::SHA1::Digest& digest)
{
  std::string result;
  for (u8 byte : digest)
    result += fmt::format("{:02x}", byte);
  return result;
}

std::vector<bool> ZeroSectors(const Layout& layout)
{
  std::vector<bool> zero(layout.data.size / Wii::BLOCK_TOTAL_SIZE);
  for (const auto& gap : layout.gaps)
  {
    if (gap.physical_zero)
      std::fill(zero.begin() + gap.offset / Wii::BLOCK_DATA_SIZE,
                zero.begin() + (gap.offset + gap.size) / Wii::BLOCK_DATA_SIZE, true);
  }
  return zero;
}

std::map<std::pair<u64, u64>, Common::SHA1::Digest> ErasedHashSlots(const Layout& layout)
{
  const auto zero = ZeroSectors(layout);
  std::map<std::pair<u64, u64>, Common::SHA1::Digest> slots;
  for (size_t group = 0; group < zero.size(); group += Wii::BLOCKS_PER_GROUP)
  {
    const size_t end = std::min(group + Wii::BLOCKS_PER_GROUP, zero.size());
    if (std::all_of(zero.begin() + group, zero.begin() + end, [](bool z) { return z; }))
    {
      slots[{3, group / Wii::BLOCKS_PER_GROUP}] = {};
      continue;
    }
    for (size_t base = group; base < end; base += 8)
    {
      const size_t sub_end = std::min(base + 8, end);
      if (std::all_of(zero.begin() + base, zero.begin() + sub_end, [](bool z) { return z; }))
        slots[{2, base / 8}] = {};
      else
        for (size_t block = base; block < sub_end; ++block)
          if (zero[block])
            slots[{1, block}] = {};
    }
  }
  return slots;
}
}  // namespace

/*
 * Seed formula ported from NKit 2: NKit/Formats/Nintendo/WiiGc/NJunk.cs via gc-junk.ts.
 * MIT License
 * Copyright (c) 2018 Nanook
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
JunkSeed FormulaSeed(std::string_view id, u8 disc_number, u64 offset)
{
  const u32 a = static_cast<u8>(id[0]);
  const u32 b = static_cast<u8>(id[1]);
  const u32 c = static_cast<u8>(id[2]);
  const u32 d = static_cast<u8>(id[3]);
  const u32 disc_seed = ((c << 24) | (b << 16) | ((d + c) << 8) | (a + b)) ^ disc_number;
  u32 x = disc_seed * 0x0260bcd5 ^ (static_cast<u32>(offset / JUNK_UNIT_SIZE) * 0x1ef29123);
  JunkSeed seed;
  for (u32& word : seed)
  {
    u32 value = 0;
    for (size_t bit = 0; bit < 32; ++bit)
    {
      x = x * 0x5d588b65 + 1;
      value = (value >> 1) | (x & 0x80000000);
    }
    word = Common::swap32(value);
  }
  seed[16] ^= Common::swap32((Common::swap32(seed[0]) >> 9) ^ (Common::swap32(seed[16]) << 23));
  return seed;
}

u64 Layout::DecryptedSize() const
{
  return data.size / Wii::BLOCK_TOTAL_SIZE * Wii::BLOCK_DATA_SIZE;
}

const SystemFile* Layout::FindSystemFile(std::string_view path) const
{
  const auto it = std::find_if(system_files.begin(), system_files.end(),
                               [&](const auto& file) { return file.path == path; });
  return it == system_files.end() ? nullptr : &*it;
}

std::vector<SystemFile> Files(const Layout& layout)
{
  std::vector<SystemFile> files = layout.system_files;
  std::vector<std::pair<size_t, std::string>> directories;
  for (size_t i = 0; i < layout.entries.size(); ++i)
  {
    const auto& entry = layout.entries[i];
    while (!directories.empty() && i >= directories.back().first)
      directories.pop_back();
    const std::string path = (directories.empty() ? "" : directories.back().second) + entry.name;
    if (entry.directory)
      directories.emplace_back(entry.next, i == 0 ? "" : path + '/');
    else
      files.push_back({{entry.offset, entry.size}, path});
  }
  return files;
}

std::optional<std::vector<u8>> BuildFST(const Layout& layout)
{
  const auto* fst = layout.FindSystemFile("sys/fst.bin");
  if (!fst || fst->size > MAX_FST_SIZE || layout.entries.empty() ||
      layout.entries.size() > fst->size / 12)
    return std::nullopt;
  const size_t strings = layout.entries.size() * 12;
  std::vector<u8> result(static_cast<size_t>(fst->size));
  std::vector<bool> assigned(result.size() - strings);
  for (size_t i = 0; i < layout.entries.size(); ++i)
  {
    const auto& entry = layout.entries[i];
    if (entry.name_offset > 0xffffff || entry.offset / 4 > 0xffffffff || entry.size > 0xffffffff ||
        entry.parent > 0xffffffff || entry.next > 0xffffffff)
      return std::nullopt;
    Write32(&result, i * 12, (entry.directory ? 0x01000000 : 0) | entry.name_offset);
    Write32(&result, i * 12 + 4, entry.directory ? entry.parent : entry.offset / 4);
    Write32(&result, i * 12 + 8, entry.directory ? entry.next : entry.size);
    if (i == 0)
      continue;
    if (entry.name_offset >= assigned.size() ||
        entry.name.size() >= assigned.size() - entry.name_offset)
      return std::nullopt;
    for (size_t j = 0; j <= entry.name.size(); ++j)
    {
      const size_t p = entry.name_offset + j;
      const u8 c = j == entry.name.size() ? 0 : static_cast<u8>(entry.name[j]);
      if (assigned[p] && result[strings + p] != c)
        return std::nullopt;
      result[strings + p] = c;
      assigned[p] = true;
    }
  }
  return result;
}

bool Validate(const Layout& layout, std::string* error)
{
  const auto fail = [&](const char* reason) {
    *error = reason;
    return false;
  };
  if (layout.id.size() != 6 ||
      !std::all_of(layout.id.begin(), layout.id.end(),
                   [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }) ||
      !IsHash(layout.h3_sha1) || !IsHash(layout.tmd_content_sha1))
    return fail("invalid layout identity or hashes");
  if (layout.partition_offset < 0x50000 || layout.partition_offset >= MAX_DISC_SIZE ||
      layout.partition_offset % 0x8000 != 0 || layout.data.offset < 0x2c0 ||
      layout.data.offset > 16 * 1024 * 1024 || layout.data.offset % 0x8000 != 0 ||
      layout.data.size == 0 || layout.data.size % Wii::BLOCK_TOTAL_SIZE != 0 ||
      !Fits(layout.data, MAX_DISC_SIZE - layout.partition_offset) ||
      (layout.data.size / Wii::GROUP_TOTAL_SIZE + (layout.data.size % Wii::GROUP_TOTAL_SIZE != 0)) *
              20 >
          DiscIO::WII_PARTITION_H3_SIZE)
    return fail("invalid layout partition extent");
  std::vector<Range> headers{{0, 0x2c0}, layout.tmd, layout.cert, layout.h3};
  std::sort(headers.begin(), headers.end(), [](auto a, auto b) { return a.offset < b.offset; });
  u64 end = 0;
  for (const auto& range : headers)
  {
    if (!Fits(range, layout.data.offset) || (range.size && range.offset < end) ||
        range.offset % 4 != 0)
      return fail("overlapping or invalid partition header ranges");
    if (range.size)
      end = range.offset + range.size;
  }
  if (layout.h3.size != DiscIO::WII_PARTITION_H3_SIZE ||
      layout.tmd.size < sizeof(IOS::ES::TMDHeader) + sizeof(IOS::ES::Content) ||
      layout.tmd.size > IOS::ES::MAX_TMD_SIZE)
    return fail("invalid layout TMD or H3 size");
  if (layout.partitions.empty() || layout.partitions.size() > 256 ||
      std::count_if(layout.partitions.begin(), layout.partitions.end(), [&](auto p) {
        return p.type == 0 && p.offset == layout.partition_offset;
      }) != 1)
    return fail("layout game partition is missing or ambiguous");
  for (const auto& partition : layout.partitions)
  {
    if (partition.offset < 0x50000 || partition.offset >= MAX_DISC_SIZE || partition.offset % 4)
      return fail("invalid partition table entry");
  }
  const auto* boot = layout.FindSystemFile("sys/boot.bin");
  const auto* bi2 = layout.FindSystemFile("sys/bi2.bin");
  const auto* app = layout.FindSystemFile("sys/apploader.img");
  const auto* dol = layout.FindSystemFile("sys/main.dol");
  const auto* fst = layout.FindSystemFile("sys/fst.bin");
  if (layout.system_files.size() != 5 || !boot || !bi2 || !app || !dol || !fst ||
      boot->offset != 0 || boot->size != 0x440 || bi2->offset != 0x440 || bi2->size != 0x2000 ||
      app->offset != 0x2440 || app->size < 0x20 || dol->size < 0x100 || fst->size > MAX_FST_SIZE ||
      fst->size % 4 || layout.fst_max_size % 4 || layout.fst_max_size < fst->size ||
      layout.fst_max_size / 4 > 0xffffffff)
    return fail("invalid layout system files");
  if (layout.entries.empty() || !layout.entries[0].directory || !layout.entries[0].name.empty() ||
      layout.entries[0].parent != 0 || layout.entries[0].next != layout.entries.size())
    return fail("invalid layout FST root");
  std::vector<size_t> stack{0};
  std::set<std::pair<size_t, std::string>> siblings;
  for (size_t i = 1; i < layout.entries.size(); ++i)
  {
    while (!stack.empty() && i >= layout.entries[stack.back()].next)
      stack.pop_back();
    const auto& entry = layout.entries[i];
    if (stack.empty() || entry.name.empty() || entry.name == "." || entry.name == ".." ||
        entry.name.find_first_of("/\\") != std::string::npos ||
        entry.name.find('\0') != std::string::npos)
      return fail("invalid layout FST name or tree");
    if (!siblings.emplace(stack.back(), entry.name).second)
      return fail("duplicate layout FST name");
    if (entry.directory)
    {
      if (entry.parent != stack.back() || entry.next <= i ||
          entry.next > layout.entries[stack.back()].next)
        return fail("invalid layout FST directory indices");
      stack.push_back(i);
    }
  }
  if (!BuildFST(layout))
    return fail("layout FST cannot be regenerated");
  std::set<std::string> paths;
  std::vector<Range> coverage;
  for (const auto& file : Files(layout))
  {
    if (!paths.insert(file.path).second || file.offset % 4 || !Fits(file, layout.DecryptedSize()))
      return fail("duplicate or invalid layout file range");
    if (file.size)
      coverage.push_back(file);
  }
  for (const auto& gap : layout.gaps)
  {
    if (gap.size == 0 || !Fits(gap, layout.DecryptedSize()) ||
        (gap.kind != "zero" && gap.kind != "junk" && gap.kind != "seeded" && gap.kind != "other") ||
        (gap.physical_zero && (gap.kind != "zero" || gap.offset % Wii::BLOCK_DATA_SIZE ||
                               gap.size % Wii::BLOCK_DATA_SIZE)) ||
        (gap.kind == "seeded" ?
             gap.seeds.size() !=
                 (gap.offset % JUNK_UNIT_SIZE + gap.size + JUNK_UNIT_SIZE - 1) / JUNK_UNIT_SIZE :
             !gap.seeds.empty()))
      return fail("invalid layout gap");
    coverage.push_back(gap);
  }
  std::sort(coverage.begin(), coverage.end(), [](auto a, auto b) { return a.offset < b.offset; });
  end = 0;
  for (const auto& range : coverage)
  {
    if (range.offset != end)
      return fail("overlapping files or incomplete layout gap coverage");
    end += range.size;
  }
  if (end != layout.DecryptedSize())
    return fail("incomplete layout data coverage");
  if (!layout.erased_hashes.empty())
  {
    auto slots = ErasedHashSlots(layout);
    for (const auto& hash : layout.erased_hashes)
      if (slots.erase({hash.level, hash.index}) != 1)
        return fail("duplicate or invalid erased hash slot");
    if (!slots.empty())
      return fail("incomplete erased hash metadata");
  }
  error->clear();
  return true;
}

std::string Serialize(const Layout& layout)
{
  // Older readers must reject layouts whose zero gaps require ciphertext semantics.
  const bool physical_zero = std::any_of(layout.gaps.begin(), layout.gaps.end(),
                                         [](const auto& gap) { return gap.physical_zero; });
  const u64 version = !layout.erased_hashes.empty() ? FORMAT_VERSION : physical_zero ? 2 : 1;
  picojson::array partitions, files, entries, gaps, erased_hashes;
  for (const auto& partition : layout.partitions)
    partitions.emplace_back(
        picojson::object{{"offset", Number(partition.offset)}, {"type", Number(partition.type)}});
  for (const auto& file : layout.system_files)
  {
    auto object = RangeJson(file).get<picojson::object>();
    object["path"] = picojson::value(file.path);
    files.emplace_back(std::move(object));
  }
  for (const auto& entry : layout.entries)
  {
    picojson::object object{{"name", picojson::value(entry.name)},
                            {"directory", picojson::value(entry.directory)},
                            {"nameOffset", Number(entry.name_offset)}};
    if (entry.directory)
    {
      object["parent"] = Number(entry.parent);
      object["next"] = Number(entry.next);
    }
    else
    {
      object["offset"] = Number(entry.offset);
      object["size"] = Number(entry.size);
    }
    entries.emplace_back(std::move(object));
  }
  for (const auto& gap : layout.gaps)
  {
    auto object = RangeJson(gap).get<picojson::object>();
    object["kind"] = picojson::value(gap.kind);
    if (gap.physical_zero)
      object["physicalZero"] = picojson::value(true);
    if (gap.kind == "seeded")
    {
      picojson::array seeds;
      for (const auto& seed : gap.seeds)
      {
        std::string hex;
        for (u32 word : seed)
          hex += fmt::format("{:08x}", Common::swap32(word));
        seeds.emplace_back(std::move(hex));
      }
      object["seeds"] = picojson::value(std::move(seeds));
    }
    gaps.emplace_back(std::move(object));
  }
  for (const auto& hash : layout.erased_hashes)
    erased_hashes.emplace_back(
        picojson::object{{"level", Number(hash.level)},
                         {"index", Number(hash.index)},
                         {"digest", picojson::value(DigestHex(hash.digest))}});
  picojson::object object{{"version", Number(version)},
                          {"id", picojson::value(layout.id)},
                          {"discNumber", Number(layout.disc_number)},
                          {"h3Sha1", picojson::value(layout.h3_sha1)},
                          {"tmdContentSha1", picojson::value(layout.tmd_content_sha1)},
                          {"partitions", picojson::value(partitions)},
                          {"partitionOffset", Number(layout.partition_offset)},
                          {"tmd", RangeJson(layout.tmd)},
                          {"cert", RangeJson(layout.cert)},
                          {"h3", RangeJson(layout.h3)},
                          {"data", RangeJson(layout.data)},
                          {"fstMaxSize", Number(layout.fst_max_size)},
                          {"systemFiles", picojson::value(files)},
                          {"fst", picojson::value(entries)},
                          {"gaps", picojson::value(gaps)}};
  if (version == FORMAT_VERSION)
    object["erasedHashes"] = picojson::value(std::move(erased_hashes));
  return picojson::value(std::move(object)).serialize(true) + '\n';
}

std::optional<Layout> Parse(std::string_view json, std::string* error)
{
  const auto fail = [&]() -> std::optional<Layout> {
    *error = "invalid layout JSON";
    return {};
  };
  picojson::value root;
  if (json.size() > MAX_FST_SIZE || !Json::ParseWholeJson(json, &root).empty() ||
      !root.is<picojson::object>())
    return fail();
  Layout layout;
  u64 version = 0;
  u64 disc_number = 0;
  if (root.contains("discNumber") &&
      (!ReadNumber(root.get("discNumber"), &disc_number) || disc_number > 255))
    return fail();
  layout.disc_number = static_cast<u8>(disc_number);
  if (!ReadNumber(root.get("version"), &version) ||
      (version != 1 && version != 2 && version != FORMAT_VERSION) ||
      !ReadString(root.get("id"), &layout.id) ||
      !ReadString(root.get("h3Sha1"), &layout.h3_sha1) ||
      !ReadString(root.get("tmdContentSha1"), &layout.tmd_content_sha1) ||
      !ReadNumber(root.get("partitionOffset"), &layout.partition_offset) ||
      !ReadNumber(root.get("fstMaxSize"), &layout.fst_max_size) ||
      !ReadRange(root.get("tmd"), &layout.tmd) || !ReadRange(root.get("cert"), &layout.cert) ||
      !ReadRange(root.get("h3"), &layout.h3) || !ReadRange(root.get("data"), &layout.data))
    return fail();
  if (root.contains("erasedHashes") || version == FORMAT_VERSION)
  {
    if (version != FORMAT_VERSION || !root.get("erasedHashes").is<picojson::array>() ||
        root.get("erasedHashes").get<picojson::array>().empty())
      return fail();
    for (const auto& value : root.get("erasedHashes").get<picojson::array>())
    {
      ErasedHash hash;
      std::string hex;
      if (!value.is<picojson::object>() || !ReadNumber(value.get("level"), &hash.level) ||
          !ReadNumber(value.get("index"), &hash.index) || !ReadString(value.get("digest"), &hex) ||
          !IsHash(hex))
        return fail();
      for (size_t i = 0; i < hash.digest.size(); ++i)
      {
        unsigned int byte = 0;
        const char* start = hex.data() + i * 2;
        if (std::from_chars(start, start + 2, byte, 16).ec != std::errc{})
          return fail();
        hash.digest[i] = static_cast<u8>(byte);
      }
      layout.erased_hashes.push_back(hash);
    }
  }
  for (const char* key : {"partitions", "systemFiles", "fst", "gaps"})
  {
    if (!root.get(key).is<picojson::array>())
      return fail();
    for (const auto& value : root.get(key).get<picojson::array>())
    {
      if (!value.is<picojson::object>())
        return fail();
      if (std::strcmp(key, "partitions") == 0)
      {
        Partition partition;
        u64 type = 0;
        if (!ReadNumber(value.get("offset"), &partition.offset) ||
            !ReadNumber(value.get("type"), &type) || type > 0xffffffff)
          return fail();
        partition.type = static_cast<u32>(type);
        layout.partitions.push_back(partition);
      }
      else if (std::strcmp(key, "systemFiles") == 0)
      {
        SystemFile file;
        if (!ReadRange(value, &file) || !ReadString(value.get("path"), &file.path))
          return fail();
        layout.system_files.push_back(std::move(file));
      }
      else if (std::strcmp(key, "gaps") == 0)
      {
        Gap gap;
        if (!ReadRange(value, &gap) || !ReadString(value.get("kind"), &gap.kind))
          return fail();
        if (value.contains("physicalZero"))
        {
          if (version < 2 || !value.get("physicalZero").is<bool>())
            return fail();
          gap.physical_zero = value.get("physicalZero").get<bool>();
        }
        if (value.contains("seeds"))
        {
          if (gap.kind != "seeded" || !value.get("seeds").is<picojson::array>())
            return fail();
          for (const auto& item : value.get("seeds").get<picojson::array>())
          {
            std::string hex;
            if (!ReadString(item, &hex) || hex.size() != JunkSeed{}.size() * 8)
              return fail();
            JunkSeed seed;
            for (size_t i = 0; i < seed.size(); ++i)
            {
              u32 word = 0;
              const char* begin = hex.data() + i * 8;
              const auto result = std::from_chars(begin, begin + 8, word, 16);
              if (result.ec != std::errc{} || result.ptr != begin + 8)
                return fail();
              seed[i] = Common::swap32(word);
            }
            gap.seeds.push_back(seed);
          }
        }
        layout.gaps.push_back(std::move(gap));
      }
      else
      {
        Entry entry;
        if (!ReadString(value.get("name"), &entry.name) ||
            !ReadNumber(value.get("nameOffset"), &entry.name_offset) ||
            !value.get("directory").is<bool>())
          return fail();
        entry.directory = value.get("directory").get<bool>();
        if (entry.directory ? (!ReadNumber(value.get("parent"), &entry.parent) ||
                               !ReadNumber(value.get("next"), &entry.next)) :
                              (!ReadNumber(value.get("offset"), &entry.offset) ||
                               !ReadNumber(value.get("size"), &entry.size)))
          return fail();
        layout.entries.push_back(std::move(entry));
      }
    }
  }
  if (!Validate(layout, error))
    return {};
  return layout;
}

std::optional<std::vector<ErasedHash>> ReadErasedHashes(const DiscIO::Volume& volume,
                                                        const Layout& layout, std::string* error)
{
  const auto fail = [&](const char* reason) -> std::optional<std::vector<ErasedHash>> {
    *error = reason;
    return {};
  };
  const auto slots = ErasedHashSlots(layout);
  std::vector<ErasedHash> result;
  if (slots.empty())
    return result;
  const auto partition = volume.GetGamePartition();
  const auto data_offset =
      volume.ReadSwappedAndShifted(partition.offset + 0x2b8, DiscIO::PARTITION_NONE);
  const auto data_size =
      volume.ReadSwappedAndShifted(partition.offset + 0x2bc, DiscIO::PARTITION_NONE);
  const auto h3_offset =
      volume.ReadSwappedAndShifted(partition.offset + 0x2b4, DiscIO::PARTITION_NONE);
  std::vector<u8> h3(layout.h3.size);
  if (!data_offset || !data_size || *data_size != layout.data.size || !h3_offset ||
      !volume.Read(partition.offset + *h3_offset, h3.size(), h3.data(), DiscIO::PARTITION_NONE) ||
      DigestHex(Common::SHA1::CalculateDigest(h3)) != layout.h3_sha1)
    return fail("layout lacks erased hash metadata for this input; export a new standard layout");
  const auto zero = ZeroSectors(layout);
  const auto key = volume.GetTitleKey(partition);
  const auto aes = Common::AES::CreateContextDecrypt(key.data());
  std::array<u8, Wii::BLOCK_HEADER_SIZE> raw;
  Wii::HashBlock hashes;
  u64 cached_block = std::numeric_limits<u64>::max();
  for (const auto& entry : slots)
  {
    if (RebuildStandard::IsCancelled())
      return fail("cancelled");
    const auto [level, index] = entry.first;
    ErasedHash hash{level, index};
    if (level == 3)
      std::copy_n(h3.data() + index * hash.digest.size(), hash.digest.size(), hash.digest.begin());
    else
    {
      const u64 first = level == 1 ? index / 8 * 8 : index / 8 * Wii::BLOCKS_PER_GROUP;
      const u64 end = std::min<u64>(zero.size(), first + (level == 1 ? 8 : Wii::BLOCKS_PER_GROUP));
      u64 block = first;
      while (block < end && zero[block])
        ++block;
      if (block == end)
        return fail("cannot locate surviving parent of erased hash");
      if (cached_block != block)
      {
        if (!volume.Read(partition.offset + *data_offset + block * Wii::BLOCK_TOTAL_SIZE,
                         raw.size(), raw.data(), DiscIO::PARTITION_NONE))
          return fail("cannot read surviving hash header");
        Wii::DecryptBlockHashes(raw.data(), &hashes, aes.get());
        cached_block = block;
      }
      hash.digest = level == 1 ? hashes.h1[index % 8] : hashes.h2[index % 8];
    }
    result.push_back(hash);
  }
  return result;
}

bool ValidateSource(const DiscIO::Volume& volume, std::string* error)
{
  const auto fail = [&](const char* reason) {
    *error = reason;
    return false;
  };
  if (volume.GetVolumeType() != DiscIO::Platform::WiiDisc || !volume.HasWiiHashes() ||
      !volume.HasWiiEncryption())
    return fail("layout requires a hashed, encrypted Wii disc");
  const u64 size = volume.GetDataSize();
  const auto partition = volume.GetGamePartition();
  if (partition == DiscIO::PARTITION_NONE || partition.offset > size ||
      !Fits({partition.offset, 0x2c0}, size))
    return fail("game partition header is missing or out of range");
  u64 total = 0;
  for (u64 group = 0; group < 4; ++group)
  {
    const auto count = volume.ReadSwapped<u32>(0x40000 + group * 8, DiscIO::PARTITION_NONE);
    const auto table = volume.ReadSwappedAndShifted(0x40004 + group * 8, DiscIO::PARTITION_NONE);
    if (!count || !table || *count > 256 - total ||
        (*count != 0 && !Fits({*table, u64{*count} * 8}, size)))
      return fail("invalid source partition table");
    total += *count;
  }
  const auto field = [&](u64 address) {
    return volume.ReadSwappedAndShifted(partition.offset + address, DiscIO::PARTITION_NONE);
  };
  const auto data_offset = field(0x2b8);
  const auto data_size = field(0x2bc);
  const auto tmd = field(0x2a8);
  const auto tmd_size = volume.ReadSwapped<u32>(partition.offset + 0x2a4, DiscIO::PARTITION_NONE);
  const auto cert = field(0x2b0);
  const auto cert_size = volume.ReadSwapped<u32>(partition.offset + 0x2ac, DiscIO::PARTITION_NONE);
  const auto h3 = field(0x2b4);
  // All sums below are bounded by this subtraction check; shifted words are widened u32s.
  if (!data_offset || !data_size || *data_offset < 0x2c0 || *data_offset > 16 * 1024 * 1024 ||
      *data_size == 0 || *data_size % Wii::BLOCK_TOTAL_SIZE != 0 ||
      !Fits({*data_offset, *data_size}, size - partition.offset) || !tmd || !tmd_size ||
      !IOS::ES::IsValidTMDSize(*tmd_size) || !Fits({*tmd, *tmd_size}, *data_offset) || !cert ||
      !cert_size || !Fits({*cert, *cert_size}, *data_offset) || !h3 ||
      !Fits({*h3, DiscIO::WII_PARTITION_H3_SIZE}, *data_offset))
    return fail("invalid source partition ranges");
  const u64 decrypted_size = *data_size / Wii::BLOCK_TOTAL_SIZE * Wii::BLOCK_DATA_SIZE;
  const auto fst = DiscIO::GetFSTOffset(volume, partition);
  const auto fst_size = DiscIO::GetFSTSize(volume, partition);
  if (!fst || !fst_size || *fst_size < 12 || *fst_size > MAX_FST_SIZE ||
      !Fits({*fst, *fst_size}, decrypted_size))
    return fail("invalid source FST range");
  std::vector<u8> bytes(static_cast<size_t>(*fst_size));
  if (!volume.Read(*fst, bytes.size(), bytes.data(), partition))
    return fail("cannot read source FST");
  const u64 count = Common::swap32(bytes.data() + 8);
  if (count == 0 || count > bytes.size() / 12 || Common::swap32(bytes.data()) >> 24 != 1 ||
      Common::swap32(bytes.data() + 4) != 0)
    return fail("invalid source FST root");
  const u64 strings = count * 12;
  // Validate iteratively before the existing recursive filesystem reader sees the tree.
  std::vector<std::pair<u64, u64>> parents{{0, count}};
  for (u64 i = 1; i < count; ++i)
  {
    while (!parents.empty() && i == parents.back().second)
      parents.pop_back();
    if (parents.empty() || parents.size() > 128)
      return fail("source FST nesting exceeds 128 directories");
    const u8* entry = bytes.data() + i * 12;
    const u32 word = Common::swap32(entry);
    const u64 name = word & 0xffffff;
    if (word >> 24 > 1 || name >= bytes.size() - strings ||
        !std::memchr(bytes.data() + strings + name, 0, bytes.size() - strings - name))
      return fail("invalid source FST name or type");
    const u64 a = Common::swap32(entry + 4);
    const u64 b = Common::swap32(entry + 8);
    if (word >> 24)
    {
      if (a != parents.back().first || b <= i || b > parents.back().second)
        return fail("invalid source FST directory");
      parents.emplace_back(i, b);
    }
    else if (!Fits({a * 4, b}, decrypted_size))
      return fail("source FST file exceeds partition data");
  }
  return true;
}

std::optional<Layout> Generate(const DiscIO::Volume& volume, std::string* error)
{
  const auto fail = [&](const char* reason) -> std::optional<Layout> {
    *error = reason;
    return {};
  };
  if (!ValidateSource(volume, error))
    return {};
  if (volume.GetVolumeType() != DiscIO::Platform::WiiDisc || !volume.HasWiiHashes() ||
      !volume.HasWiiEncryption())
    return fail("layout requires a hashed, encrypted Wii disc");
  const auto partition = volume.GetGamePartition();
  if (partition == DiscIO::PARTITION_NONE)
    return fail("game partition is missing");
  Layout layout;
  layout.id = volume.GetGameID(partition);
  const auto disc_number = volume.GetDiscNumber(partition);
  if (!disc_number)
    return fail("cannot read disc number");
  layout.disc_number = *disc_number;
  layout.partition_offset = partition.offset;
  // Read each table directly so its original entry order is retained.
  for (u64 group = 0; group < 4; ++group)
  {
    const auto count = volume.ReadSwapped<u32>(0x40000 + group * 8, DiscIO::PARTITION_NONE);
    const auto table = volume.ReadSwappedAndShifted(0x40004 + group * 8, DiscIO::PARTITION_NONE);
    if (!count || !table || *count > 256 || layout.partitions.size() + *count > 256)
      return fail("cannot read partition table");
    for (u64 i = 0; i < *count; ++i)
    {
      const auto offset = volume.ReadSwappedAndShifted(*table + i * 8, DiscIO::PARTITION_NONE);
      const auto type = volume.ReadSwapped<u32>(*table + i * 8 + 4, DiscIO::PARTITION_NONE);
      if (!offset || !type)
        return fail("cannot read partition entry");
      layout.partitions.push_back({*offset, *type});
    }
  }
  const auto field = [&](u64 address, bool shifted, u64* output) {
    const auto word = volume.ReadSwapped<u32>(partition.offset + address, DiscIO::PARTITION_NONE);
    if (!word)
      return false;
    *output = static_cast<u64>(*word) << (shifted ? 2 : 0);
    return true;
  };
  if (!field(0x2a4, false, &layout.tmd.size) || !field(0x2a8, true, &layout.tmd.offset) ||
      !field(0x2ac, false, &layout.cert.size) || !field(0x2b0, true, &layout.cert.offset) ||
      !field(0x2b4, true, &layout.h3.offset) || !field(0x2b8, true, &layout.data.offset) ||
      !field(0x2bc, true, &layout.data.size))
    return fail("cannot read partition header fields");
  layout.h3.size = DiscIO::WII_PARTITION_H3_SIZE;
  const auto report = DiscReport::Generate(volume, error, RebuildStandard::IsCancelled);
  if (!report)
    return {};
  layout.h3_sha1 = *report->header.h3_sha1;
  layout.tmd_content_sha1 = *report->header.tmd_content_sha1;
  const auto dol = DiscIO::GetBootDOLOffset(volume, partition);
  const auto fst = DiscIO::GetFSTOffset(volume, partition);
  const auto max_fst = volume.ReadSwappedAndShifted(0x42c, partition);
  if (!dol || !fst || !max_fst)
    return fail("cannot read boot layout fields");
  layout.fst_max_size = *max_fst;
  for (const auto& file : report->files)
  {
    u64 offset = 0;
    if (file.path == "sys/boot.bin")
      offset = 0;
    else if (file.path == "sys/bi2.bin")
      offset = 0x440;
    else if (file.path == "sys/apploader.img")
      offset = 0x2440;
    else if (file.path == "sys/main.dol")
      offset = *dol;
    else if (file.path == "sys/fst.bin")
      offset = *fst;
    else
      continue;
    layout.system_files.push_back({{offset, file.size}, file.path});
  }
  const auto* fst_file = layout.FindSystemFile("sys/fst.bin");
  if (!fst_file || fst_file->size < 12 || fst_file->size > MAX_FST_SIZE)
    return fail("invalid FST size");
  std::vector<u8> bytes(static_cast<size_t>(fst_file->size));
  if (!volume.Read(*fst, bytes.size(), bytes.data(), partition))
    return fail("cannot read FST");
  const u32 count = Common::swap32(bytes.data() + 8);
  if (count == 0 || count > bytes.size() / 12)
    return fail("invalid FST entry count");
  const size_t strings = static_cast<size_t>(count) * 12;
  for (size_t i = 0; i < count; ++i)
  {
    const u32 word = Common::swap32(bytes.data() + i * 12);
    if ((word >> 24) > 1)
      return fail("invalid FST entry type");
    Entry entry;
    entry.directory = word >> 24;
    entry.name_offset = word & 0xffffff;
    if (i != 0)
    {
      if (entry.name_offset >= bytes.size() - strings)
        return fail("invalid FST string offset");
      const char* name = reinterpret_cast<const char*>(bytes.data() + strings + entry.name_offset);
      const void* terminator = std::memchr(name, 0, bytes.size() - strings - entry.name_offset);
      if (!terminator)
        return fail("unterminated FST name");
      entry.name.assign(name, static_cast<const char*>(terminator) - name);
    }
    const u32 a = Common::swap32(bytes.data() + i * 12 + 4);
    const u32 b = Common::swap32(bytes.data() + i * 12 + 8);
    if (entry.directory)
    {
      entry.parent = a;
      entry.next = b;
    }
    else
    {
      entry.offset = static_cast<u64>(a) * 4;
      entry.size = b;
    }
    layout.entries.push_back(std::move(entry));
  }
  const auto regenerated = BuildFST(layout);
  if (!regenerated || *regenerated != bytes)
    return fail("FST has bytes not representable as names and structure");
  auto files = Files(layout);
  std::erase_if(files, [](const auto& file) { return file.size == 0; });
  std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.offset < b.offset; });
  u64 file_end = 0;
  for (size_t i = 0; i <= files.size(); ++i)
  {
    const u64 next = i == files.size() ? layout.DecryptedSize() : files[i].offset;
    if (next < file_end || next > layout.DecryptedSize())
      return fail("overlapping or out-of-range source files");
    if (next > file_end)
      layout.gaps.push_back({{file_end, next - file_end}, "zero"});
    if (i < files.size())
    {
      if (files[i].size > layout.DecryptedSize() - next)
        return fail("file exceeds partition data");
      file_end = next + files[i].size;
    }
  }
  if (!Validate(layout, error))
    return {};
  std::vector<Gap> classified;
  const auto append = [&](u64 start, u64 end, std::string kind, const JunkSeed* seed = nullptr,
                          bool physical_zero = false) {
    if (start == end)
      return;
    if (!classified.empty() && classified.back().kind == kind &&
        classified.back().physical_zero == physical_zero &&
        (!seed || start % JUNK_UNIT_SIZE == 0 || classified.back().seeds.back() == *seed) &&
        classified.back().offset + classified.back().size == start)
    {
      classified.back().size += end - start;
      if (seed && start % JUNK_UNIT_SIZE == 0)
        classified.back().seeds.push_back(*seed);
    }
    else
    {
      Gap gap{{start, end - start}, std::move(kind)};
      gap.physical_zero = physical_zero;
      if (seed)
        gap.seeds.push_back(*seed);
      classified.push_back(std::move(gap));
    }
  };
  std::vector<u8> buffer(JUNK_UNIT_SIZE), formula(JUNK_UNIT_SIZE), junk_bytes(JUNK_UNIT_SIZE);
  std::vector<JunkSeed> candidates;
  u64 cached_unit = std::numeric_limits<u64>::max();
  std::array<u8, Wii::BLOCK_TOTAL_SIZE> raw_block;
  u64 cached_block = std::numeric_limits<u64>::max();
  bool physical_zero = false;
  for (auto gap_it = layout.gaps.begin(); gap_it != layout.gaps.end(); ++gap_it)
  {
    for (u64 position = gap_it->offset; position < gap_it->offset + gap_it->size;)
    {
      if (RebuildStandard::IsCancelled())
        return fail("cancelled");
      const u64 unit = position / JUNK_UNIT_SIZE * JUNK_UNIT_SIZE;
      const u64 unit_end = std::min(unit + JUNK_UNIT_SIZE, layout.DecryptedSize());
      if (cached_unit != unit)
      {
        if (!volume.Read(unit, unit_end - unit, buffer.data(), partition))
          return fail("cannot read partition gap");
        DiscIO::LaggedFibonacciGenerator lfg;
        const auto seed = FormulaSeed(layout.id, layout.disc_number, unit);
        lfg.SetSeed(seed.data());
        lfg.GetBytes(formula.size(), formula.data());
        candidates.clear();
        // Recover from any sufficiently long gap in this unit, also covering tiny fragments
        // separated by files. Like RVZPack, skip the zero padding that can follow a file.
        for (auto it = gap_it; it != layout.gaps.end() && it->offset < unit_end; ++it)
        {
          // A physical-zero payload can interrupt a junk unit. Recover independently
          // on either side without rebasing the partition-relative 0x8000 LFG origin.
          const u64 limit = std::min(it->offset + it->size, unit_end);
          for (u64 candidate = std::max(it->offset, unit); candidate < limit;)
          {
            const u64 next =
                std::min(limit, (candidate / Wii::BLOCK_DATA_SIZE + 1) * Wii::BLOCK_DATA_SIZE);
            size_t start = candidate - unit;
            size_t end = next - unit;
            while (start < end && buffer[start] == 0)
              ++start;
            while (end > start && buffer[end - 1] == 0)
              --end;
            JunkSeed recovered;
            if (end - start >= sizeof(u32) &&
                DiscIO::LaggedFibonacciGenerator::GetSeed(buffer.data() + start, end - start, start,
                                                          recovered.data()) > 0)
              candidates.push_back(recovered);
            candidate = next;
          }
        }
        cached_unit = unit;
      }
      const u64 block = position / Wii::BLOCK_DATA_SIZE;
      if (cached_block != block)
      {
        // RVZ's encrypted AllSame reuse handles these before RVZPack's LFG detection.
        // Do not confuse zero ciphertext (including its zero IV/hash header) with zero data.
        if (!volume.Read(layout.partition_offset + layout.data.offset +
                             block * Wii::BLOCK_TOTAL_SIZE,
                         raw_block.size(), raw_block.data(), DiscIO::PARTITION_NONE))
          return fail("cannot read partition gap sector");
        physical_zero =
            std::all_of(raw_block.begin(), raw_block.end(), [](u8 b) { return b == 0; });
        cached_block = block;
      }
      const u64 end =
          std::min({gap_it->offset + gap_it->size, unit_end, (block + 1) * Wii::BLOCK_DATA_SIZE});
      if (physical_zero)
      {
        append(position, end, "zero", nullptr, true);
        position = end;
        continue;
      }
      const auto classify = [&](u64 start, u64 stop) {
        const auto begin = buffer.begin() + (start - unit);
        const auto finish = buffer.begin() + (stop - unit);
        if (std::equal(begin, finish, formula.begin() + (start - unit)))
        {
          append(start, stop, "junk");
          return true;
        }
        for (const auto& seed : candidates)
        {
          DiscIO::LaggedFibonacciGenerator lfg;
          lfg.SetSeed(seed.data());
          lfg.Forward(start - unit);
          lfg.GetBytes(stop - start, junk_bytes.data());
          if (std::equal(begin, finish, junk_bytes.begin()))
          {
            append(start, stop, "seeded", &seed);
            return true;
          }
        }
        return false;
      };
      if (!classify(position, end))
      {
        u64 start = position, stop = end;
        while (start < stop && buffer[start - unit] == 0)
          ++start;
        while (stop > start && buffer[stop - unit - 1] == 0)
          --stop;
        append(position, start, "zero");
        if (start < stop && !classify(start, stop))
          append(start, stop, "other");
        append(stop, end, "zero");
      }
      position = end;
    }
  }
  layout.gaps = std::move(classified);
  const auto erased_hashes = ReadErasedHashes(volume, layout, error);
  if (!erased_hashes)
    return {};
  layout.erased_hashes = *erased_hashes;
  if (!Validate(layout, error))
    return {};
  std::map<std::string, u64> totals;
  size_t seeds = 0;
  for (const auto& gap : layout.gaps)
  {
    totals[gap.kind] += gap.size;
    seeds += gap.seeds.size();
  }
  fmt::print(stderr, "GAPS junk={} seeded={} zero={} other={} bytes; {} seeds stored\n",
             totals["junk"], totals["seeded"], totals["zero"], totals["other"], seeds);
  return layout;
}

int Run(const std::string& image, const std::string& output, std::FILE* out)
{
  const auto fail = [&](std::string_view reason) {
    fmt::print(out, "{}\n", DiscReport::FailureLine(reason));
    return 1;
  };
  if (File::Exists(output))
    return fail("output already exists");
  auto volume = DiscIO::CreateVolume(image, false);
  if (!volume)
    return fail("cannot open Wii disc image");
  std::string error;
  const auto layout = Generate(*volume, &error);
  if (!layout)
    return fail(error);
  const auto partial = DiscTool::OutputFile::Create(output);
  if (!partial)
    return fail("cannot reserve layout output");
  if (RebuildStandard::IsCancelled() ||
      !File::WriteStringToFile(partial->Path(), Serialize(*layout)) ||
      RebuildStandard::IsCancelled() || !partial->Publish(output))
    return fail("cannot write layout JSON");
  fmt::print(out, "PASS: wrote {} layout\n", layout->id);
  return 0;
}
}  // namespace Orca::DiscLayout
