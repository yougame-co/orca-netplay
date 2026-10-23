// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Rulesets as data (Orca split S5): the strict JSON reader, the one-schema interpreter and the
// ruleset reader, against the shared vectors the site's validator runs too.

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/FileUtil.h"
#include "Core/Orca/Engine/Ruleset.h"
#include "Core/Orca/Engine/Schema.h"
#include "Core/Orca/Engine/StrictJson.h"

namespace
{
using Orca::Engine::ReadRuleset;
using Orca::Engine::ReadStrictJson;
using Orca::Engine::SchemaMismatch;
using Orca::Engine::SchemaProblem;

std::string RepoPath(const std::string& relative)
{
  return (std::filesystem::path(__FILE__).parent_path() / "../../../../" / relative).string();
}

std::string ReadText(const std::string& relative)
{
  std::string text;
  EXPECT_TRUE(File::ReadFileToString(RepoPath(relative), text)) << relative;
  return text;
}

picojson::value Json(std::string_view text)
{
  std::string error;
  auto v = ReadStrictJson(text, 1 << 20, 16, &error);
  EXPECT_TRUE(v) << error;
  return v ? *v : picojson::value();
}

std::string FromHex(std::string_view hex)
{
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  return out;
}

std::string ToHex(std::string_view bytes)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string out;
  for (const char c : bytes)
  {
    out.push_back(DIGITS[static_cast<unsigned char>(c) >> 4]);
    out.push_back(DIGITS[static_cast<unsigned char>(c) & 15]);
  }
  return out;
}

// A JSON pointer's parent and last token (no escapes in the vectors).
std::pair<picojson::value*, std::string> Parent(picojson::value* root, const std::string& pointer)
{
  picojson::value* at = root;
  size_t i = 1;
  while (true)
  {
    const size_t next = pointer.find('/', i);
    const std::string token = pointer.substr(i, next == std::string::npos ? std::string::npos : next - i);
    if (next == std::string::npos)
      return {at, token};
    at = at->is<picojson::array>() ? &at->get<picojson::array>().at(std::stoul(token)) :
                                     &at->get<picojson::object>().at(token);
    i = next + 1;
  }
}

std::string CaseText(const picojson::value& vectors, const picojson::value& c)
{
  const picojson::object& o = c.get<picojson::object>();
  const std::string base_text = vectors.get("baseText").get<std::string>();
  const bool textual = o.contains("text") || o.contains("text_replace") || o.contains("text_append") ||
                       o.contains("text_prepend_hex") || o.contains("text_hex_replace") ||
                       o.contains("text_pad");
  if (!textual)
  {
    picojson::value doc = vectors.get("base");
    if (o.contains("remove"))
    {
      for (const picojson::value& p : o.at("remove").get<picojson::array>())
      {
        auto [parent, token] = Parent(&doc, p.get<std::string>());
        if (parent->is<picojson::array>())
        {
          auto& a = parent->get<picojson::array>();
          a.erase(a.begin() + std::stol(token));
        }
        else
        {
          parent->get<picojson::object>().erase(token);
        }
      }
    }
    if (o.contains("set"))
    {
      for (const auto& [pointer, value] : o.at("set").get<picojson::object>())
      {
        auto [parent, token] = Parent(&doc, pointer);
        if (parent->is<picojson::array>())
          parent->get<picojson::array>().at(std::stoul(token)) = value;
        else
          parent->get<picojson::object>()[token] = value;
      }
    }
    return doc.serialize(true);
  }
  std::string text = o.contains("text") ? o.at("text").get<std::string>() : base_text;
  if (o.contains("text_replace"))
  {
    const auto& r = o.at("text_replace").get<picojson::array>();
    const size_t at = text.find(r[0].get<std::string>());
    EXPECT_NE(at, std::string::npos) << c.get("name").get<std::string>();
    if (at != std::string::npos)
      text.replace(at, r[0].get<std::string>().size(), r[1].get<std::string>());
  }
  if (o.contains("text_hex_replace"))
  {
    const auto& r = o.at("text_hex_replace").get<picojson::array>();
    std::string hex = ToHex(text);
    const size_t at = hex.find(r[0].get<std::string>());
    EXPECT_NE(at, std::string::npos) << c.get("name").get<std::string>();
    if (at != std::string::npos)
      hex.replace(at, r[0].get<std::string>().size(), r[1].get<std::string>());
    text = FromHex(hex);
  }
  if (o.contains("text_append"))
    text += o.at("text_append").get<std::string>();
  if (o.contains("text_prepend_hex"))
    text = FromHex(o.at("text_prepend_hex").get<std::string>()) + text;
  if (o.contains("text_pad"))
  {
    const size_t want = static_cast<size_t>(o.at("text_pad").get<double>());
    const size_t brace = text.rfind('}');
    if (brace != std::string::npos && text.size() < want)
      text.insert(brace, std::string(want - text.size(), ' '));
    EXPECT_EQ(text.size(), want);
  }
  return text;
}
}  // namespace

TEST(OrcaRuleset, CompiledSchemaIsTheFile)
{
  EXPECT_EQ(Orca::Engine::RulesetSchemaText(), ReadText("Tools/orca/mods/ruleset.schema.json"));
  EXPECT_EQ(Orca::Engine::RulesetSchemaProblem(), std::nullopt);
}

TEST(OrcaRuleset, SchemaListsTheInterpretersKeywords)
{
  const picojson::value schema = Json(Orca::Engine::RulesetSchemaText());
  const auto& listed = schema.get("x-orca-keywords").get<picojson::array>();
  const auto keywords = Orca::Engine::SchemaKeywords();
  ASSERT_EQ(listed.size(), keywords.size());
  for (size_t i = 0; i < keywords.size(); ++i)
    EXPECT_EQ(listed[i].get<std::string>(), keywords[i]);
}

// Every case of the shared vectors: Orca and the site agree on each.
TEST(OrcaRuleset, Vectors)
{
  // The vectors file holds non-integers on purpose (cases' "set" values), so picojson reads it.
  picojson::value vectors;
  ASSERT_EQ(picojson::parse(vectors, ReadText("Tools/orca/mods/ruleset-vectors.json")), "");
  // baseText is the builtin file, and base the same value.
  EXPECT_EQ(vectors.get("baseText").get<std::string>(),
            ReadText("Source/Core/Core/Orca/Games/Brawl/Rulesets/brawl-supernova-2025.json"));
  EXPECT_EQ(Json(vectors.get("baseText").get<std::string>()), vectors.get("base"));
  const auto& cases = vectors.get("cases").get<picojson::array>();
  EXPECT_GE(cases.size(), 60u);
  for (const picojson::value& c : cases)
  {
    const std::string name = c.get("name").get<std::string>();
    const std::string text = CaseText(vectors, c);
    std::string error;
    const auto ruleset = ReadRuleset(text, &error);
    EXPECT_EQ(ruleset.has_value(), c.get("ok").get<bool>()) << name << ": " << error;
  }
}

TEST(OrcaRuleset, StrictJson)
{
  std::string error;
  const auto refused = [&](std::string_view text, int depth = 16) {
    error.clear();
    return !ReadStrictJson(text, 1 << 16, depth, &error) && !error.empty();
  };
  EXPECT_FALSE(refused(R"({"a": [1, 2.0, -0, 1e3, true, false, null, "\u00e9\ud83c\udfae"]})"));
  EXPECT_TRUE(refused(R"({"a": 1, "a": 2})"));
  EXPECT_TRUE(refused("{\"a\": 1} x"));
  EXPECT_TRUE(refused("01"));
  EXPECT_TRUE(refused("1."));
  EXPECT_TRUE(refused(".5"));
  EXPECT_TRUE(refused("-"));
  EXPECT_TRUE(refused("1e"));
  EXPECT_TRUE(refused("1e400"));
  EXPECT_TRUE(refused("NaN"));
  EXPECT_TRUE(refused("\"a\tb\""));
  EXPECT_TRUE(refused("\"\\ud800\""));
  EXPECT_TRUE(refused("\"\\udc00\""));
  EXPECT_TRUE(refused("\"\\ud800\\u0041\""));
  EXPECT_TRUE(refused("\"\xC0\xAF\""));
  EXPECT_TRUE(refused("\"\xED\xA0\x80\""));
  EXPECT_TRUE(refused("\"\xF4\x90\x80\x80\""));
  EXPECT_TRUE(refused("\"\x80\""));
  EXPECT_TRUE(refused("\"\xE2\x82\""));
  EXPECT_TRUE(refused("[1,]"));
  EXPECT_TRUE(refused("{,}"));
  EXPECT_TRUE(refused(""));
  EXPECT_TRUE(refused("  "));
  EXPECT_TRUE(refused("tru"));
  EXPECT_TRUE(refused("\xEF\xBB\xBF{}"));
  // Depth: the root is level 1; [[1]] has three levels.
  EXPECT_FALSE(refused("[[1]]", 3));
  EXPECT_TRUE(refused("[[1]]", 2));
  EXPECT_TRUE(refused(std::string(100000, '['), 16));
  // Size, before anything is read.
  EXPECT_TRUE(!ReadStrictJson("[1]", 2, 16, &error));
  // Numbers: exact integers, the same in every locale.
  const auto n = [&](std::string_view text) { return ReadStrictJson(text, 64, 2, &error)->get<double>(); };
  EXPECT_EQ(n("30"), 30.0);
  EXPECT_EQ(n("30.0"), 30.0);
  EXPECT_EQ(n("3e1"), 30.0);
  EXPECT_EQ(n("3000e-2"), 30.0);
  EXPECT_EQ(n("-7"), -7.0);
  EXPECT_EQ(n("9007199254740991"), 9007199254740991.0);
  EXPECT_EQ(n("0e-400"), 0.0);
  EXPECT_EQ(n("3E+1"), 30.0);
  // Only exact integers within 2^53 - 1, as written, and at most 32 bytes: no host's decimal
  // conversion decides anything.
  for (const char* text : {"9007199254740992", "2.5", "1e-400", "1.0000000000000001",
                           "0.99999999999999999", "1e16", "30.000000000000000000000000000000"})
  {
    EXPECT_TRUE(refused(text)) << text;
  }
  // Escapes.
  EXPECT_EQ(ReadStrictJson(R"("\"\\\/\b\f\n\r\t\u0041\u00e9")", 64, 2, &error)->get<std::string>(),
            "\"\\/\b\f\n\r\tA\xC3\xA9");
}

TEST(OrcaRuleset, SchemaLintRefusesOutsideTheSubset)
{
  const picojson::value good = Json(Orca::Engine::RulesetSchemaText());
  ASSERT_EQ(SchemaProblem(good), std::nullopt);
  const auto with = [&](const std::string& pointer, const picojson::value& value) {
    picojson::value s = good;
    auto [parent, token] = Parent(&s, pointer);
    parent->get<picojson::object>()[token] = value;
    return SchemaProblem(s).has_value();
  };
  const auto without = [&](const std::string& pointer) {
    picojson::value s = good;
    auto [parent, token] = Parent(&s, pointer);
    parent->get<picojson::object>().erase(token);
    return SchemaProblem(s).has_value();
  };
  EXPECT_TRUE(with("/properties/format/multipleOf", picojson::value(1.0)));
  EXPECT_TRUE(with("/properties/format/exclusiveMaximum", picojson::value(2.0)));
  EXPECT_TRUE(with("/additionalProperties", picojson::value(true)));
  EXPECT_TRUE(without("/additionalProperties"));
  EXPECT_TRUE(without("/properties/format/maximum"));
  EXPECT_TRUE(without("/properties/id/x-orca-charset"));
  EXPECT_TRUE(without("/properties/id/maxLength"));
  EXPECT_TRUE(without("/properties/stages/maxItems"));
  EXPECT_TRUE(with("/properties/id/pattern", picojson::value("^.*$")));
  EXPECT_TRUE(with("/$defs/text/pattern", picojson::value("^[a-z0-9][a-z0-9-]*$")));
  EXPECT_TRUE(with("/properties/format/type", picojson::value("number")));
  EXPECT_TRUE(with("/properties/name/$ref", picojson::value("#/$defs/nothing")));
  EXPECT_TRUE(with("/$defs/seconds/$ref", picojson::value("#/$defs/graceFrames")));
  EXPECT_TRUE(with("/properties/name/minimum", picojson::value(1.0)));
  EXPECT_TRUE(with("/required", picojson::value(picojson::array{picojson::value("nothing")})));
  EXPECT_TRUE(with("/properties/format/minimum", picojson::value(2.0)));
  EXPECT_TRUE(with("/properties/format/maximum", picojson::value(1.5)));
  EXPECT_TRUE(with("/properties/block/x-orca-indexOf", picojson::value("stages")));
  EXPECT_TRUE(with("/properties/format/x-orca-rules", picojson::value(picojson::array{})));
  EXPECT_TRUE(with("/x-orca-keywords", picojson::value(picojson::array{picojson::value("type")})));
  EXPECT_TRUE(with("/x-orca-limits", picojson::value(picojson::object{})));
  EXPECT_TRUE(with("/properties/steps/properties/later/properties/kind/type",
                   picojson::value("string")));
}

TEST(OrcaRuleset, MismatchesNameThePath)
{
  const picojson::value schema = Json(Orca::Engine::RulesetSchemaText());
  picojson::value doc = Json(ReadText("Source/Core/Core/Orca/Games/Brawl/Rulesets/pplus-2024.json"));
  EXPECT_EQ(SchemaMismatch(schema, doc), std::nullopt);
  doc.get<picojson::object>()["stages"].get<picojson::array>()[3].get<picojson::object>()["kind"] =
      picojson::value(300.0);
  EXPECT_EQ(SchemaMismatch(schema, doc), "/stages/3/kind: must be 0-255");
  doc = Json(ReadText("Source/Core/Core/Orca/Games/Brawl/Rulesets/pplus-2024.json"));
  doc.get<picojson::object>()["extra"] = picojson::value(1.0);
  EXPECT_EQ(SchemaMismatch(schema, doc), "(root): unknown key \"extra\"");
}

// The same text reads to the same ruleset, field for field (no host state, no locale).
TEST(OrcaRuleset, ReadingIsAPureFunction)
{
  const std::string text = ReadText("Source/Core/Core/Orca/Games/Brawl/Rulesets/brawl-supernova-2025.json");
  std::string error;
  const auto a = ReadRuleset(text, &error);
  ASSERT_TRUE(a) << error;
  const auto b = ReadRuleset(text, &error);
  ASSERT_TRUE(b);
  EXPECT_TRUE(*a == *b);
}
