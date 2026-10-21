// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/MatchTypes.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "Core/Orca/Mod.h"

namespace Core
{
class CPUThreadGuard;
}

// Reads a matchmade game's result from the game's memory: which port won, plus the stage,
// characters and stocks for the room's game reports. Read only, except QueueRules below, which a
// host writes during solo frames before any keyframe.
//
// Memory layout (Brawl rev 2; Project+ runs the same executable), from g_GameGlobal
// (0x805A00E0 -> 0x90181300):
//   +0x08 gmGlobalModeMelee (0x90180F20): +0x1B stage id, +0x20 time limit in frames,
//         +0x98 players' init data, 4 x 0x5C: +0x00 character, +0x01 player type (0 human, 1 CPU,
//         3 none), +0x04 starting stocks (0xFF in a timed match)
//   +0x18 result info (0x9017F420), filled before the results scene's first frame:
//         +0x01 u8 mode (0 time, 1 stock), +0x04 u32 time limit (minutes), +0x08 u32 stocks,
//         +0x0C u16 stage id, +0x0E u16 how the fight ended, +0x1378 u8 decision (1 time up,
//         2 win, 9 no contest)
//         per port at +0x24 + port * 0x2AC: +0x00 u8 character, +0x01 u8 player type,
//         +0x0A u8 stocks left (0xFF timed), +0x0E u8 place (0 winner), +0x10 u32 KOs,
//         +0x14 u32 falls, +0x18 u16 self-destructs
//   +0x1C rules (0x9017F360): +0x02 u8 mode, +0x03 u8 timed minutes, +0x04 u8 stocks,
//         +0x05 u8 handicap (0 off), +0x06 u8 damage ratio x10,
//         +0x08 u8 stock match time limit in minutes (0 none), +0x0C u8 item frequency (0 none)
// gmGlobalRecord menu data (0x9017B640 + 0x810 = 0x9017BE50): +0x00 u8 item frequency (0 None),
// where a match takes its items from (RSBE01.patches' and PPLUS32.patches' items groups).
// Scene names (scene manager 0x805A0060 +4 -> +0 name): "scMelee" fight, "scMemoryChange" between
// scenes, "scVsResult" results screen.
namespace Orca::UX
{
class GuestMemory;

struct PortResult
{
  bool present = false;  // a player (human or CPU) on this port
  bool human = false;
  int character = -1;
  int stocks = -1;  // stocks left; -1 in a timed match
  int place = -1;   // 0 the winner
  bool operator==(const PortResult&) const = default;
};

struct ResultBlock
{
  int mode = -1;  // 0 time, 1 stock
  int stage = -1;
  // Bytes +0x0E..+0x0F. +0x0F is the number of players tied for first (m_numWinners).
  int end = 0;
  int decision = 0;  // +0x1378: 1 time up, 2 KO win, 9 no contest (0: not read)
  std::array<PortResult, 4> ports{};
  // With mod.json's rankedRules: the first rule this match was not played under ("stocks",
  // "timeMinutes", "items", "handicap", "damageRatio"; the rules block and the item record above),
  // or empty.
  std::string_view differing_rule;
  bool operator==(const ResultBlock&) const = default;
};

struct Reading
{
  // Other, Fight, Between, Results: Engine/MatchTypes.h (the session acts on it).
  using Scene = Orca::Engine::SceneKind;
  Scene scene = Scene::Other;
  // In the results scene only.
  ResultBlock block;
  // The ranked match block's set state, on every frame of a ranked match.
  std::optional<SetBlock::SetState> set;
  bool operator==(const Reading&) const = default;
};

// Reads one frame's state (read only). With `ranked_rules`, the results screen's reading also
// names the first rule the match differed from (ResultBlock::differing_rule).
Reading ReadResults(const GuestMemory& memory,
                    const std::optional<Orca::Mod::RankedRules>& ranked_rules = std::nullopt);

// One game's result, as both players report it: Engine/MatchTypes.h (the session reports it).
using Orca::Engine::GameResult;

// Game `index` of a ranked set, as the match block recorded it.
GameResult FromRecord(const SetBlock::SetState& set, int index);

// Judges a results screen. Void "rules-<rule>" when the match differed from mod.json's
// rankedRules, void "ports" unless exactly two humans are on ports 1 and 2, void "nocontest" unless
// the fight ended normally. One player first is a win, both first a draw.
GameResult Judge(const ResultBlock& block, int frame);

// Turns per-frame readings into game results once those frames are final, identically on every
// machine. A game is a fight entered after `plug_frame` (so a fight already running when the
// opponent joined never counts) and ended by the results scene, or by any other scene (void).
class ResultsTracker
{
public:
  // Stores the reading taken at the start of `frame`. A rollback re-run replaces the earlier
  // reading. When `resyncs` (Events::Resyncs()) changes, the state jumped (keyframe load, back to
  // solo), so everything stored is dropped.
  void Store(int frame, const Reading& reading, u64 resyncs);
  // Frames through `confirmed` are final (real inputs, no re-run pending). Returns the games they
  // finish, in order. `starts`, if given, receives each counted game's first fight frame, the id
  // its game's report will carry, once, and only for a fight begun after `plug_frame` as the
  // reports. Ranked set games begin with fighters in (SET_FIGHT_FLAGS' FIGHT_SEEN), named by
  // SET_FIGHT_START; scene games begin on entering Fight.
  std::vector<GameResult> Confirm(int confirmed, int plug_frame,
                                  std::vector<int>* starts = nullptr);
  void Reset();
  // Baseline a reconstructed frame without reporting games that happened before joining.
  void Rebase(int frame, const Reading& reading, u64 resyncs);
  // Scene of the last final reading, and of the newest (possibly predicted) reading.
  std::optional<Reading::Scene> FinalScene() const;
  std::optional<Reading::Scene> LatestScene() const;

private:
  std::map<int, Reading> m_pending;
  u64 m_resyncs = 0;
  bool m_started = false;
  // The last final reading.
  bool m_have_last = false;
  int m_last_frame = -1;
  Reading m_last;
  // A counted fight is on, or between it and its results screen.
  bool m_in_game = false;
  bool m_after_fight = false;
  // The scene game's start, or an uncounted baseline for a fight rebuilt before joining.
  int m_scene_start = -1;
  // Ranked set games seen so far. The first final reading after a reset only initializes the count.
  bool m_have_set = false;
  int m_set_games = 0;
  // The last ranked fight seen begun, fighters in (its first frame), so each is counted once.
  bool m_have_fight = false;
  int m_fight_start = -1;
};

// The process-wide tracker (CPU thread only).
ResultsTracker& Tracker();

// Frame hook: stores this frame's reading (first runs and rollback re-runs).
void ReadResultsFrame(const Core::CPUThreadGuard& guard, int frame);

// Whether the running game's results can be read as above: Brawl rev 2 and Project+.
bool ResultsVerified();

// A Brawl host presets matchmade rules: 3 stocks, 8 minutes, items off. Written at the next frame
// hook while the host is still alone, before any keyframe, so the joiner loads them with the
// host's state. Project+ keeps its own rules.
void RequestQueueRules();
void ApplyQueueRules(const Core::CPUThreadGuard& guard, bool alone);
// Writes the rules (exposed for tests). Returns how many bytes changed.
int WriteQueueRules(GuestMemory& memory);
}  // namespace Orca::UX
