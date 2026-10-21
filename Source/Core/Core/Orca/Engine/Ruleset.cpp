// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Engine/Ruleset.h"

#include <algorithm>
#include <set>
#include <vector>

#include "Core/Orca/Engine/Schema.h"
#include "Core/Orca/Engine/StrictJson.h"

namespace Orca::Engine
{
namespace
{
namespace Ranked = UX::Ranked;
using Object = picojson::object;
using Array = picojson::array;

// Tools/orca/mods/ruleset.schema.json, as bytes (CMake writes the list).
constexpr char SCHEMA_TEXT[] = {
#include "ruleset.schema.json.inc"
};

struct Compiled
{
  picojson::value schema;
  size_t max_bytes = 0;
  int max_depth = 0;
  std::optional<std::string> problem;
};

const Compiled& Schema()
{
  static const Compiled compiled = [] {
    Compiled c;
    std::string error;
    auto schema = ReadStrictJson(RulesetSchemaText(), size_t{1} << 20, 16, &error);
    if (!schema)
    {
      c.problem = "the ruleset schema doesn't parse: " + error;
      return c;
    }
    if (auto p = SchemaProblem(*schema))
    {
      c.problem = "the ruleset schema is outside the subset: " + *p;
      return c;
    }
    const Object& limits = schema->get("x-orca-limits").get<Object>();
    c.max_bytes = static_cast<size_t>(limits.at("maxBytes").get<double>());
    c.max_depth = static_cast<int>(limits.at("maxDepth").get<double>());
    c.schema = std::move(*schema);
    return c;
  }();
  return compiled;
}

// After the schema check every value below has its type and bounds, and every key read is one
// the schema requires (At() throws on a missing key: a schema edit that makes one optional must
// read it with Find()).
int Int(const picojson::value& v)
{
  return static_cast<int>(v.get<double>());
}
// A number before the schema's bounds are relied on: StrictJson keeps every number an exact
// integer of magnitude at most 2^53 - 1, so this never overflows, whatever the bounds say.
s64 Wide(const picojson::value& v)
{
  return static_cast<s64>(v.get<double>());
}
const picojson::value& At(const picojson::value& v, const char* key)
{
  return v.get<Object>().at(key);
}
const picojson::value* Find(const picojson::value& v, const char* key)
{
  const Object& o = v.get<Object>();
  const auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}

std::optional<Ranked::Ruleset> Fail(std::string* error, std::string why)
{
  if (error)
    *error = std::move(why);
  return std::nullopt;
}
}  // namespace

std::string_view RulesetSchemaText()
{
  return std::string_view(SCHEMA_TEXT, sizeof(SCHEMA_TEXT));
}

std::optional<std::string> RulesetSchemaProblem()
{
  return Schema().problem;
}

std::optional<Ranked::Ruleset> ReadRuleset(std::string_view json, std::string* error)
{
  const Compiled& compiled = Schema();
  if (compiled.problem)
    return Fail(error, *compiled.problem);
  std::string why;
  const auto doc = ReadStrictJson(json, compiled.max_bytes, compiled.max_depth, &why);
  if (!doc)
    return Fail(error, why);
  if (auto p = SchemaMismatch(compiled.schema, *doc))
    return Fail(error, *p);

  // Timers in frames fit the 16-bit counters the stage flows and the match block keep, checked on
  // the file's numbers before anything is narrowed (the schema's bounds already see to it; this
  // holds if they are ever widened). The site checks the same.
  {
    const picojson::value& st = At(*doc, "steps");
    const s64 grace = std::max(Wide(At(st, "graceFrames")), Wide(At(st, "casualGraceFrames")));
    std::vector<s64> seconds{Wide(At(At(st, "later"), "seconds")), Wide(At(st, "pickSeconds")),
                             Wide(At(st, "casualSeconds"))};
    for (const picojson::value& g : At(st, "game1Seconds").get<Array>())
      seconds.push_back(Wide(g));
    for (const s64 s : seconds)
    {
      if (s * 60 + grace > 0xFFFF)
        return Fail(error, "/steps: a step's frames must fit 16 bits");
    }
    for (const s64 s : {Wide(At(At(*doc, "css"), "readySeconds")),
                        Wide(At(At(*doc, "queue"), "readySeconds")),
                        Wide(At(At(*doc, "characterOrder"), "pickSeconds"))})
    {
      if (s * 60 > 0xFFFF)
        return Fail(error, "/css, /queue, /characterOrder: a timer's frames must fit 16 bits");
    }
  }

  // The x-orca-rules a schema can't state.
  Ranked::Ruleset r;
  r.id = static_cast<u8>(Int(At(*doc, "block")));
  r.slug = At(*doc, "id").get<std::string>();
  r.name = At(*doc, "name").get<std::string>();
  const Array& stages = At(*doc, "stages").get<Array>();
  r.stage_count = static_cast<int>(stages.size());
  for (size_t i = 0; i < stages.size(); ++i)
  {
    r.stage_names[i] = At(stages[i], "name").get<std::string>();
    r.stage_kinds[i] = static_cast<u8>(Int(At(stages[i], "kind")));
    const auto bit = static_cast<Ranked::StageMask>(1u << i);
    if (At(stages[i], "role").get<std::string>() == "starter")
      r.starters |= bit;
    else
      r.counterpicks |= bit;
  }
  if (r.starters == 0)
    return Fail(error, "/stages: at least one stage is a starter");

  const picojson::value& steps = At(*doc, "steps");
  for (size_t i = 0; i < r.game1_seconds.size(); ++i)
    r.game1_seconds[i] = static_cast<u16>(Int(At(steps, "game1Seconds").get<Array>()[i]));
  const picojson::value& later = At(steps, "later");
  r.later_kind = At(later, "kind").get<std::string>() == "ban" ? Ranked::StepKind::Ban :
                                                                 Ranked::StepKind::Strike;
  r.later_count = static_cast<u8>(Int(At(later, "count")));
  r.later_optional = At(later, "optional").get<bool>();
  if (r.later_optional && r.later_kind != Ranked::StepKind::Ban)
    return Fail(error, "/steps/later/optional: only a ban is optional");
  r.later_seconds = static_cast<u16>(Int(At(later, "seconds")));
  r.pick_seconds = static_cast<u16>(Int(At(steps, "pickSeconds")));
  r.grace_frames = static_cast<u16>(Int(At(steps, "graceFrames")));
  r.casual_seconds = static_cast<u16>(Int(At(steps, "casualSeconds")));
  r.casual_grace_frames = static_cast<u16>(Int(At(steps, "casualGraceFrames")));
  r.dsr = At(*doc, "dsr").get<bool>();

  const picojson::value& clauses = At(*doc, "clauses");
  r.ic_banned_stage = -1;
  r.ice_climbers = -1;
  r.meta_knight = -1;
  if (const picojson::value* ic = Find(clauses, "iceClimbers"))
  {
    r.ice_climbers = Int(At(*ic, "character"));
    r.ic_banned_stage = Int(At(*ic, "bannedStage"));
  }
  if (const picojson::value* mk = Find(clauses, "metaKnight"))
    r.meta_knight = Int(At(*mk, "character"));

  const picojson::value& set = At(*doc, "set");
  r.wins_needed = Int(At(set, "wins"));
  r.stocks = static_cast<u8>(Int(At(set, "stocks")));
  r.minutes = static_cast<u8>(Int(At(set, "minutes")));
  r.time_out = At(set, "timeOut").get<std::string>() == "judged" ? Ranked::TimeOut::Judged :
                                                                   Ranked::TimeOut::Results;
  const picojson::value* ledge = Find(set, "ledgeGrabLimit");
  if ((ledge != nullptr) != (r.time_out == Ranked::TimeOut::Judged))
    return Fail(error, "/set/ledgeGrabLimit: given exactly when timeOut is judged");
  if (ledge)
  {
    r.ledge.limit = static_cast<u8>(Int(At(*ledge, "limit")));
    r.ledge.tiebreak_limit = static_cast<u8>(Int(At(*ledge, "tiebreakLimit")));
    std::set<int> seen;
    for (const picojson::value& c : At(*ledge, "characters").get<Array>())
    {
      const int character = Int(At(c, "character"));
      if (!seen.insert(character).second)
        return Fail(error, "/set/ledgeGrabLimit/characters: a character is listed twice");
      r.ledge.characters[r.ledge.character_count++] = {
          character, static_cast<u8>(Int(At(c, "limit"))),
          static_cast<u8>(Int(At(c, "tiebreakLimit")))};
    }
  }
  if (const picojson::value* tiebreak = Find(set, "tiebreak"))
  {
    r.tiebreak = true;
    r.tiebreak_stocks = static_cast<u8>(Int(At(*tiebreak, "stocks")));
    r.tiebreak_minutes = static_cast<u8>(Int(At(*tiebreak, "minutes")));
  }
  else
  {
    r.tiebreak = false;
    r.tiebreak_stocks = 0;
    r.tiebreak_minutes = 0;
  }
  r.void_after_ties = static_cast<u8>(Int(At(set, "voidAfterTies")));
  r.css_ready_seconds = static_cast<u16>(Int(At(At(*doc, "css"), "readySeconds")));
  const picojson::value& queue = At(*doc, "queue");
  r.queue_ready_seconds = static_cast<u16>(Int(At(queue, "readySeconds")));
  r.queue_skip_frames = static_cast<u16>(Int(At(queue, "skipFrames")));
  r.order_pick_seconds = static_cast<u16>(Int(At(At(*doc, "characterOrder"), "pickSeconds")));
  return r;
}
}  // namespace Orca::Engine
