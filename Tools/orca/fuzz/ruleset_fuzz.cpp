// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// libFuzzer target for the ruleset reader (Orca split S5): Engine/StrictJson, Engine/Schema and
// Engine/Ruleset, the code that will read a pack's ruleset file. Built and run by
// build-ruleset-fuzz.sh (Homebrew LLVM's clang, ASan + UBSan, no Dolphin libraries needed).
//
// Beyond "no crash, no sanitizer report", every ruleset the reader accepts must hold the invariants
// the engine's stage flows and set tracker rely on; a violation aborts.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "Core/Orca/Engine/Ruleset.h"
#include "Core/Orca/Engine/StrictJson.h"

namespace
{
void Require(bool ok)
{
  if (!ok)
    std::abort();
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  std::string error;
  // The reader alone, at its widest limits.
  (void)Orca::Engine::ReadStrictJson(text, size_t{1} << 20, 16, &error);
  const auto r = Orca::Engine::ReadRuleset(text, &error);
  if (!r)
  {
    Require(!error.empty());
    return 0;
  }
  namespace Ranked = Orca::UX::Ranked;
  Require(r->stage_count >= 1 && r->stage_count <= Ranked::MAX_STAGES);
  Require(r->starters != 0 && (r->starters & r->counterpicks) == 0);
  Require((r->starters | r->counterpicks) == r->All());
  Require(r->id >= 1);
  Require(r->ic_banned_stage < r->stage_count && (r->ic_banned_stage >= 0) == (r->ice_climbers >= 0));
  Require(r->later_count >= 1 && r->later_count <= 15);
  Require(!r->later_optional || r->later_kind == Ranked::StepKind::Ban);
  for (const auto seconds : {r->game1_seconds[0], r->game1_seconds[1], r->game1_seconds[2],
                             r->later_seconds, r->pick_seconds, r->casual_seconds})
  {
    Require(seconds >= 1 && seconds * 60u + r->grace_frames <= 0xFFFF &&
            seconds * 60u + r->casual_grace_frames <= 0xFFFF);
  }
  Require(r->wins_needed >= 1 && r->wins_needed <= 2);
  Require(r->stocks >= 1 && r->stocks <= 99 && r->minutes >= 1 && r->minutes <= 99);
  Require(r->void_after_ties >= 1 && r->void_after_ties <= 8);
  Require(r->css_ready_seconds >= 1 && r->css_ready_seconds * 60u <= 0xFFFF);
  Require(r->queue_ready_seconds >= 1 && r->queue_ready_seconds * 60u <= 0xFFFF);
  Require(r->queue_skip_frames >= 1 && r->queue_skip_frames <= 600);
  Require(r->order_pick_seconds >= 1 && r->order_pick_seconds * 60u <= 0xFFFF);
  Require((r->time_out == Ranked::TimeOut::Judged) || r->ledge == Ranked::LedgeLimits{});
  Require(r->ledge.character_count <= r->ledge.characters.size());
  Require(r->ledge.limit <= 254 && r->ledge.tiebreak_limit <= 254);
  Require(!r->tiebreak || (r->tiebreak_stocks >= 1 && r->tiebreak_minutes >= 1));
  for (int i = 0; i < r->stage_count; ++i)
    Require(!r->stage_names[static_cast<size_t>(i)].empty() && r->stage_names[static_cast<size_t>(i)].size() <= 48);
  Require(!r->slug.empty() && r->slug.size() <= 32 && !r->name.empty() && r->name.size() <= 48);
  // Reading is a pure function of the text.
  const auto again = Orca::Engine::ReadRuleset(text, &error);
  Require(again && *again == *r);
  Require(r->ledge.character_count <= 4);
  return 0;
}
