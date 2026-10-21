// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The online rules for queue matches (Core/Orca/Games/Brawl/OnlineRules.h): the match block's
// header and what it locks, over a fake of the game's memory.

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Rollback/OnlineMatch.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::Rules;
namespace MB = Orca::UX::MatchBlock;

namespace
{
// The ranked character select's ready limit: the Brawl ruleset's (60 s, as both builtins).
const int CSS_READY_FRAMES = Rulesets::Brawl2025().css_ready_seconds * 60;
int NextFrame()
{
  static int s_frame = 0;
  return ++s_frame;
}

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
  bool track_writes = false;
  std::vector<u32> recorded;
  std::map<u32, u8> bytes;
  int writes = 0;
};

constexpr u32 SET_RULE = 0x9017F360;
constexpr u32 MELEE = 0x90180F20;
constexpr u32 MENU_DATA = 0x9017BE50;
constexpr u32 SCENE = 0x80910000;
constexpr u32 SCENE_NAME = 0x80920000;
constexpr u32 PPLUS_RSS = 0x8042C4E8;
constexpr u32 PPLUS_STRIKES = 0x8042C822;

void SetScene(FakeMemory& m, std::string_view scene)
{
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
}

// A port's pick in the match's init data, as the character select writes it: type 0 a human's
// pick, 1 a CPU's, 3 nobody's; the character (gmCharacterKind; 0x3E none).
void SetPick(FakeMemory& m, int port, u8 type, u8 character)
{
  const u32 p = MELEE + 0x98 + static_cast<u32>(port) * 0x5C;
  m.Write8(p + 0, character);
  m.Write8(p + 1, type);
}
constexpr u8 HUMAN = 0, CPU = 1, NOBODY = 3, NONE = 0x3E;

// The character select's task, its player areas and their hands (muSelCharHand), at addresses of
// the fake's own: the scene's +0x400 is the task, the task's +0x44 + 4 x port an area, the area's
// +0x1A8 its hand (OnlineRules.h, "The character select's hands").
constexpr u32 CSS_TASK = 0x80930000;
constexpr u32 CSS_AREA = 0x80940000;  // + port x 0x1000
constexpr u32 CSS_HAND = 0x80950000;  // + port x 0x100

void SetHand(FakeMemory& m, int port, u32 target, float y, u32 button = 0, float x = 0)
{
  const u32 hand = CSS_HAND + static_cast<u32>(port) * 0x100;
  m.Write32(hand + 0x80, target);
  m.Write32(hand + 0x90, std::bit_cast<u32>(x));
  m.Write32(hand + 0x94, std::bit_cast<u32>(y));
  m.Write32(hand + 0xAC, button);
}

void BuildHands(FakeMemory& m)
{
  m.Write32(SCENE + 0x400, CSS_TASK);
  m.Fill(CSS_TASK, 0x60);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 area = CSS_AREA + port * 0x1000;
    const u32 hand = CSS_HAND + port * 0x100;
    m.Write32(CSS_TASK + 0x44 + 4 * port, area);
    m.Fill(area, 0x1B0);
    m.Write32(area + 0x1A8, hand);
    m.Fill(hand, 0xE0);
    // In the grid with the token down, mid-screen.
    SetHand(m, static_cast<int>(port), CSS_HAND_GRID, 5.0f);
  }
}

// Brawl's memory as the probes found it: the scene manager and its scene, g_GameGlobal with the
// rules and the match's init data, the record's menu data with the fresh save's defaults, and the
// match block holding the game's dead code.
FakeMemory Game(std::string_view scene)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x10);
  m.Fill(SCENE, 0x410);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, SCENE);
  m.Write32(SCENE, SCENE_NAME);
  SetScene(m, scene);
  m.Fill(0x805A00E0, 4);
  m.Fill(0x90181300, 0x40);
  m.Write32(0x805A00E0, 0x90181300);
  m.Write32(0x90181308, MELEE);
  m.Write32(0x9018131C, SET_RULE);
  m.Fill(MELEE, 0x98 + 4 * 0x5C);
  for (int port = 0; port < 4; ++port)
    SetPick(m, port, NOBODY, NONE);
  m.Fill(SET_RULE, 0x88);
  // The fresh save's rules: a 2-minute timed match, 3 stocks, ratio 1.0, Choose, pause on.
  m.Write8(SET_RULE + 0x02, 0x00);
  m.Write8(SET_RULE + 0x03, 2);
  m.Write8(SET_RULE + 0x04, 3);
  m.Write8(SET_RULE + 0x06, 10);
  m.Write8(SET_RULE + 0x0A, 1);
  m.Write8(SET_RULE + 0x0C, 1);
  m.Fill(MENU_DATA, 0x30);
  m.Write32(MENU_DATA + 0x00, 0x02000000);  // Medium
  m.Write32(MENU_DATA + 0x04, 0x4F524341);  // 'ORCA'
  m.Write32(MENU_DATA + 0x08, 0xFFFFFFFF);
  m.Write32(MENU_DATA + 0x0C, 0xFFFFFFFF);
  m.Write32(MENU_DATA + 0x20, 0x0003FF00);  // every Melee stage
  m.Write32(MENU_DATA + 0x24, 0x7FFFFFFF);  // every Brawl stage
  for (u32 i = 0; i < MB::FULL_SIZE; i += 4)
    m.Write32(MB::BASE + i, 0x3A200000 + i);  // the game's dead code
  m.Fill(PPLUS_RSS, 0x320, 0x11);
  m.Fill(PPLUS_STRIKES, 0x1E, 0x01);
  BuildHands(m);
  m.writes = 0;
  return m;
}

FakeMemory Locked(Mode mode, Ruleset ruleset, std::string_view scene = "scSelctCharacter")
{
  FakeMemory m = Game(scene);
  WriteHeader(m, mode, ruleset);
  m.writes = 0;
  return m;
}
}  // namespace

// A joiner's keyframe carries the host's set as its game holds it: a first join (no games of the
// room's match known) takes only a set not begun, and a rejoin no more games than it played or saw
// decided. A history that already holds games the joiner never played (a set decided after one real
// game) is refused, as a header that isn't the room's is.
TEST(OrcaOnlineRules, AJoinTakesNoSetGamesItDidntSee)
{
  const SetBlock::SetState fresh;
  EXPECT_TRUE(SetFitsJoin(fresh, 0));
  EXPECT_TRUE(SetFitsJoin(fresh, -1));
  // One game won by the host's port, planted in its history before the join.
  SetBlock::SetState planted = fresh;
  planted.games = 1;
  planted.wins = {1, 0};
  planted.last_winner = 0;
  planted.records[0].start = 4210;
  planted.records[0].winner = 0;
  planted.records[0].how = SetBlock::How::Results;
  EXPECT_FALSE(SetFitsJoin(planted, 0));
  // A set already decided, or only a record or a tiebreak streak written.
  SetBlock::SetState decided = planted;
  decided.games = 2;
  decided.wins = {2, 0};
  decided.done = 1;
  decided.winner = 0;
  EXPECT_FALSE(SetFitsJoin(decided, 0));
  EXPECT_FALSE(SetFitsJoin(decided, 1));
  SetBlock::SetState record = fresh;
  record.records[3].winner = 1;
  EXPECT_FALSE(SetFitsJoin(record, 0));
  SetBlock::SetState streak = fresh;
  streak.tb_streak = 2;
  EXPECT_FALSE(SetFitsJoin(streak, 0));
  // A player back after a dropped link, who played game 1: that game is its own.
  EXPECT_TRUE(SetFitsJoin(planted, 1));
  EXPECT_TRUE(SetFitsJoin(decided, 2));
  // Not checked (a friends or casual room).
  EXPECT_TRUE(SetFitsJoin(decided, -1));
}

TEST(OrcaOnlineRules, NoHeaderInTheGamesDeadCode)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_FALSE(ReadHeader(m).present);
  EXPECT_FALSE(ReadHeader(m).Locked());
  // Clearing what isn't there touches nothing.
  EXPECT_EQ(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
  EXPECT_EQ(m.Read32(MB::BASE), 0x3A200000u);
}

TEST(OrcaOnlineRules, TheHeaderIsWrittenOnceAndSavesThePlayersRules)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  const Header h = ReadHeader(m);
  EXPECT_TRUE(h.Locked());
  EXPECT_EQ(h.mode, Mode::Casual);
  EXPECT_EQ(h.ruleset, Ruleset::Brawl);
  EXPECT_EQ(m.Read32(MB::MAGIC), MB::MAGIC_VALUE);
  // The dead code is gone from the rest of the block: zeros but the saved rules and a fresh set
  // (the stage flow's "none" ports).
  EXPECT_EQ(m.Read32(MB::SET + 0x40), 0u);
  EXPECT_EQ(m.Read8(MB::SET + 0x03), MB::NONE);
  EXPECT_EQ(m.Read8(MB::SAVED_RULES + 1), 2);  // gmSetRule +0x03
  EXPECT_EQ(m.Read32(MB::SAVED_ITEMS), 0x02000000u);
  EXPECT_EQ(m.Read32(MB::SAVED_STAGES + 4), 0x7FFFFFFFu);
  // Twice is once.
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineRules, ClearingGivesBackThePlayersRules)
{
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Ranked, Ruleset::Brawl);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 0);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0u);
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_FALSE(ReadHeader(m).present);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02), 0x00);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 1);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x00), 0x02000000u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0xFFFFFFFFu);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
  // The block is the dead code's first word, then zeros: nothing locks, clearing again writes
  // nothing, and a new header may go there.
  EXPECT_EQ(m.Read32(MB::BASE), 0x3A200000u);
  for (u32 i = 4; i < MB::FULL_SIZE; ++i)
    ASSERT_EQ(m.Read8(MB::BASE + i), 0) << i;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  EXPECT_TRUE(ReadHeader(m).Locked());
}

TEST(OrcaOnlineRules, NoHeaderBeforeTheGameLoadsTheModule)
{
  // Before sora_scene is loaded the block's memory is something else's (zeros at boot).
  FakeMemory m = Game("scSelctCharacter");
  for (u32 i = 0; i < MB::FULL_SIZE; ++i)
    m.Write8(MB::BASE + i, 0);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 7), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineRules, AnotherQueueRoomStartsANewSetAndKeepsTheSavedRules)
{
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  ApplyLocks(m, NextFrame());
  m.Write8(MB::SET, 0x55);
  m.Write16(MB::CSS_TIMER, 77);
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl), 0);
  EXPECT_EQ(ReadHeader(m).mode, Mode::Ranked);
  EXPECT_EQ(m.Read8(MB::SET), 0);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // The player's own, not the casual lock's.
  EXPECT_EQ(m.Read32(MB::SAVED_ITEMS), 0x02000000u);
  EXPECT_EQ(m.Read8(MB::SAVED_RULES + 8), 1);  // gmSetRule +0x0A, pause on
}

TEST(OrcaOnlineRules, BrawlsCasualRules)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::Brawl);
  EXPECT_GT(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02) & 7, 1);  // stock
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(SET_RULE + 0x05), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x06), 10);
  EXPECT_EQ(m.Read8(SET_RULE + 0x07), 0);  // Choose: the casual stage pick's turns
  EXPECT_EQ(m.Read8(SET_RULE + 0x08), 8);
  EXPECT_EQ(m.Read8(SET_RULE + 0x09), 1);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x03), 2);  // a timed match's minutes: not the lock's
  EXPECT_EQ(m.Read8(SET_RULE + 0x0C), 1);  // the damage gauge: not the lock's
  EXPECT_EQ(m.Read8(MENU_DATA), 0);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x04), 0x4F524341u);  // the patches' marker stays
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x0C), 0u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x20), BRAWL_LEGAL_MELEE_PAGE);
  // Idempotent: a re-run at the same boundary writes nothing.
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  // A player's change on the rules screen goes back at the next boundary.
  m.Write8(SET_RULE + 0x04, 5);
  m.Write8(MENU_DATA, 3);
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 2);
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(MENU_DATA), 0);
}

TEST(OrcaOnlineRules, RankedChoosesTheStageAndProjectPlusPlaysFourStocks)
{
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::Brawl);
  ApplyLocks(ranked, NextFrame());
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x07), 0);  // Choose
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x04), 3);
  FakeMemory pplus = Locked(Mode::Casual, Ruleset::PPlus);
  ApplyLocks(pplus, NextFrame());
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x04), 4);
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x07), 0);  // casual shows the stage select too
  // Project+'s stage list is its own: Brawl's Random Stage Switch is left alone.
  EXPECT_EQ(pplus.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
}

TEST(OrcaOnlineRules, IceClimbersTakeFinalDestinationOutOfTheRandomStages)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::Brawl);
  SetPick(m, 0, HUMAN, 0x00);  // Mario
  SetPick(m, 1, HUMAN, 0x02);  // Link
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24) & BRAWL_FINAL_DESTINATION_BIT, BRAWL_FINAL_DESTINATION_BIT);
  SetPick(m, 1, HUMAN, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  // A CPU's Ice Climbers don't count (and a queue match has none).
  SetPick(m, 1, CPU, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE);
  // On the way to the fight (a Random character resolved there).
  SetScene(m, "scSelStage");
  SetPick(m, 1, HUMAN, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  // In the fight the list stays as it was.
  SetScene(m, "scMelee");
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
}

TEST(OrcaOnlineRules, ProjectPlusTakesThe2024ProposedListOnTheCharacterSelect)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::PPlus);
  // The player's own preset was saved with the header.
  EXPECT_EQ(m.Read8(MB::SAVED_RSS), 0x11);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(PPLUS_RSS), 0x00000003u);  // page 0's random mask: the 9 stages
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0x3C), 9);      // page 0 holds 9 stages
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0xA8), 0x11);   // past the list: untouched
  EXPECT_EQ(m.Read8(PPLUS_STRIKES), 0);         // casual: no strikes left over
  // Ranked keeps the strike table to its stage select.
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::PPlus);
  ApplyLocks(ranked, NextFrame());
  EXPECT_EQ(ranked.Read8(PPLUS_STRIKES), 0x01);
  // Cleared: the player's preset comes back.
  WriteHeader(m, Mode::None, Ruleset::PPlus);
  EXPECT_EQ(m.Read8(PPLUS_RSS), 0x11);
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0x3C), 0x11);
}

TEST(OrcaOnlineRules, TheGateLocksTheMenusOnlyUnderAHeader)
{
  FakeMemory m = Game("scSelctCharacter");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
  SetScene(m, "scSelStage");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
  SetScene(m, "scMelee");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_START);
  SetScene(m, "scVsResult");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  FakeMemory p = Locked(Mode::Ranked, Ruleset::PPlus);
  for (const auto& mask : GateMasks(p))
  {
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B | PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT |
                                PAD_BUTTON_RIGHT);
    EXPECT_FALSE(mask.main_stick);
  }
  SetScene(p, "scSelStage");
  for (const auto& mask : GateMasks(p))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B | PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z);
}

// Ranked's results screen keeps B from the game: after the set, holding it leaves the room
// (OnlineMatch.cpp SetOverLeave), and the hold must never move the other player's game. Casual's
// results screen keeps its B.
TEST(OrcaOnlineRules, RankedResultsNeverTakeB)
{
  for (Ruleset ruleset : {Ruleset::Brawl, Ruleset::PPlus})
  {
    FakeMemory ranked = Locked(Mode::Ranked, ruleset, "scVsResult");
    for (const auto& mask : GateMasks(ranked))
    {
      EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
      EXPECT_EQ(mask.press, 0);
    }
    FakeMemory casual = Locked(Mode::Casual, ruleset, "scVsResult");
    for (const auto& mask : GateMasks(casual))
      EXPECT_EQ(mask.buttons & PAD_BUTTON_B, 0);
    // Ranked's fight is unchanged: only Start.
    SetScene(ranked, "scMelee");
    for (const auto& mask : GateMasks(ranked))
      EXPECT_EQ(mask.buttons, PAD_BUTTON_START);
  }
}

// The results screen's Z (its replay save, a dead end in a session) is masked in every session's
// game, with a header or without (a friends game, solo play), and nowhere else.
TEST(OrcaOnlineRules, TheResultsScreenNeverTakesZ)
{
  FakeMemory m = Game("scVsResult");
  for (const auto& mask : SessionMasks(m))
  {
    EXPECT_EQ(mask.buttons, PAD_TRIGGER_Z);
    EXPECT_EQ(mask.press, 0);
    EXPECT_FALSE(mask.main_stick);
    EXPECT_FALSE(mask.c_stick);
  }
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  for (const char* scene : {"scSelctCharacter", "scSelStage", "scMelee", "scMemoryChange",
                            "muMenuMain", ""})
  {
    SetScene(m, scene);
    for (const auto& mask : SessionMasks(m))
      EXPECT_TRUE(mask.Empty()) << scene;
  }
  for (Mode mode : {Mode::Casual, Mode::Ranked})
  {
    for (Ruleset ruleset : {Ruleset::Brawl, Ruleset::PPlus})
    {
      FakeMemory locked = Locked(mode, ruleset, "scVsResult");
      for (const auto& mask : SessionMasks(locked))
        EXPECT_EQ(mask.buttons, PAD_TRIGGER_Z);
      SetScene(locked, "scMelee");
      for (const auto& mask : SessionMasks(locked))
        EXPECT_TRUE(mask.Empty());
    }
  }
  // Reads only.
  EXPECT_EQ(m.writes, 0);
  // No scene manager yet (the game still loading): nothing.
  FakeMemory empty;
  for (const auto& mask : SessionMasks(empty))
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaOnlineRules, TheCharacterSelectPassesAOnlyInTheGrid)
{
  constexpr u16 B = PAD_BUTTON_B, A = PAD_BUTTON_A;
  FakeMemory m = Game("scSelctCharacter");
  // The later games' trap: the hands come back on the panels' player-type buttons.
  for (int port = 0; port < 4; ++port)
    SetHand(m, port, CSS_HAND_BUTTON, -19.8f, CSS_BUTTON_PLAYER_TYPE, -22.5f + 15.0f * port);
  // A friends game: nothing masked, the buttons work.
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Ranked, Ruleset::Brawl);
  // Own and others' player-type buttons (ports 3-4's would add a CPU): no A.
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, B | A);
  // Every other button and the empty space: no A.
  const struct
  {
    u32 target, button;
  } blocked[] = {
      {CSS_HAND_NOTHING, 0}, {CSS_HAND_BUTTON, 0x1C},  // name
      {CSS_HAND_BUTTON, 0x1B},                          // the panel's picture (a costume)
      {CSS_HAND_BUTTON, 0x03},                          // the BRAWL tab (teams)
      {CSS_HAND_BUTTON, 0x05}, {CSS_HAND_BUTTON, 0x06}, {CSS_HAND_BUTTON, 0x07},  // rules bar
      {CSS_HAND_EXIT, 0x02}, {CSS_HAND_EXIT, 0x04},                                // BACK, Rules
      {5, 0x08},                                                                   // unknown
  };
  for (const auto& b : blocked)
  {
    SetHand(m, 0, b.target, 5.0f, b.button);
    EXPECT_EQ(GateMasks(m)[0].buttons, B | A) << b.target << " " << b.button;
  }
  // The grid and the player's own token: A passes.
  for (const u32 target : {CSS_HAND_GRID, CSS_HAND_OWN_TOKEN, CSS_HAND_TAKING,
                           CSS_HAND_GRID_HOLDING, CSS_HAND_PLACING})
  {
    SetHand(m, 0, target, 5.0f, target == CSS_HAND_OWN_TOKEN ? 1 : 0);
    EXPECT_EQ(GateMasks(m)[0].buttons, B) << target;
    EXPECT_EQ(GateMasks(m)[1].buttons, B | A);  // port 2 still on its button
  }
  // Within one frame's travel of the grid's top or bottom edge: no A (a press arriving on the
  // rules bar or the panel would act there).
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, 14.9f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, CSS_A_TOP);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, 15.9f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, -2.9f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  // Brawl's bottom row reaches -4.07 (RANDOM at its right end), so A passes all the way down it.
  // A -3.0 edge would swallow A over the bottom quarter of every tile in that row.
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, -3.4f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  SetHand(m, 0, CSS_HAND_GRID_HOLDING, -4.07f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B);
  SetHand(m, 0, CSS_HAND_GRID, -4.3f);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, CSS_A_BOTTOM_BRAWL);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  SetHand(m, 0, CSS_HAND_GRID, std::nanf(""));
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  // A hand that can't be read: no A.
  SetHand(m, 0, CSS_HAND_GRID, 5.0f);
  m.Write32(CSS_TASK + 0x44, 0);
  EXPECT_FALSE(ReadCssHand(m, 0).valid);
  EXPECT_EQ(GateMasks(m)[0].buttons, B | A);
  // Off the character select the hands mean nothing.
  SetScene(m, "scSelStage");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, B);
  // Casual and Project+ alike (Project+ also loses the D-pad there).
  FakeMemory p = Locked(Mode::Casual, Ruleset::PPlus);
  SetHand(p, 1, CSS_HAND_BUTTON, -19.8f, CSS_BUTTON_PLAYER_TYPE);
  constexpr u16 DPAD = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD);
  EXPECT_EQ(GateMasks(p)[1].buttons, B | DPAD | A);
  // Project+'s bottom edge stays -3.0 (its rows end above -1.5; RANDOM is the bottom row's middle,
  // about -1.5..2.5).
  SetHand(p, 0, CSS_HAND_GRID_HOLDING, 0.5f);
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD);
  SetHand(p, 0, CSS_HAND_GRID, -2.9f);
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD);
  SetHand(p, 0, CSS_HAND_GRID, -3.4f);
  EXPECT_EQ(GateMasks(p)[0].buttons, B | DPAD | A);
}

// Wherever B backs out, A on the BACK button must too. Held B backs out of the queue's own
// character select (FLAG_SOLO), so A on BACK works there: port 1's new press, with the stick
// centred while A is down. Queue rooms (casual and ranked) keep it masked, Rules never takes A,
// and friends games mask nothing.
TEST(OrcaOnlineRules, AOnBackBacksOutOfTheQueuesOwnCharacterSelectOnly)
{
  constexpr u16 A = PAD_BUTTON_A;
  FakeMemory m = Game("scSelctCharacter");
  m.Write16(Queue::RAW, 0);
  m.Write16(Queue::RAW + 2, 0);
  for (int port = 0; port < 4; ++port)
    SetHand(m, port, CSS_HAND_EXIT, 19.5f, CSS_BUTTON_BACK, -28.0f);
  // Friends (no header): the game's own BACK, nothing masked.
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  // The queue's own character select: port 1's A reaches the game, the stick centred while it is
  // down; nobody else's (ports 2-4 don't play there; Queue.h masks them entirely anyway).
  WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  Rollback::InputGate::Masks masks = GateMasks(m);
  EXPECT_FALSE(masks[0].buttons & A);
  EXPECT_TRUE(masks[0].a_centres_stick);
  for (int port = 1; port < 4; ++port)
  {
    EXPECT_TRUE(masks[port].buttons & A) << port;
    EXPECT_FALSE(masks[port].a_centres_stick) << port;
  }
  // Only a new press: an A held on the frame that just ran (sliding onto BACK) stays masked.
  m.Write16(Queue::RAW, A);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  EXPECT_FALSE(GateMasks(m)[0].a_centres_stick);
  m.Write16(Queue::RAW, PAD_BUTTON_B);  // another button held: still a new A
  EXPECT_FALSE(GateMasks(m)[0].buttons & A);
  m.Write16(Queue::RAW, 0);
  // Ranked's own character select too; Project+'s alike.
  WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_FALSE(GateMasks(m)[0].buttons & A);
  FakeMemory p = Game("scSelctCharacter");
  p.Write16(Queue::RAW, 0);
  p.Write16(Queue::RAW + 2, 0);
  SetHand(p, 0, CSS_HAND_EXIT, 19.5f, CSS_BUTTON_BACK, -28.0f);
  WriteHeader(p, Mode::Casual, Ruleset::PPlus, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_FALSE(GateMasks(p)[0].buttons & A);
  EXPECT_TRUE(GateMasks(p)[0].a_centres_stick);
  // Rules (the other leaving button) never takes A; nor does anything else that isn't the grid.
  WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  SetHand(m, 0, CSS_HAND_EXIT, 19.5f, 0x04, 20.0f);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  SetHand(m, 0, CSS_HAND_BUTTON, 19.5f, 0x03, -14.0f);  // the BRAWL tab
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  SetHand(m, 0, CSS_HAND_NOTHING, 19.5f, 0, -22.0f);  // between BACK and the tab
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  // A queue room's character select, casual or ranked: BACK keeps A masked (no way back there:
  // casual finds someone else with Z, ranked is played).
  SetHand(m, 0, CSS_HAND_EXIT, 19.5f, CSS_BUTTON_BACK, -28.0f);
  for (const Mode mode : {Mode::Casual, Mode::Ranked})
  {
    for (const u8 flags : {u8{0}, u8{MB::FLAG_QUEUE2}})
    {
      WriteHeader(m, mode, Ruleset::Brawl, 0, 0, flags);
      EXPECT_TRUE(GateMasks(m)[0].buttons & A) << static_cast<int>(mode) << " " << int{flags};
      EXPECT_FALSE(GateMasks(m)[0].a_centres_stick);
    }
  }
  // The pure rule.
  CssHand back;
  back.valid = true;
  back.target = CSS_HAND_EXIT;
  back.button = CSS_BUTTON_BACK;
  EXPECT_TRUE(CssBackTakesA(back, false));
  EXPECT_FALSE(CssBackTakesA(back, true));
  back.button = 0x04;
  EXPECT_FALSE(CssBackTakesA(back, false));
  back.button = CSS_BUTTON_BACK;
  back.valid = false;
  EXPECT_FALSE(CssBackTakesA(back, false));
  EXPECT_TRUE(CssHandOnBack(CSS_HAND_EXIT, CSS_BUTTON_BACK));
  EXPECT_FALSE(CssHandOnBack(CSS_HAND_BUTTON, CSS_BUTTON_BACK));
}

TEST(OrcaOnlineRules, TheQueuesOwnSelectLetsThePlayerPickTheirTagWhileNotSearching)
{
  constexpr u16 A = PAD_BUTTON_A;
  FakeMemory m = Game("scSelctCharacter");
  m.Write16(Queue::RAW, 0);
  m.Write16(Queue::RAW + 2, 0);
  // Port 1's hand on its own name button (panel 0).
  SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_NAME, -19.0f);
  m.Write32(CSS_HAND + 0xB0, 0);
  for (const Mode mode : {Mode::Casual, Mode::Ranked})
  {
    WriteHeader(m, mode, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
    Rollback::InputGate::Masks masks = GateMasks(m);
    EXPECT_FALSE(masks[0].buttons & A);
    EXPECT_TRUE(masks[0].a_centres_stick);
    // The list open: its picks take A, L stays from the game.
    SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, 0, -19.0f);
    m.Write32(CSS_AREA + 0x200, 2);
    masks = GateMasks(m);
    EXPECT_FALSE(masks[0].buttons & A);
    EXPECT_TRUE(masks[0].buttons & PAD_TRIGGER_L);
    EXPECT_TRUE(masks[0].a_centres_stick);
    EXPECT_TRUE(ReadCssNameList(m, 0));
    EXPECT_FALSE(ReadCssNameList(m, 1));
    m.Write32(CSS_AREA + 0x200, 0);
    SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_NAME, -19.0f);
  }
  // While searching: no A there (a room's select couldn't close a list).
  Queue::State ready;
  ready.ready = 1;
  Queue::WriteState(m, ready);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  Queue::WriteState(m, Queue::State{});
  EXPECT_FALSE(GateMasks(m)[0].buttons & A);
  // The player-type button beside it never takes A.
  SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_PLAYER_TYPE, -24.0f);
  EXPECT_TRUE(GateMasks(m)[0].buttons & A);
  // A queue room's select: A stays in the grid, own name button and list or not.
  SetHand(m, 0, CSS_HAND_BUTTON, -21.5f, CSS_BUTTON_NAME, -19.0f);
  for (const Mode mode : {Mode::Casual, Mode::Ranked})
  {
    WriteHeader(m, mode, Ruleset::Brawl, 0, 0, MB::FLAG_QUEUE2);
    EXPECT_TRUE(GateMasks(m)[0].buttons & A);
    m.Write32(CSS_AREA + 0x200, 1);
    EXPECT_TRUE(GateMasks(m)[0].buttons & A);
    m.Write32(CSS_AREA + 0x200, 0);
  }
}

TEST(OrcaOnlineRules, BReachesTheCharacterSelectOnlyToTakeTheTokenUp)
{
  constexpr u16 B = PAD_BUTTON_B;
  FakeMemory m = Game("scSelctCharacter");
  // Each area's player: a human, the token down on a character (Brawl's Mario, 0x00).
  const auto token = [&m](int port, bool in_hand, bool flying, u32 character = 0x00) {
    const u32 area = CSS_AREA + static_cast<u32>(port) * 0x1000;
    m.Fill(area + 0x1B0, 0x50);
    m.Write32(area + 0x1B4, 1);
    m.Write32(area + 0x1B8, character);
    m.Write8(area + 0x1F8, in_hand ? 1 : 0);
    m.Write8(area + 0x1F9, flying ? 1 : 0);
  };
  for (int port = 0; port < 4; ++port)
    token(port, false, false);
  // A friends game: nothing masked.
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  // The latch's raw buttons, ports 1 and 2: nothing held on the frame that just ran.
  m.Write16(Queue::RAW, 0);
  m.Write16(Queue::RAW + 2, 0);
  EXPECT_TRUE(ReadCssToken(m, 0).Down());
  EXPECT_FALSE(GateMasks(m)[0].buttons & B);
  EXPECT_FALSE(GateMasks(m)[1].buttons & B);
  // Ports 3 and 4 never play a queue match: no B.
  EXPECT_TRUE(GateMasks(m)[2].buttons & B);
  EXPECT_TRUE(GateMasks(m)[3].buttons & B);
  // B held on the frame that just ran: the game had its one frame of it.
  m.Write16(Queue::RAW, B);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  EXPECT_FALSE(GateMasks(m)[1].buttons & B);
  m.Write16(Queue::RAW, PAD_BUTTON_A);
  EXPECT_FALSE(GateMasks(m)[0].buttons & B);
  // The token in the hand (held B there backs out to the menus), on its way up or down, or on no
  // character: no B.
  token(0, true, false);
  EXPECT_FALSE(ReadCssToken(m, 0).Down());
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  token(0, false, true);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  token(0, true, true);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  token(0, false, false, 0x28);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  // A CPU's token (the area isn't a human's): no B.
  token(0, false, false);
  m.Write32(CSS_AREA + 0x1B4, 0);
  EXPECT_TRUE(GateMasks(m)[0].buttons & B);
  m.Write32(CSS_AREA + 0x1B4, 1);
  EXPECT_FALSE(GateMasks(m)[0].buttons & B);
  // The pure rule.
  CssToken down;
  down.valid = down.human = true;
  down.character = 0x05;
  down.in_hand = false;
  EXPECT_TRUE(CssBUnpicks(down, false));
  EXPECT_FALSE(CssBUnpicks(down, true));
  EXPECT_FALSE(CssBUnpicks(CssToken{}, false));
  // The queue's own character select before a match: B is the game's (it backs out), the rules
  // don't take it.
  FakeMemory solo = Game("scSelctCharacter");
  WriteHeader(solo, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  for (const auto& mask : GateMasks(solo))
    EXPECT_FALSE(mask.buttons & B);
  // The stage select: never B.
  SetScene(m, "scSelStage");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.buttons & B);
}

TEST(OrcaOnlineRules, TheHandsAreReadPerPort)
{
  FakeMemory m = Game("scSelctCharacter");
  SetHand(m, 2, CSS_HAND_BUTTON, -19.8f, CSS_BUTTON_PLAYER_TYPE, 7.5f);
  const CssHand hand = ReadCssHand(m, 2);
  EXPECT_TRUE(hand.valid);
  EXPECT_EQ(hand.target, CSS_HAND_BUTTON);
  EXPECT_EQ(hand.button, CSS_BUTTON_PLAYER_TYPE);
  EXPECT_EQ(hand.x, 7.5f);
  EXPECT_EQ(hand.y, -19.8f);
  EXPECT_EQ(ReadCssHand(m, 1).target, CSS_HAND_GRID);
  EXPECT_FALSE(ReadCssHand(m, 4).valid);
  EXPECT_FALSE(ReadCssHand(m, -1).valid);
  SetScene(m, "scMelee");
  EXPECT_FALSE(ReadCssHand(m, 2).valid);
}

TEST(OrcaOnlineRules, FriendsGamesAreUntouched)
{
  FakeMemory m = Game("scSelctCharacter");
  SetPick(m, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  // A header with mode none (never written so, but a reader must not lock on it).
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, 0);
  m.Write8(MB::RULESET, 1);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  // Another version's block is no block.
  m.Write8(MB::MODE, 1);
  m.Write8(MB::VERSION, 9);
  EXPECT_FALSE(ReadHeader(m).present);
}

TEST(OrcaOnlineRules, RankedsReadyTimerFindsTheNoShow)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  m.Write8(MB::PLUGGED, 3);
  SetPick(m, 0, HUMAN, 0x00);  // ready: Mario
  for (int f = 0; f < CSS_READY_FRAMES; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), CSS_READY_FRAMES - 1);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);  // port 2
  // Picking now changes nothing: the no-show stands while the character select is up.
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);
  // Leaving the character select clears it.
  SetScene(m, "scMemoryChange");
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, TheReadyTimerWaitsForBothPlayersAndStopsWhenBothPick)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  SetPick(m, 0, HUMAN, 0x00);
  // The opponent isn't plugged in yet: no timer.
  m.Write8(MB::PLUGGED, 1);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  m.Write8(MB::PLUGGED, 3);
  for (int f = 0; f < 101; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 100);
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // Nobody picked yet: no timer either.
  SetPick(m, 0, NOBODY, NONE);
  SetPick(m, 1, NOBODY, NONE);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // After a fight the set's later games keep their characters: no no-show.
  SetScene(m, "scMelee");
  ApplyLocks(m, NextFrame());
  SetScene(m, "scSelctCharacter");
  SetPick(m, 0, HUMAN, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  // Casual has no timer.
  FakeMemory c = Locked(Mode::Casual, Ruleset::Brawl);
  c.Write8(MB::PLUGGED, 3);
  SetPick(c, 0, HUMAN, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(c, NextFrame());
  EXPECT_EQ(c.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(c.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, TheReadyTimerIsTheSameWhenABoundaryRunsAgain)
{
  // A boundary run again after a load (the hook at the same frame over memory it already wrote)
  // leaves the timer where the first run did.
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  m.Write8(MB::PLUGGED, 3);
  SetPick(m, 0, HUMAN, 0x00);
  for (int f = 1000; f < 1100; ++f)
    ApplyLocks(m, f);
  const auto once = m.bytes;
  EXPECT_EQ(ApplyLocks(m, 1099), 0);
  EXPECT_EQ(m.bytes, once);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 99);
}

TEST(OrcaOnlineRules, TheHeaderCarriesTheCoinAndANewRoomStartsANewSet)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 0x1234), 0);
  EXPECT_EQ(m.Read8(MB::COIN), 1);
  EXPECT_EQ(m.Read32(MB::ROOM), 0x1234u);
  // Every track's set starts fresh: the stage flow's and the ranked set's "none" ports.
  const auto state = Orca::UX::MatchBlock::Read(m);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->coin, 1);
  EXPECT_EQ(state->last_winner, Orca::UX::MatchBlock::NONE);
  EXPECT_EQ(Orca::UX::SetBlock::ReadSet(m).last_winner, Orca::UX::SetBlock::NO_PORT);
  // The same room again: nothing to write.
  EXPECT_EQ(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 0x1234), 0);
  // A set in progress, then another room with the same mode: a new set.
  Orca::UX::SetBlock::SetState set;
  set.games = 1;
  set.wins = {1, 0};
  set.last_winner = 0;
  Orca::UX::SetBlock::WriteSet(m, set);
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 0, 0x5678), 0);
  EXPECT_EQ(Orca::UX::SetBlock::ReadSet(m), Orca::UX::SetBlock::SetState{});
  EXPECT_EQ(m.Read8(MB::COIN), 0);
}

TEST(OrcaOnlineRules, TheTestQueueKnobTakesACoin)
{
  using Orca::UX::Rules::ParseTestQueue;
  using Orca::UX::Rules::TestQueue;
  EXPECT_EQ(ParseTestQueue("ranked"), (TestQueue{Mode::Ranked, 0}));
  EXPECT_EQ(ParseTestQueue("ranked:1"), (TestQueue{Mode::Ranked, 1}));
  EXPECT_EQ(ParseTestQueue("casual:0"), (TestQueue{Mode::Casual, 0}));
  EXPECT_FALSE(ParseTestQueue("ranked:2").has_value());
  // The queue's character select (Queue.h): q2 in a room, solo its own.
  EXPECT_EQ(ParseTestQueue("casual:q2"), (TestQueue{Mode::Casual, 0, MB::FLAG_QUEUE2}));
  EXPECT_EQ(ParseTestQueue("ranked:1:q2"), (TestQueue{Mode::Ranked, 1, MB::FLAG_QUEUE2}));
  EXPECT_EQ(ParseTestQueue("casual:solo"),
            (TestQueue{Mode::Casual, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2}));
  EXPECT_FALSE(ParseTestQueue("casual:q2:1").has_value());
  EXPECT_FALSE(ParseTestQueue("casual:q2:solo").has_value());
  EXPECT_FALSE(ParseTestQueue("casual:0:1").has_value());
  EXPECT_FALSE(ParseTestQueue("brawl").has_value());
  EXPECT_FALSE(ParseTestQueue("").has_value());
}

TEST(OrcaOnlineRules, LocksArePureFunctionsOfMemory)
{
  // The same memory twice (a first run and its re-run from the snapshot) writes the same bytes.
  FakeMemory a = Locked(Mode::Casual, Ruleset::Brawl);
  FakeMemory b = Locked(Mode::Casual, Ruleset::Brawl);
  SetPick(a, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  SetPick(b, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  EXPECT_EQ(ApplyLocks(a, 500), ApplyLocks(b, 500));
  EXPECT_EQ(a.bytes, b.bytes);
}

// The queue's flags (Queue.h): part of the header, so another flag is a new header (a fresh set,
// the queue's region cleared); the queue's own character select lets B back out.
TEST(OrcaOnlineRules, TheQueuesFlagsAreTheHeaders)
{
  constexpr u16 B = PAD_BUTTON_B;
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2),
            0);
  Header h = ReadHeader(m);
  EXPECT_TRUE(h.Locked());
  EXPECT_TRUE(h.Solo());
  EXPECT_TRUE(h.Queue2());
  EXPECT_EQ(m.Read8(MB::FLAGS), MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  // B passes there (it backs out of the search); A still only in the grid.
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons & B, 0);
  EXPECT_EQ(WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2),
            0);
  // The queue's own region, written meanwhile, then the room's header: fresh.
  m.Write8(MB::QUEUE, 1);
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl, 1, 0x77, MB::FLAG_QUEUE2), 0);
  h = ReadHeader(m);
  EXPECT_FALSE(h.Solo());
  EXPECT_TRUE(h.Queue2());
  EXPECT_EQ(m.Read8(MB::QUEUE), 0);
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons & B, B);
  // The player's own rules were saved once, from before the first header, and come back.
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 1);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0xFFFFFFFFu);
}

// The header a game carries for its room (HeaderFor): a queue room's coin and hash come from its
// code; the queue's own character select is no room's.
TEST(OrcaOnlineRules, TheRoomsHeaderComesFromItsCode)
{
  const Header room = HeaderFor(Mode::Casual, Ruleset::PPlus, "rfkyxp4s", MB::FLAG_QUEUE2);
  EXPECT_TRUE(room.Locked());
  EXPECT_FALSE(room.Solo());
  EXPECT_TRUE(room.Queue2());
  EXPECT_EQ(room.coin, MB::CoinFromCode("rfkyxp4s"));
  EXPECT_EQ(room.room, MB::RoomHash("rfkyxp4s"));
  const Header own = HeaderFor(Mode::Casual, Ruleset::PPlus, "rfkyxp4s",
                               MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_TRUE(own.Solo());
  EXPECT_EQ(own.coin, 0);
  EXPECT_EQ(own.room, 0u);
  EXPECT_FALSE(HeaderFor(Mode::None, Ruleset::PPlus, "rfkyxp4s", MB::FLAG_QUEUE2).present);
  EXPECT_FALSE(HeaderFor(Mode::Casual, Ruleset::None, "rfkyxp4s", 0).present);
  // What the host writes is what it reads back, every field.
  for (const Header& h : {room, own})
  {
    FakeMemory m = Game("scSelctCharacter");
    EXPECT_GT(WriteHeader(m, h.mode, Ruleset::PPlus, h.coin, h.room, h.flags), 0);
    EXPECT_EQ(ReadHeader(m), h);
    EXPECT_TRUE(HeaderIs(ReadHeader(m), h));
  }
}

// The joiner's check (OnFrame, ExpectHeader): the header of the host's own queue character select
// (before it switched to the room's) has the room's mode and ruleset, but is still refused, as is
// any other room's header.
TEST(OrcaOnlineRules, AJoinerRefusesAHeaderThatIsNotItsRooms)
{
  const std::string code = "rfkyxp4s";
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Casual, Ruleset::Brawl, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  Header h = ReadHeader(m);
  ASSERT_TRUE(h.Locked());
  EXPECT_EQ(h.mode, Mode::Casual);
  EXPECT_EQ(h.ruleset, Ruleset::Brawl);
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  // Even a joiner that asked for FLAG_SOLO: a room's header never has it.
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code,
                              MB::FLAG_SOLO | MB::FLAG_QUEUE2));

  // The room's own header: accepted.
  const Header room = HeaderFor(Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2);
  WriteHeader(m, room.mode, Ruleset::Brawl, room.coin, room.room, room.flags);
  h = ReadHeader(m);
  EXPECT_TRUE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  // Not another queue's, ruleset's or room's (the last room's header, never rewritten).
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Ranked, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::PPlus, code, MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, "zzzzzzzz", MB::FLAG_QUEUE2));
  // A joiner without `queue2` in a queue2 room, and the other way round: the two games' character
  // selects would differ in what the pages drive.
  EXPECT_FALSE(HeaderFitsRoom(h, Mode::Casual, Ruleset::Brawl, code, 0));
  WriteHeader(m, room.mode, Ruleset::Brawl, room.coin, room.room, 0);
  EXPECT_TRUE(HeaderFitsRoom(ReadHeader(m), Mode::Casual, Ruleset::Brawl, code, 0));
  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(m), Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  // Another coin for the same room: not the room's.
  WriteHeader(m, room.mode, Ruleset::Brawl, static_cast<u8>(room.coin ^ 1), room.room, room.flags);
  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(m), Mode::Casual, Ruleset::Brawl, code, MB::FLAG_QUEUE2));

  // A friends room: no header at all.
  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(m), Mode::None, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  WriteHeader(m, Mode::None, Ruleset::Brawl);
  EXPECT_TRUE(HeaderFitsRoom(ReadHeader(m), Mode::None, Ruleset::Brawl, code, MB::FLAG_QUEUE2));
  EXPECT_TRUE(HeaderFitsRoom(ReadHeader(Game("scSelctCharacter")), Mode::None, Ruleset::Brawl, "",
                             0));
  EXPECT_FALSE(HeaderFitsRoom(ReadHeader(Game("scSelctCharacter")), Mode::Casual, Ruleset::Brawl,
                              code, MB::FLAG_QUEUE2));
}

// The header called for (SetWanted): each change counts once, and the game's memory is not known
// to carry it until the next frame hook reads it.
TEST(OrcaOnlineRules, EveryChangeOfTheHeaderCalledForCounts)
{
  SetWanted(Mode::None);
  const u64 g = WantedGeneration();
  EXPECT_FALSE(SetWanted(Mode::None, MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_EQ(WantedGeneration(), g);
  // The queue's own character select: no room's code in it.
  EXPECT_TRUE(SetWanted(Mode::Casual, MB::FLAG_SOLO | MB::FLAG_QUEUE2));
  EXPECT_FALSE(HeaderInPlace());
  EXPECT_FALSE(SetWanted(Mode::Casual, MB::FLAG_SOLO | MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_EQ(WantedGeneration(), g + 1);
  // The room's: its flags, then its code.
  EXPECT_TRUE(SetWanted(Mode::Casual, MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_FALSE(SetWanted(Mode::Casual, MB::FLAG_QUEUE2, "abcdefgh"));
  EXPECT_TRUE(SetWanted(Mode::Casual, MB::FLAG_QUEUE2, "ijklmnop"));
  EXPECT_TRUE(SetWanted(Mode::Ranked, MB::FLAG_QUEUE2, "ijklmnop"));
  EXPECT_EQ(WantedGeneration(), g + 4);
  EXPECT_EQ(Wanted(), Mode::Ranked);
  EXPECT_EQ(WantedFlags(), MB::FLAG_QUEUE2);
  MemoryReplaced();
  EXPECT_FALSE(HeaderInPlace());
  SetWanted(Mode::None);
  EXPECT_TRUE(HeaderIs(ReadHeader(Game("scSelctCharacter")), Header{}));
  EXPECT_FALSE(HeaderIs(ReadHeader(Locked(Mode::Casual, Ruleset::Brawl)), Header{}));
}

// The host's side (Rollback/OnlineMatch.h): the header may land with the opponent on the way, as
// long as nobody replays the frame; and a queue room makes no keyframe before it has.
TEST(OrcaOnlineRules, TheRoomsHeaderLandsWithTheOpponentOnTheWay)
{
  using Rollback::OnlineMatch::AloneAt;
  using Rollback::OnlineMatch::HeaderFreeAt;
  using Rollback::OnlineMatch::KEYFRAME_FRESH_FRAMES;
  // The opponent's hello came with the room's welcome: not alone, but free for the header.
  EXPECT_FALSE(AloneAt(5000, false, true, std::nullopt));
  EXPECT_TRUE(HeaderFreeAt(5000, true, std::nullopt));
  // A session, a keyframe being made or downloaded, a friend plugging in, catching up: no.
  EXPECT_FALSE(HeaderFreeAt(5000, false, std::nullopt));
  // A keyframe stored for this header that a friend would take, and replay every frame since.
  EXPECT_FALSE(HeaderFreeAt(5000, true, 4000));
  EXPECT_FALSE(HeaderFreeAt(4000 + KEYFRAME_FRESH_FRAMES, true, 4000));
  EXPECT_TRUE(HeaderFreeAt(4000 + KEYFRAME_FRESH_FRAMES + 1, true, 4000));
}

TEST(OrcaOnlineRules, AQueueRoomNeverMakesAKeyframeWithoutItsHeader)
{
  using Rollback::OnlineMatch::HEADER_FAIL_BOUNDARIES;
  using Rollback::OnlineMatch::HEADER_WAIT_BOUNDARIES;
  using Rollback::OnlineMatch::HeaderWait;
  using Rollback::OnlineMatch::StepHeaderWait;
  int waited = 0;
  // In place: at once.
  EXPECT_EQ(StepHeaderWait(&waited, true, true, true), HeaderWait::Ready);
  // Nobody waiting (a prepare-join): no keyframe, and nothing to fail.
  for (int i = 0; i < 2 * HEADER_FAIL_BOUNDARIES; ++i)
    EXPECT_EQ(StepHeaderWait(&waited, false, false, true), HeaderWait::Wait);
  EXPECT_EQ(waited, 0);
  // The opponent waits: never Ready until the header is there; then the join fails, once.
  int ready = 0, fails = 0, fail_at = -1;
  for (int i = 1; i <= HEADER_FAIL_BOUNDARIES + 1; ++i)
  {
    const HeaderWait w = StepHeaderWait(&waited, false, true, true);
    ready += w == HeaderWait::Ready;
    if (w == HeaderWait::Fail)
    {
      ++fails;
      fail_at = i;
    }
  }
  EXPECT_EQ(ready, 0);
  EXPECT_EQ(fails, 1);
  EXPECT_EQ(fail_at, HEADER_FAIL_BOUNDARIES + 1);
  EXPECT_EQ(waited, 0);
  // The header lands while the opponent waits: the keyframe follows.
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(StepHeaderWait(&waited, false, true, true), HeaderWait::Wait);
  EXPECT_EQ(StepHeaderWait(&waited, true, true, true), HeaderWait::Ready);
  EXPECT_EQ(waited, 0);
  // A friends room keeps its bound: the keyframe is made anyway (the joiner refuses a header that
  // locks), and the join never fails here.
  for (int i = 1; i <= HEADER_WAIT_BOUNDARIES; ++i)
    EXPECT_EQ(StepHeaderWait(&waited, false, true, false), HeaderWait::Wait);
  EXPECT_EQ(StepHeaderWait(&waited, false, true, false), HeaderWait::Ready);
  for (int i = 0; i < 2 * HEADER_FAIL_BOUNDARIES; ++i)
    EXPECT_NE(StepHeaderWait(&waited, false, true, false), HeaderWait::Fail);
}

// ---- Project+'s Code Menu and banned fighters (OnlineRules.h) ----
namespace
{
constexpr u32 FLIGHT = 0x804E23BC;
constexpr u32 P1_SHIELD = 0x804E0F08;
constexpr u32 P2_ALC = 0x804E1534;
constexpr u32 HITSTUN = 0x804E1E28;
constexpr u32 DEBUG_MODE = 0x804E0D3C;

// Project+'s Code Menu as its netplay codeset loads it: the layout CodeMenuPresent checks, the
// defaults, a player's cheats on top, and the game's pause word.
void LoadCodeMenu(FakeMemory& m, u32 activation = 0)
{
  m.Fill(CODE_MENU, CODE_MENU_END - CODE_MENU);
  m.Write32(CODE_MENU, CODE_MENU_ROOT);
  m.Write32(CODE_MENU + 4, CODE_MENU_ROOT);
  m.Write16(0x804E0C30, 0x0058);
  m.Write32(0x804E0C48, 0x804E0C30);
  m.Write16(0x804E23B4, 0x0030);
  m.Write32(0x804E23CC, 0x804E0F00);
  for (const CodeMenuWord& word : CodeMenuDefaults())
    m.Write32(word.address, word.value);
  m.Write32(CODE_MENU_ACTIVATION, activation);
  for (int port = 0; port < 4; ++port)
    m.Write32(CODE_MENU_CHARACTERS[port], 0x0C);
  m.Fill(CODE_MENU_GAME_PAUSE, 4);
  m.writes = 0;
}

void Cheat(FakeMemory& m)
{
  m.Write32(FLIGHT, 1);
  m.Write32(P1_SHIELD, 1);
  m.Write32(P2_ALC, 1);
  m.Write32(HITSTUN, 0x3F800000);
  m.Write32(DEBUG_MODE, 1);
  m.Write32(CODE_MENU_ACTIVATION, 0);
}

FakeMemory PPlusWithCodeMenu(Mode mode, std::string_view scene, u8 flags = 0)
{
  FakeMemory m = Game(scene);
  LoadCodeMenu(m);
  WriteHeader(m, mode, Ruleset::PPlus, 0, 0, flags);
  m.writes = 0;
  return m;
}

std::map<u32, u8> CodeMenuBytes(const FakeMemory& m)
{
  std::map<u32, u8> out;
  for (u32 a = CODE_MENU; a < CODE_MENU_END; ++a)
    out[a] = m.Read8(a);
  return out;
}
}  // namespace

// The table holds 73 distinct words inside the file, activation as OFF, and never a Character
// Select line.
TEST(OrcaOnlineRules, TheCodeMenuDefaultsAreTheFilesWords)
{
  const auto defaults = CodeMenuDefaults();
  EXPECT_EQ(defaults.size(), 73u);
  std::map<u32, u32> seen;
  for (const CodeMenuWord& word : defaults)
  {
    EXPECT_GE(word.address, CODE_MENU_ROOT);
    EXPECT_LT(word.address + 3, CODE_MENU_END);
    EXPECT_EQ(word.address % 4, 0u);
    EXPECT_TRUE(seen.emplace(word.address, word.value).second) << std::hex << word.address;
    for (u32 character : CODE_MENU_CHARACTERS)
      EXPECT_NE(word.address, character);
  }
  EXPECT_EQ(seen[CODE_MENU_ACTIVATION], CODE_MENU_OFF);
  EXPECT_EQ(seen[FLIGHT], 0u);
  EXPECT_EQ(seen[0x804E0ECC], 1u);  // HUD on
}

// Under every locking header (casual, ranked, the queue's own character select), in every scene:
// the menu off, its cheats back to the defaults, the characters left to the fighters. Twice is
// once.
TEST(OrcaOnlineRules, TheCodeMenuIsOffAndAtItsDefaultsUnderAProjectPlusHeader)
{
  for (const u8 flags : {u8{0}, u8{MB::FLAG_QUEUE2}, u8(MB::FLAG_SOLO | MB::FLAG_QUEUE2)})
  {
    for (Mode mode : {Mode::Casual, Mode::Ranked})
    {
      for (const char* scene : {"scSelctCharacter", "scSelStage", "scMemoryChange", "scMelee",
                                "scVsResult"})
      {
        FakeMemory m = PPlusWithCodeMenu(mode, scene, flags);
        Cheat(m);
        m.Write32(CODE_MENU_CHARACTERS[1], 0x2C);
        EXPECT_GT(ApplyLocks(m, NextFrame()), 0) << scene;
        EXPECT_EQ(m.Read32(CODE_MENU_ACTIVATION), CODE_MENU_OFF) << scene;
        EXPECT_EQ(m.Read32(FLIGHT), 0u);
        EXPECT_EQ(m.Read32(P1_SHIELD), 0u);
        EXPECT_EQ(m.Read32(P2_ALC), 0u);
        EXPECT_EQ(m.Read32(DEBUG_MODE), 0u);
        EXPECT_EQ(m.Read32(HITSTUN), 0x3ECCCCCDu);
        for (const CodeMenuWord& word : CodeMenuDefaults())
          EXPECT_EQ(m.Read32(word.address), word.value) << std::hex << word.address;
        EXPECT_EQ(m.Read32(CODE_MENU_CHARACTERS[0]), 0x0Cu);
        EXPECT_EQ(m.Read32(CODE_MENU_CHARACTERS[1]), 0x2Cu);
        m.writes = 0;
        EXPECT_EQ(ApplyLocks(m, NextFrame()), 0) << scene;
        EXPECT_EQ(m.writes, 0);
      }
    }
  }
}

// Open, the menu pauses the game. The locks close it the way its own Start does, giving the pause
// word back; any other state closes too (1 would open without the activation check, 3 swaps
// characters).
TEST(OrcaOnlineRules, AnOpenCodeMenuClosesAndTheGameRunsAgain)
{
  FakeMemory m = PPlusWithCodeMenu(Mode::Ranked, "scMelee");
  m.Write32(CODE_MENU_STATE, CODE_MENU_OPEN);
  m.Write32(CODE_MENU_SAVED_PAUSE, 0);
  m.Write32(CODE_MENU_GAME_PAUSE, 1);
  m.track_writes = true;
  ApplyLocks(m, NextFrame());
  m.track_writes = false;
  const auto regions = BrawlGame::Regions();
  ASSERT_FALSE(m.recorded.empty());
  for (const u32 address : m.recorded)
    EXPECT_TRUE(std::any_of(regions.begin(), regions.end(),
                           [&](const auto& region) { return region.Contains(address, 1); }))
        << std::hex << address;

  EXPECT_EQ(m.Read32(CODE_MENU_STATE), 0u);
  EXPECT_EQ(m.Read32(CODE_MENU_GAME_PAUSE), 0u);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  for (u32 state : {1u, 2u, 3u})
  {
    m.Write32(CODE_MENU_STATE, state);
    // Paused some other way: left alone.
    m.Write32(CODE_MENU_GAME_PAUSE, 1);
    ApplyLocks(m, NextFrame());
    EXPECT_EQ(m.Read32(CODE_MENU_STATE), 0u) << state;
    EXPECT_EQ(m.Read32(CODE_MENU_GAME_PAUSE), 1u) << state;
  }
}

// Nothing is written without a Project+ header, with Brawl's, or when the memory isn't the pinned
// menu's layout.
TEST(OrcaOnlineRules, TheCodeMenuIsLeftAloneWithoutItsHeaderOrItsLayout)
{
  {
    FakeMemory m = Game("scMelee");
    LoadCodeMenu(m);
    Cheat(m);
    const auto before = CodeMenuBytes(m);
    EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
    EXPECT_EQ(CodeMenuBytes(m), before);
  }
  {
    FakeMemory m = Game("scMelee");
    LoadCodeMenu(m);
    WriteHeader(m, Mode::Ranked, Ruleset::Brawl);
    Cheat(m);
    const auto before = CodeMenuBytes(m);
    ApplyLocks(m, NextFrame());
    EXPECT_EQ(CodeMenuBytes(m), before);
  }
  const struct
  {
    u32 address;
    u32 value;
  } breaks[] = {
      {CODE_MENU, 0x804E0000},     {CODE_MENU, CODE_MENU_END}, {CODE_MENU + 4, 0x804E0800},
      {0x804E0C30, 0x00540000},   {0x804E0C48, 0x804E0C34},   {0x804E23B4, 0x00340000},
      {0x804E23CC, 0x804E0F40},
  };
  for (const auto& broken : breaks)
  {
    FakeMemory m = PPlusWithCodeMenu(Mode::Ranked, "scMelee");
    m.Write32(broken.address, broken.value);
    EXPECT_FALSE(CodeMenuPresent(m)) << std::hex << broken.address;
    Cheat(m);
    m.Write32(CODE_MENU_STATE, CODE_MENU_OPEN);
    const auto before = CodeMenuBytes(m);
    ApplyLocks(m, NextFrame());
    EXPECT_EQ(CodeMenuBytes(m), before) << std::hex << broken.address;
  }
  // Not mapped (the game still loading): nothing.
  FakeMemory empty = Locked(Mode::Ranked, Ruleset::PPlus, "scMelee");
  EXPECT_FALSE(CodeMenuPresent(empty));
}

// The header saves the player's activation and its clear gives it back; the values stay at their
// defaults. A later header (the queue's own character select, then the room's) keeps the first
// save.
TEST(OrcaOnlineRules, ClearingGivesBackTheCodeMenusActivation)
{
  FakeMemory m = Game("scSelctCharacter");
  LoadCodeMenu(m, 1);  // PM 3.6
  m.Write32(FLIGHT, 1);
  WriteHeader(m, Mode::Ranked, Ruleset::PPlus, 0, 0, MB::FLAG_SOLO | MB::FLAG_QUEUE2);
  EXPECT_EQ(m.Read8(MB::SAVED_CODE_MENU), 1);
  EXPECT_EQ(m.Read8(MB::SAVED_FLAGS) & 4, 4);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(CODE_MENU_ACTIVATION), CODE_MENU_OFF);
  WriteHeader(m, Mode::Ranked, Ruleset::PPlus, 1, 0x1234, MB::FLAG_QUEUE2);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::SAVED_CODE_MENU), 1);
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::PPlus), 0);
  EXPECT_EQ(m.Read32(CODE_MENU_ACTIVATION), 1u);
  EXPECT_EQ(m.Read32(FLIGHT), 0u);
  // Without the menu when the header came, there is nothing to give back.
  FakeMemory late = Game("scSelctCharacter");
  WriteHeader(late, Mode::Casual, Ruleset::PPlus);
  EXPECT_EQ(late.Read8(MB::SAVED_FLAGS) & 4, 0);
  LoadCodeMenu(late, 0);
  ApplyLocks(late, NextFrame());
  WriteHeader(late, Mode::None, Ruleset::PPlus);
  EXPECT_EQ(late.Read32(CODE_MENU_ACTIVATION), CODE_MENU_OFF);
  // Brawl's header never saves one.
  FakeMemory brawl = Game("scSelctCharacter");
  LoadCodeMenu(brawl, 0);
  WriteHeader(brawl, Mode::Casual, Ruleset::Brawl);
  EXPECT_EQ(brawl.Read8(MB::SAVED_FLAGS) & 4, 0);
}

// Giga Bowser and Wario-Man (Project+'s character select makes them from a Bowser or Wario pick
// with shield held as it ends) go to the fight as Bowser and Wario: in the init data and in the
// character select's record (gmSelCharData: slots at +0x0A + 4 x port, characters at +0xB8 + port x
// 0x5C), on the character select, the stage select and between. The fight itself and other
// characters are left alone.
namespace
{
constexpr u32 RECORD = 0x90180B40;
u32 RecordCharacter(int port)
{
  return RECORD + 0xB8 + static_cast<u32>(port) * 0x5C;
}
u32 RecordSlot(int port)
{
  return RECORD + 0x0A + static_cast<u32>(port) * 4;
}
void BuildRecord(FakeMemory& m)
{
  m.Write32(0x90181300 + 0x10, RECORD);
  m.Fill(RECORD, 0xB8 + 4 * 0x5C);
  for (int port = 0; port < 4; ++port)
  {
    m.Write8(RecordCharacter(port), NONE);
    m.Write8(RecordSlot(port), 0x28);
  }
}
}  // namespace

TEST(OrcaOnlineRules, ProjectPlusBannedFightersPlayAsTheirBaseFighters)
{
  for (Mode mode : {Mode::Casual, Mode::Ranked})
  {
    for (const char* scene : {"scSelctCharacter", "scSelStage", "scMemoryChange"})
    {
      FakeMemory m = Locked(mode, Ruleset::PPlus, scene);
      BuildRecord(m);
      SetPick(m, 0, HUMAN, CHARACTER_GIGA_BOWSER);
      SetPick(m, 1, HUMAN, CHARACTER_WARIO_MAN);
      SetPick(m, 2, NOBODY, 0x11);  // Sopo stays
      m.Write8(RecordCharacter(0), CHARACTER_GIGA_BOWSER);
      m.Write8(RecordCharacter(1), CHARACTER_WARIO_MAN);
      m.Write8(RecordCharacter(2), 0x11);
      m.Write8(RecordSlot(0), 0x38);
      m.Write8(RecordSlot(1), 0x36);
      m.Write8(RecordSlot(2), 0x37);  // Sopo's slot stays
      ApplyLocks(m, NextFrame());
      EXPECT_EQ(ReadPicks(m)[0].character, CHARACTER_BOWSER) << scene;
      EXPECT_EQ(ReadPicks(m)[1].character, CHARACTER_WARIO) << scene;
      EXPECT_EQ(m.Read8(MELEE + 0x98 + 2 * 0x5C), 0x11);
      EXPECT_EQ(m.Read8(RecordCharacter(0)), CHARACTER_BOWSER) << scene;
      EXPECT_EQ(m.Read8(RecordCharacter(1)), CHARACTER_WARIO) << scene;
      EXPECT_EQ(m.Read8(RecordCharacter(2)), 0x11);
      EXPECT_EQ(m.Read8(RecordSlot(0)), 0x0C) << scene;
      EXPECT_EQ(m.Read8(RecordSlot(1)), 0x15) << scene;
      EXPECT_EQ(m.Read8(RecordSlot(2)), 0x37);
      m.writes = 0;
      EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
    }
  }
  FakeMemory fight = Locked(Mode::Ranked, Ruleset::PPlus, "scMelee");
  BuildRecord(fight);
  SetPick(fight, 0, HUMAN, CHARACTER_GIGA_BOWSER);
  fight.Write8(RecordSlot(0), 0x38);
  ApplyLocks(fight, NextFrame());
  EXPECT_EQ(ReadPicks(fight)[0].character, CHARACTER_GIGA_BOWSER);
  EXPECT_EQ(fight.Read8(RecordSlot(0)), 0x38);
  FakeMemory brawl = Locked(Mode::Ranked, Ruleset::Brawl);
  SetPick(brawl, 0, HUMAN, CHARACTER_GIGA_BOWSER);
  ApplyLocks(brawl, NextFrame());
  EXPECT_EQ(ReadPicks(brawl)[0].character, CHARACTER_GIGA_BOWSER);
  FakeMemory friends = Game("scSelctCharacter");
  SetPick(friends, 0, HUMAN, CHARACTER_WARIO_MAN);
  ApplyLocks(friends, NextFrame());
  EXPECT_EQ(ReadPicks(friends)[0].character, CHARACTER_WARIO_MAN);
}

// D-pad Down never reaches a Project+ game while L and R are held, in every scene under a header:
// the Code Menu's chord. Brawl and friends games keep it.
TEST(OrcaOnlineRules, ProjectPlusNeverSeesTheCodeMenusChord)
{
  for (const char* scene : {"scSelctCharacter", "scSelStage", "scMemoryChange", "scMelee",
                            "scVsResult", "muMenuMain"})
  {
    FakeMemory p = Locked(Mode::Ranked, Ruleset::PPlus, scene);
    for (const auto& mask : GateMasks(p))
      EXPECT_EQ(mask.drop_with_lr, PAD_BUTTON_DOWN) << scene;
    FakeMemory b = Locked(Mode::Ranked, Ruleset::Brawl, scene);
    for (const auto& mask : GateMasks(b))
      EXPECT_EQ(mask.drop_with_lr, 0) << scene;
    FakeMemory friends = Game(scene);
    for (const auto& mask : GateMasks(friends))
      EXPECT_TRUE(mask.Empty()) << scene;
  }
  // In the fight, Start stays masked and nothing else.
  FakeMemory fight = Locked(Mode::Casual, Ruleset::PPlus, "scMelee");
  for (const auto& mask : GateMasks(fight))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_START);
}

// A re-run from the same memory writes the same bytes (the Code Menu and the fighters included).
TEST(OrcaOnlineRules, TheCodeMenuLockIsAPureFunctionOfMemory)
{
  FakeMemory a = PPlusWithCodeMenu(Mode::Ranked, "scSelStage");
  Cheat(a);
  a.Write32(CODE_MENU_STATE, CODE_MENU_OPEN);
  a.Write32(CODE_MENU_SAVED_PAUSE, 0);
  a.Write32(CODE_MENU_GAME_PAUSE, 1);
  SetPick(a, 0, HUMAN, CHARACTER_WARIO_MAN);
  FakeMemory b = a;
  EXPECT_EQ(ApplyLocks(a, 700), ApplyLocks(b, 700));
  EXPECT_EQ(a.bytes, b.bytes);
}
