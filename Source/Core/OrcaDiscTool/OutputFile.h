// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

namespace Orca::DiscTool
{
// Owns an exclusively created sibling directory. Only this run writes/cleans its partial file.
class OutputFile final
{
public:
  static std::unique_ptr<OutputFile> Create(const std::string& output);
  ~OutputFile();
  const std::string& Path() const { return m_path; }
  // Atomically publishes the file without replacing any existing destination (even a symlink).
  bool Publish(const std::string& output) const;

private:
  explicit OutputFile(std::string directory);
  std::string m_directory;
  std::string m_path;
};
}  // namespace Orca::DiscTool
