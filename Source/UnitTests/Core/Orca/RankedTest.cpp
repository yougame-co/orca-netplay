// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The ranked stage flow (ORCA.md "Ranked steps"): the shared step machine (RankedSteps.h),
// the match block (MatchBlock.h) and Project+'s flow on its stage select (RankedPPlus.h), over a
// fake of the game's memory.

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/PPlus/RankedPPlus.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using Ranked::StageMask;
using Ranked::StepKind;

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
    if (track_writes)
      recorded.push_back(a);
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
  void Fill(u32 a, u32 n, u8 v = 0)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = v;
  }
  void Text(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  bool track_writes = false;
  std::vector<u32> recorded;
  std::map<u32, u8> bytes;
};

constexpr u32 SCENE = 0x80910000;
constexpr u32 SCENE_NAME = 0x80920000;
constexpr u32 TASK = 0x80930000;
constexpr u32 RESULT_INFO = 0x9017F420;
constexpr u32 MODE_MELEE = 0x90180F20;

// The preset's page 0 in the ruleset's order of the legal list: GHZ, BC, FH, DL, ToT, SV, BF, PS2,
// LM (Switch03.rss), so position p is legal stage POS_LEGAL[p].
constexpr int POS_LEGAL[9] = {5, 6, 7, 8, 4, 2, 0, 1, 3};
int PosOf(int legal)
{
  for (int p = 0; p < 9; ++p)
  {
    if (POS_LEGAL[p] == legal)
      return p;
  }
  return -1;
}

// Project+'s memory as RankedPPlus reads it: the scene manager and the scene's name, the stage
// select task, the preset and the strike table, the match setup and the result info, and the
// match block's dead code.
FakeMemory Game(std::string_view scene)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x10);
  m.Fill(SCENE, 0x400);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, SCENE);
  m.Write32(SCENE, SCENE_NAME);
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
  m.Write32(SCENE + 0x3AC, TASK);
  m.Fill(TASK, 0x300);
  m.Write32(TASK + RankedPPlus::SSS_SELECTED, 0xFFFFFFFF);
  m.Write32(TASK + RankedPPlus::SSS_CONTROLLER, 0xF0);
  // Project+'s netplay default preset in RSS_EXDATA (any other bytes will do), its tables after.
  m.Fill(RankedPPlus::RSS_EXDATA, 0x400, 0x11);
  // Past the preset's first bytes every preset is the same: the slot table (kinds) included.
  for (u32 i = RankedPPlus::PRESET_BYTES; i < RankedPPlus::RSS_EXDATA_SIZE; ++i)
    m.Write8(RankedPPlus::RSS_EXDATA + i, RankedPPlus::kSwitch03[i]);
  m.Fill(RankedPPlus::STAGE_STRIKE_TABLE - 1, 0x1F, 0);
  m.Fill(RankedPPlus::CURRENT_PAGE, 4);
  m.Fill(0x805A00E0, 4);
  m.Fill(0x90181300, 0x40);
  m.Write32(0x805A00E0, 0x90181300);
  m.Write32(0x90181308, MODE_MELEE);
  m.Write32(0x90181318, RESULT_INFO);
  m.Fill(MODE_MELEE, 0x100);
  m.Fill(RESULT_INFO, 0x24 + 4 * 0x2AC + 4);
  for (u32 port = 0; port < 4; ++port)
    m.Write8(RESULT_INFO + 0x24 + port * 0x2AC + 0x01, 3);
  m.Fill(MatchBlock::BASE, 0x200, 0x3A);
  m.Write32(MatchBlock::BASE, FreeSpace::kMatchBlockOriginalWord);
  return m;
}

void SetScene(FakeMemory& m, std::string_view scene)
{
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
}

// A results screen where `winner` (port 0 or 1) took the game.
void SetResult(FakeMemory& m, int winner)
{
  m.Write8(RESULT_INFO + 0x01, 1);
  m.Write16(RESULT_INFO + 0x0E, 1);
  for (u32 port = 0; port < 2; ++port)
  {
    const u32 rec = RESULT_INFO + 0x24 + port * 0x2AC;
    m.Write8(rec + 0x00, port == 0 ? 0x00 : 0x03);
    m.Write8(rec + 0x01, 0);
    m.Write8(rec + 0x0A, static_cast<int>(port) == winner ? 1 : 0);
    m.Write8(rec + 0x0E, static_cast<int>(port) == winner ? 0 : 1);
  }
}

u32 Page0Bits(const FakeMemory& m)
{
  return u32(m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 2)) << 16 |
         m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 4);
}

u32 BitsOf(StageMask legal)
{
  u32 bits = 0;
  for (int i = 0; i < 9; ++i)
  {
    if (legal & (1u << i))
      bits |= 1u << PosOf(i);
  }
  return bits;
}

MatchBlock::State Block(const FakeMemory& m)
{
  const auto s = MatchBlock::Read(m);
  EXPECT_TRUE(s.has_value());
  return s.value_or(MatchBlock::State{});
}

// A ranked Project+ set in the block, with the 2024 Proposed preset already loaded (as the frame
// hook leaves it outside the stage select).
FakeMemory RankedSss(u8 coin)
{
  FakeMemory m = Game("scSelctCharacter");
  MatchBlock::Write(
      m, MatchBlock::State::Fresh(MatchBlock::MODE_RANKED, Ranked::RULESET_PPLUS_2024, coin));
  RankedPPlus::Apply(m, 1, true);
  SetScene(m, "scSelStage");
  return m;
}

// The two cursors (StageCursors.h) as the latch leaves them: `port`'s on the middle of legal stage
// `legal`'s tile on page 0.
void Aim(FakeMemory& m, int port, int legal)
{
  auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  const StageCursors::Tile* t = StageCursors::TileOf(StageCursors::PPlusLayout(), legal, 0);
  ASSERT_NE(t, nullptr);
  c->cursors[port].x = static_cast<s16>((t->rect.x0 + t->rect.x1) / 2);
  c->cursors[port].y = static_cast<s16>((t->rect.y0 + t->rect.y1) / 2);
  StageCursors::Write(m, *c);
}

// `port`'s cursor on the middle of the Random button.
void AimRandom(FakeMemory& m, int port)
{
  auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  const StageCursors::Rect& r = StageCursors::PPlusLayout().random_button;
  c->cursors[port].x = static_cast<s16>((r.x0 + r.x1) / 2);
  c->cursors[port].y = static_cast<s16>((r.y0 + r.y1) / 2);
  StageCursors::Write(m, *c);
}

// `port`'s buttons in the frame that just ran (the latch's). A cursor no latch has read yet
// (StageCursors::Start's HELD_ALL) is first read with nothing held, as for a player who let go
// before the stage select; AnAMashedOrHeldIntoTheStageSelectNamesNothing drives the real latch.
void Hold(FakeMemory& m, int port, u8 buttons)
{
  auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  if (c->cursors[port].now == StageCursors::HELD_ALL)
    c->cursors[port].seen = 0;
  c->cursors[port].now = buttons;
  StageCursors::Write(m, *c);
}

// `port` presses `button` on `legal`: a boundary with it held, one with it let go; each run twice
// (the second must change nothing).
void Press(FakeMemory& m, int port, int legal, u8 button, int* frame)
{
  if (legal >= 0)
    Aim(m, port, legal);
  Hold(m, port, button);
  RankedPPlus::Apply(m, ++*frame, true);
  EXPECT_EQ(RankedPPlus::Apply(m, *frame, true), 0);
  Hold(m, port, 0);
  RankedPPlus::Apply(m, ++*frame, true);
  EXPECT_EQ(RankedPPlus::Apply(m, *frame, true), 0);
}

constexpr u8 BTN_A = StageCursors::BTN_A;
constexpr u8 BTN_B = StageCursors::BTN_B;
constexpr u8 BTN_X = StageCursors::BTN_X;
}  // namespace

TEST(OrcaRankedSteps, PPlusGameOneIsOneTwoOneOnTheStartersFromTheCoin)
{
  const Ranked::Ruleset& r = Rulesets::PPlus2024();
  for (int coin = 0; coin < 2; ++coin)
  {
    Ranked::SetView v;
    v.coin = coin;
    const Ranked::Plan plan = Ranked::PlanGame(r, v);
    ASSERT_EQ(plan.steps.size(), 3u);
    EXPECT_EQ(plan.available, r.starters);
    EXPECT_EQ(plan.steps[0], (Ranked::Step{StepKind::Strike, static_cast<u8>(coin), 1, false, 30}));
    EXPECT_EQ(plan.steps[1],
              (Ranked::Step{StepKind::Strike, static_cast<u8>(1 - coin), 2, false, 30}));
    EXPECT_EQ(plan.steps[2], (Ranked::Step{StepKind::Pick, static_cast<u8>(coin), 1, false, 10}));
  }
}

TEST(OrcaRankedSteps, PPlusLaterGamesWinnerStrikesTwoLoserPicksWithFullDsr)
{
  const Ranked::Ruleset& r = Rulesets::PPlus2024();
  Ranked::SetView v;
  v.game = 3;
  v.last_winner = 1;
  // Port 1 won game 1 on Battlefield, port 2 won game 2 on Smashville; port 1 picks game 3.
  v.won_on = {0x0001, 0x0004};
  const Ranked::Plan plan = Ranked::PlanGame(r, v);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[0], (Ranked::Step{StepKind::Strike, 1, 2, false, 30}));
  EXPECT_EQ(plan.steps[1], (Ranked::Step{StepKind::Pick, 0, 1, false, 30}));
  EXPECT_EQ(plan.available, r.All() & ~StageMask{0x0001});
  // The winner strikes Smashville and PS2; the loser may pick the 6 left, never Battlefield.
  EXPECT_EQ(Ranked::Allowed(plan, 1, 0x0006), r.All() & ~StageMask{0x0007});
}

TEST(OrcaRankedSteps, BrawlBanIsOptionalMetaKnightAndIceClimbersClauses)
{
  const Ranked::Ruleset& r = Rulesets::Brawl2025();
  Ranked::SetView v;
  v.game = 2;
  v.last_winner = 0;
  Ranked::Plan plan = Ranked::PlanGame(r, v);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[0], (Ranked::Step{StepKind::Ban, 0, 1, true, 30}));
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 0, 0, 0), 0);  // the ban's timer: no ban
  // Ice Climbers: FD (5) never available.
  v.characters = {0x10, 0x00};
  plan = Ranked::PlanGame(r, v);
  EXPECT_EQ(plan.available & (1 << 5), 0);
  // Exactly one Meta Knight (port 1): port 2 picks any stage, game 1 too.
  v = {};
  v.characters = {0x18, 0x00};
  plan = Ranked::PlanGame(r, v);
  ASSERT_EQ(plan.steps.size(), 1u);
  EXPECT_EQ(plan.steps[0].kind, StepKind::Pick);
  EXPECT_EQ(plan.steps[0].port, 1);
  EXPECT_EQ(plan.available, r.All());
  // Both Meta Knight: no clause.
  v.characters = {0x18, 0x18};
  EXPECT_EQ(Ranked::PlanGame(r, v).steps.size(), 3u);
}

TEST(OrcaRankedSteps, TimerDefaultsTakeTheFirstStagesInListOrder)
{
  const Ranked::Ruleset& r = Rulesets::PPlus2024();
  Ranked::SetView v;
  const Ranked::Plan plan = Ranked::PlanGame(r, v);
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 0, 0, 0), 0x0001);
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 1, 0x0001, 0), 0x0006);
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 1, 0x0001, 1), 0x0002);
  EXPECT_EQ(Ranked::DefaultPick(plan, 2, 0x0007), 3);
  EXPECT_EQ(Ranked::StepFrames(r, plan.steps[0]), 30u * 60 + 180);
}

TEST(OrcaRankedSteps, CoinIsFairAndTheSameEverywhere)
{
  int ones = 0;
  for (int i = 0; i < 2000; ++i)
    ones += MatchBlock::CoinFromCode("ROOM" + std::to_string(i * 7919));
  EXPECT_GT(ones, 900);
  EXPECT_LT(ones, 1100);
  EXPECT_EQ(MatchBlock::CoinFromCode("K7Q2"), MatchBlock::CoinFromCode("K7Q2"));
}

TEST(OrcaMatchBlock, RoundTripsAndWritesOnlyWhatChanged)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_FALSE(MatchBlock::Read(m).has_value());  // the dead code is no header
  MatchBlock::State s = MatchBlock::State::Fresh(MatchBlock::MODE_RANKED, 2, 1);
  s.score = {1, 0};
  s.won_on = {0x0100, 0};
  s.deadline = 123456;
  EXPECT_GT(MatchBlock::Write(m, s), 0);
  EXPECT_EQ(MatchBlock::Read(m), s);
  EXPECT_EQ(MatchBlock::Write(m, s), 0);
  EXPECT_EQ(MatchBlock::Clear(m), 4);
  EXPECT_FALSE(MatchBlock::Read(m).has_value());
  EXPECT_EQ(MatchBlock::Clear(m), 0);
  EXPECT_EQ(m.Read32(MatchBlock::BASE), FreeSpace::kMatchBlockOriginalWord);
  // Written again after a clear; never over memory that isn't the dead code.
  EXPECT_GT(MatchBlock::Write(m, s), 0);
  MatchBlock::Clear(m);
  m.Write32(MatchBlock::BASE, 0x12345678);
  EXPECT_EQ(MatchBlock::Write(m, s), 0);
  EXPECT_FALSE(MatchBlock::Read(m).has_value());
}

TEST(OrcaRankedPPlus, PageZeroOfTheProposedPresetIsTheLegalList)
{
  FakeMemory m = Game("scSelctCharacter");
  for (u32 i = 0; i < RankedPPlus::RSS_EXDATA_SIZE; ++i)
    m.Write8(RankedPPlus::RSS_EXDATA + i, RankedPPlus::kSwitch03[i]);
  const std::vector<int> page = RankedPPlus::PageZero(m);
  ASSERT_EQ(page.size(), 9u);
  for (int p = 0; p < 9; ++p)
    EXPECT_EQ(page[p], POS_LEGAL[p]) << "position " << p;
}

TEST(OrcaRankedPPlus, NothingWithoutARankedBlock)
{
  FakeMemory m = Game("scSelStage");
  const auto before = m.bytes;
  EXPECT_EQ(RankedPPlus::Apply(m, 10, true), 0);
  EXPECT_EQ(m.bytes, before);
  const auto masks = RankedPPlus::Masks(m);
  for (const auto& mask : masks)
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaRankedPPlus, PresetOutsideTheStageSelectOnly)
{
  FakeMemory m = Game("scSelctCharacter");
  MatchBlock::Write(
      m, MatchBlock::State::Fresh(MatchBlock::MODE_RANKED, Ranked::RULESET_PPLUS_2024, 0));
  // Past the preset's first bytes nothing is written (every preset is the same there).
  m.Write8(RankedPPlus::RSS_EXDATA + RankedPPlus::PRESET_BYTES, 0x77);
  EXPECT_GT(RankedPPlus::Apply(m, 1, true), 0);
  for (u32 i = 0; i < RankedPPlus::PRESET_BYTES; ++i)
    ASSERT_EQ(m.Read8(RankedPPlus::RSS_EXDATA + i), RankedPPlus::kSwitch03[i]);
  EXPECT_EQ(m.Read8(RankedPPlus::RSS_EXDATA + RankedPPlus::PRESET_BYTES), 0x77);
  EXPECT_EQ(RankedPPlus::Apply(m, 2, true), 0);
  // On the stage select it's left alone.
  SetScene(m, "scSelStage");
  m.Write8(RankedPPlus::RSS_EXDATA, 0x77);
  RankedPPlus::Apply(m, 3, true);
  EXPECT_EQ(m.Read8(RankedPPlus::RSS_EXDATA), 0x77);
}

// A My Music step left from an earlier stage select hid the next one's tiles (the ranked strike
// screen showed only Random) and sent the timer's pick to My Music.
TEST(OrcaRankedPPlus, CharacterSelectClearsALeftoverMyMusicStep)
{
  const auto leftover = [](FakeMemory& m) {
    m.Fill(RankedPPlus::MUSIC_SELECT, 0x30, 0);
    m.Write32(RankedPPlus::MUSIC_SELECT, 1);            // step 1: going to My Music
    m.Write32(RankedPPlus::MUSIC_SELECT + 0x08, 0x2A);  // song
    m.Write32(RankedPPlus::MUSIC_SELECT + 0x14, 0x01000000);
    m.Write32(RankedPPlus::MUSIC_SELECT + 0x2C, 7);
    m.Write32(RankedPPlus::MUSIC_SELECT + 0x04, 0x55);  // not My Music's
    m.Write32(RankedPPlus::MUSIC_SELECT + 0x1C, 0x66);  // not My Music's
  };
  // Ranked, casual, and a session with no flow (With Friends, solo).
  for (const int mode : {int{MatchBlock::MODE_RANKED}, int{MatchBlock::MODE_CASUAL}, -1})
  {
    SCOPED_TRACE(mode);
    FakeMemory m = Game("scSelctCharacter");
    leftover(m);
    if (mode >= 0)
    {
      MatchBlock::Write(
          m, MatchBlock::State::Fresh(static_cast<u8>(mode), Ranked::RULESET_PPLUS_2024, 0));
    }
    EXPECT_EQ(RankedPPlus::ClearStaleMusicSelect(m), 16);
    for (const u32 offset : RankedPPlus::MUSIC_SELECT_WORDS)
      EXPECT_EQ(m.Read32(RankedPPlus::MUSIC_SELECT + offset), 0u) << "word +" << offset;
    EXPECT_EQ(m.Read32(RankedPPlus::MUSIC_SELECT + 0x04), 0x55u);
    EXPECT_EQ(m.Read32(RankedPPlus::MUSIC_SELECT + 0x1C), 0x66u);
    EXPECT_EQ(RankedPPlus::ClearStaleMusicSelect(m), 0);
  }
  // Never on the stage select, where My Music's own steps run.
  FakeMemory sss = RankedSss(0);
  leftover(sss);
  EXPECT_EQ(RankedPPlus::ClearStaleMusicSelect(sss), 0);
  EXPECT_EQ(sss.Read32(RankedPPlus::MUSIC_SELECT), 1u);
}

TEST(OrcaRankedPPlus, GameOneStrikesInTurnsThenTheCoinPicks)
{
  FakeMemory m = RankedSss(1);  // port 2 strikes first
  int frame = 100;
  EXPECT_GT(RankedPPlus::Apply(m, frame, true), 0);
  MatchBlock::State s = Block(m);
  EXPECT_EQ(s.flow, 1);
  EXPECT_EQ(s.step, 0);
  EXPECT_EQ(s.active, 1);
  ASSERT_TRUE(StageCursors::Read(m));
  // The counterpicks struck (not in play), the other pages all struck, a redraw asked for.
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01E0));
  EXPECT_EQ(m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 6), 0xFFFF);
  EXPECT_EQ(m.Read8(RankedPPlus::PAGE_INDEX), 0xFF);
  // Idempotent: the hook again at the same boundary (after a rollback's load) changes nothing.
  EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);
  // The game sees no pad while the two cursors run: the flow acts for the players.
  for (const auto& mask : RankedPPlus::Masks(m))
    EXPECT_EQ(mask, Rollback::InputGate::ALL);

  // Port 1's X does nothing on port 2's turn; port 2 strikes Smashville: port 1's turn, two.
  Press(m, 0, 2, BTN_X, &frame);
  EXPECT_EQ(Block(m).struck, 0);
  Press(m, 1, 2, BTN_X, &frame);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0004);
  EXPECT_EQ(s.step, 1);
  EXPECT_EQ(s.active, 0);
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01E4));
  // A strike on a counterpick (not in play) counts for nothing.
  Press(m, 0, 6, BTN_X, &frame);
  EXPECT_EQ(Block(m).step_done, 0);
  Press(m, 0, 0, BTN_X, &frame);
  EXPECT_EQ(Block(m).step, 1);
  EXPECT_EQ(Block(m).step_done, 1);
  Press(m, 0, 4, BTN_X, &frame);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0015);
  EXPECT_EQ(s.step, 2);
  EXPECT_EQ(s.active, 1);

  // Port 1 proposes PS2 (not its pick); port 2, the picker, picks PS2 with A.
  Press(m, 0, 1, BTN_A, &frame);
  EXPECT_EQ(Block(m).prefs[0], 1);
  EXPECT_FALSE(Block(m).auto_pick);
  Aim(m, 1, 1);
  Hold(m, 1, BTN_A);
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_TRUE(s.auto_pick);
  EXPECT_EQ(s.auto_stage, 1);
  EXPECT_EQ(s.auto_why, MatchBlock::AUTO_PICKED);
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_SELECTED), static_cast<u32>(PosOf(1)));
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_CONTROLLER), 1u);
  EXPECT_EQ(RankedPPlus::Masks(m)[1].press, PAD_BUTTON_A);
  EXPECT_EQ(RankedPPlus::Masks(m)[0].press, 0);

  // The fight starts on it, and port 2 wins it.
  m.Write8(MODE_MELEE + 0x1B, 0x2E);
  SetScene(m, "scMemoryChange");
  RankedPPlus::Apply(m, ++frame, true);
  EXPECT_EQ(Block(m).flow, 1);
  EXPECT_FALSE(StageCursors::Read(m));
  SetScene(m, "scMelee");
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_TRUE(s.fight);
  EXPECT_EQ(s.flow, 0);
  EXPECT_EQ(s.stage, 1);
  EXPECT_TRUE(RankedPPlus::Masks(m)[0].Empty());
  SetResult(m, 1);
  SetScene(m, "scVsResult");
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_FALSE(s.fight);
  EXPECT_EQ(s.games, 1);
  EXPECT_EQ(s.score[1], 1);
  EXPECT_EQ(s.last_winner, 1);
  EXPECT_EQ(s.won_on[1], 0x0002);
  EXPECT_EQ(s.records[0].winner, 1);
  EXPECT_EQ(s.records[0].stage, 1);
  // Counted once.
  RankedPPlus::Apply(m, ++frame, true);
  EXPECT_EQ(Block(m).games, 1);

  // Game 2: the stage select again; port 2 (the winner) strikes 2 from all but PS2 (port 1 never
  // won); then port 1 picks.
  SetScene(m, "scSelctCharacter");
  RankedPPlus::Apply(m, ++frame, true);
  SetScene(m, "scSelStage");
  m.Fill(RankedPPlus::STAGE_STRIKE_TABLE - 1, 0x1F, 0);  // the task's create clears it
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_EQ(s.active, 1);
  EXPECT_EQ(s.struck, 0);
  EXPECT_EQ(s.prefs[0], MatchBlock::NONE);
  EXPECT_EQ(Page0Bits(m), 0u);
  Press(m, 1, 7, BTN_X, &frame);
  Press(m, 1, 8, BTN_X, &frame);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0180);
  EXPECT_EQ(s.step, 1);
  EXPECT_EQ(s.active, 0);
}

TEST(OrcaRankedPPlus, BothProposingOneStageSkipsTheSteps)
{
  // Game 2: port 1 won game 1 on Battlefield. Port 1 (the winner) strikes 2, then port 2 picks.
  // Port 1 proposes Battlefield during its own strikes and port 2 agrees: Battlefield, the rest of
  // the strikes and the pick skipped.
  FakeMemory m = RankedSss(0);
  MatchBlock::State s = Block(m);
  s.games = 1;
  s.score = {1, 0};
  s.last_winner = 0;
  s.won_on = {0x0001, 0};
  MatchBlock::Write(m, s);
  int frame = 300;
  RankedPPlus::Apply(m, frame, true);
  EXPECT_EQ(Block(m).active, 0);
  // Port 2 strikes nothing (not its turn); port 1 strikes Dream Land, which port 2 then can't
  // propose; port 1 proposes Battlefield; B takes it back; again; port 2 agrees.
  Press(m, 0, 8, BTN_X, &frame);
  Press(m, 1, 8, BTN_A, &frame);
  EXPECT_EQ(Block(m).prefs[1], MatchBlock::NONE);
  Press(m, 0, 0, BTN_A, &frame);
  EXPECT_EQ(Block(m).prefs[0], 0);
  Press(m, 0, -1, BTN_B, &frame);
  EXPECT_EQ(Block(m).prefs[0], MatchBlock::NONE);
  Press(m, 0, 0, BTN_A, &frame);
  Aim(m, 1, 0);
  Hold(m, 1, BTN_A);
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_TRUE(s.auto_pick);
  EXPECT_EQ(s.auto_stage, 0);
  EXPECT_EQ(s.auto_why, MatchBlock::AUTO_AGREED);
  EXPECT_EQ(s.struck, 0x0100);
  // Everything else shown struck; A pressed for the cursor's port.
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01FF & ~0x0001));
  const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).set,
            "Ranked · ada 1–0 bo · Agreed: Battlefield");
  const auto masks = RankedPPlus::Masks(m);
  EXPECT_EQ(masks[s.active].press, PAD_BUTTON_A);
  // Nothing more counts.
  Press(m, 0, 2, BTN_X, &frame);
  EXPECT_EQ(Block(m).struck, 0x0100);
}

TEST(OrcaRankedPPlus, TimersStrikeTheFirstStagesThenPickForThePlayer)
{
  FakeMemory m = RankedSss(0);
  int frame = 1000;
  RankedPPlus::Apply(m, frame, true);
  // Port 1's 30 s (and the grace) run out: Battlefield is struck for it.
  frame += 30 * 60 + 180;
  RankedPPlus::Apply(m, frame, true);
  MatchBlock::State s = Block(m);
  EXPECT_EQ(s.struck, 0x0001);
  EXPECT_EQ(s.step, 1);
  EXPECT_EQ(s.deadline, static_cast<u32>(frame + 30 * 60 + 180));
  // The struck art for the default strike: a redraw.
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01E1));
  frame += 30 * 60 + 180;
  RankedPPlus::Apply(m, frame, true);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0007);
  EXPECT_EQ(s.step, 2);
  // The pick's 10 s run out: Luigi's Mansion, the first left, picked for port 1.
  frame += 10 * 60 + 180;
  RankedPPlus::Apply(m, frame, true);
  s = Block(m);
  EXPECT_TRUE(s.auto_pick);
  EXPECT_EQ(s.auto_stage, 3);
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_SELECTED), static_cast<u32>(PosOf(3)));
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01FF & ~0x0008));
  EXPECT_EQ(s.auto_why, MatchBlock::AUTO_TIMER);
  const auto masks = RankedPPlus::Masks(m);
  EXPECT_EQ(masks[0].press, PAD_BUTTON_A);
  EXPECT_TRUE(masks[0].main_stick);
  EXPECT_EQ(masks[1], Rollback::InputGate::ALL);
  // Idempotent at that boundary too.
  EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);
}

TEST(OrcaRankedPPlus, TwoWinsTakeTheSetAndFreeTheStageSelect)
{
  FakeMemory m = RankedSss(0);
  MatchBlock::State s = Block(m);
  s.games = 1;
  s.score = {1, 0};
  s.last_winner = 0;
  s.fight = true;
  s.stage = 0;
  MatchBlock::Write(m, s);
  SetResult(m, 0);
  SetScene(m, "scVsResult");
  RankedPPlus::Apply(m, 50, true);
  s = Block(m);
  EXPECT_TRUE(s.done);
  EXPECT_EQ(s.set_winner, 0);
  EXPECT_EQ(s.score[0], 2);
  SetScene(m, "scSelStage");
  RankedPPlus::Apply(m, 51, true);
  EXPECT_EQ(Block(m).flow, 0);
  for (const auto& mask : RankedPPlus::Masks(m))
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaRankedPPlus, AnOpponentWhoLeavesFreesTheStageSelect)
{
  FakeMemory m = RankedSss(0);
  RankedPPlus::Apply(m, 10, true);
  EXPECT_EQ(Block(m).flow, 1);
  RankedPPlus::Apply(m, 11, false);
  EXPECT_EQ(Block(m).flow, 0);
  for (const auto& mask : RankedPPlus::Masks(m))
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaRankedPPlus, OverlayLines)
{
  FakeMemory m = RankedSss(1);
  int frame = 100;
  RankedPPlus::Apply(m, frame, true);
  const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  // The top line: the set, whose turn, how many stages, the time left; under it the keys for the
  // player whose turn it is, A's proposal for both, what the other proposes.
  RankedPPlus::Lines l = RankedPPlus::Describe(m, 100, ports);
  EXPECT_EQ(l.set, "Ranked · ada 0–0 bo · bo strikes 1 · 0:30");
  // Both cursors start on nothing, so no "Random is casual only" yet
  // (RankedIgnoresRandomAndSaysItIsCasualOnly).
  EXPECT_EQ(l.turn, "A proposes a stage");
  // The turn as STAGE SELECT's relabel names it (Relabel.h): the same step as the line's.
  EXPECT_EQ(RankedPPlus::CurrentTurn(m), (Ranked::Turn{StepKind::Strike, 1, 1}));
  const std::vector<Orca::Events::PortInfo> theirs{{0, "ada", true, {}}, {1, "bo", false, {}}};
  l = RankedPPlus::Describe(m, 100 + 6 * 60, theirs);
  EXPECT_EQ(l.set, "Ranked · ada 0–0 bo · bo strikes 1 · 0:24");
  EXPECT_EQ(l.turn, "Your turn: X on a stage strikes it · A proposes a stage");
  // Port 1's two strikes: the count says how many are left.
  Press(m, 1, 2, BTN_X, &frame);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).set,
            "Ranked · ada 0–0 bo · ada strikes 2 · 0:30");
  EXPECT_EQ(RankedPPlus::CurrentTurn(m), (Ranked::Turn{StepKind::Strike, 0, 2}));
  Press(m, 0, 0, BTN_X, &frame);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).set,
            "Ranked · ada 0–0 bo · ada strikes 1 · 0:30");
  Press(m, 1, 3, BTN_A, &frame);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).turn,
            "Your turn: X on a stage strikes it · bo wants Luigi's Mansion · A on it to agree");
  // The overlay's view of the cursors.
  const StageCursors::View v = RankedPPlus::CursorView(m, ports, frame);
  EXPECT_TRUE(v.on);
  EXPECT_EQ(v.turn, 0);
  // The step for the overlay's turn cues: ada strikes 1 more, the line's clock.
  EXPECT_EQ(v.kind, static_cast<u8>(StepKind::Strike));
  EXPECT_EQ(v.left, 1);
  EXPECT_FALSE(v.both);
  EXPECT_FALSE(v.mk_clause);
  EXPECT_EQ(v.seconds, 30);
  EXPECT_EQ(v.players[1].proposal, 3);
  EXPECT_EQ(v.players[1].name, "bo");
  // On the character select, the set alone.
  SetScene(m, "scSelctCharacter");
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).set, "Ranked · ada 0–0 bo");
  EXPECT_EQ(RankedPPlus::CurrentTurn(m), std::nullopt);
  SetScene(m, "scMelee");
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports), RankedPPlus::Lines{});
}

namespace
{
// A casual Project+ game on the stage select (ORCA.md "Casual stage pick"): each player names a
// preferred stage in turn, then the coin takes one.
FakeMemory CasualSss(u8 coin)
{
  FakeMemory m = Game("scSelctCharacter");
  MatchBlock::Write(
      m, MatchBlock::State::Fresh(MatchBlock::MODE_CASUAL, Ranked::RULESET_PPLUS_2024, coin));
  m.Write32(MatchBlock::ROOM, 0x5EED1234);
  // The casual lock's preset (OnlineRules writes it on the character select).
  for (u32 i = 0; i < RankedPPlus::PRESET_BYTES; ++i)
    m.Write8(RankedPPlus::RSS_EXDATA + i, RankedPPlus::kSwitch03[i]);
  RankedPPlus::Apply(m, 1, true);
  SetScene(m, "scSelStage");
  return m;
}
}  // namespace

TEST(OrcaRankedPPlus, CasualBothNameAStageAtOnce)
{
  FakeMemory m = CasualSss(1);
  int frame = 200;
  EXPECT_GT(RankedPPlus::Apply(m, frame, true), 0);
  MatchBlock::State s = Block(m);
  EXPECT_EQ(s.flow, 1);
  EXPECT_EQ(s.step, 0);
  ASSERT_TRUE(StageCursors::Read(m));
  // Every legal stage in play: nothing struck on page 0; the other pages all struck.
  EXPECT_EQ(Page0Bits(m), 0u);
  EXPECT_EQ(m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 6), 0xFFFF);
  EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);
  for (const auto& mask : RankedPPlus::Masks(m))
    EXPECT_EQ(mask, Rollback::InputGate::ALL);
  const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  RankedPPlus::Lines l = RankedPPlus::Describe(m, frame, ports);
  EXPECT_EQ(l.set, "Stage pick · 0:15");
  EXPECT_EQ(RankedPPlus::CurrentTurn(m), (Ranked::Turn{StepKind::Prefer, Ranked::BOTH, 1}));
  EXPECT_EQ(l.turn,
            "A on a stage or Random · the same pick plays there, else a coin takes one of the "
            "two");
  // Port 2 names Smashville; port 1 names Smashville too: it plays there.
  Press(m, 1, 2, BTN_A, &frame);
  EXPECT_EQ(Block(m).prefs[1], 2);
  EXPECT_FALSE(Block(m).auto_pick);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).turn,
            "A on a stage or Random · the same pick plays there, else a coin takes one of the "
            "two · bo wants Smashville · A on it to agree");
  Aim(m, 0, 2);
  Hold(m, 0, BTN_A);
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_TRUE(s.auto_pick);
  EXPECT_EQ(s.auto_stage, 2);
  EXPECT_EQ(s.auto_why, MatchBlock::AUTO_CASUAL);
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_SELECTED), static_cast<u32>(PosOf(2)));
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01FF & ~(1 << 2)));
  EXPECT_EQ(RankedPPlus::Masks(m)[s.active].press, PAD_BUTTON_A);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).set, "Smashville (both picks)");
  EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);
  // The fight: a game counted (the next coin is another), the flow idle again.
  SetScene(m, "scMelee");
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_EQ(s.flow, 0);
  EXPECT_EQ(s.games, 1);
  EXPECT_FALSE(s.auto_pick);
  EXPECT_EQ(s.prefs[0], MatchBlock::NONE);
  EXPECT_TRUE(RankedPPlus::Masks(m)[0].Empty());
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports), RankedPPlus::Lines{});
  // The preset isn't the flow's to write in casual (the casual lock writes it on the characters).
  SetScene(m, "scSelctCharacter");
  m.Write8(RankedPPlus::RSS_EXDATA, 0x77);
  RankedPPlus::Apply(m, ++frame, true);
  EXPECT_EQ(m.Read8(RankedPPlus::RSS_EXDATA), 0x77);
}

TEST(OrcaRankedPPlus, CasualTwoStagesTheCoinAndTimeRunsOut)
{
  FakeMemory m = CasualSss(1);
  int frame = 200;
  RankedPPlus::Apply(m, frame, true);
  const Ranked::Plan plan = Ranked::PlanCasual(
      Rulesets::PPlus2024(), Ranked::SetView{1, -1, 1, {}, {-1, -1}},
      Ranked::CasualSeed(0x5EED1234, 1, 1));
  // Port 1 names Battlefield and takes it back; port 2 names Dream Land; time runs out: a random
  // legal stage for port 1, then the coin between the two.
  Press(m, 0, 0, BTN_A, &frame);
  Press(m, 0, -1, BTN_B, &frame);
  Press(m, 1, 8, BTN_A, &frame);
  EXPECT_EQ(Block(m).prefs[0], MatchBlock::NONE);
  frame += 15 * 60 + 30;
  RankedPPlus::Apply(m, frame, true);
  const MatchBlock::State s = Block(m);
  EXPECT_TRUE(s.auto_pick);
  EXPECT_EQ(s.prefs[0], Ranked::CasualDefault(plan, 0));
  EXPECT_EQ(s.auto_stage, Ranked::CasualResult(plan, {-1, 8}));
}

TEST(OrcaRankedPPlus, CasualPickWaitsForTwoPlayers)
{
  FakeMemory m = CasualSss(0);
  RankedPPlus::Apply(m, 10, false);
  EXPECT_EQ(Block(m).flow, 0);
  for (const auto& mask : RankedPPlus::Masks(m))
    EXPECT_TRUE(mask.Empty());
  RankedPPlus::Apply(m, 11, true);
  EXPECT_EQ(Block(m).flow, 1);
  EXPECT_TRUE(StageCursors::Read(m));
  RankedPPlus::Apply(m, 12, false);
  EXPECT_EQ(Block(m).flow, 0);
  // The cursor is the remaining player's again, and the two cursors stop.
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_CONTROLLER), 0xF0u);
  EXPECT_FALSE(StageCursors::Read(m));
  EXPECT_EQ(RankedPPlus::Apply(m, 12, false), 0);
}

// Random on Project+'s stage select (casual): A on the game's Random button names Random, B takes
// it back; the block keeps RANDOM, never the stage it draws; the result is CasualResult's.
TEST(OrcaRankedPPlus, CasualRandomOnTheRandomButton)
{
  FakeMemory m = CasualSss(1);
  int frame = 200;
  RankedPPlus::Apply(m, frame, true);
  const Ranked::Plan plan = Ranked::PlanCasual(
      Rulesets::PPlus2024(), Ranked::SetView{1, -1, 1, {}, {-1, -1}},
      Ranked::CasualSeed(0x5EED1234, 1, 1));
  const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  // Port 2 names Random and takes it back; names it again. Nothing is picked yet.
  AimRandom(m, 1);
  Press(m, 1, -1, BTN_A, &frame);
  EXPECT_EQ(Block(m).prefs[1], MatchBlock::RANDOM);
  Press(m, 1, -1, BTN_B, &frame);
  EXPECT_EQ(Block(m).prefs[1], MatchBlock::NONE);
  Press(m, 1, -1, BTN_A, &frame);
  EXPECT_EQ(Block(m).prefs[1], MatchBlock::RANDOM);
  EXPECT_FALSE(Block(m).auto_pick);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).turn,
            "A on a stage or Random · the same pick plays there, else a coin takes one of the "
            "two · bo wants Random · A on it to agree");
  StageCursors::View v = RankedPPlus::CursorView(m, ports, frame);
  EXPECT_EQ(v.players[1].proposal, Ranked::PREF_RANDOM);
  EXPECT_EQ(StageCursors::TurnText(v, 1).mode, StageCursors::TurnCue::Mode::Idle);
  EXPECT_EQ(StageCursors::TurnText(v, 0).detail, "A PICKS A STAGE OR RANDOM");
  // Port 1 names Smashville: the coin takes Smashville or port 2's random stage.
  Press(m, 0, 2, BTN_A, &frame);
  const MatchBlock::State s = Block(m);
  ASSERT_TRUE(s.auto_pick);
  EXPECT_EQ(s.auto_why, MatchBlock::AUTO_CASUAL);
  EXPECT_EQ(s.auto_stage, Ranked::CasualResult(plan, {2, Ranked::PREF_RANDOM}));
  EXPECT_EQ(s.prefs[1], MatchBlock::RANDOM);  // Random stays Random
  const bool random_won = Ranked::CasualChoice(plan) == 1;
  EXPECT_EQ(s.auto_stage, random_won ? Ranked::CasualDefault(plan, 1) : 2);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).set,
            fmt::format("{} ({})", Rulesets::PPlus2024().stage_names[s.auto_stage],
                        random_won ? "random" : "your pick"));
  EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);
  // The flow's A waits while the game's hover still reads its own Random or page button (it
  // follows the cursor a frame late), so Project+ never takes its own random stage.
  ASSERT_TRUE(Block(m).auto_press);
  for (const u32 item : {RankedPPlus::RANDOM_ITEM, RankedPPlus::PAGE_ITEM})
  {
    m.Write32(TASK + RankedPPlus::SSS_HOVERED, item);
    for (const auto& mask : RankedPPlus::Masks(m))
    {
      EXPECT_EQ(mask, Rollback::InputGate::ALL);
      EXPECT_EQ(mask.press, 0);
    }
  }
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, 0x07);  // a stage
  EXPECT_EQ(RankedPPlus::Masks(m)[s.active].press, PAD_BUTTON_A);
  EXPECT_EQ(RankedPPlus::Masks(m)[1 - s.active].press, 0);
}

TEST(OrcaRankedPPlus, CasualBothRandomPicksAtOnceAndARandomOutlastsTheTimer)
{
  // Both name Random: the stage is picked at once, without waiting out the timer.
  {
    FakeMemory m = CasualSss(0);
    int frame = 200;
    RankedPPlus::Apply(m, frame, true);
    const Ranked::Plan plan = Ranked::PlanCasual(
        Rulesets::PPlus2024(), Ranked::SetView{1, -1, 0, {}, {-1, -1}},
        Ranked::CasualSeed(0x5EED1234, 0, 1));
    AimRandom(m, 0);
    Press(m, 0, -1, BTN_A, &frame);
    EXPECT_FALSE(Block(m).auto_pick);
    AimRandom(m, 1);
    Press(m, 1, -1, BTN_A, &frame);
    const MatchBlock::State s = Block(m);
    ASSERT_TRUE(s.auto_pick);
    EXPECT_EQ(s.auto_stage, Ranked::CasualResult(plan, {Ranked::PREF_RANDOM, Ranked::PREF_RANDOM}));
    EXPECT_EQ(s.active, 0);
    EXPECT_EQ(s.prefs[0], MatchBlock::RANDOM);
    EXPECT_EQ(s.prefs[1], MatchBlock::RANDOM);
    const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
    EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).set,
              fmt::format("{} (random)", Rulesets::PPlus2024().stage_names[s.auto_stage]));
  }
  // Random against nobody: time runs out, the same stage as CasualResult's, Random kept.
  {
    FakeMemory m = CasualSss(1);
    int frame = 200;
    RankedPPlus::Apply(m, frame, true);
    const Ranked::Plan plan = Ranked::PlanCasual(
        Rulesets::PPlus2024(), Ranked::SetView{1, -1, 1, {}, {-1, -1}},
        Ranked::CasualSeed(0x5EED1234, 1, 1));
    AimRandom(m, 0);
    Press(m, 0, -1, BTN_A, &frame);
    frame += 15 * 60 + 30;
    RankedPPlus::Apply(m, frame, true);
    const MatchBlock::State s = Block(m);
    ASSERT_TRUE(s.auto_pick);
    EXPECT_EQ(s.auto_stage, Ranked::CasualResult(plan, {Ranked::PREF_RANDOM, Ranked::PREF_NONE}));
    EXPECT_EQ(s.prefs[0], MatchBlock::RANDOM);
    EXPECT_EQ(s.prefs[1], Ranked::CasualDefault(plan, 1));
  }
}

TEST(OrcaRankedPPlus, RankedIgnoresRandomAndSaysItIsCasualOnly)
{
  FakeMemory m = RankedSss(0);
  int frame = 200;
  RankedPPlus::Apply(m, frame, true);
  const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  const MatchBlock::State before = Block(m);
  // Port 1's cursor starts on nothing; its A there does nothing, and no such line.
  const auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  EXPECT_EQ(StageCursors::Hovered(StageCursors::PPlusLayout(), 0, c->cursors[0].x, c->cursors[0].y),
            StageCursors::NOTHING);
  Press(m, 0, -1, BTN_A, &frame);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).turn,
            "Your turn: X on a stage strikes it · A proposes a stage");
  // On Random: A and X do nothing, and the line says why.
  AimRandom(m, 0);
  Press(m, 0, -1, BTN_A, &frame);
  Press(m, 0, -1, BTN_X, &frame);
  MatchBlock::State after = Block(m);
  EXPECT_EQ(after.prefs, before.prefs);
  EXPECT_EQ(after.struck, before.struck);
  EXPECT_FALSE(after.auto_pick);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).turn,
            "Your turn: X on a stage strikes it · A proposes a stage · Random is casual only");
  // Off the button, no such line.
  Aim(m, 0, 2);
  EXPECT_EQ(RankedPPlus::Describe(m, frame, ports).turn,
            "Your turn: X on a stage strikes it · A proposes a stage");
}

// An A mashed into the stage select names nothing, as in 0.3.39: both cursors start on nothing (no
// tile or Random). A button held through the screen change counts once it is let go
// (StageCursors::Start holds every button; the steps take presses in the frame the cursors start,
// before any latch), so an A held onto Random or a stage names nothing either. Ranked's strikes
// work from the start. The real latch reads the pads here.
TEST(OrcaRankedPPlus, AnAMashedOrHeldIntoTheStageSelectNamesNothing)
{
  for (const bool casual : {true, false})
  {
    FakeMemory m = casual ? CasualSss(1) : RankedSss(1);  // ranked: port 2 strikes first
    std::array<std::optional<GCPadStatus>, 4> raw{};
    GCPadStatus pad{};
    pad.isConnected = true;
    pad.stickX = pad.stickY = 128;
    pad.button = PAD_BUTTON_A;
    raw[0] = raw[1] = pad;
    int frame = 200;
    const auto next = [&] {
      StageCursors::Latch(m, raw);
      RankedPPlus::Apply(m, ++frame, true);
      EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);
    };
    EXPECT_EQ(StageCursors::Latch(m, raw), 0);  // no cursors yet
    RankedPPlus::Apply(m, frame, true);
    ASSERT_TRUE(StageCursors::Read(m));
    // Both keep A down, then mash it (press, let go, press...): nothing named or struck.
    for (int i = 0; i < 10; ++i)
      next();
    for (int i = 0; i < 6; ++i)
    {
      raw[0]->button = raw[1]->button = (i & 1) ? PAD_BUTTON_A : 0;
      next();
    }
    EXPECT_EQ(Block(m).prefs[0], MatchBlock::NONE) << "casual " << casual;
    EXPECT_EQ(Block(m).prefs[1], MatchBlock::NONE) << "casual " << casual;
    EXPECT_EQ(Block(m).struck, 0) << "casual " << casual;
    EXPECT_FALSE(Block(m).auto_pick) << "casual " << casual;
    if (!casual)
    {
      // Ranked from the start: port 2 moves 4 frames up-right and 3 right onto Luigi's Mansion
      // (pplus-sss-cursors-ranked.txt) and strikes it with X.
      raw[1]->button = 0;
      raw[1]->stickX = raw[1]->stickY = 255;
      for (int i = 0; i < 4; ++i)
        next();
      raw[1]->stickY = 128;
      for (int i = 0; i < 3; ++i)
        next();
      raw[1]->stickX = 128;
      raw[1]->button = PAD_BUTTON_X;
      next();
      EXPECT_EQ(Block(m).struck, 0x0008);  // Luigi's Mansion
    }
    // Port 1 moves onto Random (casual) or Battlefield (ranked) with A still down: nothing. It lets
    // go for a frame and presses again: named. Port 2 names nothing.
    if (casual)
      AimRandom(m, 0);
    else
      Aim(m, 0, 0);
    next();
    EXPECT_EQ(Block(m).prefs[0], MatchBlock::NONE) << "casual " << casual;
    raw[0]->button = 0;
    next();
    raw[0]->button = PAD_BUTTON_A;
    next();
    EXPECT_EQ(Block(m).prefs[0], casual ? MatchBlock::RANDOM : u8{0}) << "casual " << casual;
    EXPECT_EQ(Block(m).prefs[1], MatchBlock::NONE) << "casual " << casual;
    EXPECT_FALSE(Block(m).auto_pick) << "casual " << casual;
  }
}

TEST(OrcaRankedSteps, CasualIsOnePreferStepForBothOnTheWholeList)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  Ranked::SetView set;
  set.coin = 1;
  Ranked::Plan plan = Ranked::PlanCasual(rules, set, 1234);
  EXPECT_TRUE(plan.casual);
  EXPECT_EQ(plan.seed, 1234u);
  EXPECT_EQ(plan.available, static_cast<StageMask>(0x7F));
  EXPECT_EQ(plan.agreeable, plan.available);
  ASSERT_EQ(plan.steps.size(), 1u);
  EXPECT_EQ(plan.steps[0], (Ranked::Step{StepKind::Prefer, Ranked::BOTH, 1, false, 15}));
  // Players get 15 s to pick, plus a half-second grace.
  EXPECT_EQ(Ranked::StepFrames(rules, plan.steps[0]), 15u * 60 + 30);
  // Never FD with an Ice Climbers; Meta Knight changes nothing in casual.
  set.characters = {rules.ice_climbers, rules.meta_knight};
  plan = Ranked::PlanCasual(rules, set, 1);
  EXPECT_EQ(plan.available, static_cast<StageMask>(0x7F & ~(1 << 5)));
  EXPECT_FALSE(plan.mk_clause);
  EXPECT_EQ(plan.steps.size(), 1u);
  // Project+: the 2024 Proposed list's 9.
  EXPECT_EQ(Ranked::PlanCasual(Rulesets::PPlus2024(), {}, 1).available,
            static_cast<StageMask>(0x1FF));
}

TEST(OrcaRankedSteps, AgreementIsLegalInPlayAndNotStruck)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  Ranked::SetView set;
  // Game 1: the starters.
  Ranked::Plan plan = Ranked::PlanGame(rules, set);
  EXPECT_EQ(plan.agreeable, rules.starters);
  Ranked::Progress p = Ranked::Begin(rules, plan);
  EXPECT_FALSE(Ranked::Propose(plan, &p, 0, 5));  // FD: a counterpick, not in play in game 1
  EXPECT_TRUE(Ranked::Propose(plan, &p, 0, 3));
  EXPECT_FALSE(Ranked::Propose(plan, &p, 0, 3));  // nothing new
  EXPECT_EQ(p.picked, -1);
  // A strike takes the proposal off.
  ASSERT_TRUE(Ranked::Strike(rules, plan, &p, 3));
  EXPECT_EQ(p.prefs[0], -1);
  EXPECT_FALSE(Ranked::Propose(plan, &p, 1, 3));
  EXPECT_TRUE(Ranked::Propose(plan, &p, 1, 4));
  EXPECT_TRUE(Ranked::Withdraw(&p, 1));
  EXPECT_FALSE(Ranked::Withdraw(&p, 1));
  EXPECT_TRUE(Ranked::Propose(plan, &p, 1, 4));
  EXPECT_TRUE(Ranked::Propose(plan, &p, 0, 4));
  EXPECT_EQ(p.picked, 4);
  EXPECT_EQ(Ranked::Current(plan, p), nullptr);
  EXPECT_FALSE(Ranked::Propose(plan, &p, 0, 2));
  // Later games: any legal stage, DSR ignored; never FD with an Ice Climbers.
  set.game = 2;
  set.last_winner = 1;
  set.won_on = {1 << 3, 0};
  set.characters = {rules.ice_climbers, 0};
  plan = Ranked::PlanGame(rules, set);
  EXPECT_EQ(plan.agreeable, static_cast<StageMask>(0x7F & ~(1 << 5)));
  EXPECT_FALSE(plan.available & (1 << 3));
  Ranked::Progress q = Ranked::Begin(rules, plan);
  EXPECT_TRUE(Ranked::Propose(plan, &q, 0, 3));
  EXPECT_FALSE(Ranked::Propose(plan, &q, 1, 5));
  EXPECT_TRUE(Ranked::Propose(plan, &q, 1, 3));
  EXPECT_EQ(q.picked, 3);
}

TEST(OrcaRankedSteps, CasualCoinIsTheSameEverywhereAndFair)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  // The same room, coin and game: the same seed; another game: another.
  EXPECT_EQ(Ranked::CasualSeed(77, 1, 2), Ranked::CasualSeed(77, 1, 2));
  EXPECT_NE(Ranked::CasualSeed(77, 1, 2), Ranked::CasualSeed(77, 1, 3));
  std::array<int, 2> chosen{};
  std::array<int, 7> defaults{};
  for (u32 room = 0; room < 500; ++room)
  {
    for (int game = 1; game <= 4; ++game)
    {
      const Ranked::Plan plan =
          Ranked::PlanCasual(rules, {}, Ranked::CasualSeed(room * 2654435761u, room & 1, game));
      ++chosen[Ranked::CasualChoice(plan)];
      const int d = Ranked::CasualDefault(plan, game & 1);
      ASSERT_GE(d, 0);
      ASSERT_LT(d, 7);
      ++defaults[d];
    }
  }
  EXPECT_GT(chosen[0], 900);
  EXPECT_GT(chosen[1], 900);
  for (const int n : defaults)
    EXPECT_GT(n, 180);
  // A default is always an allowed stage (FD out with an Ice Climbers).
  Ranked::SetView ic;
  ic.characters = {rules.ice_climbers, 0};
  for (u32 seed = 0; seed < 200; ++seed)
  {
    const Ranked::Plan plan = Ranked::PlanCasual(rules, ic, seed);
    EXPECT_NE(Ranked::CasualDefault(plan, 0), 5);
    EXPECT_NE(Ranked::CasualDefault(plan, 1), 5);
  }
}

TEST(OrcaRankedSteps, ProgressCasualBothNameThenTheSameOrTheCoin)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  Ranked::SetView set;
  const Ranked::Plan plan = Ranked::PlanCasual(rules, set, 99);
  Ranked::Progress p = Ranked::Begin(rules, plan);
  EXPECT_EQ(p.frames, 15 * 60 + 30);
  EXPECT_EQ(Ranked::SecondsShown(rules, *Ranked::Current(plan, p), p.frames), 15);
  EXPECT_EQ(Ranked::SecondsShown(rules, *Ranked::Current(plan, p), 30), 0);
  // No strikes, bans or picks in a Prefer step.
  EXPECT_FALSE(Ranked::Strike(rules, plan, &p, 0));
  EXPECT_FALSE(Ranked::Skip(rules, plan, &p));
  EXPECT_FALSE(Ranked::Pick(plan, &p, 0));
  // Either may name first, and change it until both have.
  EXPECT_TRUE(Ranked::Propose(plan, &p, 1, 2));
  EXPECT_TRUE(Ranked::Propose(plan, &p, 1, 3));
  EXPECT_EQ(p.picked, -1);
  EXPECT_TRUE(Ranked::Propose(plan, &p, 0, 3));
  EXPECT_EQ(p.picked, 3);
  EXPECT_EQ(Ranked::Current(plan, p), nullptr);
  EXPECT_FALSE(Ranked::Propose(plan, &p, 0, 4));
  // Two different ones: the coin's.
  Ranked::Progress d = Ranked::Begin(rules, plan);
  Ranked::Propose(plan, &d, 0, 1);
  Ranked::Propose(plan, &d, 1, 6);
  EXPECT_EQ(d.picked, Ranked::CasualChoice(plan) == 0 ? 1 : 6);
  EXPECT_EQ(Ranked::CasualResult(plan, {1, 6}), d.picked);
  EXPECT_EQ(Ranked::CasualResult(plan, {2, 2}), 2);
  // Time runs out: each a random legal stage, then the coin's.
  Ranked::Progress q = Ranked::Begin(rules, plan);
  Ranked::Propose(plan, &q, 1, 6);
  for (int i = 0; i < 10000 && q.picked < 0; ++i)
    Ranked::Tick(rules, plan, &q);
  EXPECT_EQ(q.picked, Ranked::CasualResult(plan, {-1, 6}));
  // A stage not allowed (FD with an Ice Climbers) isn't named.
  set.characters = {rules.ice_climbers, 0};
  const Ranked::Plan ic = Ranked::PlanCasual(rules, set, 99);
  Ranked::Progress r = Ranked::Begin(rules, ic);
  EXPECT_FALSE(Ranked::Propose(ic, &r, 0, 5));
  EXPECT_EQ(r.prefs[0], -1);
}

// Casual Random (Ranked::PREF_RANDOM): the seeded default CasualDefault draws for that port, the
// same as naming nothing at the timer, but at once. The marker is kept, never the stage it draws.
TEST(OrcaRankedSteps, CasualRandomIsThePortsSeededDrawAndStaysAMarker)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  const s8 R = Ranked::PREF_RANDOM, N = Ranked::PREF_NONE;
  EXPECT_TRUE(Ranked::Named(R));
  EXPECT_TRUE(Ranked::Named(0));
  EXPECT_FALSE(Ranked::Named(N));
  EXPECT_FALSE(Ranked::Named(-100));
  for (u32 room = 0; room < 64; ++room)
  {
    for (int game = 1; game <= 3; ++game)
    {
      const Ranked::Plan plan =
          Ranked::PlanCasual(rules, {}, Ranked::CasualSeed(room * 2654435761u, room & 1, game));
      const int d0 = Ranked::CasualDefault(plan, 0), d1 = Ranked::CasualDefault(plan, 1);
      const int coin = Ranked::CasualChoice(plan);
      // Random is the same as none in the result.
      EXPECT_EQ(Ranked::CasualResult(plan, {R, 3}), Ranked::CasualResult(plan, {N, 3}));
      EXPECT_EQ(Ranked::CasualResult(plan, {R, R}), Ranked::CasualResult(plan, {N, N}));
      // Both Random: picked the moment the second names it, each port's own draw, then the coin.
      Ranked::Progress both = Ranked::Begin(rules, plan);
      EXPECT_TRUE(Ranked::Propose(plan, &both, 0, R));
      EXPECT_EQ(both.picked, -1);
      EXPECT_EQ(both.prefs[0], R);
      EXPECT_TRUE(Ranked::Propose(plan, &both, 1, R));
      EXPECT_EQ(both.picked, d0 == d1 ? d0 : (coin == 0 ? d0 : d1));
      EXPECT_EQ(both.prefs, (std::array<s8, 2>{R, R}));
      // One Random, one stage: the coin between the stage and the Random's draw.
      for (int stage = 0; stage < rules.stage_count; ++stage)
      {
        Ranked::Progress one = Ranked::Begin(rules, plan);
        EXPECT_TRUE(Ranked::Propose(plan, &one, 1, stage));
        EXPECT_TRUE(Ranked::Propose(plan, &one, 0, R));
        EXPECT_EQ(one.picked, d0 == stage ? stage : (coin == 0 ? d0 : stage));
        EXPECT_EQ(one.prefs[0], R);
      }
      // Two stages: as before, the coin's.
      Ranked::Progress two = Ranked::Begin(rules, plan);
      Ranked::Propose(plan, &two, 0, 1);
      Ranked::Propose(plan, &two, 1, 6);
      EXPECT_EQ(two.picked, coin == 0 ? 1 : 6);
    }
  }
  const Ranked::Plan plan = Ranked::PlanCasual(rules, {}, 99);
  Ranked::Progress p = Ranked::Begin(rules, plan);
  // B takes Random back; naming it twice is no change.
  EXPECT_FALSE(Ranked::Withdraw(&p, 0));
  EXPECT_TRUE(Ranked::Propose(plan, &p, 0, R));
  EXPECT_FALSE(Ranked::Propose(plan, &p, 0, R));
  EXPECT_TRUE(Ranked::Withdraw(&p, 0));
  EXPECT_EQ(p.prefs[0], N);
  // Random then a stage, and back: the last one counts.
  EXPECT_TRUE(Ranked::Propose(plan, &p, 0, R));
  EXPECT_TRUE(Ranked::Propose(plan, &p, 0, 4));
  EXPECT_TRUE(Ranked::Propose(plan, &p, 0, R));
  EXPECT_EQ(p.picked, -1);
  // Time runs out with Random against nobody: CasualResult's stage, the Random kept, the silent
  // port's own draw recorded.
  for (int i = 0; i < 10000 && p.picked < 0; ++i)
    Ranked::Tick(rules, plan, &p);
  EXPECT_EQ(p.picked, Ranked::CasualResult(plan, {R, N}));
  EXPECT_EQ(p.prefs[0], R);
  EXPECT_EQ(p.prefs[1], Ranked::CasualDefault(plan, 1));
  EXPECT_FALSE(Ranked::Propose(plan, &p, 1, R));  // picked: nothing more
  EXPECT_FALSE(Ranked::Withdraw(&p, 0));
  // Never FD with an Ice Climbers, Random or not.
  Ranked::SetView ic;
  ic.characters = {rules.ice_climbers, 0};
  for (u32 seed = 0; seed < 300; ++seed)
  {
    const Ranked::Plan icp = Ranked::PlanCasual(rules, ic, seed);
    Ranked::Progress q = Ranked::Begin(rules, icp);
    Ranked::Propose(icp, &q, 0, R);
    Ranked::Propose(icp, &q, 1, R);
    ASSERT_GE(q.picked, 0);
    EXPECT_NE(q.picked, 5);
  }
  // Nothing allowed to draw from: no Random.
  Ranked::Plan empty = plan;
  empty.available = 0;
  Ranked::Progress e = Ranked::Begin(rules, empty);
  EXPECT_FALSE(Ranked::Propose(empty, &e, 0, R));
  EXPECT_EQ(e.prefs[0], N);
}

TEST(OrcaRankedSteps, RankedHasNoRandom)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  for (const Ranked::SetView& set : {Ranked::SetView{}, Ranked::SetView{2, 1, 0, {8, 0}, {-1, -1}}})
  {
    const Ranked::Plan plan = Ranked::PlanGame(rules, set);
    Ranked::Progress p = Ranked::Begin(rules, plan);
    const Ranked::Progress before = p;
    EXPECT_FALSE(Ranked::Propose(plan, &p, 0, Ranked::PREF_RANDOM));
    EXPECT_FALSE(Ranked::Propose(plan, &p, 1, Ranked::PREF_RANDOM));
    EXPECT_EQ(p, before);
  }
}

// The pick is a pure function of the header (room, coin, game) and the names: the same stage
// whichever order a frame handles the two ports in, a repeated name changes nothing, and nothing
// reads the game's own RNG. This checks the step functions only; rollback re-runs of the flows are
// covered by the Press and Boundary helpers (each runs a frame twice), and an old build meeting a
// new one by the compatibility key (UX::kCompatVersion).
TEST(OrcaRankedSteps, CasualRandomIsTheSameOnBothMachines)
{
  const Ranked::Ruleset& rules = Rulesets::PPlus2024();
  struct Peer
  {
    Ranked::Plan plan;
    Ranked::Progress progress;
  };
  const auto start = [&](u32 room, u8 coin, int game) {
    Peer p;
    p.plan = Ranked::PlanCasual(rules, {}, Ranked::CasualSeed(room, coin, game));
    p.progress = Ranked::Begin(rules, p.plan);
    return p;
  };
  const std::array<std::array<s8, 2>, 6> picks{{{Ranked::PREF_RANDOM, Ranked::PREF_RANDOM},
                                                {Ranked::PREF_RANDOM, 2},
                                                {7, Ranked::PREF_RANDOM},
                                                {3, 3},
                                                {0, 8},
                                                {Ranked::PREF_RANDOM, Ranked::PREF_NONE}}};
  std::array<int, 9> seen{};
  for (u32 room = 1; room < 2000; room += 7)
  {
    for (const auto& names : picks)
    {
      const u8 coin = static_cast<u8>(room & 1);
      const int game = 1 + static_cast<int>(room % 3);
      Peer host = start(room * 2654435761u, coin, game);
      Peer guest = start(room * 2654435761u, coin, game);
      // The host's frame handles port 1 then port 2; the guest's the other way round (it
      // shouldn't matter), and re-runs each press as after a rollback.
      for (int port : {0, 1})
      {
        if (Ranked::Named(names[port]))
          Ranked::Propose(host.plan, &host.progress, port, names[port]);
      }
      for (int port : {1, 0})
      {
        if (!Ranked::Named(names[port]))
          continue;
        Ranked::Propose(guest.plan, &guest.progress, port, names[port]);
        const Ranked::Progress once = guest.progress;
        Ranked::Propose(guest.plan, &guest.progress, port, names[port]);
        EXPECT_EQ(guest.progress, once);
      }
      for (int i = 0; i < 2000 && host.progress.picked < 0; ++i)
      {
        Ranked::Tick(rules, host.plan, &host.progress);
        Ranked::Tick(rules, guest.plan, &guest.progress);
      }
      ASSERT_GE(host.progress.picked, 0);
      EXPECT_EQ(host.progress, guest.progress);
      EXPECT_EQ(host.progress.picked,
                Ranked::CasualResult(host.plan, {names[0], names[1]}));
      if (names[0] == Ranked::PREF_RANDOM && names[1] == Ranked::PREF_RANDOM)
        ++seen[host.progress.picked];
    }
  }
  // Random against Random lands on every legal stage over many rooms.
  for (const int n : seen)
    EXPECT_GT(n, 0);
}

// The shared machine's Progress driver (Brawl's stage select glue drives the steps one action and
// one screen frame at a time).
TEST(OrcaRankedSteps, ProgressDrivesGameOneOneTwoOneThenThePick)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  Ranked::SetView set;
  set.coin = 1;
  const Ranked::Plan plan = Ranked::PlanGame(rules, set);
  Ranked::Progress p = Ranked::Begin(rules, plan);
  ASSERT_NE(Ranked::Current(plan, p), nullptr);
  EXPECT_EQ(Ranked::Current(plan, p)->port, 1);
  EXPECT_EQ(p.frames, Ranked::StepFrames(rules, plan.steps[0]));
  // A counterpick can't be struck in game 1; a starter can.
  EXPECT_FALSE(Ranked::Strike(rules, plan, &p, 5));
  EXPECT_TRUE(Ranked::Strike(rules, plan, &p, 0));
  EXPECT_EQ(Ranked::Current(plan, p)->port, 0);
  EXPECT_TRUE(Ranked::Strike(rules, plan, &p, 1));
  EXPECT_TRUE(Ranked::Strike(rules, plan, &p, 2));
  // The coin's port picks from the last two; a strike is no pick.
  ASSERT_EQ(Ranked::Current(plan, p)->kind, StepKind::Pick);
  EXPECT_EQ(Ranked::Current(plan, p)->port, 1);
  EXPECT_FALSE(Ranked::Strike(rules, plan, &p, 3));
  EXPECT_EQ(Ranked::Allowed(plan, p), static_cast<StageMask>((1 << 3) | (1 << 4)));
  EXPECT_FALSE(Ranked::Pick(plan, &p, 0));
  EXPECT_TRUE(Ranked::Pick(plan, &p, 4));
  EXPECT_EQ(p.picked, 4);
  EXPECT_EQ(Ranked::Current(plan, p), nullptr);
}

TEST(OrcaRankedSteps, ProgressTimersTakeTheDefaults)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  const Ranked::Plan plan = Ranked::PlanGame(rules, {});
  Ranked::Progress p = Ranked::Begin(rules, plan);
  // Every step's timer runs out: the first starters struck in list order, then the first allowed
  // stage picked.
  for (int i = 0; i < 100000 && p.picked < 0; ++i)
    Ranked::Tick(rules, plan, &p);
  EXPECT_EQ(p.struck, static_cast<StageMask>(0x07));
  EXPECT_EQ(p.picked, 3);
  // A timer shows 0:00 through its grace.
  EXPECT_EQ(Ranked::SecondsShown(rules, rules.grace_frames), 0);
  EXPECT_EQ(Ranked::SecondsShown(rules, rules.grace_frames + 1), 1);
  EXPECT_EQ(Ranked::SecondsShown(rules, rules.grace_frames + 60 * 30), 30);
}

TEST(OrcaRankedSteps, ProgressLaterGamesBanIsOptionalAndThePickHasFullDsr)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  Ranked::SetView set;
  set.game = 2;
  set.last_winner = 1;
  set.won_on = {1 << 5, 1 << 3};  // port 1 won on FD, port 2 on Smashville
  const Ranked::Plan plan = Ranked::PlanGame(rules, set);
  Ranked::Progress p = Ranked::Begin(rules, plan);
  ASSERT_EQ(Ranked::Current(plan, p)->kind, StepKind::Ban);
  EXPECT_EQ(Ranked::Current(plan, p)->port, 1);
  // Port 1 (the loser) never picks FD, where it won.
  EXPECT_FALSE(Ranked::Allowed(plan, p) & (1 << 5));
  EXPECT_TRUE(Ranked::Skip(rules, plan, &p));
  ASSERT_EQ(Ranked::Current(plan, p)->kind, StepKind::Pick);
  EXPECT_EQ(Ranked::Current(plan, p)->port, 0);
  EXPECT_FALSE(Ranked::Pick(plan, &p, 5));
  EXPECT_TRUE(Ranked::Pick(plan, &p, 6));
  // The ban's timer running out bans nothing.
  Ranked::Progress q = Ranked::Begin(rules, plan);
  for (u32 i = 0; i < Ranked::StepFrames(rules, plan.steps[0]); ++i)
    Ranked::Tick(rules, plan, &q);
  EXPECT_EQ(q.struck, 0);
  EXPECT_EQ(Ranked::Current(plan, q)->kind, StepKind::Pick);
}

TEST(OrcaRankedSteps, ProgressMetaKnightClauseIsOnePickOfAnyLegalStage)
{
  const Ranked::Ruleset& rules = Rulesets::Brawl2025();
  Ranked::SetView set;
  set.characters = {rules.meta_knight, 0x10};  // Meta Knight against Ice Climbers
  const Ranked::Plan plan = Ranked::PlanGame(rules, set);
  EXPECT_TRUE(plan.mk_clause);
  Ranked::Progress p = Ranked::Begin(rules, plan);
  ASSERT_EQ(Ranked::Current(plan, p)->kind, StepKind::Pick);
  EXPECT_EQ(Ranked::Current(plan, p)->port, 1);
  // Any legal stage but FD (Ice Climbers).
  EXPECT_EQ(Ranked::Allowed(plan, p), static_cast<StageMask>(0x7F & ~(1 << 5)));
}

TEST(OrcaRankedPPlus, EveryMusicCleanupWriteIsDeclared)
{
  FakeMemory m = Game("scSelctCharacter");
  m.Fill(RankedPPlus::MUSIC_SELECT, 0x30);
  for (const u32 offset : RankedPPlus::MUSIC_SELECT_WORDS)
    m.Write32(RankedPPlus::MUSIC_SELECT + offset, 1);
  m.track_writes = true;
  EXPECT_EQ(RankedPPlus::ClearStaleMusicSelect(m), 40);
  ASSERT_EQ(m.recorded.size(), 40u);
  const auto regions = BrawlGame::Regions();
  for (const u32 address : m.recorded)
    EXPECT_TRUE(std::any_of(regions.begin(), regions.end(),
                           [&](const auto& region) { return region.Contains(address, 1); }))
        << std::hex << address;
}
