// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Chat.h"

#include <utility>

namespace Orca::UX::Chat
{
namespace
{
// What encodeURIComponent leaves as it is.
bool Unreserved(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' ||
         c == ')';
}

int HexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

// No control characters or Unicode line/paragraph separators: they would break the toast's layout.
bool Shown(char32_t c)
{
  if (c < 0x20 || c == 0x7f || (c >= 0x80 && c <= 0x9f))
    return false;
  return c != 0x2028 && c != 0x2029;
}

// Counts the code points of valid UTF-8 that are all Shown(); -1 for anything else.
long CountShown(std::string_view s)
{
  long points = 0;
  std::size_t i = 0;
  while (i < s.size())
  {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t len;
    char32_t c;
    if (b < 0x80)
    {
      len = 1;
      c = b;
    }
    else if (b >= 0xc2 && b <= 0xdf)
    {
      len = 2;
      c = b & 0x1f;
    }
    else if (b >= 0xe0 && b <= 0xef)
    {
      len = 3;
      c = b & 0x0f;
    }
    else if (b >= 0xf0 && b <= 0xf4)
    {
      len = 4;
      c = b & 0x07;
    }
    else
    {
      return -1;  // stray continuation byte, overlong lead (C0, C1), or past F4
    }
    if (i + len > s.size())
      return -1;
    for (std::size_t k = 1; k < len; ++k)
    {
      const auto cont = static_cast<unsigned char>(s[i + k]);
      if ((cont & 0xc0) != 0x80)
        return -1;
      c = (c << 6) | (cont & 0x3f);
    }
    // Reject overlong 3- and 4-byte forms, surrogates, and code points past U+10FFFF.
    if ((len == 3 && c < 0x800) || (len == 4 && (c < 0x10000 || c > 0x10ffff)) ||
        (c >= 0xd800 && c <= 0xdfff))
    {
      return -1;
    }
    if (!Shown(c))
      return -1;
    ++points;
    i += len;
  }
  return points;
}
}  // namespace

std::optional<std::string> DecodeField(std::string_view field, std::size_t max_encoded,
                                       std::size_t max_points)
{
  if (field.empty() || field.size() > max_encoded)
    return std::nullopt;
  std::string out;
  out.reserve(field.size());
  for (std::size_t i = 0; i < field.size(); ++i)
  {
    const char c = field[i];
    if (c == '%')
    {
      if (i + 2 >= field.size())
        return std::nullopt;
      const int hi = HexValue(field[i + 1]);
      const int lo = HexValue(field[i + 2]);
      if (hi < 0 || lo < 0)
        return std::nullopt;
      out.push_back(static_cast<char>(hi * 16 + lo));
      i += 2;
    }
    else if (Unreserved(c))
    {
      out.push_back(c);
    }
    else
    {
      return std::nullopt;
    }
  }
  const long points = CountShown(out);
  if (points < 1 || static_cast<std::size_t>(points) > max_points)
    return std::nullopt;
  return out;
}

std::optional<Line> Parse(std::string_view line)
{
  constexpr std::string_view prefix = "chat ";
  if (line.substr(0, prefix.size()) != prefix)
    return std::nullopt;
  const std::string_view rest = line.substr(prefix.size());
  const std::size_t space = rest.find(' ');
  if (space == std::string_view::npos)
    return std::nullopt;
  // A second space fails the text's decoding, since ' ' is never valid in an encoded field.
  std::optional<std::string> name = DecodeField(rest.substr(0, space), NAME_ENCODED, NAME_POINTS);
  std::optional<std::string> text = DecodeField(rest.substr(space + 1), TEXT_ENCODED, TEXT_POINTS);
  if (!name || !text)
    return std::nullopt;
  return Line{std::move(*name), std::move(*text)};
}
}  // namespace Orca::UX::Chat
