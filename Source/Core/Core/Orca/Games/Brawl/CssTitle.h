// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// Swaps the Versus character select's title logo (Brawl's BRAWL, Project+'s VERSUS) for CASUAL,
// RANKED or FRIENDS in an online room. See ORCA.md, "On-screen UI".
//
// The logo is a texture (MenSelchrPanelRule3.1), so Orca doesn't draw on it. Instead it nudges the
// blue of the texture's fully transparent background palette entry by 1-3 steps. That change is
// invisible but gives the texture a new hash, and Orca's texture pack maps each hash to a label.
// Without the pack, the original title shows.
//
// The title is a pure function of emulated memory and the synced port plan, so both machines write
// the same value on the same frame:
//   1. a casual or ranked queue room's match block header (OnlineRules.h): CASUAL or RANKED;
//   2. a With Friends main-menu pick: FRIENDS;
//   3. two or more ports plugged in with no queue header: FRIENDS;
//   4. a With Anyone Casual or Ranked pick (solo character select while searching): CASUAL/RANKED;
//   5. otherwise the game's own title.
//
// Addresses (Brawl rev 2 and Project+): scene manager 0x805A0060; +4 is the current scene, whose
// +0x410 is the character select's gfArchive; +0x10 is the running sequence.
//
// The palette word is only written when it holds the game's value or one of our marks, and only
// when it differs. The archive reloads each time the character select opens, so marks never leak.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::CssTitle
{
enum class Title : u8
{
  Game = 0,
  Casual = 1,
  Ranked = 2,
  Friends = 3,
};

std::string_view TitleName(Title title);

// The main-menu pick Orca stores at sqVsMelee +0x18. 0 if sqVsMelee isn't running or the word
// isn't a menu code (e.g. the heap fill 0xCCCCCCCC).
u32 ReadPick(const GuestMemory& memory);

// What the title should say. `players` is the number of ports plugged in.
Title Wanted(const GuestMemory& memory, int players);

// The palette word that carries the mark.
struct Slot
{
  u32 address = 0;  // 0 if no known title texture is loaded
  u16 game = 0;     // the game's original value
  int step = 0;     // value = game + step * title (Brawl -1, Project+ +1)
  u16 ValueFor(Title title) const
  {
    return static_cast<u16>(game + step * static_cast<int>(title));
  }
  // Which title a value stands for, if it is the original or one of our marks.
  std::optional<Title> TitleOf(u16 value) const;
};
Slot FindSlot(const GuestMemory& memory);

// Writes the wanted title if needed. Returns words written (0 or 1); `shown` receives the
// resulting title (Game when there is no slot).
int Apply(GuestMemory& memory, int players, Title* shown = nullptr);

// Per-frame hook (Brawl rev 2 and Project+ only). Runs on resimulated frames too.
void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Orca::Events::PortInfo>& ports);
}  // namespace Orca::UX::CssTitle
