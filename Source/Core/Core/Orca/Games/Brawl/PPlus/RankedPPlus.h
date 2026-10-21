// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// Project+'s ranked stage flow on its own stage select, using RankedSteps.h with the set kept in
// the match block. Outside the stage select it loads the 2024 Proposed preset. On the stage select
// the input gate masks Project+'s own controls, each player acts on their own cursor, and the frame
// hook writes the strike table so Project+'s own check refuses struck stages. Apply is a pure,
// idempotent function of emulated memory and the frame, so rollback re-runs write nothing new.
// See ORCA.md, "Ranked steps".
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::RankedPPlus
{
// Project+ v3.2's stage select data (Source/Netplay/Net-Random.asm).
constexpr u32 RSS_EXDATA = 0x8042C4E8;
constexpr u32 RSS_EXDATA_SIZE = 0x320;
// Bytes of the preset the flow writes; every shipped preset matches past this point.
constexpr u32 PRESET_BYTES = 0xA8;
constexpr u32 PAGE_INDEX = 0x8042C821;
constexpr u32 STAGE_STRIKE_TABLE = 0x8042C822;
constexpr u32 STRIKE_PAGES = 5;
constexpr u32 CURRENT_PAGE = 0x80496000;
// muSelectStageTask fields (same layout as Brawl).
constexpr u32 SSS_HOVERED = 0x244;     // item under the cursor
constexpr u32 SSS_SELECTED = 0x248;    // selected page position, -1 none
constexpr u32 SSS_CONTROLLER = 0x278;  // 0xF0 every port, 0-3 that port only
constexpr u32 SSS_CURSOR = 0x200;      // cursor; float x and y below, up is +y
constexpr u32 SSS_CURSOR_X = 0x3C;
constexpr u32 SSS_CURSOR_Y = 0x40;
constexpr u32 SSS_PAGE = 0x228;        // CUSTOM_PAGE is the stage builder's page
constexpr u32 CUSTOM_PAGE = 2;
// Hovered item values: 0 nothing, 1-0x34 a stage, then these.
constexpr u32 PAGE_ITEM = 0x35;    // page button
constexpr u32 RANDOM_ITEM = 0x36;  // Random
// Project+'s My Music from the stage select (Source/Community/MusicSelect.asm): a step word, then
// the song, stage, page, mode and hidden alts it carries there and back. Step 1 hides the stage
// select's tiles and turns its pick into My Music; 4 backs it out; 7 picks a stage by itself.
constexpr u32 MUSIC_SELECT = 0x80002810;
// The words its own cleanup clears, as offsets from MUSIC_SELECT.
constexpr std::array<u32, 10> MUSIC_SELECT_WORDS{0x00, 0x08, 0x0C, 0x10, 0x14,
                                                 0x18, 0x20, 0x24, 0x28, 0x2C};

// The 2024 Proposed preset, Project+ v3.2's pf/stage/switch/Switch03.rss.
extern const u8 kSwitch03[RSS_EXDATA_SIZE];

// The ruleset's stage index for a Project+ stage kind on the preset's page 0, or -1.
int LegalIndexOfKind(int kind);
// The ruleset's stage at each position of page 0 in memory, -1 for none.
std::vector<int> PageZero(const GuestMemory& memory);

// On the character select, clears a My Music step left from an earlier stage select: step 1 hides
// the next stage select's tiles and turns its pick into My Music. Every Project+ session runs it.
// Pure and idempotent; returns bytes changed.
int ClearStaleMusicSelect(GuestMemory& memory);

// Runs the flow for one frame. Pure and idempotent; returns how many bytes changed.
int Apply(GuestMemory& memory, int frame, bool two_players);
// The input gate's masks for the next frame. Read-only.
Rollback::InputGate::Masks Masks(const GuestMemory& memory);

// Overlay text: the set line on menus, and the turn and timer on the stage select. Empty hides it.
struct Lines
{
  std::string set;
  std::string turn;
  bool operator==(const Lines&) const = default;
};
Lines Describe(const GuestMemory& memory, int frame, const std::vector<Events::PortInfo>& ports);

// The current step on the stage select, or nullopt. A pure function of emulated memory.
std::optional<Ranked::Turn> CurrentTurn(const GuestMemory& memory);

// The two cursors for the overlay. UI only.
StageCursors::View CursorView(const GuestMemory& memory,
                              const std::vector<Events::PortInfo>& ports, int frame);

// Frame hook entry: runs the flow and publishes the overlay lines.
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone);
// The last published overlay lines. Any thread.
Lines CurrentLines();
}  // namespace Orca::UX::RankedPPlus
