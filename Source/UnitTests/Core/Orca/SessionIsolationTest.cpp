// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "Core/Orca/Bundle.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"

namespace
{
void Session(const std::string& value)
{
#ifdef _WIN32
  _putenv_s("ORCA_SESSION", value.c_str());
#else
  setenv("ORCA_SESSION", value.c_str(), 1);
#endif
}

class OrcaSessionIsolation : public testing::Test
{
protected:
  void SetUp() override
  {
    m_session = Orca::GetEnv("ORCA_SESSION");
    if (const auto* profile = Orca::ActiveProfile())
      m_profile = *profile;
    Orca::SetActiveProfile(std::nullopt);
    m_seed = Orca::SeedSessionSave();
    Orca::SetSeedSessionSave(true);
    Session("1");
  }
  void TearDown() override
  {
    Orca::Mod::SetLocalPack({});
    Orca::SetActiveProfile(std::move(m_profile));
    Orca::SetSeedSessionSave(m_seed);
    Session(m_session);
  }
  void Mod(bool flat = false, bool save_file = false)
  {
    Orca::Profile profile;
    profile.game_id = "RSBE01";
    profile.save_title = 0x0001000052534245ull;
    profile.mod.emplace();
    profile.mod->flat = flat;
    profile.mod->manifest.format = flat ? 2 : 1;
    // A tree may list an arbitrary file, but no manifest field imports one into the NAND.
    if (save_file)
      profile.mod->tree.files.push_back({"save.bin", std::string(64, 'a'), 1});
    Orca::SetActiveProfile(std::move(profile));
  }

private:
  std::string m_session;
  std::optional<Orca::Profile> m_profile;
  bool m_seed = true;
};
}  // namespace

TEST_F(OrcaSessionIsolation, ModSessionsStartCleanWithOrWithoutATreeSaveFile)
{
  for (const bool flat : {false, true})
  {
    for (const bool save_file : {false, true})
    {
      Mod(flat, save_file);
      EXPECT_FALSE(Orca::SeedSessionSave());
      Orca::SetSeedSessionSave(false);
      EXPECT_FALSE(Orca::SeedSessionSave());
      Orca::SetSeedSessionSave(true);
    }
  }
}

TEST_F(OrcaSessionIsolation, LegacySeedDecisionIsUnchanged)
{
  for (const std::string game : {"RSBE01", "PPLUS32", "PPLUS321"})
  {
    Orca::Profile profile;
    profile.game_id = game;
    Orca::SetActiveProfile(std::move(profile));
    for (const bool seed : {false, true})
    {
      Orca::SetSeedSessionSave(seed);
      EXPECT_EQ(Orca::SeedSessionSave(), seed) << game;
    }
  }
  // Even a retained mod profile must not change non-session behavior.
  Mod();
  Session("0");
  EXPECT_TRUE(Orca::SeedSessionSave());
}

TEST_F(OrcaSessionIsolation, RequestedModDoesNotSeedBeforeProfileSelection)
{
  Orca::Mod::SetLocalPack("unused-pack-fixture");
  ASSERT_TRUE(Orca::Mod::Requested());
  ASSERT_EQ(Orca::ActiveProfile(), nullptr);
  EXPECT_FALSE(Orca::SeedSessionSave());
}
