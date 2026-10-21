// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>

#include "Common/CommonTypes.h"

// What a game module shows (Orca split, S2 commit 6): one struct the overlay asks for when it draws
// a frame, instead of reading each Brawl module's published state. Host-only: nothing here reaches
// emulated memory or a peer.
namespace Orca::Engine
{
// The two stage select cursors (Brawl: StageCursors.h, which keeps these names).
namespace Cursors
{
// In cursor units x 16: x0 <= x < x1, y0 <= y < y1 (up is +y).
struct Rect
{
  s16 x0, y0, x1, y1;
  constexpr bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
// A legal stage's tile: ruleset stage index (RankedSteps.h), page, and rectangle.
struct Tile
{
  s8 stage;
  u8 page;
  Rect rect;
};
struct Point
{
  s16 x, y;
};

struct Layout
{
  u8 game = 0;
  // The cursor's range (the game's own cursor never leaves it), x 16.
  Rect bounds{};
  // Movement per frame at full tilt (x 16), and the stick dead zone (of 128).
  s16 speed = 16;
  s16 dead_zone = 24;
  std::span<const Tile> tiles;
  // Brawl's Melee Stages / Brawl Stages page button. Without pages it is never hovered.
  bool pages = false;
  Rect page_button{};
  // The game's own Random button (hovered item 0x36), on every page. An empty rectangle is never
  // hovered.
  Rect random_button{};
  // Overlay only, never synced: the Random button as drawn, for the frame around a Random pick (the
  // hit rectangle above runs past it).
  Rect random_art{};
  // Where the two cursors start: on nothing (Hovered reads NOTHING), not a tile, Random or the page
  // button.
  std::array<Point, 2> start{};
  // Overlay only, never synced: where cursor (0, 0) sits in the picture (0..1 across and down),
  // and how far one cursor unit moves across and down.
  float u0 = 0.5f, du = 0.01f, v0 = 0.5f, dv = -0.01f;
};

// ---- Overlay view (UI only, published by the stage flows on first runs) ----
struct View
{
  bool on = false;
  u8 game = 0;
  u32 page = 0;
  struct Player
  {
    bool present = false;
    s16 x = 0, y = 0;
    s8 proposal = -1;  // proposed stage (ruleset index), -1 none, -2 Random (Ranked::PREF_*)
    bool local = false;
    std::string name;
  };
  std::array<Player, 2> players;
  s8 picked = -1;    // the stage the flow took (-1: not yet)
  s8 turn = -1;      // whose turn it is (-1: nobody's or both)
  // The current step: kind (Ranked::StepKind, 0 none), strikes or bans still owed, whether a ban
  // is optional, Brawl's Meta Knight clause (one pick of any legal stage), a step for both players
  // at once (a casual pick), and the timer's seconds (-1: none).
  u8 kind = 0;
  u8 left = 0;
  bool optional = false;
  bool mk_clause = false;
  bool both = false;
  s16 seconds = -1;
  bool operator==(const View&) const = default;
};

// The turn tag drawn by a player's cursor (UI only).
struct TurnCue
{
  enum class Mode : u8
  {
    Idle,     // nothing to say: no tag, no blink
    Acting,   // this player has to act: the cursor blinks, the tag says what to do
    Waiting,  // the local player waits for the other: a small grey tab, no blink
  };
  Mode mode = Mode::Idle;
  bool local = false;  // the cursor belongs to this machine's player
  std::string title;   // "YOUR TURN", "BO'S TURN", "WAITING FOR BO"
  std::string detail;  // "X STRIKES A STAGE · 2 LEFT" (may be empty)
  int seconds = -1;    // the turn's timer (-1: none)
  bool operator==(const TurnCue&) const = default;
};
}  // namespace Cursors

// A ranked set's game dots (Brawl: RankedSet.h Dots): each game's winner (-1 not played), the one
// being played.
struct SetDots
{
  bool on = false;
  std::array<int, 3> games{-1, -1, -1};
  int current = -1;
  bool operator==(const SetDots&) const = default;
};

struct Display
{
  // NativeText: which name plates (bit p) and whether the rules bar show Orca's words, so the
  // overlay leaves those out.
  u8 plates = 0;
  bool rules_bar = false;
  // Relabel: READY TO FIGHT! relabelled with a turn timer on it, STAGE SELECT relabelled with the
  // turn, Project+'s stage select legend relabelled.
  bool band_clock = false;
  bool line_relabelled = false;
  bool legend = false;
  // The queue's one-time tip for the control guide (empty: none).
  std::string tip;
  // A ranked set's lines: Project+'s stage flow (set, turn), else the set score.
  std::string ranked_set;
  std::string ranked_turn;
  std::string score_line;
  // The stage select cursors, with what is drawn from them: the layout (nullptr: none for this
  // game), each player's proposed tile (nullptr: none), each cursor's turn tag, and the Meta Knight
  // clause banner line.
  Cursors::View cursors;
  const Cursors::Layout* layout = nullptr;
  std::array<const Cursors::Rect*, 2> proposals{};
  std::array<Cursors::TurnCue, 2> cues{};
  std::string mk_note;
  // The character order's picker (-1: none) and the ribbon on its portrait.
  int picker = -1;
  std::string picker_ribbon;
  SetDots dots;
};
}  // namespace Orca::Engine
