// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/FreeSpace.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// Character pick order for games 2+ of a ranked set, on the game's own character select. See
// ORCA.md, "Character order". Two orders exist (kLaterGameOrder picks one):
//   - WinnerFirst (in use, as in the standard rulesets): the winner picks, then the loser picks
//     knowing the winner's character.
//   - Free: the loser is asked first; the winner may lock in at any time meanwhile.
// Each turn has 45 seconds. When time runs out, the player keeps the character their token is on
// (by default the previous game's). A token still in hand then goes down for them: Orca takes their
// stick, brings the hand back to the last character it held the token over this order (its
// anchor) and places it there, and the next turn starts only once it is down. So a turn always
// ends with a pick, and the second picker always sees the first one. A token still in hand 45
// seconds after both turns makes its player the ranked no-show (NoShow), who leaves the set.
//
// Controls on your turn: A places the token on a character, B picks it back up, Start locks the
// pick in. Start never reaches the game here; the order reads it from the raw input latch
// (Queue.h RAW). The other player's controller does nothing until their turn. Once both have
// picked, a queue2 room continues straight on (Queue.h); otherwise Orca presses Start for the
// second picker. A player whose turn ended with the token in hand has no input until it is down.
//
// Locking in needs Start, not A: tokens stay placed from one game's character select to the next,
// and A on another character would otherwise lock in the old pick.
//
// L and R held as a player locks in (or as their time runs out) are kept, and held for them while
// the character select ends: both games read them then (Brawl: Samus or Zelda with L or R plays
// Zero Suit Samus or Sheik; Project+: L makes Nana lead the Ice Climbers, R picks a hidden
// costume). The choice is made with the pick, so the second picker never counterpicks a Samus
// that turns into Zero Suit Samus afterwards.
//
// Everything is a pure function of emulated memory and the frame number, so both machines and
// every resimulation agree. State lives in the match block at kArea, written only by the frame
// hook, and the input gate's masks are computed from that memory (Rollback/InputGate.h).
//
// Brawl uses the same order as Project+. Supernova's exact order puts the stage pick before the
// characters; MkFreePicker() is that order's Meta Knight rule, for when it is implemented.
//
// Character select memory (Brawl rev 2 and Project+): scene "scSelctCharacter" +0x400 is
// muSelCharTask; port i's area is at task +0x44 + 4 * i:
//   +0x0C u32 held buttons, +0x10 newly pressed (PAD_* bits, after the input gate)
//   +0x1B4 player kind (1 = a human has joined)
//   +0x1B8 character under the hand while holding the token, else the token's (kNoCharacter: none)
//   +0x1F8 u8 1 while the token is in hand, +0x1F9 1 while it is flying
namespace Orca::UX
{
class GuestMemory;
}

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

// 45 seconds per pick at 60 fps.
constexpr int PICK_FRAMES = 45 * 60;

// 16 bytes right after the 512-byte match block: +0 kMagic, +4 game, +5 step, +6 first,
// +7 locked, +8 since, +0xC elapsed, +0xE held, +0xF 0. Anything without the magic reads as Idle.
constexpr u32 kArea = FreeSpace::kMatchBlock.end;
constexpr u32 kAreaSize = 0x10;
constexpr u32 kMagic = 0x5947434F;  // "YGCO"
static_assert(kArea % 16 == 0 && kArea + kAreaSize <= FreeSpace::kMatchBlockLimit &&
              FreeSpace::Detail::Free({kArea, kArea + kAreaSize}));
// The anchors, the match block's last 12 bytes (MatchBlock.h): +0 anchored, +1..+3 0, then at
// +4 + 4 * port s16 x, s16 y. Read only with kArea's magic.
constexpr u32 kAnchorArea = FreeSpace::kMatchBlock.begin + 0x1F4;
constexpr u32 kAnchorAreaSize = 0x0C;
static_assert(kAnchorArea + kAnchorAreaSize <= FreeSpace::kMatchBlock.end &&
              FreeSpace::Detail::Free({kAnchorArea, kAnchorArea + kAnchorAreaSize}));
// Where a token goes with no anchor known for either port: the middle of the character grid.
constexpr float kGridMiddleX = 0.0f;
constexpr float kGridMiddleY = 5.0f;

// Match block offsets ReadSet() uses (header: MatchBlock.h; set: SetBlock.h).
namespace Block
{
constexpr u32 MAGIC = 0x00;  // u32 "YGM1"
constexpr u32 MAGIC_VALUE = 0x59474D31;
constexpr u32 VERSION = 0x04;  // u8 1
constexpr u32 MODE = 0x05;     // u8: 2 ranked
constexpr u32 RULESET = 0x06;  // u8: 1 Brawl (Supernova 2025), 2 Project+ (2024)
// Ranked set (SetBlock.h, +0x210).
constexpr u32 SET_WINS = 0x211;         // u8[2]: games won by ports 1 and 2
constexpr u32 SET_DONE = 0x213;         // u8: 0 while the set is open
constexpr u32 SET_LAST_WINNER = 0x215;  // u8: last decided game's winner, 0xFF none
constexpr u8 MODE_RANKED = 2;
}  // namespace Block

// Advances the step machine by one frame. Pure. Starts the order on the character select of game
// 2+, moves on when the current picker locks in or time runs out, and clears a stale order on any
// other character select. Does nothing off the character select.
State Advance(const SetView& set, const CssView& css, const State& state, int frame);

// Input gate masks and presses for the next frame. Pure.
Rollback::InputGate::Masks Gate(const SetView& set, const CssView& css, const State& state);

// The overlay's status line, e.g. "ada locked in · bo: pick and press Start · 0:31". Empty when
// inactive. UI only.
std::string Line(const SetView& set, const CssView& css, const State& state,
                 const std::vector<Events::PortInfo>& ports);

// The port whose turn it is, or -1. Display only.
int Picker(const CssView& css, const State& state);
// The latest Picker() from the frame hook, for the overlay. Any thread.
int CurrentPicker();
// Ribbon on the picker's portrait so it's obvious whose turn it is: "YOUR PICK" on the picker's
// screen, else "<NAME> PICKS" ("P1 PICKS" without a name). Empty with no picker. UI only.
std::string PickerRibbon(int picker, const std::vector<Events::PortInfo>& ports);
// The latest PickerRibbon() from the frame hook, for the overlay. Any thread.
std::string CurrentPickerRibbon();

// Bitmask of ports that have locked in, for the overlay's LOCKED IN ribbons. Display only.
u8 LockedPorts(const SetView& set, const CssView& css, const State& state);

// The port whose token is still in hand PICK_FRAMES after both turns ended, or -1 (the second
// picker's first). Frame() writes it as the ranked no-show (MatchBlock CSS_NO_SHOW), which takes
// that player out of the set as one who didn't pick in time (Rollback/OnlineMatch.cpp). Pure.
int NoShow(const SetView& set, const CssView& css, const State& state);

// Supernova 2025's Meta Knight rule: if exactly one player is Meta Knight, the other port may pick
// any legal stage. Returns that port, or -1. `meta_knight` is Meta Knight's id in the same id space
// as `characters`.
int MkFreePicker(const std::array<int, 2>& characters, int meta_knight);

// ---- Memory ----

SetView ReadSet(const GuestMemory& memory);
CssView ReadCss(const GuestMemory& memory);
State ReadState(const GuestMemory& memory);
// Writes changed bytes only. Returns the number of bytes changed.
int WriteState(GuestMemory& memory, const State& state);

// Per-frame hook: advances and writes the state (also when resimulating) and updates the overlay.
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports);
// Input gate source: masks computed from memory alone.
Rollback::InputGate::Masks GateFrame(const Core::CPUThreadGuard& guard);

// Test override ORCA_TEST_CHAR_ORDER=<pplus|brawl>:<game>:<winner port 1-2>[:free|winnerfirst]
// fakes a set view on every character select, with no match block. Set it on both machines.
bool ParseTestSpec(const std::string& spec, SetView* out);
// Whether ORCA_TEST_CHAR_ORDER is set (the raw input latch then also runs on its character select).
bool TestActive();
}  // namespace Orca::UX::CharOrder
