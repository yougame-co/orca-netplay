// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// No CPUs online: an online character select shows only the players who are there, and no CPU
// reaches an online fight. See ORCA.md, "No CPUs online".
//
// The Versus character select builds its panels from a record the game keeps between visits
// (gmSelCharData) and saves them back as it goes on to the stage select. Brawl resets the record
// when Versus starts from the main menu; Project+ doesn't, so a CPU from a local game could come
// back on an online select and play in the match.
//
// A select is online (Online()) when the header locks, when the main menu's pick is With Friends,
// Casual or Ranked, or when two ports or more are plugged in. Offline modes keep their CPUs.
//
// While online, every frame boundary (first runs and re-runs) writes only the bytes that differ, so
// this stays a pure, idempotent function of emulated memory and the synced ports:
// - ClearRecord, between two scenes only: a CPU's state, and a human's on a port nobody plugged
//   in, become nobody's. This lands before the character select builds its panels (5 boundaries
//   before in Project+, 33 in Brawl), and never on the stage select or in a fight, so no fight
//   loses a player.
// - ClearPanels, on the character select: a CPU's panel becomes empty, so the select saves no CPU.
//   It keeps its CPU plate until ApplySeats hides it or a player joins it. A human's panel on an
//   unplugged port is left to the game, which empties it 17 frames after the controller goes;
//   emptying it first would leave the token's highlight on the grid.
// - GateMasks, without a header: A never reaches a player-type button, another panel's name
//   button, or any place within one frame's travel of them. The player's own name button and its
//   list take A as on the queue's own select (Rules::CssNameTakesA). A panel a CPU had is joined by
//   Orca pressing A for its port while that port's hand rests on the panel's player-type button
//   (PressesJoin).
// - ApplySeats, on the character select: an empty panel whose port nobody plugged in is hidden.
//   Hiding only stops drawing; the panel's logic and buttons stay. A panel that should show gets
//   Orca's hide bits cleared on any character select.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::OnlineSeats
{
// ---- The menu's picks (sqVsMelee +0x18, RSBE01.patches) ----
constexpr u32 PICK_FRIENDS_FIRST = 24;
constexpr u32 PICK_FRIENDS_LAST = 27;
constexpr u32 PICK_CASUAL = 30;
constexpr u32 PICK_RANKED = 31;

// ---- gmSelCharData's players (both games) ----
constexpr u32 GLOBAL_SEL_CHAR = 0x10;  // [0x805A00E0] + 0x10: gmSelCharData*
constexpr u32 RECORD_PLAYERS = 0xB8;
constexpr u32 RECORD_PLAYER_SIZE = 0x5C;
constexpr u32 RECORD_STATE = 0x01;
constexpr u8 STATE_HUMAN = 0;
constexpr u8 STATE_CPU = 1;
constexpr u8 STATE_NOBODY = 3;

// ---- The character select's player areas (muSelCharPlayerArea, task +0x44 + 4 x port) ----
constexpr u32 AREA_KIND_NONE = 0;
constexpr u32 AREA_KIND_HUMAN = 1;
constexpr u32 AREA_KIND_CPU = 2;
constexpr u32 AREA_MODELS = 0xB0;  // 40 MuObject pointers (one slot unused)
constexpr u32 AREA_MODEL_COUNT = 40;
constexpr u32 MUOBJECT_SCENE_MODEL = 0x0C;  // nw4r::g3d::ScnMdl*
constexpr u32 SCENE_MODEL_FLAGS = 0xCC;     // nw4r::g3d::ScnObj's option flags
constexpr u32 MODEL_FLAGS_GAME = 0xA0000000;  // every panel model, both games
constexpr u32 MODEL_FLAGS_HIDDEN = 0x00000060;  // DISABLE_DRAW_OPA | DISABLE_DRAW_XLU

// ---- The buttons (both games) ----
// The player-type and name buttons' top edges lie between y -17.4 and -18.3, and the hand moves at
// most 1.0 a frame (0.83 in Brawl), so from -16.4 up no single frame takes the hand onto one with A
// down. Below -16.4, A loses only the bottom edge of the panel's costume picture.
constexpr float TYPE_BUTTONS_A_FLOOR = -16.4f;
constexpr u32 CSS_BUTTON_NAME = Rules::CSS_BUTTON_NAME;

// ---- Pure decisions ----
// Whether a select is online: a header that locks, an online pick, or two ports or more plugged in.
bool Online(const Rules::Header& header, u32 pick, int players);
// A port's record state while online: a CPU, or a human on a port nobody plugged in, becomes
// nobody; anything else stays.
constexpr u8 RecordStateFor(u8 state, bool plugged)
{
  return state == STATE_CPU || (state == STATE_HUMAN && !plugged) ? STATE_NOBODY : state;
}
// A panel's kind while online: a CPU's panel becomes empty; a human's stays, plugged in or not.
constexpr u32 PanelKindFor(u32 kind)
{
  return kind == AREA_KIND_CPU ? AREA_KIND_NONE : kind;
}
// Whether port `port`'s (0-3) panel is hidden: online, empty, nobody plugged in, and not the
// opponent's seat on a queue select (port 2, always shown in a queue room and on the queue's own
// select while it searches).
bool SeatHidden(bool online, const Rules::Header& header, int port, u32 kind, bool plugged,
                bool searching);
// Whether A may reach the game from this hand on an online select without a header, apart from the
// player's own name button (Rules::CssNameTakesA). An unreadable hand may not, as with
// Rules::CssHandMayPressA.
bool HandMayPressA(const Rules::CssHand& hand);
// Whether Orca presses A for plugged-in port `port` (0-3) on an online select without a header:
// its panel is empty (`kind` 0) with its token neither in the hand nor flying (a panel a CPU had;
// a fresh empty panel holds its token in the hand), and its hand rests on that panel's player-type
// button. The game then makes the panel the player's, as when a local player replaces a CPU.
bool PressesJoin(const Rules::CssHand& hand, int port, u32 kind, bool token_in_hand,
                 bool token_flying);

// ---- Memory ----
// The ports plugged in (bit n: port n+1) as game memory has them for the input gate: MatchBlock
// FRIENDS_SEEN once a friend has plugged in, else port 1 alone.
u8 PluggedInMemory(const GuestMemory& memory);
// Online() for this memory and these ports.
bool ReadOnline(const GuestMemory& memory, u8 plugged);
// The record's clear (between two scenes, online only). Returns the bytes changed.
int ClearRecord(GuestMemory& memory, u8 plugged);
// The panels' clear (the character select, online only). Returns the bytes changed.
int ClearPanels(GuestMemory& memory, u8 plugged);
// The seats (any character select). Returns the flag words written; `hidden` gets the hidden
// panels (bit p).
int ApplySeats(GuestMemory& memory, u8 plugged, u8* hidden = nullptr);
// What the input gate masks for the frame about to run (read only).
Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

// ---- Frame hook steps (UX.cpp), on first runs and re-runs, Brawl rev 2 and Project+ ----
// `plugged`: the session's ports for the frame about to run (bit n: port n+1), as the online rules
// take them.
// Runs after the online rules, so this boundary's header is in memory: the record and the panels.
void ClearFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged);
// Runs after the queue, whose ready flag says whether the queue's own select searches: the seats.
void SeatsFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged);
}  // namespace Orca::UX::OnlineSeats
