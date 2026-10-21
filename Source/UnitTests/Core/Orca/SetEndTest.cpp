// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <imgui.h>

#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/Widgets.h"

using namespace Orca::UX::SetEnd;
using Phase = RatingCount::Phase;
using Orca::UX::Widgets::Tone;

// ---- The rating's count ----

TEST(OrcaSetEndRating, UpdatingUntilTheNewNumberThenGoneAfterTheWait)
{
  RatingCount r;
  EXPECT_EQ(r.At(0).phase, Phase::None);
  r.Start(1532, 1000);
  RatingCount::View v = r.At(1000);
  EXPECT_EQ(v.phase, Phase::Updating);
  EXPECT_EQ(v.shown, 1532);  // the old number, while the new one is on its way
  EXPECT_FALSE(v.has_delta);
  EXPECT_EQ(r.At(1000 + RatingCount::WAIT_MS - 1).phase, Phase::Updating);
  EXPECT_EQ(r.At(1000 + RatingCount::WAIT_MS).phase, Phase::Gone);
  EXPECT_EQ(r.At(1000 + RatingCount::WAIT_MS).shown, -1);
}

TEST(OrcaSetEndRating, CountsFromOldToNewThenPopsAndSettles)
{
  RatingCount r;
  r.Start(1532, 0);
  r.Arrive(1548, 2000);
  EXPECT_DOUBLE_EQ(r.LandsAt(), 2000 + RatingCount::COUNT_MS);
  int last = 1532;
  for (double t = 2000; t < 2000 + RatingCount::COUNT_MS; t += 25)
  {
    const RatingCount::View v = r.At(t);
    ASSERT_EQ(v.phase, Phase::Counting) << t;
    EXPECT_GE(v.shown, last) << t;  // never backwards
    EXPECT_LE(v.shown, 1548) << t;
    EXPECT_EQ(v.badge_pop, 0) << t;  // the change shows as it lands
    EXPECT_TRUE(v.has_delta);
    EXPECT_EQ(v.delta, 16);
    last = v.shown;
  }
  EXPECT_EQ(r.At(2000).shown, 1532);
  // Landed: the new number, swelling and settling back to 1 within POP_MS.
  const double land = r.LandsAt();
  RatingCount::View v = r.At(land);
  EXPECT_EQ(v.phase, Phase::Landed);
  EXPECT_EQ(v.shown, 1548);
  float biggest = 1;
  for (double t = land; t < land + RatingCount::POP_MS; t += 10)
    biggest = std::max(biggest, r.At(t).pop);
  EXPECT_GT(biggest, 1.2f);
  EXPECT_LE(biggest, 1 + RatingCount::POP_SCALE + 1e-4f);
  v = r.At(land + RatingCount::POP_MS);
  EXPECT_FLOAT_EQ(v.pop, 1);
  EXPECT_FLOAT_EQ(v.badge_pop, 1);
  EXPECT_EQ(v.shown, 1548);
}

TEST(OrcaSetEndRating, ALossCountsDownAndTheChangeIsNegative)
{
  RatingCount r;
  r.Start(1532, 0);
  r.Arrive(1517, 100);
  EXPECT_EQ(r.At(100 + RatingCount::COUNT_MS / 2).delta, -15);
  int last = 1532;
  for (double t = 100; t <= 100 + RatingCount::COUNT_MS; t += 50)
  {
    EXPECT_LE(r.At(t).shown, last);
    last = r.At(t).shown;
  }
  EXPECT_EQ(r.At(5000).shown, 1517);
}

TEST(OrcaSetEndRating, NoChangeOrNoNumberBeforeShowsAtOnceWithoutAnimation)
{
  RatingCount same;
  same.Start(1500, 0);
  same.Arrive(1500, 300);
  RatingCount::View v = same.At(300);
  EXPECT_EQ(v.phase, Phase::Landed);
  EXPECT_TRUE(v.has_delta);
  EXPECT_EQ(v.delta, 0);  // "±0"
  EXPECT_FLOAT_EQ(v.pop, 1);
  EXPECT_FLOAT_EQ(v.badge_pop, 1);

  RatingCount unknown;
  unknown.Start(-1, 0);
  EXPECT_EQ(unknown.At(0).shown, -1);
  EXPECT_EQ(unknown.At(0).phase, Phase::Updating);
  unknown.Arrive(1210, 500);
  v = unknown.At(500);
  EXPECT_EQ(v.phase, Phase::Landed);
  EXPECT_EQ(v.shown, 1210);
  EXPECT_FALSE(v.has_delta);
  EXPECT_FLOAT_EQ(v.pop, 1);

  // The room's own number before replaces the one going in.
  RatingCount room;
  room.Start(1500, 0);
  room.Arrive(1530, 0, 1510);
  EXPECT_EQ(room.At(5000).delta, 20);
  EXPECT_EQ(room.At(0).shown, 1510);
}

TEST(OrcaSetEndRating, AnotherNumberCountsOnFromWhereTheCountStands)
{
  RatingCount r;
  r.Start(1500, 0);
  r.Arrive(1600, 0);
  const int mid = r.At(500).shown;
  EXPECT_GT(mid, 1500);
  EXPECT_LT(mid, 1600);
  r.Arrive(1600, 600);  // the same number again: nothing changes
  EXPECT_DOUBLE_EQ(r.LandsAt(), RatingCount::COUNT_MS);
  r.Arrive(1550, 500);
  EXPECT_EQ(r.At(500).shown, mid);
  EXPECT_EQ(r.At(500 + RatingCount::COUNT_MS).shown, 1550);
  EXPECT_EQ(r.At(500 + RatingCount::COUNT_MS).delta, 50);  // against the number going in
}

// ---- The countdown ----

TEST(OrcaSetEndCountdown, SecondsLeftRoundUp)
{
  EXPECT_EQ(SecondsLeft(10000, 0), 10);
  EXPECT_EQ(SecondsLeft(10000, 1000), 9);
  EXPECT_EQ(SecondsLeft(10000, 1001), 9);
  EXPECT_EQ(SecondsLeft(10000, 1999), 9);
  EXPECT_EQ(SecondsLeft(10000, 2000), 8);
  EXPECT_EQ(SecondsLeft(10000, 9999), 1);
  EXPECT_EQ(SecondsLeft(10000, 10000), 0);
  EXPECT_EQ(SecondsLeft(10000, 25000), 0);
  EXPECT_EQ(SecondsLeft(10000, std::nan("")), 0);
  char buffer[16];
  EXPECT_EQ(Orca::UX::Widgets::ClockText(9, buffer), "0:09");
  EXPECT_EQ(Orca::UX::Widgets::ClockText(65, buffer), "1:05");
  EXPECT_EQ(Orca::UX::Widgets::ClockText(-3, buffer), "0:00");
}

// The room's leave: the forfeit is the room's GRACE (15 s) after it, counted down whole seconds,
// then the verdict: "bo forfeited · You win the set", then VICTORY.
TEST(OrcaSetEndCountdown, ALeaveCountsTheRoomsGraceThenTheForfeitWinsTheSet)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.GameBegan(500);
  m.OpponentLeft(1000);
  View v = m.At(1000);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "bo disconnected");
  EXPECT_EQ(v.notice->line, "They forfeit in 0:15");
  EXPECT_EQ(v.notice->seconds, 15);
  EXPECT_EQ(v.notice->tone, Tone::Warning);
  EXPECT_TRUE(v.hides_waiting);
  EXPECT_FALSE(v.panel);
  EXPECT_EQ(m.At(2000).notice->seconds, 14);
  EXPECT_NEAR(m.At(2000).notice->fraction, 14.0 / 15, 1e-6);
  EXPECT_EQ(m.At(15999).notice->seconds, 1);
  EXPECT_EQ(m.At(16000).notice->seconds, 0);
  EXPECT_EQ(m.At(16000).notice->line, "They forfeit now");
  EXPECT_EQ(m.At(7001).notice->line, "They forfeit in 0:09");
  EXPECT_EQ(m.At(16500).notice->seconds, 0);  // until the verdict
  // A second leave (another roster) doesn't move the deadline.
  m.OpponentLeft(5000);
  EXPECT_EQ(m.At(6000).notice->seconds, 10);

  m.SetVerdict(Outcome::Won, true, 1, 0, -1, -1, 1532, 16400);
  v = m.At(16400);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "bo forfeited");
  EXPECT_EQ(v.notice->line, "You win the set");
  EXPECT_EQ(v.notice->tone, Tone::Win);
  EXPECT_EQ(v.notice->seconds, -1);
  EXPECT_FALSE(v.panel);  // the panel comes after
  const double panel_at = 16400 + Model::FORFEITED_MS;
  v = m.At(panel_at);
  EXPECT_FALSE(v.notice);
  ASSERT_TRUE(v.panel);
  EXPECT_EQ(v.panel->title, "VICTORY");
  EXPECT_EQ(v.panel->score, "1–0");
  EXPECT_EQ(v.panel->detail, "vs bo · by forfeit");
  EXPECT_TRUE(v.panel->show_rating);
  EXPECT_EQ(v.panel->rating.phase, Phase::Updating);
  EXPECT_EQ(v.panel->rating.shown, 1532);
  // The page's new rating counts in the panel.
  m.QueueRating(1549, panel_at + 500);
  EXPECT_EQ(m.At(panel_at + 500 + RatingCount::COUNT_MS).panel->rating.shown, 1549);
  EXPECT_EQ(m.At(panel_at + 500 + RatingCount::COUNT_MS).panel->rating.delta, 17);
}

// A stall with no leave: the session gives up STALL_FORFEIT_MS after it began (its silence
// limit) and claims the set, which the room lets stand at once; shown once it is STALL_NOTICE_MS
// long.
TEST(OrcaSetEndCountdown, AStallCountsTheSessionsLimitAndGoesIfTheyComeBack)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.Stall(static_cast<int>(Model::STALL_NOTICE_MS) - 1, 10000);
  EXPECT_FALSE(m.At(10000).notice);
  m.Stall(static_cast<int>(Model::STALL_NOTICE_MS), 10001);
  View v = m.At(10001);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->seconds, 9);  // 8.5 s left: "0:09"
  // Later stall readings don't move the start.
  m.Stall(5000, 13501);
  EXPECT_EQ(m.At(13501).notice->seconds, 5);
  // Their inputs came: the notice goes.
  m.StallOver(true, 13600);
  EXPECT_FALSE(m.At(13600).notice);
  // Again, and this time the session gives up: the countdown stays at 0 for the verdict, then
  // goes NO_VERDICT_MS past its end if none comes.
  m.Stall(2000, 30000);
  m.StallOver(false, 36000);
  EXPECT_EQ(m.At(36000).notice->seconds, 2);
  m.OpponentDropped(38000);
  EXPECT_EQ(m.At(38000).notice->seconds, 0);
  m.StallOver(true, 38100);  // a later stall's end never brings an unplugged opponent back
  ASSERT_TRUE(m.At(38100).notice);
  EXPECT_TRUE(m.At(38000 + Model::NO_VERDICT_MS).notice);
  EXPECT_FALSE(m.At(38001 + Model::NO_VERDICT_MS).notice);
}

TEST(OrcaSetEndCountdown, ALeaveDuringAStallCountsFromTheLeave)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.Stall(3000, 5000);
  EXPECT_EQ(m.At(5000).notice->seconds, 7);
  m.OpponentLeft(5000);
  EXPECT_EQ(m.At(5000).notice->seconds, 15);
  // The session's unplugging them changes nothing then.
  m.OpponentDropped(12000);
  EXPECT_EQ(m.At(12000).notice->seconds, 8);
  // A session that unplugged them with no stall on screen: the countdown is at its end.
  Model n;
  n.MatchBegan(true, "bo", 0);
  n.OpponentDropped(1000);
  EXPECT_EQ(n.At(1000).notice->seconds, 0);
}

// Before the set's first game began (its fighters in on final frames, or a game ended), the room
// voids a leave or a stall instead of forfeiting it (YouGame docs/ORCA_ONLINE_UX.md "3B leaving
// mid-game"): the same countdown, to no contest.
TEST(OrcaSetEndCountdown, BeforeTheFirstGameTheCountdownIsToNoContest)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.OpponentLeft(1000);
  View v = m.At(1000);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "bo disconnected");
  EXPECT_EQ(v.notice->line, "No contest in 0:15");
  EXPECT_EQ(v.notice->seconds, 15);
  EXPECT_EQ(v.notice->tone, Tone::Warning);
  EXPECT_EQ(m.At(16000).notice->line, "No contest now");
  // The room's verdict: void, "No contest · This set doesn't count".
  m.SetVerdict(Outcome::Void, false, 0, 0, -1, -1, 1532, 16100);
  v = m.At(16100);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "No contest");
  EXPECT_EQ(v.notice->line, "This set doesn't count");
  EXPECT_FALSE(v.panel);

  // A stall alone, the same.
  Model n;
  n.MatchBegan(true, "bo", 0);
  n.Stall(static_cast<int>(Model::STALL_NOTICE_MS), 10000);
  ASSERT_TRUE(n.At(10000).notice);
  EXPECT_EQ(n.At(10000).notice->line, "No contest in 0:09");
  // The game begins on final frames during the countdown: from then on it is a forfeit.
  n.GameBegan(11000);
  EXPECT_EQ(n.At(11000).notice->line, "They forfeit in 0:08");

  // A new set starts unbegun.
  Model r;
  r.MatchBegan(true, "bo", 0);
  r.GameBegan(100);
  r.MatchBegan(true, "cy", 200);
  r.OpponentLeft(300);
  EXPECT_EQ(r.At(300).notice->line, "No contest in 0:15");
}

TEST(OrcaSetEndCountdown, BackInTheRoomTakesTheNoticeAway)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.OpponentLeft(0);
  m.OpponentBack(4000);
  EXPECT_FALSE(m.At(4000).notice);
  EXPECT_FALSE(m.At(4000).hides_waiting);
}

TEST(OrcaSetEndCountdown, OnlyMidRankedSet)
{
  Model none;
  none.OpponentLeft(0);
  none.Stall(5000, 0);
  EXPECT_TRUE(none.At(0).Empty());

  Model casual;
  casual.MatchBegan(false, "bo", 0);
  casual.OpponentLeft(0);
  casual.Stall(5000, 0);
  casual.OpponentDropped(0);
  EXPECT_TRUE(casual.At(0).Empty());

  // Once the block says the set is over, nobody forfeits it any more.
  Model over;
  over.MatchBegan(true, "bo", 0);
  over.BlockSetOver(100, true, 2, 0, 1500, 1000);
  over.OpponentLeft(1200);
  EXPECT_FALSE(over.At(1300).notice);
  ASSERT_TRUE(over.At(1300).panel);
}

// ---- VICTORY / DEFEAT ----

TEST(OrcaSetEndPanel, TheBlockOpensVictoryOnceAndTheRoomsRatingCountsIn)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.BlockSetOver(4321, true, 2, 1, 1532, 1000);
  View v = m.At(1000);
  ASSERT_TRUE(v.panel);
  EXPECT_FALSE(v.notice);
  EXPECT_TRUE(v.panel->ranked);
  EXPECT_EQ(v.panel->title, "VICTORY");
  EXPECT_EQ(v.panel->score, "2–1");
  EXPECT_EQ(v.panel->detail, "vs bo");
  EXPECT_TRUE(v.panel->show_rating);
  EXPECT_EQ(v.panel->rating.phase, Phase::Updating);
  EXPECT_LT(v.panel->scale, 1.0f);  // its entrance
  EXPECT_FLOAT_EQ(m.At(1000 + Model::ENTER_MS).panel->scale, 1.0f);
  // The same set's block again (every first run on that screen) opens nothing new: the panel is
  // still the one from 1000, settled at 3000.
  m.BlockSetOver(4321, true, 2, 1, 1532, 3000);
  ASSERT_TRUE(m.At(1100).panel);
  EXPECT_FLOAT_EQ(m.At(3000).panel->scale, 1.0f);
  // The room's verdict with the site's ratings: counted from the room's number before.
  m.SetVerdict(Outcome::Won, false, 2, 1, 1530, 1546, 1532, 2000);
  v = m.At(2000 + RatingCount::COUNT_MS);
  EXPECT_EQ(v.panel->rating.shown, 1546);
  EXPECT_EQ(v.panel->rating.delta, 16);
  // The page's line after it is no news.
  m.QueueRating(1546, 3500);
  m.QueueRating(1532, 3600);
  EXPECT_EQ(m.At(5000).panel->rating.shown, 1546);
  // Shown PANEL_MS at least, PANEL_AFTER_LAND_MS after the count lands.
  const double land = 2000 + RatingCount::COUNT_MS;
  const double end = std::max(1000 + Model::PANEL_MS, land + Model::PANEL_AFTER_LAND_MS);
  EXPECT_TRUE(m.At(end - 1).panel);
  EXPECT_FALSE(m.At(end).panel);
}

TEST(OrcaSetEndPanel, DefeatWithThePagesRatingAndNoneAfterTheWait)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.BlockSetOver(77, false, 1, 2, 1532, 0);
  EXPECT_EQ(m.At(0).panel->title, "DEFEAT");
  EXPECT_EQ(m.At(0).panel->score, "1–2");
  // The page's number before the set, sent again as it searches anew, is ignored.
  m.QueueRating(1532, 500);
  EXPECT_EQ(m.At(600).panel->rating.phase, Phase::Updating);
  m.QueueRating(1517, 1500);
  EXPECT_EQ(m.At(1500 + RatingCount::COUNT_MS).panel->rating.shown, 1517);
  EXPECT_EQ(m.At(1500 + RatingCount::COUNT_MS).panel->rating.delta, -15);

  // Nothing comes: "Rating updating…" for the wait, and the panel goes with it.
  Model quiet;
  quiet.MatchBegan(true, "bo", 0);
  quiet.BlockSetOver(78, false, 0, 2, 1532, 0);
  EXPECT_TRUE(quiet.At(RatingCount::WAIT_MS - 1).panel);
  EXPECT_EQ(quiet.At(RatingCount::WAIT_MS - 1).panel->rating.phase, Phase::Updating);
  EXPECT_FALSE(quiet.At(RatingCount::WAIT_MS).panel);
  // A number after the wait is too late.
  quiet.QueueRating(1500, RatingCount::WAIT_MS + 10);
  EXPECT_FALSE(quiet.At(RatingCount::WAIT_MS + 10).panel);
}

TEST(OrcaSetEndPanel, TheVerdictAloneOpensItAndAVoidSetSaysSo)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.SetVerdict(Outcome::Lost, false, 0, 2, -1, -1, 1400, 0);
  ASSERT_TRUE(m.At(0).panel);
  EXPECT_EQ(m.At(0).panel->title, "DEFEAT");

  Model v;
  v.MatchBegan(true, "bo", 0);
  v.BlockSetOver(5, true, 2, 0, 1500, 0);
  v.SetVerdict(Outcome::Void, false, 0, 0, -1, -1, 1500, 100);
  const View view = v.At(200);
  EXPECT_FALSE(view.panel);
  ASSERT_TRUE(view.notice);
  EXPECT_EQ(view.notice->title, "No contest");
  EXPECT_EQ(view.notice->line, "This set doesn't count");
}

TEST(OrcaSetEndPanel, ANewMatchClearsEverything)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.BlockSetOver(5, true, 2, 0, 1500, 0);
  m.OpponentLeft(10);
  m.MatchBegan(true, "cy", 100);
  EXPECT_TRUE(m.At(100).Empty());
  EXPECT_EQ(m.Opponent(), "cy");
  // The new set's block opens its own panel, even with the old key.
  m.BlockSetOver(5, false, 0, 2, 1500, 200);
  EXPECT_EQ(m.At(200).panel->title, "DEFEAT");
  EXPECT_EQ(m.At(200).panel->detail, "vs cy");
}

// ---- Leaving, casual ----

TEST(OrcaSetEndNotice, LeavingARankedSetSaysItCountsAsALoss)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.GameBegan(500);
  m.SelfLeft(false, 1000);
  View v = m.At(1000);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "You left");
  EXPECT_EQ(v.notice->line, "This set counts as a loss");
  EXPECT_EQ(v.notice->tone, Tone::Loss);
  EXPECT_TRUE(m.At(1000 + Model::NOTICE_MS - 1).notice);
  EXPECT_FALSE(m.At(1000 + Model::NOTICE_MS).notice);

  // A second leave inside the notice keeps it.
  m.SelfLeft(true, 1100);
  EXPECT_EQ(m.At(1100).notice->title, "You left");
}

// Before the set's first game began the room voids a leave, and rates nobody: a no-show always (the
// pick runs out on game 1's character select), or a leave or a crash during the stage's loading
// image (YouGame docs/ORCA_ONLINE_UX.md "3B leaving mid-game").
TEST(OrcaSetEndNotice, LeavingBeforeTheFirstGameSaysItDoesNotCount)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.SelfLeft(false, 1000);
  View v = m.At(1000);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "You left");
  EXPECT_EQ(v.notice->line, "This set doesn't count");
  EXPECT_EQ(v.notice->tone, Tone::Neutral);
  EXPECT_FALSE(m.At(1000 + Model::NOTICE_MS).notice);

  Model n;
  n.MatchBegan(true, "bo", 0);
  n.SelfLeft(true, 0);
  n.SelfLeft(false, 100);  // the leave that follows keeps the no-show's words
  EXPECT_EQ(n.At(100).notice->title, "Time's up");
  EXPECT_EQ(n.At(100).notice->line, "You didn't pick in time · This set doesn't count");
  EXPECT_EQ(n.At(100).notice->tone, Tone::Neutral);

  // A game that ended (GameBegan from a result) counts as begun: a later no-show is a loss.
  Model o;
  o.MatchBegan(true, "bo", 0);
  o.GameBegan(50);
  o.SelfLeft(true, 100);
  EXPECT_EQ(o.At(100).notice->line, "You didn't pick in time · This set counts as a loss");
  EXPECT_EQ(o.At(100).notice->tone, Tone::Loss);
}

TEST(OrcaSetEndNotice, CasualGamesAndTheOpponentLeaving)
{
  Model m;
  m.MatchBegan(false, "bo", 0);
  m.CasualGame(Outcome::Won, 2, 1, 1000);
  View v = m.At(1000);
  ASSERT_TRUE(v.panel);
  EXPECT_FALSE(v.panel->ranked);
  EXPECT_EQ(v.panel->title, "YOU WON");
  EXPECT_EQ(v.panel->score, "2–1");
  EXPECT_EQ(v.panel->detail, "vs bo");
  EXPECT_FALSE(v.panel->show_rating);
  EXPECT_FALSE(m.At(1000 + Model::CASUAL_MS).panel);
  m.CasualGame(Outcome::Lost, 2, 2, 20000);
  EXPECT_EQ(m.At(20000).panel->title, "YOU LOST");
  m.CasualOpponentLeft(21000);
  v = m.At(21000);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "bo left");
  EXPECT_EQ(v.notice->line, "Searching again");
  EXPECT_FALSE(v.hides_waiting);
  m.CasualGame(Outcome::Void, 2, 2, 30000);
  EXPECT_EQ(m.At(30000).notice->line, "That game didn't count");
}

// ---- Set over (the room stays open after the set) ----

// After the verdict the room stays open: the opponent leaving or going quiet is no forfeit, only
// "<name> left"; the way out shows while on the results screen.
TEST(OrcaSetEndSetOver, NothingIsAForfeitAndTheOpponentLeavingIsOnlyThat)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.BlockSetOver(9, true, 2, 1, 1532, 1000);
  m.SetVerdict(Outcome::Won, false, 2, 1, 1530, 1546, 1532, 1500);
  EXPECT_FALSE(m.InSetOver());
  m.SetOver(1500);
  EXPECT_TRUE(m.InSetOver());
  // Leaves, stalls and drops that would count a forfeit down mid-set show nothing here.
  m.OpponentLeft(2000);
  m.Stall(5000, 2000);
  m.OpponentDropped(2000);
  View v = m.At(2100);
  EXPECT_FALSE(v.notice);
  EXPECT_FALSE(v.hides_waiting);
  ASSERT_TRUE(v.panel);
  EXPECT_EQ(v.panel->title, "VICTORY");
  // The opponent leaves the room.
  m.OpponentLeftAfterSet(3000);
  v = m.At(3000);
  ASSERT_TRUE(v.notice);
  EXPECT_EQ(v.notice->title, "bo left");
  EXPECT_EQ(v.notice->line, "");
  EXPECT_EQ(v.notice->tone, Tone::Neutral);
  EXPECT_EQ(v.notice->seconds, -1);
  EXPECT_FALSE(v.hides_waiting);
  EXPECT_FALSE(m.At(3000 + Model::NOTICE_MS).notice);

  // This player leaving takes the note and the hint away, and the opponent's leave after it is no
  // news.
  m.OpponentLeftAfterSet(4000);
  m.LeaveHint(true, true, 0);
  ASSERT_TRUE(m.At(4100).notice);
  ASSERT_TRUE(m.At(4100).hint);
  m.LeavingSetRoom(4200);
  EXPECT_FALSE(m.At(4200).notice);
  EXPECT_FALSE(m.At(4200).hint);
  m.LeaveHint(true, true, 0);
  m.OpponentLeftAfterSet(4300);
  EXPECT_FALSE(m.At(4300).notice);
  EXPECT_FALSE(m.At(4300).hint);
}

TEST(OrcaSetEndSetOver, OnlyAfterTheRoomSaysSo)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.OpponentLeftAfterSet(100);
  m.LeaveHint(true, false, 0.5f);
  EXPECT_TRUE(m.At(100).Empty());
  // Mid-set the leave still counts the forfeit down.
  m.OpponentLeft(200);
  EXPECT_EQ(m.At(200).notice->title, "bo disconnected");
  // A new match clears set over.
  m.SetOver(300);
  m.MatchBegan(true, "cy", 400);
  EXPECT_FALSE(m.InSetOver());
  m.LeaveHint(true, false, 0);
  EXPECT_TRUE(m.At(400).Empty());
}

TEST(OrcaSetEndSetOver, TheLeaveHintAndBsHold)
{
  Model m;
  m.MatchBegan(true, "bo", 0);
  m.SetOver(0);
  m.LeaveHint(true, false, 0);
  View v = m.At(10);
  ASSERT_TRUE(v.hint);
  EXPECT_EQ(v.hint->text, "Hold B to leave");
  EXPECT_FLOAT_EQ(v.hint->progress, 0.0f);
  EXPECT_FALSE(v.Empty());
  m.LeaveHint(true, false, 0.4f);
  EXPECT_FLOAT_EQ(m.At(20).hint->progress, 0.4f);
  EXPECT_EQ(m.At(20).hint->text, "Hold B to leave");  // the bar fills, the words stay
  m.LeaveHint(true, false, 3.0f);
  EXPECT_FLOAT_EQ(m.At(30).hint->progress, 1.0f);
  // The opponent gone: Start too.
  m.LeaveHint(true, true, 0);
  EXPECT_EQ(m.At(40).hint->text, "Press Start to continue · Hold B to leave");
  // Left, or off the results screen.
  m.LeaveHint(false, true, 0.7f);
  EXPECT_FALSE(m.At(50).hint);
  EXPECT_TRUE(m.At(50).Empty());
}

// ---- Drawing ----

class OrcaSetEndDraw : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |=
        ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
  }
  void TearDown() override { ImGui::DestroyContext(m_context); }
  static int Frame(float width, float height, const View& view)
  {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    Draw(ImGui::GetForegroundDrawList(), height / 100, 0, 0, width, height, view);
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
  }
  ImGuiContext* m_context = nullptr;
};

// Every screen draws, at every stage of its animations, in any picture the overlay draws in,
// without an ImGui assert; an empty view draws nothing.
TEST_F(OrcaSetEndDraw, EveryScreenDrawsWithoutAnImGuiAssert)
{
  Model victory;
  victory.MatchBegan(true, "bo", 0);
  victory.BlockSetOver(1, true, 2, 1, 1532, 0);
  victory.QueueRating(1548, 1500);
  Model forfeit;
  forfeit.MatchBegan(true, "bo", 0);
  forfeit.OpponentLeft(0);
  Model forfeited;
  forfeited.MatchBegan(true, "bo", 0);
  forfeited.SetVerdict(Outcome::Won, true, 1, 0, -1, -1, 1532, 0);
  Model left;
  left.MatchBegan(true, "bo", 0);
  left.SelfLeft(false, 0);
  Model casual;
  casual.MatchBegan(false, "bo", 0);
  casual.CasualGame(Outcome::Lost, 0, 1, 0);
  casual.CasualOpponentLeft(0);
  Model stay;
  stay.MatchBegan(true, "bo", 0);
  stay.BlockSetOver(1, false, 1, 2, 1488, 0);
  stay.SetOver(0);
  stay.OpponentLeftAfterSet(0);
  stay.LeaveHint(true, true, 0.6f);
  Model hint_only;
  hint_only.MatchBegan(true, "bo", 0);
  hint_only.SetOver(0);
  hint_only.LeaveHint(true, false, 0);
  for (const float h : {100.0f, 480.0f, 720.0f, 2160.0f})
  {
    for (double t = 0; t < 6000; t += 125)
    {
      SCOPED_TRACE(testing::Message() << h << " at " << t);
      for (const Model* m : {&victory, &forfeit, &forfeited, &left, &casual, &stay, &hint_only})
      {
        const View view = m->At(t);
        const int vertices = Frame(h * 16 / 9, h, view);
        if (view.Empty())
          EXPECT_EQ(vertices, 0);
        else
          EXPECT_GT(vertices, 0);
      }
    }
  }
  EXPECT_EQ(Frame(1280, 720, View{}), 0);
}

TEST(OrcaSetEndPanel, AuthoritativeOutcomeCanWaitForGameReceiptsWithoutAPartialScore)
{
  for (const bool forfeit : {false, true})
  {
    for (const int wins : {0, 2})
    {
      Model m;
      m.MatchBegan(true, "bo", 0);
      m.BlockSetOver(1, true, 1, 0, 1500, 0);
      m.SetVerdict(Outcome::Won, forfeit, -1, -1, -1, -1, 1500, 1000);
      m.BlockSetOver(2, true, 1, 0, 1500, 2000);
      const auto pending = m.At(4000);
      ASSERT_TRUE(pending.panel);
      EXPECT_EQ(pending.panel->out, Outcome::Won);
      EXPECT_EQ(pending.panel->score, "Score pending");
      m.SetVerdict(Outcome::Won, forfeit, wins, 0, -1, -1, 1500, 4100);
      const auto complete = m.At(7000);
      ASSERT_TRUE(complete.panel);
      EXPECT_EQ(complete.panel->score, wins ? "2–0" : "0–0");
    }
  }
}
