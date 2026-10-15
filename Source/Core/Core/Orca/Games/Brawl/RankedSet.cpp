// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/RankedSet.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/SetEnd.h"

namespace Orca::UX::RankedSet
{
namespace
{
using SetBlock::GameRecord;
using SetBlock::How;
using SetBlock::NO_PORT;
using SetBlock::Ruleset;
using SetBlock::SetState;
using Scene = Reading::Scene;

constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_SET_RULE = 0x1C;
constexpr u32 RULE_MODE = 0x02;
constexpr u32 RULE_STOCKS = 0x04;
constexpr u32 RULE_STOCK_MINUTES = 0x08;
constexpr u8 DECISION_TIME_UP = 1;
constexpr u8 DECISION_NO_CONTEST = 9;

u32 SetRule(const GuestMemory& m)
{
  if (!m.Valid(GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (global % 4 || !m.Valid(global + GLOBAL_SET_RULE))
    return 0;
  const u32 rule = m.Read32(global + GLOBAL_SET_RULE);
  return rule % 4 == 0 && m.Valid(rule) && m.Valid(rule + RULE_STOCK_MINUTES) ? rule : 0;
}

u8 Byte(int v)
{
  return v < 0 || v > 254 ? 0xFF : static_cast<u8>(v);
}

// The ruleset a set block's ruleset byte names. A ranked header whose byte names none (Orca never
// writes one) keeps what the tracker did before rulesets were data: the results screen decides,
// two wins take the set, five ties in a row void it, no tiebreak game.
const Ranked::Ruleset* RulesOf(Ruleset ruleset)
{
  return Rulesets::ByBlock(static_cast<u8>(ruleset));
}
constexpr int NO_RULESET_WINS = 2;
constexpr int NO_RULESET_VOID_AFTER_TIES = 5;

bool Judged(const Ranked::Ruleset* rules)
{
  return rules && rules->time_out == Ranked::TimeOut::Judged;
}

// Last published set for CurrentScoreLine. Bit 31 ranked, 30 fighting, 29 results screen, 24-25
// done, 23 tiebreak, then each port's wins and the game number.
std::atomic<u32> s_published{0};
std::atomic<int> s_local_port{0};
// Last published dots for CurrentDots. Bit 31 on, bits 2i..2i+1 game i's winner plus one (0 none),
// bits 8..9 the current game (3 none).
std::atomic<u32> s_dots{0};
}  // namespace

std::optional<GameRecord> Judge(Ruleset ruleset, const SetState& set, const ResultBlock& block,
                                u8 decision)
{
  // Decision 9 means no contest, e.g. quitting from the pause menu.
  const auto& p = block.ports;
  if (decision == DECISION_NO_CONTEST || !p[0].present || !p[1].present || !p[0].human ||
      !p[1].human || p[2].present || p[3].present)
  {
    return std::nullopt;
  }
  GameRecord r;
  r.start = set.fight_start;
  r.stage = Byte(block.stage);
  r.tiebreak = set.tiebreak;
  r.ledge = set.ledge;
  for (int i = 0; i < 2; ++i)
  {
    r.characters[i] = Byte(p[i].character);
    r.stocks[i] = Byte(p[i].stocks);
  }
  const bool snapped = set.fight_flags & SetBlock::FIGHT_TIMEUP;
  const bool sudden = set.fight_flags & SetBlock::FIGHT_SUDDEN;
  r.timeout = snapped || decision == DECISION_TIME_UP;
  const bool first0 = p[0].place == 0, first1 = p[1].place == 0;
  const auto win = [&](int port, How how) {
    r.winner = static_cast<u8>(port);
    r.how = how;
    return r;
  };
  const auto tie = [&] {
    r.winner = NO_PORT;
    r.how = How::Tie;
    return r;
  };
  const Ranked::Ruleset* rules = RulesOf(ruleset);
  if (!Judged(rules))
  {
    // The results screen decides (Project+'s codeset settles time-outs itself).
    if (first0 != first1)
      return win(first0 ? 0 : 1, How::Results);
    if (first0 && first1)
      return tie();
    return std::nullopt;
  }
  // A sudden death not caused by a seen time-up (a simultaneous last-stock KO, or a time-out we
  // couldn't read) is a tie, settled by a tiebreak game.
  if (sudden && !snapped)
    return tie();
  if (r.timeout)
  {
    // Ledge-grab limit first: if exactly one player is over it, they lose.
    const bool over0 = set.ledge[0] > rules->ledge.For(set.tiebreak, r.characters[0]);
    const bool over1 = set.ledge[1] > rules->ledge.For(set.tiebreak, r.characters[1]);
    if (over0 != over1)
      return win(over0 ? 1 : 0, How::Ledge);
    // Then stocks, then percent, as they were when time ran out.
    std::array<int, 2> stocks{p[0].stocks, p[1].stocks};
    if (snapped)
      stocks = {set.snap_stocks[0], set.snap_stocks[1]};
    if (stocks[0] >= 0 && stocks[1] >= 0 && stocks[0] != stocks[1])
      return win(stocks[0] > stocks[1] ? 0 : 1, How::Stocks);
    if (snapped && set.snap_percent[0] != set.snap_percent[1])
      return win(set.snap_percent[0] < set.snap_percent[1] ? 0 : 1, How::Percent);
    if (!snapped && first0 != first1)
      return win(first0 ? 0 : 1, How::Results);
    return tie();
  }
  if (first0 != first1)
    return win(first0 ? 0 : 1, How::Results);
  if (first0 && first1)
    return tie();
  return std::nullopt;
}

SetState Step(SetState s, Ruleset ruleset, const Facts& f, u32 frame)
{
  const Ranked::Ruleset* rules = RulesOf(ruleset);
  const bool judged = Judged(rules);
  if (s.done)
  {
    if (s.fight)
    {
      s.fight = false;
      s.fight_flags = 0;
    }
    return s;
  }
  switch (f.scene)
  {
  case Scene::Fight:
    if (!s.fight)
    {
      s.fight = true;
      s.fight_flags = 0;
      s.fight_start = frame;
      s.ledge = {0, 0};
      s.snap_stocks = {0, 0};
      s.snap_percent = {0, 0};
      s.on_ledge = {0, 0};
    }
    else if (s.fight_flags & SetBlock::FIGHT_BETWEEN)
    {
      // Back into a fight from between scenes: sudden death in the same game.
      s.fight_flags =
          static_cast<u8>((s.fight_flags & ~SetBlock::FIGHT_BETWEEN) | SetBlock::FIGHT_SUDDEN);
    }
    if (f.live.valid && !judged)
    {
      // The results screen decides (Project+): only the fighters being in counts, when the game
      // began (its game-start). Nothing else is counted.
      s.fight_flags |= SetBlock::FIGHT_SEEN;
    }
    else if (f.live.valid)
    {
      // Fighters reappearing in the same scene means Brawl started a sudden death.
      if (s.fight_flags & SetBlock::FIGHT_GONE)
      {
        s.fight_flags = static_cast<u8>((s.fight_flags & ~SetBlock::FIGHT_GONE) |
                                        SetBlock::FIGHT_SUDDEN);
      }
      s.fight_flags |= SetBlock::FIGHT_SEEN;
      // Ledge grabs and the time-up snapshot count only in regulation, not sudden death.
      if (!(s.fight_flags & (SetBlock::FIGHT_SUDDEN | SetBlock::FIGHT_TIMEUP)))
      {
        for (int i = 0; i < 2; ++i)
        {
          if (f.live.on_ledge[i] && !s.on_ledge[i] && s.ledge[i] < 255)
            ++s.ledge[i];
          s.on_ledge[i] = f.live.on_ledge[i] ? 1 : 0;
        }
        if (f.live.time_up)
        {
          s.fight_flags |= SetBlock::FIGHT_TIMEUP;
          s.snap_stocks = f.live.stocks;
          s.snap_percent = f.live.percent;
        }
      }
    }
    else if (judged && f.live_read && (s.fight_flags & SetBlock::FIGHT_SEEN))
    {
      s.fight_flags |= SetBlock::FIGHT_GONE;
    }
    break;
  case Scene::Between:
    if (s.fight)
      s.fight_flags |= SetBlock::FIGHT_BETWEEN;
    break;
  case Scene::Results:
  {
    if (!s.fight)
      break;
    const std::optional<GameRecord> r = Judge(ruleset, s, f.block, f.decision);
    s.fight = false;
    s.fight_flags = 0;
    s.on_ledge = {0, 0};
    if (!r)
      break;
    if (s.games < SetBlock::MAX_RECORDS)
      s.records[s.games] = *r;
    ++s.games;
    if (r->winner != NO_PORT)
    {
      const int w = r->winner;
      ++s.wins[w];
      s.last_winner = r->winner;
      s.tiebreak = false;
      s.tb_streak = 0;
      if (s.wins[w] >= (rules ? rules->wins_needed : NO_RULESET_WINS))
      {
        s.done = 1;
        s.winner = r->winner;
      }
    }
    else
    {
      ++s.tb_streak;
      // A ruleset with a tiebreak game plays one (Brawl). Project+ has its own overtime.
      s.tiebreak = rules && rules->tiebreak;
      if (s.tb_streak >= (rules ? rules->void_after_ties : NO_RULESET_VOID_AFTER_TIES))
        s.done = 2;
    }
    if (!s.done && s.games >= SetBlock::MAX_RECORDS)
      s.done = 2;
    if (s.done == 2)
    {
      s.winner = NO_PORT;
      s.tiebreak = false;
    }
    break;
  }
  case Scene::Other:
    if (s.fight)
    {
      // A fight that ends without a results screen is not a game.
      s.fight = false;
      s.fight_flags = 0;
      s.on_ledge = {0, 0};
    }
    break;
  }
  return s;
}

int WriteSetRules(GuestMemory& memory, const SetState& set, const Ranked::Ruleset& rules,
                  std::optional<std::pair<u8, u8>> stocks_minutes)
{
  const u8 stocks = stocks_minutes ? stocks_minutes->first : rules.stocks;
  const u8 minutes = stocks_minutes ? stocks_minutes->second : rules.minutes;
  const u32 rule = SetRule(memory);
  if (!rule)
    return 0;
  int changed = 0;
  const auto put = [&](u32 offset, u8 value) {
    if (memory.Read8(rule + offset) == value)
      return;
    memory.Write8(rule + offset, value);
    ++changed;
  };
  put(RULE_MODE, 1);
  put(RULE_STOCKS, set.tiebreak ? rules.tiebreak_stocks : stocks);
  put(RULE_STOCK_MINUTES, set.tiebreak ? rules.tiebreak_minutes : minutes);
  return changed;
}

namespace
{
// Fighters come from sora_melee's g_ftEntryManager (module 27, section 6 + 0x2E88), found through
// the OS module list. ftEntryManager +0x00 is the entries and +0x04 their count. An ftEntry
// (0x244 bytes) has the active instance at +0x0A, its ftOwner at +0x28, the (kind, Fighter*)
// instances at +0x30 and the port at +0x58. An ftOwner's data pointer is at +0x00.
constexpr u32 OS_MODULE_LIST = 0x800030C8;
constexpr u32 SORA_MELEE = 27;
constexpr u32 SORA_MELEE_BSS_SECTION = 6;
constexpr u32 G_FT_ENTRY_MANAGER = 0x2E88;
constexpr u32 ENTRY_SIZE = 0x244;
constexpr u32 ENTRY_ACTIVE = 0x0A;
constexpr u32 ENTRY_OWNER = 0x28;
constexpr u32 ENTRY_INSTANCES = 0x30;
constexpr u32 ENTRY_PLAYER_NO = 0x58;
// Fighter +0x60 is its soModuleAccesser, +0xD8 the module list, +0x70 the status module.
constexpr u32 FIGHTER_MODULES = 0x60;
constexpr u32 MODULES_LIST = 0xD8;
constexpr u32 LIST_STATUS = 0x70;
// Found in RAM dumps: stock count is the owner data's u32 at +0x34, damage its f32 at +0x24, and
// the status kind the status module's u32 at +0x34.
constexpr u32 OWNER_DATA_STOCKS = 0x34;
constexpr u32 OWNER_DATA_DAMAGE = 0x24;
constexpr u32 STATUS_KIND = 0x34;
// The result info's decision during a fight: 9 while it runs, 1 at time-up, 2 at the deciding KO.
constexpr u32 GLOBAL_RESULT_INFO = 0x18;
constexpr u32 RESULT_DECISION = 0x1378;
// Fighter::StatusKind: Cliff_Catch_Move, Cliff_Catch, Cliff_Wait.
constexpr u32 STATUS_CLIFF_FIRST = 0x73;
constexpr u32 STATUS_CLIFF_LAST = 0x75;

u32 Ptr(const GuestMemory& m, u32 at)
{
  if (at % 4 || !m.Valid(at) || !m.Valid(at + 3))
    return 0;
  const u32 p = m.Read32(at);
  return p % 4 == 0 && p >= 0x80000000 && m.Valid(p) ? p : 0;
}

u32 SoraMeleeBss(const GuestMemory& m)
{
  u32 module = Ptr(m, OS_MODULE_LIST);
  for (int i = 0; module && i < 64; ++i)
  {
    if (m.Read32(module) == SORA_MELEE && m.Valid(module + 0x10))
    {
      if (m.Read32(module + 0x0C) <= SORA_MELEE_BSS_SECTION)
        return 0;
      const u32 sections = Ptr(m, module + 0x10);
      const u32 at = sections + 8 * SORA_MELEE_BSS_SECTION;
      if (!sections || !m.Valid(at))
        return 0;
      const u32 bss = m.Read32(at) & ~1u;
      return bss && m.Valid(bss) ? bss : 0;
    }
    module = Ptr(m, module + 4);
  }
  return 0;
}
}  // namespace

Live ReadLiveFight(const GuestMemory& m)
{
  Live live;
  const u32 global = Ptr(m, GAME_GLOBAL);
  const u32 info = global ? Ptr(m, global + GLOBAL_RESULT_INFO) : 0;
  if (!info || !m.Valid(info + RESULT_DECISION))
    return live;
  live.time_up = m.Read8(info + RESULT_DECISION) == DECISION_TIME_UP;
  const u32 bss = SoraMeleeBss(m);
  const u32 manager = bss ? Ptr(m, bss + G_FT_ENTRY_MANAGER) : 0;
  const u32 entries = manager ? Ptr(m, manager) : 0;
  if (!entries || !m.Valid(manager + 4))
    return live;
  const u32 count = std::min<u32>(m.Read32(manager + 4), 8);
  std::array<bool, 2> seen{false, false};
  for (u32 i = 0; i < count; ++i)
  {
    const u32 e = entries + ENTRY_SIZE * i;
    if (!m.Valid(e + ENTRY_SIZE - 1))
      break;
    const u32 port = m.Read32(e + ENTRY_PLAYER_NO);
    if (port > 1 || seen[port])
      continue;
    const u32 owner = Ptr(m, e + ENTRY_OWNER);
    const u32 data = owner ? Ptr(m, owner) : 0;
    if (!data || !m.Valid(data + OWNER_DATA_STOCKS) || !m.Valid(data + OWNER_DATA_DAMAGE + 3))
      return {};
    live.stocks[port] = static_cast<u8>(std::min<u32>(m.Read32(data + OWNER_DATA_STOCKS), 255));
    const u32 bits = m.Read32(data + OWNER_DATA_DAMAGE);
    float damage;
    std::memcpy(&damage, &bits, sizeof(damage));
    live.percent[port] =
        damage > 0 && damage < 10000 ? static_cast<u16>(static_cast<int>(damage)) : u16(0);
    const u32 active = std::min<u32>(m.Read8(e + ENTRY_ACTIVE), 3);
    const u32 fighter = Ptr(m, e + ENTRY_INSTANCES + 8 * active + 4);
    const u32 modules = fighter ? Ptr(m, fighter + FIGHTER_MODULES) : 0;
    const u32 list = modules ? Ptr(m, modules + MODULES_LIST) : 0;
    const u32 status = list ? Ptr(m, list + LIST_STATUS) : 0;
    if (status && m.Valid(status + STATUS_KIND + 3))
    {
      const u32 kind = m.Read32(status + STATUS_KIND);
      live.on_ledge[port] = kind >= STATUS_CLIFF_FIRST && kind <= STATUS_CLIFF_LAST;
      live.status[port] = kind;
    }
    seen[port] = true;
  }
  live.valid = seen[0] && seen[1];
  return live;
}

Facts ReadFacts(const GuestMemory& memory, bool live)
{
  Facts f;
  const Reading r = ReadResults(memory);
  f.scene = r.scene;
  f.block = r.block;
  f.decision = static_cast<u8>(r.block.decision);
  if (live && f.scene == Scene::Fight)
  {
    f.live = ReadLiveFight(memory);
    f.live_read = true;
  }
  return f;
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating)
{
  if (!ResultsVerified())
    return;
  GuardMemory memory(guard);
  const std::optional<SetBlock::Header> header = SetBlock::ReadHeader(memory);
  if (!header || header->mode != SetBlock::Mode::Ranked)
  {
    s_published.store(0, std::memory_order_relaxed);
    s_dots.store(0, std::memory_order_relaxed);
    return;
  }
  // Brawl's verdict needs the live fight state, and in both games the fighters being in is when a
  // game began (FIGHT_SEEN: the room's game-start, after the loading image).
  const Facts facts = ReadFacts(memory, true);
  const SetState before = SetBlock::ReadSet(memory);
  const SetState after = Step(before, header->ruleset, facts, static_cast<u32>(frame));
  if (after != before)
  {
    SetBlock::WriteSet(memory, after);
    if ((after.fight_flags & SetBlock::FIGHT_SEEN) &&
        !(before.fight_flags & SetBlock::FIGHT_SEEN) && !resimulating)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Ranked set: game {}'s fighters are in at frame {} (fight from {})",
                     after.GameNumber(), frame, after.fight_start);
    }
    if (after.games != before.games && !resimulating)
    {
      const GameRecord& r = after.records[std::min(after.games, u8(SetBlock::MAX_RECORDS)) - 1];
      NOTICE_LOG_FMT(ROLLBACK,
                     "Ranked set: game {} at frame {} (fight from {}): winner {}, how {}, "
                     "ledge {}/{}, set {}-{}{}",
                     after.games, frame, r.start, r.winner == NO_PORT ? 0 : r.winner + 1,
                     static_cast<int>(r.how), r.ledge[0], r.ledge[1], after.wins[0], after.wins[1],
                     after.done == 1 ? " (over)" :
                     after.done == 2 ? " (void)" :
                     after.tiebreak  ? " (tiebreak next)" :
                                       "");
    }
  }
  const Ranked::Ruleset* rules = RulesOf(header->ruleset);
  if (rules && rules->SetWritesGameRules() && facts.scene != Scene::Fight)
  {
    // Test knob ORCA_TEST_SET_RULES=<stocks>,<minutes> for shorter games.
    static const std::optional<std::pair<u8, u8>> s_rules = []() -> std::optional<std::pair<u8, u8>> {
      const char* v = std::getenv("ORCA_TEST_SET_RULES");
      char* end = nullptr;
      const long stocks = v ? std::strtol(v, &end, 10) : 0;
      const long minutes = end && *end == ',' ? std::strtol(end + 1, &end, 10) : 0;
      if (stocks >= 1 && stocks <= 99 && minutes >= 1 && minutes <= 99 && end && *end == '\0' &&
          TestKnobsAllowed())
      {
        return std::pair<u8, u8>(static_cast<u8>(stocks), static_cast<u8>(minutes));
      }
      return std::nullopt;
    }();
    WriteSetRules(memory, after, *rules, s_rules);
  }
  // Show VICTORY or DEFEAT on the deciding game's results screen. Local display, never on re-runs.
  if (!resimulating && after.done == 1 && facts.scene == Scene::Results && after.games > 0 &&
      after.winner != NO_PORT)
  {
    const int me = s_local_port.load(std::memory_order_relaxed) == 1 ? 1 : 0;
    const GameRecord& last = after.records[std::min(after.games, u8(SetBlock::MAX_RECORDS)) - 1];
    SetEnd::Current().BlockSetOver(last.start, after.winner == me, after.wins[me],
                                   after.wins[1 - me], QueueApp::OwnRating(), SetEnd::NowMs());
  }
  const bool fighting = facts.scene == Scene::Fight || facts.scene == Scene::Between;
  const bool results = facts.scene == Scene::Results;
  const u32 packed = 0x80000000u | (fighting ? 1u << 30 : 0) | (results ? 1u << 29 : 0) |
                     (u32(after.done & 3) << 24) |
                     (after.tiebreak ? 1u << 23 : 0) | (u32(after.wins[0]) << 16) |
                     (u32(after.wins[1]) << 8) | u32(std::min(after.GameNumber(), 255));
  s_published.store(packed, std::memory_order_relaxed);
  u32 dots = 0;
  if (!fighting)
  {
    const Dots d = DotsOf(after);
    dots = 0x80000000u | (u32(d.current < 0 ? 3 : d.current) << 8);
    for (int i = 0; i < 3; ++i)
      dots |= u32(d.games[static_cast<std::size_t>(i)] + 1) << (2 * i);
  }
  s_dots.store(dots, std::memory_order_relaxed);
}

Dots DotsOf(const SetState& set)
{
  Dots d;
  d.on = true;
  std::size_t decided = 0;
  const int records = std::min<int>(set.games, SetBlock::MAX_RECORDS);
  for (int i = 0; i < records && decided < d.games.size(); ++i)
  {
    const u8 winner = set.records[static_cast<std::size_t>(i)].winner;
    if (winner == 0 || winner == 1)
      d.games[decided++] = winner;
  }
  d.current = set.done || decided >= d.games.size() ? -1 : static_cast<int>(decided);
  return d;
}

Dots CurrentDots()
{
  const u32 packed = s_dots.load(std::memory_order_relaxed);
  Dots d;
  if (!(packed & 0x80000000u))
    return d;
  d.on = true;
  for (int i = 0; i < 3; ++i)
    d.games[static_cast<std::size_t>(i)] = static_cast<int>((packed >> (2 * i)) & 3) - 1;
  const int current = static_cast<int>((packed >> 8) & 3);
  d.current = current == 3 ? -1 : current;
  return d;
}

std::string ScoreLine(const SetState& set, int local_port, bool results)
{
  const int me = local_port == 1 ? 1 : 0;
  const int mine = set.wins[me], theirs = set.wins[1 - me];
  if (set.done == 2)
    return fmt::format("Set void · {}–{}", mine, theirs);
  if (set.done)
    return fmt::format("Set over · {}–{}", mine, theirs);
  if (set.tiebreak)
    return fmt::format("Tiebreak game · {}–{}", mine, theirs);
  // On a results screen, name the game just played. The set already counts it there.
  const int played = set.wins[0] + set.wins[1];
  if (results && played > 0)
    return fmt::format("Game {} · {}–{}", played, mine, theirs);
  // Elsewhere the dots and the result toast already say it.
  return "";
}

std::string ResultToast(std::string_view out, int mine, int theirs)
{
  if (out == "won")
    return fmt::format("You won · {}–{}", mine, theirs);
  if (out == "lost")
    return fmt::format("You lost · {}–{}", mine, theirs);
  if (out == "draw")
    return "Draw · played again";
  return "That game didn't count";
}

std::string CurrentScoreLine()
{
  const u32 packed = s_published.load(std::memory_order_relaxed);
  // Menus only, never over a fight.
  if (!(packed & 0x80000000u) || (packed & (1u << 30)))
    return "";
  SetState s;
  s.done = static_cast<u8>((packed >> 24) & 3);
  s.tiebreak = packed & (1u << 23);
  s.wins = {static_cast<u8>((packed >> 16) & 0x7F), static_cast<u8>((packed >> 8) & 0xFF)};
  return ScoreLine(s, s_local_port.load(std::memory_order_relaxed), packed & (1u << 29));
}

void SetLocalPort(int port)
{
  s_local_port.store(port, std::memory_order_relaxed);
}
}  // namespace Orca::UX::RankedSet
