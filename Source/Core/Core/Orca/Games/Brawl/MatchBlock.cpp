// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/MatchBlock.h"

#include "Core/Orca/Engine/GuestMemory.h"

namespace Orca::UX::MatchBlock
{
namespace
{
constexpr u32 OFF_MAGIC = 0x00;
constexpr u32 OFF_VERSION = 0x04;
constexpr u32 OFF_MODE = 0x05;
constexpr u32 OFF_RULESET = 0x06;
constexpr u32 OFF_COIN = 0x07;
constexpr u32 OFF_GAMES = 0x10;
constexpr u32 OFF_SCORE = 0x11;
constexpr u32 OFF_LAST_WINNER = 0x13;
constexpr u32 OFF_WON_ON = 0x14;
constexpr u32 OFF_DONE = 0x18;
constexpr u32 OFF_SET_WINNER = 0x19;
constexpr u32 OFF_FIGHT = 0x1A;
constexpr u32 OFF_STAGE = 0x1B;
constexpr u32 OFF_CHARACTERS = 0x1C;
constexpr u32 OFF_RECORDS = 0x20;
constexpr u32 OFF_PREFS = 0x40;
static_assert(OFF_RECORDS + 4 * MAX_GAMES <= OFF_PREFS);
constexpr u32 OFF_FLOW = 0x80;
constexpr u32 OFF_STEP = 0x81;
constexpr u32 OFF_ACTIVE = 0x82;
constexpr u32 OFF_STEP_DONE = 0x83;
constexpr u32 OFF_STRUCK = 0x84;
constexpr u32 OFF_DEADLINE = 0x88;
constexpr u32 OFF_AUTO = 0x8C;
constexpr u32 OFF_AUTO_STAGE = 0x8D;
constexpr u32 OFF_AUTO_PRESS = 0x8E;
constexpr u32 OFF_AUTO_WHY = 0x8F;
static_assert(OFF_AUTO_WHY < STATE_SIZE);

// Serializes the state so writes can compare byte by byte.
std::array<u8, STATE_SIZE> Bytes(const State& s)
{
  std::array<u8, STATE_SIZE> b{};
  const auto put16 = [&](u32 at, u16 v) {
    b[at] = static_cast<u8>(v >> 8);
    b[at + 1] = static_cast<u8>(v);
  };
  const auto put32 = [&](u32 at, u32 v) {
    put16(at, static_cast<u16>(v >> 16));
    put16(at + 2, static_cast<u16>(v));
  };
  put32(OFF_MAGIC, MAGIC_VALUE);
  b[OFF_VERSION] = VERSION_VALUE;
  b[OFF_MODE] = s.mode;
  b[OFF_RULESET] = s.ruleset;
  b[OFF_COIN] = s.coin;
  b[OFF_GAMES] = s.games;
  b[OFF_SCORE] = s.score[0];
  b[OFF_SCORE + 1] = s.score[1];
  b[OFF_LAST_WINNER] = s.last_winner;
  put16(OFF_WON_ON, s.won_on[0]);
  put16(OFF_WON_ON + 2, s.won_on[1]);
  b[OFF_DONE] = s.done ? 1 : 0;
  b[OFF_SET_WINNER] = s.set_winner;
  b[OFF_FIGHT] = s.fight ? 1 : 0;
  b[OFF_STAGE] = s.stage;
  b[OFF_CHARACTERS] = s.characters[0];
  b[OFF_CHARACTERS + 1] = s.characters[1];
  for (int i = 0; i < MAX_GAMES; ++i)
  {
    const GameRecord& r = s.records[i];
    const u32 at = OFF_RECORDS + 4 * i;
    b[at] = r.winner;
    b[at + 1] = r.stage;
    b[at + 2] = r.characters[0];
    b[at + 3] = r.characters[1];
  }
  b[OFF_PREFS] = s.prefs[0];
  b[OFF_PREFS + 1] = s.prefs[1];
  b[OFF_FLOW] = s.flow;
  b[OFF_STEP] = s.step;
  b[OFF_ACTIVE] = s.active;
  b[OFF_STEP_DONE] = s.step_done;
  put16(OFF_STRUCK, s.struck);
  put32(OFF_DEADLINE, s.deadline);
  b[OFF_AUTO] = s.auto_pick ? 1 : 0;
  b[OFF_AUTO_STAGE] = s.auto_stage;
  b[OFF_AUTO_PRESS] = s.auto_press ? 1 : 0;
  b[OFF_AUTO_WHY] = s.auto_why;
  return b;
}
}  // namespace

State State::Fresh(u8 mode, u8 ruleset, u8 coin)
{
  State s;
  s.mode = mode;
  s.ruleset = ruleset;
  s.coin = coin <= 1 ? coin : 0;
  return s;
}

std::optional<State> Read(const GuestMemory& m)
{
  if (!m.Valid(BASE) || !m.Valid(BASE + STATE_SIZE - 1))
    return std::nullopt;
  if (m.Read32(BASE + OFF_MAGIC) != MAGIC_VALUE || m.Read8(BASE + OFF_VERSION) != VERSION_VALUE)
    return std::nullopt;
  const auto r8 = [&](u32 off) { return m.Read8(BASE + off); };
  const auto r16 = [&](u32 off) { return m.Read16(BASE + off); };
  State s;
  s.mode = r8(OFF_MODE);
  s.ruleset = r8(OFF_RULESET);
  s.coin = r8(OFF_COIN);
  s.games = r8(OFF_GAMES);
  s.score = {r8(OFF_SCORE), r8(OFF_SCORE + 1)};
  s.last_winner = r8(OFF_LAST_WINNER);
  s.won_on = {r16(OFF_WON_ON), r16(OFF_WON_ON + 2)};
  s.done = r8(OFF_DONE) != 0;
  s.set_winner = r8(OFF_SET_WINNER);
  s.fight = r8(OFF_FIGHT) != 0;
  s.stage = r8(OFF_STAGE);
  s.characters = {r8(OFF_CHARACTERS), r8(OFF_CHARACTERS + 1)};
  for (int i = 0; i < MAX_GAMES; ++i)
  {
    const u32 at = OFF_RECORDS + 4 * i;
    s.records[i] = {r8(at), r8(at + 1), {r8(at + 2), r8(at + 3)}};
  }
  s.prefs = {r8(OFF_PREFS), r8(OFF_PREFS + 1)};
  s.flow = r8(OFF_FLOW);
  s.step = r8(OFF_STEP);
  s.active = r8(OFF_ACTIVE);
  s.step_done = r8(OFF_STEP_DONE);
  s.struck = r16(OFF_STRUCK);
  s.deadline = m.Read32(BASE + OFF_DEADLINE);
  s.auto_pick = r8(OFF_AUTO) != 0;
  s.auto_stage = r8(OFF_AUTO_STAGE);
  s.auto_press = r8(OFF_AUTO_PRESS) != 0;
  s.auto_why = r8(OFF_AUTO_WHY);
  return s;
}

bool Present(const GuestMemory& m)
{
  if (!m.Valid(BASE) || !m.Valid(BASE + STATE_SIZE - 1))
    return false;
  const u32 first = m.Read32(BASE + OFF_MAGIC);
  return first == MAGIC_VALUE || first == FreeSpace::kMatchBlockOriginalWord;
}

int Write(GuestMemory& m, const State& s)
{
  // Before sora_scene loads, this memory belongs to something else.
  if (!Present(m))
    return 0;
  const std::array<u8, STATE_SIZE> b = Bytes(s);
  int changed = 0;
  // Write the magic last so a reader never sees a header over half-written bytes.
  for (u32 i = STATE_SIZE; i-- > 0;)
  {
    // Room hash and queue flags (+0x08..+0x0F) belong to the header writer.
    if (i >= ROOM - BASE && i < HEADER_SIZE)
      continue;
    if (m.Read8(BASE + i) != b[i])
    {
      m.Write8(BASE + i, b[i]);
      ++changed;
    }
  }
  return changed;
}

int Clear(GuestMemory& m)
{
  if (!m.Valid(BASE + 3) || m.Read32(BASE + OFF_MAGIC) != MAGIC_VALUE)
    return 0;
  // Restore the original first word: no header.
  m.Write32(BASE + OFF_MAGIC, FreeSpace::kMatchBlockOriginalWord);
  return 4;
}

u32 RoomHash(std::string_view code)
{
  u32 h = 2166136261u;
  for (const char c : code)
  {
    h ^= static_cast<u8>(c);
    h *= 16777619u;
  }
  return h;
}

u8 CoinFromCode(std::string_view code)
{
  u32 h = RoomHash(code);
  // Fold the hash so the coin depends on every bit.
  h ^= h >> 16;
  h ^= h >> 8;
  h ^= h >> 4;
  h ^= h >> 2;
  h ^= h >> 1;
  return static_cast<u8>(h & 1);
}
}  // namespace Orca::UX::MatchBlock
