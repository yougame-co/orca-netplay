// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/Session/Replay.h"

#include <atomic>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Profile.h"

namespace Orca::UX
{
namespace
{
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_RESULT_INFO = 0x18;
constexpr u32 GLOBAL_SET_RULE = 0x1C;
constexpr u32 RESULT_MODE = 0x01;
constexpr u32 RESULT_STAGE = 0x0C;
constexpr u32 RESULT_END = 0x0E;
constexpr u32 RESULT_DECISION = 0x1378;
constexpr u32 RESULT_PORTS = 0x24;
constexpr u32 RESULT_PORT_SIZE = 0x2AC;
constexpr u32 PORT_CHARACTER = 0x00;
constexpr u32 PORT_TYPE = 0x01;
constexpr u32 PORT_TEAM = 0x06;
constexpr u32 PORT_STOCKS = 0x0A;
constexpr u32 PORT_PLACE = 0x0E;
constexpr u8 TYPE_HUMAN = 0;
constexpr u8 TYPE_NONE = 3;
// gmSetRule offsets: what the Rules menu sets.
constexpr u32 RULE_MODE = 0x02;
constexpr u32 RULE_STOCKS = 0x04;
constexpr u32 RULE_HANDICAP = 0x05;
constexpr u32 RULE_RATIO = 0x06;
constexpr u32 RULE_STOCK_MINUTES = 0x08;
constexpr u32 RULE_ITEMS = 0x0C;
// The rule in RULE_MODE's low 3 bits (OnlineRules.cpp keeps the others): 1 a stock match.
constexpr u8 RULE_MODE_MASK = 0x07;
constexpr u8 RULE_MODE_STOCK = 1;
// Handicap off, and a damage ratio of 1.0 (stored x10), as the online rules' locks set them.
constexpr u8 HANDICAP_OFF = 0;
constexpr u8 RATIO_ONE = 10;
// gmGlobalRecord's menu data: the item frequency a match takes (u8, 0 None), as RSBE01.patches'
// items group and OnlineRules.cpp's MENU_ITEM_FREQUENCY use it.
constexpr u32 RECORD_ITEM_FREQUENCY = 0x9017BE50;
constexpr u8 ITEMS_OFF = 0;
// Readings waiting to become final. Far more than a rollback window or a joiner's catch-up, since
// confirmations come every frame.
constexpr size_t MAX_PENDING = 4096;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

// The struct a g_GameGlobal field points to, or 0.
u32 GlobalField(const GuestMemory& m, u32 field)
{
  if (!Pointer(m, GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + field))
    return 0;
  const u32 p = m.Read32(global + field);
  return Pointer(m, p) ? p : 0;
}

std::atomic<bool> s_rules_pending{false};

// mod.json's rankedRules (Mod.h): the first rule this match was not played under, or empty: its
// mod.json fields ("stocks", "timeMinutes", "items"), then two the engine fixes for every ranked
// game ("handicap" off, "damageRatio" 1.0). Read on the results screen from the rules the fight
// started with: gmSetRule, the block OnlineRules' locks write (mode, stocks, handicap, damage ratio,
// the stock match's time limit), which only the Rules menu changes, and the record's item
// frequency. A rule that can't be read differs.
std::string_view DifferingRule(const GuestMemory& m, const Orca::Mod::RankedRules& rules)
{
  const auto byte = [&m](u32 address) -> std::optional<u8> {
    return address != 0 && m.Valid(address) ? std::optional(m.Read8(address)) : std::nullopt;
  };
  const u32 rule = GlobalField(m, GLOBAL_SET_RULE);
  const std::optional<u8> mode = byte(rule ? rule + RULE_MODE : 0);
  if (!mode || (*mode & RULE_MODE_MASK) != RULE_MODE_STOCK ||
      byte(rule + RULE_STOCKS) != rules.stocks)
  {
    return "stocks";
  }
  if (byte(rule + RULE_STOCK_MINUTES) != rules.time_minutes)
    return "timeMinutes";
  if (byte(RECORD_ITEM_FREQUENCY) != ITEMS_OFF)
    return "items";
  if (byte(rule + RULE_HANDICAP) != HANDICAP_OFF)
    return "handicap";
  if (byte(rule + RULE_RATIO) != RATIO_ONE)
    return "damageRatio";
  return {};
}
}  // namespace

GameResult FromRecord(const SetBlock::SetState& set, int index)
{
  GameResult g;
  if (index < 0 || index >= SetBlock::MAX_RECORDS)
  {
    g.why = "limit";
    return g;
  }
  // A recorded game: the block, not the room's tally, ends its set (a "limit" game above never did).
  g.from_set_block = true;
  const SetBlock::GameRecord& r = set.records[index];
  const auto value = [](u8 v) { return v == 0xFF ? -1 : int(v); };
  g.start = static_cast<int>(r.start & 0x7FFFFFFF);
  g.frame = g.start;
  g.number = index + 1;
  g.how = static_cast<int>(r.how);
  g.tiebreak = r.tiebreak;
  g.timeout = r.timeout;
  g.stage = value(r.stage);
  for (int i = 0; i < 2; ++i)
  {
    g.characters[i] = value(r.characters[i]);
    g.stocks[i] = value(r.stocks[i]);
    g.ledge[i] = r.ledge[i];
  }
  if (r.winner < 2)
  {
    g.kind = GameResult::Kind::Win;
    g.winner_port = r.winner;
  }
  else
  {
    g.kind = GameResult::Kind::Draw;
  }
  return g;
}

Reading ReadResults(const GuestMemory& m, const std::optional<Orca::Mod::RankedRules>& ranked_rules)
{
  Reading r;
  const std::string scene = ReadSceneName(m);
  if (scene == "scMelee")
    r.scene = Reading::Scene::Fight;
  else if (scene == "scMemoryChange")
    r.scene = Reading::Scene::Between;
  else if (scene == "scVsResult")
    r.scene = Reading::Scene::Results;
  if (r.scene != Reading::Scene::Results)
    return r;
  const u32 info = GlobalField(m, GLOBAL_RESULT_INFO);
  if (!info || !m.Valid(info + RESULT_PORTS + 4 * RESULT_PORT_SIZE))
    return r;
  r.block.mode = m.Read8(info + RESULT_MODE);
  if (ranked_rules)
    r.block.differing_rule = DifferingRule(m, *ranked_rules);
  r.block.stage = m.Read16(info + RESULT_STAGE);
  r.block.end = m.Read16(info + RESULT_END);
  if (m.Valid(info + RESULT_DECISION))
    r.block.decision = m.Read8(info + RESULT_DECISION);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 rec = info + RESULT_PORTS + port * RESULT_PORT_SIZE;
    PortResult& p = r.block.ports[port];
    const u8 type = m.Read8(rec + PORT_TYPE);
    p.present = type != TYPE_NONE;
    p.human = type == TYPE_HUMAN;
    if (!p.present)
      continue;
    p.character = m.Read8(rec + PORT_CHARACTER);
    p.team = m.Read8(rec + PORT_TEAM);
    const u8 stocks = m.Read8(rec + PORT_STOCKS);
    p.stocks = stocks == 0xFF ? -1 : stocks;
    p.place = m.Read8(rec + PORT_PLACE);
  }
  return r;
}

GameResult Judge(const ResultBlock& block, int frame)
{
  GameResult g;
  g.frame = frame;
  if (!block.differing_rule.empty())
  {
    g.why = fmt::format("rules-{}", block.differing_rule);
    return g;
  }
  // No contest: decision 9 is the fight's initial value, and quitting from pause leaves it. With
  // no decision read, fall back to +0x0E..+0x0F != 1 (players tied for first, so a tied time-up
  // is void too).
  if (block.decision == 9 || (block.decision == 0 && block.end != 1))
  {
    g.why = "nocontest";
    return g;
  }
  const auto& p = block.ports;
  // Retained four-machine result dumps (friendlies-0348 case 3) identify +0x06 as the
  // native team colour. Placement names the surviving winner, not necessarily their partner.
  if (p[2].present || p[3].present)
  {
    g.players = 4;
    std::array<int, 3> teams{};
    int first = -1, winning_team = -1;
    bool tied = false;
    for (int port = 0; port < 4; ++port)
    {
      if (!p[port].present || !p[port].human)
      {
        g.why = "ports";
        return g;
      }
      if (p[port].team < 0 || p[port].team > 2)
      {
        g.why = "teams";
        return g;
      }
      ++teams[p[port].team];
      g.characters[port] = p[port].character;
      g.stocks[port] = p[port].stocks;
      if (p[port].place == 0)
      {
        if (first < 0)
        {
          first = port;
          winning_team = p[port].team;
        }
        else
          tied |= winning_team != p[port].team;
      }
    }
    int pairs = 0;
    for (int count : teams)
      pairs += count == 2;
    if (pairs != 2 || first < 0)
    {
      g.why = "teams";
      return g;
    }
    g.stage = block.stage;
    g.timeout = block.mode == 0 || block.decision == 1;
    if (tied)
      g.kind = GameResult::Kind::Draw;
    else
    {
      g.kind = GameResult::Kind::Win;
      g.winner_port = first;
      for (int port = 0; port < 4; ++port)
        if (p[port].team == winning_team)
          g.winning_ports |= 1u << port;
    }
    return g;
  }
  if (!p[0].present || !p[1].present || !p[0].human || !p[1].human || p[2].present || p[3].present)
  {
    g.why = "ports";
    return g;
  }
  g.stage = block.stage;
  for (int i = 0; i < 2; ++i)
  {
    g.characters[i] = p[i].character;
    g.stocks[i] = p[i].stocks;
  }
  // Out of time: a timed match always; a stock match when nobody ran out of stocks.
  g.timeout = block.mode == 0 || (p[0].stocks > 0 && p[1].stocks > 0);
  const bool first0 = p[0].place == 0, first1 = p[1].place == 0;
  if (first0 && first1)
  {
    g.kind = GameResult::Kind::Draw;
  }
  else if (first0 || first1)
  {
    g.kind = GameResult::Kind::Win;
    g.winner_port = first0 ? 0 : 1;
  }
  else
  {
    g.why = "nocontest";
  }
  return g;
}

void ResultsTracker::Reset()
{
  m_pending.clear();
  m_have_last = false;
  m_last_frame = -1;
  m_last = {};
  m_in_game = false;
  m_after_fight = false;
  m_scene_start = -1;
  m_have_set = false;
  m_set_games = 0;
  m_have_fight = false;
  m_fight_start = -1;
}

std::optional<Reading::Scene> ResultsTracker::FinalScene() const
{
  if (!m_have_last)
    return std::nullopt;
  return m_last.scene;
}

std::optional<Reading::Scene> ResultsTracker::LatestScene() const
{
  if (!m_pending.empty())
    return m_pending.rbegin()->second.scene;
  return FinalScene();
}

void ResultsTracker::Store(int frame, const Reading& reading, u64 resyncs)
{
  if (!m_started || resyncs != m_resyncs)
  {
    Reset();
    m_started = true;
    m_resyncs = resyncs;
  }
  // Final frames are never stored again: a rollback never reaches a confirmed frame.
  if (m_have_last && frame <= m_last_frame)
    return;
  m_pending[frame] = reading;
  while (m_pending.size() > MAX_PENDING)
    m_pending.erase(m_pending.begin());
}

void ResultsTracker::Rebase(int frame, const Reading& reading, u64 resyncs)
{
  int scene_start = -1;
  if (m_have_last && !m_last.set && (frame == m_last_frame || frame == m_last_frame + 1))
    scene_start = m_scene_start;
  Reset();
  m_started = true;
  m_resyncs = resyncs;
  m_have_last = true;
  m_last_frame = frame;
  m_last = reading;
  if (reading.set)
  {
    m_have_set = true;
    m_set_games = reading.set->games;
    // A fight already begun (fighters in) at the rebuilt frame was not this machine's to begin.
    if (reading.set->fight && (reading.set->fight_flags & SetBlock::FIGHT_SEEN))
    {
      m_have_fight = true;
      m_fight_start = static_cast<int>(reading.set->fight_start & 0x7FFFFFFF);
    }
  }
  else if (reading.scene == Reading::Scene::Fight)
  {
    // This fight is already underway, like FIGHT_SEEN above: do not begin it again, even if the
    // next scene transition is sudden death. Rebuilt history never counts as a new game.
    m_scene_start = scene_start >= 0 ? scene_start : frame;
  }
  else if (reading.scene == Reading::Scene::Between && scene_start >= 0)
  {
    m_scene_start = scene_start;
    m_after_fight = true;
  }
}

std::vector<GameResult> ResultsTracker::Confirm(int confirmed, int plug_frame,
                                                std::vector<int>* starts)
{
  using Scene = Reading::Scene;
  std::vector<GameResult> out;
  for (auto it = m_pending.begin(); it != m_pending.end() && it->first <= confirmed;
       it = m_pending.erase(it))
  {
    const int frame = it->first;
    const Reading& r = it->second;
    // A gap (frames this machine never read) ends whatever game was being followed.
    const bool follows = m_have_last && frame == m_last_frame + 1;
    if (!follows)
    {
      m_in_game = false;
      m_after_fight = false;
      m_scene_start = -1;
    }
    const Scene before = follows ? m_last.scene : Scene::Other;
    if (r.set)
    {
      // Ranked set: the match block (RankedSet.h) judges games inside the game. Report only games
      // whose fight began after the opponent's plug frame, and none from before the first final
      // reading.
      const SetBlock::SetState& set = *r.set;
      // A game began once its fighters are in (FIGHT_SEEN, after the stage's loading image: a
      // crash or a stall while it loads leaves the set void). The fight's first frame names it, as
      // FromRecord's start does. A sudden death keeps the fight and its first frame, so it is the
      // same game.
      if (set.fight && (set.fight_flags & SetBlock::FIGHT_SEEN))
      {
        const int start = static_cast<int>(set.fight_start & 0x7FFFFFFF);
        if (!m_have_fight || start != m_fight_start)
        {
          m_have_fight = true;
          m_fight_start = start;
          if (starts && start - 1 > plug_frame)
            starts->push_back(start);
        }
      }
      if (!m_have_set || set.games < m_set_games)
      {
        m_have_set = true;
        m_set_games = set.games;
      }
      for (; m_set_games < set.games; ++m_set_games)
      {
        GameResult g = FromRecord(set, m_set_games);
        if (m_set_games + 1 == set.games && set.done)
        {
          g.set_done = set.done;
          g.set_winner = set.done == 1 && set.winner < 2 ? set.winner : -1;
        }
        if (g.start >= 0 && g.start - 1 <= plug_frame)
          continue;
        out.push_back(std::move(g));
      }
      m_in_game = false;
      m_after_fight = false;
      m_scene_start = -1;
      m_last = r;
      m_last_frame = frame;
      m_have_last = true;
      continue;
    }
    m_have_set = false;
    switch (r.scene)
    {
    case Scene::Fight:
      if (before != Scene::Fight)
      {
        if (m_scene_start >= 0 && m_after_fight)
        {
          // Back into the fight from between scenes (sudden death): the same game.
          m_after_fight = false;
        }
        else
        {
          // A new game, counted only if the previous frame was read on both machines.
          m_in_game = follows && frame - 1 > plug_frame;
          m_after_fight = false;
          m_scene_start = frame;
          if (m_in_game && starts)
            starts->push_back(m_scene_start);
        }
      }
      break;
    case Scene::Between:
      if (m_scene_start >= 0 && before == Scene::Fight)
        m_after_fight = true;
      break;
    case Scene::Results:
      if (m_in_game)
      {
        GameResult g = Judge(r.block, frame);
        g.start = m_scene_start;
        out.push_back(std::move(g));
        if (!r.block.differing_rule.empty())
        {
          NOTICE_LOG_FMT(ROLLBACK, "Orca result: frame {}: not rated, its {} is not the ranked rules'",
                         frame, r.block.differing_rule);
        }
      }
      m_in_game = false;
      m_scene_start = -1;
      m_after_fight = false;
      break;
    case Scene::Other:
      if (m_in_game)
      {
        // The fight ended without its results screen.
        GameResult g;
        g.frame = frame;
        g.start = m_scene_start;
        g.why = "nocontest";
        out.push_back(g);
      }
      m_in_game = false;
      m_scene_start = -1;
      m_after_fight = false;
      break;
    }
    m_last = r;
    m_last_frame = frame;
    m_have_last = true;
  }
  return out;
}

ResultsTracker& Tracker()
{
  static ResultsTracker tracker;
  return tracker;
}

bool ResultsVerified()
{
  // Brawl rev 2 or a launcher booting its executable (Project+), whose scenes and result info
  // match, while the module's addresses hold (BrawlGame.h).
  return Builtin().Active() && Builtin().Describe().results;
}

void ReadResultsFrame(const Core::CPUThreadGuard& guard, int frame)
{
  if (!ResultsVerified())
    return;
  GuardMemory memory(guard);
  const Orca::Mod::ModInfo* mod = Orca::Mod::ActiveMod();
  Reading reading = ReadResults(memory, mod ? mod->manifest.ranked_rules : std::nullopt);
  // Only a game with a ruleset writes a ranked set into the match block; without one that memory
  // is the game's own (brawl-rev2-results), whatever it holds.
  if (Builtin().Describe().ruleset != Orca::Engine::Ruleset::None)
  {
    if (const auto header = SetBlock::ReadHeader(memory);
        header && header->mode == SetBlock::Mode::Ranked)
    {
      reading.set = SetBlock::ReadSet(memory);
    }
  }
  if (Orca::Net::ReplayScope::NetworkPlaying())
    Tracker().Rebase(frame, reading, Orca::Events::Resyncs());
  else
    Tracker().Store(frame, reading, Orca::Events::Resyncs());
}

void RequestQueueRules()
{
  // Vanilla Brawl's rules screen only, while its addresses hold: the module's answer.
  if (Builtin().ProfileRuleset() == Orca::Engine::Ruleset::Brawl)
    s_rules_pending = true;
}

int WriteQueueRules(GuestMemory& memory)
{
  const u32 rule = GlobalField(memory, GLOBAL_SET_RULE);
  if (!rule || !memory.Valid(rule + RULE_ITEMS))
    return 0;
  int changed = 0;
  const auto set = [&](u32 offset, u8 value) {
    if (memory.Read8(rule + offset) == value)
      return;
    memory.Write8(rule + offset, value);
    ++changed;
  };
  set(RULE_MODE, 1);
  set(RULE_STOCKS, 3);
  set(RULE_STOCK_MINUTES, 8);
  set(RULE_ITEMS, 0);
  return changed;
}

void ApplyQueueRules(const Core::CPUThreadGuard& guard, bool alone)
{
  // Only right after the host takes its room. A later write could land after a keyframe the
  // joiner loads.
  if (!s_rules_pending.exchange(false))
    return;
  if (!alone)
  {
    WARN_LOG_FMT(ROLLBACK, "Matchmaking: no rules preset: the game wasn't alone");
    return;
  }
  GuardMemory memory(guard);
  const int changed = WriteQueueRules(memory);
  NOTICE_LOG_FMT(ROLLBACK,
                 "Matchmaking: the rules preset (3 stocks, 8:00, items off), {} byte(s) "
                 "changed",
                 changed);
}
}  // namespace Orca::UX
