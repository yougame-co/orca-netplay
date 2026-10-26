// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "DiscIO/LaggedFibonacciGenerator.h"

namespace DiscIO
{
class Volume;
}

namespace Orca::DiscLayout
{
inline constexpr u64 FORMAT_VERSION = 3;
// RVZPack's unit, measured in decrypted partition bytes (not physical Wii sectors).
constexpr u64 JUNK_UNIT_SIZE = 0x8000;
using JunkSeed = std::array<u32, DiscIO::LaggedFibonacciGenerator::SEED_SIZE>;

struct Range
{
  u64 offset = 0;
  u64 size = 0;
};

struct SystemFile : Range
{
  std::string path;
};

struct Entry
{
  std::string name;
  bool directory = false;
  u64 name_offset = 0;
  // Directories use parent/next; files use offset/size.
  u64 parent = 0;
  u64 next = 0;
  u64 offset = 0;
  u64 size = 0;
};

struct Gap : Range
{
  std::string kind;
  // Dolphin's big-endian seed bytes, one seed per intersected junk unit, in offset order.
  std::vector<JunkSeed> seeds;
  // A physically zero Wii sector decrypts to a repeating AES block, not zero plaintext.
  // Only valid for kind == "zero"; absent/false retains decrypted-zero semantics.
  bool physical_zero = false;
};

struct Partition
{
  u64 offset = 0;
  u32 type = 0;
};

// A missing subtree's digest, retained by a surviving parent or the H3 table.
// Level 1 indexes sectors, level 2 indexes eight-sector subgroups, level 3 indexes groups.
struct ErasedHash
{
  u64 level = 0;
  u64 index = 0;
  Common::SHA1::Digest digest{};
};

struct Layout
{
  std::string id;
  u8 disc_number = 0;
  std::string h3_sha1;
  std::string tmd_content_sha1;
  std::vector<Partition> partitions;
  u64 partition_offset = 0;
  Range tmd;
  Range cert;
  Range h3;
  Range data;  // Physical (hashed/encrypted) bytes, relative to the partition header.
  u64 fst_max_size = 0;
  std::vector<SystemFile> system_files;
  std::vector<Entry> entries;
  std::vector<Gap> gaps;
  std::vector<ErasedHash> erased_hashes;

  u64 DecryptedSize() const;
  const SystemFile* FindSystemFile(std::string_view path) const;
};

std::optional<Layout> Generate(const DiscIO::Volume& volume, std::string* error);
// Preflight source metadata before lazy filesystem traversal or allocations from disc fields.
bool ValidateSource(const DiscIO::Volume& volume, std::string* error);
std::optional<std::vector<ErasedHash>> ReadErasedHashes(const DiscIO::Volume& volume,
                                                        const Layout& layout, std::string* error);
std::optional<Layout> Parse(std::string_view json, std::string* error);
std::string Serialize(const Layout& layout);
// Also checks the complete, non-overlapping coverage of the decrypted partition.
bool Validate(const Layout& layout, std::string* error);
std::optional<std::vector<u8>> BuildFST(const Layout& layout);
std::vector<SystemFile> Files(const Layout& layout);
JunkSeed FormulaSeed(std::string_view id, u8 disc_number, u64 offset);
int Run(const std::string& image, const std::string& output, std::FILE* out);
}  // namespace Orca::DiscLayout
