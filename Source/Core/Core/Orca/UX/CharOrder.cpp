// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/CharOrder.h"

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
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Queue.h"
#include "Core/Orca/UX/SetBlock.h"
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

// Whether the upcoming game uses a pick order.
bool SetApplies(const SetView& set)
{
  return set.ruleset != Ruleset::None && set.game >= 2 && set.game <= 0xFF &&
         (set.last_winner == 0 || set.last_winner == 1);
}

// Shared by the step machine and the gate, so the gate never acts on a state the machine would
// clear. Once begun, the order survives a port briefly not being human, so it never restarts.
bool Active(const SetView& set, const CssView& css, const State& state)
{
  return css.on_css && SetApplies(set) && state.step != Step::Idle && state.game == set.game &&
         state.first == FirstPicker(set);
}

std::string Clock(int frames)
{
  const int seconds = (std::max(frames, 0) + 59) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

// A hand position in State's anchor units (1/64), from the float's bits (no float arithmetic, so
// every machine agrees). Nullopt off any sane position.
std::optional<s16> ToAnchor(float value)
{
  const std::optional<s64> fixed = Queue::ToFixed(value);
  if (!fixed)
    return std::nullopt;
  const s64 v = *fixed / (Queue::FIXED_ONE / 64);
  if (v < -0x7FFF || v > 0x7FFF)
    return std::nullopt;
  return static_cast<s16>(v);
}

// Where `port`'s token goes back to: its anchor, else the other port's, else the grid's middle.
std::pair<float, float> AnchorFor(const State& state, int port)
{
  for (const int p : {port, 1 - port})
  {
    if (state.anchored & (1 << p))
      return {state.anchor_x[p] / 64.0f, state.anchor_y[p] / 64.0f};
  }
  return {kGridMiddleX, kGridMiddleY};
}

// Where Orca steers `port`'s hand, its token in hand over no character: the anchor, or the grid's
// middle once the hand is within a unit of it (an anchor at a character's edge, the hand stopped
// just outside). Integer comparisons only.
std::pair<float, float> SteerTarget(const State& state, int port, const CssPort& p)
{
  const std::pair<float, float> anchor = AnchorFor(state, port);
  const std::optional<s64> x = Queue::ToFixed(p.hand_x), y = Queue::ToFixed(p.hand_y);
  const std::optional<s64> ax = Queue::ToFixed(anchor.first), ay = Queue::ToFixed(anchor.second);
  if (x && y && ax && ay)
  {
    const s64 dx = *x - *ax, dy = *y - *ay;
    if ((dx < 0 ? -dx : dx) < Queue::FIXED_ONE && (dy < 0 ? -dy : dy) < Queue::FIXED_ONE)
      return {kGridMiddleX, kGridMiddleY};
  }
  return anchor;
}

// The current turn's time is up.
bool TimedOut(const State& state)
{
  return state.elapsed >= PICK_FRAMES;
}

// Port `port`'s L and R from its raw buttons, as State::held keeps them.
u8 HeldBits(int port, u16 raw)
{
  const int bits = ((raw & PAD_TRIGGER_L) ? 1 : 0) | ((raw & PAD_TRIGGER_R) ? 2 : 0);
  return static_cast<u8>(bits << (2 * port));
}

// The L and R kept for `port`, as PAD_* bits.
u16 HeldButtons(const State& state, int port)
{
  const int bits = (state.held >> (2 * port)) & 3;
  return static_cast<u16>(((bits & 1) ? PAD_TRIGGER_L : 0) | ((bits & 2) ? PAD_TRIGGER_R : 0));
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

int FirstPicker(const SetView& set)
{
  if (set.last_winner != 0 && set.last_winner != 1)
    return -1;
  return set.order == Order::WinnerFirst ? set.last_winner : 1 - set.last_winner;
}

State Advance(const SetView& set, const CssView& css, const State& state, int frame)
{
  if (!css.on_css)
    return state;
  if (!SetApplies(set))
    return state == State{} ? state : State{};
  if (!Active(set, css, state))
  {
    // Start the first picker's turn once both players have joined. Presses on this frame don't
    // count, since they were made before the order began.
    if (!css.ports[0].human || !css.ports[1].human)
      return state == State{} ? state : State{};
    return State{static_cast<u8>(set.game), Step::First, static_cast<u8>(FirstPicker(set)), 0,
                 static_cast<u32>(frame), 0};
  }
  State s = state;
  const u32 now = static_cast<u32>(frame);
  const u32 elapsed = now >= s.since ? now - s.since : 0;
  // Past 0xFFFF frames it keeps alternating, so presses made every other frame never stop.
  s.elapsed = static_cast<u16>(elapsed <= 0xFFFF ? elapsed : 0xFFFE | (elapsed & 1));
  // Each hand's last place over a character with its token in hand (State::anchored).
  for (int port = 0; port < 2; ++port)
  {
    const CssPort& p = css.ports[port];
    if (!p.human || p.placed || p.character == kNoCharacter || !p.hand)
      continue;
    const std::optional<s16> x = ToAnchor(p.hand_x), y = ToAnchor(p.hand_y);
    if (!x || !y)
      continue;
    s.anchored = static_cast<u8>(s.anchored | (1 << port));
    s.anchor_x[port] = *x;
    s.anchor_y[port] = *y;
  }
  if (elapsed == 0 || s.step == Step::Done)
    return s;
  // Lock-in is a new raw Start press with the token placed on a character.
  const auto locks_in = [&css](int port) {
    const CssPort& p = css.ports[port];
    return (p.raw_new & PAD_BUTTON_START) != 0 && p.Down();
  };
  const bool timed_out = elapsed >= static_cast<u32>(PICK_FRAMES);
  const int first = s.first & 1;
  const int second = 1 - first;
  // L and R as a turn ends (CharOrder.h): the game reads them as the character select ends.
  const auto keep = [&css, &s](int port) {
    s.held = static_cast<u8>((s.held & ~(3 << (2 * port))) | HeldBits(port, css.ports[port].raw));
  };
  bool next = timed_out;
  if (s.step == Step::First)
  {
    // Free: the winner may lock in during the loser's turn, once: a later Start keeps the L and R
    // it locked in with.
    if (set.order == Order::Free && !(s.locked & (1 << second)) && locks_in(second))
    {
      s.locked = static_cast<u8>(s.locked | (1 << second));
      keep(second);
    }
    // A first pick that timed out with the token in hand ends once Orca has put it down (Gate),
    // so the second picker never picks before the first pick is down.
    next = (next && css.ports[first].Down()) || locks_in(first);
  }
  else
  {
    next = next || locks_in(second);
  }
  if (next)
  {
    keep(s.step == Step::First ? first : second);
    // Skip the second picker's turn if they already locked in.
    s.step = s.step == Step::First && !(s.locked & (1 << second)) ? Step::Second : Step::Done;
    s.since = now;
    s.elapsed = 0;
  }
  return s;
}

Masks Gate(const SetView& set, const CssView& css, const State& state)
{
  Masks masks{};
  if (!Active(set, css, state))
    return masks;
  const int first = state.first & 1;
  const int second = 1 - first;
  const bool even = state.elapsed % 2 == 0;  // synthesized presses repeat every other frame
  // On your turn: A picks, B (first frame, token placed) picks the token back up, and Start locks
  // in without reaching the game. Same B handling as OnlineRules.h, so test runs without a header
  // behave alike.
  const auto picking = [&css](int port) {
    const CssPort& p = css.ports[port];
    Mask mask;
    mask.buttons = PAD_BUTTON_START;
    Rules::CssToken token;
    token.valid = true;
    token.human = p.human;
    token.character = p.character;
    token.in_hand = !p.placed;
    token.flying = p.flying;
    if (!Rules::CssBUnpicks(token, (p.raw & PAD_BUTTON_B) != 0))
      mask.buttons |= PAD_BUTTON_B;
    return mask;
  };
  // After your turn: no input. A token still in hand goes down for you: Orca brings the hand back
  // to its anchor (CharOrder.h) and presses A over a character, the stick its own meanwhile.
  const auto done = [&css, &state, even](int port) {
    const CssPort& p = css.ports[port];
    if (p.placed)
      return ALL;
    Mask mask;
    mask.buttons = Rollback::InputGate::ALL_BUTTONS & ~PAD_BUTTON_A;
    mask.main_stick = true;
    if (p.character != kNoCharacter)
    {
      if (even)
        mask.press = PAD_BUTTON_A;
    }
    else if (p.hand)
    {
      const auto [x, y] = SteerTarget(state, port, p);
      const auto [stick_x, stick_y] = Queue::SteerToward(p.hand_x, p.hand_y, x, y);
      mask.steer = true;
      mask.steer_x = stick_x;
      mask.steer_y = stick_y;
    }
    return mask;
  };
  switch (state.step)
  {
  case Step::First:
    // Time ran out with the token in hand: it goes down before the second picker's turn.
    masks[first] = TimedOut(state) && !css.ports[first].Down() ? done(first) : picking(first);
    // WinnerFirst: the other player waits. Free: the winner may pick and lock in, then waits.
    if (set.order == Order::WinnerFirst || (state.locked & (1 << second)))
      masks[second] = set.order == Order::WinnerFirst ? ALL : done(second);
    else
      masks[second] = picking(second);
    break;
  case Step::Second:
    masks[first] = done(first);
    masks[second] = picking(second);
    break;
  case Step::Done:
    masks[first] = done(first);
    masks[second] = done(second);
    // Each player's L and R as they locked in, held for them while the character select ends.
    masks[first].press |= HeldButtons(state, first);
    masks[second].press |= HeldButtons(state, second);
    // Press Start for the second picker once both tokens are placed. A queue2 room handles this
    // itself (UX.cpp).
    if (even && css.ports[0].placed && css.ports[1].placed)
      masks[second].press |= PAD_BUTTON_START;
    break;
  case Step::Idle:
    break;
  }
  return masks;
}

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
  const std::string left = Clock(PICK_FRAMES - state.elapsed);
  const int first = state.first & 1;
  const int second = 1 - first;
  // The first pick's time is up with its token in hand: it is going down.
  if (state.step == Step::First && TimedOut(state) && !css.ports[first].Down())
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

u8 LockedPorts(const SetView& set, const CssView& css, const State& state)
{
  if (!Active(set, css, state))
    return 0;
  const int first = state.first & 1;
  switch (state.step)
  {
  case Step::First:
    return set.order == Order::Free ? static_cast<u8>(state.locked & (1 << (1 - first))) : 0;
  // Never a token still in hand.
  case Step::Second:
    return css.ports[first].placed ? static_cast<u8>(1 << first) : 0;
  case Step::Done:
    return static_cast<u8>((css.ports[0].placed ? 1 : 0) | (css.ports[1].placed ? 2 : 0));
  case Step::Idle:
    break;
  }
  return 0;
}

int NoShow(const SetView& set, const CssView& css, const State& state)
{
  if (!Active(set, css, state) || state.step != Step::Done || !TimedOut(state))
    return -1;
  const int first = state.first & 1;
  for (const int port : {1 - first, first})
  {
    if (!css.ports[port].placed)
      return port;
  }
  return -1;
}

int MkFreePicker(const std::array<int, 2>& characters, int meta_knight)
{
  const bool a = characters[0] == meta_knight;
  const bool b = characters[1] == meta_knight;
  if (a == b)
    return -1;
  return a ? 1 : 0;
}

static_assert(Block::SET_WINS == SetBlock::SET_WINS && Block::SET_DONE == SetBlock::SET_DONE &&
              Block::SET_LAST_WINNER == SetBlock::SET_LAST_WINNER);
static_assert(kArea == FreeSpace::kMatchBlock.begin + 0x200);

SetView ReadSet(const GuestMemory& memory)
{
  // Test override.
  if (TestSet().ruleset != Ruleset::None)
    return TestSet();
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
  return set;
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
