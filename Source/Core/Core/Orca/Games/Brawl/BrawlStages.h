// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
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

// Ranked stage selection on vanilla Brawl's own stage select (RSBE01 rev 2, Supernova 2025 rules).
// See ORCA.md, "Ranked steps". Game 1 uses 1-2-1 strikes on the five starters, with the
// first striker chosen by a coin flip. Later games: the winner may ban, then the loser picks
// (with DSR). Also handles the Meta Knight clause, turn timers and their defaults, and greys out
// struck or illegal stages.
//
// Everything is a pure function of emulated memory. State lives in the match block's step region
// (+0x80..+0xBF), the frame hook updates it from what the game shows, and the input gate reads it
// back. Writes are idempotent and the turn timer counts game frames, so both machines, every
// resimulation and a drop-in's keyframe all agree.
//
// - Ranked or casual comes from the match block header (written by OnlineRules); game 1's first
//   striker is the header's coin.
// - The set (game number, last winner, stages won) comes from SetBlock.h when valid; otherwise
//   this flow keeps its own record from the results screens.
// - A new stage select (frame counter reset) restarts the steps, so stale bytes never carry over.
// - The step machine is shared with Project+ (RankedSteps.h). UX.cpp ORs all rules' gate masks.
//
// Each player has their own cursor (StageCursors.h). On your turn X strikes or bans and Y skips
// the ban. The picker's A picks; anyone's A proposes, and matching proposals pick that stage
// (RankedSteps.h Propose). B withdraws a proposal. The gate masks the game's own cursor and pads;
// the flow moves the game's cursor and presses A itself.
//
// Casual games ("Casual stage pick" in ORCA.md) use the same machinery: both players name a
// preferred stage, or Random, with A within 15 s. Matching picks win; otherwise a coin seeded from
// the room and game decides. Random, and a player who names none, get a random legal stage.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::BrawlStages
{
// Supernova 2025's legal stages, in the Brawl ruleset's order (Rulesets/brawl-supernova-2025.json).
enum Stage : u8
{
  Battlefield,
  PokemonStadium,  // Melee's
  LylatCruise,
  Smashville,
  YoshisIsland,  // Brawl's, not Melee's
  FinalDestination,
  DelfinoPlaza,
  kStages,
};

// ---- Game memory (Brawl rev 2) ----
constexpr u32 SCENE_MANAGER = 0x805A0060;  // gfSceneManager*: +4 is the current scene
constexpr u32 PAD_SYSTEM = 0x805A0040;     // gfPadSystem*
constexpr u32 RNG_SEED = 0x805A00BC;       // g_mtRandDefault's seed
constexpr u32 GAME_GLOBAL = 0x805A00E0;    // g_GameGlobal*
// gfPadSystem: four 0x40-byte pads from +0x40; +0 is the held buttons.
constexpr u32 PAD_SYSTEM_PADS = 0x40;
constexpr u32 PAD_STRIDE = 0x40;
// gfPadButtons bits.
constexpr u16 GF_X = 0x0400;
constexpr u16 GF_Y = 0x0800;
// scSelStage +0x3AC is its task.
constexpr u32 SCENE_SSS_TASK = 0x3AC;
// Task: +0x8C stage icon pointers, +0x244 hovered item, +0x248 selected slot (-1 none), +0x278 the
// port driving the cursor (0xF0 = any port).
constexpr u32 TASK_ICONS = 0x8C;
constexpr u32 TASK_HOVER = 0x244;
constexpr u32 TASK_SELECTED = 0x248;
constexpr u32 TASK_PORT = 0x278;
constexpr u32 PORT_ANY = 0xF0;

// ---- Match block (MatchBlock.h, SetBlock.h) ----
constexpr u32 kBlock = 0x806F2380;
// Header: 'YGM1', version, mode, ruleset.
constexpr u32 kHeaderMagic = 0x59474D31;
constexpr u8 kHeaderVersion = 1;
constexpr u8 kModeCasual = 1;
constexpr u8 kModeRanked = 2;
constexpr u8 kRulesetBrawl = 1;
// Ranked set (SetBlock.h): +0x00 games recorded, +0x01 wins for ports 1 and 2, +0x03 done,
// +0x05 last decided game's winner (0xFF none), +0x20 up to 8 game records of 12 bytes
// (+0x04 winner, +0x06 stage kind).
constexpr u32 kResultsSet = kBlock + 0x210;
// This flow's state: the block's step region (Project+ uses the same bytes for its own flow).
constexpr u32 kRegion = kBlock + 0x80;
constexpr u32 kRegionSize = 0x40;
constexpr u32 kMagic = 0x59474253;  // 'YGBS'
constexpr u8 kVersion = 1;

enum class Phase : u8
{
  Idle = 0,      // before this game's stage select
  Select = 1,    // on the stage select (steps under way or done)
  Fight = 2,     // in game
  Recorded = 3,  // result recorded in the set
};

// Contents of the step region.
struct State
{
  // Fallback set record from the results screens, used when SetBlock.h has none (SetOf).
  Ranked::SetView set;
  std::array<u8, 2> score{};
  u8 coin = 0xFF;  // game 1's first striker, 0xFF until read from the header
  Phase phase = Phase::Idle;
  Ranked::Progress progress;
  std::array<s8, 2> characters{-1, -1};
  std::array<u16, 2> buttons{};  // last frame's buttons per port, for X/Y edge detection
  u32 clock = 0;                 // stage select frame counter at the last tick
  s8 hovered = -1;               // stage under the cursor last frame
  u8 hovered_for = 0;            // frames the cursor has rested there (A needs at least 1)
  u8 hover_key = 0xFF;           // page and hovered item (0xFF = nothing)
  // Casual: seed for random choices, from the room, the header's coin and the game number.
  u32 seed = 0;
  bool operator==(const State&) const = default;
};
std::optional<State> ReadState(const GuestMemory& memory);
// Writes only the bytes that differ.
void WriteState(GuestMemory& memory, const State& state);
void ClearState(GuestMemory& memory);

// A legal stage's icon: page (0 Brawl, 1 Melee), slot on the page, and a point inside it in
// cursor coordinates.
struct Tile
{
  u8 page;
  u8 slot;
  float x;
  float y;
};
const Tile& TileFor(int stage);

// Maps the game's stage id (result info +0x0C) to a legal Stage, or -1.
int StageForStageId(int stage_id);

// The stage select as seen this frame.
struct View
{
  bool present = false;  // scSelStage with its task
  u32 task = 0;
  u32 hovered_item = 0xFFFFFFFF;
  s32 selected = -1;
  u32 clock = 0;
};
View ReadView(const GuestMemory& memory);

// Whether the match block header says ranked Brawl.
bool RankedBrawlMatch(const GuestMemory& memory);
// Whether the match block header says casual Brawl.
bool CasualBrawlMatch(const GuestMemory& memory);
// The set from SetBlock.h when valid, else this flow's own record. `score` gets games won per port.
Ranked::SetView SetOf(const GuestMemory& memory, const State& state,
                           std::array<u8, 2>* score = nullptr);
// Unit tests: override the header check (nullopt restores it).
void SetRankedForTests(std::optional<bool> ranked);
void SetCasualForTests(std::optional<bool> casual);

// Per-frame hook, including resimulation: tracks the set and steps and writes the turn, greyed
// tiles and picked stage. A casual pick only runs when `two_players` (ports 1 and 2 both in play).
void Frame(GuestMemory& memory, bool two_players = true);
// Input gate: which buttons each port may not press on the stage select. Read only.
void Masks(const GuestMemory& memory, Rollback::InputGate::Masks* masks);
// Overlay lines: set score, and on the stage select whose turn it is with its timer. UI only.
std::pair<std::string, std::string> Lines(const GuestMemory& memory,
                                          const std::vector<Orca::Events::PortInfo>& ports);

// The current step on the stage select, or nullopt. Pure; Relabel.h draws it on screen.
std::optional<Ranked::Turn> CurrentTurn(const GuestMemory& memory);

// What the overlay draws for the two cursors (StageCursors.h). UI only.
StageCursors::View CursorView(const GuestMemory& memory,
                              const std::vector<Orca::Events::PortInfo>& ports);

// CPU-thread frame hook and gate source.
void FrameHook(const Core::CPUThreadGuard& guard, bool resimulating,
               const std::vector<Orca::Events::PortInfo>& ports);
void GateMasks(const Core::CPUThreadGuard& guard, Rollback::InputGate::Masks* masks);
}  // namespace Orca::UX::BrawlStages
