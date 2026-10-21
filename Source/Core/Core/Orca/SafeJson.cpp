// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/SafeJson.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdlib>

#include <picojson.h>

namespace Orca::Json
{
namespace
{
constexpr int MAX_JSON_DEPTH = 16;
constexpr int MAX_JSON_DIGITS = 17;

// The index just past the string starting at `at` (a quote), or npos if it never ends.
size_t SkipJsonString(std::string_view text, size_t at)
{
  for (size_t i = at + 1; i < text.size(); ++i)
  {
    if (text[i] == '\\')
      ++i;
    else if (text[i] == '"')
      return i + 1;
  }
  return std::string_view::npos;
}

// The index of the bracket closing the array or object opened at `at`, or npos.
size_t MatchingClose(std::string_view text, size_t at)
{
  int depth = 0;
  for (size_t i = at; i < text.size();)
  {
    const char c = text[i];
    if (c == '"')
    {
      i = SkipJsonString(text, i);
      if (i == std::string_view::npos)
        return i;
      continue;
    }
    if (c == '[' || c == '{')
      ++depth;
    else if ((c == ']' || c == '}') && --depth == 0)
      return i;
    ++i;
  }
  return std::string_view::npos;
}

bool SafeNumber(std::string_view number, NumberPolicy numbers)
{
  if (numbers == NumberPolicy::Finite)
  {
    std::string token(number);
#if PICOJSON_USE_LOCALE
    if (const size_t decimal = token.find('.'); decimal != std::string::npos)
      token.replace(decimal, 1, std::localeconv()->decimal_point);
#endif
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    return end == token.c_str() + token.size() && std::isfinite(value);
  }

  int digits = 0;
  for (const char c : number)
  {
    if (c == 'e' || c == 'E')
      return false;
    digits = c >= '0' && c <= '9' ? digits + 1 : 0;
    if (digits > MAX_JSON_DIGITS)
      return false;
  }
  return true;
}

}  // namespace

// Values outside the protocol limits become null. Unclosed strings or brackets are refused.
std::optional<std::string> SanitizeJson(std::string_view text, NumberPolicy numbers)
{
  std::string out;
  out.reserve(text.size());
  int depth = 0;
  for (size_t i = 0; i < text.size();)
  {
    const char c = text[i];
    if (c == '"')
    {
      const size_t end = SkipJsonString(text, i);
      if (end == std::string_view::npos)
        return std::nullopt;
      out.append(text.substr(i, end - i));
      i = end;
      continue;
    }
    if (c == '[' || c == '{')
    {
      if (depth >= MAX_JSON_DEPTH)
      {
        const size_t end = MatchingClose(text, i);
        if (end == std::string_view::npos)
          return std::nullopt;
        out += "null";
        i = end + 1;
        continue;
      }
      ++depth;
    }
    else if (c == ']' || c == '}')
    {
      --depth;
    }
    else if (c == '-' || (c >= '0' && c <= '9'))
    {
      size_t end = i + 1;
      while (end < text.size() &&
             ((text[end] >= '0' && text[end] <= '9') || text[end] == '.' || text[end] == 'e' ||
              text[end] == 'E' || text[end] == '+' || text[end] == '-'))
      {
        ++end;
      }
      const std::string_view number = text.substr(i, end - i);
      if (SafeNumber(number, numbers))
        out.append(number);
      else
        out += "null";
      i = end;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

bool ParseJson(const std::string& text, picojson::value* value, NumberPolicy numbers)
{
  const std::optional<std::string> safe = SanitizeJson(text, numbers);
  return safe && picojson::parse(*value, *safe).empty();
}

std::string ParseWholeJson(std::string_view text, picojson::value* value)
{
  const auto safe = SanitizeJson(text);
  if (!safe)
    return "invalid JSON";
  std::string error;
  const auto end = picojson::parse(*value, safe->begin(), safe->end(), &error);
  if (!error.empty())
    return error;
  if (!std::all_of(end, safe->end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
      }))
  {
    return "something after the JSON value";
  }
  return {};
}
}  // namespace Orca::Json
