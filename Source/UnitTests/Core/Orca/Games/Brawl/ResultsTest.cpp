// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Matchmaking (ORCA.md "Matchmaking"): each game's result read from the game's memory as its frames
// become final, the ranked set's tally, and the Online menu's lines with the app's "host".

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/UX/MenuEvents.h"
#include "Core/Orca/UX/Search.h"

using namespace Orca::UX;
using Scene = Reading::Scene;
using Kind = GameResult::Kind;

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
  void Fill(u32 a, u32 n)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = 0;
  }
  void Text(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  std::map<u32, u8> bytes;
  int writes = 0;
};

constexpr u32 RESULT_INFO = 0x9017F420;
constexpr u32 SET_RULE = 0x9017F360;

// Brawl's memory as the probes found it (Results.h): the scene manager, g_GameGlobal with the
// result info and the rules, and a 1v1's result records (Mario on port 1, Link's 0x03 on port 2).
FakeMemory Game(std::string_view scene)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x10);
  m.Fill(0x80910000, 4);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, 0x80910000);
  m.Write32(0x80910000, 0x80920000);
  m.Text(0x80920000, scene);
  m.Fill(0x805A00E0, 4);
  m.Fill(0x90181300, 0x40);
  m.Write32(0x805A00E0, 0x90181300);
  m.Write32(0x90181318, RESULT_INFO);
  m.Write32(0x9018131C, SET_RULE);
  m.Fill(SET_RULE, 0x10);
  m.Write8(SET_RULE + 0x03, 2);  // a timed match, 2 minutes: the fresh save's rules
  m.Write8(SET_RULE + 0x04, 3);
  m.Write8(SET_RULE + 0x0C, 1);
  m.Fill(RESULT_INFO, 0x24 + 4 * 0x2AC + 4);
  m.Write8(RESULT_INFO + 0x01, 1);  // stock
  m.Write16(RESULT_INFO + 0x0C, 2);
  m.Write16(RESULT_INFO + 0x0E, 1);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 rec = RESULT_INFO + 0x24 + port * 0x2AC;
    m.Write8(rec + 0x00, port == 0 ? 0x00 : port == 1 ? 0x03 : 0x3E);
    m.Write8(rec + 0x01, port < 2 ? 0 : 3);
    m.Write8(rec + 0x0A, port == 0 ? 3 : 0);
    m.Write8(rec + 0x0E, port == 0 ? 0 : 1);
  }
  m.writes = 0;
  return m;
}

void SetPort(FakeMemory& m, u32 port, u8 type, u8 stocks, u8 place)
{
  const u32 rec = RESULT_INFO + 0x24 + port * 0x2AC;
  m.Write8(rec + 0x01, type);
  m.Write8(rec + 0x0A, stocks);
  m.Write8(rec + 0x0E, place);
}

Reading At(Scene scene, int winner_port = 0)
{
  Reading r;
  r.scene = scene;
  if (scene == Scene::Results)
  {
    r.block.mode = 1;
    r.block.stage = 2;
    r.block.end = 1;
    for (int p = 0; p < 2; ++p)
    {
      r.block.ports[p] = {true, true, p == 0 ? 0 : 3, p == winner_port ? 2 : 0,
                          p == winner_port ? 0 : 1};
    }
  }
  return r;
}

// Feeds a scene timeline from `first`, each scene for `frames` frames, confirming as it goes.
struct Timeline
{
  ResultsTracker tracker;
  int frame = 0;
  int plug = -1;
  std::vector<GameResult> out;
  void Run(Scene scene, int frames, int winner_port = 0)
  {
    for (int i = 0; i < frames; ++i, ++frame)
    {
      tracker.Store(frame, At(scene, winner_port), 0);
      for (GameResult& g : tracker.Confirm(frame, plug))
        out.push_back(std::move(g));
    }
  }
};
}  // namespace

TEST(OrcaResults, ReadsTheResultInfoOnTheResultsScreenOnly)
{
  {
    FakeMemory m = Game("scVsResult");
    const Reading r = ReadResults(m);
    EXPECT_EQ(r.scene, Scene::Results);
    EXPECT_EQ(r.block.mode, 1);
    EXPECT_EQ(r.block.stage, 2);
    EXPECT_EQ(r.block.end, 1);
    EXPECT_TRUE(r.block.ports[0].present && r.block.ports[0].human);
    EXPECT_EQ(r.block.ports[0].character, 0);
    EXPECT_EQ(r.block.ports[1].character, 3);
    EXPECT_EQ(r.block.ports[0].stocks, 3);
    EXPECT_EQ(r.block.ports[1].place, 1);
    EXPECT_FALSE(r.block.ports[2].present);
    EXPECT_EQ(m.writes, 0);
  }
  {
    // A timed match's stocks read as none.
    FakeMemory m = Game("scVsResult");
    SetPort(m, 0, 0, 0xFF, 0);
    EXPECT_EQ(ReadResults(m).block.ports[0].stocks, -1);
  }
  {
    const FakeMemory m = Game("scMelee");
    const Reading r = ReadResults(m);
    EXPECT_EQ(r.scene, Scene::Fight);
    EXPECT_EQ(r.block, ResultBlock{});
  }
  EXPECT_EQ(ReadResults(Game("scMemoryChange")).scene, Scene::Between);
  EXPECT_EQ(ReadResults(Game("scSelctCharacter")).scene, Scene::Other);
  // Nothing mapped: nothing read.
  EXPECT_EQ(ReadResults(FakeMemory{}).scene, Scene::Other);
}

TEST(OrcaResults, JudgesAOneVersusOne)
{
  FakeMemory m = Game("scVsResult");
  {
    const GameResult g = Judge(ReadResults(m).block, 5037);
    EXPECT_EQ(g.kind, Kind::Win);
    EXPECT_EQ(g.winner_port, 0);
    EXPECT_EQ(g.DetailJson(), R"({"f":5037,"st":2,"c":[0,3],"s":[3,0],"to":0})");
  }
  {
    SetPort(m, 0, 0, 0, 1);
    SetPort(m, 1, 0, 2, 0);
    const GameResult g = Judge(ReadResults(m).block, 7);
    EXPECT_EQ(g.kind, Kind::Win);
    EXPECT_EQ(g.winner_port, 1);
    EXPECT_FALSE(g.timeout);
  }
  {
    // Out of time with stocks left on both sides; a timed match is always out of time.
    SetPort(m, 0, 0, 1, 1);
    SetPort(m, 1, 0, 2, 0);
    EXPECT_TRUE(Judge(ReadResults(m).block, 7).timeout);
    FakeMemory t = Game("scVsResult");
    t.Write8(RESULT_INFO + 0x01, 0);
    SetPort(t, 0, 0, 0xFF, 0);
    SetPort(t, 1, 0, 0xFF, 1);
    const GameResult g = Judge(ReadResults(t).block, 9);
    EXPECT_TRUE(g.timeout);
    EXPECT_EQ(g.DetailJson(), R"({"f":9,"st":2,"c":[0,3],"s":[-1,-1],"to":1})");
  }
  {
    // Both first: a draw.
    FakeMemory d = Game("scVsResult");
    SetPort(d, 1, 0, 3, 0);
    EXPECT_EQ(Judge(ReadResults(d).block, 1).kind, Kind::Draw);
  }
  {
    // The pause menu's quit: no contest.
    FakeMemory q = Game("scVsResult");
    q.Write16(RESULT_INFO + 0x0E, 2);
    const GameResult g = Judge(ReadResults(q).block, 1);
    EXPECT_EQ(g.kind, Kind::Void);
    EXPECT_EQ(g.why, "nocontest");
  }
  {
    // A CPU on port 3, or port 2 a CPU: not a 1v1 of the two players.
    FakeMemory c = Game("scVsResult");
    SetPort(c, 2, 1, 3, 2);
    EXPECT_EQ(Judge(ReadResults(c).block, 1).why, "ports");
    FakeMemory d = Game("scVsResult");
    SetPort(d, 1, 1, 0, 1);
    EXPECT_EQ(Judge(ReadResults(d).block, 1).why, "ports");
    FakeMemory e = Game("scVsResult");
    SetPort(e, 1, 3, 0, 1);
    EXPECT_EQ(Judge(ReadResults(e).block, 1).why, "ports");
  }
}

TEST(OrcaResults, AGameIsAFightThatReachesItsResults)
{
  Timeline t;
  t.Run(Scene::Other, 10);
  t.Run(Scene::Between, 5);
  t.Run(Scene::Fight, 100);
  EXPECT_TRUE(t.out.empty());
  t.Run(Scene::Between, 5);
  t.Run(Scene::Results, 30, 1);
  ASSERT_EQ(t.out.size(), 1u);
  EXPECT_EQ(t.out[0].kind, Kind::Win);
  EXPECT_EQ(t.out[0].winner_port, 1);
  // The frame of the first results reading.
  EXPECT_EQ(t.out[0].frame, 120);
  // The results screen and the character select after it count nothing more.
  t.Run(Scene::Between, 5);
  t.Run(Scene::Other, 50);
  EXPECT_EQ(t.out.size(), 1u);
  // The next game.
  t.Run(Scene::Between, 5);
  t.Run(Scene::Fight, 100);
  t.Run(Scene::Between, 5);
  t.Run(Scene::Results, 3, 0);
  ASSERT_EQ(t.out.size(), 2u);
  EXPECT_EQ(t.out[1].winner_port, 0);
}

TEST(OrcaResults, AFightRunningWhenTheOpponentPluggedInNeverCounts)
{
  Timeline t;
  t.Run(Scene::Fight, 50);
  // Plugged in mid-fight: this fight is not a game of theirs.
  t.plug = 60;
  t.Run(Scene::Fight, 50);
  t.Run(Scene::Between, 5);
  t.Run(Scene::Results, 10);
  EXPECT_TRUE(t.out.empty());
  // A fight entered right at the plug frame, whose frame before was read before it: not either.
  Timeline u;
  u.Run(Scene::Other, 10);
  u.plug = 10;
  u.Run(Scene::Fight, 20);
  u.Run(Scene::Results, 5);
  EXPECT_TRUE(u.out.empty());
  // The next one does.
  t.Run(Scene::Other, 20);
  t.Run(Scene::Fight, 20);
  t.Run(Scene::Between, 2);
  t.Run(Scene::Results, 2);
  EXPECT_EQ(t.out.size(), 1u);
}

TEST(OrcaResults, ReconstructedHistoryNeverReportsOldResults)
{
  ResultsTracker tracker;
  tracker.Store(1, At(Scene::Fight), 0);
  tracker.Store(2, At(Scene::Results, 0), 0);
  tracker.Rebase(100, At(Scene::Fight), 1);
  EXPECT_TRUE(tracker.Confirm(100, 90).empty());
  tracker.Store(101, At(Scene::Results, 0), 1);
  EXPECT_TRUE(tracker.Confirm(101, 90).empty());
  tracker.Store(102, At(Scene::Other), 1);
  tracker.Store(103, At(Scene::Fight), 1);
  tracker.Store(104, At(Scene::Results, 0), 1);
  const auto results = tracker.Confirm(104, 90);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].frame, 104);
}

TEST(OrcaResults, AFightThatEndsWithoutItsResultsIsNoContest)
{
  Timeline t;
  t.Run(Scene::Other, 5);
  t.Run(Scene::Fight, 50);
  t.Run(Scene::Between, 5);
  t.Run(Scene::Other, 5);
  ASSERT_EQ(t.out.size(), 1u);
  EXPECT_EQ(t.out[0].kind, Kind::Void);
  EXPECT_EQ(t.out[0].why, "nocontest");
  EXPECT_EQ(t.out[0].frame, 60);
}

TEST(OrcaResults, SuddenDeathIsTheSameGame)
{
  Timeline t;
  t.Run(Scene::Other, 5);
  t.Run(Scene::Fight, 50);
  t.Run(Scene::Between, 5);
  t.Run(Scene::Fight, 50);
  t.Run(Scene::Between, 5);
  t.Run(Scene::Results, 5, 1);
  ASSERT_EQ(t.out.size(), 1u);
  EXPECT_EQ(t.out[0].winner_port, 1);
}

TEST(OrcaResults, OnlyFinalFramesCountAndARerunReplacesTheGuess)
{
  ResultsTracker tracker;
  int f = 0;
  for (; f < 10; ++f)
    tracker.Store(f, At(Scene::Other), 0);
  for (; f < 60; ++f)
    tracker.Store(f, At(Scene::Fight), 0);
  // On guessed inputs, port 1 won.
  for (int g = 60; g < 70; ++g)
    tracker.Store(g, At(Scene::Results, 0), 0);
  EXPECT_TRUE(tracker.Confirm(59, -1).empty());
  // The real inputs arrive: the frames from 60 re-run (frames through 59 were final), and port 2
  // won.
  for (int g = 60; g < 70; ++g)
    tracker.Store(g, At(Scene::Results, 1), 0);
  const auto out = tracker.Confirm(69, -1);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].winner_port, 1);
  // A frame already final is never read again.
  tracker.Store(65, At(Scene::Results, 0), 0);
  EXPECT_TRUE(tracker.Confirm(70, -1).empty());
}

TEST(OrcaResults, ConfirmingInStepsOrAtOnceIsTheSame)
{
  std::vector<Reading> line;
  for (int i = 0; i < 10; ++i)
    line.push_back(At(Scene::Other));
  for (int i = 0; i < 40; ++i)
    line.push_back(At(Scene::Fight));
  for (int i = 0; i < 3; ++i)
    line.push_back(At(Scene::Between));
  for (int i = 0; i < 10; ++i)
    line.push_back(At(Scene::Results, 1));
  ResultsTracker a, b;
  std::vector<GameResult> in_steps;
  for (int f = 0; f < static_cast<int>(line.size()); ++f)
  {
    a.Store(f, line[f], 0);
    b.Store(f, line[f], 0);
    if (f % 7 == 0)
    {
      for (auto& g : a.Confirm(f - 3, -1))
        in_steps.push_back(g);
    }
  }
  for (auto& g : a.Confirm(1000, -1))
    in_steps.push_back(g);
  EXPECT_EQ(in_steps, b.Confirm(1000, -1));
  EXPECT_EQ(in_steps.size(), 1u);
}

TEST(OrcaResults, AResyncForgetsWhatWasRead)
{
  ResultsTracker t;
  for (int f = 0; f < 50; ++f)
    t.Store(f, At(f < 10 ? Scene::Other : Scene::Fight), 0);
  EXPECT_TRUE(t.Confirm(49, -1).empty());
  // A keyframe loaded (resync 1): the results screen that follows is not the end of a fight seen
  // here.
  for (int f = 50; f < 60; ++f)
    t.Store(f, At(Scene::Results, 0), 1);
  EXPECT_TRUE(t.Confirm(59, -1).empty());
}

TEST(OrcaResults, TheQueueRulesPresetBrawlsRules)
{
  FakeMemory m = Game("scSelctCharacter");
  // The fresh save's 3 stocks stay; the mode, the stock match's time limit and the items change.
  EXPECT_EQ(WriteQueueRules(m), 3);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02), 1);
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(SET_RULE + 0x08), 8);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0C), 0);
  // Twice is once.
  EXPECT_EQ(WriteQueueRules(m), 0);
  // No rules to find: nothing written.
  FakeMemory empty;
  EXPECT_EQ(WriteQueueRules(empty), 0);
  EXPECT_EQ(empty.writes, 0);
}

TEST(OrcaRankedSet, FirstToTwoWins)
{
  Orca::Net::SetTally t;
  EXPECT_FALSE(t.Game("ada"));
  EXPECT_FALSE(t.Game("bo"));
  EXPECT_FALSE(t.Game(""));  // a draw or a void game counts for nobody
  const auto end = t.Game("bo");
  ASSERT_TRUE(end);
  EXPECT_EQ(end->winner, "bo");
  EXPECT_EQ(t.Wins("bo"), 2);
  EXPECT_EQ(t.Wins("ada"), 1);
  // Nothing after the end.
  EXPECT_FALSE(t.Game("ada"));
  EXPECT_FALSE(t.Game("ada"));
}

TEST(OrcaRankedSet, SevenGamesWithoutAWinnerAreVoid)
{
  Orca::Net::SetTally t;
  for (int i = 0; i < 6; ++i)
    EXPECT_FALSE(t.Game(i == 0 ? "ada" : i == 1 ? "bo" : ""));
  const auto end = t.Game("");
  ASSERT_TRUE(end);
  EXPECT_EQ(end->winner, "");
  EXPECT_EQ(end->why, "limit");
  t.Reset();
  EXPECT_FALSE(t.Game("ada"));
  EXPECT_TRUE(t.Game("ada"));
}

TEST(OrcaMatchmakingMenu, CasualAndRankedSayLocalOnlyWithoutHost)
{
  EXPECT_EQ(MenuEventText(OnlinePick::Casual, true), "online casual");
  EXPECT_EQ(MenuEventText(OnlinePick::Ranked, true), "online ranked");
  EXPECT_EQ(MenuEventText(OnlinePick::Friends, true), "online friends");
  EXPECT_EQ(MenuEventText(OnlinePick::Casual, false), "online casual local");
  EXPECT_EQ(MenuEventText(OnlinePick::Ranked, false), "online ranked local");
}

TEST(OrcaMatchmakingMenu, TheSearchLinesFollowTheQueue)
{
  Search::End();
  EXPECT_TRUE(Search::Lines().first.empty());
  Search::Begin(true);
  EXPECT_EQ(Search::Lines().first, "Searching for an opponent… (B: back)");
  EXPECT_EQ(Search::Lines().second, "RANKED BETA");
  Search::Matched(true);
  EXPECT_EQ(Search::Current(), Search::State::Found);
  EXPECT_NE(Search::Lines().first.find("Opponent found"), std::string::npos);
  Search::End();
  Search::Begin(false);
  EXPECT_TRUE(Search::Lines().second.empty());
  // A join: its own messages take over.
  Search::Matched(false);
  EXPECT_EQ(Search::Current(), Search::State::None);
  // A match after the player backed out never shows Found.
  Search::Matched(true);
  EXPECT_EQ(Search::Current(), Search::State::None);
}

// A matched joiner sees that the opponent was found from its `join` until the host's game is loaded
// (or the join ends), over its own select's search lines; Current() stays None meanwhile, so
// nothing that reads the search state changes.
TEST(OrcaMatchmakingMenu, AMatchedJoinerSaysTheOpponentWasFound)
{
  Search::End();
  Search::Begin(false);
  Search::Matched(false);
  EXPECT_EQ(Search::Current(), Search::State::None);
  EXPECT_TRUE(Search::Joining());
  // No room yet: the host has no name.
  EXPECT_EQ(Search::Lines().first, "Opponent found · joining their game…");
  EXPECT_EQ(Search::JoiningLines("Cy", false),
            (std::pair<std::string, std::string>{"Opponent found · joining Cy…", ""}));
  EXPECT_EQ(Search::JoiningLines("Cy", true).second, "RANKED BETA");
  Search::JoinOver();
  EXPECT_FALSE(Search::Joining());
  EXPECT_TRUE(Search::Lines().first.empty());
  // The queue's own select (queue2) searches without Search: the join says so all the same.
  Search::Matched(false);
  EXPECT_FALSE(Search::Joining());
  Search::JoinStarted();
  EXPECT_TRUE(Search::Joining());
  Search::End();
  EXPECT_FALSE(Search::Joining());
  // A new search, or hosting, is never "joining".
  Search::JoinStarted();
  Search::Begin(true);
  EXPECT_FALSE(Search::Joining());
  Search::Matched(true);
  EXPECT_FALSE(Search::Joining());
  Search::End();
}

TEST(OrcaMatchmakingMenu, HostIsACapResultsOnlyWithAVerifiedReader)
{
  // No profile here: host is offered, results is not.
  EXPECT_EQ(Orca::Status::OfferedCaps(),
            "join leave stats pause delay perf direct host chat yougame gfx talk rumble record hostui");
  Orca::Status::SetAppCaps("join host results");
  EXPECT_TRUE(Orca::Status::Cap("host"));
  EXPECT_FALSE(Orca::Status::Cap("results"));
  Orca::Status::SetAppCaps("");
}

namespace
{
constexpr u32 ITEM_FREQUENCY = 0x9017BE50;

// A 1v1's results screen (port 1 won) played under 3 stocks, 8:00 and items off, as the rules block
// and the item record hold them. The fresh save's own rules (Game) differ in all three.
FakeMemory RankedResult()
{
  FakeMemory m = Game("scVsResult");
  m.Write8(SET_RULE + 0x02, 0x81);  // stock; the bits above the rule are not the rule's
  m.Write8(SET_RULE + 0x03, 2);     // a timed match's minutes: not this match's
  m.Write8(SET_RULE + 0x04, 3);
  m.Write8(SET_RULE + 0x05, 0);   // handicap off
  m.Write8(SET_RULE + 0x06, 10);  // damage ratio 1.0
  m.Write8(SET_RULE + 0x08, 8);
  m.Write8(ITEM_FREQUENCY, 0);
  m.writes = 0;
  return m;
}

const Orca::Mod::RankedRules RULES{3, 8};
}  // namespace

TEST(OrcaResults, RankedRulesReadTheRulesTheFightStartedWith)
{
  FakeMemory m = RankedResult();
  // Not read: the rules block's own item byte (+0x0C) and the result info's +0x04 and +0x08.
  m.Write8(SET_RULE + 0x0C, 3);
  m.Write32(RESULT_INFO + 0x04, 2);
  m.Write32(RESULT_INFO + 0x08, 1);
  m.writes = 0;
  const Reading checked = ReadResults(m, RULES);
  EXPECT_TRUE(checked.block.differing_rule.empty());
  EXPECT_EQ(Judge(checked.block, 500).kind, Kind::Win);
  EXPECT_EQ(Judge(checked.block, 500).winner_port, 0);
  EXPECT_EQ(m.writes, 0);
  // The fresh save's rules (timed, 2 minutes, items Medium) differ from the first rule on.
  EXPECT_EQ(ReadResults(Game("scVsResult"), RULES).block.differing_rule, "stocks");
  // Only the results screen is judged.
  FakeMemory fight = RankedResult();
  fight.Text(0x80920000, "scMelee");
  fight.Write8(SET_RULE + 0x04, 4);
  EXPECT_TRUE(ReadResults(fight, RULES).block.differing_rule.empty());
  const auto check = [](FakeMemory image, u32 address, u8 value, std::string_view rule) {
    SCOPED_TRACE(fmt::format("{:08x} = {}", address, value));
    image.Write8(address, value);
    image.writes = 0;
    const Reading reading = ReadResults(image, RULES);
    EXPECT_EQ(reading.block.differing_rule, rule);
    const GameResult result = Judge(reading.block, 500);
    EXPECT_EQ(result.kind, Kind::Void);
    EXPECT_EQ(result.winner_port, -1);
    EXPECT_EQ(result.why, fmt::format("rules-{}", rule));
    EXPECT_EQ(image.writes, 0);
    // Without rankedRules the same screen is a win, as before.
    EXPECT_TRUE(ReadResults(image).block.differing_rule.empty());
    EXPECT_EQ(Judge(ReadResults(image).block, 500).kind, Kind::Win);
  };
  check(m, SET_RULE + 0x04, 4, "stocks");
  check(m, SET_RULE + 0x04, 2, "stocks");
  check(m, SET_RULE + 0x02, 0x80, "stocks");  // a timed match
  check(m, SET_RULE + 0x02, 0x02, "stocks");
  check(m, SET_RULE + 0x08, 7, "timeMinutes");
  check(m, SET_RULE + 0x08, 0, "timeMinutes");  // no time limit
  for (const int frequency : {1, 2, 3, 4})
    check(m, ITEM_FREQUENCY, static_cast<u8>(frequency), "items");
  // The engine's own two: handicap off and a damage ratio of 1.0, whatever mod.json says.
  for (const int handicap : {1, 2})
    check(m, SET_RULE + 0x05, static_cast<u8>(handicap), "handicap");
  for (const int ratio : {0, 5, 9, 11, 20})
    check(m, SET_RULE + 0x06, static_cast<u8>(ratio), "damageRatio");
  // The first rule that differs is the one named.
  FakeMemory all = RankedResult();
  all.Write8(SET_RULE + 0x08, 6);
  all.Write8(ITEM_FREQUENCY, 2);
  EXPECT_EQ(ReadResults(all, RULES).block.differing_rule, "timeMinutes");
  EXPECT_EQ(ReadResults(all, Orca::Mod::RankedRules{3, 6}).block.differing_rule, "items");
  all.Write8(SET_RULE + 0x05, 1);
  all.Write8(SET_RULE + 0x06, 20);
  all.Write8(ITEM_FREQUENCY, 0);
  EXPECT_EQ(ReadResults(all, Orca::Mod::RankedRules{3, 6}).block.differing_rule, "handicap");
  all.Write8(SET_RULE + 0x05, 0);
  EXPECT_EQ(ReadResults(all, Orca::Mod::RankedRules{3, 6}).block.differing_rule, "damageRatio");
}

TEST(OrcaResults, RankedRulesThatCantBeReadAreNotRated)
{
  for (const auto& [address, rule] :
       std::map<u32, std::string>{{SET_RULE + 0x02, "stocks"},
                                  {SET_RULE + 0x04, "stocks"},
                                  {SET_RULE + 0x05, "handicap"},
                                  {SET_RULE + 0x06, "damageRatio"},
                                  {SET_RULE + 0x08, "timeMinutes"},
                                  {ITEM_FREQUENCY, "items"}})
  {
    SCOPED_TRACE(fmt::format("{:08x}", address));
    FakeMemory m = RankedResult();
    m.bytes.erase(address);
    const Reading reading = ReadResults(m, RULES);
    EXPECT_EQ(reading.block.differing_rule, rule);
    EXPECT_EQ(Judge(reading.block, 500).kind, Kind::Void);
    EXPECT_EQ(Judge(ReadResults(m).block, 500).kind, Kind::Win);
  }
  // No rules block at all (g_GameGlobal +0x1C null).
  FakeMemory m = RankedResult();
  m.Write32(0x9018131C, 0);
  EXPECT_EQ(ReadResults(m, RULES).block.differing_rule, "stocks");
  EXPECT_EQ(Judge(ReadResults(m).block, 500).kind, Kind::Win);
}

TEST(OrcaResults, RankedRulesVerdictUsesFinalFramesAndReportsOnce)
{
  FakeMemory m = RankedResult();
  m.Write8(ITEM_FREQUENCY, 2);
  const Reading bad = ReadResults(m, RULES);
  ResultsTracker tracker;
  tracker.Store(0, At(Scene::Other), 0);
  tracker.Store(1, At(Scene::Fight), 0);
  tracker.Store(2, bad, 0);
  EXPECT_TRUE(tracker.Confirm(1, -1).empty());
  // A rollback re-run replaces frame 2's predicted reading before it is final.
  m.Write8(ITEM_FREQUENCY, 0);
  tracker.Store(2, ReadResults(m, RULES), 0);
  auto out = tracker.Confirm(2, -1);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, Kind::Win);
  tracker.Store(3, At(Scene::Fight), 0);
  tracker.Store(4, bad, 0);
  tracker.Store(5, bad, 0);
  out = tracker.Confirm(5, -1);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, Kind::Void);
  EXPECT_EQ(out[0].why, "rules-items");
  EXPECT_TRUE(tracker.Confirm(5, -1).empty());
}
