// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/CharOrder.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <cstdlib>
#include <string_view>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX::CharOrder
{
namespace
{
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;
using Rollback::InputGate::Masks;

// Character select memory (see CharOrder.h).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x04;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_KIND = 0x1B4;
constexpr u32 AREA_CHARACTER = 0x1B8;
// u8: 1 while the token is in hand, 0 once placed (the next byte is 1 while it is flying).
constexpr u32 AREA_IN_HAND = 0x1F8;
constexpr u32 AREA_FLYING = 0x1F9;
constexpr u32 KIND_HUMAN = 1;

// State offsets at kArea.
constexpr u32 AT_MAGIC = 0x0;
constexpr u32 AT_GAME = 0x4;
constexpr u32 AT_STEP = 0x5;
constexpr u32 AT_FIRST = 0x6;
constexpr u32 AT_LOCKED = 0x7;
constexpr u32 AT_SINCE = 0x8;
constexpr u32 AT_ELAPSED = 0xC;
constexpr u32 AT_HELD = 0xE;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

std::string Clock(int frames)
{
  const int seconds = (std::max(frames, 0) + 59) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

// ORCA_TEST_CHAR_ORDER's set view, parsed once.
const SetView& TestSet()
{
  static const SetView set = [] {
    SetView out;
    const std::string spec = TestCharOrderSpec();
    if (!spec.empty() && !ParseTestSpec(spec, &out))
    {
      ERROR_LOG_FMT(ROLLBACK,
                    "Character order: ORCA_TEST_CHAR_ORDER={} is not "
                    "<pplus|brawl>:<game>:<winner port 1-2>[:free|winnerfirst]; ignored",
                    spec);
      out = {};
    }
    else if (!spec.empty())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Character order: test set ORCA_TEST_CHAR_ORDER={}", spec);
    }
    return out;
  }();
  return set;
}

}  // namespace

std::string Line(const SetView& set, const CssView& css, const State& state,
                 const std::vector<Events::PortInfo>& ports)
{
  if (!Active(set, css, state))
    return {};
  // Port names (default "P<n>") and the local port (-1 in a harness run).
  std::array<std::string, 2> names{"P1", "P2"};
  int local = -1;
  for (const Events::PortInfo& p : ports)
  {
    if (p.port < 0 || p.port > 1)
      continue;
    if (!p.name.empty())
      names[p.port] = p.name;
    if (!p.remote)
      local = p.port;
  }
  if (state.step == Step::Done)
  {
    // Only while a token is still in hand (the game waits for it).
    for (int port = 0; port < 2; ++port)
    {
      if (css.ports[port].placed)
        continue;
      if (port == local)
        return "Put your token on a character";
      return fmt::format("Waiting for {} to put the token down", names[port]);
    }
    return {};
  }
  const std::string left = Clock(set.pick_frames - state.elapsed);
  const int first = state.first & 1;
  const int second = 1 - first;
  // The first pick's time is up with its token in hand: it is going down.
  if (state.step == Step::First && TimedOut(set, state) && !css.ports[first].Down())
  {
    if (first == local)
      return "Time's up: your token goes on a character";
    return fmt::format("Waiting for {} to put the token down", names[first]);
  }
  // The current picker's next action: pick with A, then press Start.
  const auto todo = [&css](int port) {
    return css.ports[port].Down() ? "Start locks it in · B to change" : "A on a character, then Start";
  };
  if (state.step == Step::First && set.order == Order::Free && local == second)
  {
    // Free: the winner, during the loser's turn.
    if (state.locked & (1 << local))
      return fmt::format("You're locked in · Waiting for {} · {}", names[first], left);
    return fmt::format("{} picks first · {} · Start locks yours in any time", names[first], left);
  }
  if (state.step == Step::First)
  {
    if (local == first)
      return fmt::format("Your pick · {} · {}", todo(first), left);
    return fmt::format("{} picks first · {}", names[first], left);
  }
  // Second picker's turn; the first has locked in.
  if (local == second)
    return fmt::format("{} locked in · Your pick: {} · {}", names[first], todo(second), left);
  if (local == first)
    return fmt::format("You're locked in · {} picks · {}", names[second], left);
  return fmt::format("{} locked in · {}: pick and press Start · {}", names[first], names[second],
                     left);
}

static_assert(Block::SET_WINS == SetBlock::SET_WINS && Block::SET_DONE == SetBlock::SET_DONE &&
              Block::SET_LAST_WINNER == SetBlock::SET_LAST_WINNER);
static_assert(kArea == FreeSpace::kMatchBlock.begin + 0x200);

namespace
{
// A set's turn time, from the ruleset its block names (a set names a builtin; none only in a
// build whose ruleset files don't load, OrcaRulesets.BuiltinsAreTheOldTables: no set then).
SetView WithTimer(SetView set)
{
  const Ranked::Ruleset* rules = Rulesets::ByBlock(static_cast<u8>(set.ruleset));
  if (!rules)
    return {};
  set.pick_frames = static_cast<u16>(rules->order_pick_seconds * 60);
  return set;
}
}  // namespace

SetView ReadSet(const GuestMemory& memory)
{
  // Test override.
  if (TestSet().ruleset != Ruleset::None)
    return WithTimer(TestSet());
  const GuestMemory& m = memory;
  const u32 base = FreeSpace::kMatchBlock.begin;
  if (!m.Valid(base + Block::MAGIC) || !m.Valid(base + Block::SET_LAST_WINNER) ||
      m.Read32(base + Block::MAGIC) != Block::MAGIC_VALUE || m.Read8(base + Block::VERSION) != 1 ||
      m.Read8(base + Block::MODE) != Block::MODE_RANKED || m.Read8(base + Block::SET_DONE) != 0)
  {
    return {};
  }
  SetView set;
  const u8 ruleset = m.Read8(base + Block::RULESET);
  if (ruleset == static_cast<u8>(Ruleset::Brawl))
    set.ruleset = Ruleset::Brawl;
  else if (ruleset == static_cast<u8>(Ruleset::PPlus))
    set.ruleset = Ruleset::PPlus;
  else
    return {};
  set.game = m.Read8(base + Block::SET_WINS) + m.Read8(base + Block::SET_WINS + 1) + 1;
  const u8 last = m.Read8(base + Block::SET_LAST_WINNER);
  set.last_winner = last <= 1 ? last : -1;
  return WithTimer(set);
}

CssView ReadCss(const GuestMemory& m)
{
  CssView css;
  if (ReadSceneName(m) != "scSelctCharacter")
    return css;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return css;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return css;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  if (!Pointer(m, task + TASK_AREAS + 4))
    return css;
  css.on_css = true;
  for (u32 i = 0; i < 2; ++i)
  {
    const u32 area = m.Read32(task + TASK_AREAS + 4 * i);
    CssPort& p = css.ports[i];
    if (Pointer(m, area + AREA_KIND) && Pointer(m, area + AREA_CHARACTER) &&
        m.Valid(area + AREA_FLYING))
    {
      p.human = m.Read32(area + AREA_KIND) == KIND_HUMAN;
      p.character = static_cast<int>(m.Read32(area + AREA_CHARACTER));
      p.placed = m.Read8(area + AREA_IN_HAND) == 0;
      p.flying = m.Read8(area + AREA_FLYING) != 0;
    }
    if (const Rules::CssHand hand = Rules::ReadCssHand(m, static_cast<int>(i)); hand.valid)
    {
      p.hand = true;
      p.hand_x = hand.x;
      p.hand_y = hand.y;
    }
    // Raw buttons from the latch (zero when the latch isn't running).
    const u32 raw = Queue::RAW + 2 * i, prev = Queue::RAW_PREV + 2 * i;
    if (m.Valid(raw + 1) && m.Valid(prev + 1))
    {
      p.raw = m.Read16(raw);
      p.raw_new = static_cast<u16>(p.raw & ~m.Read16(prev));
    }
  }
  return css;
}

State ReadState(const GuestMemory& m)
{
  State s;
  if (!m.Valid(kArea) || !m.Valid(kArea + kAreaSize - 1) || m.Read32(kArea + AT_MAGIC) != kMagic)
    return s;
  const u8 step = m.Read8(kArea + AT_STEP);
  if (step > static_cast<u8>(Step::Done))
    return s;
  s.game = m.Read8(kArea + AT_GAME);
  s.step = static_cast<Step>(step);
  s.first = m.Read8(kArea + AT_FIRST) & 1;
  s.locked = m.Read8(kArea + AT_LOCKED) & 3;
  s.since = m.Read32(kArea + AT_SINCE);
  s.elapsed = m.Read16(kArea + AT_ELAPSED);
  s.held = m.Read8(kArea + AT_HELD) & 0x0F;
  if (m.Valid(kAnchorArea) && m.Valid(kAnchorArea + kAnchorAreaSize - 1))
  {
    s.anchored = m.Read8(kAnchorArea) & 3;
    for (u32 port = 0; port < 2; ++port)
    {
      s.anchor_x[port] = static_cast<s16>(m.Read16(kAnchorArea + 4 + 4 * port));
      s.anchor_y[port] = static_cast<s16>(m.Read16(kAnchorArea + 6 + 4 * port));
    }
  }
  return s;
}

int WriteState(GuestMemory& m, const State& s)
{
  if (!m.Valid(kArea) || !m.Valid(kArea + kAreaSize - 1))
    return 0;
  int changed = 0;
  const auto put8 = [&](u32 at, u8 v) {
    if (m.Read8(kArea + at) != v)
    {
      m.Write8(kArea + at, v);
      ++changed;
    }
  };
  const auto put32 = [&](u32 at, u32 v) {
    for (u32 i = 0; i < 4; ++i)
      put8(at + i, static_cast<u8>(v >> (24 - 8 * i)));
  };
  put32(AT_MAGIC, kMagic);
  put8(AT_GAME, s.game);
  put8(AT_STEP, static_cast<u8>(s.step));
  put8(AT_FIRST, s.first);
  put8(AT_LOCKED, s.locked);
  put32(AT_SINCE, s.since);
  put8(AT_ELAPSED, static_cast<u8>(s.elapsed >> 8));
  put8(AT_ELAPSED + 1, static_cast<u8>(s.elapsed));
  put8(AT_HELD, s.held);
  put8(AT_HELD + 1, 0);
  if (m.Valid(kAnchorArea) && m.Valid(kAnchorArea + kAnchorAreaSize - 1))
  {
    const auto at = [&](u32 offset, u8 v) {
      if (m.Read8(kAnchorArea + offset) != v)
      {
        m.Write8(kAnchorArea + offset, v);
        ++changed;
      }
    };
    at(0, s.anchored);
    for (u32 i = 1; i < 4; ++i)
      at(i, 0);
    for (u32 port = 0; port < 2; ++port)
    {
      const u16 x = static_cast<u16>(s.anchor_x[port]), y = static_cast<u16>(s.anchor_y[port]);
      at(4 + 4 * port, static_cast<u8>(x >> 8));
      at(5 + 4 * port, static_cast<u8>(x));
      at(6 + 4 * port, static_cast<u8>(y >> 8));
      at(7 + 4 * port, static_cast<u8>(y));
    }
  }
  return changed;
}

namespace
{
std::atomic<int> s_picker{-1};  // for CurrentPicker()
std::mutex s_ribbon_lock;
std::string s_ribbon;  // for CurrentPickerRibbon()
}  // namespace

std::string PickerRibbon(int picker, const std::vector<Events::PortInfo>& ports)
{
  if (picker < 0 || picker > 1)
    return "";
  std::string name = fmt::format("P{}", picker + 1);
  for (const Events::PortInfo& p : ports)
  {
    if (p.port != picker)
      continue;
    if (!p.remote)
      return "YOUR PICK";
    if (!p.name.empty())
      name = p.name;
  }
  for (char& c : name)
  {
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  }
  return name + " PICKS";
}

std::string CurrentPickerRibbon()
{
  std::lock_guard lk(s_ribbon_lock);
  return s_ribbon;
}

int Picker(const CssView& css, const State& state)
{
  if (!css.on_css || state.game == 0)
    return -1;
  if (state.step == Step::First)
    return state.first & 1;
  if (state.step == Step::Second)
    return (state.first & 1) ^ 1;
  return -1;
}

int CurrentPicker()
{
  return s_picker.load(std::memory_order_relaxed);
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports)
{
  // No ruleset, no character order: nothing to read or write, and nothing left on screen.
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
  {
    if (!resimulating)
    {
      SetRuleLine({});
      s_picker.store(-1, std::memory_order_relaxed);
      std::lock_guard lk(s_ribbon_lock);
      s_ribbon.clear();
    }
    return;
  }
  GuardMemory m(guard);
  const SetView set = ReadSet(m);
  const CssView css = ReadCss(m);
  const State before = ReadState(m);
  const State after = Advance(set, css, before, frame);
  if (after != before)
    WriteState(m, after);
  // A token still in hand long after both turns: that player is the ranked no-show.
  const int no_show = TestActive() ? -1 : NoShow(set, css, after);
  const bool new_no_show = no_show >= 0 && m.Valid(MatchBlock::CSS_NO_SHOW) &&
                           m.Read8(MatchBlock::CSS_NO_SHOW) == 0;
  if (new_no_show)
    m.Write8(MatchBlock::CSS_NO_SHOW, static_cast<u8>(no_show + 1));
  if (resimulating)
    return;
  if (new_no_show)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Character order: frame {} game {}: port {}'s token is still in hand",
                   frame, after.game, no_show + 1);
  }
  SetRuleLine(Line(set, css, after, ports));
  const int picker = Picker(css, after);
  s_picker.store(picker, std::memory_order_relaxed);
  {
    std::lock_guard lk(s_ribbon_lock);
    s_ribbon = PickerRibbon(picker, ports);
  }
  if (after.step != before.step || after.game != before.game || after.locked != before.locked)
  {
    static constexpr const char* STEPS[] = {"idle", "first", "second", "done"};
    NOTICE_LOG_FMT(ROLLBACK,
                   "Character order: frame {} game {} step {} (port {} first, locked {}), was {} "
                   "after {} frames",
                   frame, after.game, STEPS[static_cast<int>(after.step)], after.first + 1,
                   after.locked, STEPS[static_cast<int>(before.step)], before.elapsed);
  }
}

Masks GateFrame(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const SetView set = ReadSet(m);
  if (set.ruleset == Ruleset::None)
    return {};
  return Gate(set, ReadCss(m), ReadState(m));
}

bool TestActive()
{
  return TestSet().ruleset != Ruleset::None;
}

bool ParseTestSpec(const std::string& spec, SetView* out)
{
  const size_t a = spec.find(':');
  const size_t b = a == std::string::npos ? a : spec.find(':', a + 1);
  if (b == std::string::npos)
    return false;
  const size_t c = spec.find(':', b + 1);
  if (c != std::string::npos && spec.find(':', c + 1) != std::string::npos)
    return false;
  const std::string_view rules = std::string_view(spec).substr(0, a);
  const std::string game = spec.substr(a + 1, b - a - 1);
  const std::string winner = spec.substr(b + 1, c == std::string::npos ? c : c - b - 1);
  const std::string order = c == std::string::npos ? std::string() : spec.substr(c + 1);
  SetView set;
  if (rules == "pplus")
    set.ruleset = Ruleset::PPlus;
  else if (rules == "brawl")
    set.ruleset = Ruleset::Brawl;
  else
    return false;
  if (game.empty() || game.size() > 2 ||
      !std::all_of(game.begin(), game.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return false;
  set.game = std::atoi(game.c_str());
  if (set.game < 1 || winner.size() != 1 || winner[0] < '1' || winner[0] > '2')
    return false;
  set.last_winner = winner[0] - '1';
  if (order == "free")
    set.order = Order::Free;
  else if (order == "winnerfirst")
    set.order = Order::WinnerFirst;
  else if (c != std::string::npos)
    return false;
  *out = set;
  return true;
}
}  // namespace Orca::UX::CharOrder
