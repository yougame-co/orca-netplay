// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Engine/Schema.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "Common/CommonTypes.h"

namespace Orca::Engine
{
namespace
{
using Object = picojson::object;
using Array = picojson::array;

constexpr std::array<std::string_view, 24> KEYWORDS{
    "$schema",      "$id",           "$defs",           "$ref",           "title",
    "description",  "x-orca-limits", "x-orca-keywords", "x-orca-rules",   "type",
    "properties",   "required",      "additionalProperties", "items",     "minItems",
    "maxItems",     "minimum",       "maximum",         "minLength",      "maxLength",
    "pattern",      "enum",          "x-orca-charset",  "x-orca-indexOf",
};
// x-orca-charset "id", as a pattern for other JSON Schema tools.
constexpr std::string_view ID_PATTERN = "^[a-z0-9][a-z0-9-]*$";
constexpr double MAX_SAFE_INTEGER = 9007199254740991.0;  // 2^53 - 1, as JavaScript's
constexpr double MAX_LENGTH = 65536;
constexpr std::string_view DEFS_PREFIX = "#/$defs/";

std::string Where(const std::string& path)
{
  return path.empty() ? std::string("(root)") : path;
}

// A JSON integer a JavaScript number holds exactly.
bool Integer(const picojson::value& v, double* out)
{
  if (!v.is<double>())
    return false;
  const double d = v.get<double>();
  if (!std::isfinite(d) || d != std::floor(d) || std::fabs(d) > MAX_SAFE_INTEGER)
    return false;
  *out = d;
  return true;
}

const picojson::value* Member(const Object& o, std::string_view key)
{
  const auto it = o.find(std::string(key));
  return it == o.end() ? nullptr : &it->second;
}

// Code points of well-formed UTF-8, or false.
template <typename F>
bool EachCodePoint(std::string_view s, F&& f)
{
  for (size_t i = 0; i < s.size();)
  {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    u32 cp;
    size_t extra;
    if (lead < 0x80)
    {
      cp = lead;
      extra = 0;
    }
    else if (lead >= 0xC2 && lead <= 0xDF)
    {
      cp = lead & 0x1F;
      extra = 1;
    }
    else if (lead >= 0xE0 && lead <= 0xEF)
    {
      cp = lead & 0x0F;
      extra = 2;
    }
    else if (lead >= 0xF0 && lead <= 0xF4)
    {
      cp = lead & 0x07;
      extra = 3;
    }
    else
    {
      return false;
    }
    if (s.size() - i < extra + 1)
      return false;
    for (size_t k = 1; k <= extra; ++k)
    {
      const unsigned char c = static_cast<unsigned char>(s[i + k]);
      if ((c & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
    {
      return false;
    }
    if (!f(cp))
      return false;
    i += extra + 1;
  }
  return true;
}

bool IdCharset(std::string_view s)
{
  if (s.empty() || s[0] == '-')
    return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

bool TextCharset(std::string_view s)
{
  return EachCodePoint(s, [](u32 cp) { return cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp <= 0x9F); });
}

// "/a/b": segments of [A-Za-z0-9_], at least one.
bool SimplePointer(std::string_view p)
{
  if (p.size() < 2 || p[0] != '/')
    return false;
  size_t seg = 0;
  for (size_t i = 1; i <= p.size(); ++i)
  {
    if (i == p.size() || p[i] == '/')
    {
      if (seg == 0)
        return false;
      seg = 0;
      continue;
    }
    const char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
    ++seg;
  }
  return true;
}

const picojson::value* Resolve(const picojson::value& root, std::string_view pointer)
{
  const picojson::value* at = &root;
  size_t i = 1;
  while (i <= pointer.size())
  {
    const size_t next = std::min(pointer.find('/', i), pointer.size());
    const std::string key(pointer.substr(i, next - i));
    if (!at->is<Object>())
      return nullptr;
    at = Member(at->get<Object>(), key);
    if (!at)
      return nullptr;
    i = next + 1;
  }
  return at;
}

// ---- The schema itself ----

class Lint
{
public:
  explicit Lint(const Object* defs) : m_defs(defs) {}

  std::optional<std::string> Node(const picojson::value& node, const std::string& where,
                                  bool in_def, bool root = false) const
  {
    if (!node.is<Object>())
      return where + ": a schema node must be an object";
    const Object& o = node.get<Object>();
    static constexpr std::array<std::string_view, 6> ROOT_ONLY{
        "$schema", "$id", "$defs", "x-orca-limits", "x-orca-keywords", "x-orca-rules"};
    for (const auto& [key, value] : o)
    {
      if (std::ranges::find(KEYWORDS, key) == KEYWORDS.end())
        return where + ": unknown keyword " + key;
      if (!root && std::ranges::find(ROOT_ONLY, key) != ROOT_ONLY.end())
        return where + ": " + key + " only at the root";
      if ((key == "description" || key == "title") && !value.is<std::string>())
        return where + ": " + key + " must be a string";
    }
    const auto only = [&](std::initializer_list<std::string_view> allowed) -> std::optional<std::string> {
      for (const auto& [key, value] : o)
      {
        if (key == "description" || key == "title" ||
            (root && std::ranges::find(ROOT_ONLY, key) != ROOT_ONLY.end()))
        {
          continue;
        }
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
          return where + ": " + key + " does not apply here";
      }
      return std::nullopt;
    };
    if (const picojson::value* ref = Member(o, "$ref"))
    {
      if (in_def || root)
        return where + ": no $ref here (a definition holds none, so a $ref is one hop)";
      if (!ref->is<std::string>())
        return where + ": $ref must be a string";
      const std::string& r = ref->get<std::string>();
      if (!r.starts_with(DEFS_PREFIX) || !m_defs || !Member(*m_defs, r.substr(DEFS_PREFIX.size())))
        return where + ": $ref " + r + " names no definition";
      return only({"$ref"});
    }
    if (const picojson::value* e = Member(o, "enum"))
    {
      if (auto p = only({"enum"}))
        return p;
      if (!e->is<Array>() || e->get<Array>().empty())
        return where + ": enum must be a non-empty array";
      std::set<std::string> seen;
      for (const picojson::value& item : e->get<Array>())
      {
        if (!item.is<std::string>() || !TextCharset(item.get<std::string>()) ||
            !seen.insert(item.get<std::string>()).second)
        {
          return where + ": enum lists distinct strings only";
        }
      }
      return std::nullopt;
    }
    const picojson::value* type = Member(o, "type");
    if (!type || !type->is<std::string>())
      return where + ": every node has a type (or an enum)";
    const std::string& t = type->get<std::string>();
    const auto bound = [&](std::string_view key, double lo, double hi, double* out) -> bool {
      const picojson::value* v = Member(o, key);
      double d;
      if (!v || !Integer(*v, &d) || d < lo || d > hi)
        return false;
      *out = d;
      return true;
    };
    if (t == "object")
    {
      if (auto p = only({"type", "properties", "required", "additionalProperties"}))
        return p;
      const picojson::value* props = Member(o, "properties");
      if (!props || !props->is<Object>())
        return where + ": an object node lists its properties";
      const picojson::value* extra = Member(o, "additionalProperties");
      if (!extra || !extra->is<bool>() || extra->get<bool>())
        return where + ": an object node has additionalProperties false";
      for (const auto& [key, sub] : props->get<Object>())
      {
        if (auto p = Node(sub, where + "/properties/" + key, in_def))
          return p;
      }
      if (const picojson::value* req = Member(o, "required"))
      {
        if (!req->is<Array>())
          return where + ": required must be an array";
        std::set<std::string> seen;
        for (const picojson::value& item : req->get<Array>())
        {
          if (!item.is<std::string>() || !Member(props->get<Object>(), item.get<std::string>()) ||
              !seen.insert(item.get<std::string>()).second)
          {
            return where + ": required lists distinct properties";
          }
        }
      }
      return std::nullopt;
    }
    if (t == "array")
    {
      if (auto p = only({"type", "items", "minItems", "maxItems"}))
        return p;
      double max_items, min_items = 0;
      if (!bound("maxItems", 0, MAX_LENGTH, &max_items))
        return where + ": an array node has a maxItems (0-65536)";
      if (Member(o, "minItems") && !bound("minItems", 0, max_items, &min_items))
        return where + ": minItems must be 0-maxItems";
      const picojson::value* items = Member(o, "items");
      if (!items)
        return where + ": an array node has items";
      return Node(*items, where + "/items", in_def);
    }
    if (t == "integer")
    {
      if (auto p = only({"type", "minimum", "maximum", "x-orca-indexOf"}))
        return p;
      double lo, hi;
      if (!bound("minimum", -MAX_SAFE_INTEGER, MAX_SAFE_INTEGER, &lo) ||
          !bound("maximum", lo, MAX_SAFE_INTEGER, &hi))
      {
        return where + ": an integer node has a minimum and a maximum above it";
      }
      if (const picojson::value* index = Member(o, "x-orca-indexOf");
          index && (!index->is<std::string>() || !SimplePointer(index->get<std::string>())))
      {
        return where + ": x-orca-indexOf must be a pointer like /a/b";
      }
      return std::nullopt;
    }
    if (t == "boolean")
      return only({"type"});
    if (t == "string")
    {
      if (auto p = only({"type", "minLength", "maxLength", "x-orca-charset", "pattern"}))
        return p;
      double lo, hi;
      if (!bound("minLength", 0, MAX_LENGTH, &lo) || !bound("maxLength", lo, MAX_LENGTH, &hi))
        return where + ": a string node has a minLength and a maxLength above it";
      const picojson::value* charset = Member(o, "x-orca-charset");
      if (!charset || !charset->is<std::string>() ||
          (charset->get<std::string>() != "id" && charset->get<std::string>() != "text"))
      {
        return where + ": a string node has an x-orca-charset (id or text)";
      }
      if (const picojson::value* pattern = Member(o, "pattern");
          pattern && (charset->get<std::string>() != "id" || !pattern->is<std::string>() ||
                      pattern->get<std::string>() != ID_PATTERN))
      {
        return where + ": pattern is only the id charset's own (" + std::string(ID_PATTERN) + ")";
      }
      return std::nullopt;
    }
    return where + ": unknown type " + t;
  }

private:
  const Object* m_defs;
};

// ---- A document ----

class Check
{
public:
  Check(const Object* defs, const picojson::value& document) : m_defs(defs), m_document(document)
  {
  }

  std::optional<std::string> Node(const picojson::value& node, const picojson::value& v,
                                  const std::string& path) const
  {
    const Object& o = node.get<Object>();
    if (const picojson::value* ref = Member(o, "$ref"))
    {
      const std::string& r = ref->get<std::string>();
      return Node(*Member(*m_defs, r.substr(DEFS_PREFIX.size())), v, path);
    }
    if (const picojson::value* e = Member(o, "enum"))
    {
      if (v.is<std::string>())
      {
        for (const picojson::value& item : e->get<Array>())
        {
          if (item.get<std::string>() == v.get<std::string>())
            return std::nullopt;
        }
      }
      std::string list;
      for (const picojson::value& item : e->get<Array>())
        list += (list.empty() ? "" : ", ") + item.get<std::string>();
      return Where(path) + ": must be one of " + list;
    }
    const std::string& t = Member(o, "type")->get<std::string>();
    const auto num = [&](std::string_view key) { return Member(o, key)->get<double>(); };
    if (t == "object")
    {
      if (!v.is<Object>())
        return Where(path) + ": must be an object";
      const Object& props = Member(o, "properties")->get<Object>();
      for (const auto& [key, value] : v.get<Object>())
      {
        if (!Member(props, key))
          return Where(path) + ": unknown key \"" + key + "\"";
      }
      if (const picojson::value* req = Member(o, "required"))
      {
        for (const picojson::value& item : req->get<Array>())
        {
          if (!Member(v.get<Object>(), item.get<std::string>()))
            return Where(path) + ": missing \"" + item.get<std::string>() + "\"";
        }
      }
      for (const auto& [key, value] : v.get<Object>())
      {
        if (auto p = Node(*Member(props, key), value, path + "/" + key))
          return p;
      }
      return std::nullopt;
    }
    if (t == "array")
    {
      if (!v.is<Array>())
        return Where(path) + ": must be an array";
      const Array& a = v.get<Array>();
      const double min_items = Member(o, "minItems") ? num("minItems") : 0;
      if (static_cast<double>(a.size()) < min_items || static_cast<double>(a.size()) > num("maxItems"))
      {
        return Where(path) + ": must have " + std::to_string(static_cast<u64>(min_items)) + "-" +
               std::to_string(static_cast<u64>(num("maxItems"))) + " items";
      }
      for (size_t i = 0; i < a.size(); ++i)
      {
        if (auto p = Node(*Member(o, "items"), a[i], path + "/" + std::to_string(i)))
          return p;
      }
      return std::nullopt;
    }
    if (t == "integer")
    {
      double d;
      if (!Integer(v, &d))
        return Where(path) + ": must be an integer";
      if (d < num("minimum") || d > num("maximum"))
      {
        return Where(path) + ": must be " + std::to_string(static_cast<s64>(num("minimum"))) +
               "-" + std::to_string(static_cast<s64>(num("maximum")));
      }
      if (const picojson::value* index = Member(o, "x-orca-indexOf"))
      {
        const picojson::value* target = Resolve(m_document, index->get<std::string>());
        if (!target || !target->is<Array>() ||
            d >= static_cast<double>(target->get<Array>().size()) || d < 0)
        {
          return Where(path) + ": must be an index into " + index->get<std::string>();
        }
      }
      return std::nullopt;
    }
    if (t == "boolean")
      return v.is<bool>() ? std::nullopt : std::optional(Where(path) + ": must be true or false");
    // string
    if (!v.is<std::string>())
      return Where(path) + ": must be a string";
    const std::string& s = v.get<std::string>();
    if (static_cast<double>(s.size()) < num("minLength") ||
        static_cast<double>(s.size()) > num("maxLength"))
    {
      return Where(path) + ": must be " + std::to_string(static_cast<u64>(num("minLength"))) +
             "-" + std::to_string(static_cast<u64>(num("maxLength"))) + " bytes";
    }
    const bool id = Member(o, "x-orca-charset")->get<std::string>() == "id";
    if (id ? !IdCharset(s) : !TextCharset(s))
    {
      return Where(path) + (id ? ": must be a-z 0-9 - (not starting with -)" :
                                 ": must be printable UTF-8");
    }
    return std::nullopt;
  }

private:
  const Object* m_defs;
  const picojson::value& m_document;
};
}  // namespace

std::span<const std::string_view> SchemaKeywords()
{
  return KEYWORDS;
}

std::optional<std::string> SchemaProblem(const picojson::value& schema)
{
  if (!schema.is<Object>())
    return "a schema is an object";
  const Object& root = schema.get<Object>();
  for (const char* key : {"$schema", "$id"})
  {
    if (const picojson::value* v = Member(root, key); v && !v->is<std::string>())
      return std::string(key) + " must be a string";
  }
  const picojson::value* keywords = Member(root, "x-orca-keywords");
  if (!keywords || !keywords->is<Array>() || keywords->get<Array>().size() != KEYWORDS.size())
    return "x-orca-keywords must list the interpreter's keywords";
  for (size_t i = 0; i < KEYWORDS.size(); ++i)
  {
    const picojson::value& k = keywords->get<Array>()[i];
    if (!k.is<std::string>() || k.get<std::string>() != KEYWORDS[i])
      return "x-orca-keywords must list the interpreter's keywords, in its order";
  }
  if (const picojson::value* rules = Member(root, "x-orca-rules"))
  {
    if (!rules->is<Array>() ||
        !std::ranges::all_of(rules->get<Array>(), [](const picojson::value& r) { return r.is<std::string>(); }))
    {
      return "x-orca-rules must be strings";
    }
  }
  const picojson::value* limits = Member(root, "x-orca-limits");
  if (!limits || !limits->is<Object>() || limits->get<Object>().size() != 2)
    return "x-orca-limits must give maxBytes and maxDepth";
  double d;
  const picojson::value* max_bytes = Member(limits->get<Object>(), "maxBytes");
  const picojson::value* max_depth = Member(limits->get<Object>(), "maxDepth");
  if (!max_bytes || !Integer(*max_bytes, &d) || d < 1 || d > (1 << 20) || !max_depth ||
      !Integer(*max_depth, &d) || d < 1 || d > 16)
  {
    return "x-orca-limits: maxBytes 1-1048576, maxDepth 1-16";
  }
  const Object* defs = nullptr;
  if (const picojson::value* v = Member(root, "$defs"))
  {
    if (!v->is<Object>())
      return "$defs must be an object";
    defs = &v->get<Object>();
  }
  const Lint lint(defs);
  if (defs)
  {
    for (const auto& [name, node] : *defs)
    {
      if (auto p = lint.Node(node, "#/$defs/" + name, true))
        return p;
    }
  }
  if (auto p = lint.Node(schema, "#", false, true))
    return p;
  const picojson::value* type = Member(root, "type");
  if (!type || type->get<std::string>() != "object")
    return "the root is an object node";
  return std::nullopt;
}

std::optional<std::string> SchemaMismatch(const picojson::value& schema,
                                          const picojson::value& document)
{
  const Object& root = schema.get<Object>();
  const picojson::value* defs = Member(root, "$defs");
  const Check check(defs ? &defs->get<Object>() : nullptr, document);
  return check.Node(schema, document, "");
}
}  // namespace Orca::Engine
