// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"

// Types the session and a game module share (Orca split, S2 commit 5): what the module reports and
// the session acts on. They were the Brawl modules' own types; the modules keep using-declarations.
namespace Orca::Engine
{
// The match header's mode, ruleset and queue flags (the header bytes the session wants in memory;
// Brawl: MatchBlock MODE, RULESET, FLAGS, whose values these are).
enum class Mode : u8
{
  None = 0,
  Casual = 1,
  Ranked = 2,
};

enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,  // Supernova 2025: 3 stocks, 8:00, BF PS1 LC SV YI + FD DP
  PPlus = 2,  // Project+ 2024 recommended procedure and proposed stage list
};

// FLAG_SOLO: on the queue's character select alone, before a match. FLAG_QUEUE2: Slippi-style
// character select (ready with Start, 30 s timer).
constexpr u8 FLAG_SOLO = 0x01;
constexpr u8 FLAG_QUEUE2 = 0x02;

// What one frame shows, for the session's origin and a fresh start's tail (Brawl: OnlineMenu.h
// FreshMove::Seen).
struct FreshSeen
{
  // The main menu runs with its pages built and no exit chosen (it can be left).
  bool menu_built = false;
  // A character select is up: its task and panels can be read.
  bool css = false;
  bool operator==(const FreshSeen&) const = default;
};

// Where a frame's game is, as the results tracker reads it (Brawl: Results.h Reading::Scene).
enum class SceneKind : u8
{
  Other,
  Fight,
  Between,
  Results,
};

// One game's result, as both players report it.
struct GameResult
{
  enum class Kind
  {
    Win,
    Draw,
    Void,
  };
  Kind kind = Kind::Void;
  // Win: the winning port (0-based; seat s plays port s).
  int winner_port = -1;
  // Void: why ("nocontest", "ports").
  std::string why;
  // Win and Draw: first results-scene frame, stage, ports 1 and 2's characters and stocks left
  // (-1 timed), and whether time ran out.
  int frame = -1;
  int stage = -1;
  std::array<int, 2> characters{-1, -1};
  std::array<int, 2> stocks{-1, -1};
  bool timeout = false;
  // Ranked set games only (from the match block): the fight's first frame (the report id, same on
  // both machines), game number, how it was decided (SetBlock::How), tiebreak, each port's ledge
  // grabs, and for the deciding game the set's end (1 won by `set_winner`, 2 void).
  int start = -1;
  int number = 0;
  int how = 0;
  bool tiebreak = false;
  std::array<int, 2> ledge{-1, -1};
  int set_done = 0;
  int set_winner = -1;
  bool operator==(const GameResult&) const = default;

  // {"f":F,"st":S,"c":[c1,c2],"s":[s1,s2],"to":0|1}: integers only, port order, this key order. A
  // ranked set game adds "k":how,"lg":[l1,l2],"tb":0|1 (F is then the fight's first frame).
  std::string DetailJson() const;
};
}  // namespace Orca::Engine
