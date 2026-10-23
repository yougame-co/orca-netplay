// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// Relabels the game's big menu words with Orca's meaning: READY TO FIGHT! on the character select
// (PRESS START TO LOCK IN!, ...), STAGE SELECT (whose turn to strike, ban or pick), the mode title
// (RANKED, CASUAL) and Project+'s stage select legend.
//
// Orca never draws into the game's art. It changes one invisible value in the texture so that
// Dolphin's custom-texture hash gives a different name per label, and the Orca texture pack
// (Data/Sys/Orca/Textures/RSBE01) supplies art for each name. Without the pack the game's words
// show. The mark is a transparent palette entry's blue (C4 textures) or the first texel (I4/IA8),
// which Dolphin's sampled texture hash always reads.
//
// Determinism: labels are a pure function of emulated memory and the ports in effect, written every
// frame including re-simulation, and only where the slot holds the game's value or a known mark.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::Relabel
{
// READY TO FIGHT! (MenSelchrReady01_2).
enum class Band : u8
{
  Game = 0,
  LockIn = 1,       // PRESS START TO LOCK IN!
  WinnerFirst = 2,  // WINNER PICKS FIRST!
  LoserNow = 3,     // LOSER PICKS NOW!
};

// STAGE SELECT (MenSelmapBack07). 1-6 strikes (P1 1-3, P2 1-3 left), 7-10 bans (P1 1-2, P2 1-2),
// 11-12 picks (P1, P2), 13 a casual game's simultaneous pick.
using Line = u8;
constexpr Line LINE_GAME = 0;
constexpr Line LINE_PICK_A_STAGE = 13;

// The stage select's BRAWL / VERSUS (MenSelchrRtitle.30).
enum class Title : u8
{
  Game = 0,
  Ranked = 1,
  Casual = 2,
};

// Where one texture's mark lives and how labels encode into it.
struct Slot
{
  enum class Kind : u8
  {
    None,
    PaletteEntry,  // u16 RGB5A3 entry: game value + step * label
    HighNibble,    // high nibble of an I4 texel byte: (game value + label) mod 16
    Byte,          // IA8 texel intensity byte: game value + label
  };
  Kind kind = Kind::None;
  u32 address = 0;
  u16 game = 0;
  int labels = 0;  // highest label
  // The value to write for `label` (0 is the game's own), given the current value.
  u16 ValueFor(int label, u16 now) const;
  // The label a value encodes, or nullopt if it is neither the game's value nor a mark.
  std::optional<int> LabelOf(u16 now) const;
  u16 Read(const GuestMemory& memory) const;
  void Write(GuestMemory& memory, u16 value) const;
};

// The slots, or Kind::None when their screen is not loaded.
Slot FindBand(const GuestMemory& memory);
Slot FindLine(const GuestMemory& memory);
Slot FindTitle(const GuestMemory& memory);
std::array<Slot, 2> FindLegend(const GuestMemory& memory);  // Project+: words, buttons

// What each label should say now (pure).
Band WantedBand(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
Line WantedLine(const GuestMemory& memory);
Title WantedTitle(const GuestMemory& memory);
bool WantedLegend(const GuestMemory& memory);

// Whether READY TO FIGHT! is on screen (pure).
bool BandUp(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
// Whether READY TO FIGHT! is up with an Orca label, so the turn timer is drawn beside it, not in
// the rules bar (pure).
bool ClockInBand(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);

// The line for a stage step. `kind` is a Ranked::StepKind, `port` is 0, 1 or 2 for both, `left` is
// strikes or bans remaining (pure).
Line LineFor(u8 kind, u8 port, int left);

// What the labels say after one frame.
struct Shown
{
  Band band = Band::Game;
  Line line = LINE_GAME;
  Title title = Title::Game;
  bool legend = false;
  bool band_up = false;  // BandUp, where there is a band to label
  bool operator==(const Shown&) const = default;
};

// Writes each slot's wanted value where it differs and returns how many slots changed.
int Apply(GuestMemory& memory, const std::vector<Events::PortInfo>& ports, Shown* shown = nullptr);

// Called by the frame hook every frame, including re-simulation (Brawl rev 2 and Project+ only).
// Outside re-simulation it also publishes Current(). Returns this frame's labels.
Shown Frame(const Core::CPUThreadGuard& guard, bool resimulating,
            const std::vector<Events::PortInfo>& ports);
// ClockInBand from an already computed Shown.
inline bool ClockInBand(const Shown& shown)
{
  return shown.band != Band::Game && shown.band_up;
}
// The last published labels. Safe from any thread.
Shown Current();
}  // namespace Orca::UX::Relabel
