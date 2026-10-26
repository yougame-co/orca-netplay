// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "OrcaDiscTool/OutputFile.h"

#include <filesystem>
#include <random>
#include <utility>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

namespace Orca::DiscTool
{
OutputFile::OutputFile(std::string directory)
    : m_directory(std::move(directory)), m_path(m_directory + "/data")
{
}

std::unique_ptr<OutputFile> OutputFile::Create(const std::string& output)
{
  std::random_device random;
  for (int attempt = 0; attempt < 16; ++attempt)
  {
    const std::string directory = fmt::format("{}.partial.{:08x}{:08x}{:08x}{:08x}", output,
                                              random(), random(), random(), random());
    std::error_code error;
    if (std::filesystem::create_directory(StringToPath(directory), error))
      return std::unique_ptr<OutputFile>(new OutputFile(directory));
    if (error && error != std::errc::file_exists)
      return nullptr;
  }
  return nullptr;
}

OutputFile::~OutputFile()
{
  // Do not recursively delete: an unrelated path never becomes our cleanup responsibility.
  std::error_code error;
  std::filesystem::remove(StringToPath(m_path), error);
  std::filesystem::remove(StringToPath(m_directory), error);
}

bool OutputFile::Publish(const std::string& output) const
{
  // Linking in the same directory/filesystem is atomic and fails if output already exists.
  // Cleanup removes the temporary name after publication; the destination keeps its inode.
  std::error_code error;
  std::filesystem::create_hard_link(StringToPath(m_path), StringToPath(output), error);
  return !error;
}
}  // namespace Orca::DiscTool
