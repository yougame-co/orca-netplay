// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Branding.h"

#include <array>

#include "Core/ConfigManager.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"

namespace Orca
{
namespace
{
bool IsWordChar(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Part of a path or a file name, not a word on its own.
bool IsPathChar(char c)
{
  return c == '/' || c == '\\';
}
}  // namespace

std::string WindowTitle()
{
  std::string title(PRODUCT_NAME);
  // A launcher profile's title is the mod it boots (Project+, not Brawl).
  const Profile* profile = ActiveProfile();
  if (profile && !profile->title.empty())
    title += " — " + profile->title;
  else if (const std::string& game = SConfig::GetInstance().GetTitleDescription(); !game.empty())
    title += " — " + game;
  if (std::string status = Online::StatusLine(); !status.empty())
  {
    // Drop the room code: screen-share and streaming pickers show window titles, and the code lets
    // anyone join.
    if (status.starts_with("Room "))
    {
      if (const auto colon = status.find(": "); colon != std::string::npos)
        status.erase(0, colon + 2);
    }
    title += " — " + status;
  }
  return title;
}

std::string BrandWords(std::string_view text)
{
  // The names this fork had before 0.3.42. Dolphin keeps its own name.
  static constexpr std::array<std::string_view, 2> WORDS = {"YG Baiji", "Orca"};
  std::string branded;
  branded.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size())
  {
    std::size_t matched = 0;
    for (const std::string_view word : WORDS)
    {
      if (!text.substr(at).starts_with(word))
        continue;
      const std::size_t end = at + word.size();
      const bool open_before = at == 0 || (!IsWordChar(text[at - 1]) && !IsPathChar(text[at - 1]) &&
                                           text[at - 1] != '.');
      // A '.' after the word ends a sentence unless a letter or digit follows it (a file name).
      const bool open_after =
          end == text.size() ||
          (!IsWordChar(text[end]) && !IsPathChar(text[end]) &&
           !(text[end] == '.' && end + 1 < text.size() && IsWordChar(text[end + 1])));
      if (open_before && open_after)
        matched = word.size();
      break;
    }
    if (matched != 0)
    {
      branded += PRODUCT_NAME;
      at += matched;
    }
    else
    {
      branded += text[at];
      ++at;
    }
  }
  return branded;
}

std::string BrandText(std::string_view text)
{
  if (!SessionActive())
    return std::string(text);
  return BrandWords(text);
}
}  // namespace Orca
