// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The builtin module's rulesets moved out of C++ into data (Orca split S5). These are the values
// the tables and constants held at split-s3 (RankedSteps.cpp MakeBrawl/MakePPlus, BrawlStages.cpp
// kStageKinds, RankedPPlus.cpp LEGAL_KINDS, RankedSet.h WINS/TIEBREAK_STREAK_LIMIT/LEDGE_LIMIT*/
// META_KNIGHT and WriteSetRules' 3/8 and 1/3, OnlineRules.cpp StocksFor and ForceRules' 8 minutes,
// OnlineRules.h CSS_READY_FRAMES, Policy/Queue.h READY_FRAMES and SKIP_FRAMES, Policy/CharOrder.h
// PICK_FRAMES): the data must say exactly the same, or the guest writes change.

#include <array>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Core/Orca/Engine/Ruleset.h"
#include "Core/Orca/Games/Brawl/BrawlStages.h"
#include "Core/Orca/Games/Brawl/PPlus/RankedPPlus.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"

namespace
{
namespace Ranked = Orca::UX::Ranked;

std::string RepoPath(const std::string& relative)
{
  return (std::filesystem::path(__FILE__).parent_path() / "../../../../../../" / relative).string();
}

void ExpectStages(const Ranked::Ruleset& r, const std::vector<std::string>& names,
                  const std::vector<u8>& kinds)
{
  ASSERT_EQ(r.stage_count, static_cast<int>(names.size()));
  for (size_t i = 0; i < names.size(); ++i)
  {
    EXPECT_EQ(r.stage_names[i], names[i]) << i;
    EXPECT_EQ(r.stage_kinds[i], kinds[i]) << i;
  }
  for (size_t i = names.size(); i < Ranked::MAX_STAGES; ++i)
  {
    EXPECT_EQ(r.stage_names[i], "") << i;
    EXPECT_EQ(r.stage_kinds[i], 0) << i;
  }
}
}  // namespace

TEST(OrcaRulesets, BuiltinsAreTheOldTables)
{
  const Ranked::Ruleset& b = Orca::UX::Rulesets::Brawl2025();
  EXPECT_EQ(b.id, 1);
  EXPECT_EQ(b.slug, "brawl-supernova-2025");
  EXPECT_EQ(b.name, "Brawl Supernova 2025");
  ExpectStages(b,
               {"Battlefield", "Pok\xC3\xA9mon Stadium", "Lylat Cruise", "Smashville",
                "Yoshi's Island", "Final Destination", "Delfino Plaza"},
               {0x01, 0x2E, 0x13, 0x21, 0x0D, 0x02, 0x03});
  EXPECT_EQ(b.starters, 0x001F);
  EXPECT_EQ(b.counterpicks, 0x0060);
  EXPECT_EQ(b.wins_needed, 2);
  EXPECT_EQ(b.later_kind, Ranked::StepKind::Ban);
  EXPECT_EQ(b.later_count, 1);
  EXPECT_TRUE(b.later_optional);
  EXPECT_TRUE(b.dsr);
  EXPECT_EQ(b.ic_banned_stage, 5);
  EXPECT_EQ(b.ice_climbers, 0x10);
  EXPECT_EQ(b.meta_knight, 0x18);
  EXPECT_EQ(b.game1_seconds, (std::array<u16, 3>{30, 30, 10}));
  EXPECT_EQ(b.later_seconds, 30);
  EXPECT_EQ(b.pick_seconds, 30);
  EXPECT_EQ(b.grace_frames, 180);
  EXPECT_EQ(b.casual_seconds, 15);
  EXPECT_EQ(b.casual_grace_frames, 30);
  EXPECT_EQ(b.stocks, 3);
  EXPECT_EQ(b.minutes, 8);
  EXPECT_EQ(b.time_out, Ranked::TimeOut::Judged);
  EXPECT_EQ(b.ledge.limit, 35);
  EXPECT_EQ(b.ledge.tiebreak_limit, 11);
  EXPECT_EQ(b.ledge.character_count, 1);
  EXPECT_EQ(b.ledge.For(false, 0x18), 20);
  EXPECT_EQ(b.ledge.For(true, 0x18), 6);
  EXPECT_EQ(b.ledge.For(false, 0x10), 35);
  EXPECT_EQ(b.ledge.For(true, 0x10), 11);
  EXPECT_TRUE(b.tiebreak);
  EXPECT_EQ(b.tiebreak_stocks, 1);
  EXPECT_EQ(b.tiebreak_minutes, 3);
  EXPECT_EQ(b.void_after_ties, 5);
  EXPECT_EQ(b.css_ready_seconds, 60);
  EXPECT_EQ(b.queue_ready_seconds, 30);
  EXPECT_EQ(b.queue_skip_frames, 90);
  EXPECT_EQ(b.order_pick_seconds, 45);

  const Ranked::Ruleset& p = Orca::UX::Rulesets::PPlus2024();
  EXPECT_EQ(p.id, 2);
  EXPECT_EQ(p.slug, "pplus-2024");
  EXPECT_EQ(p.name, "Project+ 2024");
  ExpectStages(p,
               {"Battlefield", "Pok\xC3\xA9mon Stadium 2", "Smashville", "Luigi's Mansion",
                "Temple of Time", "Green Hill Zone", "Bowser's Castle", "Frigate Husk",
                "Dream Land"},
               {0x01, 0x2E, 0x21, 0x04, 0x09, 0x23, 0x06, 0x0C, 0x2D});
  EXPECT_EQ(p.starters, 0x001F);
  EXPECT_EQ(p.counterpicks, 0x01E0);
  EXPECT_EQ(p.wins_needed, 2);
  EXPECT_EQ(p.later_kind, Ranked::StepKind::Strike);
  EXPECT_EQ(p.later_count, 2);
  EXPECT_FALSE(p.later_optional);
  EXPECT_TRUE(p.dsr);
  EXPECT_EQ(p.ic_banned_stage, -1);
  EXPECT_EQ(p.ice_climbers, -1);
  EXPECT_EQ(p.meta_knight, -1);
  EXPECT_EQ(p.game1_seconds, (std::array<u16, 3>{30, 30, 10}));
  EXPECT_EQ(p.later_seconds, 30);
  EXPECT_EQ(p.pick_seconds, 30);
  EXPECT_EQ(p.grace_frames, 180);
  EXPECT_EQ(p.casual_seconds, 15);
  EXPECT_EQ(p.casual_grace_frames, 30);
  EXPECT_EQ(p.stocks, 4);
  EXPECT_EQ(p.minutes, 8);
  EXPECT_EQ(p.time_out, Ranked::TimeOut::Results);
  EXPECT_EQ(p.ledge, Ranked::LedgeLimits{});
  EXPECT_FALSE(p.tiebreak);
  EXPECT_EQ(p.void_after_ties, 5);
  EXPECT_EQ(p.css_ready_seconds, 60);
  EXPECT_EQ(p.queue_ready_seconds, 30);
  EXPECT_EQ(p.queue_skip_frames, 90);
  EXPECT_EQ(p.order_pick_seconds, 45);

  EXPECT_EQ(Orca::UX::Rulesets::ByBlock(1), &b);
  EXPECT_EQ(Orca::UX::Rulesets::ByBlock(2), &p);
  for (const int block : {0, 3, 255})
    EXPECT_EQ(Orca::UX::Rulesets::ByBlock(static_cast<u8>(block)), nullptr);
}

// What is compiled in is the files in the tree, read by the same reader a pack's file gets.
TEST(OrcaRulesets, CompiledInAreTheFiles)
{
  for (const char* name : {"brawl-supernova-2025", "pplus-2024"})
  {
    std::string text, error;
    ASSERT_TRUE(File::ReadFileToString(
        RepoPath(std::string("Source/Core/Core/Orca/Games/Brawl/Rulesets/") + name + ".json"), text));
    const auto r = Orca::Engine::ReadRuleset(text, &error);
    ASSERT_TRUE(r) << name << ": " << error;
    const Ranked::Ruleset& builtin = r->id == 1 ? Orca::UX::Rulesets::Brawl2025() :
                                                  Orca::UX::Rulesets::PPlus2024();
    EXPECT_EQ(r->slug, name);
    EXPECT_EQ(builtin.slug, name);
    EXPECT_EQ(r->stage_names, builtin.stage_names);
    EXPECT_EQ(r->stage_kinds, builtin.stage_kinds);
    EXPECT_EQ(r->starters, builtin.starters);
    EXPECT_EQ(r->ledge, builtin.ledge);
    EXPECT_EQ(r->css_ready_seconds, builtin.css_ready_seconds);
  }
}

// The stage flows find a stage by the game's id through the ruleset's list; Brawl's stage select
// tiles (BrawlStages.cpp kTiles, layout data for a later slice) follow the same order.
TEST(OrcaRulesets, StageIdsComeFromTheRuleset)
{
  namespace BS = Orca::UX::BrawlStages;
  EXPECT_EQ(Orca::UX::Rulesets::Brawl2025().stage_count, static_cast<int>(BS::kStages));
  EXPECT_EQ(BS::StageForStageId(0x01), BS::Battlefield);
  EXPECT_EQ(BS::StageForStageId(0x2E), BS::PokemonStadium);
  EXPECT_EQ(BS::StageForStageId(0x13), BS::LylatCruise);
  EXPECT_EQ(BS::StageForStageId(0x21), BS::Smashville);
  EXPECT_EQ(BS::StageForStageId(0x0D), BS::YoshisIsland);
  EXPECT_EQ(BS::StageForStageId(0x02), BS::FinalDestination);
  EXPECT_EQ(BS::StageForStageId(0x03), BS::DelfinoPlaza);
  EXPECT_EQ(BS::StageForStageId(0x04), -1);
  const std::array<int, 9> pplus{0x01, 0x2E, 0x21, 0x04, 0x09, 0x23, 0x06, 0x0C, 0x2D};
  for (size_t i = 0; i < pplus.size(); ++i)
    EXPECT_EQ(Orca::UX::RankedPPlus::LegalIndexOfKind(pplus[i]), static_cast<int>(i));
  EXPECT_EQ(Orca::UX::RankedPPlus::LegalIndexOfKind(0x02), -1);
  EXPECT_EQ(Orca::UX::RankedPPlus::LegalIndexOfKind(0), -1);
}
