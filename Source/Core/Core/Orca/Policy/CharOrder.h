// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Core/Rollback/InputGate.h"

// A ranked set's character order on later games, as a step machine (ORCA.md "Character order"):
// who picks first, 45 s turns, Start locks in, a token still in hand put down at its anchor, L and
// R kept through the select's exit, the no-show. Pure functions of what a frame sees (SetView,
// CssView, State) and the frame number. Engine-side competitive policy (Orca split, S2 commit 7,
// Luis's decision 11); the Brawl module (Games/Brawl/CharOrder.h) reads them from memory, calls
// these at the same positions as before and writes the state back.
namespace Orca::UX::CharOrder
{
// The ranked set's ruleset, from the match block header.
enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,  // Supernova 2025
  PPlus = 2,  // Project+ 2024 Recommended
};

// Who picks first on a later game.
enum class Order : u8
{
  Free = 0,         // the loser is asked first; the winner may lock in at any time
  WinnerFirst = 1,  // the winner locks in first, then the loser picks
};
// The order both games use.
constexpr Order kLaterGameOrder = Order::WinnerFirst;

// The upcoming game of an open ranked set, as read by ReadSet(). The game number is one more than
// the games won, since a tied game is replayed.
struct SetView
{
  Ruleset ruleset = Ruleset::None;  // None: no ranked set, nothing here acts
  int game = 0;                     // 1-based
  int last_winner = -1;             // previous game's winning port (0 or 1), from game 2 on
  Order order = kLaterGameOrder;
  // Each turn, in frames: the ruleset's characterOrder.pickSeconds x 60 (45 s in both builtins), set
  // by ReadSet with the set.
  u16 pick_frames = 0;
  bool operator==(const SetView&) const = default;
};
// The winner (WinnerFirst) or the loser (Free). -1 without a previous winner.
int FirstPicker(const SetView& set);

// One port's character select state.
constexpr int kNoCharacter = 0x28;

struct CssPort
{
  bool human = false;
  int character = kNoCharacter;  // under the hand while holding the token, else the token's
  bool placed = false;  // token is out of the hand (placed, or falling)
  bool flying = false;  // token is moving up or down
  // Raw buttons (PAD_* bits) from the input latch (Queue.h RAW): held and newly pressed.
  u16 raw = 0;
  u16 raw_new = 0;
  // The hand's position (OnlineRules.h CssHand), when it could be read.
  bool hand = false;
  float hand_x = 0;
  float hand_y = 0;
  // Token resting on a character: Start locks it in, B picks it up.
  bool Down() const { return human && placed && !flying && character != kNoCharacter; }
  bool operator==(const CssPort&) const = default;
};
struct CssView
{
  bool on_css = false;
  std::array<CssPort, 2> ports{};
  bool operator==(const CssView&) const = default;
};

enum class Step : u8
{
  Idle = 0,    // no order on this game
  First = 1,   // first picker's turn (Free: the winner may also lock in)
  Second = 2,  // second picker's turn
  Done = 3,    // both picked: Orca presses Start until the screen changes
};

// Stored in the match block at kArea.
struct State
{
  u8 game = 0;  // the set game this belongs to (0: none)
  Step step = Step::Idle;
  u8 first = 0;      // port that picks first
  u8 locked = 0;     // Free: bit p set once port p locked in
  u32 since = 0;     // frame the step began
  u16 elapsed = 0;   // frames into the step at the last frame (read by the input gate)
  u8 held = 0;       // L and R as each port's turn ended: bit 2p port p's L, bit 2p + 1 its R
  // Where each port's hand last held its token over a character this order, in 1/64 units (bit p
  // of `anchored` once known): a token still in hand after its turn goes back there.
  u8 anchored = 0;
  std::array<s16, 2> anchor_x{0, 0};
  std::array<s16, 2> anchor_y{0, 0};
  bool operator==(const State&) const = default;
};

// Where a token goes with no anchor known for either port: the middle of the character grid.
constexpr float kGridMiddleX = 0.0f;
constexpr float kGridMiddleY = 5.0f;

// The order is on: the character select of an applying set game, begun for this game and picker.
// Shared by the step machine, the gate and the overlay's line.
bool Active(const SetView& set, const CssView& css, const State& state);
// The current turn's time is up (SetView::pick_frames).
bool TimedOut(const SetView& set, const State& state);

// Advances the step machine by one frame. Pure. Starts the order on the character select of game
// 2+, moves on when the current picker locks in or time runs out, and clears a stale order on any
// other character select. Does nothing off the character select.
State Advance(const SetView& set, const CssView& css, const State& state, int frame);

// Input gate masks and presses for the next frame. Pure.
Rollback::InputGate::Masks Gate(const SetView& set, const CssView& css, const State& state);

// Bitmask of ports that have locked in, for the overlay's LOCKED IN ribbons. Display only.
u8 LockedPorts(const SetView& set, const CssView& css, const State& state);

// The port whose token is still in hand a turn's time (pick_frames) after both turns ended, or -1 (the second
// picker's first). Frame() writes it as the ranked no-show (MatchBlock CSS_NO_SHOW), which takes
// that player out of the set as one who didn't pick in time (Rollback/OnlineMatch.cpp). Pure.
int NoShow(const SetView& set, const CssView& css, const State& state);

// Supernova 2025's Meta Knight rule: if exactly one player is Meta Knight, the other port may pick
// any legal stage. Returns that port, or -1. `meta_knight` is Meta Knight's id in the same id space
// as `characters`.
int MkFreePicker(const std::array<int, 2>& characters, int meta_knight);
}  // namespace Orca::UX::CharOrder
