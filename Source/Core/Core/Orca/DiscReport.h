// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace DiscIO
{
class Volume;
struct Partition;
}

namespace Orca::DiscReport
{
namespace Detail
{
// Shared range reader for report generation and focused tests.
std::optional<std::string> HashRange(const DiscIO::Volume& volume,
                                     const DiscIO::Partition& partition, std::string_view path,
                                     u64 offset, u64 size);
}  // namespace Detail

struct FileEntry
{
  std::string path;
  u64 size;
  std::string sha1;
};

struct Header
{
  std::string id;
  u16 revision = 0;
  std::optional<std::string> tmd_content_sha1;
  std::optional<std::string> h3_sha1;
  std::optional<u64> partition_offset;
};

struct Report
{
  Header header;
  std::vector<FileEntry> files;
};

struct Reference
{
  std::string name;
  std::vector<FileEntry> files;
  std::optional<u64> partition_offset;
};

// Uses an independent DiscIO volume, never the mounted disc or session configuration.
// Only metadata and digests leave this function; file data stays in a bounded memory buffer.
std::optional<Report> Generate(const std::string& image, std::string* error);
std::optional<Reference> ParseReference(std::string_view json, std::string* error);

// Sorted, distinct paths. A requested partition offset mismatch is named "partitionOffset".
std::vector<std::string> Compare(const Report& report, const Reference& reference);
std::string HeaderLine(const Header& header);
std::string FileLine(const FileEntry& file);
std::string VerdictLine(const Reference& reference, const std::vector<std::string>& differences);
std::string FailureLine(std::string_view reason);

// Without a reference, a successfully read report ends with "PASS: files match <game id>".
// These are standalone tools: callers must dispatch before any application initialization.
int Run(const std::string& image, const std::optional<std::string>& reference, std::FILE* out);
}  // namespace Orca::DiscReport
