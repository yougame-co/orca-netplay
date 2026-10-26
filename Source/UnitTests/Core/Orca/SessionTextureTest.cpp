// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/LogManager.h"
#include "Common/ScopeGuard.h"
#include "Core/ConfigManager.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/VideoConfig.h"

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

std::string FileList(const std::vector<HiresTextureFile>& files)
{
  std::string bytes;
  for (const auto& file : files)
  {
    bytes += file.id + '\t' + file.path + '\t' +
             (file.has_arbitrary_mipmaps ? "1" : "0") + '\t' + (file.orca_pack ? "1\n" : "0\n");
  }
  return bytes;
}

class IsolationLog final : public Common::Log::LogListener
{
public:
  void Log(Common::Log::LogLevel level, const char* message) override
  {
    std::string line = message;
    const size_t start = line.find("orca isolation ");
    if (start == std::string::npos)
      return;
    line.erase(0, start);
    if (line.ends_with('\n'))
      line.pop_back();
    lines.push_back(std::move(line));
    levels.push_back(level);
  }
  std::vector<std::string> lines;
  std::vector<Common::Log::LogLevel> levels;
};

class MemoryLayer final : public Config::ConfigLayerLoader
{
public:
  MemoryLayer() : ConfigLayerLoader(Config::LayerType::Base) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

class OrcaSessionTextures : public testing::Test
{
protected:
  void SetUp() override
  {
    m_session = Orca::GetEnv("ORCA_SESSION");
    if (const auto* profile = Orca::ActiveProfile())
      m_profile = *profile;
    Orca::SetActiveProfile(std::nullopt);
    m_seed = Orca::SeedSessionSave();
    Session("1");
    m_root = File::CreateTempDir();
    ASSERT_FALSE(m_root.empty()) << "mkdtemp: " << std::strerror(errno);
    m_player = m_root + "/User/Load/Textures/RSBE01";
    m_bundle = m_root + "/pack/orca/textures";
    m_engine = m_root + "/Sys/Orca/Textures/RSBE01";
    for (const auto& path : {m_player + "/tex1_player_arb.png",
                             m_player + "/tex1_player_arb_mip1.png",
                             m_player + "/tex1_shared.png", m_player + "/notes.png",
                             m_bundle + "/tex1_shared.png", m_engine + "/tex1_shared.png"})
    {
      ASSERT_TRUE(File::CreateFullPath(path));
      ASSERT_TRUE(File::WriteStringToFile(path, "texture fixture; never decoded"));
    }
  }
  void TearDown() override
  {
    Orca::Mod::SetLocalPack({});
    Orca::SetActiveProfile(std::move(m_profile));
    Orca::SetSeedSessionSave(m_seed);
    Session(m_session);
    if (!m_root.empty())
      File::DeleteDirRecursively(m_root);
  }
  void Mod(bool flat)
  {
    Orca::Profile profile;
    profile.mod.emplace();
    profile.mod->flat = flat;
    Orca::SetActiveProfile(std::move(profile));
  }
  std::string TrustedOnly(const std::string& directory) const
  {
    return "tex1_shared\t" + directory + "/tex1_shared.png\t0\t1\n";
  }
  std::string PlayerOnly() const
  {
    return "tex1_player\t" + m_player + "/tex1_player_arb.png\t1\t0\n";
  }
  std::string m_root, m_player, m_bundle, m_engine;

private:
  std::string m_session;
  std::optional<Orca::Profile> m_profile;
  bool m_seed = true;
};
}  // namespace

TEST_F(OrcaSessionTextures, ModSessionsUseOnlyBundleOrEngineTextureFiles)
{
  for (const bool flat : {false, true})
  {
    Mod(flat);
    const std::string& trusted = flat ? m_engine : m_bundle;
    EXPECT_EQ(FileList(CollectHiresTextureFiles({m_player}, {trusted})), TrustedOnly(trusted));
  }
}

TEST_F(OrcaSessionTextures, ModWithoutATexturePackDoesNotUsePlayerFiles)
{
  Mod(false);
  EXPECT_TRUE(CollectHiresTextureFiles({m_player}, {}).empty());
}

TEST_F(OrcaSessionTextures, RequestedModIgnoresPlayerFilesBeforeProfileSelection)
{
  Orca::Mod::SetLocalPack(m_root + "/pack");
  ASSERT_TRUE(Orca::Mod::Requested());
  ASSERT_EQ(Orca::ActiveProfile(), nullptr);
  EXPECT_TRUE(CollectHiresTextureFiles({m_player}, {}).empty());
}

TEST_F(OrcaSessionTextures, LegacySessionSeedAndTextureFileListAreByteIdentical)
{
  // These bytes pin the pre-isolation ordering, paths, arbitrary-mipmap flag and pack ownership.
  const std::string expected = TrustedOnly(m_engine) + PlayerOnly();
  for (const std::string game : {"RSBE01", "PPLUS32", "PPLUS321"})
  {
    Orca::Profile profile;
    profile.game_id = game;
    Orca::SetActiveProfile(std::move(profile));
    for (const bool seed : {false, true})
    {
      Orca::SetSeedSessionSave(seed);
      EXPECT_EQ(Orca::SeedSessionSave(), seed) << game;
      EXPECT_EQ(FileList(CollectHiresTextureFiles({m_player}, {m_engine})), expected) << game;
    }
  }
}

TEST_F(OrcaSessionTextures, NonSessionPlayKeepsPlayerTextures)
{
  Mod(false);
  Session("0");
  EXPECT_EQ(FileList(CollectHiresTextureFiles({m_player}, {})),
            PlayerOnly() + "tex1_shared\t" + m_player + "/tex1_shared.png\t0\t0\n");
}

TEST_F(OrcaSessionTextures, StatusIsInfoOncePerSessionEvenWithReplacementsDisabled)
{
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  const bool old_hires = g_ActiveConfig.bHiresTextures;
  File::SetUserPath(D_USER_IDX, m_root + "/User/");
  Config::Init();
  Config::AddLayer(std::make_unique<MemoryLayer>());
  SConfig::Init();
  Common::Log::LogManager::Init();
  Common::ScopeGuard done([&] {
    HiresTexture::Shutdown();
    g_ActiveConfig.bHiresTextures = old_hires;
    SConfig::Shutdown();
    Common::Log::LogManager::Shutdown();
    Config::Shutdown();
    File::SetUserPath(D_USER_IDX, old_user);
  });

  const std::string player = File::GetUserPath(D_HIRESTEXTURES_IDX) +
                             SConfig::GetInstance().GetGameIDsForTextures().front() + '/';
  for (const std::string name : {"tex1_status.png", "tex1_status_mip1.png", "notes.png"})
  {
    ASSERT_TRUE(File::CreateFullPath(player + name));
    ASSERT_TRUE(File::WriteStringToFile(player + name, "never decoded"));
  }
  auto listener = std::make_unique<IsolationLog>();
  const auto* captured = listener.get();
  auto& logs = *Common::Log::LogManager::GetInstance();
  logs.RegisterListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, std::move(listener));
  logs.EnableListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, true);
  Config::SetCurrent(Common::Log::LOGGER_VERBOSITY, Common::Log::LogLevel::LINFO);
  logs.SetEnable(Common::Log::LogType::VIDEO, true);

  Mod(false);
  g_ActiveConfig.bHiresTextures = false;
  HiresTexture::Shutdown();
  HiresTexture::Update();
  HiresTexture::Clear();
  HiresTexture::Update();
  const std::string expected =
      "orca isolation save=clean textures=bundle-only ignored-user-textures=1";
  EXPECT_EQ(captured->lines, std::vector<std::string>{expected});
  HiresTexture::Shutdown();
  HiresTexture::Update();
  EXPECT_EQ(captured->lines, (std::vector<std::string>{expected, expected}));
  EXPECT_EQ(captured->levels, (std::vector<Common::Log::LogLevel>{
                                 Common::Log::LogLevel::LINFO, Common::Log::LogLevel::LINFO}));
}
