// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Games/Brawl/FreeSpace.h"
#include "Core/Orca/Policy/CharOrder.h"
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
// The step machine (SetView, CssView, State, Advance, Gate, LockedPorts, NoShow, MkFreePicker):
// Policy/CharOrder.h, engine-side competitive policy (S2 commit 7).

// 16 bytes right after the 512-byte match block: +0 kMagic, +4 game, +5 step, +6 first,
// +7 locked, +8 since, +0xC elapsed, +0xE held, +0xF 0. Anything without the magic reads as Idle.
constexpr u32 kArea = FreeSpace::kMatchBlockHead.end;
constexpr u32 kAreaSize = 0x10;
constexpr u32 kMagic = 0x5947434F;  // "YGCO"
static_assert(kArea % 16 == 0 && kArea + kAreaSize <= FreeSpace::kMatchBlockLimit &&
              FreeSpace::Detail::Free({kArea, kArea + kAreaSize}));
// The anchors, the match block's last 12 bytes (MatchBlock.h): +0 anchored, +1..+3 0, then at
// +4 + 4 * port s16 x, s16 y. Read only with kArea's magic.
constexpr u32 kAnchorArea = FreeSpace::kMatchBlock.begin + 0x1F4;
constexpr u32 kAnchorAreaSize = 0x0C;
static_assert(kAnchorArea + kAnchorAreaSize <= FreeSpace::kMatchBlockHead.end &&
              FreeSpace::Detail::Free({kAnchorArea, kAnchorArea + kAnchorAreaSize}));

// Match block offsets ReadSet() uses (header: MatchBlock.h; set: SetBlock.h).
namespace Block
{
constexpr u32 MAGIC = 0x00;  // u32 "YGM1"
constexpr u32 MAGIC_VALUE = 0x59474D31;
constexpr u32 VERSION = 0x04;  // u8 1
constexpr u32 MODE = 0x05;     // u8: 2 ranked
constexpr u32 RULESET = 0x06;  // u8: 1 Brawl (Supernova), 2 Project+ (2024)
// Ranked set (SetBlock.h, +0x210).
constexpr u32 SET_WINS = 0x211;         // u8[2]: games won by ports 1 and 2
constexpr u32 SET_DONE = 0x213;         // u8: 0 while the set is open
constexpr u32 SET_LAST_WINNER = 0x215;  // u8: last decided game's winner, 0xFF none
constexpr u8 MODE_RANKED = 2;
}  // namespace Block

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
