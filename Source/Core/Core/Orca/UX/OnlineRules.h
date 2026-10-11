// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/SetBlock.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// Online rules for casual and ranked queue matches. The host writes a header (mode, ruleset, coin,
// room hash) into the match block while its game is not yet shared, and the joiner checks it after
// loading the keyframe. While the header locks, every frame enforces the ruleset's rules, the
// stage choice, the legal stage list, Project+'s Code Menu and fighters, and the input gate's
// masks.
// Everything except writing the header is a pure function of emulated memory, so it runs the same
// on both machines and on rollback re-runs. Friends rooms have no header and only get SessionMasks.
// See ORCA.md, "Online rules".
namespace Orca::UX
{
class GuestMemory;

namespace Rules
{
using MatchBlock::Mode;
using MatchBlock::Ruleset;

struct Header
{
  bool present = false;  // magic and version match
  Mode mode = Mode::None;
  Ruleset ruleset = Ruleset::None;
  u8 flags = 0;  // MatchBlock::FLAG_SOLO, FLAG_QUEUE2
  u8 coin = 0;   // game 1's first striker
  u32 room = 0;  // hash of the room code
  bool Locked() const { return present && mode != Mode::None && ruleset != Ruleset::None; }
  // The queue's own character select, before a match is found.
  bool Solo() const { return Locked() && (flags & MatchBlock::FLAG_SOLO) != 0; }
  // The Slippi-style character select with ready flags and a timer.
  bool Queue2() const { return Locked() && (flags & MatchBlock::FLAG_QUEUE2) != 0; }
  bool operator==(const Header&) const = default;
};
Header ReadHeader(const GuestMemory& memory);

// The ruleset the running profile plays: Brawl rev 2's, Project+'s, or none (no locks).
Ruleset ProfileRuleset();

// ---- The header ----
// Writes the header, or clears it for Mode::None and restores the player's own rules saved when it
// was first written. Any new header resets every track's set state. Returns the bytes changed.
int WriteHeader(GuestMemory& memory, Mode mode, Ruleset ruleset, u8 coin = 0, u32 room = 0,
                u8 flags = 0);

// The header a game in a room of `mode` should carry. A queue room's `code` gives the coin and room
// hash. With FLAG_SOLO there is no room, so no coin and no hash.
Header HeaderFor(Mode mode, Ruleset ruleset, std::string_view code, u8 flags);
// Whether `in_memory` matches `wanted` in every field. When none is wanted, any non-locking
// header matches.
bool HeaderIs(const Header& in_memory, const Header& wanted);

// ---- The locks, every frame ----
// `frame` drives the ranked ready timer. Returns the bytes changed (0 without a locking header).
int ApplyLocks(GuestMemory& memory, int frame);

// What the input gate masks for the frame about to run (read only). In Project+, D-pad Down never
// reaches the game while L and R are held, in every scene: the Code Menu's chord and the debug
// codes' own.
Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

// ---- Every session, with or without a header ----
// Masks Z on the results screen for every port. Z saves a replay, which no session can do (no save
// file on the NAND, SD absent or read-only), and the save dialog it opens can only be left with B,
// so players got stuck there. See ORCA.md, "The results screen's replay".
Rollback::InputGate::Masks SessionMasks(const GuestMemory& memory);

// ---- The character select's hands ----
// Each player area (character select task +0x44 + 4 * port) has its hand at +0x1A8. The hand
// holds a target kind (+0x80), a button id (+0xAC) and a position (+0x90 x, +0x94 y, y up). The
// grid spans about y -5 to 16.
//
// A acts on whatever the hand points at after this frame's stick movement. So while the locks hold,
// A passes only when the hand is in the grid or on its own token and at least one frame's travel
// (0.83 in Brawl, 1.0 in Project+) inside the grid's edges. That keeps A off every other button
// (player type, name, costume, teams, rules, BACK). X and Y still change costumes.
constexpr u32 CSS_HAND_NOTHING = 0;
constexpr u32 CSS_HAND_BUTTON = 1;  // button id at +0xAC: 0x1D player type, 0x1C name, ...
constexpr u32 CSS_HAND_GRID = 2;    // over the grid, token placed
constexpr u32 CSS_HAND_OWN_TOKEN = 3;  // A picks it up
constexpr u32 CSS_HAND_EXIT = 4;       // BACK (button 2) or Rules (4)
constexpr u32 CSS_HAND_TAKING = 6;     // token moving into the hand
constexpr u32 CSS_HAND_GRID_HOLDING = 7;  // over the grid, token in hand (A places it)
constexpr u32 CSS_HAND_PLACING = 8;       // token moving down
constexpr u32 CSS_BUTTON_PLAYER_TYPE = 0x1D;
constexpr u32 CSS_BUTTON_NAME = 0x1C;
constexpr u32 CSS_BUTTON_BACK = 0x02;  // with CSS_HAND_EXIT (Rules is 0x04)
// A passes only strictly between these y bounds. Brawl's bottom row reaches down to -4.07 and the
// player's panel (a button) starts near -5.8, so -4.2 stays one frame's travel inside the grid
// while still covering the whole bottom row, including RANDOM.
constexpr float CSS_A_TOP = 15.0f;
constexpr float CSS_A_BOTTOM = -3.0f;
constexpr float CSS_A_BOTTOM_BRAWL = -4.2f;

struct CssHand
{
  bool valid = false;  // the hand could be read
  u32 target = CSS_HAND_NOTHING;
  u32 button = 0;
  float x = 0;
  float y = 0;
  // The panel (0-3) whose button the hand points at (+0xB0; found under the harness, both games:
  // port 1's hand over panel 2's player-type button reads 1, port 2's over its own 1), or -1 where
  // it can't be read.
  int panel = -1;
  bool operator==(const CssHand&) const = default;
};
// Port `port`'s (0-3) player area on the character select (muSelCharPlayerArea, at the select
// task's +0x44 + 4 x port), or 0 anywhere else or before the task is built. Callers check the
// fields they read.
u32 ReadCssArea(const GuestMemory& memory, int port);
// Port `port`'s (0-3) hand on the character select; invalid anywhere else.
CssHand ReadCssHand(const GuestMemory& memory, int port);
// Whether A may reach the game from this hand while the locks hold. `bottom` is the game's lower
// edge. An unreadable hand may not.
bool CssHandMayPressA(const CssHand& hand, float bottom = CSS_A_BOTTOM);

// ---- A on BACK, queue's own character select only ----
// Wherever B could back out, A on the BACK button should too. On the queue's own character select
// (FLAG_SOLO), port 1's A reaches the game when the hand is on BACK and A is a new press; a held A
// sliding onto BACK never backs out. If the player is searching, Queue.h first stops the search and
// passes A the next frame. Never in a queue room, never on Rules. Pure.
constexpr bool CssHandOnBack(u32 target, u32 button)
{
  return target == CSS_HAND_EXIT && button == CSS_BUTTON_BACK;
}
constexpr bool CssBackTakesA(const CssHand& hand, bool a_held)
{
  return hand.valid && CssHandOnBack(hand.target, hand.button) && !a_held;
}

// ---- A on the player's own name button: their tag and its controls ----
// A on a panel's name button opens its list of tags. While it is open, the player area's +0x200
// is nonzero (1 shown, 2 and 3 scrolling) and the hand reads target 1 with button 0 where the name
// button is (both games, probed under the harness). A pick sets the area's tag (+0x1C8).
// The name button sits beside the player-type button, so A reaches it only from the hand's own
// panel with the stick centred while A is down: the hand can't slide onto the player-type button
// with the press. L is kept from an open list: Project+'s hold-L on a new name leaves the select
// for its controls scene. Pure.
constexpr u32 CSS_AREA_NAME_LIST = 0x200;
constexpr bool CssNameTakesA(const CssHand& hand, int port, bool list_open)
{
  return list_open || (hand.valid && hand.target == CSS_HAND_BUTTON &&
                       hand.button == CSS_BUTTON_NAME && hand.panel == port);
}
// Whether port `port`'s (0-3) name list is open; false anywhere but the character select.
bool ReadCssNameList(const GuestMemory& memory, int port);

// ---- B on the character select: only to pick the token back up ----
// B on a placed token undoes the pick, but B held with the token in hand backs out to the menus.
// So while the locks hold, B reaches the game only on the first frame of a press and only while
// that player's token is down. "New press" comes from the queue's raw-button latch in emulated
// memory, never from host state. Queue.h and CharOrder.h may mask B further.
struct CssToken
{
  bool valid = false;  // the player's area could be read
  bool human = false;
  int character = 0x28;  // under the hand while held, else the token's (0x28 = none)
  bool in_hand = true;   // +0x1F8
  bool flying = false;   // +0x1F9: moving up or down
  // Placed on a character and still: B picks it up.
  constexpr bool Down() const { return valid && human && character != 0x28 && !in_hand && !flying; }
  bool operator==(const CssToken&) const = default;
};
// Port `port`'s (0-3) token on the character select; invalid anywhere else.
CssToken ReadCssToken(const GuestMemory& memory, int port);
// Whether B may reach the game, given the token and whether B was held last frame. Pure.
constexpr bool CssBUnpicks(const CssToken& token, bool b_held)
{
  return token.Down() && !b_held;
}
// The same from memory, with B from the latch (ports 1 and 2 only; 3 and 4 never play a queue
// match).
bool CssBMayUnpick(const GuestMemory& memory, int port);

// Brawl's Random Stage Switch masks for the legal stages: the Brawl page (record menu data +0x24)
// and the Melee page (+0x20).
constexpr u32 BRAWL_LEGAL_BRAWL_PAGE = 0x04005007;  // BF, FD, Delfino, YI, Lylat, Smashville
constexpr u32 BRAWL_LEGAL_MELEE_PAGE = 0x00020000;  // Pokemon Stadium
constexpr u32 BRAWL_FINAL_DESTINATION_BIT = 0x00000002;

// gmCharacterKind values. The character select writes each pick into the match's init data.
constexpr int CHARACTER_ICE_CLIMBERS = 0x10;
constexpr int CHARACTER_NONE = 0x3E;
// Project+'s banned fighters (its character select makes them from a Bowser or Wario pick with L
// held as it ends) and the fighters the locks play instead.
constexpr int CHARACTER_BOWSER = 0x0C;
constexpr int CHARACTER_WARIO = 0x17;
constexpr int CHARACTER_GIGA_BOWSER = 0x2C;
constexpr int CHARACTER_WARIO_MAN = 0x2D;

// ---- Project+'s Code Menu ----
// Project+'s netplay codeset loads its Code Menu (pf/menu3/dnet.cmnu) verbatim at CODE_MENU. Any
// port opens it with L+R+Down in any scene, mid-fight too, or with the main menu's button. Open, it
// pauses both games; from it a player can turn on flight, infinite shield and debug displays, swap
// any port's character and change gameplay constants, all in sync. Under a locking header the
// locks turn it off, close it and hold every cheat and gameplay value at the file's default. The
// Character Select lines follow the fighters and are left alone. Friends rooms keep the menu.
constexpr u32 CODE_MENU = 0x804E0000;
constexpr u32 CODE_MENU_END = 0x804E2520;
// The root page, the first after the file's header. The menu checks the word at CODE_MENU + 4.
constexpr u32 CODE_MENU_ROOT = 0x804E07D8;
// u32: 0 closed, 1 the main menu's button (A or Start opens), 2 opening, 3 closing, 4 open.
constexpr u32 CODE_MENU_STATE = 0x804E0034;
constexpr u32 CODE_MENU_OPEN = 4;
// u32: the game's pause word as the menu opened. Open, the menu writes 1 there every frame.
constexpr u32 CODE_MENU_SAVED_PAUSE = 0x804E006C;
constexpr u32 CODE_MENU_GAME_PAUSE = 0x805B8A08;
// u32 Code Menu Activation: 0 Default (any scene), 1 PM 3.6 (not mid-fight), 2 OFF.
constexpr u32 CODE_MENU_ACTIVATION = 0x804E0C38;
constexpr u32 CODE_MENU_OFF = 2;
// P1-P4's Character Select values.
constexpr std::array<u32, 4> CODE_MENU_CHARACTERS{0x804E0F48, 0x804E1424, 0x804E1688, 0x804E18EC};
struct CodeMenuWord
{
  u32 address;
  u32 value;
};
// The words the locks hold: dnet.cmnu's defaults for every cheat and gameplay line, activation OFF.
std::span<const CodeMenuWord> CodeMenuDefaults();
// Whether memory holds the pinned release's menu, by its layout. Nothing is written without it.
bool CodeMenuPresent(const GuestMemory& memory);

// One port's pick, from the match's init data (gmGlobalModeMelee +0x98 + port * 0x5C).
struct PlayerPick
{
  bool human = false;  // type 0, not a CPU or empty
  int character = -1;  // gmCharacterKind, -1 none
};
std::array<PlayerPick, 4> ReadPicks(const GuestMemory& memory);

// Ranked character select ready limit, in frames (60 s).
constexpr int CSS_READY_FRAMES = 60 * 60;

// ---- Session side (Rollback/OnlineMatch.cpp, CPU thread) ----
// Sets the header this machine's room calls for, every frame before the frame hook and again before
// a keyframe is made. Returns whether it changed (WantedGeneration then increments).
bool SetWanted(Mode mode, u8 flags = 0, std::string_view code = {});
Mode Wanted();
u8 WantedFlags();
// Counts changes to the wanted header, so a keyframe made under an older count is stale.
u64 WantedGeneration();
// Whether memory carried the wanted header at the last first-run frame hook, with no change since.
// A host offers no keyframe for a queue room until it does.
bool HeaderInPlace();
// Memory was replaced after this frame's hook, so the header read is stale until the next one.
void MemoryReplaced();
// Lets this frame's hook write the header even though a peer is joining and waiting for a keyframe.
// Affects only the header; the rest of the UX still uses `alone`.
void SetHeaderFree(bool free);
// A joiner just loaded its keyframe into a room of `mode` (None: friends room). The host's header
// is checked at the next frame hook. `known_games` (a ranked room): the games of the room's match
// this player has played or heard the verdict of (Online::KnownSetGames); -1: the set isn't
// checked.
void ExpectHeader(Mode mode, std::string_view code = {}, u8 flags = 0, int known_games = -1);
// Whether a joiner accepts the host's header `h` (pure).
bool HeaderFitsRoom(const Header& h, Mode mode, Ruleset ruleset, std::string_view code, u8 flags);
// Whether a joiner accepts the set the host's game holds (pure): no more decided games than the
// `known_games` this player knows of in the room's match, and with none known, a set not begun.
// A host's history can't hand anyone games nobody played in the room.
bool SetFitsJoin(const SetBlock::SetState& set, int known_games);
// "mismatch" once the check failed; the joiner leaves.
std::optional<std::string> TakeMismatch();
// The ranked no-show port (0-based), once frames through it are `confirmed` final.
std::optional<int> ConfirmNoShow(int confirmed);

// ---- Frame hook (UX.cpp) ----
// Writes the header (first runs only, when alone or SetHeaderFree allows), then applies the locks
// (first runs and re-runs). `plugged` is the synced set of ports in play, used by the ready timer.
void OnFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, bool alone,
             u8 plugged);
// Test override ORCA_TEST_QUEUE=casual|ranked[:<coin 0|1>][:q2|:solo] makes this game act as the
// host of such a queue room. `q2` sets FLAG_QUEUE2; `solo` sets FLAG_SOLO | FLAG_QUEUE2.
struct TestQueue
{
  Mode mode = Mode::None;
  u8 coin = 0;
  u8 flags = 0;
  bool operator==(const TestQueue&) const = default;
};
std::optional<TestQueue> ParseTestQueue(const std::string& spec);
std::optional<TestQueue> TestQueueSpec();
std::optional<Mode> TestQueueMode();
}  // namespace Rules
}  // namespace Orca::UX
