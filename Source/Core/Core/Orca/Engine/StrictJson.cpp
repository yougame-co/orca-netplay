// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Engine/StrictJson.h"

#include <string>

#include "Common/CommonTypes.h"

namespace Orca::Engine
{
namespace
{
// A number literal's longest spelling (the site's limit too).
constexpr size_t MAX_NUMBER_BYTES = 32;

class Reader
{
public:
  Reader(std::string_view text, int max_depth) : m_text(text), m_max_depth(max_depth) {}

  bool Document(picojson::value* out)
  {
    if (!Value(out, 1))
      return false;
    SkipSpace();
    if (m_pos != m_text.size())
      return Fail("text after the JSON value");
    return true;
  }

  const std::string& Error() const { return m_error; }

private:
  bool Fail(std::string_view why)
  {
    if (m_error.empty())
      m_error = std::string(why) + " at byte " + std::to_string(m_pos);
    return false;
  }

  bool AtEnd() const { return m_pos >= m_text.size(); }
  unsigned char Peek() const { return static_cast<unsigned char>(m_text[m_pos]); }

  void SkipSpace()
  {
    while (!AtEnd() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\n' || Peek() == '\r'))
      ++m_pos;
  }

  bool Literal(std::string_view word)
  {
    if (m_text.substr(m_pos, word.size()) != word)
      return Fail("unexpected character");
    m_pos += word.size();
    return true;
  }

  bool Value(picojson::value* out, int depth)
  {
    if (depth > m_max_depth)
      return Fail("nested deeper than " + std::to_string(m_max_depth) + " levels");
    SkipSpace();
    if (AtEnd())
      return Fail("unexpected end");
    switch (Peek())
    {
    case '{':
      return Object(out, depth);
    case '[':
      return Array(out, depth);
    case '"':
    {
      std::string s;
      if (!String(&s))
        return false;
      *out = picojson::value(std::move(s));
      return true;
    }
    case 't':
      *out = picojson::value(true);
      return Literal("true");
    case 'f':
      *out = picojson::value(false);
      return Literal("false");
    case 'n':
      *out = picojson::value();
      return Literal("null");
    default:
      return Number(out);
    }
  }

  bool Object(picojson::value* out, int depth)
  {
    ++m_pos;  // {
    picojson::object object;
    SkipSpace();
    if (!AtEnd() && Peek() == '}')
    {
      ++m_pos;
      *out = picojson::value(std::move(object));
      return true;
    }
    while (true)
    {
      SkipSpace();
      if (AtEnd() || Peek() != '"')
        return Fail("expected a key");
      std::string key;
      if (!String(&key))
        return false;
      if (object.contains(key))
        return Fail("duplicate key \"" + key + "\"");
      SkipSpace();
      if (AtEnd() || Peek() != ':')
        return Fail("expected ':'");
      ++m_pos;
      picojson::value item;
      if (!Value(&item, depth + 1))
        return false;
      object.emplace(std::move(key), std::move(item));
      SkipSpace();
      if (AtEnd())
        return Fail("expected ',' or '}'");
      if (Peek() == ',')
      {
        ++m_pos;
        continue;
      }
      if (Peek() == '}')
      {
        ++m_pos;
        *out = picojson::value(std::move(object));
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool Array(picojson::value* out, int depth)
  {
    ++m_pos;  // [
    picojson::array array;
    SkipSpace();
    if (!AtEnd() && Peek() == ']')
    {
      ++m_pos;
      *out = picojson::value(std::move(array));
      return true;
    }
    while (true)
    {
      picojson::value item;
      if (!Value(&item, depth + 1))
        return false;
      array.push_back(std::move(item));
      SkipSpace();
      if (AtEnd())
        return Fail("expected ',' or ']'");
      if (Peek() == ',')
      {
        ++m_pos;
        continue;
      }
      if (Peek() == ']')
      {
        ++m_pos;
        *out = picojson::value(std::move(array));
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  static void Utf8(std::string* out, u32 cp)
  {
    if (cp < 0x80)
    {
      out->push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // Four hex digits after "\u".
  bool Hex4(u32* out)
  {
    if (m_text.size() - m_pos < 4)
      return Fail("bad \\u escape");
    u32 v = 0;
    for (int i = 0; i < 4; ++i)
    {
      const unsigned char c = Peek();
      u32 d;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        d = c - 'A' + 10;
      else
        return Fail("bad \\u escape");
      v = v * 16 + d;
      ++m_pos;
    }
    *out = v;
    return true;
  }

  // One raw UTF-8 sequence starting at a byte >= 0x80: well formed, shortest form, no surrogates,
  // at most U+10FFFF (what a fatal TextDecoder accepts).
  bool RawUtf8(std::string* out)
  {
    const unsigned char lead = Peek();
    int extra;
    u32 cp;
    if (lead >= 0xC2 && lead <= 0xDF)
    {
      extra = 1;
      cp = lead & 0x1F;
    }
    else if (lead >= 0xE0 && lead <= 0xEF)
    {
      extra = 2;
      cp = lead & 0x0F;
    }
    else if (lead >= 0xF0 && lead <= 0xF4)
    {
      extra = 3;
      cp = lead & 0x07;
    }
    else
    {
      return Fail("invalid UTF-8");
    }
    if (m_text.size() - m_pos < static_cast<size_t>(extra) + 1)
      return Fail("invalid UTF-8");
    for (int i = 1; i <= extra; ++i)
    {
      const unsigned char c = static_cast<unsigned char>(m_text[m_pos + i]);
      if ((c & 0xC0) != 0x80)
        return Fail("invalid UTF-8");
      cp = (cp << 6) | (c & 0x3F);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
    {
      return Fail("invalid UTF-8");
    }
    out->append(m_text.substr(m_pos, static_cast<size_t>(extra) + 1));
    m_pos += static_cast<size_t>(extra) + 1;
    return true;
  }

  bool String(std::string* out)
  {
    ++m_pos;  // opening quote
    while (true)
    {
      if (AtEnd())
        return Fail("unterminated string");
      const unsigned char c = Peek();
      if (c == '"')
      {
        ++m_pos;
        return true;
      }
      if (c < 0x20)
        return Fail("control character in a string");
      if (c >= 0x80)
      {
        if (!RawUtf8(out))
          return false;
        continue;
      }
      if (c != '\\')
      {
        out->push_back(static_cast<char>(c));
        ++m_pos;
        continue;
      }
      ++m_pos;
      if (AtEnd())
        return Fail("unterminated string");
      const unsigned char e = Peek();
      ++m_pos;
      switch (e)
      {
      case '"':
      case '\\':
      case '/':
        out->push_back(static_cast<char>(e));
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'u':
      {
        u32 cp;
        if (!Hex4(&cp))
          return false;
        if (cp >= 0xDC00 && cp <= 0xDFFF)
          return Fail("unpaired surrogate");
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
          u32 low;
          if (m_text.substr(m_pos, 2) != "\\u")
            return Fail("unpaired surrogate");
          m_pos += 2;
          if (!Hex4(&low))
            return false;
          if (low < 0xDC00 || low > 0xDFFF)
            return Fail("unpaired surrogate");
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        Utf8(out, cp);
        break;
      }
      default:
        return Fail("bad escape");
      }
    }
  }

  static bool Digit(unsigned char c) { return c >= '0' && c <= '9'; }

  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?, the site's grammar, at most
  // MAX_NUMBER_BYTES long, and as written an exact integer of magnitude at most 2^53 - 1 ("30",
  // "30.0", "3e1", "3000e-2", "-0"). Anything else ("0.5", "1e-400", "1.0000000000000001") is
  // refused here, not rounded: the value never depends on a host's decimal conversion, and the
  // site applies the same rule to the same text (orca-ruleset-validate.ts).
  bool Number(picojson::value* out)
  {
    const size_t start = m_pos;
    if (!AtEnd() && Peek() == '-')
      ++m_pos;
    if (AtEnd() || !Digit(Peek()))
      return Fail("unexpected character");
    if (Peek() == '0')
    {
      ++m_pos;
      if (!AtEnd() && Digit(Peek()))
        return Fail("leading zero");
    }
    else
    {
      while (!AtEnd() && Digit(Peek()))
        ++m_pos;
    }
    const size_t int_end = m_pos;
    size_t frac_begin = m_pos, frac_end = m_pos;
    if (!AtEnd() && Peek() == '.')
    {
      ++m_pos;
      frac_begin = m_pos;
      if (AtEnd() || !Digit(Peek()))
        return Fail("bad number");
      while (!AtEnd() && Digit(Peek()))
        ++m_pos;
      frac_end = m_pos;
    }
    s64 exponent = 0;
    if (!AtEnd() && (Peek() == 'e' || Peek() == 'E'))
    {
      ++m_pos;
      bool negative_exponent = false;
      if (!AtEnd() && (Peek() == '+' || Peek() == '-'))
      {
        negative_exponent = Peek() == '-';
        ++m_pos;
      }
      if (AtEnd() || !Digit(Peek()))
        return Fail("bad number");
      while (!AtEnd() && Digit(Peek()))
      {
        if (exponent < 1000)
          exponent = exponent * 10 + (Peek() - '0');
        ++m_pos;
      }
      if (negative_exponent)
        exponent = -exponent;
    }
    if (m_pos - start > MAX_NUMBER_BYTES)
      return Fail("number longer than " + std::to_string(MAX_NUMBER_BYTES) + " bytes");
    const bool negative = m_text[start] == '-';
    // The significant digits, int part then fraction, and the power of ten they are scaled by.
    std::string digits(m_text.substr(negative ? start + 1 : start, int_end - (negative ? start + 1 : start)));
    digits.append(m_text.substr(frac_begin, frac_end - frac_begin));
    exponent -= static_cast<s64>(frac_end - frac_begin);
    const size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos)
    {
      *out = picojson::value(negative ? -0.0 : 0.0);
      return true;
    }
    digits.erase(0, first);
    while (digits.back() == '0')
    {
      digits.pop_back();
      ++exponent;
    }
    constexpr u64 MAX_SAFE = (u64{1} << 53) - 1;
    if (exponent < 0 || static_cast<s64>(digits.size()) + exponent > 16)
      return Fail("not an exact integer of at most 2^53 - 1");
    u64 v = 0;
    for (const char c : digits)
      v = v * 10 + static_cast<u64>(c - '0');
    for (s64 i = 0; i < exponent; ++i)
      v *= 10;
    if (v > MAX_SAFE)
      return Fail("not an exact integer of at most 2^53 - 1");
    const double value = static_cast<double>(v);
    *out = picojson::value(negative ? -value : value);
    return true;
  }

  std::string_view m_text;
  int m_max_depth;
  size_t m_pos = 0;
  std::string m_error;
};
}  // namespace

std::optional<picojson::value> ReadStrictJson(std::string_view text, size_t max_bytes,
                                              int max_depth, std::string* error)
{
  if (text.size() > max_bytes)
  {
    if (error)
      *error = "over " + std::to_string(max_bytes) + " bytes";
    return std::nullopt;
  }
  Reader reader(text, max_depth);
  picojson::value out;
  if (!reader.Document(&out))
  {
    if (error)
      *error = reader.Error();
    return std::nullopt;
  }
  return out;
}
}  // namespace Orca::Engine
