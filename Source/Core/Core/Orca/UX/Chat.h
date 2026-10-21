// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Room chat while Orca is fullscreen. The page sends `chat <name> <text>` on stdin and Orca shows
// it as an overlay toast. Both fields are encodeURIComponent'd UTF-8, so a message can't contain a
// space or newline and can never smuggle in a second command. Chat text is never logged. Drawn on
// the host only, never into game memory, so it can't desync.
namespace Orca::UX::Chat
{
// Limits in code points after decoding (the page truncates to these).
inline constexpr std::size_t NAME_POINTS = 24;
inline constexpr std::size_t TEXT_POINTS = 140;
// Encoded limits: up to 4 UTF-8 bytes per code point, 3 characters ("%XX") per byte.
inline constexpr std::size_t NAME_ENCODED = NAME_POINTS * 12;
inline constexpr std::size_t TEXT_ENCODED = TEXT_POINTS * 12;

struct Line
{
  std::string name;
  std::string text;
};

// Percent-decodes one field. Returns nullopt unless the input uses only encodeURIComponent's
// output alphabet and decodes to valid UTF-8 of 1..max_points code points with no control
// characters or line separators.
std::optional<std::string> DecodeField(std::string_view field, std::size_t max_encoded,
                                       std::size_t max_points);

// Parses a full "chat <name> <text>" line. Returns nullopt if it is malformed.
std::optional<Line> Parse(std::string_view line);
}  // namespace Orca::UX::Chat
