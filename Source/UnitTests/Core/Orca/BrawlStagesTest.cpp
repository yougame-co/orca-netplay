// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// A ranked set's stage flow on vanilla Brawl's stage select (BrawlStages.h), through a fake of the
// game's memory: the steps follow the game's pads and cursor, every write is idempotent at its
// boundary, the timer counts the screen's own frames, the gate keeps the off-turn port out.

#include <bit>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/UX/BrawlStages.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/StageCursors.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::BrawlStages;
namespace RS = Orca::UX::Ranked;

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
  std::map<u32, u8> bytes;
  int writes = 0;
};

constexpr u32 MANAGER = 0x80900000;
constexpr u32 SCENE = 0x80910000;
constexpr u32 NAME = 0x80920000;
constexpr u32 TASK = 0x80930000;
constexpr u32 ICONS = 0x80940000;
constexpr u32 CURSOR = 0x80948000;
constexpr u32 PADS = 0x80950000;
constexpr u32 GLOBAL = 0x90181300;
constexpr u32 MELEE = 0x90180F20;
constexpr u32 RESULT = 0x9017F420;
constexpr int SLOTS = 40;

constexpr int MARIO = 0;
constexpr int LINK = 2;

// Brawl in a scene; the stage select's task with its icons on `page`; two human players.
FakeMemory Game(std::string_view scene, u32 page = 0)
{
  FakeMemory m;
  m.Fill(0x805A0040, 0xC0);
  m.Write32(0x805A0060, MANAGER);
  m.Write32(0x805A0040, PADS);
  m.Write32(0x805A00BC, 0x12345678);  // the RNG's seed
  m.Write32(0x805A00E0, GLOBAL);
  m.Fill(MANAGER, 0x20);
  m.Write32(MANAGER + 4, SCENE);
  m.Fill(SCENE, 0x400);
  m.Write32(SCENE, NAME);
  m.Write32(SCENE + 0x3AC, TASK);
  m.Text(NAME, scene);
  m.Fill(TASK, 0x300);
  m.Write32(TASK + 0x200, CURSOR);
  m.Write32(TASK + 0x228, page);
  m.Fill(0x806B9298, 0x18);
  m.Write8(0x806B9298 + 4, 31);
  m.Write8(0x806B9298 + 8 + 4, 10);
  m.Write32(TASK + 0x244, 0);
  m.Write32(TASK + 0x248, 0xFFFFFFFF);
  m.Write32(TASK + 0x278, 0xF0);
  m.Fill(CURSOR, 0x50);
  for (u32 i = 0; i < SLOTS; ++i)
  {
    const u32 icon = ICONS + i * 0x100;
    m.Fill(icon, 0x100);
    m.Write32(TASK + 0x8C + 4 * i, icon);
    m.Write32(icon + 0x14, icon + 0x40);
    m.Write32(icon + 0x40 + 0x10, icon + 0x80);
    m.Write32(icon + 0x80 + 0x18, 0x3F800000 + i);  // its picture
  }
  m.Fill(PADS, 0x200);
  m.Fill(GLOBAL, 0x40);
  m.Write32(GLOBAL + 0x08, MELEE);
  m.Write32(GLOBAL + 0x18, RESULT);
  m.Fill(MELEE, 0x98 + 4 * 0x5C);
  for (u32 port = 0; port < 4; ++port)
    m.Write8(MELEE + 0x98 + port * 0x5C + 1, port < 2 ? 0 : 3);
  m.Write8(MELEE + 0x98, MARIO);
  m.Write8(MELEE + 0x98 + 0x5C, LINK);
  m.Fill(RESULT, 0x24 + 4 * 0x2AC + 4);
  m.Fill(kBlock, 0x10);
  m.Write32(kBlock, kHeaderMagic);  // a header with no mode (the tests say which flow runs)
  m.Write8(kBlock + 4, kHeaderVersion);
  m.Write8(kBlock + 7, 1);  // the header's coin: port 2 strikes first
  m.Fill(kRegion, kRegionSize, 0x60);  // the dead code that's there before Orca writes it
  m.Fill(StageCursors::SHARED, StageCursors::SHARED_SIZE);
  m.Fill(StageCursors::CURSORS, StageCursors::CURSORS_SIZE);
  return m;
}

void Scene(FakeMemory& m, std::string_view scene)
{
  m.Text(NAME, scene);
}

void Hover(FakeMemory& m, int stage)
{
  const Tile& t = TileFor(stage);
  m.Write32(TASK + 0x228, t.page);
  m.Write32(TASK + 0x244, t.slot + 2u);  // the hovered item (page 0: the slot + 2)
  m.Write32(TASK + 0x248, t.slot);
}

// One frame of the stage select: its clock ticks, then the boundary runs (twice when `again`, as
// after a rollback's load: the second run must change nothing).
void Boundary(FakeMemory& m, bool again = true)
{
  m.Write32(TASK + 0x250, m.Read32(TASK + 0x250) + 1);
  Frame(m);
  if (again)
  {
    const auto before = m.bytes;
    Frame(m);
    EXPECT_EQ(before, m.bytes) << "a boundary run again changed memory";
  }
}

// The two cursors (StageCursors.h) as the latch leaves them: `port`'s on the middle of `stage`'s
// tile (its page shown), or on the page button.
void Aim(FakeMemory& m, int port, int stage)
{
  auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  const StageCursors::Layout& l = StageCursors::BrawlLayout();
  StageCursors::Rect r = l.page_button;
  if (stage >= 0)
  {
    const StageCursors::Tile* t = StageCursors::TileOf(l, stage, TileFor(stage).page);
    ASSERT_NE(t, nullptr);
    r = t->rect;
    m.Write32(TASK + 0x228, t->page);
  }
  c->cursors[port].x = static_cast<s16>((r.x0 + r.x1) / 2);
  c->cursors[port].y = static_cast<s16>((r.y0 + r.y1) / 2);
  StageCursors::Write(m, *c);
}

// `port`'s buttons in the frame that just ran (the latch's).
void Hold(FakeMemory& m, int port, u8 buttons)
{
  auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  c->cursors[port].now = buttons;
  StageCursors::Write(m, *c);
}

// A press and its release, a boundary each.
void Push(FakeMemory& m, int port, u8 button)
{
  Hold(m, port, button);
  Boundary(m);
  Hold(m, port, 0);
  Boundary(m);
}

constexpr u8 A = StageCursors::BTN_A;
constexpr u8 B = StageCursors::BTN_B;
constexpr u8 X = StageCursors::BTN_X;
constexpr u8 Y = StageCursors::BTN_Y;

float CursorX(const FakeMemory& m)
{
  return std::bit_cast<float>(m.Read32(CURSOR + 0x3C));
}
float CursorY(const FakeMemory& m)
{
  return std::bit_cast<float>(m.Read32(CURSOR + 0x40));
}

u32 IconFrame(const FakeMemory& m, int slot)
{
  return m.Read32(ICONS + static_cast<u32>(slot) * 0x100 + 0x80 + 0x18);
}

class OrcaBrawlStages : public ::testing::Test
{
protected:
  void SetUp() override { SetRankedForTests(true); }
  void TearDown() override { SetRankedForTests(std::nullopt); }
};
}  // namespace

TEST_F(OrcaBrawlStages, NothingWithoutARankedSet)
{
  SetRankedForTests(false);
  FakeMemory m = Game("scSelStage");
  const auto before = m.bytes;
  Boundary(m);
  m.Write32(TASK + 0x250, 0);
  EXPECT_EQ(before, m.bytes);
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  for (const auto& mask : masks)
    EXPECT_TRUE(mask.Empty());
}

TEST_F(OrcaBrawlStages, GameOneStrikesOnTheStageSelect)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  auto s = ReadState(m);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->phase, Phase::Select);
  EXPECT_EQ(s->set.game, 1);
  ASSERT_NE(s->coin, 0xFF);
  EXPECT_EQ(s->coin, 1);  // the header's (the room code's)
  EXPECT_EQ(s->characters[0], MARIO);
  EXPECT_EQ(s->characters[1], LINK);
  const int first = s->coin;
  const int second = 1 - first;
  // Both cursors run, at their start points.
  const auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->cursors[0].x, StageCursors::BrawlLayout().start[0].x);
  EXPECT_EQ(c->cursors[1].x, StageCursors::BrawlLayout().start[1].x);

  // The other port's X does nothing; the striker's X on a starter strikes it.
  Aim(m, first, Battlefield);
  Aim(m, second, Battlefield);
  Push(m, second, X);
  EXPECT_EQ(ReadState(m)->progress.struck, 0);
  Push(m, first, X);
  s = ReadState(m);
  EXPECT_EQ(s->progress.struck, 1 << Battlefield);
  // A held X is one strike, wherever the cursor goes meanwhile.
  Aim(m, second, Smashville);
  Hold(m, second, X);
  Boundary(m);
  Aim(m, second, LylatCruise);
  Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.struck, (1 << Battlefield) | (1 << Smashville));
  Hold(m, second, 0);
  Boundary(m);
  // A counterpick can't be struck in game 1.
  Aim(m, second, FinalDestination);
  Push(m, second, X);
  EXPECT_EQ(ReadState(m)->progress.struck, (1 << Battlefield) | (1 << Smashville));
  Aim(m, second, LylatCruise);
  Push(m, second, X);
  // The first striker picks from the last two (X strikes nothing more).
  Aim(m, first, YoshisIsland);
  Push(m, first, X);
  s = ReadState(m);
  EXPECT_EQ(s->progress.struck, (1 << Battlefield) | (1 << Smashville) | (1 << LylatCruise));
  EXPECT_EQ(s->progress.picked, -1);
  // The struck stages' icons are grey on their page.
  m.Write32(TASK + 0x228, TileFor(Battlefield).page);
  Boundary(m);
  EXPECT_EQ(IconFrame(m, TileFor(Battlefield).slot), 0u);
  // The game takes Pokemon Stadium for the picker.
  m.Write32(TASK + 0x224, 2);
  m.Write32(TASK + 0x258, 0x2E);
  Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.picked, PokemonStadium);
}

TEST_F(OrcaBrawlStages, ThePickersAPicks)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  const int first = ReadState(m)->coin;
  const int second = 1 - first;
  for (const auto& [port, stage] : {std::pair{first, Battlefield}, std::pair{second, Smashville},
                                    std::pair{second, LylatCruise}})
  {
    Aim(m, port, stage);
    Push(m, port, X);
  }
  // The other player's A on Yoshi's Island is a proposal; the picker's A on it picks it.
  Aim(m, second, YoshisIsland);
  Push(m, second, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[second], YoshisIsland);
  EXPECT_EQ(ReadState(m)->progress.picked, -1);
  Aim(m, first, PokemonStadium);
  Push(m, first, A);
  EXPECT_EQ(ReadState(m)->progress.picked, PokemonStadium);
  // The game's cursor goes to the page button first (Pokemon Stadium is on the Melee page), for the
  // picker's port.
  m.Write32(TASK + 0x228, 0);
  Boundary(m);
  EXPECT_EQ(m.Read32(TASK + 0x278), static_cast<u32>(first));
  EXPECT_FLOAT_EQ(CursorX(m), 23.0f);
  EXPECT_FLOAT_EQ(CursorY(m), -18.11f);
}

TEST_F(OrcaBrawlStages, TheTimerCountsTheScreensFrames)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  const u16 start = ReadState(m)->progress.frames;
  for (int i = 0; i < 10; ++i)
    Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.frames, start - 10);
  // No tick without a frame of the screen.
  Frame(m);
  EXPECT_EQ(ReadState(m)->progress.frames, start - 10);
  // Run out: the first starter is struck by default.
  for (int i = 0; i < start; ++i)
    Boundary(m, false);
  EXPECT_EQ(ReadState(m)->progress.struck, 1 << Battlefield);
}

TEST_F(OrcaBrawlStages, TheGameSeesNoPadWhileTheCursorsRunAndTheFlowPressesForIt)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  for (const auto& mask : masks)
    EXPECT_EQ(mask, Rollback::InputGate::ALL);
  // Both propose Smashville: it is picked, the game's cursor goes there, and once the game's hover
  // reads it, A is pressed for the cursor's port on every other frame.
  Aim(m, 0, Smashville);
  Aim(m, 1, Smashville);
  Push(m, 0, A);
  Push(m, 1, A);
  ASSERT_EQ(ReadState(m)->progress.picked, Smashville);
  const StageCursors::Tile* t = StageCursors::TileOf(StageCursors::BrawlLayout(), Smashville, 0);
  EXPECT_FLOAT_EQ(CursorX(m), StageCursors::MidX(t->rect));
  EXPECT_FLOAT_EQ(CursorY(m), StageCursors::MidY(t->rect));
  Hover(m, Smashville);
  int presses = 0;
  for (int i = 0; i < 4; ++i)
  {
    Boundary(m);
    masks = {};
    Masks(m, &masks);
    for (const auto& mask : masks)
      presses += mask.press == PAD_BUTTON_A;
  }
  EXPECT_EQ(presses, 2);
}

TEST_F(OrcaBrawlStages, AgreementSkipsTheStepsButNeverTakesAStruckStage)
{
  FakeMemory m = Game("scSelStage");  // port 2 strikes first
  Boundary(m);
  // Port 1 proposes Battlefield; port 2 strikes it: the proposal is gone, and can't be made again.
  Aim(m, 0, Battlefield);
  Push(m, 0, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], Battlefield);
  Aim(m, 1, Battlefield);
  Push(m, 1, X);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], -1);
  Push(m, 0, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], -1);
  // No proposal on a counterpick in game 1 (not in play), nor on another tile than a stage's.
  Aim(m, 0, FinalDestination);
  Push(m, 0, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], -1);
  // Port 1 proposes Yoshi's Island, then takes it back with B; then Lylat, and port 2 agrees.
  Aim(m, 0, YoshisIsland);
  Push(m, 0, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], YoshisIsland);
  Push(m, 0, B);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], -1);
  Aim(m, 0, LylatCruise);
  Push(m, 0, A);
  Aim(m, 1, LylatCruise);
  Push(m, 1, A);
  auto s = ReadState(m);
  EXPECT_EQ(s->progress.picked, LylatCruise);
  // Strikes owed, the step's: nothing more counts once the stage is agreed.
  Aim(m, 0, Smashville);
  Push(m, 0, X);
  EXPECT_EQ(ReadState(m)->progress.struck, 1 << Battlefield);
  std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  EXPECT_EQ(Lines(m, ports).first, "You 0–0 bo · Agreed: Lylat Cruise");
}

TEST_F(OrcaBrawlStages, LaterGamesAgreementIgnoresDsrNotTheBan)
{
  // Game 3: 1-1, port 2 won game 2; port 1 won game 1 on Smashville (DSR keeps it from port 1).
  FakeMemory m = Game("scSelStage");
  State s;
  s.set.game = 3;
  s.set.last_winner = 1;
  s.set.won_on = {1 << Smashville, 1 << Battlefield};
  s.score = {1, 1};
  s.coin = 0;
  s.phase = Phase::Recorded;
  WriteState(m, s);
  Boundary(m);
  // Port 1 proposes Smashville during port 2's ban; port 2 bans Delfino, then agrees.
  Aim(m, 0, Smashville);
  Push(m, 0, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], Smashville);
  Aim(m, 1, DelfinoPlaza);
  Push(m, 1, X);
  EXPECT_EQ(ReadState(m)->progress.struck, 1 << DelfinoPlaza);
  // Port 1's pick step now: its A on Smashville (DSR) can't pick it, but proposes it still.
  Aim(m, 0, Smashville);
  Push(m, 0, A);
  EXPECT_EQ(ReadState(m)->progress.picked, -1);
  // A banned stage is never agreed.
  Aim(m, 0, DelfinoPlaza);
  Aim(m, 1, DelfinoPlaza);
  Push(m, 1, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[1], -1);
  Aim(m, 1, Smashville);
  Push(m, 1, A);
  EXPECT_EQ(ReadState(m)->progress.picked, Smashville);
}

TEST_F(OrcaBrawlStages, TheWinnerSkipsTheBanWithYAndBothAgree)
{
  // The loser proposes Battlefield; the winner skips the ban with Y and agrees with A.
  FakeMemory m = Game("scSelStage");
  State s;
  s.set.game = 2;
  s.set.last_winner = 1;
  s.set.won_on = {0, 1 << Smashville};
  s.score = {0, 1};
  s.coin = 0;
  s.phase = Phase::Recorded;
  WriteState(m, s);
  Boundary(m);
  Aim(m, 0, Battlefield);
  Push(m, 0, A);
  Push(m, 1, Y);
  EXPECT_EQ(ReadState(m)->progress.step, 1);  // the loser's pick
  Aim(m, 1, Battlefield);
  Push(m, 1, A);
  EXPECT_EQ(ReadState(m)->progress.picked, Battlefield);
  EXPECT_EQ(ReadState(m)->progress.struck, 0);
}

TEST_F(OrcaBrawlStages, APageTurnGoesThroughThePageButton)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  Aim(m, 1, -1);  // port 2 on the page button
  Push(m, 1, A);
  auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  EXPECT_NE(c->page_turn, 0);
  // The game's cursor on the page button for port 2; A pressed once the game's hover has rested.
  EXPECT_FLOAT_EQ(CursorX(m), 23.0f);
  EXPECT_EQ(m.Read32(TASK + 0x278), 1u);
  m.Write32(TASK + 0x244, 0x35);
  bool pressed = false;
  for (int i = 0; i < 4; ++i)
  {
    Boundary(m);
    Rollback::InputGate::Masks masks{};
    Masks(m, &masks);
    pressed = pressed || masks[1].press == PAD_BUTTON_A;
    EXPECT_EQ(masks[0].press, 0);
  }
  EXPECT_TRUE(pressed);
  // The page turned: the game's cursor follows its follower again.
  m.Write32(TASK + 0x228, 1);
  Boundary(m);
  EXPECT_EQ(StageCursors::Read(m)->page_turn, 0);
  // Pokemon Stadium is on this page: port 2's cursor reads it there.
  Aim(m, 1, PokemonStadium);
  Push(m, 1, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[1], PokemonStadium);
}

// The page is both players' view: only the player whose step it is turns it. The repro: the other
// player presses A on the page button whenever the striker's page shows, so the striker never sees
// the stage it means to strike.
TEST_F(OrcaBrawlStages, OnlyThePlayerWhoseStepItIsTurnsThePage)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  // Port 2 strikes first (the coin). Port 1 on the page button: its A turns nothing.
  Aim(m, 0, -1);
  Push(m, 0, A);
  auto c = StageCursors::Read(m);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->page_turn, 0);
  m.Write32(TASK + 0x244, 0x35);  // the game's hover on the page button
  for (int i = 0; i < 4; ++i)
  {
    Boundary(m);
    Rollback::InputGate::Masks masks{};
    Masks(m, &masks);
    EXPECT_EQ(masks[0].press & PAD_BUTTON_A, 0);
    EXPECT_EQ(masks[1].press & PAD_BUTTON_A, 0);
  }
  EXPECT_EQ(StageCursors::Read(m)->page_turn, 0);
  // Port 2's own A on it does.
  Aim(m, 1, -1);
  Push(m, 1, A);
  EXPECT_EQ(StageCursors::Read(m)->page_turn & 0x81, 0x81);
}

TEST_F(OrcaBrawlStages, ResultsMakeTheSetAndLaterGamesBanThenPick)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  Scene(m, "scMelee");
  Boundary(m);
  EXPECT_EQ(ReadState(m)->phase, Phase::Fight);
  EXPECT_FALSE(StageCursors::Read(m));  // off the stage select, the cursors stop
  // Port 2 wins on Smashville.
  Scene(m, "scVsResult");
  m.Write16(RESULT + 0x0C, 0x21);
  m.Write16(RESULT + 0x0E, 1);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 rec = RESULT + 0x24 + port * 0x2AC;
    m.Write8(rec + 1, port < 2 ? 0 : 3);
    m.Write8(rec + 0x0E, port == 1 ? 0 : 1);
  }
  Boundary(m);
  Boundary(m);
  auto s = ReadState(m);
  EXPECT_EQ(s->set.game, 2);
  EXPECT_EQ(s->score[0], 0);
  EXPECT_EQ(s->score[1], 1);
  EXPECT_EQ(s->set.last_winner, 1);
  EXPECT_EQ(s->set.won_on[1], 1 << Smashville);
  // Game 2: port 2 may ban, then port 1 picks.
  Scene(m, "scSelctCharacter");
  Boundary(m);
  Scene(m, "scSelStage");
  Boundary(m);
  s = ReadState(m);
  EXPECT_EQ(s->phase, Phase::Select);
  std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  EXPECT_EQ(CursorView(m, ports).turn, 1);
  {
    // The overlay's turn cue: bo may ban 1 (optional), 30 s; ada waits.
    const StageCursors::View v = CursorView(m, ports);
    EXPECT_EQ(v.kind, static_cast<u8>(RS::StepKind::Ban));
    EXPECT_EQ(v.left, 1);
    EXPECT_TRUE(v.optional);
    EXPECT_FALSE(v.both);
    EXPECT_FALSE(v.mk_clause);
    EXPECT_EQ(v.seconds, 30);
    EXPECT_EQ(StageCursors::TurnText(v, 1).title, "BO MAY BAN 1");
    EXPECT_EQ(StageCursors::TurnText(v, 0).title, "WAITING FOR BO");
  }
  Push(m, 0, Y);  // not port 1's to skip
  EXPECT_EQ(CursorView(m, ports).turn, 1);
  Push(m, 1, Y);  // no ban
  EXPECT_EQ(CursorView(m, ports).turn, 0);
  // Port 1 picks Delfino.
  Aim(m, 0, DelfinoPlaza);
  Push(m, 0, A);
  EXPECT_EQ(ReadState(m)->progress.picked, DelfinoPlaza);
}

TEST_F(OrcaBrawlStages, AFriendsGameClearsTheSet)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  ASSERT_TRUE(ReadState(m));
  ASSERT_TRUE(StageCursors::Read(m));
  SetRankedForTests(false);
  Boundary(m);
  EXPECT_FALSE(ReadState(m));
  EXPECT_FALSE(StageCursors::Read(m));
}

TEST_F(OrcaBrawlStages, ATakenStageTheStepsDontAllowIsReplaced)
{
  // Game 2: port 2 won on Smashville and skips its ban; port 1 picks.
  FakeMemory m = Game("scSelStage");
  State s;
  s.set.game = 2;
  s.set.last_winner = 1;
  s.set.won_on = {1 << FinalDestination, 1 << Smashville};
  s.score = {0, 1};
  s.coin = 0;
  s.phase = Phase::Recorded;
  WriteState(m, s);
  Boundary(m);
  Push(m, 1, Y);
  RS::SetView view = ReadState(m)->set;
  view.characters = {0, 2};
  const RS::Plan plan = RS::PlanGame(RS::Brawl2025(), view);
  ASSERT_NE(RS::Current(plan, ReadState(m)->progress), nullptr);
  ASSERT_EQ(RS::Current(plan, ReadState(m)->progress)->kind, RS::StepKind::Pick);
  // The game's cursor rests on Delfino, then A lands on Final Destination (port 1 won there: DSR).
  Hover(m, DelfinoPlaza);
  Boundary(m);
  Boundary(m);
  m.Write32(TASK + 0x224, 2);
  m.Write32(TASK + 0x258, 0x02);
  Boundary(m);
  EXPECT_EQ(m.Read32(TASK + 0x258), 0x03u);  // Delfino Plaza
  EXPECT_EQ(ReadState(m)->progress.picked, DelfinoPlaza);
  // Everybody's pads are off until the screen leaves.
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  EXPECT_EQ(masks[0], Rollback::InputGate::ALL);
  EXPECT_EQ(masks[1], Rollback::InputGate::ALL);
}

TEST_F(OrcaBrawlStages, AStageTakenDuringStrikesTakesTheDefaults)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  m.Write32(TASK + 0x224, 2);
  m.Write32(TASK + 0x258, 0x01);  // Battlefield
  Boundary(m);
  // Strikes by default: Battlefield, then Pokemon Stadium and Lylat, then the pick's default, the
  // first of the last two (Smashville).
  EXPECT_EQ(ReadState(m)->progress.picked, Smashville);
  EXPECT_EQ(m.Read32(TASK + 0x258), 0x21u);
}

TEST_F(OrcaBrawlStages, TheMatchBlocksHeaderTurnsTheFlowOn)
{
  SetRankedForTests(std::nullopt);
  FakeMemory m = Game("scSelStage");
  m.Fill(kBlock, 0x300, 0x60);
  Boundary(m);
  EXPECT_FALSE(ReadState(m));  // no header: friends or solo
  m.Write32(kBlock, kHeaderMagic);
  m.Write8(kBlock + 4, kHeaderVersion);
  m.Write8(kBlock + 5, kModeRanked);
  m.Write8(kBlock + 6, 2);  // Project+'s ruleset
  Boundary(m);
  EXPECT_FALSE(ReadState(m));
  m.Write8(kBlock + 5, kModeCasual);
  m.Write8(kBlock + 6, kRulesetBrawl);
  // Casual: its own stage pick, once both players are in.
  Frame(m, false);
  EXPECT_FALSE(ReadState(m));
  Boundary(m);
  ASSERT_TRUE(ReadState(m));
  EXPECT_NE(ReadState(m)->seed, 0u);
  m.Write8(kBlock + 5, kModeRanked);
  m.Write32(TASK + 0x250, 0);  // a new stage select
  Boundary(m);
  ASSERT_TRUE(ReadState(m));
  EXPECT_EQ(ReadState(m)->phase, Phase::Select);
  EXPECT_EQ(ReadState(m)->seed, 0u);
  // The header goes (the opponent left a casual game, the host is alone again): so does the flow.
  m.Write32(kBlock, 0);
  Boundary(m);
  EXPECT_FALSE(ReadState(m));
}

TEST_F(OrcaBrawlStages, TheResultsTracksSetDrivesTheSteps)
{
  FakeMemory m = Game("scSelStage");
  m.Fill(kBlock, 0x300, 0x60);
  m.Write32(kBlock, kHeaderMagic);
  m.Write8(kBlock + 4, kHeaderVersion);
  m.Write8(kBlock + 5, kModeRanked);
  m.Write8(kBlock + 6, kRulesetBrawl);
  // One game recorded: port 2 won on Smashville.
  m.Fill(kResultsSet, 0x80);
  m.Write8(kResultsSet + 0, 1);
  m.Write8(kResultsSet + 2, 1);
  m.Write8(kResultsSet + 4, 0xFF);
  m.Write8(kResultsSet + 5, 1);
  m.Write8(kResultsSet + 0x20 + 4, 1);
  m.Write8(kResultsSet + 0x20 + 6, 0x21);
  Boundary(m);
  std::array<u8, 2> score{};
  const RS::SetView set = SetOf(m, *ReadState(m), &score);
  EXPECT_EQ(set.game, 2);
  EXPECT_EQ(set.last_winner, 1);
  EXPECT_EQ(set.won_on[1], 1 << Smashville);
  EXPECT_EQ(score[1], 1);
  // Port 2 won: its ban comes first.
  std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  EXPECT_EQ(CursorView(m, ports).turn, 1);
  // A block whose set bytes aren't a set (the results track isn't there): this flow's own record.
  m.Write8(kResultsSet + 0, 0x60);
  EXPECT_EQ(SetOf(m, *ReadState(m)).game, 1);
}

TEST_F(OrcaBrawlStages, ANewStageSelectStartsTheStepsAgain)
{
  FakeMemory m = Game("scSelStage");
  for (int i = 0; i < 50; ++i)
    Boundary(m);
  const int first = ReadState(m)->coin;
  Aim(m, first, Battlefield);
  Push(m, first, X);
  ASSERT_NE(ReadState(m)->progress.struck, 0);
  Aim(m, 1 - first, Smashville);
  Push(m, 1 - first, A);
  // The set ended mid-select and another began on a new stage select: its frame counter is back
  // at the start.
  m.Write32(TASK + 0x250, 0);
  Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.struck, 0);
  EXPECT_EQ(ReadState(m)->progress.step, 0);
  EXPECT_EQ(ReadState(m)->progress.prefs[1 - first], -1);
  EXPECT_EQ(StageCursors::Read(m)->cursors[1 - first].x,
            StageCursors::BrawlLayout().start[1 - first].x);
}

TEST_F(OrcaBrawlStages, TheGamesCursorFollowsWhoeverMovedLast)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  std::array<std::optional<GCPadStatus>, 4> raw{};
  GCPadStatus still{};
  still.isConnected = true;
  still.stickX = still.stickY = 128;
  GCPadStatus right = still;
  right.stickX = 228;  // full tilt
  raw[0] = still;
  raw[1] = right;
  EXPECT_GT(StageCursors::Latch(m, raw), 0);
  auto c = StageCursors::Read(m);
  EXPECT_EQ(c->follower, 1);
  const StageCursors::Layout& l = StageCursors::BrawlLayout();
  EXPECT_EQ(c->cursors[1].x, l.start[1].x + l.speed);
  EXPECT_EQ(c->cursors[0].x, StageCursors::BrawlLayout().start[0].x);
  Boundary(m);
  EXPECT_EQ(m.Read32(TASK + 0x278), 1u);
  EXPECT_FLOAT_EQ(CursorX(m), static_cast<float>(c->cursors[1].x) / 16.0f);
  EXPECT_FLOAT_EQ(CursorY(m), static_cast<float>(c->cursors[1].y) / 16.0f);
}

TEST_F(OrcaBrawlStages, TheOverlaysTopLineHasTheSetTheTurnTheCountAndTheTime)
{
  FakeMemory m = Game("scSelStage");  // the header's coin: port 2 strikes first
  std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  Scene(m, "scSelctCharacter");
  Boundary(m);
  EXPECT_EQ(Lines(m, ports), (std::pair<std::string, std::string>{"You 0–0 bo", ""}));
  EXPECT_EQ(CurrentTurn(m), std::nullopt);
  Scene(m, "scSelStage");
  Boundary(m);
  EXPECT_EQ(Lines(m, ports), (std::pair<std::string, std::string>{
                                 "You 0–0 bo · bo strikes 1 · 0:30",
                                 "A proposes a stage"}));
  // The turn as STAGE SELECT's relabel names it (Relabel.h): the same step as the line's.
  EXPECT_EQ(CurrentTurn(m), (Ranked::Turn{Ranked::StepKind::Strike, 1, 1}));
  Aim(m, 1, Battlefield);
  Push(m, 1, X);
  // Port 1's two strikes, its keys under the line.
  EXPECT_EQ(Lines(m, ports), (std::pair<std::string, std::string>{
                                 "You 0–0 bo · You strike 2 · 0:30",
                                 "X on a stage strikes it · A proposes a stage"}));
  EXPECT_EQ(CurrentTurn(m), (Ranked::Turn{Ranked::StepKind::Strike, 0, 2}));
  Aim(m, 0, Smashville);
  Push(m, 0, X);
  EXPECT_EQ(Lines(m, ports).first, "You 0–0 bo · You strike 1 · 0:30");
  EXPECT_EQ(CurrentTurn(m), (Ranked::Turn{Ranked::StepKind::Strike, 0, 1}));
  // What the other player proposes.
  Aim(m, 1, YoshisIsland);
  Push(m, 1, A);
  EXPECT_EQ(Lines(m, ports).second,
            "X on a stage strikes it · bo wants Yoshi's Island · A on it to agree");
  // The overlay's view: both cursors, bo's proposal, whose turn.
  const StageCursors::View v = CursorView(m, ports);
  EXPECT_TRUE(v.on);
  EXPECT_EQ(v.turn, 0);
  EXPECT_TRUE(v.players[0].local);
  EXPECT_FALSE(v.players[1].local);
  EXPECT_EQ(v.players[1].name, "bo");
  EXPECT_EQ(v.players[1].proposal, YoshisIsland);
  EXPECT_EQ(v.players[0].proposal, -1);
  // The turn's step for the overlay's cues: ada strikes, one strike left, 0:30.
  EXPECT_EQ(v.kind, static_cast<u8>(RS::StepKind::Strike));
  EXPECT_EQ(v.left, 1);
  EXPECT_FALSE(v.optional);
  EXPECT_FALSE(v.both);
  EXPECT_EQ(v.seconds, 30);
  EXPECT_EQ(StageCursors::TurnText(v, 0),
            (StageCursors::TurnCue{StageCursors::TurnCue::Mode::Acting, true, "YOUR TURN",
                                   "X STRIKES A STAGE \u00b7 1 LEFT", 30}));
  EXPECT_EQ(StageCursors::TurnText(v, 1).mode, StageCursors::TurnCue::Mode::Idle);
}

// Each cursor makes it obvious whose turn it is, from both machines' sides: the acting player's
// tag, the waiting local player's tab, nothing for the rest.
TEST(OrcaStageCursors, TurnText)
{
  using Mode = StageCursors::TurnCue::Mode;
  StageCursors::View v;
  v.on = true;
  v.game = StageCursors::GAME_BRAWL;
  v.players[0].present = v.players[1].present = true;
  v.players[0].name = "Ada";
  v.players[1].name = "Bo";
  v.players[1].local = true;  // Bo's machine
  v.turn = 0;
  v.kind = static_cast<u8>(RS::StepKind::Strike);
  v.left = 2;
  v.seconds = 24;
  // Nothing before a step or after the pick, nor off the stage select.
  EXPECT_EQ(StageCursors::TurnText(StageCursors::View{}, 0), StageCursors::TurnCue{});
  {
    StageCursors::View none = v;
    none.kind = 0;
    EXPECT_EQ(StageCursors::TurnText(none, 0), StageCursors::TurnCue{});
    none = v;
    none.picked = 4;
    EXPECT_EQ(StageCursors::TurnText(none, 0), StageCursors::TurnCue{});
    EXPECT_EQ(StageCursors::TurnText(none, 1), StageCursors::TurnCue{});
    EXPECT_EQ(StageCursors::TurnText(v, 2), StageCursors::TurnCue{});
  }
  // A strike, on the other player's screen: their name, the count, the timer; this player waits.
  EXPECT_EQ(StageCursors::TurnText(v, 0),
            (StageCursors::TurnCue{Mode::Acting, false, "ADA'S TURN", "STRIKING 2", 24}));
  EXPECT_EQ(StageCursors::TurnText(v, 1),
            (StageCursors::TurnCue{Mode::Waiting, true, "WAITING FOR ADA", "", -1}));
  // The same strike on the striker's own screen; the other cursor says nothing.
  v.players[0].local = true;
  v.players[1].local = false;
  EXPECT_EQ(StageCursors::TurnText(v, 0),
            (StageCursors::TurnCue{Mode::Acting, true, "YOUR TURN",
                                   "X STRIKES A STAGE \u00b7 2 LEFT", 24}));
  EXPECT_EQ(StageCursors::TurnText(v, 1).mode, Mode::Idle);
  // No name yet: the tab's P1.
  {
    StageCursors::View unnamed = v;
    unnamed.players[0].name.clear();
    unnamed.players[0].local = false;
    EXPECT_EQ(StageCursors::TurnText(unnamed, 0).title, "P1'S TURN");
  }
  // Brawl's optional ban: X bans, Y skips.
  v.kind = static_cast<u8>(RS::StepKind::Ban);
  v.left = 1;
  v.optional = true;
  EXPECT_EQ(StageCursors::TurnText(v, 0).title, "YOUR TURN");
  EXPECT_EQ(StageCursors::TurnText(v, 0).detail, "X BANS 1 \u00b7 Y SKIPS");
  v.players[0].local = false;
  v.players[1].local = true;
  EXPECT_EQ(StageCursors::TurnText(v, 0).title, "ADA MAY BAN 1");
  EXPECT_EQ(StageCursors::TurnText(v, 0).detail, "");
  // A pick.
  v.optional = false;
  v.kind = static_cast<u8>(RS::StepKind::Pick);
  v.turn = 1;
  EXPECT_EQ(StageCursors::TurnText(v, 1),
            (StageCursors::TurnCue{Mode::Acting, true, "YOUR PICK", "A PICKS THE STAGE", 24}));
  EXPECT_EQ(StageCursors::TurnText(v, 0).mode, Mode::Idle);
  v.players[0].local = true;
  v.players[1].local = false;
  EXPECT_EQ(StageCursors::TurnText(v, 1).title, "BO PICKS");
  EXPECT_EQ(StageCursors::TurnText(v, 0).title, "WAITING FOR BO");
  // The Meta Knight clause: Ada plays Meta Knight, so Bo picks any stage.
  v.mk_clause = true;
  EXPECT_EQ(StageCursors::TurnText(v, 1),
            (StageCursors::TurnCue{Mode::Acting, false, "BO PICKS ANY STAGE",
                                   "META KNIGHT CLAUSE", 24}));
  EXPECT_EQ(StageCursors::MkClauseNote(v), "BO PICKS ANY STAGE \u00b7 NO STRIKES");
  v.players[0].local = false;
  v.players[1].local = true;
  EXPECT_EQ(StageCursors::TurnText(v, 1),
            (StageCursors::TurnCue{Mode::Acting, true, "YOUR PICK",
                                   "ANY STAGE \u00b7 META KNIGHT CLAUSE", 24}));
  EXPECT_EQ(StageCursors::MkClauseNote(v), "YOU PICK ANY STAGE \u00b7 NO STRIKES");
  {
    StageCursors::View picked = v;
    picked.picked = 4;
    EXPECT_EQ(StageCursors::MkClauseNote(picked), "");
    picked = v;
    picked.mk_clause = false;
    EXPECT_EQ(StageCursors::MkClauseNote(picked), "");
  }
}

// A casual pick: both at once; whoever hasn't named a stage acts, each in their own colour, and a
// local player who has waits for the other.
TEST(OrcaStageCursors, TurnTextBothAtOnce)
{
  using Mode = StageCursors::TurnCue::Mode;
  StageCursors::View v;
  v.on = true;
  v.players[0].present = v.players[1].present = true;
  v.players[0].name = "ada";
  v.players[1].name = "bo";
  v.players[0].local = true;
  v.turn = -1;
  v.both = true;
  v.kind = static_cast<u8>(RS::StepKind::Prefer);
  v.left = 1;
  v.seconds = 15;
  EXPECT_EQ(StageCursors::TurnText(v, 0),
            (StageCursors::TurnCue{Mode::Acting, true, "YOUR PICK", "A PICKS A STAGE", 15}));
  EXPECT_EQ(StageCursors::TurnText(v, 1),
            (StageCursors::TurnCue{Mode::Acting, false, "BO PICKS", "", 15}));
  v.players[0].proposal = 2;
  EXPECT_EQ(StageCursors::TurnText(v, 0),
            (StageCursors::TurnCue{Mode::Waiting, true, "WAITING FOR BO", "", -1}));
  EXPECT_EQ(StageCursors::TurnText(v, 1).mode, Mode::Acting);
  v.players[1].proposal = 3;
  EXPECT_EQ(StageCursors::TurnText(v, 0).mode, Mode::Idle);
  EXPECT_EQ(StageCursors::TurnText(v, 1).mode, Mode::Idle);
  EXPECT_EQ(StageCursors::MkClauseNote(v), "");
}

namespace
{
class OrcaBrawlCasual : public ::testing::Test
{
protected:
  void SetUp() override
  {
    SetRankedForTests(false);
    SetCasualForTests(true);
  }
  void TearDown() override
  {
    SetRankedForTests(std::nullopt);
    SetCasualForTests(std::nullopt);
  }
};

constexpr u32 ROOM = 0x5EED1234;

RS::Plan CasualPlanOf(const State& s)
{
  RS::SetView v;
  v.coin = s.coin;
  v.characters = {s.characters[0], s.characters[1]};
  return RS::PlanCasual(RS::Brawl2025(), v, s.seed);
}
}  // namespace

TEST_F(OrcaBrawlCasual, BothPickAtOnceTheSameStagePlays)
{
  FakeMemory m = Game("scSelStage");
  m.Write32(kBlock + 8, ROOM);
  Boundary(m);
  auto s = ReadState(m);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->phase, Phase::Select);
  EXPECT_EQ(s->seed, RS::CasualSeed(ROOM, 1, 1));
  // One step for both players: 15 s and a half-second grace.
  EXPECT_EQ(s->progress.frames, 15 * 60 + 30);
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  for (const auto& mask : masks)
    EXPECT_EQ(mask, Rollback::InputGate::ALL);
  std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  EXPECT_EQ(Lines(m, ports),
            (std::pair<std::string, std::string>{
                "Stage pick · 0:15",
                "A on the stage you want · the same pick plays there, else a coin takes one of the "
                "two"}));
  EXPECT_EQ(CurrentTurn(m), (Ranked::Turn{Ranked::StepKind::Prefer, Ranked::BOTH, 1}));
  // Port 2 names Smashville, then changes its mind to Battlefield; port 1 names Battlefield too.
  Aim(m, 1, Smashville);
  Push(m, 1, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[1], Smashville);
  EXPECT_EQ(Lines(m, ports).second,
            "A on the stage you want · the same pick plays there, else a coin takes one of the "
            "two · bo wants Smashville · A on it to agree");
  Aim(m, 1, Battlefield);
  Push(m, 1, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[1], Battlefield);
  EXPECT_EQ(ReadState(m)->progress.picked, -1);
  Aim(m, 0, Battlefield);
  Push(m, 0, A);
  s = ReadState(m);
  EXPECT_EQ(s->progress.picked, Battlefield);
  EXPECT_EQ(Lines(m, ports).first, "Battlefield (both picks)");
  // The flow takes it: the game's cursor to the stage, A pressed on it every other frame.
  Hover(m, Battlefield);
  bool pressed = false;
  for (int i = 0; i < 3; ++i)
  {
    Boundary(m);
    masks = {};
    Masks(m, &masks);
    for (const auto& mask : masks)
      pressed = pressed || mask.press == PAD_BUTTON_A;
  }
  EXPECT_TRUE(pressed);
  // The fight counts a game: the next stage select's coin is another.
  Scene(m, "scMelee");
  Boundary(m);
  EXPECT_EQ(ReadState(m)->set.game, 2);
  Scene(m, "scSelStage");
  m.Write32(TASK + 0x224, 0);
  m.Write32(TASK + 0x250, 0);
  Boundary(m);
  s = ReadState(m);
  EXPECT_EQ(s->seed, RS::CasualSeed(ROOM, 1, 2));
  EXPECT_EQ(s->progress.prefs[0], -1);
  EXPECT_EQ(s->progress.prefs[1], -1);
}

// A casual stage pick is a step both take at once: either player turns the page.
TEST_F(OrcaBrawlCasual, EitherPlayerTurnsThePage)
{
  for (const int port : {0, 1})
  {
    FakeMemory m = Game("scSelStage");
    m.Write32(kBlock + 8, ROOM);
    Boundary(m);
    Aim(m, port, -1);
    Push(m, port, A);
    const auto c = StageCursors::Read(m);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->page_turn & 0x81, 0x80 | port);
  }
}

TEST_F(OrcaBrawlCasual, TwoStagesTheCoinTakesOneAndTimeRunsOut)
{
  FakeMemory m = Game("scSelStage");
  m.Write32(kBlock + 8, ROOM);
  Boundary(m);
  Aim(m, 0, Smashville);
  Push(m, 0, A);
  Aim(m, 1, LylatCruise);
  Push(m, 1, A);
  auto s = ReadState(m);
  const RS::Plan plan = CasualPlanOf(*s);
  ASSERT_GE(s->progress.picked, 0);
  EXPECT_EQ(s->progress.picked, RS::CasualChoice(plan) == 0 ? Smashville : LylatCruise);
  // Another game: port 1 names Smashville and takes it back; nobody names anything in time.
  m.Write32(TASK + 0x250, 0);
  Scene(m, "scMelee");
  Boundary(m);
  Scene(m, "scSelStage");
  Boundary(m);
  Aim(m, 0, Smashville);
  Push(m, 0, A);
  Push(m, 0, B);
  EXPECT_EQ(ReadState(m)->progress.prefs[0], -1);
  for (int i = 0; i < 15 * 60 + 30 && ReadState(m)->progress.picked < 0; ++i)
    Boundary(m, false);
  s = ReadState(m);
  const RS::Plan later = CasualPlanOf(*s);
  EXPECT_EQ(s->progress.picked, RS::CasualResult(later, {-1, -1}));
}

TEST_F(OrcaBrawlCasual, NoFinalDestinationForAnIceClimbersAndNothingAlone)
{
  FakeMemory m = Game("scSelStage");
  m.Write8(MELEE + 0x98, 0x10);  // port 1 plays Ice Climbers
  Boundary(m);
  const auto s = ReadState(m);
  ASSERT_TRUE(s);
  EXPECT_FALSE(RS::Allowed(CasualPlanOf(*s), s->progress) & (1 << FinalDestination));
  Aim(m, 1, FinalDestination);
  Push(m, 1, A);
  EXPECT_EQ(ReadState(m)->progress.prefs[1], -1);
  // FD's tile is grey.
  m.Write32(TASK + 0x228, 0);
  Boundary(m);
  EXPECT_EQ(IconFrame(m, TileFor(FinalDestination).slot), 0u);
  EXPECT_NE(IconFrame(m, TileFor(Battlefield).slot), 0u);
  // The opponent gone: the stage select is the player's own again, its cursor anyone's.
  Frame(m, false);
  EXPECT_FALSE(ReadState(m));
  EXPECT_FALSE(StageCursors::Read(m));
  EXPECT_EQ(m.Read32(TASK + 0x278), 0xF0u);
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  for (const auto& mask : masks)
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaStageCursors, TheLatchMovesEachCursorWithItsOwnStick)
{
  FakeMemory m = Game("scSelStage");
  std::array<std::optional<GCPadStatus>, 4> raw{};
  GCPadStatus pad{};
  pad.isConnected = true;
  pad.stickX = pad.stickY = 128;
  // Off (no tag): nothing moves.
  raw[0] = pad;
  EXPECT_EQ(StageCursors::Latch(m, raw), 0);
  const StageCursors::Layout& l = StageCursors::BrawlLayout();
  StageCursors::Write(m, StageCursors::Start(l));
  // Inside the dead zone: nothing; past it, a speed in proportion; full tilt (and past it) the
  // layout's speed; up is +y; the buttons are the frame's.
  pad.stickX = static_cast<u8>(128 + l.dead_zone - 1);
  pad.button = PAD_BUTTON_A | PAD_BUTTON_X;
  raw[0] = pad;
  StageCursors::Latch(m, raw);
  auto c = StageCursors::Read(m);
  EXPECT_EQ(c->cursors[0].x, l.start[0].x);
  EXPECT_EQ(c->cursors[0].now, StageCursors::BTN_A | StageCursors::BTN_X);
  pad.stickX = 128 + 50;
  pad.stickY = 255;
  pad.button = 0;
  raw[0] = pad;
  StageCursors::Latch(m, raw);
  c = StageCursors::Read(m);
  EXPECT_EQ(c->cursors[0].x, l.start[0].x + 50 * l.speed / 100);
  EXPECT_EQ(c->cursors[0].y, l.start[0].y + l.speed);
  EXPECT_EQ(c->cursors[0].now, 0);
  EXPECT_EQ(c->follower, 0);
  // Port 2's own stick moves port 2's cursor only; it never goes past the bounds.
  pad.stickX = 0;
  pad.stickY = 128;
  raw[0].reset();
  raw[1] = pad;
  for (int i = 0; i < 200; ++i)
    StageCursors::Latch(m, raw);
  c = StageCursors::Read(m);
  EXPECT_EQ(c->cursors[1].x, l.bounds.x0);
  EXPECT_EQ(c->cursors[0].x, l.start[0].x + 50 * l.speed / 100);
  EXPECT_EQ(c->follower, 1);
  // Off the stage select, nothing moves.
  Scene(m, "scMelee");
  EXPECT_EQ(StageCursors::Latch(m, raw), 0);
}

TEST(OrcaStageCursors, EdgesAreSeenOnceAndTilesAreTheLayouts)
{
  StageCursors::State s;
  s.cursors[0].now = StageCursors::BTN_A;
  EXPECT_EQ(StageCursors::TakePresses(&s, 0), StageCursors::BTN_A);
  EXPECT_EQ(StageCursors::TakePresses(&s, 0), 0);
  s.cursors[0].now = StageCursors::BTN_A | StageCursors::BTN_B;
  EXPECT_EQ(StageCursors::TakePresses(&s, 0), StageCursors::BTN_B);
  // Every legal stage has a tile in each game, no two overlap, and each tile's middle reads it.
  for (const StageCursors::Layout* l : {&StageCursors::BrawlLayout(), &StageCursors::PPlusLayout()})
  {
    for (const StageCursors::Tile& t : l->tiles)
    {
      EXPECT_EQ(StageCursors::Hovered(*l, t.page, (t.rect.x0 + t.rect.x1) / 2,
                                      (t.rect.y0 + t.rect.y1) / 2),
                t.stage)
          << "game " << int(l->game) << " stage " << int(t.stage);
      for (const StageCursors::Tile& u : l->tiles)
      {
        if (&t == &u || t.page != u.page)
          continue;
        const bool apart = t.rect.x1 <= u.rect.x0 || u.rect.x1 <= t.rect.x0 ||
                           t.rect.y1 <= u.rect.y0 || u.rect.y1 <= t.rect.y0;
        EXPECT_TRUE(apart) << "game " << int(l->game) << " stages " << int(t.stage) << " and "
                           << int(u.stage);
      }
    }
  }
  EXPECT_EQ(StageCursors::BrawlLayout().tiles.size(), 7u);
  EXPECT_EQ(StageCursors::PPlusLayout().tiles.size(), 9u);
}
