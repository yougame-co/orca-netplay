// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Ranked sets (ORCA.md "Ranked sets"): the match block's layout, the set followed in the game's
// memory, each ruleset's verdict, and the tracker's reports from the block.

#include <array>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/RankedSet.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"

using namespace Orca::UX;
using SetBlock::GameRecord;
using SetBlock::How;
using SetBlock::Mode;
using SetBlock::NO_PORT;
using SetBlock::Ruleset;
using SetBlock::SetState;
using Scene = Reading::Scene;

namespace
{
class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    ++writes;
  }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  std::map<u32, u8> bytes;
  int writes = 0;
};

// The block as the game leaves it before Orca writes it: dead code, not zeros.
FakeMemory Block()
{
  FakeMemory m;
  for (u32 i = 0; i < SetBlock::SIZE; ++i)
    m.bytes[SetBlock::BASE + i] = static_cast<u8>(0x3A + i * 7);
  return m;
}

// A 1v1's result block: two humans on ports 1 and 2.
ResultBlock Result(int place0, int place1, int stocks0 = 1, int stocks1 = 0, int character0 = 0,
                   int character1 = 3)
{
  ResultBlock b;
  b.mode = 1;
  b.stage = 2;
  b.end = 1;
  b.decision = 2;
  b.ports[0] = {true, true, character0, stocks0, place0};
  b.ports[1] = {true, true, character1, stocks1, place1};
  return b;
}

RankedSet::Facts At(Scene scene)
{
  RankedSet::Facts f;
  f.scene = scene;
  return f;
}

// One whole game through Step: CSS, between, the fight, between, the results screen.
SetState Play(SetState s, Ruleset ruleset, u32 start, const ResultBlock& result,
              const std::vector<RankedSet::Live>& live = {}, bool sudden = false)
{
  s = RankedSet::Step(s, ruleset, At(Scene::Other), start - 2);
  s = RankedSet::Step(s, ruleset, At(Scene::Between), start - 1);
  u32 frame = start;
  s = RankedSet::Step(s, ruleset, At(Scene::Fight), frame++);
  for (const RankedSet::Live& l : live)
  {
    RankedSet::Facts f = At(Scene::Fight);
    f.live = l;
    f.live_read = true;
    s = RankedSet::Step(s, ruleset, f, frame++);
  }
  s = RankedSet::Step(s, ruleset, At(Scene::Between), frame++);
  if (sudden)
  {
    s = RankedSet::Step(s, ruleset, At(Scene::Fight), frame++);
    s = RankedSet::Step(s, ruleset, At(Scene::Between), frame++);
  }
  RankedSet::Facts results = At(Scene::Results);
  results.block = result;
  results.decision = static_cast<u8>(result.decision);
  s = RankedSet::Step(s, ruleset, results, frame++);
  // The results screen stays: nothing more is recorded.
  s = RankedSet::Step(s, ruleset, results, frame++);
  return s;
}

RankedSet::Live LiveAt(bool time_up, u8 stocks0, u8 stocks1, u16 percent0, u16 percent1,
                       bool ledge0 = false, bool ledge1 = false)
{
  RankedSet::Live l;
  l.valid = true;
  l.time_up = time_up;
  l.stocks = {stocks0, stocks1};
  l.percent = {percent0, percent1};
  l.on_ledge = {ledge0, ledge1};
  return l;
}

// `grabs` ledge grabs by `port`: on the ledge, off it, again.
std::vector<RankedSet::Live> Grabs(int port, int grabs)
{
  std::vector<RankedSet::Live> out;
  for (int i = 0; i < grabs; ++i)
  {
    out.push_back(LiveAt(false, 3, 3, 0, 0, port == 0, port == 1));
    out.push_back(LiveAt(false, 3, 3, 0, 0));
  }
  return out;
}
}  // namespace

TEST(OrcaSetBlock, HeaderRoundTripsAndStartsAFreshSet)
{
  FakeMemory m = Block();
  EXPECT_FALSE(SetBlock::ReadHeader(m));
  EXPECT_GT(SetBlock::WriteHeader(m, {Mode::Ranked, Ruleset::Brawl}), 0);
  ASSERT_TRUE(SetBlock::ReadHeader(m));
  EXPECT_EQ(*SetBlock::ReadHeader(m), (SetBlock::Header{Mode::Ranked, Ruleset::Brawl}));
  EXPECT_EQ(SetBlock::ReadSet(m), SetState{});
  // The same header again writes nothing and keeps the set.
  SetState s;
  s.games = 1;
  s.wins = {1, 0};
  SetBlock::WriteSet(m, s);
  EXPECT_EQ(SetBlock::WriteHeader(m, {Mode::Ranked, Ruleset::Brawl}), 0);
  EXPECT_EQ(SetBlock::ReadSet(m), s);
  // Another mode starts a fresh set.
  EXPECT_GT(SetBlock::WriteHeader(m, {Mode::Casual, Ruleset::Brawl}), 0);
  EXPECT_EQ(SetBlock::ReadSet(m), SetState{});
  EXPECT_GT(SetBlock::Clear(m), 0);
  EXPECT_FALSE(SetBlock::ReadHeader(m));
  EXPECT_EQ(SetBlock::Clear(m), 0);
}

TEST(OrcaSetBlock, SetRoundTripsAndWritesOnlyWhatChanged)
{
  FakeMemory m = Block();
  SetBlock::WriteHeader(m, {Mode::Ranked, Ruleset::PPlus});
  SetState s;
  s.games = 3;
  s.wins = {2, 1};
  s.done = 1;
  s.winner = 0;
  s.last_winner = 0;
  s.fight_start = 0x12345678;
  s.snap_percent = {123, 456};
  s.ledge = {36, 2};
  s.records[0] = {100, 0, How::Results, 2, false, false, {0, 3}, {1, 0}, {0, 0}};
  s.records[1] = {200, 1, How::Percent, 0x1F, true, true, {0x16, 3}, {1, 1}, {21, 4}};
  EXPECT_GT(SetBlock::WriteSet(m, s), 0);
  EXPECT_EQ(SetBlock::ReadSet(m), s);
  m.writes = 0;
  EXPECT_EQ(SetBlock::WriteSet(m, s), 0);
  EXPECT_EQ(m.writes, 0);
  EXPECT_EQ(s.StagesWonBy(0), u64(1) << 2);
  EXPECT_EQ(s.StagesWonBy(1), u64(1) << 0x1F);
  // Nothing outside the block.
  for (const auto& [a, v] : m.bytes)
    EXPECT_TRUE(a >= SetBlock::BASE && a < SetBlock::BASE + SetBlock::SIZE);
}

TEST(OrcaRankedSet, BestOfThree)
{
  SetState s;
  s = Play(s, Ruleset::Brawl, 1000, Result(0, 1));
  EXPECT_EQ(s.games, 1);
  EXPECT_EQ(s.wins, (std::array<u8, 2>{1, 0}));
  EXPECT_EQ(s.last_winner, 0);
  EXPECT_EQ(s.records[0].start, 1000u);
  EXPECT_EQ(s.records[0].winner, 0);
  EXPECT_EQ(s.records[0].how, How::Results);
  EXPECT_EQ(s.GameNumber(), 2);
  EXPECT_FALSE(s.done);
  s = Play(s, Ruleset::Brawl, 5000, Result(1, 0, 0, 2));
  EXPECT_EQ(s.wins, (std::array<u8, 2>{1, 1}));
  EXPECT_EQ(s.last_winner, 1);
  EXPECT_EQ(s.StagesWonBy(1), u64(1) << 2);
  s = Play(s, Ruleset::Brawl, 9000, Result(1, 0, 0, 1));
  EXPECT_EQ(s.done, 1);
  EXPECT_EQ(s.winner, 1);
  EXPECT_EQ(s.games, 3);
  // A finished set follows nothing more.
  const SetState after = Play(s, Ruleset::Brawl, 13000, Result(0, 1));
  EXPECT_EQ(after.games, 3);
  EXPECT_FALSE(after.fight);
}

TEST(OrcaRankedSet, VoidGamesAreNotRecorded)
{
  SetState s;
  ResultBlock cpu = Result(0, 1);
  cpu.ports[1].human = false;
  s = Play(s, Ruleset::Brawl, 1000, cpu);
  EXPECT_EQ(s.games, 0);
  ResultBlock quit = Result(0, 1);
  quit.end = 2;
  quit.decision = 9;
  s = Play(s, Ruleset::Brawl, 2000, quit);
  EXPECT_EQ(s.games, 0);
  // A fight left for the menus without its results screen.
  s = RankedSet::Step(s, Ruleset::Brawl, At(Scene::Fight), 3000);
  EXPECT_TRUE(s.fight);
  s = RankedSet::Step(s, Ruleset::Brawl, At(Scene::Other), 3001);
  EXPECT_FALSE(s.fight);
  EXPECT_EQ(s.games, 0);
}

TEST(OrcaRankedSet, BrawlTimeOutLedgeLimitThenStocksThenPercent)
{
  // Port 1 grabs the ledge 36 times (over 35): port 2 wins although port 1 had more stocks.
  std::vector<RankedSet::Live> live = Grabs(0, 36);
  live.push_back(LiveAt(true, 3, 2, 10, 20));
  SetState s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 3, 2), live);
  EXPECT_EQ(s.records[0].ledge, (std::array<u8, 2>{36, 0}));
  EXPECT_EQ(s.records[0].winner, 1);
  EXPECT_EQ(s.records[0].how, How::Ledge);
  EXPECT_TRUE(s.records[0].timeout);
  // 35 is within the limit: stocks decide.
  live = Grabs(0, 35);
  live.push_back(LiveAt(true, 3, 2, 10, 20));
  s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 3, 2), live);
  EXPECT_EQ(s.records[0].winner, 0);
  EXPECT_EQ(s.records[0].how, How::Stocks);
  // Meta Knight's limit is 20.
  live = Grabs(0, 21);
  live.push_back(LiveAt(true, 3, 2, 10, 20));
  s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 3, 2, static_cast<u8>(Rulesets::Brawl2025().meta_knight)), live);
  EXPECT_EQ(s.records[0].how, How::Ledge);
  EXPECT_EQ(s.records[0].winner, 1);
  // Both over: the limit is ignored.
  live = Grabs(0, 40);
  for (const auto& l : Grabs(1, 40))
    live.push_back(l);
  live.push_back(LiveAt(true, 2, 3, 10, 20));
  s = Play({}, Ruleset::Brawl, 1000, Result(1, 0, 2, 3), live);
  EXPECT_EQ(s.records[0].how, How::Stocks);
  EXPECT_EQ(s.records[0].winner, 1);
  // Level on stocks at time-up (sudden death follows, and is ignored): lower percent wins.
  live = {LiveAt(false, 2, 2, 10, 20), LiveAt(true, 2, 2, 88, 40)};
  s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 1, 0), live, true);
  EXPECT_EQ(s.records[0].how, How::Percent);
  EXPECT_EQ(s.records[0].winner, 1);
  EXPECT_EQ(s.wins, (std::array<u8, 2>{0, 1}));
  // What happens after time-up never changes the snapshot.
  live = {LiveAt(true, 2, 2, 40, 88), LiveAt(true, 1, 2, 300, 300)};
  s = Play({}, Ruleset::Brawl, 1000, Result(1, 0, 0, 1), live, true);
  EXPECT_EQ(s.records[0].how, How::Percent);
  EXPECT_EQ(s.records[0].winner, 0);
}

TEST(OrcaRankedSet, BrawlTieMeansATiebreakGame)
{
  // Level on everything at time-up.
  std::vector<RankedSet::Live> live = {LiveAt(true, 2, 2, 50, 50)};
  SetState s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 1, 0), live, true);
  EXPECT_EQ(s.games, 1);
  EXPECT_EQ(s.records[0].winner, NO_PORT);
  EXPECT_EQ(s.records[0].how, How::Tie);
  EXPECT_TRUE(s.tiebreak);
  EXPECT_EQ(s.tb_streak, 1);
  EXPECT_EQ(s.wins, (std::array<u8, 2>{0, 0}));
  EXPECT_EQ(s.GameNumber(), 1);
  // The tiebreak game is played with 1 stock and 3:00.
  FakeMemory m;
  for (u32 a : {0x805A00E0u, 0x805A00E1u, 0x805A00E2u, 0x805A00E3u})
    m.bytes[a] = 0;
  m.Write32(0x805A00E0, 0x90181300);
  for (u32 i = 0; i < 0x20; ++i)
    m.bytes[0x90181300 + i] = 0;
  m.Write32(0x9018131C, 0x9017F360);
  for (u32 i = 0; i < 0x10; ++i)
    m.bytes[0x9017F360 + i] = 0;
  EXPECT_EQ(RankedSet::WriteSetRules(m, s, Rulesets::Brawl2025()), 3);
  EXPECT_EQ(m.Read8(0x9017F362), 1);
  EXPECT_EQ(m.Read8(0x9017F364), 1);
  EXPECT_EQ(m.Read8(0x9017F368), 3);
  EXPECT_EQ(RankedSet::WriteSetRules(m, s, Rulesets::Brawl2025()), 0);
  // The tiebreak game's ledge limit is 11.
  live = Grabs(1, 12);
  live.push_back(LiveAt(true, 1, 1, 50, 60));
  SetState t = Play(s, Ruleset::Brawl, 5000, Result(0, 1, 1, 1), live);
  EXPECT_TRUE(t.records[1].tiebreak);
  EXPECT_EQ(t.records[1].how, How::Ledge);
  EXPECT_EQ(t.records[1].winner, 0);
  EXPECT_EQ(t.wins, (std::array<u8, 2>{1, 0}));
  EXPECT_FALSE(t.tiebreak);
  EXPECT_EQ(t.tb_streak, 0);
  EXPECT_EQ(t.GameNumber(), 2);
  EXPECT_EQ(RankedSet::WriteSetRules(m, t, Rulesets::Brawl2025()), 2);
  EXPECT_EQ(m.Read8(0x9017F364), 3);
  EXPECT_EQ(m.Read8(0x9017F368), 8);
  // A simultaneous last-stock loss (sudden death, no time-up): a tie as well.
  SetState u = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 1, 0), {}, true);
  EXPECT_EQ(u.records[0].how, How::Tie);
  EXPECT_TRUE(u.tiebreak);
  // Five ties in a row void the set.
  SetState v;
  for (int i = 0; i < Rulesets::Brawl2025().void_after_ties; ++i)
    v = Play(v, Ruleset::Brawl, 1000 + 1000 * i, Result(0, 1, 1, 0), {}, true);
  EXPECT_EQ(v.done, 2);
  EXPECT_EQ(v.winner, NO_PORT);
  EXPECT_FALSE(v.tiebreak);
}

TEST(OrcaRankedSet, BrawlSuddenDeathInTheSameFightScene)
{
  // As a level time-up leaves the result info: two players tied for first (+0x0F), decision 1.
  ResultBlock level = Result(0, 1, 1, 0);
  level.end = 2;
  level.decision = 1;
  {
    const RankedSet::Live gone{};
    SetState s = Play({}, Ruleset::Brawl, 1000, level,
                      {LiveAt(true, 1, 1, 12, 5), gone, LiveAt(false, 1, 1, 300, 300)});
    EXPECT_EQ(s.games, 1);
    EXPECT_EQ(s.records[0].how, How::Percent);
    EXPECT_EQ(s.records[0].winner, 1);
  }
  // Brawl's time-up sudden death stays in scMelee: the fighters are gone for a few frames, then
  // rebuilt with 1 stock each. Level stocks at time-up: the lower percent wins, whoever wins the
  // sudden death; and the ledge grabs after time-up don't count.
  const RankedSet::Live gone{};
  std::vector<RankedSet::Live> live = {LiveAt(false, 2, 2, 10, 20), LiveAt(true, 2, 2, 30, 20),
                                       gone, gone, LiveAt(false, 1, 1, 300, 300)};
  for (const auto& l : Grabs(1, 40))
    live.push_back(l);
  SetState s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 1, 0), live);
  EXPECT_EQ(s.records[0].how, How::Percent);
  EXPECT_EQ(s.records[0].winner, 1);
  EXPECT_EQ(s.records[0].ledge, (std::array<u8, 2>{0, 0}));
  // A simultaneous last-stock loss: the fighters rebuilt with no time-up seen, a tie.
  live = {LiveAt(false, 1, 1, 80, 90), gone, LiveAt(false, 1, 1, 300, 300)};
  s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 1, 0), live);
  EXPECT_EQ(s.records[0].how, How::Tie);
  EXPECT_TRUE(s.tiebreak);
  // The fighters gone at the fight's end (no sudden death): the results screen's winner.
  live = {LiveAt(false, 1, 1, 80, 90), gone, gone};
  s = Play({}, Ruleset::Brawl, 1000, Result(1, 0, 0, 1), live);
  EXPECT_EQ(s.records[0].how, How::Results);
  EXPECT_EQ(s.records[0].winner, 1);
  // Before the fighters are first read (the fight's first frames), nothing is gone.
  live = {gone, gone, LiveAt(false, 3, 3, 0, 0), gone};
  s = Play({}, Ruleset::Brawl, 1000, Result(0, 1, 1, 0), live);
  EXPECT_EQ(s.records[0].how, How::Results);
}

TEST(OrcaRankedSet, BrawlTimeOutWithoutTheClockUsesTheGamesStocks)
{
  // The game's own time-up (decision 1) with no live reading: more stocks, as the result info says.
  ResultBlock r = Result(1, 0, 1, 2);
  r.decision = 1;
  SetState s = Play({}, Ruleset::Brawl, 1000, r);
  EXPECT_EQ(s.records[0].how, How::Stocks);
  EXPECT_EQ(s.records[0].winner, 1);
  EXPECT_TRUE(s.records[0].timeout);
}

TEST(OrcaRankedSet, ProjectPlusKeepsItsOwnTimeOutRules)
{
  std::vector<RankedSet::Live> live = Grabs(0, 80);
  live.push_back(LiveAt(true, 2, 2, 10, 90));
  ResultBlock r = Result(0, 1, 2, 2);
  r.decision = 1;
  SetState s = Play({}, Ruleset::PPlus, 1000, r, live);
  EXPECT_EQ(s.records[0].how, How::Results);
  EXPECT_EQ(s.records[0].winner, 0);
  EXPECT_FALSE(s.tiebreak);
  // The live fight is read only to see the fighters in (the game began): no ledge grabs or time-up
  // snapshot are counted.
  EXPECT_EQ(s.records[0].ledge, (std::array<u8, 2>{0, 0}));
  // A tie (overtime should have prevented it) is recorded without a tiebreak game.
  s = Play({}, Ruleset::PPlus, 1000, Result(0, 0, 1, 1));
  EXPECT_EQ(s.records[0].how, How::Tie);
  EXPECT_FALSE(s.tiebreak);
}

// A ranked header whose ruleset byte names no ruleset (Orca never writes one) keeps the tracker's
// behaviour from before rulesets were data: the results screen decides a time-out, two wins take the
// set, five ties void it, no tiebreak game, and ledge grabs aren't counted.
TEST(OrcaRankedSet, NoRulesetKeepsTheOldDefaults)
{
  std::vector<RankedSet::Live> live = Grabs(0, 40);
  live.push_back(LiveAt(true, 3, 1, 10, 90));
  ResultBlock timeout = Result(1, 0, 3, 1);
  timeout.decision = 1;
  SetState s = Play({}, Ruleset::None, 1000, timeout, live);
  ASSERT_EQ(s.games, 1);
  EXPECT_EQ(s.records[0].how, How::Results);
  EXPECT_EQ(s.records[0].winner, 1);
  EXPECT_EQ(s.records[0].ledge, (std::array<u8, 2>{0, 0}));
  s = Play(s, Ruleset::None, 5000, Result(1, 0));
  EXPECT_EQ(s.done, 1);
  EXPECT_EQ(s.winner, 1);
  SetState t;
  for (int i = 0; i < 5; ++i)
  {
    t = Play(t, Ruleset::None, 1000 + 1000 * i, Result(0, 0, 1, 1));
    EXPECT_FALSE(t.tiebreak);
  }
  EXPECT_EQ(t.done, 2);
}

// A ranked game begins once its fighters are in (FIGHT_SEEN), not at the fight scene's first frame:
// the stage's loading image comes first (Brawl to frame ~169, Project+ to 27), and a crash or a
// stall there leaves the set void (YouGame docs/ORCA_ONLINE_UX.md "3B leaving mid-game").
TEST(OrcaRankedSet, AGameBeginsWhenItsFightersAreIn)
{
  for (const Ruleset ruleset : {Ruleset::Brawl, Ruleset::PPlus})
  {
    SCOPED_TRACE(static_cast<int>(ruleset));
    SetState s;
    s = RankedSet::Step(s, ruleset, At(Scene::Between), 99);
    // The loading image: the fight is followed from its first frame, but nobody is in yet, read or
    // not.
    RankedSet::Facts loading = At(Scene::Fight);
    s = RankedSet::Step(s, ruleset, loading, 100);
    loading.live_read = true;
    for (u32 f = 101; f < 270; ++f)
      s = RankedSet::Step(s, ruleset, loading, f);
    EXPECT_TRUE(s.fight);
    EXPECT_EQ(s.fight_start, 100u);
    EXPECT_FALSE(s.fight_flags & SetBlock::FIGHT_SEEN);
    // The fighters are in: the game began, still named by the fight's first frame.
    RankedSet::Facts in = At(Scene::Fight);
    in.live = LiveAt(false, 3, 3, 0, 0, true, false);
    in.live_read = true;
    s = RankedSet::Step(s, ruleset, in, 270);
    EXPECT_TRUE(s.fight_flags & SetBlock::FIGHT_SEEN);
    EXPECT_EQ(s.fight_start, 100u);
    // They vanish (Brawl's sudden death) and come back: the same game, still begun.
    s = RankedSet::Step(s, ruleset, loading, 271);
    EXPECT_TRUE(s.fight_flags & SetBlock::FIGHT_SEEN);
    s = RankedSet::Step(s, ruleset, in, 272);
    EXPECT_TRUE(s.fight_flags & SetBlock::FIGHT_SEEN);
    EXPECT_EQ(s.fight_start, 100u);
    if (ruleset == Ruleset::Brawl)
    {
      EXPECT_TRUE(s.fight_flags & SetBlock::FIGHT_SUDDEN);
      EXPECT_EQ(s.ledge[0], 1);
    }
    else
    {
      // Project+ counts nothing else from the live fight: its own codeset settles time-outs.
      EXPECT_EQ(s.fight_flags, SetBlock::FIGHT_SEEN);
      EXPECT_EQ(s.ledge, (std::array<u8, 2>{0, 0}));
    }
    // A fight left without its results screen: the next one starts unbegun.
    s = RankedSet::Step(s, ruleset, At(Scene::Other), 300);
    s = RankedSet::Step(s, ruleset, At(Scene::Fight), 400);
    EXPECT_EQ(s.fight_start, 400u);
    EXPECT_FALSE(s.fight_flags & SetBlock::FIGHT_SEEN);
  }
}

TEST(OrcaRankedSet, ScoreLine)
{
  SetState s;
  EXPECT_EQ(RankedSet::ScoreLine(s, 0), "");
  EXPECT_EQ(RankedSet::ScoreLine(s, 0, true), "");
  s.wins = {1, 0};
  EXPECT_EQ(RankedSet::ScoreLine(s, 0), "");
  EXPECT_EQ(RankedSet::ScoreLine(s, 1), "");
  // The results screen names the game just played: game 1's says Game 1.
  EXPECT_EQ(RankedSet::ScoreLine(s, 0, true), "Game 1 · 1–0");
  EXPECT_EQ(RankedSet::ScoreLine(s, 1, true), "Game 1 · 0–1");
  s.wins = {1, 1};
  EXPECT_EQ(RankedSet::ScoreLine(s, 0, true), "Game 2 · 1–1");
  EXPECT_EQ(RankedSet::ScoreLine(s, 0), "");
  s.wins = {1, 0};
  s.tiebreak = true;
  EXPECT_EQ(RankedSet::ScoreLine(s, 1), "Tiebreak game · 0–1");
  s.tiebreak = false;
  s.wins = {2, 1};
  s.done = 1;
  EXPECT_EQ(RankedSet::ScoreLine(s, 1), "Set over · 1–2");
  s.done = 2;
  EXPECT_EQ(RankedSet::ScoreLine(s, 0), "Set void · 2–1");
}

// A matchmade game's result toast: no "Game N", and the local player's score first.
TEST(OrcaRankedSet, ResultToast)
{
  EXPECT_EQ(RankedSet::ResultToast("won", 1, 0), "You won · 1–0");
  EXPECT_EQ(RankedSet::ResultToast("lost", 0, 1), "You lost · 0–1");
  EXPECT_EQ(RankedSet::ResultToast("draw", 1, 1), "Draw · played again");
  EXPECT_EQ(RankedSet::ResultToast("void", 1, 0), "That game didn't count");
  EXPECT_EQ(RankedSet::ResultToast("", 0, 0), "That game didn't count");
}

// The overlay's set dots: each decided game's winner in order, a tied game skipped, then the game
// being played next; none once the set is over.
TEST(OrcaRankedSet, DotsFollowTheDecidedGames)
{
  SetState s;
  RankedSet::Dots d = RankedSet::DotsOf(s);
  EXPECT_TRUE(d.on);
  EXPECT_EQ(d.games, (std::array<int, 3>{-1, -1, -1}));
  EXPECT_EQ(d.current, 0);
  s.games = 1;
  s.records[0].winner = 0;
  s.wins = {1, 0};
  d = RankedSet::DotsOf(s);
  EXPECT_EQ(d.games, (std::array<int, 3>{0, -1, -1}));
  EXPECT_EQ(d.current, 1);
  // A tie, then port 2's win: the tie takes no dot.
  s.games = 3;
  s.records[1].winner = NO_PORT;
  s.records[2].winner = 1;
  s.wins = {1, 1};
  d = RankedSet::DotsOf(s);
  EXPECT_EQ(d.games, (std::array<int, 3>{0, 1, -1}));
  EXPECT_EQ(d.current, 2);
  s.games = 4;
  s.records[3].winner = 0;
  s.wins = {2, 1};
  s.done = 1;
  d = RankedSet::DotsOf(s);
  EXPECT_EQ(d.games, (std::array<int, 3>{0, 1, 0}));
  EXPECT_EQ(d.current, -1);
}

TEST(OrcaRankedSet, TrackerReportsTheBlocksGames)
{
  ResultsTracker t;
  SetState s;
  int frame = 100;
  const auto store = [&](const SetState& set, Scene scene = Scene::Other) {
    Reading r;
    r.scene = scene;
    r.set = set;
    t.Store(frame++, r, 0);
  };
  // The first final reading only sets what was already there.
  s = Play(s, Ruleset::Brawl, 50, Result(0, 1));
  store(s);
  EXPECT_TRUE(t.Confirm(frame, 10).empty());
  // A game whose fight began after the opponent's plug frame (10).
  s = Play(s, Ruleset::Brawl, 2000, Result(1, 0, 0, 1));
  store(s, Scene::Results);
  store(s, Scene::Results);
  std::vector<GameResult> out = t.Confirm(frame, 10);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, GameResult::Kind::Win);
  EXPECT_EQ(out[0].winner_port, 1);
  EXPECT_EQ(out[0].start, 2000);
  EXPECT_EQ(out[0].number, 2);
  EXPECT_EQ(out[0].set_done, 0);
  EXPECT_EQ(out[0].DetailJson(),
            "{\"f\":2000,\"st\":2,\"c\":[0,3],\"s\":[0,1],\"to\":0,\"k\":1,\"lg\":[0,0],\"tb\":0}");
  // Unconfirmed readings wait.
  s = Play(s, Ruleset::Brawl, 3000, Result(0, 1));
  store(s);
  EXPECT_TRUE(t.Confirm(frame - 2, 10).empty());
  out = t.Confirm(frame, 10);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].start, 3000);
  EXPECT_EQ(out[0].set_done, 1);
  EXPECT_EQ(out[0].set_winner, 0);
  // A tie is a draw; a fight from before the plug frame isn't reported.
  ResultsTracker t2;
  SetState a;
  Reading r;
  r.set = a;
  t2.Store(1, r, 0);
  EXPECT_TRUE(t2.Confirm(1, 500).empty());
  a = Play(a, Ruleset::Brawl, 400, Result(0, 1));
  a = Play(a, Ruleset::Brawl, 1000, Result(0, 1, 1, 0), {}, true);
  r.set = a;
  t2.Store(2, r, 0);
  out = t2.Confirm(2, 500);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, GameResult::Kind::Draw);
  EXPECT_EQ(out[0].start, 1000);
  EXPECT_EQ(out[0].how, static_cast<int>(How::Tie));
}

// A ranked game began (ORCA.md "Matchmaking and results"): once the first frame with its fighters
// in is final, the tracker names it by the fight's first frame, the id the game's report carries,
// so the room's game-start goes once per game.
TEST(OrcaRankedSet, TrackerSaysWhenEachGameBegan)
{
  ResultsTracker t;
  SetState s;
  int frame = 0;
  std::vector<int> starts;
  std::vector<GameResult> games;
  // Each frame: the set block's step, its reading, and every frame up to 3 back confirmed. A fight
  // has its fighters in unless `loading`.
  const auto run = [&](Scene scene, int frames, const ResultBlock& result = Result(0, 1),
                       bool loading = false) {
    for (int i = 0; i < frames; ++i, ++frame)
    {
      RankedSet::Facts f = At(scene);
      f.block = result;
      f.decision = static_cast<u8>(result.decision);
      if (scene == Scene::Fight)
      {
        f.live_read = true;
        if (!loading)
          f.live = LiveAt(false, 3, 3, 0, 0);
      }
      s = RankedSet::Step(s, Ruleset::Brawl, f, static_cast<u32>(frame));
      Reading r;
      r.scene = scene;
      r.set = s;
      t.Store(frame, r, 0);
      for (GameResult& g : t.Confirm(frame - 3, 10, &starts))
        games.push_back(std::move(g));
    }
  };
  // The character select, then game 1's fight from frame 23: its loading image, then the fighters
  // in at frame 193.
  run(Scene::Other, 20);
  run(Scene::Between, 3);
  run(Scene::Fight, 170, Result(0, 1), true);
  EXPECT_TRUE(starts.empty());
  run(Scene::Fight, 3);
  // Frame 193 is read but not final yet.
  EXPECT_TRUE(starts.empty());
  run(Scene::Fight, 1);
  EXPECT_EQ(starts, std::vector<int>{23});
  // A whole fight with a sudden death in it is still one game (a tie: Brawl plays a tiebreak).
  run(Scene::Fight, 200);
  run(Scene::Between, 5);
  run(Scene::Fight, 30);
  run(Scene::Between, 5);
  run(Scene::Results, 20, Result(0, 1));
  EXPECT_EQ(starts, std::vector<int>{23});
  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].kind, GameResult::Kind::Draw);
  // The report's id is the start's.
  EXPECT_EQ(games[0].start, 23);
  // A fight left during its loading image (a crash or a quit at load) never began.
  run(Scene::Other, 40);
  run(Scene::Between, 3);
  run(Scene::Fight, 160, Result(0, 1), true);
  run(Scene::Other, 10);
  EXPECT_EQ(starts, std::vector<int>{23});
  EXPECT_EQ(games.size(), 1u);
  // Game 2 begins; leaving it for the menus without its results screen records no game, but it
  // had begun.
  run(Scene::Between, 3);
  const int second = frame;
  run(Scene::Fight, 60);
  run(Scene::Other, 10);
  EXPECT_EQ(starts, (std::vector<int>{23, second}));
  EXPECT_EQ(games.size(), 1u);
  // Games 3 and 4 to the set's end; nothing begins after it.
  std::vector<int> want{23, second};
  for (int i = 0; i < 2; ++i)
  {
    run(Scene::Other, 20);
    run(Scene::Between, 3);
    want.push_back(frame);
    run(Scene::Fight, 60);
    run(Scene::Between, 3);
    run(Scene::Results, 10, Result(0, 1));
  }
  ASSERT_EQ(s.done, 1);
  run(Scene::Other, 20);
  run(Scene::Between, 3);
  run(Scene::Fight, 60);
  run(Scene::Other, 10);
  EXPECT_EQ(starts, want);
  ASSERT_EQ(games.size(), 3u);
  EXPECT_EQ(games[1].start, want[2]);
  EXPECT_EQ(games[2].start, want[3]);
  EXPECT_EQ(games[2].set_done, 1);
}

TEST(OrcaRankedSet, AGameBeginsOnlyOnFinalFramesAfterThePlugFrame)
{
  const auto fight_from = [](int start, bool in = true) {
    Reading r;
    r.scene = Scene::Fight;
    r.set = SetState{};
    r.set->fight = true;
    r.set->fight_start = static_cast<u32>(start);
    r.set->fight_flags = in ? SetBlock::FIGHT_SEEN : 0;
    return r;
  };
  Reading select;
  select.set = SetState{};
  // On guessed inputs the fight began at frame 10; the real inputs put it at 13. Only the re-run's
  // start is ever named.
  {
    ResultsTracker t;
    std::vector<int> starts;
    for (int f = 0; f < 10; ++f)
      t.Store(f, select, 0);
    EXPECT_TRUE(t.Confirm(9, -1, &starts).empty());
    for (int f = 10; f < 20; ++f)
      t.Store(f, fight_from(10), 0);
    t.Confirm(9, -1, &starts);
    EXPECT_TRUE(starts.empty());
    for (int f = 10; f < 20; ++f)
      t.Store(f, f < 13 ? select : fight_from(13), 0);
    t.Confirm(19, -1, &starts);
    EXPECT_EQ(starts, std::vector<int>{13});
    // A final frame is never read again, and the same fight is not named twice.
    t.Store(15, fight_from(10), 0);
    for (int f = 20; f < 30; ++f)
      t.Store(f, fight_from(13), 0);
    t.Confirm(29, -1, &starts);
    EXPECT_EQ(starts, std::vector<int>{13});
  }
  // As the reports: a fight begun before the opponent plugged in (500), or right at the frame after
  // it, never counts; one begun later does.
  {
    ResultsTracker t;
    std::vector<int> starts;
    int f = 600;
    for (const int start : {400, 501, 502})
    {
      t.Store(f++, select, 0);
      t.Store(f++, fight_from(start), 0);
    }
    t.Confirm(f, 500, &starts);
    EXPECT_EQ(starts, std::vector<int>{502});
  }
  // A keyframe loaded mid-fight (a resync): the fight began before this player's plug frame, so it
  // is not named again.
  {
    ResultsTracker t;
    std::vector<int> starts;
    for (int f = 30; f < 40; ++f)
      t.Store(f, fight_from(30), 0);
    t.Confirm(39, -1, &starts);
    EXPECT_EQ(starts, std::vector<int>{30});
    for (int f = 50; f < 60; ++f)
      t.Store(f, fight_from(30), 1);
    t.Confirm(59, 50, &starts);
    EXPECT_EQ(starts, std::vector<int>{30});
  }
  // A fight still loading (fighters not in) is named only once they come in, by its first frame;
  // one whose fighters only came in on guessed inputs is not named.
  {
    ResultsTracker t;
    std::vector<int> starts;
    for (int f = 0; f < 10; ++f)
      t.Store(f, select, 0);
    for (int f = 10; f < 40; ++f)
      t.Store(f, fight_from(10, f >= 35), 0);
    t.Confirm(34, -1, &starts);
    EXPECT_TRUE(starts.empty());
    t.Confirm(39, -1, &starts);
    EXPECT_EQ(starts, std::vector<int>{10});
    ResultsTracker u;
    std::vector<int> none;
    for (int f = 10; f < 40; ++f)
      u.Store(f, fight_from(10, f >= 35), 0);
    for (int f = 35; f < 40; ++f)
      u.Store(f, fight_from(10, false), 0);
    u.Confirm(39, -1, &none);
    EXPECT_TRUE(none.empty());
  }
  // Confirming in steps or at once names the same starts.
  {
    ResultsTracker a, b;
    std::vector<int> in_steps, at_once;
    for (int f = 0; f < 200; ++f)
    {
      const Reading r = f < 20 || (f >= 90 && f < 120) ? select : fight_from(f < 90 ? 20 : 120);
      a.Store(f, r, 0);
      b.Store(f, r, 0);
      if (f % 7 == 0)
        a.Confirm(f - 3, -1, &in_steps);
    }
    a.Confirm(1000, -1, &in_steps);
    b.Confirm(1000, -1, &at_once);
    EXPECT_EQ(in_steps, (std::vector<int>{20, 120}));
    EXPECT_EQ(in_steps, at_once);
  }
  // A join rebuilt from an input replay (Rebase) mid-fight: that fight began before this machine
  // was there, whatever the plug frame; the next one is named.
  {
    ResultsTracker t;
    std::vector<int> starts;
    for (int f = 100; f < 200; ++f)
      t.Rebase(f, fight_from(50), 0);
    for (int f = 200; f < 210; ++f)
      t.Store(f, fight_from(50), 0);
    t.Confirm(209, -1, &starts);
    EXPECT_TRUE(starts.empty());
    for (int f = 210; f < 220; ++f)
      t.Store(f, f < 215 ? select : fight_from(215), 0);
    t.Confirm(219, -1, &starts);
    EXPECT_EQ(starts, std::vector<int>{215});
  }
}
