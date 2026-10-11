// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/Config/Config.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/HSP/HSP_Device.h"
#include "Core/Orca/Perf.h"
#include "Core/Orca/Profile.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoConfig.h"

// Which settings a session takes from the player (the app's -C, GFX.ini) and which it forces,
// whatever comes from outside. See ORCA.md, "Graphics".
namespace
{
// A layer kept in memory: GFX.ini and Dolphin.ini (Base), the app's -C (CommandLine), a game INI.
class MemoryLayer final : public Config::ConfigLayerLoader
{
public:
  explicit MemoryLayer(Config::LayerType layer) : ConfigLayerLoader(layer) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

class OrcaGraphicsSettings : public testing::Test
{
protected:
  void SetUp() override
  {
    Config::Init();
    for (const Config::LayerType layer :
         {Config::LayerType::Base, Config::LayerType::CommandLine, Config::LayerType::LocalGame})
    {
      Config::AddLayer(std::make_unique<MemoryLayer>(layer));
    }
    m_profile.game_id = "RSBE01";
    m_profile.revision = 2;
  }
  void TearDown() override { Config::Shutdown(); }
  // The app's -C.
  template <typename T, typename V>
  static void CommandLine(const Config::Info<T>& info, const V& value)
  {
    Config::Set(Config::LayerType::CommandLine, info, value);
  }
  // The player's own GFX.ini or Dolphin.ini.
  template <typename T, typename V>
  static void Ini(const Config::Info<T>& info, const V& value)
  {
    Config::SetBase(info, value);
  }
  // The session boots: Orca's layer goes on top of everything else.
  void Boot() { Config::AddLayer(Orca::GenerateConfigLoader(m_profile)); }
  std::optional<std::string> Verify() const { return Orca::VerifyForcedSettings(m_profile); }

  Orca::Profile m_profile;
};

bool Has(const std::vector<Config::Location>& locations, const Config::Location& location)
{
  return std::find(locations.begin(), locations.end(), location) != locations.end();
}
}  // namespace

TEST_F(OrcaGraphicsSettings, GameAffectingSettingsFromOutsideAreIgnored)
{
  // Everything a player or a modified app could pass to get an edge or break the match.
  CommandLine(Config::MAIN_EMULATION_SPEED, 0.5f);
  CommandLine(Config::MAIN_GDB_PORT, 2345);
  CommandLine(Config::MAIN_DEBUG_JIT_OFF, true);
  CommandLine(Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF, true);
  CommandLine(Config::MAIN_ENABLE_DEBUGGING, true);
  CommandLine(Config::GFX_WIDESCREEN_HACK, true);
  CommandLine(Config::MAIN_CPU_THREAD, true);
  Ini(Config::GFX_DISABLE_FOG, true);
  Ini(Config::GFX_ENABLE_WIREFRAME, true);
  Ini(Config::GFX_TEXFMT_OVERLAY_ENABLE, true);
  Ini(Config::FREE_LOOK_ENABLED, true);
  Ini(Config::MAIN_GDB_SOCKET, std::string("/tmp/orca-gdb"));
  Ini(Config::MAIN_LOW_DCBZ_HACK, true);
  Ini(Config::MAIN_OVERRIDE_BOOT_IOS, 36);
  Ini(Config::MAIN_HSP_DEVICE, HSP::HSPDeviceType::ARAMExpansion);
  Ini(Config::MAIN_RUSH_FRAME_PRESENTATION, true);
  Ini(Config::MAIN_MAX_FALLBACK, 500);
  Ini(Config::GFX_HACK_CAP_IMMEDIATE_XFB, true);
  Ini(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, false);
  Ini(Config::GFX_VSYNC, true);
  Ini(Config::MAIN_PAUSE_ON_PANIC, true);
  Ini(Config::MAIN_AUDIO_BUFFER_SIZE, 512);
  Ini(Config::GFX_HACK_VERTEX_ROUNDING, true);
  Boot();

  EXPECT_FALSE(Config::Get(Config::MAIN_PAUSE_ON_PANIC));
  EXPECT_EQ(Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE), 80);
  EXPECT_FALSE(Config::Get(Config::GFX_HACK_VERTEX_ROUNDING));
  EXPECT_EQ(Config::Get(Config::MAIN_EMULATION_SPEED), 1.0f);
  EXPECT_EQ(Config::Get(Config::MAIN_GDB_PORT), -1);
  EXPECT_EQ(Config::Get(Config::MAIN_GDB_SOCKET), "");
  EXPECT_FALSE(Config::Get(Config::MAIN_DEBUG_JIT_OFF));
  EXPECT_FALSE(Config::Get(Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF));
  EXPECT_FALSE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
  EXPECT_FALSE(Config::Get(Config::GFX_WIDESCREEN_HACK));
  EXPECT_FALSE(Config::Get(Config::MAIN_CPU_THREAD));
  EXPECT_FALSE(Config::Get(Config::GFX_DISABLE_FOG));
  EXPECT_FALSE(Config::Get(Config::GFX_ENABLE_WIREFRAME));
  EXPECT_FALSE(Config::Get(Config::GFX_TEXFMT_OVERLAY_ENABLE));
  EXPECT_FALSE(Config::Get(Config::FREE_LOOK_ENABLED));
  EXPECT_FALSE(Config::Get(Config::MAIN_LOW_DCBZ_HACK));
  EXPECT_EQ(Config::Get(Config::MAIN_OVERRIDE_BOOT_IOS), -1);
  EXPECT_EQ(Config::Get(Config::MAIN_HSP_DEVICE), HSP::HSPDeviceType::None);
  EXPECT_EQ(Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION),
            Config::MAIN_RUSH_FRAME_PRESENTATION.GetDefaultValue());
  EXPECT_EQ(Config::Get(Config::MAIN_MAX_FALLBACK), Config::MAIN_MAX_FALLBACK.GetDefaultValue());
  EXPECT_FALSE(Config::Get(Config::GFX_HACK_CAP_IMMEDIATE_XFB));
  EXPECT_TRUE(Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM));
  EXPECT_FALSE(Config::Get(Config::GFX_VSYNC));
  EXPECT_EQ(Verify(), std::nullopt);
}

TEST_F(OrcaGraphicsSettings, DrawingOnlySettingsAreThePlayers)
{
  CommandLine(Config::MAIN_GFX_BACKEND, std::string("Vulkan"));
  CommandLine(Config::GFX_ADAPTER, 1);
  CommandLine(Config::GFX_EFB_SCALE, 3);
  CommandLine(Config::GFX_MAX_EFB_SCALE, 4);
  CommandLine(Config::GFX_MSAA, 4u);
  CommandLine(Config::GFX_SSAA, true);
  CommandLine(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropicFilteringMode::Force16x);
  CommandLine(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, TextureFilteringMode::Linear);
  CommandLine(Config::GFX_ENHANCE_OUTPUT_RESAMPLING, OutputResamplingMode::SharpBilinear);
  CommandLine(Config::GFX_HACK_COPY_EFB_SCALED, false);
  CommandLine(Config::GFX_ENABLE_GPU_TEXTURE_DECODING, true);
  CommandLine(Config::GFX_CPU_CULL, true);
  CommandLine(Config::GFX_BACKEND_MULTITHREADING, false);
  CommandLine(Config::GFX_SHADER_COMPILATION_MODE,
              ShaderCompilationMode::AsynchronousSkipRendering);
  CommandLine(Config::GFX_SHADER_COMPILER_THREADS, 2);
  Boot();

  EXPECT_EQ(Config::Get(Config::MAIN_GFX_BACKEND), "Vulkan");
  EXPECT_EQ(Config::Get(Config::GFX_ADAPTER), 1);
  EXPECT_EQ(Config::Get(Config::GFX_EFB_SCALE), 3);
  EXPECT_EQ(Config::Get(Config::GFX_MAX_EFB_SCALE), 4);
  EXPECT_EQ(Config::Get(Config::GFX_MSAA), 4u);
  EXPECT_TRUE(Config::Get(Config::GFX_SSAA));
  EXPECT_EQ(Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY), AnisotropicFilteringMode::Force16x);
  EXPECT_EQ(Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING), TextureFilteringMode::Linear);
  EXPECT_EQ(Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING),
            OutputResamplingMode::SharpBilinear);
  EXPECT_FALSE(Config::Get(Config::GFX_HACK_COPY_EFB_SCALED));
  EXPECT_TRUE(Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING));
  EXPECT_TRUE(Config::Get(Config::GFX_CPU_CULL));
  EXPECT_FALSE(Config::Get(Config::GFX_BACKEND_MULTITHREADING));
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILATION_MODE),
            ShaderCompilationMode::AsynchronousSkipRendering);
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILER_THREADS), 2);
  EXPECT_EQ(Verify(), std::nullopt);
}

TEST_F(OrcaGraphicsSettings, ShaderModesThatStallTheOtherPlayerAreRefused)
{
  // Synchronous compiles at a pipeline's first draw on the emulation thread; so does a thread
  // count of 0.
  CommandLine(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
  CommandLine(Config::GFX_SHADER_COMPILER_THREADS, 0);
  Boot();
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILATION_MODE),
            ShaderCompilationMode::AsynchronousUberShaders);
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILER_THREADS), -1);
  EXPECT_EQ(Verify(), std::nullopt);
}

TEST_F(OrcaGraphicsSettings, SyncUberIsRefusedToo)
{
  Ini(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::SynchronousUberShaders);
  Boot();
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILATION_MODE),
            ShaderCompilationMode::AsynchronousUberShaders);
}

TEST_F(OrcaGraphicsSettings, NothingPassedMeansWhatSessionsForcedBefore)
{
  // An old app passes neither; Dolphin's defaults are Synchronous and one thread. Sessions keep
  // what they forced before the player could choose: AsyncUber and Auto (-1).
  Boot();
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILATION_MODE),
            ShaderCompilationMode::AsynchronousUberShaders);
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILER_THREADS), -1);
  EXPECT_EQ(Verify(), std::nullopt);
}

TEST_F(OrcaGraphicsSettings, OneThreadIsThePlayersChoiceToo)
{
  CommandLine(Config::GFX_SHADER_COMPILER_THREADS, 1);
  Boot();
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILER_THREADS), 1);
}

TEST_F(OrcaGraphicsSettings, ThreadsOutsideTheAppsChoicesAreAuto)
{
  // A hand-edited GFX.ini asking for 500 compile threads: Auto instead.
  Ini(Config::GFX_SHADER_COMPILER_THREADS, 500);
  Boot();
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILER_THREADS), -1);
  EXPECT_EQ(Verify(), std::nullopt);
  EXPECT_TRUE(Orca::SessionShaderThreadsAllowed(4));
  EXPECT_FALSE(Orca::SessionShaderThreadsAllowed(5));
  EXPECT_FALSE(Orca::SessionShaderThreadsAllowed(-2));
}

TEST_F(OrcaGraphicsSettings, TheHighestLayerUnderTheSessionDecides)
{
  // A game INI over the app's -C, as Config::Get would read them without the session.
  CommandLine(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::AsynchronousUberShaders);
  Config::Set(Config::LayerType::LocalGame, Config::GFX_SHADER_COMPILATION_MODE,
              ShaderCompilationMode::Synchronous);
  Boot();
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILATION_MODE),
            ShaderCompilationMode::AsynchronousUberShaders);
}

TEST_F(OrcaGraphicsSettings, TheModeIsPinnedForTheRun)
{
  // A launcher's disc brings its game INI layers in after the session's: they can't change the
  // mode the session took at boot.
  CommandLine(Config::GFX_SHADER_COMPILATION_MODE,
              ShaderCompilationMode::AsynchronousSkipRendering);
  Boot();
  Config::Set(Config::LayerType::LocalGame, Config::GFX_SHADER_COMPILATION_MODE,
              ShaderCompilationMode::Synchronous);
  Config::Set(Config::LayerType::LocalGame, Config::GFX_SHADER_COMPILER_THREADS, 0);
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILATION_MODE),
            ShaderCompilationMode::AsynchronousSkipRendering);
  EXPECT_EQ(Config::Get(Config::GFX_SHADER_COMPILER_THREADS), -1);
}

TEST_F(OrcaGraphicsSettings, AnythingAboveTheSessionFailsTheBoot)
{
  Boot();
  // Written after the session's layer, on the one layer above it: refused at boot.
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 0.5f);
  ASSERT_TRUE(Verify().has_value());
  EXPECT_NE(Verify()->find("EmulationSpeed"), std::string::npos);
  Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_EMULATION_SPEED);
  EXPECT_EQ(Verify(), std::nullopt);

  Config::SetCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
  ASSERT_TRUE(Verify().has_value());
  EXPECT_NE(Verify()->find("shader compilation mode"), std::string::npos);
  Config::DeleteKey(Config::LayerType::CurrentRun, Config::GFX_SHADER_COMPILATION_MODE);
  Config::SetCurrent(Config::GFX_SHADER_COMPILER_THREADS, 0);
  ASSERT_TRUE(Verify().has_value());
  EXPECT_NE(Verify()->find("shader compiler threads"), std::string::npos);
}

TEST_F(OrcaGraphicsSettings, LiveChangesTouchOnlyDrawingSettings)
{
  // The `gfx` command writes the layer above the session's, so none of its keys may be forced...
  const std::vector<Config::Location> forced = Orca::UniversalForcedLocations();
  for (const Config::Location& location : Orca::Perf::GfxLocations())
    EXPECT_FALSE(Has(forced, location)) << location.section << "." << location.key;
  // ...and every change it can make leaves the session's settings standing.
  Boot();
  for (const char* line : {"gfx res 4", "gfx aniso 4", "gfx filter 1", "gfx resample 6",
                           "gfx efb 0", "gfx gpudecode 1", "gfx cpucull 1"})
  {
    const std::optional<Orca::Perf::GfxChange> change = Orca::Perf::ParseGfx(line);
    ASSERT_TRUE(change) << line;
    Orca::Perf::ApplyGfx(*change);
  }
  EXPECT_EQ(Verify(), std::nullopt);
  EXPECT_EQ(Config::Get(Config::GFX_EFB_SCALE), 4);
  EXPECT_EQ(Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY), AnisotropicFilteringMode::Force16x);
  EXPECT_EQ(Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING),
            TextureFilteringMode::Nearest);
  EXPECT_EQ(Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING), OutputResamplingMode::AreaSampling);
  EXPECT_FALSE(Config::Get(Config::GFX_HACK_COPY_EFB_SCALED));
  EXPECT_TRUE(Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING));
  EXPECT_TRUE(Config::Get(Config::GFX_CPU_CULL));
}

TEST_F(OrcaGraphicsSettings, ResolutionChangesNeverReloadTheShaders)
{
  // VertexRounding is a shader host-config bit only away from native resolution: with it on, the
  // switch to Performance (res 1) or a live `gfx res` would reload every shader mid-match. Forced
  // off, no `gfx` change moves the host config.
  Ini(Config::GFX_HACK_VERTEX_ROUNDING, true);
  CommandLine(Config::GFX_EFB_SCALE, 4);
  Boot();
  EXPECT_FALSE(Config::Get(Config::GFX_HACK_VERTEX_ROUNDING));
  const auto host_bits = [] {
    g_Config.Refresh();
    UpdateActiveConfig();
    return ShaderHostConfig::GetCurrent().bits;
  };
  const u32 before = host_bits();
  for (const char* line :
       {"gfx res 1", "gfx res 0", "gfx res 4", "gfx aniso 4", "gfx aniso -1", "gfx filter 1",
        "gfx filter 2", "gfx resample 6", "gfx efb 0", "gfx efb 1", "gfx gpudecode 1",
        "gfx cpucull 1", "gfx res 1"})
  {
    const std::optional<Orca::Perf::GfxChange> change = Orca::Perf::ParseGfx(line);
    ASSERT_TRUE(change) << line;
    Orca::Perf::ApplyGfx(*change);
    EXPECT_EQ(host_bits(), before) << line;
  }
  // The switch to Performance is the same changes.
  for (const Orca::Perf::GfxChange& change : Orca::Perf::PerformancePreset())
    Orca::Perf::ApplyGfx(change);
  EXPECT_EQ(host_bits(), before);
}

TEST_F(OrcaGraphicsSettings, QueuedChangesKeepTheLastValuePerKey)
{
  Boot();
  // A burst of lines (a page while the game is paused): one change per key when they apply.
  for (int i = 0; i < 1000; ++i)
    Orca::Perf::QueueGfx({Orca::Perf::GfxKey::Res, i % 2 ? 2 : 3});
  Orca::Perf::QueueGfx({Orca::Perf::GfxKey::Efb, 0});
  Orca::Perf::QueueGfx({Orca::Perf::GfxKey::Res, 4});
  // The next forward boundary applies them (measuring hasn't started: nothing else happens).
  Orca::Perf::Boundary boundary;
  boundary.first_run = true;
  Orca::Perf::OnBoundary(boundary);
  EXPECT_EQ(Config::Get(Config::GFX_EFB_SCALE), 4);
  EXPECT_FALSE(Config::Get(Config::GFX_HACK_COPY_EFB_SCALED));
  EXPECT_EQ(Verify(), std::nullopt);
}

TEST_F(OrcaGraphicsSettings, TheShippedProfilesForceNoLiveKey)
{
  // The `gfx` map writes above every forced setting, the profiles' own included.
  const std::string sys =
      (std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/").string();
  for (const auto& [id, revision] : {std::pair<const char*, std::optional<u16>>{"RSBE01", 2},
                                     {"PPLUS32", std::nullopt}})
  {
    std::string error;
    const std::optional<Orca::Profile> profile = Orca::LoadProfileFrom(sys, id, revision, &error);
    ASSERT_TRUE(profile) << id << ": " << error;
    for (const auto& [location, value] : profile->forced)
    {
      for (const Config::Location& live : Orca::Perf::GfxLocations())
      {
        EXPECT_FALSE(location == live)
            << id << " forces " << location.section << "." << location.key;
      }
    }
  }
}

TEST_F(OrcaGraphicsSettings, ThePerformancePresetIsRecognized)
{
  CommandLine(Config::GFX_EFB_SCALE, 3);
  CommandLine(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropicFilteringMode::Force8x);
  Boot();
  EXPECT_FALSE(Orca::Perf::PerformanceNow());
  for (const Orca::Perf::GfxChange& change : Orca::Perf::PerformancePreset())
    Orca::Perf::ApplyGfx(change);
  EXPECT_TRUE(Orca::Perf::PerformanceNow());
  // Anti-aliasing is a next-start setting, but it still isn't Performance: the picture is.
  CommandLine(Config::GFX_MSAA, 4u);
  EXPECT_FALSE(Orca::Perf::PerformanceNow());
  EXPECT_TRUE(Orca::Perf::PerformancePicture());
  // A player who raised the resolution again is on their own settings.
  Orca::Perf::ApplyGfx({Orca::Perf::GfxKey::Res, 3});
  EXPECT_FALSE(Orca::Perf::PerformancePicture());
}

TEST_F(OrcaGraphicsSettings, TheForcedListHoldsTheNewKeys)
{
  const std::vector<Config::Location> forced = Orca::UniversalForcedLocations();
  std::vector<Config::Location> added = {
      Config::MAIN_GDB_PORT.GetLocation(),
      Config::MAIN_GDB_SOCKET.GetLocation(),
      Config::MAIN_ENABLE_DEBUGGING.GetLocation(),
      Config::MAIN_DEBUG_JIT_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_LOAD_STORE_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_FLOATING_POINT_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_INTEGER_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_PAIRED_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_BRANCH_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF.GetLocation(),
      Config::MAIN_DEBUG_JIT_ENABLE_PROFILING.GetLocation(),
      Config::MAIN_LOW_DCBZ_HACK.GetLocation(),
      Config::MAIN_PAGE_TABLE_FASTMEM.GetLocation(),
      Config::MAIN_FASTMEM_ARENA.GetLocation(),
      Config::MAIN_LARGE_ENTRY_POINTS_MAP.GetLocation(),
      Config::MAIN_OVERRIDE_BOOT_IOS.GetLocation(),
      Config::MAIN_HSP_DEVICE.GetLocation(),
      Config::MAIN_EMULATION_SPEED.GetLocation(),
      Config::MAIN_RUSH_FRAME_PRESENTATION.GetLocation(),
      Config::MAIN_MAX_FALLBACK.GetLocation(),
      Config::MAIN_TIMING_VARIANCE.GetLocation(),
      Config::MAIN_CORRECT_TIME_DRIFT.GetLocation(),
      Config::GFX_HACK_CAP_IMMEDIATE_XFB.GetLocation(),
      Config::GFX_WIDESCREEN_HACK.GetLocation(),
      Config::GFX_DISABLE_FOG.GetLocation(),
      Config::GFX_ENABLE_WIREFRAME.GetLocation(),
      Config::GFX_TEXFMT_OVERLAY_ENABLE.GetLocation(),
      Config::FREE_LOOK_ENABLED.GetLocation(),
      // From the review: a game-affecting JIT switch, a pacing setting, and the host-config bit
      // that would make a resolution change reload every shader.
      Config::MAIN_PAUSE_ON_PANIC.GetLocation(),
      Config::MAIN_AUDIO_BUFFER_SIZE.GetLocation(),
      Config::GFX_HACK_VERTEX_ROUNDING.GetLocation(),
  };
  EXPECT_EQ(added.size(), 37u);
  for (const Config::Location& location : added)
    EXPECT_TRUE(Has(forced, location)) << location.section << "." << location.key;
  // The player's settings are not on the list (WaitForShadersBeforeStarting stays forced).
  for (const Config::Location& location :
       {Config::MAIN_GFX_BACKEND.GetLocation(), Config::GFX_ADAPTER.GetLocation(),
        Config::GFX_EFB_SCALE.GetLocation(), Config::GFX_MAX_EFB_SCALE.GetLocation(),
        Config::GFX_MSAA.GetLocation(), Config::GFX_SSAA.GetLocation(),
        Config::GFX_ENHANCE_MAX_ANISOTROPY.GetLocation(),
        Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING.GetLocation(),
        Config::GFX_ENHANCE_OUTPUT_RESAMPLING.GetLocation(),
        Config::GFX_HACK_COPY_EFB_SCALED.GetLocation(),
        Config::GFX_SHADER_COMPILATION_MODE.GetLocation(),
        Config::GFX_SHADER_COMPILER_THREADS.GetLocation(),
        Config::GFX_ENABLE_GPU_TEXTURE_DECODING.GetLocation(), Config::GFX_CPU_CULL.GetLocation(),
        Config::GFX_BACKEND_MULTITHREADING.GetLocation(), Config::GFX_SHOW_FPS.GetLocation()})
  {
    EXPECT_FALSE(Has(forced, location)) << location.section << "." << location.key;
  }
  EXPECT_TRUE(Has(forced, Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING.GetLocation()));
  EXPECT_TRUE(Has(forced, Config::GFX_VSYNC.GetLocation()));
}
