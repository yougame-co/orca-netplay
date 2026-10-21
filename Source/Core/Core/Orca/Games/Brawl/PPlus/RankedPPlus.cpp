// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/PPlus/RankedPPlus.h"

#include <algorithm>
#include <bit>
#include <mutex>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"

namespace Orca::UX::RankedPPlus
{
// Project+ v3.2, pf/stage/switch/Switch03.rss ("2024 Proposed"): 0x320 bytes, zeros after 0x182.
const u8 kSwitch03[RSS_EXDATA_SIZE] = {
    0x00, 0x00, 0x00, 0x03, 0x01, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x1C, 0x05, 0x0B,
    0x23, 0x08, 0x1A, 0x00, 0x28, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1B, 0x01, 0x38, 0x24, 0x3B, 0x04, 0x02, 0x2D, 0x31, 0x20, 0x0C, 0x39,
    0x15, 0x06, 0x33, 0x07, 0x32, 0x21, 0x09, 0x1F, 0x3D, 0x2E, 0x27, 0x35, 0x0A, 0x18, 0x0D, 0x36,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x0F, 0x3C, 0x10,
    0x34, 0x25, 0x3E, 0x0E, 0x22, 0x19, 0x14, 0x12, 0x26, 0x11, 0x13, 0x17, 0x16, 0x37, 0x2F, 0x1D,
    0x3A, 0x2C, 0x1B, 0x1E, 0x30, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06,
    0x07, 0x07, 0x08, 0x08, 0x09, 0x09, 0x33, 0x0A, 0x49, 0x2C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0E, 0x0E,
    0x13, 0x0F, 0x14, 0x10, 0x15, 0x11, 0x16, 0x12, 0x17, 0x13, 0x18, 0x14, 0x19, 0x15, 0x1C, 0x16,
    0x1D, 0x17, 0x1E, 0x18, 0x1F, 0x19, 0x20, 0x1A, 0x21, 0x1B, 0x22, 0x1C, 0x23, 0x1D, 0x24, 0x1E,
    0x43, 0x26, 0x29, 0x32, 0x2A, 0x33, 0x47, 0x2A, 0x2C, 0x35, 0x2D, 0x36, 0x2F, 0x37, 0x30, 0x38,
    0x31, 0x39, 0x32, 0x3A, 0x2E, 0x3B, 0xFF, 0x64, 0xFF, 0x64, 0x37, 0x3C, 0x40, 0x23, 0x41, 0x24,
    0x42, 0x25, 0x25, 0x1F, 0x44, 0x27, 0x45, 0x28, 0x46, 0x29, 0x2B, 0x34, 0x48, 0x2B, 0x0B, 0x0B,
    0x4A, 0x2D, 0x4B, 0x2E, 0x4C, 0x2F, 0x4D, 0x30, 0x4E, 0x31, 0x4F, 0x3D, 0x50, 0x3E, 0x51, 0x3F,
    0x52, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

namespace
{
using Ranked::Plan;
using Ranked::StageMask;
using Ranked::Step;
using Ranked::StepKind;
using Rollback::InputGate::Mask;

constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 SCENE_SSS_TASK = 0x3AC;
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_MODE_MELEE = 0x08;
constexpr u32 MODE_MELEE_STAGE = 0x1B;
// The preset's page 0: its stage count, the stage slots, and the slot table (slot -> kind).
constexpr u32 RSS_PAGE0_COUNT = RSS_EXDATA + 0x3C;
constexpr u32 RSS_PAGE0_SLOTS = RSS_EXDATA + 0x3D;
constexpr u32 RSS_SLOT_KINDS = RSS_EXDATA + 0x104;
constexpr int MAX_PAGE_STAGES = 31;

constexpr StageMask Bit(int i)
{
  return static_cast<StageMask>(1u << i);
}

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

// The stage select task while the scene is scSelStage, else 0.
u32 StageSelectTask(const GuestMemory& m, const std::string& scene)
{
  if (scene != "scSelStage" || !Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + 4))
    return 0;
  const u32 current = m.Read32(manager + 4);
  if (!Pointer(m, current + SCENE_SSS_TASK))
    return 0;
  const u32 task = m.Read32(current + SCENE_SSS_TASK);
  return Pointer(m, task) && m.Valid(task + SSS_CONTROLLER + 3) ? task : 0;
}

int ModeMeleeStage(const GuestMemory& m)
{
  if (!Pointer(m, GAME_GLOBAL))
    return -1;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + GLOBAL_MODE_MELEE))
    return -1;
  const u32 mm = m.Read32(global + GLOBAL_MODE_MELEE);
  if (!Pointer(m, mm) || !m.Valid(mm + MODE_MELEE_STAGE))
    return -1;
  return m.Read8(mm + MODE_MELEE_STAGE);
}

bool IsRankedPPlus(const MatchBlock::State& s)
{
  return s.mode == MatchBlock::MODE_RANKED && s.ruleset == Ranked::RULESET_PPLUS_2024;
}

bool IsCasualPPlus(const MatchBlock::State& s)
{
  return s.mode == MatchBlock::MODE_CASUAL && s.ruleset == Ranked::RULESET_PPLUS_2024;
}

Ranked::SetView ViewOf(const MatchBlock::State& s)
{
  Ranked::SetView v;
  v.game = s.games + 1;
  v.last_winner = s.last_winner <= 1 ? s.last_winner : -1;
  v.coin = s.coin;
  v.won_on = s.won_on;
  v.characters = {s.characters[0] == MatchBlock::NONE ? -1 : s.characters[0],
                  s.characters[1] == MatchBlock::NONE ? -1 : s.characters[1]};
  return v;
}

// A casual game's plan, seeded from the room hash, the header's coin and the game number.
Plan CasualPlan(const GuestMemory& m, const MatchBlock::State& s)
{
  const u32 room = m.Valid(MatchBlock::ROOM + 3) ? m.Read32(MatchBlock::ROOM) : 0;
  return Ranked::PlanCasual(Rulesets::PPlus2024(), ViewOf(s),
                            Ranked::CasualSeed(room, s.coin, s.games + 1));
}

Plan PlanFor(const GuestMemory& m, const MatchBlock::State& s)
{
  return IsCasualPPlus(s) ? CasualPlan(m, s) : Ranked::PlanGame(Rulesets::PPlus2024(), ViewOf(s));
}

// Moves the stage select's own cursor; the hover follows next frame. Returns bytes changed.
int PutCursor(GuestMemory& m, u32 task, float x, float y)
{
  if (!Pointer(m, task + SSS_CURSOR))
    return 0;
  const u32 cursor = m.Read32(task + SSS_CURSOR);
  if (!Pointer(m, cursor) || !m.Valid(cursor + SSS_CURSOR_Y + 3))
    return 0;
  int changed = 0;
  for (const auto& [at, v] : {std::pair{cursor + SSS_CURSOR_X, std::bit_cast<u32>(x)},
                              std::pair{cursor + SSS_CURSOR_Y, std::bit_cast<u32>(y)}})
  {
    if (m.Read32(at) != v)
    {
      m.Write32(at, v);
      changed += 4;
    }
  }
  return changed;
}

// Stages shown struck: not in play, struck or banned so far, and after a pick timed out,
// everything but the default.
StageMask ShownStruck(const Ranked::Ruleset& rules, const Plan& plan, const MatchBlock::State& s)
{
  StageMask shown = static_cast<StageMask>(rules.All() & ~plan.available);
  shown |= s.struck;
  if (s.step < plan.steps.size() && plan.steps[s.step].kind == StepKind::Pick)
    shown |= plan.pick_excluded;
  if (s.auto_pick && s.auto_stage < Ranked::MAX_STAGES)
    shown = static_cast<StageMask>(rules.All() & ~Bit(s.auto_stage));
  return shown;
}

int WriteIfDifferent16(GuestMemory& m, u32 address, u16 value)
{
  if (m.Read16(address) == value)
    return 0;
  m.Write16(address, value);
  return 2;
}

// Writes the strike table: page 0's struck or non-legal positions, and every other page fully
// struck. Returns bytes changed.
int WriteStrikeTable(GuestMemory& m, const std::vector<int>& page0, StageMask shown)
{
  if (!m.Valid(STAGE_STRIKE_TABLE + STRIKE_PAGES * 6 - 1))
    return 0;
  u32 bits = 0;
  for (size_t pos = 0; pos < page0.size(); ++pos)
  {
    const int legal = page0[pos];
    if (legal < 0 || (shown & Bit(legal)))
      bits |= 1u << pos;
  }
  int changed = 0;
  changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + 0, 0);
  changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + 2, static_cast<u16>(bits >> 16));
  changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + 4, static_cast<u16>(bits));
  for (u32 page = 1; page < STRIKE_PAGES; ++page)
  {
    for (u32 half = 0; half < 3; ++half)
      changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + page * 6 + half * 2, 0xFFFF);
  }
  return changed;
}

int PositionOf(const std::vector<int>& page0, int legal)
{
  for (size_t pos = 0; pos < page0.size(); ++pos)
  {
    if (page0[pos] == legal)
      return static_cast<int>(pos);
  }
  return -1;
}

void StartStep(const Ranked::Ruleset& rules, const Plan& plan, MatchBlock::State& s, int frame)
{
  s.step_done = 0;
  s.auto_pick = false;
  s.auto_stage = MatchBlock::NONE;
  s.auto_press = false;
  if (s.step < plan.steps.size())
  {
    s.active = plan.steps[s.step].port;
    s.deadline = static_cast<u32>(frame) + Ranked::StepFrames(rules, plan.steps[s.step]);
  }
  else
  {
    s.active = MatchBlock::NONE;
    s.deadline = 0;
  }
}

// Picks `stage` for `port` by striking every other stage, selecting it and pressing A through
// the input gate.
void AutoPick(MatchBlock::State& s, int stage, int port, u8 why, int frame)
{
  s.auto_pick = true;
  s.auto_stage = static_cast<u8>(stage);
  s.auto_why = why;
  s.active = static_cast<u8>(port & 1);
  s.deadline = static_cast<u32>(frame);
  s.auto_press = false;
}

// A block preference as RankedSteps keeps it: a stage, PREF_RANDOM, or PREF_NONE.
s8 Signed(u8 pref)
{
  if (pref == MatchBlock::RANDOM)
    return Ranked::PREF_RANDOM;
  return pref < Ranked::MAX_STAGES ? static_cast<s8>(pref) : Ranked::PREF_NONE;
}

// Whether the local player's own cursor is on the Random button (UI only).
bool LocalOnRandom(const GuestMemory& m, int me)
{
  if (me < 0 || me > 1)
    return false;
  const std::optional<StageCursors::State> c = StageCursors::Read(m);
  if (!c)
    return false;
  const StageCursors::Cursor& mine = c->cursors[me];
  return StageCursors::Hovered(StageCursors::PPlusLayout(), 0, mine.x, mine.y) ==
         StageCursors::RANDOM_BUTTON;
}

// Ranked presses on each player's own cursor. X strikes on your turn, A picks on your pick or
// otherwise proposes, B withdraws a proposal. Edges are against what the hook last saw, so a
// re-run sees none.
void RankedPresses(const Plan& plan, MatchBlock::State& s, int frame, StageCursors::State& c)
{
  const StageCursors::Layout& layout = StageCursors::PPlusLayout();
  for (int port = 0; port < 2; ++port)
  {
    const u8 pressed = StageCursors::TakePresses(&c, port);
    if (!pressed || s.auto_pick || s.step >= plan.steps.size())
      continue;
    const StageCursors::Cursor& mine = c.cursors[port];
    const int on = StageCursors::Hovered(layout, 0, mine.x, mine.y);
    const Step& now = plan.steps[s.step];
    const bool my_turn = now.port == port;
    const StageMask allowed = Ranked::Allowed(plan, s.step, s.struck);
    if (pressed & StageCursors::BTN_B)
      s.prefs[port] = MatchBlock::NONE;
    if (on < 0)
      continue;
    if (my_turn && now.kind != StepKind::Pick && (pressed & StageCursors::BTN_X) &&
        (allowed & Bit(on)) && Ranked::CountBits(allowed) > 1 && s.step_done < now.count)
    {
      s.struck |= Bit(on);
      ++s.step_done;
      // A proposal on a struck stage is dropped.
      for (u8& pref : s.prefs)
      {
        if (pref == on)
          pref = MatchBlock::NONE;
      }
    }
    else if (pressed & StageCursors::BTN_A)
    {
      const StageMask agreeable = static_cast<StageMask>(plan.agreeable & ~s.struck);
      if (my_turn && now.kind == StepKind::Pick && (allowed & Bit(on)))
      {
        AutoPick(s, on, port, MatchBlock::AUTO_PICKED, frame);
      }
      else if ((agreeable & Bit(on)) && s.prefs[port] != on)
      {
        s.prefs[port] = static_cast<u8>(on);
        // Matching proposals pick the stage and skip the remaining steps.
        if (s.prefs[1 - port] == on)
          AutoPick(s, on, now.port, MatchBlock::AUTO_AGREED, frame);
      }
    }
  }
}

void StageSelect(GuestMemory& m, const Ranked::Ruleset& rules, MatchBlock::State& s, int frame,
                 StageCursors::State& c)
{
  if (s.flow == 0)
  {
    // The steps begin with nothing struck or proposed and both cursors at their start.
    s.flow = 1;
    s.step = 0;
    s.struck = 0;
    s.prefs = {MatchBlock::NONE, MatchBlock::NONE};
    const Plan plan = Ranked::PlanGame(rules, ViewOf(s));
    StartStep(rules, plan, s, frame);
    c = StageCursors::Start(StageCursors::PPlusLayout());
  }
  const Plan plan = Ranked::PlanGame(rules, ViewOf(s));
  if (plan.steps.empty())
    return;
  if (s.step >= plan.steps.size())
  {
    s.step = static_cast<u8>(plan.steps.size() - 1);
    StartStep(rules, plan, s, frame);
  }
  RankedPresses(plan, s, frame, c);
  if (s.auto_pick)
    return;
  // Clamp a deadline from another frame numbering to at most one full step.
  const Step& now = plan.steps[s.step];
  const u32 full = Ranked::StepFrames(rules, now);
  if (s.deadline > static_cast<u32>(frame) + full)
    s.deadline = static_cast<u32>(frame) + full;
  const bool expired = static_cast<u32>(frame) >= s.deadline;

  if (now.kind != StepKind::Pick)
  {
    bool advance = s.step_done >= now.count ||
                   Ranked::CountBits(Ranked::Allowed(plan, s.step, s.struck)) <= 1;
    if (!advance && expired)
    {
      const StageMask defaults = Ranked::DefaultStrikes(plan, s.step, s.struck, s.step_done);
      s.struck |= defaults;
      for (u8& pref : s.prefs)
      {
        if (pref < Ranked::MAX_STAGES && (defaults & Bit(pref)))
          pref = MatchBlock::NONE;
      }
      advance = true;
    }
    if (advance)
    {
      ++s.step;
      StartStep(rules, plan, s, frame);
    }
  }
  else if (expired)
  {
    const int pick = Ranked::DefaultPick(plan, s.step, s.struck);
    if (pick >= 0)
    {
      AutoPick(s, pick, now.port, MatchBlock::AUTO_TIMER, frame);
      s.deadline = static_cast<u32>(frame);
    }
  }
  else if (Ranked::CountBits(Ranked::Allowed(plan, s.step, s.struck)) == 1)
  {
    AutoPick(s, Ranked::DefaultPick(plan, s.step, s.struck), now.port, MatchBlock::AUTO_PICKED,
             frame);
  }
}

// Clears the step state when leaving the stage select or starting a fight.
void ResetSteps(MatchBlock::State& s)
{
  s.flow = 0;
  s.step = 0;
  s.struck = 0;
  s.step_done = 0;
  s.active = MatchBlock::NONE;
  s.auto_pick = false;
  s.auto_stage = MatchBlock::NONE;
  s.auto_press = false;
  s.auto_why = MatchBlock::AUTO_TIMER;
  s.prefs = {MatchBlock::NONE, MatchBlock::NONE};
}

// A casual stage select. Both players name a stage, or Random, at once with A (B takes it back),
// and the flow picks the result. Random, and a player who named none when the timer runs out, get
// a random legal stage (Ranked::CasualResult). The block keeps Random as RANDOM, not its stage.
void CasualSelect(GuestMemory& m, const Ranked::Ruleset& rules, MatchBlock::State& s, int frame,
                  StageCursors::State& c)
{
  if (s.flow == 0)
  {
    // Nothing named before the pick begins counts.
    ResetSteps(s);
    s.flow = 1;
    StartStep(rules, CasualPlan(m, s), s, frame);
    c = StageCursors::Start(StageCursors::PPlusLayout());
    return;
  }
  const Plan plan = CasualPlan(m, s);
  if (s.step >= plan.steps.size() || s.auto_pick)
    return;
  const Step& now = plan.steps[s.step];
  const u32 full = Ranked::StepFrames(rules, now);
  if (s.deadline > static_cast<u32>(frame) + full)
    s.deadline = static_cast<u32>(frame) + full;
  const bool expired = static_cast<u32>(frame) >= s.deadline;
  const StageMask allowed = Ranked::Allowed(plan, s.step, 0);
  const StageCursors::Layout& layout = StageCursors::PPlusLayout();
  for (int port = 0; port < 2; ++port)
  {
    const u8 pressed = StageCursors::TakePresses(&c, port);
    const StageCursors::Cursor& mine = c.cursors[port];
    const int on = StageCursors::Hovered(layout, 0, mine.x, mine.y);
    if (pressed & StageCursors::BTN_B)
      s.prefs[port] = MatchBlock::NONE;
    if ((pressed & StageCursors::BTN_A) && on >= 0 && (allowed & Bit(on)))
      s.prefs[port] = static_cast<u8>(on);
    else if ((pressed & StageCursors::BTN_A) && on == StageCursors::RANDOM_BUTTON && allowed)
      s.prefs[port] = MatchBlock::RANDOM;
  }
  const std::array<s8, 2> prefs{Signed(s.prefs[0]), Signed(s.prefs[1])};
  const bool both = Ranked::Named(prefs[0]) && Ranked::Named(prefs[1]);
  if (!both && !expired)
    return;
  // Both named, or time is up: the flow picks the result for that player. Whoever named nothing
  // gets their random stage; a Random stays RANDOM.
  const int chosen = Ranked::CasualResult(plan, prefs);
  for (int port = 0; port < 2; ++port)
  {
    if (!Ranked::Named(prefs[port]))
      s.prefs[port] = static_cast<u8>(Ranked::CasualDefault(plan, port));
  }
  const int port = s.prefs[0] == s.prefs[1] ? 0 : Ranked::CasualChoice(plan);
  ++s.step;
  AutoPick(s, chosen >= 0 ? chosen : 0, port, MatchBlock::AUTO_CASUAL, frame);
}

// One frame of a casual Project+ game. `task` is 0 outside the stage select.
void CasualStep(GuestMemory& m, MatchBlock::State& s, int frame, bool two_players,
                const std::string& scene, u32 task, StageCursors::State& c)
{
  if (scene == "scMelee" && s.flow == 1)
  {
    // A fight started from the flow's pick, so the next game gets a new coin.
    ResetSteps(s);
    s.games = static_cast<u8>(std::min<int>(s.games + 1, 0xFF));
  }
  if (scene == "scSelctCharacter" && s.flow != 0)
    ResetSteps(s);
  if (task && two_players)
    CasualSelect(m, Rulesets::PPlus2024(), s, frame, c);
  else if (task && s.flow == 1)
    ResetSteps(s);
}

// One frame of a ranked Project+ set. `task` is 0 outside the stage select.
void Step1(GuestMemory& m, MatchBlock::State& s, int frame, bool two_players,
           const std::string& scene, u32 task, StageCursors::State& c)
{
  const Ranked::Ruleset& rules = Rulesets::PPlus2024();
  if (scene == "scVsResult" && s.fight)
  {
    const Reading r = ReadResults(m);
    const GameResult g = Judge(r.block, frame);
    if (g.kind == GameResult::Kind::Win && !s.done)
    {
      const int w = g.winner_port;
      const int stage = s.stage < Ranked::MAX_STAGES ? s.stage : LegalIndexOfKind(g.stage);
      if (s.games < MatchBlock::MAX_GAMES)
      {
        MatchBlock::GameRecord& rec = s.records[s.games];
        rec.winner = static_cast<u8>(w);
        rec.stage = stage >= 0 ? static_cast<u8>(stage) : MatchBlock::NONE;
        for (int p = 0; p < 2; ++p)
          rec.characters[p] =
              g.characters[p] >= 0 ? static_cast<u8>(g.characters[p]) : MatchBlock::NONE;
      }
      for (int p = 0; p < 2; ++p)
        s.characters[p] =
            g.characters[p] >= 0 ? static_cast<u8>(g.characters[p]) : MatchBlock::NONE;
      s.games = static_cast<u8>(std::min<int>(s.games + 1, 0xFF));
      s.score[w] = static_cast<u8>(s.score[w] + 1);
      s.last_winner = static_cast<u8>(w);
      if (stage >= 0)
        s.won_on[w] |= Bit(stage);
      if (s.score[w] >= rules.wins_needed)
      {
        s.done = true;
        s.set_winner = static_cast<u8>(w);
      }
    }
    s.fight = false;
    s.stage = MatchBlock::NONE;
  }
  if (scene == "scMelee" && s.flow == 1)
  {
    const int legal = LegalIndexOfKind(ModeMeleeStage(m));
    ResetSteps(s);
    s.fight = true;
    s.stage = legal >= 0 ? static_cast<u8>(legal) : MatchBlock::NONE;
  }
  if (scene == "scSelctCharacter")
  {
    // Back on character select. An unfinished flow restarts next time, and a fight without a
    // results screen doesn't count.
    if (s.flow != 0 || s.fight)
    {
      ResetSteps(s);
      s.fight = false;
      s.stage = MatchBlock::NONE;
    }
  }
  if (task && !s.done && two_players)
  {
    StageSelect(m, rules, s, frame, c);
  }
  else if (task && s.flow == 1)
  {
    // The opponent left or the set ended. The stage select is free again, and the steps restart
    // if the opponent returns.
    ResetSteps(s);
  }
}
}  // namespace

int LegalIndexOfKind(int kind)
{
  // The 2024 Proposed stages' Project+ stage kinds, in the ruleset's order.
  const Ranked::Ruleset& rules = Rulesets::PPlus2024();
  for (int i = 0; i < rules.stage_count; ++i)
  {
    if (rules.stage_kinds[static_cast<size_t>(i)] == kind)
      return i;
  }
  return -1;
}

std::vector<int> PageZero(const GuestMemory& m)
{
  std::vector<int> page;
  if (!m.Valid(RSS_EXDATA) || !m.Valid(RSS_EXDATA + RSS_EXDATA_SIZE - 1))
    return page;
  const int count = std::min<int>(m.Read8(RSS_PAGE0_COUNT), MAX_PAGE_STAGES);
  for (int pos = 0; pos < count; ++pos)
  {
    const u8 slot = m.Read8(RSS_PAGE0_SLOTS + pos);
    const u32 kind_at = RSS_SLOT_KINDS + 2u * slot;
    page.push_back(m.Valid(kind_at) ? LegalIndexOfKind(m.Read8(kind_at)) : -1);
  }
  return page;
}

int ClearStaleMusicSelect(GuestMemory& m)
{
  // My Music's round trip goes from the stage select to My Music and straight back, never through
  // the character select, so a step seen here is left over.
  if (ReadSceneName(m) != "scSelctCharacter")
    return 0;
  int changed = 0;
  for (const u32 offset : MUSIC_SELECT_WORDS)
  {
    const u32 at = MUSIC_SELECT + offset;
    if (m.Valid(at) && m.Valid(at + 3) && m.Read32(at) != 0)
    {
      m.Write32(at, 0);
      changed += 4;
    }
  }
  return changed;
}

int Apply(GuestMemory& m, int frame, bool two_players)
{
  const std::optional<MatchBlock::State> read = MatchBlock::Read(m);
  if (!read || (!IsRankedPPlus(*read) && !IsCasualPPlus(*read)))
    return 0;
  MatchBlock::State s = *read;
  const bool casual = IsCasualPPlus(s);
  const std::string scene = ReadSceneName(m);
  const u32 task = StageSelectTask(m, scene);
  int changed = 0;
  // Outside the stage select, load the 2024 Proposed preset, which the stage select builds its
  // pages from. Only the first PRESET_BYTES differ between shipped presets.
  if (!casual && scene != "scSelStage" && m.Valid(RSS_EXDATA) &&
      m.Valid(RSS_EXDATA + PRESET_BYTES - 1))
  {
    for (u32 i = 0; i < PRESET_BYTES; ++i)
    {
      if (m.Read8(RSS_EXDATA + i) != kSwitch03[i])
      {
        m.Write8(RSS_EXDATA + i, kSwitch03[i]);
        ++changed;
      }
    }
  }
  const std::optional<StageCursors::State> had = StageCursors::Read(m);
  StageCursors::State c = had.value_or(StageCursors::Start(StageCursors::PPlusLayout()));
  if (casual)
    CasualStep(m, s, frame, two_players, scene, task, c);
  else
    Step1(m, s, frame, two_players, scene, task, c);
  // The flow stopped on the stage select, so its cursor is anyone's again.
  if (task && read->flow == 1 && s.flow == 0 && m.Read32(task + SSS_CONTROLLER) != 0xF0)
  {
    m.Write32(task + SSS_CONTROLLER, 0xF0);
    changed += 4;
  }
  if (task && s.flow == 1 && !s.done && two_players)
  {
    const Ranked::Ruleset& rules = Rulesets::PPlus2024();
    const Plan plan = PlanFor(m, s);
    const std::vector<int> page0 = PageZero(m);
    const int table = WriteStrikeTable(m, page0, ShownStruck(rules, plan, s));
    if (table > 0 && m.Valid(PAGE_INDEX))
    {
      // Project+'s buttonProc redraws the struck art when PAGE_INDEX differs from CURRENT_PAGE.
      if (m.Read8(PAGE_INDEX) != 0xFF)
      {
        m.Write8(PAGE_INDEX, 0xFF);
        ++changed;
      }
    }
    changed += table;
    // The game's cursor sits on the picked stage, or follows a player's cursor so the preview
    // shows. Its port decides whose A the gate presses.
    const bool picking = s.auto_pick && s.active <= 1;
    const u32 controller = picking ? s.active : (c.follower & 1);
    if (m.Read32(task + SSS_CONTROLLER) != controller)
    {
      m.Write32(task + SSS_CONTROLLER, controller);
      changed += 4;
    }
    const StageCursors::Layout& layout = StageCursors::PPlusLayout();
    const StageCursors::Tile* tile =
        picking ? StageCursors::TileOf(layout, s.auto_stage, 0) : nullptr;
    const StageCursors::Cursor& f = c.cursors[c.follower & 1];
    changed += PutCursor(m, task,
                         tile ? StageCursors::MidX(tile->rect) : static_cast<float>(f.x) / 16.0f,
                         tile ? StageCursors::MidY(tile->rect) : static_cast<float>(f.y) / 16.0f);
    if (s.auto_pick)
    {
      // A held 4 frames, released 4. The game needs a fresh press, and its struck-art redraw
      // clears the selection once.
      s.auto_press = ((static_cast<u32>(frame) - s.deadline) >> 2 & 1) == 0;
      const int pos = PositionOf(page0, s.auto_stage);
      if (pos >= 0 && m.Read32(task + SSS_SELECTED) != static_cast<u32>(pos))
      {
        m.Write32(task + SSS_SELECTED, static_cast<u32>(pos));
        changed += 4;
      }
    }
    changed += StageCursors::Write(m, c);
  }
  else
  {
    changed += StageCursors::Clear(m);
  }
  changed += MatchBlock::Write(m, s);
  return changed;
}

Rollback::InputGate::Masks Masks(const GuestMemory& m)
{
  Rollback::InputGate::Masks masks{};
  const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
  if (!s || (!IsRankedPPlus(*s) && !IsCasualPPlus(*s)) || s->done || s->flow != 1)
    return masks;
  const u32 task = StageSelectTask(m, ReadSceneName(m));
  if (!task || !StageCursors::Read(m))
    return masks;
  // Players use their own cursors, so the game's cursor and buttons are masked for everyone. Only
  // the flow drives them, pressing A for the cursor's port when it picks.
  for (Mask& mask : masks)
    mask = Rollback::InputGate::ALL;
  // A only once the game's hover has left its Random and page buttons. Apply moves the game's
  // cursor onto the picked tile, but the hover follows a frame later; an A while it still reads
  // Random would take Project+'s own random stage instead of the flow's.
  const u32 hovered = m.Read32(task + SSS_HOVERED);
  const bool on_button = hovered == RANDOM_ITEM || hovered == PAGE_ITEM;
  if (s->auto_pick && s->active <= 1 && !on_button)
    masks[s->active].press = s->auto_press ? PAD_BUTTON_A : 0;
  return masks;
}

std::optional<Ranked::Turn> CurrentTurn(const GuestMemory& m)
{
  // Describe's turn, without names or clock.
  const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
  if (!s || (!IsRankedPPlus(*s) && !IsCasualPPlus(*s)) || s->flow != 1 || s->auto_pick ||
      (!IsCasualPPlus(*s) && s->done) || ReadSceneName(m) != "scSelStage")
  {
    return std::nullopt;
  }
  const Plan plan = PlanFor(m, *s);
  if (s->step >= plan.steps.size())
    return std::nullopt;
  const Step& step = plan.steps[s->step];
  return Ranked::Turn{step.kind, step.port, std::max(1, step.count - s->step_done)};
}

Lines Describe(const GuestMemory& m, int frame, const std::vector<Events::PortInfo>& ports)
{
  Lines lines;
  const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
  if (!s || (!IsRankedPPlus(*s) && !IsCasualPPlus(*s)))
    return lines;
  const std::string scene = ReadSceneName(m);
  if (scene == "scMelee")
    return lines;
  const auto name = [&](int port) {
    for (const Events::PortInfo& p : ports)
    {
      if (p.port == port && !p.name.empty())
        return p.name;
    }
    return fmt::format("P{}", port + 1);
  };
  const auto local = [&](int port) {
    for (const Events::PortInfo& p : ports)
    {
      if (p.port == port)
        return !p.remote;
    }
    return false;
  };
  const Ranked::Ruleset& rules = Rulesets::PPlus2024();
  const auto clock = [&](const Step& step) {
    const int left = std::max<int>(
        0, static_cast<int>(s->deadline) - Ranked::GraceFrames(rules, step) - frame);
    const int seconds = (left + 59) / 60;
    return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
  };
  // The local player's port (-1 when watching) and the opponent's proposal.
  int me = -1;
  for (const Events::PortInfo& p : ports)
  {
    if (!p.remote && (p.port == 0 || p.port == 1))
      me = p.port;
  }
  const auto join = [](std::string a, const std::string& b) {
    if (a.empty())
      return b;
    if (!b.empty())
      a += " · " + b;
    return a;
  };
  // A preference as the overlay names it: a stage, "Random", or empty for none.
  const auto pref_name = [&](u8 pref) -> std::string {
    if (pref == MatchBlock::RANDOM)
      return "Random";
    return pref < rules.stage_count ? std::string(rules.stage_names[pref]) : std::string();
  };
  const auto proposal_note = [&]() {
    std::string out;
    for (int port = 0; port < 2; ++port)
    {
      const std::string wanted = pref_name(s->prefs[port]);
      if (port == me || wanted.empty())
        continue;
      out = fmt::format("{} wants {}", name(port), wanted);
      if (me >= 0 && s->prefs[me] != s->prefs[port])
        out += " · A on it to agree";
    }
    return out;
  };
  if (IsCasualPPlus(*s))
  {
    // Casual games only show the pick and its result.
    if (scene != "scSelStage" || s->flow != 1)
      return lines;
    if (s->auto_pick && s->auto_stage < Ranked::MAX_STAGES && s->active <= 1)
    {
      // The flow picks for the coin's port (port 1 when both named the same), so a Random there
      // says so, never what the other port's Random would have drawn.
      std::string whose = local(s->active) ? std::string("your pick") :
                                             fmt::format("{}'s pick", name(s->active));
      if (s->prefs[s->active] == MatchBlock::RANDOM)
        whose = "random";
      else if (s->prefs[0] == s->prefs[1])
        whose = "both picks";
      lines.set = fmt::format("{} ({})", rules.stage_names[s->auto_stage], whose);
      return lines;
    }
    const Plan plan = CasualPlan(m, *s);
    if (s->step >= plan.steps.size())
      return lines;
    lines.set = fmt::format("Stage pick · {}", clock(plan.steps[s->step]));
    if (me >= 0)
    {
      const std::string mine = pref_name(s->prefs[me]);
      lines.turn = !mine.empty() ?
                       fmt::format("You want {}", mine) :
                       std::string("A on a stage or Random · the same pick plays there, else a "
                                   "coin takes one of the two");
    }
    lines.turn = join(lines.turn, proposal_note());
    return lines;
  }
  if (s->done && s->set_winner <= 1)
  {
    lines.set = fmt::format("Ranked · {} won the set {}–{}", name(s->set_winner),
                            std::max(s->score[0], s->score[1]), std::min(s->score[0], s->score[1]));
    return lines;
  }
  // No "Game N"; the dots show it.
  const std::string set =
      fmt::format("Ranked · {} {}–{} {}", name(0), s->score[0], s->score[1], name(1));
  lines.set = set;
  if (scene != "scSelStage" || s->flow != 1)
    return lines;
  const Plan plan = Ranked::PlanGame(rules, ViewOf(*s));
  if (s->step >= plan.steps.size())
    return lines;
  const Step& step = plan.steps[s->step];
  if (s->auto_pick && s->auto_stage < Ranked::MAX_STAGES)
  {
    const char* why = s->auto_why == MatchBlock::AUTO_AGREED ? "Agreed: " :
                      s->auto_why == MatchBlock::AUTO_PICKED ? "Stage: " :
                                                               "Time's up · ";
    lines.set = fmt::format("{} · {}{}", set, why, rules.stage_names[s->auto_stage]);
    return lines;
  }
  // Top line: the set, whose turn, strikes or bans left, and the time.
  const int owed = std::max(1, step.count - s->step_done);
  const int turn_port = step.port <= 1 ? step.port : 0;
  std::string turn, keys;
  switch (step.kind)
  {
  case StepKind::Strike:
    turn = fmt::format("{} strikes {}", name(turn_port), owed);
    keys = "X on a stage strikes it";
    break;
  case StepKind::Ban:
    turn = fmt::format("{} bans {}", name(turn_port), owed);
    keys = "X on a stage bans it";
    break;
  case StepKind::Pick:
  case StepKind::Prefer:
    turn = fmt::format("{} picks", name(turn_port));
    keys = "A on a stage picks it";
    break;
  }
  lines.set = fmt::format("{} · {} · {}", set, turn, clock(step));
  // Second line: the turn's keys, then the opponent's proposal or a hint that A proposes. On the
  // Random button, that Random is casual only.
  if (local(turn_port))
    lines.turn = fmt::format("Your turn: {}", keys);
  const std::string theirs = proposal_note();
  if (!theirs.empty())
    lines.turn = join(lines.turn, theirs);
  else if (me >= 0 && !(me == turn_port && step.kind == StepKind::Pick))
    lines.turn = join(lines.turn, "A proposes a stage");
  if (LocalOnRandom(m, me))
    lines.turn = join(lines.turn, "Random is casual only");
  return lines;
}

StageCursors::View CursorView(const GuestMemory& m, const std::vector<Events::PortInfo>& ports,
                              int frame)
{
  StageCursors::View view;
  const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
  const std::optional<StageCursors::State> c = StageCursors::Read(m);
  if (!s || !c || (!IsRankedPPlus(*s) && !IsCasualPPlus(*s)) || s->flow != 1 ||
      !StageSelectTask(m, ReadSceneName(m)))
  {
    return view;
  }
  view.on = true;
  view.game = StageCursors::GAME_PPLUS;
  view.page = 0;
  view.picked = s->auto_pick && s->auto_stage < Ranked::MAX_STAGES ?
                    static_cast<s8>(s->auto_stage) :
                    s8(-1);
  const Plan plan = PlanFor(m, *s);
  if (s->step < plan.steps.size())
  {
    const Step& step = plan.steps[s->step];
    if (step.port <= 1)
      view.turn = static_cast<s8>(step.port);
    // Turn cues for the overlay, with seconds matching Describe's clock.
    view.kind = static_cast<u8>(step.kind);
    view.left = static_cast<u8>(std::max(1, step.count - s->step_done));
    view.optional = step.optional;
    view.both = step.port == Ranked::BOTH;
    const int left = std::max<int>(0, static_cast<int>(s->deadline) -
                                          Ranked::GraceFrames(Rulesets::PPlus2024(), step) - frame);
    view.seconds = static_cast<s16>(std::min(9999, (left + 59) / 60));
  }
  for (int port = 0; port < 2; ++port)
  {
    StageCursors::View::Player& p = view.players[port];
    p.x = c->cursors[port].x;
    p.y = c->cursors[port].y;
    p.proposal = Signed(s->prefs[port]);
    p.name = fmt::format("P{}", port + 1);
    for (const Events::PortInfo& info : ports)
    {
      if (info.port != port)
        continue;
      p.present = true;
      p.local = !info.remote;
      if (!info.name.empty())
        p.name = info.name;
    }
  }
  return view;
}

namespace
{
std::mutex s_lines_mutex;
Lines s_lines;
}  // namespace

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool /*alone*/)
{
  if (!ResultsVerified() || Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory memory(guard);
  bool p1 = false, p2 = false;
  for (const Events::PortInfo& p : ports)
  {
    p1 = p1 || p.port == 0;
    p2 = p2 || p.port == 1;
  }
  const std::optional<MatchBlock::State> before =
      resimulating ? std::nullopt : MatchBlock::Read(memory);
  // Every Project+ session, not only the flows: a With Friends stage select blanks the same way.
  if (Rules::ProfileRuleset() == Rules::Ruleset::PPlus)
  {
    const bool seen = memory.Valid(MUSIC_SELECT + 0x0B);
    const u32 step = seen ? memory.Read32(MUSIC_SELECT) : 0;
    const u32 song = seen ? memory.Read32(MUSIC_SELECT + 0x08) : 0;
    if (ClearStaleMusicSelect(memory) && !resimulating)
      NOTICE_LOG_FMT(ROLLBACK, "Project+: frame {}: cleared a leftover My Music step {} song {:x}",
                     frame, step, song);
  }
  Apply(memory, frame, p1 && p2);
  if (!resimulating)
  {
    // Log the flow's changes on first runs only.
    const std::optional<MatchBlock::State> after = MatchBlock::Read(memory);
    if (after && (IsRankedPPlus(*after) || IsCasualPPlus(*after)) &&
        (!before || *before != *after))
    {
      const MatchBlock::State& a = *after;
      const MatchBlock::State b = before.value_or(MatchBlock::State{});
      if (a.flow != b.flow || a.step != b.step || a.struck != b.struck ||
          a.auto_pick != b.auto_pick || a.games != b.games || a.fight != b.fight ||
          a.done != b.done || a.prefs != b.prefs)
      {
        NOTICE_LOG_FMT(
            ROLLBACK,
            "{}: frame {}: game {} score {}-{} flow {} step {} port {} struck {:03x} "
            "made {} auto {} fight {} stage {} won {:03x}/{:03x} prefs {}/{}{}",
            IsCasualPPlus(a) ? "Casual" : "Ranked", frame, a.games + 1, a.score[0], a.score[1], a.flow, a.step,
            a.active <= 1 ? a.active + 1 : 0, a.struck, a.step_done,
            a.auto_pick ? static_cast<int>(a.auto_stage) : -1, a.fight ? 1 : 0,
            a.stage == MatchBlock::NONE ? -1 : a.stage, a.won_on[0], a.won_on[1],
            static_cast<int>(Signed(a.prefs[0])), static_cast<int>(Signed(a.prefs[1])),
            a.done ? fmt::format(" SET port {}", a.set_winner + 1) : std::string());
      }
    }
    Lines lines = Describe(memory, frame, ports);
    std::lock_guard lock(s_lines_mutex);
    s_lines = std::move(lines);
  }
}

Lines CurrentLines()
{
  std::lock_guard lock(s_lines_mutex);
  return s_lines;
}
}  // namespace Orca::UX::RankedPPlus
