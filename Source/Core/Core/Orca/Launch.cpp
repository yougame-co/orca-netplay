// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Launch.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

namespace Orca
{
namespace
{
constexpr size_t MAX_INI_BYTES = 4096;
constexpr size_t MAX_PATH_CHARS = 300;

bool ValidProfileName(std::string_view v)
{
  return !v.empty() && v.size() <= 32 && std::ranges::all_of(v, [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_';
  });
}

// A relative path of plain components: no root, no "." or "..", no empty component, no backslash
// or control character.
bool ValidRelativePath(std::string_view v)
{
  if (v.empty() || v.size() > MAX_PATH_CHARS || v.front() == '/')
    return false;
  if (std::ranges::any_of(v, [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '\\'; }))
    return false;
  size_t start = 0;
  while (start <= v.size())
  {
    const size_t end = std::min(v.find('/', start), v.size());
    const std::string_view part = v.substr(start, end - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    start = end + 1;
  }
  return true;
}
}  // namespace

std::optional<LaunchSpec> ParseLaunchIni(std::string_view text, std::string* error)
{
  error->clear();
  if (text.size() > MAX_INI_BYTES)
  {
    *error = "orca-launch.ini is too large";
    return std::nullopt;
  }
  LaunchSpec spec;
  bool in_launch = false, seen_section = false, have_profile = false, have_executable = false;
  int line_number = 0;
  const auto fail = [&](std::string_view why) {
    *error = fmt::format("orca-launch.ini line {}: {}", line_number, why);
    return std::nullopt;
  };
  size_t start = 0;
  while (start < text.size())
  {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos)
      end = text.size();
    std::string_view raw = text.substr(start, end - start);
    start = end + 1;
    ++line_number;
    if (!raw.empty() && raw.back() == '\r')
      raw.remove_suffix(1);
    const std::string line(StripWhitespace(raw));
    if (line.empty() || line.front() == '#' || line.front() == ';')
      continue;
    if (line.front() == '[')
    {
      if (line != "[Launch]")
        return fail(fmt::format("unknown section {}", line));
      if (seen_section)
        return fail("[Launch] appears twice");
      seen_section = in_launch = true;
      continue;
    }
    if (!in_launch)
      return fail("a key before [Launch]");
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      return fail("expected <key> = <value>");
    const std::string key(StripWhitespace(line.substr(0, eq)));
    const std::string value(StripWhitespace(line.substr(eq + 1)));
    if (key == "Profile")
    {
      if (have_profile)
        return fail("Profile appears twice");
      if (!ValidProfileName(value))
        return fail("Profile must be 1-32 letters, digits, - or _");
      spec.profile = value;
      have_profile = true;
    }
    else if (key == "Executable")
    {
      if (have_executable)
        return fail("Executable appears twice");
      if (!ValidRelativePath(value))
        return fail("Executable must be a relative path inside the build's folder");
      spec.executable = value;
      have_executable = true;
    }
    else
    {
      return fail(fmt::format("unknown key {}", key.empty() ? std::string("(empty)") : key));
    }
  }
  if (!have_profile || !have_executable)
  {
    *error = "orca-launch.ini needs [Launch] with Profile and Executable";
    return std::nullopt;
  }
  return spec;
}

std::optional<LaunchSpec> ReadLaunchIni(const std::string& folder, std::string* error)
{
  error->clear();
  const std::string path = folder + "/" + std::string(LAUNCH_INI);
  if (!File::Exists(path))
    return std::nullopt;
  std::string text;
  if (File::GetSize(path) > MAX_INI_BYTES || !File::ReadFileToString(path, text))
  {
    *error = fmt::format("{} can't be read", path);
    return std::nullopt;
  }
  std::optional<LaunchSpec> spec = ParseLaunchIni(text, error);
  if (!spec)
    return std::nullopt;

  // The loader, symlinks resolved, must be a regular file inside the resolved folder. Use
  // StringToPath/PathToString: fs::path(std::string) and .string() use the ANSI code page on
  // Windows.
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root = fs::canonical(StringToPath(folder), ec);
  const fs::path exe = ec ? fs::path() : fs::canonical(root / StringToPath(spec->executable), ec);
  if (ec || !fs::is_regular_file(exe, ec))
  {
    *error = fmt::format("orca-launch.ini: {} isn't there", spec->executable);
    return std::nullopt;
  }
  const auto [root_end, exe_it] = std::mismatch(root.begin(), root.end(), exe.begin(), exe.end());
  if (root_end != root.end() || exe_it == exe.end())
  {
    *error = fmt::format("orca-launch.ini: {} leads outside the build's folder", spec->executable);
    return std::nullopt;
  }
  spec->executable = PathToString(exe);
  return spec;
}
}  // namespace Orca
