// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Policy/CharOrder.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "Core/Orca/Policy/Queue.h"
#include "Core/Orca/Policy/CssRules.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX::CharOrder
{
namespace
{
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;
using Rollback::InputGate::Masks;

// Whether the upcoming game uses a pick order.
bool SetApplies(const SetView& set)
{
  return set.ruleset != Ruleset::None && set.game >= 2 && set.game <= 0xFF &&
         (set.last_winner == 0 || set.last_winner == 1);
}

}  // namespace

// Shared by the step machine and the gate, so the gate never acts on a state the machine would
// clear. Once begun, the order survives a port briefly not being human, so it never restarts.
bool Active(const SetView& set, const CssView& css, const State& state)
{
  return css.on_css && SetApplies(set) && state.step != Step::Idle && state.game == set.game &&
         state.first == FirstPicker(set);
}

namespace
{
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

}  // namespace

// The current turn's time is up.
bool TimedOut(const SetView& set, const State& state)
{
  return state.elapsed >= set.pick_frames;
}

namespace
{
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
  const bool timed_out = elapsed >= set.pick_frames;
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
    masks[first] = TimedOut(set, state) && !css.ports[first].Down() ? done(first) : picking(first);
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
  if (!Active(set, css, state) || state.step != Step::Done || !TimedOut(set, state))
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
}  // namespace Orca::UX::CharOrder
