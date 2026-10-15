// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include "Common/CommonTypes.h"
#include "Core/Orca/Games/Brawl/FreeSpace.h"

// The ranked set's part of the match block, kept in emulated memory so every rollback snapshot and
// drop-in keyframe carries it. Only Orca's frame hook writes it, as a pure function of emulated
// memory and the frame number, and only the bytes that differ. Before Orca writes it the block
// holds dead game code, so readers check the magic. Offsets are from the block's start and
// big-endian. The header (+0x00..+0x06) is shared with MatchBlock.h. The set lives at
// +0x210..+0x28F, past the 512-byte block in the same dead code, clear of other layouts.
namespace Orca::UX
{
class GuestMemory;

namespace SetBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;
// The block plus its free tail, up to the next relocated word.
constexpr u32 SIZE = FreeSpace::kMatchBlockLimit - FreeSpace::kMatchBlock.begin;

constexpr u32 MAGIC = 0x59474D31;  // "YGM1"
constexpr u8 VERSION = 1;

// Header.
constexpr u32 HDR_MAGIC = 0x00;    // u32
constexpr u32 HDR_VERSION = 0x04;  // u8
constexpr u32 HDR_MODE = 0x05;     // u8 Mode
constexpr u32 HDR_RULESET = 0x06;  // u8 Ruleset

enum class Mode : u8
{
  None = 0,
  Casual = 1,
  Ranked = 2,
};

enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,  // Supernova 2025
  PPlus = 2,  // Project+ 2024
};

struct Header
{
  Mode mode = Mode::None;
  Ruleset ruleset = Ruleset::None;
  bool operator==(const Header&) const = default;
};

// The set's state.
constexpr u32 SET_BEGIN = 0x210;
constexpr u32 SET_GAMES = 0x210;         // u8: games recorded (wins and ties, not voids)
constexpr u32 SET_WINS = 0x211;          // u8[2]: games won by ports 1 and 2
constexpr u32 SET_DONE = 0x213;          // u8: 0 open, 1 won (SET_WINNER), 2 void
constexpr u32 SET_WINNER = 0x214;        // u8: set winner's port (0-based), 0xFF none
constexpr u32 SET_LAST_WINNER = 0x215;   // u8: last decided game's winner, 0xFF none
constexpr u32 SET_TIEBREAK = 0x216;      // u8: 1 if the next game is a tiebreak
constexpr u32 SET_TB_STREAK = 0x217;     // u8: tied games in a row
constexpr u32 SET_FIGHT = 0x218;         // u8: 1 from fight start to results
constexpr u32 SET_FIGHT_FLAGS = 0x219;   // u8: FIGHT_* below
constexpr u32 SET_LEDGE = 0x21A;         // u8[2]: ledge grabs this fight (255 at most)
constexpr u32 SET_FIGHT_START = 0x21C;   // u32: fight's first frame, used as the game's id
constexpr u32 SET_SNAP_STOCKS = 0x220;   // u8[2]: stocks when time ran out
constexpr u32 SET_SNAP_PERCENT = 0x222;  // u16[2]: percent when time ran out
constexpr u32 SET_ON_LEDGE = 0x226;      // u8[2]: on the ledge last frame, for edge counting
constexpr u32 SET_RECORDS = 0x230;       // GameRecord x MAX_RECORDS
constexpr u32 RECORD_SIZE = 0x0C;
constexpr int MAX_RECORDS = 8;
constexpr u32 SET_END = SET_RECORDS + RECORD_SIZE * MAX_RECORDS;  // 0x290
static_assert(SET_END <= SIZE && FreeSpace::Detail::Free({BASE + SET_BEGIN, BASE + SET_END}));

constexpr u8 FIGHT_BETWEEN = 0x01;  // left the fight scene
constexpr u8 FIGHT_SUDDEN = 0x02;   // sudden death started
constexpr u8 FIGHT_TIMEUP = 0x04;   // time ran out; snap_* hold stocks and percent
constexpr u8 FIGHT_SEEN = 0x08;     // fighters were read this fight
constexpr u8 FIGHT_GONE = 0x10;     // then vanished; sudden death rebuilds them

constexpr u8 NO_PORT = 0xFF;

// How a game was decided.
enum class How : u8
{
  None = 0,
  Results = 1,  // results screen's winner (KO, or the game's own time-out rules)
  Stocks = 2,   // time-out: more stocks
  Percent = 3,  // time-out: lower percent
  Ledge = 4,    // time-out: opponent went over the ledge-grab limit
  Tie = 5,      // no winner; Brawl plays a tiebreak game
};

// One game, 12 bytes:
//   +0x00 u32 fight's first frame     +0x04 u8 winner port (NO_PORT for a tie)
//   +0x05 u8 How in bits 0-3, 0x10 tiebreak, 0x20 time ran out
//   +0x06 u8 stage                    +0x07 u8[2] characters
//   +0x09 u8 stocks left, port 1 high nibble, port 2 low (0xF unknown, so at most 14)
//   +0x0A u8[2] ledge grabs
struct GameRecord
{
  u32 start = 0;
  u8 winner = NO_PORT;
  How how = How::None;
  u8 stage = 0xFF;
  bool tiebreak = false;
  bool timeout = false;
  std::array<u8, 2> characters{0xFF, 0xFF};
  std::array<u8, 2> stocks{0xFF, 0xFF};
  std::array<u8, 2> ledge{0, 0};
  bool operator==(const GameRecord&) const = default;
};

struct SetState
{
  u8 games = 0;
  std::array<u8, 2> wins{0, 0};
  u8 done = 0;
  u8 winner = NO_PORT;
  u8 last_winner = NO_PORT;
  bool tiebreak = false;
  u8 tb_streak = 0;
  bool fight = false;
  u8 fight_flags = 0;
  std::array<u8, 2> ledge{0, 0};
  u32 fight_start = 0;
  std::array<u8, 2> snap_stocks{0, 0};
  std::array<u16, 2> snap_percent{0, 0};
  std::array<u8, 2> on_ledge{0, 0};
  std::array<GameRecord, MAX_RECORDS> records{};
  bool operator==(const SetState&) const = default;

  // The game in play or about to start.
  int GameNumber() const { return wins[0] + wins[1] + 1; }
  // Mask of stage ids (below 64) a port has won on this set, for DSR.
  u64 StagesWonBy(int port) const;
};

// The header, if the magic and version match.
std::optional<Header> ReadHeader(const GuestMemory& memory);
// Writes the header. A new mode or ruleset starts a fresh set. Returns how many bytes changed.
int WriteHeader(GuestMemory& memory, const Header& header);
// Clears the header and set state, leaving the stage flow's bytes. Returns bytes changed.
int Clear(GuestMemory& memory);

SetState ReadSet(const GuestMemory& memory);
// Writes only differing bytes. Returns how many bytes changed.
int WriteSet(GuestMemory& memory, const SetState& state);

// True once the game is booted and the block is mapped.
bool Mapped(const GuestMemory& memory);
}  // namespace SetBlock
}  // namespace Orca::UX
