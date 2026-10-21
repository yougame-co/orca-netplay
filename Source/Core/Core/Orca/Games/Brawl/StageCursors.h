// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/Display.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "InputCommon/GCPadStatus.h"

// Two cursors on a matchmade stage select, one per player. The game's stage select has one cursor
// driven by one port. While these cursors run, the stage flows (BrawlStages.h, RankedPPlus.h) take
// every pad away from it and drive it themselves, so the game only sees what the flows decide.
//
// Each cursor is synced state in the match block, in fixed point (1/16 of the game's cursor units,
// integer math only). The input gate's latch writes it from ports 1-2's raw pads as a pure function
// of memory and those pads, on every pass over a frame (first runs, rollback re-runs, a joiner's
// first frame), so every machine reads the same cursors. Presses are edges against the last seen
// buttons, also in the block, so re-running a frame changes nothing. The overlay draws the cursors
// from what the frame hook publishes on first runs.
//
// Match block bytes (MatchBlock.h):
//   +0xF4..+0xFF   shared: tag (on), game, which cursor the native one follows, page turn
//   +0x1B4..+0x1BF two cursors, 6 bytes each: s16 x, s16 y, u8 buttons now, u8 buttons seen
namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::StageCursors
{
constexpr u32 SHARED = MatchBlock::STAGE_CURSORS_SHARED;
constexpr u32 SHARED_SIZE = MatchBlock::STAGE_CURSORS_SHARED_SIZE;
constexpr u32 CURSORS = MatchBlock::STAGE_CURSORS;
constexpr u32 CURSOR_SIZE = 6;
constexpr u32 CURSORS_SIZE = 2 * CURSOR_SIZE;
static_assert(CURSORS_SIZE == MatchBlock::STAGE_CURSORS_SIZE);
// Tag value while the two cursors run (anything else: off).
constexpr u8 TAG_ON = 0xC2;

// The buttons the flows read, one bit each.
enum Button : u8
{
  BTN_A = 0x01,
  BTN_B = 0x02,
  BTN_X = 0x04,
  BTN_Y = 0x08,
  BTN_START = 0x10,
  BTN_Z = 0x20,
};
u8 ButtonsOf(u16 pad_buttons);

// MatchBlock's ruleset ids.
constexpr u8 GAME_BRAWL = 1;
constexpr u8 GAME_PPLUS = 2;

// Rect, Tile, Point and Layout (a layout's geometry, what the cursors move over): Engine/Display.h,
// which the overlay draws from.
using Orca::Engine::Cursors::Layout;
using Orca::Engine::Cursors::Point;
using Orca::Engine::Cursors::Rect;
using Orca::Engine::Cursors::Tile;

const Layout& BrawlLayout();
const Layout& PPlusLayout();
const Layout* LayoutFor(u8 game);

// What a cursor is on: a legal stage's index, PAGE_BUTTON, RANDOM_BUTTON, or NOTHING.
constexpr int NOTHING = -1;
constexpr int PAGE_BUTTON = -2;
constexpr int RANDOM_BUTTON = -3;
int Hovered(const Layout& layout, u32 page, int x, int y);
// The tile of `stage` on `page` (nullptr: not on that page).
const Tile* TileOf(const Layout& layout, int stage, u32 page);
// The middle of a rectangle in the game's cursor units (not x 16).
float MidX(const Rect& r);
float MidY(const Rect& r);

struct Cursor
{
  s16 x = 0;
  s16 y = 0;
  u8 now = 0;   // buttons of the frame that just ran
  u8 seen = 0;  // the buttons the frame hook last saw
  bool operator==(const Cursor&) const = default;
};

struct State
{
  u8 game = 0;
  // The port the game's own cursor follows (the last to move), so its hover shows the preview.
  u8 follower = 0;
  // Page turn requested: 0 none, else 1 + port. Frames since then (it times out).
  u8 page_turn = 0;
  u8 page_frames = 0;
  std::array<Cursor, 2> cursors{};
  bool operator==(const State&) const = default;
};

// The two cursors, or nullopt while the tag is off.
std::optional<State> Read(const GuestMemory& memory);
// Writes `state` with the tag on, only the bytes that differ. Returns bytes changed.
int Write(GuestMemory& memory, const State& state);
// Turns the tag off if it was on. Returns bytes changed.
int Clear(GuestMemory& memory);
// A fresh pair of cursors at the layout's start points, which are on nothing, so a mashed A names
// nothing. Every button starts as held (`now` and `seen` HELD_ALL), so a button held through the
// screen change only counts once it has been let go: an A from the character select never names
// the stage or Random a cursor is moved onto.
constexpr u8 HELD_ALL = 0xFF;
State Start(const Layout& layout);

// Presses `port` made since the frame hook last looked (`now` against `seen`). Marks them seen.
u8 TakePresses(State* state, int port);

// While the tag is on, ports 1-2's raw sticks move their cursors (integer math, clamped to the
// bounds) and record the frame's buttons. Returns bytes changed.
int Latch(GuestMemory& memory, const std::array<std::optional<GCPadStatus>, 4>& raw);
// The CPU thread's latch (InputGate.h).
void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw);

// ---- Overlay view (UI only, published by the stage flows on first runs) ----
// View: Engine/Display.h (the overlay draws it).
using Orca::Engine::Cursors::View;

// TurnCue: Engine/Display.h.
using Orca::Engine::Cursors::TurnCue;
// The turn tag for `port`'s cursor (0 or 1), from this machine's point of view.
TurnCue TurnText(const View& view, int port);
// The banner line under "META KNIGHT CLAUSE" ("BO PICKS ANY STAGE · NO STRIKES", or "YOU PICK ..."
// on the picker's screen). Empty when the clause doesn't apply or the stage is picked.
std::string MkClauseNote(const View& view);
// Publish runs on every first-run frame. Current returns the view from the frame before the last,
// which is the frame on screen.
void Publish(View view);
View Current();
}  // namespace Orca::UX::StageCursors
