// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Orca/Engine/Module.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/JitWarm.h"
#include "Core/Orca/Launch.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/UX/UX.h"

namespace
{
const std::string SYS =
    (std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/").string();

class OrcaPPlus321 : public testing::Test
{
protected:
  void SetUp() override
  {
    old_user = File::GetUserPath(D_USER_IDX);
    directory = File::CreateTempDir();
    File::SetUserPath(D_USER_IDX, directory + DIR_SEP);
    Config::Init();
    SConfig::Init();
    old_ux = Orca::Events::UXCompatVersion();
    Orca::Events::SetUXCompatVersion(Orca::UX::kCompatVersion);
    Orca::Engine::SetModule(&Orca::UX::Builtin());
    old = Orca::LoadProfileFrom(SYS, "PPLUS32", std::nullopt, &error);
    next = Orca::LoadProfileFrom(SYS, "PPLUS321", std::nullopt, &error);
    ASSERT_TRUE(old) << error;
    ASSERT_TRUE(next) << error;
  }

  void TearDown() override
  {
    Orca::SetActiveProfile(std::nullopt);
    Orca::Engine::SetModule(nullptr);
    Orca::Events::SetUXCompatVersion(old_ux);
    SConfig::Shutdown();
    Config::Shutdown();
    File::SetUserPath(D_USER_IDX, old_user);
    File::DeleteDirRecursively(directory);
  }

  std::string old_user, directory, error;
  int old_ux = 0;
  std::optional<Orca::Profile> old, next;
};
}  // namespace

TEST_F(OrcaPPlus321, ReleasePinsAndForcedMachine)
{
  EXPECT_TRUE(next->IsLauncher());
  EXPECT_EQ(next->disc, "RSBE01");
  EXPECT_EQ(next->revision, 2);
  EXPECT_EQ(next->launcher_hash, "1e45a9a7ce1b8837020d15bc1379c21e");
  EXPECT_EQ(next->launcher_hash, old->launcher_hash);
  EXPECT_EQ(next->sd_image, "sd.raw");
  EXPECT_EQ(next->sd_image_hash, "5a9d851ea39afcbe15a1d17e8e42cf5d");
  EXPECT_NE(next->sd_image_hash, old->sd_image_hash);
  EXPECT_EQ(old->sd_image_hash, "5293a846d4de6139fd3ccd61faa9ac8d");
  EXPECT_EQ(next->forced, old->forced);
  EXPECT_EQ(next->save_title, old->save_title);
  EXPECT_EQ(next->kept_code, old->kept_code);
  EXPECT_EQ(next->jit_clear_frame, 2u);
  EXPECT_EQ(next->frame_hook, 0x80017504u);
  EXPECT_EQ(next->frame_hook_word, 0x90170100u);
  EXPECT_EQ(next->boot_nand_frame, 600u);
  Config::AddLayer(Orca::GenerateConfigLoader(*next));
  EXPECT_TRUE(Config::Get(Config::MAIN_WII_SD_CARD));
  EXPECT_FALSE(Config::Get(Config::MAIN_ALLOW_SD_WRITES));
}

TEST_F(OrcaPPlus321, BothReleasesUseProjectPlusRules)
{
  for (const auto* profile : {&*old, &*next})
  {
    SCOPED_TRACE(profile->game_id);
    Orca::SetActiveProfile(*profile);
    EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::Engine::Ruleset::PPlus);
    const auto game = Orca::UX::BrawlGame::Identify(profile);
    EXPECT_EQ(game.id, Orca::UX::BrawlGame::BRAWL_REV2_PPLUS);
    EXPECT_EQ(game.compat, Orca::UX::BrawlGame::BRAWL_REV2_PPLUS_COMPAT);
    EXPECT_TRUE(game.results);
    EXPECT_TRUE(game.music);
    auto wrong = *profile;
    wrong.revision = 1;
    EXPECT_TRUE(Orca::UX::BrawlGame::Identify(&wrong).id.empty());
    wrong = *profile;
    wrong.disc = "RSBP01";
    EXPECT_TRUE(Orca::UX::BrawlGame::Identify(&wrong).id.empty());
  }
}

TEST_F(OrcaPPlus321, CompatibilityKeysSeparateReleasesAndPreserve320)
{
  const auto key = [](const Orca::Profile& profile) {
    Orca::SetActiveProfile(profile);
    Config::AddLayer(Orca::GenerateConfigLoader(profile));
    return Orca::Online::CompatibilityKey();
  };
  const std::string before = key(*old);
  const std::string updated = key(*next);
  EXPECT_NE(before, updated);
  EXPECT_EQ(before, key(*old));
  RecordProperty("PPLUS32", before);
  RecordProperty("PPLUS321", updated);
  // Even a mislabelled card cannot merge the two launcher identities.
  auto same_card = *next;
  same_card.sd_image_hash = old->sd_image_hash;
  EXPECT_NE(before, key(same_card));
}

TEST_F(OrcaPPlus321, LegacyFilesAreByteIdenticalTo647d1aa6c7)
{
  EXPECT_EQ(Orca::HashFile(SYS + "Orca/PPLUS32.ini"), "ea7ba547b132f2957e109c4620160f97");
  EXPECT_EQ(Orca::HashFile(SYS + "Orca/PPLUS32.patches"), "342cc4c7634a2122a098b3f7db0eac1b");
  EXPECT_EQ(Orca::HashFile(SYS + "Orca/PPLUS32.jitwarm"), "8878e3439e5b4db1e44b4e81c28a2dc1");
}

TEST_F(OrcaPPlus321, OnlyBootToCssBodyMovesInGuardedPatches)
{
  std::string a, b;
  ASSERT_TRUE(File::ReadFileToString(SYS + "Orca/PPLUS32.patches", a));
  ASSERT_TRUE(File::ReadFileToString(SYS + "Orca/PPLUS321.patches", b));
  const auto original = Orca::UX::ParseGamePatches(a, &error);
  const auto updated = Orca::UX::ParseGamePatches(b, &error);
  ASSERT_TRUE(original) << error;
  ASSERT_TRUE(updated) << error;
  ASSERT_EQ(original->size(), updated->size());
  unsigned moved = 0;
  for (size_t i = 0; i < original->size(); ++i)
  {
    const auto& from = (*original)[i];
    const auto& to = (*updated)[i];
    const bool boot = from.address >= 0x80559C20 && from.address <= 0x80559C2C;
    EXPECT_EQ(to.address, from.address - (boot ? 0x18u : 0u));
    EXPECT_EQ(to.original, from.original);
    EXPECT_EQ(to.value, from.value);
    EXPECT_EQ(to.when, from.when);
    EXPECT_EQ(to.joins_previous, from.joins_previous);
    EXPECT_TRUE(to.original.has_value());
    moved += boot;
  }
  EXPECT_EQ(moved, 4u);
}

TEST(OrcaPPlus321Launch, EightCharacterProfileName)
{
  std::string error;
  const auto launch = Orca::ParseLaunchIni(
      "[Launch]\nProfile = PPLUS321\nExecutable = Project+ Netplay Launcher.dol\n", &error);
  ASSERT_TRUE(launch) << error;
  EXPECT_EQ(launch->profile, "PPLUS321");
}

TEST(OrcaPPlus321Seed, RecordedForTheNewCard)
{
  std::string bytes;
  ASSERT_TRUE(File::ReadFileToString(SYS + "Orca/PPLUS321.jitwarm", bytes));
  const auto blocks = Orca::JitWarm::Decode(
      std::span(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
  ASSERT_TRUE(blocks);
  EXPECT_GT(blocks->size(), 20'000u);
  EXPECT_LE(blocks->size(), Orca::JitWarm::MAX_BLOCKS);
  bool early = false;
  for (const auto& block : *blocks)
    early = early || block.when < 30;
  EXPECT_TRUE(early);
  EXPECT_NE(Orca::HashFile(SYS + "Orca/PPLUS321.jitwarm"),
            Orca::HashFile(SYS + "Orca/PPLUS32.jitwarm"));
}
