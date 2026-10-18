// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/SetBlock.h"

#include "Core/Orca/Engine/GuestMemory.h"

namespace Orca::UX::SetBlock
{
namespace
{
// Writes only bytes that differ, counting them.
class Writer
{
public:
  explicit Writer(GuestMemory& memory) : m(memory) {}
  void U8(u32 offset, u8 value)
  {
    if (m.Read8(BASE + offset) == value)
      return;
    m.Write8(BASE + offset, value);
    ++changed;
  }
  void U16(u32 offset, u16 value)
  {
    U8(offset, static_cast<u8>(value >> 8));
    U8(offset + 1, static_cast<u8>(value));
  }
  void U32(u32 offset, u32 value)
  {
    U16(offset, static_cast<u16>(value >> 16));
    U16(offset + 2, static_cast<u16>(value));
  }
  GuestMemory& m;
  int changed = 0;
};

u8 R8(const GuestMemory& m, u32 offset)
{
  return m.Read8(BASE + offset);
}
u16 R16(const GuestMemory& m, u32 offset)
{
  return static_cast<u16>(R8(m, offset) << 8 | R8(m, offset + 1));
}
u32 R32(const GuestMemory& m, u32 offset)
{
  return u32(R16(m, offset)) << 16 | R16(m, offset + 2);
}
}  // namespace

u64 SetState::StagesWonBy(int port) const
{
  u64 mask = 0;
  const int n = games < MAX_RECORDS ? games : MAX_RECORDS;
  for (int i = 0; i < n; ++i)
  {
    const GameRecord& r = records[i];
    if (r.winner == port && r.stage < 64)
      mask |= u64(1) << r.stage;
  }
  return mask;
}

bool Mapped(const GuestMemory& memory)
{
  return memory.Valid(BASE) && memory.Valid(BASE + SIZE - 1);
}

std::optional<Header> ReadHeader(const GuestMemory& memory)
{
  if (!Mapped(memory) || R32(memory, HDR_MAGIC) != MAGIC || R8(memory, HDR_VERSION) != VERSION)
    return std::nullopt;
  Header h;
  const u8 mode = R8(memory, HDR_MODE), ruleset = R8(memory, HDR_RULESET);
  if (mode > static_cast<u8>(Mode::Ranked) || ruleset > static_cast<u8>(Ruleset::PPlus))
    return std::nullopt;
  h.mode = static_cast<Mode>(mode);
  h.ruleset = static_cast<Ruleset>(ruleset);
  return h;
}

int WriteHeader(GuestMemory& memory, const Header& header)
{
  if (!Mapped(memory))
    return 0;
  const std::optional<Header> before = ReadHeader(memory);
  Writer w(memory);
  w.U32(HDR_MAGIC, MAGIC);
  w.U8(HDR_VERSION, VERSION);
  w.U8(HDR_MODE, static_cast<u8>(header.mode));
  w.U8(HDR_RULESET, static_cast<u8>(header.ruleset));
  int changed = w.changed;
  if (!before || *before != header)
    changed += WriteSet(memory, SetState{});
  return changed;
}

int Clear(GuestMemory& memory)
{
  if (!Mapped(memory))
    return 0;
  Writer w(memory);
  for (u32 i = HDR_MAGIC; i <= HDR_RULESET; ++i)
    w.U8(i, 0);
  for (u32 i = SET_BEGIN; i < SET_END; ++i)
    w.U8(i, 0);
  return w.changed;
}

SetState ReadSet(const GuestMemory& memory)
{
  SetState s;
  if (!Mapped(memory))
    return s;
  s.games = R8(memory, SET_GAMES);
  s.wins = {R8(memory, SET_WINS), R8(memory, SET_WINS + 1)};
  s.done = R8(memory, SET_DONE);
  s.winner = R8(memory, SET_WINNER);
  s.last_winner = R8(memory, SET_LAST_WINNER);
  s.tiebreak = R8(memory, SET_TIEBREAK) != 0;
  s.tb_streak = R8(memory, SET_TB_STREAK);
  s.fight = R8(memory, SET_FIGHT) != 0;
  s.fight_flags = R8(memory, SET_FIGHT_FLAGS);
  s.ledge = {R8(memory, SET_LEDGE), R8(memory, SET_LEDGE + 1)};
  s.fight_start = R32(memory, SET_FIGHT_START);
  s.snap_stocks = {R8(memory, SET_SNAP_STOCKS), R8(memory, SET_SNAP_STOCKS + 1)};
  s.snap_percent = {R16(memory, SET_SNAP_PERCENT), R16(memory, SET_SNAP_PERCENT + 2)};
  s.on_ledge = {R8(memory, SET_ON_LEDGE), R8(memory, SET_ON_LEDGE + 1)};
  for (int i = 0; i < MAX_RECORDS; ++i)
  {
    const u32 o = SET_RECORDS + RECORD_SIZE * i;
    GameRecord& r = s.records[i];
    r.start = R32(memory, o);
    r.winner = R8(memory, o + 4);
    const u8 how = R8(memory, o + 5);
    r.how = (how & 0x0F) <= static_cast<u8>(How::Tie) ? static_cast<How>(how & 0x0F) : How::None;
    r.tiebreak = how & 0x10;
    r.timeout = how & 0x20;
    r.stage = R8(memory, o + 6);
    r.characters = {R8(memory, o + 7), R8(memory, o + 8)};
    const u8 stocks = R8(memory, o + 9);
    const auto nibble = [](u8 v) { return v == 0xF ? u8(0xFF) : v; };
    r.stocks = {nibble(stocks >> 4), nibble(stocks & 0xF)};
    r.ledge = {R8(memory, o + 10), R8(memory, o + 11)};
  }
  return s;
}

int WriteSet(GuestMemory& memory, const SetState& s)
{
  if (!Mapped(memory))
    return 0;
  Writer w(memory);
  w.U8(SET_GAMES, s.games);
  w.U8(SET_WINS, s.wins[0]);
  w.U8(SET_WINS + 1, s.wins[1]);
  w.U8(SET_DONE, s.done);
  w.U8(SET_WINNER, s.winner);
  w.U8(SET_LAST_WINNER, s.last_winner);
  w.U8(SET_TIEBREAK, s.tiebreak ? 1 : 0);
  w.U8(SET_TB_STREAK, s.tb_streak);
  w.U8(SET_FIGHT, s.fight ? 1 : 0);
  w.U8(SET_FIGHT_FLAGS, s.fight_flags);
  w.U8(SET_LEDGE, s.ledge[0]);
  w.U8(SET_LEDGE + 1, s.ledge[1]);
  w.U32(SET_FIGHT_START, s.fight_start);
  w.U8(SET_SNAP_STOCKS, s.snap_stocks[0]);
  w.U8(SET_SNAP_STOCKS + 1, s.snap_stocks[1]);
  w.U16(SET_SNAP_PERCENT, s.snap_percent[0]);
  w.U16(SET_SNAP_PERCENT + 2, s.snap_percent[1]);
  w.U8(SET_ON_LEDGE, s.on_ledge[0]);
  w.U8(SET_ON_LEDGE + 1, s.on_ledge[1]);
  for (u32 i = SET_ON_LEDGE + 2; i < SET_RECORDS; ++i)
    w.U8(i, 0);
  for (int i = 0; i < MAX_RECORDS; ++i)
  {
    const u32 o = SET_RECORDS + RECORD_SIZE * i;
    const GameRecord& r = s.records[i];
    const auto nibble = [](u8 v) { return static_cast<u8>(v > 14 ? 0xF : v); };
    w.U32(o, r.start);
    w.U8(o + 4, r.winner);
    w.U8(o + 5, static_cast<u8>((static_cast<u8>(r.how) & 0x0F) | (r.tiebreak ? 0x10 : 0) |
                                (r.timeout ? 0x20 : 0)));
    w.U8(o + 6, r.stage);
    w.U8(o + 7, r.characters[0]);
    w.U8(o + 8, r.characters[1]);
    w.U8(o + 9, static_cast<u8>(nibble(r.stocks[0]) << 4 | nibble(r.stocks[1])));
    w.U8(o + 10, r.ledge[0]);
    w.U8(o + 11, r.ledge[1]);
  }
  return w.changed;
}
}  // namespace Orca::UX::SetBlock
