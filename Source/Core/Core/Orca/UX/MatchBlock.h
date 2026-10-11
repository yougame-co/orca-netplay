// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/FreeSpace.h"

// The match block: a queue match's rules and set state, stored in the game's own memory at
// FreeSpace::kMatchBlock (same address in Brawl rev 2 and Project+). Living in MEM1 means rollback
// snapshots and drop-in keyframes carry it for free. Big-endian, like the game. See ORCA.md,
// "Online rules", "Ranked steps" and "Ranked sets".
//
// Until Orca writes it the block holds dead game code, so readers check the magic. Friends rooms
// and solo play never have a header.
//
// Writers:
// - The header and saved rules are written only by OnlineRules.h WriteHeader, and only while this
//   machine's game has no peer (Events.h `alone`). A new header resets all set state.
// - Everything else is game logic run by the frame hook every frame (including resimulation) as a
//   pure function of memory.
//
// Layout (offsets from BASE):
//   +0x00..+0x0F   header: magic, version, mode, ruleset, coin, room hash, queue flags
//   +0x10..+0x7F   Project+ set state (casual uses +0x10 fight count, +0x40 preferred stages)
//   +0x80..+0xBF   stage flow steps (Project+ State below; Brawl in BrawlStages.h)
//   +0xC0..+0xCF   ranked character select ready timer
//   +0xD0..+0xF3   the player's own rules, saved when the header is written (+0xDA: Project+'s
//                  Code Menu activation)
//   +0xF4..+0xFF   shared stage select cursors (StageCursors.h)
//   +0x100..+0x1A7 Project+ stage switch data, saved when the header is written
//   +0x1B4..+0x1BF stage select cursors (StageCursors.h)
//   +0x1C0..+0x1EF queue character select (Queue.h)
//   +0x1F0         saved-rules flags
//   +0x1F4..+0x1FF character order: where each hand last held its token (CharOrder.h)
//   +0x200..+0x20F character order (CharOrder.h)
//   +0x210..+0x28F ranked set, authoritative (SetBlock.h)
//   +0x290..+0x297 friend drop-in from the menus (OnlineMenu.h FriendsMove)
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::MatchBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;  // 0x806F2380
constexpr u32 SIZE = FreeSpace::kMatchBlock.Size();  // 512
// Including the free tail up to the next relocated word.
constexpr u32 FULL_SIZE = FreeSpace::kMatchBlockLimit - FreeSpace::kMatchBlock.begin;

// ---- Header, +0x00 ----
constexpr u32 MAGIC = BASE + 0x00;  // u32 'YGM1'
constexpr u32 MAGIC_VALUE = 0x59474D31;
constexpr u32 VERSION = BASE + 0x04;  // u8
constexpr u8 VERSION_VALUE = 1;
constexpr u32 MODE = BASE + 0x05;     // u8 Mode
constexpr u32 RULESET = BASE + 0x06;  // u8 Ruleset
// u8: game 1's first striker (port 0 or 1), derived from the server-made room code.
constexpr u32 COIN = BASE + 0x07;
// u32: room code hash, so a new room with the same mode and ruleset still starts a fresh set.
constexpr u32 ROOM = BASE + 0x08;
// u8: queue flags (Queue.h). FLAG_SOLO: on the queue's character select alone, before a match.
// FLAG_QUEUE2: Slippi-style character select (ready with Start, 30 s timer). A toggled patch group
// reads this word, so keep its address stable.
constexpr u32 FLAGS = BASE + 0x0C;
constexpr u8 FLAG_SOLO = 0x01;
constexpr u8 FLAG_QUEUE2 = 0x02;
// +0x0D..+0x0F: reserved (zero).
constexpr u32 HEADER_SIZE = 0x10;

// A toggled patch group reads the mode byte via the word at VERSION, so keep its address stable.
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

// ---- Set state, +0x10..+0x7F: Project+'s set (State below) ----
constexpr u32 SET = BASE + 0x10;
constexpr u32 SET_SIZE = 0x70;

// ---- Step state, +0x80..+0xBF: strikes, bans and picks ----
constexpr u32 STEP = BASE + 0x80;
constexpr u32 STEP_SIZE = 0x40;

// ---- Ranked character select timer, +0xC0..+0xCF ----
// u16: frames since the ready timer started (runs while one port has a character and the other
// doesn't); 0 when stopped.
constexpr u32 CSS_TIMER = BASE + 0xC0;
// u8: the port (1-4) that didn't ready in time, or 0. Cleared on leaving the character select.
constexpr u32 CSS_NO_SHOW = BASE + 0xC2;
// u8: bit 0 set once a fight has begun (this timer's no-shows only count before that; a later
// game's come from the character order, CharOrder.h NoShow).
constexpr u32 FOUGHT = BASE + 0xC3;
// u8: ports in play (bit n = port n+1), from the session's synced plan.
constexpr u32 PLUGGED = BASE + 0xC4;
// u32: frame number + 1 when the ready timer started (0 = not running). Basing the timer on the
// frame number keeps it identical however often a frame is resimulated.
constexpr u32 CSS_TIMER_START = BASE + 0xC8;
constexpr u32 PHASE1_SIZE = 0x10;

// ---- The player's own settings, saved when the header is written and restored on clear ----
constexpr u32 SAVED_RULES = BASE + 0xD0;   // 10 bytes, gmSetRule +0x02..+0x0B
// Project+ only: u8, its Code Menu Activation before the lock (OnlineRules.h).
constexpr u32 SAVED_CODE_MENU = BASE + 0xDA;
constexpr u32 SAVED_ITEMS = BASE + 0xE0;   // 3 words: menu data +0x00, +0x08, +0x0C
constexpr u32 SAVED_STAGES = BASE + 0xEC;  // 2 words: menu data +0x20, +0x24
// Project+ only: its stage switch data (RSS_EXDATA) before the lock.
constexpr u32 SAVED_RSS = BASE + 0x100;
constexpr u32 SAVED_RSS_SIZE = 0xA8;
// u8: bit 0 rules saved, bit 1 RSS saved, bit 2 Code Menu activation saved
constexpr u32 SAVED_FLAGS = BASE + 0x1F0;

// ---- Stage select cursors (StageCursors.h) ----
constexpr u32 STAGE_CURSORS_SHARED = BASE + 0xF4;
constexpr u32 STAGE_CURSORS_SHARED_SIZE = 0x0C;
constexpr u32 STAGE_CURSORS = BASE + 0x1B4;
constexpr u32 STAGE_CURSORS_SIZE = 0x0C;

// ---- Queue character select (Queue.h), +0x1C0..+0x1EF ----
constexpr u32 QUEUE = BASE + 0x1C0;
constexpr u32 QUEUE_SIZE = 0x30;

// ---- Past the first 512 bytes, up to kMatchBlockLimit ----
constexpr u32 CHAR_ORDER = BASE + 0x200;  // 16 bytes (CharOrder.h)
constexpr u32 CHAR_ORDER_SIZE = 0x10;
constexpr u32 RANKED_SET = BASE + 0x210;  // SetBlock.h (+0x210..+0x28F)

// ---- Friend drop-in from the menus (OnlineMenu.h FriendsMove), +0x290..+0x297 ----
// Unlike the rest of the block, these are used in every session, with or without a header.
constexpr u32 FRIENDS = BASE + 0x290;
// u8: FRIENDS_TAG_VALUE once initialized; anything else is still dead code.
constexpr u32 FRIENDS_TAG = FRIENDS + 0;
constexpr u8 FRIENDS_TAG_VALUE = 0xF5;
// u8: ports plugged in last frame (bit n = port n+1).
constexpr u32 FRIENDS_SEEN = FRIENDS + 1;
// u8: 1 while a newly joined friend waits for the host to reach the main menu, so both can go to
// the character select together.
constexpr u32 FRIENDS_PENDING = FRIENDS + 2;
// u32: frame number + 1 since the main menu has been ready with a friend pending (0 = not).
constexpr u32 FRIENDS_MENU_SINCE = FRIENDS + 4;
constexpr u32 FRIENDS_END = FRIENDS + 8;

static_assert(SAVED_RSS + SAVED_RSS_SIZE <= SAVED_FLAGS);
static_assert(SAVED_RULES + 10 <= SAVED_CODE_MENU && SAVED_CODE_MENU < SAVED_ITEMS);
static_assert(SAVED_STAGES + 8 <= STAGE_CURSORS_SHARED &&
              STAGE_CURSORS_SHARED + STAGE_CURSORS_SHARED_SIZE <= SAVED_RSS);
static_assert(SAVED_RSS + SAVED_RSS_SIZE <= STAGE_CURSORS &&
              STAGE_CURSORS + STAGE_CURSORS_SIZE <= QUEUE);
static_assert(RANKED_SET + 0x80 <= FRIENDS && FRIENDS_END <= FreeSpace::kMatchBlockLimit);
static_assert(FRIENDS_MENU_SINCE % 4 == 0);
static_assert(SAVED_RSS + SAVED_RSS_SIZE <= QUEUE && QUEUE + QUEUE_SIZE <= SAVED_FLAGS);
static_assert(SAVED_FLAGS < BASE + SIZE);
static_assert(CHAR_ORDER + CHAR_ORDER_SIZE <= RANKED_SET);
static_assert(CSS_TIMER_START + 4 <= BASE + 0xC0 + PHASE1_SIZE);

// ---- Project+ stage flow state (RankedPPlus.h), +0x00..+0x8F ----
//   header   +0x00 u32 magic "YGM1"   +0x04 u8 version   +0x05 u8 mode   +0x06 u8 ruleset
//            +0x07 u8 coin
//   set      +0x10 u8 games counted   +0x11 u8 port 1 wins   +0x12 u8 port 2 wins
//            +0x13 u8 last counted game's winner (0xFF none)
//            +0x14 u16 stages port 1 won on   +0x16 u16 port 2 (bitmasks of ruleset indices)
//            +0x18 u8 set decided   +0x19 u8 set winner (0xFF none)
//            +0x1A u8 a counted fight is in progress   +0x1B u8 its stage (0xFF unknown)
//            +0x1C u8 port 1 character, +0x1D port 2 (gmCharacterKind, 0xFF unknown)
//            +0x20 4 bytes per counted game (up to 8): winner, stage, both characters
//            (casual uses only +0x10, as a count of fights started)
//            +0x40 u8 port 1 preferred stage, +0x41 port 2 (casual only, 0xFF none)
//   steps    +0x80 u8 flow: 0 idle, 1 running on the stage select
//            +0x81 u8 step   +0x82 u8 port whose turn it is (0xFF none)
//            +0x83 u8 strikes/bans made this step   +0x84 u16 stages struck/banned this game
//            +0x88 u32 step deadline (frame number)
//            +0x8C u8 auto pick in progress   +0x8D u8 its stage
//            +0x8E u8 auto pick's A pressed this frame
//            +0x8F u8 why the flow is picking (AUTO_*)
constexpr u32 STATE_SIZE = 0x90;
static_assert(BASE + STATE_SIZE <= FreeSpace::kMatchBlock.end);

constexpr u8 MODE_NONE = 0;
constexpr u8 MODE_CASUAL = 1;
constexpr u8 MODE_RANKED = 2;

constexpr u8 NONE = 0xFF;
// Why the stage flow is picking a stage (State::auto_why).
constexpr u8 AUTO_TIMER = 0;   // timer ran out: pick the default
constexpr u8 AUTO_PICKED = 1;  // the picker pressed A on a stage
constexpr u8 AUTO_AGREED = 2;  // both players proposed the same stage
constexpr u8 AUTO_CASUAL = 3;  // casual: shared preference, or the coin decides
constexpr int MAX_GAMES = 8;

struct GameRecord
{
  u8 winner = NONE;
  u8 stage = NONE;
  std::array<u8, 2> characters{NONE, NONE};
  bool operator==(const GameRecord&) const = default;
};

struct State
{
  // Header.
  u8 mode = MODE_NONE;
  u8 ruleset = 0;
  u8 coin = 0;
  // Set.
  u8 games = 0;
  std::array<u8, 2> score{};
  u8 last_winner = NONE;
  std::array<u16, 2> won_on{};
  bool done = false;
  u8 set_winner = NONE;
  bool fight = false;
  u8 stage = NONE;
  std::array<u8, 2> characters{NONE, NONE};
  std::array<GameRecord, MAX_GAMES> records{};
  // Steps.
  u8 flow = 0;
  u8 step = 0;
  u8 active = NONE;
  u8 step_done = 0;
  u16 struck = 0;
  u32 deadline = 0;
  bool auto_pick = false;
  u8 auto_stage = NONE;
  bool auto_press = false;  // auto pick's A: pressed 4 frames, then released 4
  u8 auto_why = 0;          // AUTO_TIMER, AUTO_PICKED, AUTO_AGREED or AUTO_CASUAL
  // Casual preferred stages (RankedPPlus.h).
  std::array<u8, 2> prefs{NONE, NONE};
  bool operator==(const State&) const = default;

  // A new set with only the header fields filled in.
  static State Fresh(u8 mode, u8 ruleset, u8 coin);
};

// Nullopt when there is no header (still dead code, or an unknown version).
std::optional<State> Read(const GuestMemory& memory);
// True when the block's memory holds sora_scene's dead code or a block. Before sora_scene loads,
// this memory belongs to something else.
bool Present(const GuestMemory& memory);
// Writes changed bytes of +0x00..+0x8F (except room hash and queue flags), only when Present.
// Returns the number of bytes changed.
int Write(GuestMemory& memory, const State& state);
// Removes the header by restoring the original first word. Returns bytes changed.
int Clear(GuestMemory& memory);

// Game 1's first striker: one bit of the room code's FNV-1a hash. The server makes the code, so
// neither player can choose it.
u8 CoinFromCode(std::string_view code);
// FNV-1a hash of the room code.
u32 RoomHash(std::string_view code);
}  // namespace Orca::UX::MatchBlock
