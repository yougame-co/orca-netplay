// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/LocalStore.h"

#include <mutex>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/Orca/Profile.h"

namespace Orca::LocalStore
{
namespace
{
std::mutex s_mutex;
bool s_seen_loaded = false;
std::string s_seen_text;
}  // namespace

bool SeenIn(std::string_view text, std::string_view key)
{
  while (!text.empty())
  {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.remove_prefix(1);
    if (line == key)
      return true;
    if (end == std::string_view::npos)
      break;
    text.remove_prefix(end + 1);
  }
  return false;
}

std::string SeenWith(std::string text, std::string_view key)
{
  if (SeenIn(text, key))
    return text;
  if (!text.empty() && text.back() != '\n')
    text += '\n';
  text += key;
  text += '\n';
  return text;
}

std::string SeenPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "OrcaSeen.txt";
}

bool Seen(std::string_view key)
{
  std::lock_guard lk(s_mutex);
  if (!s_seen_loaded)
  {
    s_seen_loaded = true;
    std::string text;
    const std::string path = SeenPath();
    if (!path.empty() && File::Exists(path) && File::ReadFileToString(path, text))
      s_seen_text = std::move(text);
  }
  return SeenIn(s_seen_text, key);
}

bool MarkSeen(std::string_view key)
{
  std::lock_guard lk(s_mutex);
  std::string text;
  const std::string path = SeenPath();
  if (!path.empty() && File::Exists(path))
    File::ReadFileToString(path, text);
  text = SeenWith(std::move(text), key);
  if (path.empty() || !File::WriteStringToFile(path, text))
    return false;
  s_seen_loaded = true;
  s_seen_text = std::move(text);
  return true;
}

std::string ControlsPath()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || profile->game_id.empty())
    return {};
  return File::GetUserPath(D_CONFIG_IDX) + "OrcaControls-" + profile->game_id + ".txt";
}

std::optional<std::string> KeptControls()
{
  std::string text;
  if (const std::string path = ControlsPath();
      !path.empty() && File::Exists(path) && File::ReadFileToString(path, text))
  {
    return text;
  }
  return std::nullopt;
}

bool KeepControls(std::string_view text)
{
  const std::string path = ControlsPath();
  return !path.empty() && File::WriteStringToFile(path, text);
}
}  // namespace Orca::LocalStore
