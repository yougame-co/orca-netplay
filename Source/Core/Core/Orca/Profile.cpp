// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Profile.h"

#include "Core/Orca/Status.h"
#include "Core/Orca/Session/Online.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>

#include <cctype>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/CPUDetect.h"
#include "Common/CommonFuncs.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#ifdef USE_RETRO_ACHIEVEMENTS
#include "Core/Config/AchievementSettings.h"
#endif
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/HSP/HSP_Device.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Sram.h"
#include "Core/HW/Wiimote.h"
#include "Core/Orca/BootAudit.h"
#include "Core/Orca/BootPhase.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/Session/Session.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "VideoCommon/VideoConfig.h"

namespace Orca
{
namespace
{
bool EnvIs(const char* name, const char* value)
{
  return GetEnv(name) == value;
}

// Decimal, or hex with a 0x prefix (a leading zero is still decimal, not octal).
bool ParseNumber(const std::string& text, u32* out)
{
  if (text.starts_with("0x") || text.starts_with("0X"))
    return TryParse(text, out, 16);
  return TryParse(text, out, 10);
}

// Every setting an Orca session forces, whatever the game. ForcedConfigLoader applies these and
// VerifyForcedSettings checks them, so the two lists can't drift apart.
template <typename F>
void ForEachUniversalSetting(F&& f)
{
  // CPU and timing. Each host uses its own JIT (JitArm64 on Mac, Jit64 on PC); both compute the
  // same game (see MAIN_ACCURATE_NANS below and Online.cpp CompatibilityKey).
  f(Config::MAIN_CPU_THREAD, false);
  f(Config::MAIN_CPU_CORE, TestCpuCore().value_or(PowerPC::DefaultCPUCore()));
  f(Config::MAIN_OVERCLOCK_ENABLE, false);
  f(Config::MAIN_OVERCLOCK, 1.0f);
  f(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
  f(Config::MAIN_MMU, false);
  f(Config::MAIN_FASTMEM, Config::MAIN_FASTMEM.GetDefaultValue());
  // PowerPC NaN rules on every host: an input NaN is picked in PowerPC operand order and quieted,
  // and a NaN produced from non-NaNs (inf * 0, inf - inf, 0 / 0, frsqrte of -1) is the positive
  // default 0x7FF8000000000000. Host rules differ (x86-64 and ARM with FEAT_AFP make it negative,
  // ARM without it positive), and Project+ produces such a NaN when a match starts. The accurate
  // paths cost one never-taken branch per arithmetic op unless a NaN comes out.
  f(Config::MAIN_ACCURATE_NANS, true);
  f(Config::MAIN_ACCURATE_FMADDS, Config::MAIN_ACCURATE_FMADDS.GetDefaultValue());
  f(Config::MAIN_FPRF, false);
  f(Config::MAIN_FLOAT_EXCEPTIONS, false);
  f(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS, false);
  f(Config::MAIN_DISABLE_ICACHE, false);
  f(Config::MAIN_ACCURATE_CPU_CACHE, false);
  f(Config::MAIN_JIT_FOLLOW_BRANCH, Config::MAIN_JIT_FOLLOW_BRANCH.GetDefaultValue());
  f(Config::MAIN_SYNC_ON_SKIP_IDLE, Config::MAIN_SYNC_ON_SKIP_IDLE.GetDefaultValue());
  // Fused multiply-add changes float results. ARM64 always has it; on x86-64 it depends on the
  // host, so it is part of the lobby compatibility key.
  f(Config::SESSION_USE_FMA, cpu_info.bFMA);
  f(Config::MAIN_RAM_OVERRIDE_ENABLE, false);
  // Switching to the YouGame app (to invite a friend) must not pause the match.
  f(Config::MAIN_PAUSE_ON_FOCUS_LOST, false);
  // Orca talks to YouGame only: no usage reports to Dolphin's analytics server.
  f(Config::MAIN_ANALYTICS_ENABLED, false);
  f(Config::MAIN_CUSTOM_RTC_ENABLE, true);
  // A pinned clock, the same for both players. Not Dolphin's default (2000-01-01, the GameCube
  // epoch): there the timebase reads about -2^32 at boot, and an SI alarm queued for a small
  // absolute time waits ~70 s for it to climb past 0, leaving Brawl's pads in PADReset. 2025-01-01
  // UTC.
  f(Config::MAIN_CUSTOM_RTC_VALUE, 1735689600u);
  f(Config::MAIN_DSP_HLE, true);
  f(Config::MAIN_FAST_DISC_SPEED, false);
  f(Config::MAIN_SKIP_IPL, true);
  f(Config::SESSION_LOAD_IPL_DUMP, false);
  f(Config::MAIN_ENABLE_CHEATS, false);
  // No SD card unless the profile inserts one; a profile's card is a verified read-only image
  // (writes would outlive a rollback and differ between players), never rebuilt from a folder.
  f(Config::MAIN_WII_SD_CARD, false);
  f(Config::MAIN_ALLOW_SD_WRITES, false);
  f(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC, false);
  f(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, false);
  // No USB keyboard and no real USB device handed to the guest. The session's device budget
  // (IOS::HLE::SessionAllowsDevice) keeps the USB devices closed anyway; these keep a player's
  // Dolphin.ini from asking for them.
  f(Config::MAIN_WII_KEYBOARD, false);
  f(Config::MAIN_USB_PASSTHROUGH_DEVICES, std::string());
  f(Config::MAIN_OVERRIDE_REGION_SETTINGS, true);
  // The console region a DOL boots as (the Project+ launcher). Dolphin otherwise takes it from the
  // host locale, so two machines could boot different regions and diverge from the first frame.
  // Orca's games are NTSC-U discs; a disc boot uses the disc's region and never reads this.
  f(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_U);
  f(Config::MAIN_GC_LANGUAGE, 0);  // English; written into the session SRAM
  f(Config::MAIN_WII_WIILINK_ENABLE, false);
  f(Config::MAIN_EMULATE_SKYLANDER_PORTAL, false);
  f(Config::MAIN_EMULATE_INFINITY_BASE, false);
  f(Config::MAIN_EMULATE_WII_SPEAK, false);
  for (const auto& mic : Config::MAIN_EMULATE_LOGITECH_MIC)
    f(mic, false);
  // No memory cards or other EXI devices (netplay forces these too).
  for (const auto slot : {ExpansionInterface::Slot::A, ExpansionInterface::Slot::B,
                          ExpansionInterface::Slot::SP1})
  {
    f(Config::GetInfoForEXIDevice(slot), ExpansionInterface::EXIDeviceType::None);
  }
#ifdef USE_RETRO_ACHIEVEMENTS
  // Hardcore mode refuses state loads, which rollback needs.
  f(Config::RA_HARDCORE_ENABLED, false);
#endif

  // Wii system settings: the same on both machines rather than each user's.
  f(Config::SYSCONF_LANGUAGE, 1u);  // English
  f(Config::SYSCONF_COUNTRY, 49u);  // USA
  f(Config::SYSCONF_WIDESCREEN, true);
  f(Config::SYSCONF_PROGRESSIVE_SCAN, true);
  f(Config::SYSCONF_PAL60, false);  // BootCore forces it off for NTSC Wii games anyway
  f(Config::SYSCONF_SOUND_MODE, 1u);

  // Controllers: GameCube pads only. 1v1 by default; four ports in online (drop-in) play, where an
  // empty port reports no controller (Orca::Net::UNPLUGGED_PAD) until a friend plugs in.
  const bool four = TestPads() == 4 || Online::Enabled();
  f(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);
  f(Config::GetInfoForSIDevice(1), SerialInterface::SIDEVICE_GC_CONTROLLER);
  f(Config::GetInfoForSIDevice(2),
    four ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE);
  f(Config::GetInfoForSIDevice(3),
    four ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE);
  for (int i = 0; i < 5; ++i)
    f(Config::GetInfoForWiimoteSource(i), WiimoteSource::None);

  // Graphics: EFB copies stay on the GPU, so RAM only ever gets a constant placeholder, never
  // pixels; no CPU readbacks of GPU results; no graphics mods (they can skip copies).
  f(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, true);
  f(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, true);
  f(Config::GFX_HACK_DISABLE_COPY_TO_VRAM, false);
  f(Config::GFX_HACK_DEFER_EFB_COPIES, true);
  // Latency: present each frame at its XFB copy instead of waiting for the VI field that would scan
  // it out (about 20 ms sooner), and after a rollback show the corrected frame. Host-side only:
  // ImmediateSwap is skipped on re-runs, the VI throttle it bypasses only sleeps, and SyncGPU does
  // nothing in single core. Brawl and P+ make one XFB copy per frame, so CapImmediateXFB stays off.
  // See ORCA.md, "Input latency".
  f(Config::GFX_HACK_IMMEDIATE_XFB, true);
  // Smooth Early Presentation: Immediate XFB frames are shown at a steady offset from their VI time
  // that covers the usual rollback re-run (VideoCommon/PresentPacing.h), so with the v-sync move's
  // early copy a re-run doesn't move the picture. Host-side only.
  f(Config::MAIN_SMOOTH_EARLY_PRESENTATION, true);
  // Those presents wait until a set time: sleep to it precisely (Dolphin's default). Host-only.
  f(Config::MAIN_PRECISION_FRAME_TIMING, true);
  // VSync adds 2-19 ms of display latency with no pacing gain (a composited window never tears)
  // and, below 60 Hz, would slow the game: always off in a session, whatever GFX.ini says.
  f(Config::GFX_VSYNC, false);
  // The JIT vertex loaders: the session's vertex bounds (VertexLoaderManager::IndexedReadsFit)
  // assume what they read, and the software loader reads a skipped vertex's position too.
  f(Config::GFX_VERTEX_LOADER_TYPE, VertexLoaderType::Native);
  f(Config::GFX_HACK_EFB_ACCESS_ENABLE, false);
  f(Config::GFX_HACK_BBOX_ENABLE, false);
  f(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, false);
  f(Config::GFX_PERF_QUERIES_ENABLE, false);
  f(Config::GFX_MODS_ENABLE, false);
  // Texture replacement, for Orca's own pack (the Online menu's labels, Data/Sys/Orca/Textures),
  // loaded alongside the player's own. It only changes what is drawn: the cache hashes RAM, and
  // nothing drawn reaches RAM. Prefetching stays the player's choice, since forcing it would load a
  // player's whole pack into memory; Orca's few labels are always preloaded.
  f(Config::GFX_HIRES_TEXTURES, true);
  // Shaders: every pipeline seen before (Cache/<game>.uidcache), the ubershaders and the EFB copy
  // pipelines compile before the first frame. The compilation mode and thread count are the
  // player's (the app's Graphics settings), within what a session allows: ForcedConfigLoader
  // refuses the modes that compile on the emulation thread. See ORCA.md, "Match start" and
  // "Graphics".
  f(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);

  // Every other graphics setting is the player's: it only changes what this machine draws and how
  // fast (ORCA.md, "Graphics"). The settings below are Dolphin's defaults, pinned so a player's
  // Dolphin.ini or GFX.ini can't change them.
  //
  // Security: no GDB stub. It would serve the game's memory and registers to anyone who connects.
  f(Config::MAIN_GDB_PORT, -1);
  f(Config::MAIN_GDB_SOCKET, std::string());
  // Debugging and the JIT: each of these routes code through the interpreter or changes how the JIT
  // analyses it, a different machine from the one the other player runs.
  f(Config::MAIN_ENABLE_DEBUGGING, false);
  for (const Config::Info<bool>* jit_off :
       {&Config::MAIN_DEBUG_JIT_OFF, &Config::MAIN_DEBUG_JIT_LOAD_STORE_OFF,
        &Config::MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF, &Config::MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF,
        &Config::MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF,
        &Config::MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF,
        &Config::MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF, &Config::MAIN_DEBUG_JIT_FLOATING_POINT_OFF,
        &Config::MAIN_DEBUG_JIT_INTEGER_OFF, &Config::MAIN_DEBUG_JIT_PAIRED_OFF,
        &Config::MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF, &Config::MAIN_DEBUG_JIT_BRANCH_OFF,
        &Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF, &Config::MAIN_DEBUG_JIT_ENABLE_PROFILING})
  {
    f(*jit_off, false);
  }
  // PauseOnPanic turns on the JIT's memory checks (jo.memcheck): an unresolved access then breaks
  // into a DSI instead of reading 0 and going on, so this player's game would leave the other's.
  f(Config::MAIN_PAUSE_ON_PANIC, false);
  f(Config::MAIN_LOW_DCBZ_HACK, false);
  f(Config::MAIN_PAGE_TABLE_FASTMEM, Config::MAIN_PAGE_TABLE_FASTMEM.GetDefaultValue());
  f(Config::MAIN_FASTMEM_ARENA, Config::MAIN_FASTMEM_ARENA.GetDefaultValue());
  f(Config::MAIN_LARGE_ENTRY_POINTS_MAP, Config::MAIN_LARGE_ENTRY_POINTS_MAP.GetDefaultValue());
  f(Config::MAIN_OVERRIDE_BOOT_IOS, -1);
  f(Config::MAIN_HSP_DEVICE, HSP::HSPDeviceType::None);
  // Pacing: full speed, Dolphin's throttle. A slower speed would stall the other player every
  // frame, and the rest interact with Orca's presentation pacing and rollback re-runs.
  f(Config::MAIN_EMULATION_SPEED, 1.0f);
  f(Config::MAIN_RUSH_FRAME_PRESENTATION, Config::MAIN_RUSH_FRAME_PRESENTATION.GetDefaultValue());
  f(Config::MAIN_MAX_FALLBACK, Config::MAIN_MAX_FALLBACK.GetDefaultValue());
  f(Config::MAIN_TIMING_VARIANCE, Config::MAIN_TIMING_VARIANCE.GetDefaultValue());
  f(Config::MAIN_CORRECT_TIME_DRIFT, Config::MAIN_CORRECT_TIME_DRIFT.GetDefaultValue());
  f(Config::GFX_HACK_CAP_IMMEDIATE_XFB, false);
  // The audio buffer bounds how far the throttle may fall behind before it skips ahead
  // (CoreTiming's max fallback); kept at Dolphin's 80 ms like the other pacing settings.
  f(Config::MAIN_AUDIO_BUFFER_SIZE, Config::MAIN_AUDIO_BUFFER_SIZE.GetDefaultValue());
  // Drawing-only, but it is a shader host-config bit that flips whenever the internal resolution
  // goes to or from native (VideoConfig::UseVertexRounding). With it on, a live `gfx res` or the
  // switch to Performance would reload every shader on the CPU thread mid-match, with no time
  // limit, and freeze both players.
  f(Config::GFX_HACK_VERTEX_ROUNDING, false);
  // Fairness: drawing-only, but each shows a player more than the game does (the stage past its
  // framing, through fog, through cover) or a free camera.
  f(Config::GFX_WIDESCREEN_HACK, false);
  f(Config::GFX_DISABLE_FOG, false);
  f(Config::GFX_ENABLE_WIREFRAME, false);
  f(Config::GFX_TEXFMT_OVERLAY_ENABLE, false);
  f(Config::FREE_LOOK_ENABLED, false);
}

// The value a setting has from the layers under the forced one, as if Orca forced nothing: the
// first of them that sets it, as Config::Get reads it; nullopt when none sets it.
template <typename T>
std::optional<T> BelowForced(const Config::Info<T>& info)
{
  for (const Config::LayerType type :
       {Config::LayerType::Movie, Config::LayerType::LocalGame, Config::LayerType::GlobalGame,
        Config::LayerType::CommandLine, Config::LayerType::Base})
  {
    const std::shared_ptr<Config::Layer> layer = Config::GetLayer(type);
    if (!layer)
      continue;
    if (const std::optional<std::string> text = layer->Get<std::string>(info.GetLocation()))
      return Config::detail::TryParse<T>(*text).value_or(info.GetDefaultValue());
  }
  return std::nullopt;
}

class ForcedConfigLoader final : public Config::ConfigLayerLoader
{
public:
  // Reads the layers under this one now, before it is added (Load runs under the config lock).
  explicit ForcedConfigLoader(Profile profile)
      : ConfigLayerLoader(Config::LayerType::Netplay), m_profile(std::move(profile))
  {
    // The player's shader mode and compile threads, within what a session allows, pinned for the
    // run (a launcher's disc brings its own game INI layers in later). Synchronous modes compile
    // on the emulation thread (Synchronous at a pipeline's first draw, which freezes the other
    // player too; SyncUber never compiles specialized shaders, and is heavy on the graphics card),
    // and so do zero compile threads. Set by nobody, they are what sessions forced before the
    // player could choose: AsyncUber (Dolphin's default is Synchronous) and Dolphin's Auto thread
    // count (cores - 3, 1 to 4; Dolphin's default is one thread).
    const std::optional<ShaderCompilationMode> mode =
        BelowForced(Config::GFX_SHADER_COMPILATION_MODE);
    m_shader_mode = mode && SessionShaderModeAllowed(*mode) ?
                        *mode :
                        ShaderCompilationMode::AsynchronousUberShaders;
    const std::optional<int> threads = BelowForced(Config::GFX_SHADER_COMPILER_THREADS);
    m_shader_threads = threads && SessionShaderThreadsAllowed(*threads) ? *threads : -1;
  }

  void Load(Config::Layer* layer) override
  {
    ForEachUniversalSetting([layer](const auto& info, auto value) { layer->Set(info, value); });
    layer->Set(Config::GFX_SHADER_COMPILATION_MODE, m_shader_mode);
    layer->Set(Config::GFX_SHADER_COMPILER_THREADS, m_shader_threads);
    for (const auto& [location, value] : m_profile.forced)
      layer->Set(location, value);
    // A launcher profile's verified SD image overrides any card path the player set.
    if (!m_profile.sd_image_path.empty())
      layer->Set(Config::MAIN_WII_SD_CARD_IMAGE_PATH, m_profile.sd_image_path);
  }

  void Save(Config::Layer*) override {}

private:
  Profile m_profile;
  ShaderCompilationMode m_shader_mode = ShaderCompilationMode::AsynchronousUberShaders;
  int m_shader_threads = -1;
};
}  // namespace

std::string GetEnv(const char* name)
{
#ifdef _WIN32
  const wchar_t* value = _wgetenv(UTF8ToWString(name).c_str());
  return value ? WStringToUTF8(value) : std::string();
#else
  const char* value = std::getenv(name);
  return value ? value : std::string();
#endif
}

bool SessionActive()
{
  return Mod::LocalPackRequested() || EnvIs("ORCA_SESSION", "1");
}

bool HarnessInputActive()
{
  return !GetEnv("YG_INPUT").empty() || EnvIs("ORCA_TEST_COMMANDS", "1");
}

std::string HarnessReport(std::string json)
{
  if (HarnessInputActive() && !json.empty() && json.back() == '}')
    json.insert(json.size() - 1, ",\"harness\":true");
  return json;
}

int TestPads()
{
  return SessionActive() && EnvIs("ORCA_TEST_PADS", "4") ? 4 : 0;
}

bool TestSkipRender()
{
  return SessionActive() && EnvIs("ORCA_TEST_SKIP_RENDER", "1");
}

std::optional<int> TestNetDelayMs()
{
  const std::string value = GetEnv("ORCA_TEST_NET_DELAY_MS");
  if (!SessionActive())
    return std::nullopt;
  if (value.empty())
  {
    return TestNetSpikesConfig() || TestNetUplinkConfig() ? std::optional<int>(0) :
                                                            std::nullopt;
  }
  return std::clamp(std::atoi(value.c_str()), 0, 1000);
}

std::optional<TestBlackout> TestBlackoutConfig()
{
  if (!SessionActive())
    return std::nullopt;
  const std::string value = GetEnv("ORCA_TEST_BLACKOUT");
  const auto dash = value.find('-');
  if (dash == std::string::npos)
    return std::nullopt;
  const auto number = [](std::string_view text, int* out) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && *out >= 0;
  };
  TestBlackout blackout;
  if (!number(std::string_view(value).substr(0, dash), &blackout.from_s) ||
      !number(std::string_view(value).substr(dash + 1), &blackout.to_s) ||
      blackout.to_s <= blackout.from_s)
    return std::nullopt;
  const std::string before = GetEnv("ORCA_TEST_BLACKOUT_ROOM_AFTER");
  if (!before.empty() && (!number(before, &blackout.room_before_s) ||
                          blackout.room_before_s > blackout.to_s - blackout.from_s))
    return std::nullopt;
  return blackout;
}

std::optional<TestNetSpikes> TestNetSpikesConfig()
{
  const std::string value = GetEnv("ORCA_TEST_NET_SPIKES");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  TestNetSpikes spikes;
  int fields;
  if (value[0] == '~')
  {
    // Random gaps around a mean (a Poisson process), not between two bounds.
    fields = 1 + std::sscanf(value.c_str(), "~%d/%d-%d/%d-%d", &spikes.gap_min_ms,
                             &spikes.length_min_ms, &spikes.length_max_ms, &spikes.from_s,
                             &spikes.to_s);
    spikes.gap_max_ms = spikes.gap_min_ms;
    spikes.poisson = true;
  }
  else
  {
    fields = std::sscanf(value.c_str(), "%d-%d/%d-%d/%d-%d", &spikes.gap_min_ms,
                         &spikes.gap_max_ms, &spikes.length_min_ms, &spikes.length_max_ms,
                         &spikes.from_s, &spikes.to_s);
  }
  if ((fields != 4 && fields != 6) || spikes.gap_min_ms < 1 ||
      spikes.gap_max_ms < spikes.gap_min_ms || spikes.length_min_ms < 0 ||
      spikes.length_max_ms < spikes.length_min_ms || spikes.length_max_ms > 5000 ||
      spikes.to_s < spikes.from_s)
  {
    return std::nullopt;
  }
  return spikes;
}

std::optional<TestNetUplink> TestNetUplinkConfig()
{
  const std::string value = GetEnv("ORCA_TEST_NET_UPLINK");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  TestNetUplink uplink;
  const int fields = std::sscanf(value.c_str(), "%d+%d/%d-%d", &uplink.delay_ms, &uplink.jitter_ms,
                                 &uplink.from_s, &uplink.to_s);
  if ((fields != 2 && fields != 4) || uplink.delay_ms < 0 || uplink.delay_ms > 1000 ||
      uplink.jitter_ms < 0 || uplink.jitter_ms > 1000 || uplink.to_s < uplink.from_s)
  {
    return std::nullopt;
  }
  return uplink;
}

int TestNetJitterMs()
{
  const std::string value = GetEnv("ORCA_TEST_NET_JITTER_MS");
  return TestNetDelayMs() && !value.empty() ? std::clamp(std::atoi(value.c_str()), 0, 1000) : 0;
}

int TestSnapshotEvery()
{
  const std::string value = GetEnv("ORCA_TEST_SNAPSHOT_EVERY");
  if (!SessionActive() || value.empty())
    return 0;
  return std::clamp(std::atoi(value.c_str()), 1, Net::MAX_SNAPSHOT_EVERY);
}

bool TestFullSnapshots()
{
  return SessionActive() && EnvIs("ORCA_TEST_FULL_SNAPSHOTS", "1");
}

std::optional<PowerPC::CPUCore> TestCpuCore()
{
  const std::string value = GetEnv("ORCA_TEST_CPUCORE");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  switch (std::atoi(value.c_str()))
  {
  case static_cast<int>(PowerPC::CPUCore::Interpreter):
    return PowerPC::CPUCore::Interpreter;
  case static_cast<int>(PowerPC::CPUCore::CachedInterpreter):
    return PowerPC::CPUCore::CachedInterpreter;
  default:
    return std::nullopt;
  }
}

bool TestNoAfp()
{
  // Read at CPU detection (Common/ArmCPUDetect.cpp), which clears cpu_info.bAFP. Only ARM builds
  // have anything to clear, but the key counts it everywhere.
  return EnvIs("ORCA_TEST_NO_AFP", "1");
}

std::optional<int> TestShaderWaitMs()
{
  const std::string value = GetEnv("ORCA_TEST_SHADER_WAIT_MS");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  return std::clamp(std::atoi(value.c_str()), 0, 600'000);
}

std::string TestGateSpec()
{
  return SessionActive() ? GetEnv("ORCA_TEST_GATE") : std::string();
}

std::string TestQueueSpec()
{
  return SessionActive() ? GetEnv("ORCA_TEST_QUEUE") : std::string();
}

std::string TestQueuePickSpec()
{
  return SessionActive() ? GetEnv("ORCA_TEST_QUEUE_PICK") : std::string();
}

std::string TestCharOrderSpec()
{
  return SessionActive() ? GetEnv("ORCA_TEST_CHAR_ORDER") : std::string();
}

std::optional<int> ShaderWaitLimitS()
{
  const std::string value = GetEnv("ORCA_SHADER_WAIT_S");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  return std::clamp(std::atoi(value.c_str()), 60, 900);
}

std::optional<u32> TestFxcFlags()
{
  const std::string value = GetEnv("ORCA_TEST_FXC_FLAGS");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  return static_cast<u32>(std::strtoul(value.c_str(), nullptr, 16));
}

bool TestOverridesActive()
{
  return TestPads() != 0 || TestSkipRender() || TestNetDelayMs().has_value() ||
         TestBlackoutConfig().has_value() ||
         TestSnapshotEvery() != 0 || TestFullSnapshots() || TestNoAfp() ||
         TestCpuCore().has_value() || !TestGateSpec().empty() || !TestQueueSpec().empty() ||
         !TestCharOrderSpec().empty() || !TestQueuePickSpec().empty();
}

const Sram* SessionSram()
{
  static const Sram sram = [] {
    Sram s;
    InitSRAM(&s, "");  // no file: Dolphin's built-in default
    return s;
  }();
  return &sram;
}

namespace
{
u64 s_seed_save_hash = 0;
bool s_seed_session_save = true;
std::optional<Profile> s_active_profile;
// First-run frame boundaries since boot, for jit_clear_frame.
u32 s_boot_frames = 0;
// The active profile's kept_code by physical address (MEM1: the low 29 bits of the effective
// address, so cached and uncached mirrors match). Set at boot, before the CPU thread runs.
std::vector<Profile::KeptCode> s_kept_code;
}  // namespace

void SetActiveProfile(std::optional<Profile> profile)
{
  if (s_active_profile && s_active_profile->boot_audit &&
      (!profile || profile->boot_audit != s_active_profile->boot_audit))
    s_active_profile->boot_audit->Cancel();
  s_active_profile = std::move(profile);
  s_boot_frames = 0;
  Mod::ResetGuards();
  Status::ResetOnline();
  s_kept_code.clear();
  if (s_active_profile)
  {
    for (Profile::KeptCode kept : s_active_profile->kept_code)
    {
      kept.address &= 0x1FFFFFFF;
      s_kept_code.push_back(kept);
    }
  }
}

u32 KeptInstruction(u32 physical_address, u32 hex)
{
  for (const Profile::KeptCode& kept : s_kept_code)
  {
    if (kept.address == physical_address && kept.loader == hex)
      return kept.game;
  }
  return hex;
}

bool JitClearDue()
{
  if (!s_active_profile || !s_active_profile->jit_clear_frame ||
      s_boot_frames > *s_active_profile->jit_clear_frame)
  {
    return false;
  }
  return s_boot_frames++ == *s_active_profile->jit_clear_frame;
}

const Profile* ActiveProfile()
{
  return s_active_profile ? &*s_active_profile : nullptr;
}

u64 HashSessionSave(u64 title_id)
{
  const std::string dir = File::GetUserPath(D_SESSION_WIIROOT_IDX) +
                          fmt::format("/title/{:08x}/{:08x}/data/", static_cast<u32>(title_id >> 32),
                                      static_cast<u32>(title_id));
  if (!File::IsDirectory(dir))
    return 0;
  // Files in name order, each hashed with its name.
  File::FSTEntry tree = File::ScanDirectoryTree(dir, true);
  // Names relative to the root as ScanDirectoryTree spells it (on Windows it rebuilds paths in
  // generic form), so every platform hashes the same names.
  std::string root = tree.physicalName;
  if (!root.ends_with('/'))
    root += '/';
  std::vector<std::string> paths;
  const std::function<void(const File::FSTEntry&)> collect = [&](const File::FSTEntry& e) {
    if (!e.isDirectory)
      paths.push_back(e.physicalName);
    for (const auto& child : e.children)
      collect(child);
  };
  collect(tree);
  // ES creates the title's data folder at boot, so an empty one also means no save.
  if (paths.empty())
    return 0;
  std::sort(paths.begin(), paths.end());
  u64 hash = 0xcbf29ce484222325ull;
  for (const std::string& path : paths)
  {
    std::string data;
    File::ReadFileToString(path, data);
    const std::string name = path.starts_with(root) ? path.substr(root.size()) : path;
    hash = (hash ^ Common::GetHash64(reinterpret_cast<const u8*>(name.data()),
                                     static_cast<u32>(name.size()), 0)) *
           0x100000001b3ull;
    hash = (hash ^ Common::GetHash64(reinterpret_cast<const u8*>(data.data()),
                                     static_cast<u32>(data.size()), 0)) *
           0x100000001b3ull;
  }
  return hash == 0 ? 1 : hash;
}

void SetSeedSaveHash(u64 hash)
{
  s_seed_save_hash = hash;
}

u64 GetSeedSaveHash()
{
  return s_seed_save_hash;
}

void SetSeedSessionSave(bool seed)
{
  s_seed_session_save = seed;
}

bool SeedSessionSave()
{
  return s_seed_session_save;
}

std::optional<Profile> LoadProfile(const std::string& game_id, std::optional<u16> revision,
                                   std::string* error)
{
  return LoadProfileFrom(File::GetSysDirectory(), game_id, revision, error);
}

std::optional<Profile> LoadProfileFrom(const std::string& sys_directory, const std::string& game_id,
                                       std::optional<u16> revision, std::string* error)
{
  const std::string path = sys_directory + "Orca/" + game_id + ".ini";
  // OpenFStream, because the path is UTF-8 and std::ifstream would read it in the ANSI code page on
  // Windows.
  std::ifstream in;
  File::OpenFStream(in, path, std::ios_base::in);
  if (!in)
  {
    *error = fmt::format("YG Dolphin has no profile for {} ({})", game_id, path);
    return std::nullopt;
  }

  Profile profile;
  profile.game_id = game_id;
  std::optional<u16> profile_revision;
  std::string section, line;
  int line_number = 0;
  const auto fail = [&](std::string_view why) {
    *error = fmt::format("Orca profile {} line {}: {}", path, line_number, why);
    return std::nullopt;
  };
  while (std::getline(in, line))
  {
    ++line_number;
    // Comments start a line or follow whitespace.
    if (const size_t hash = line.find('#');
        hash == 0 || (hash != std::string::npos && (line[hash - 1] == ' ' || line[hash - 1] == '\t')))
    {
      line.resize(hash);
    }
    line = std::string(StripWhitespace(line));
    if (line.empty())
      continue;
    if (line.front() == '[' && line.back() == ']')
    {
      section = line.substr(1, line.size() - 2);
      if (section != "Profile" && section != "Forced")
        return fail(fmt::format("unknown section [{}]", section));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      return fail("expected <key> = <value>");
    const std::string key(StripWhitespace(line.substr(0, eq)));
    const std::string value(StripWhitespace(line.substr(eq + 1)));
    if (key.empty() || value.empty())
      return fail("empty key or value");

    if (section == "Profile" && (key == "Launcher" || key == "SDImageHash"))
    {
      if (value.size() != 32 ||
          !std::ranges::all_of(value, [](unsigned char c) {
            return std::isxdigit(c) && !std::isupper(c);
          }))
      {
        return fail(fmt::format("{} must be 32 lowercase hex digits (XXH3-128)", key));
      }
      (key == "Launcher" ? profile.launcher_hash : profile.sd_image_hash) = value;
    }
    else if (section == "Profile" && (key == "Disc" || key == "SDImage"))
    {
      // A file name next to the launcher, never a path out of its folder.
      if (key == "SDImage" && (value.find_first_of("/\\") != std::string::npos || value == ".."))
        return fail("SDImage must be a file name");
      (key == "Disc" ? profile.disc : profile.sd_image) = value;
    }
    else if (section == "Profile" && key == "Title")
    {
      // Shown in the window title: printable ASCII only, and short.
      if (value.size() > 48 ||
          !std::ranges::all_of(value, [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
      {
        return fail("Title must be at most 48 printable ASCII characters");
      }
      profile.title = value;
    }
    else if (section == "Profile" && key == "SaveTitle")
    {
      u64 title = 0;
      if (!value.starts_with("0x") || !TryParse(value.substr(2), &title, 16))
        return fail("SaveTitle must be a hex title ID (0x...)");
      profile.save_title = title;
    }
    else if (section == "Profile" && key == "KeepGameCode")
    {
      // <address> <game's word> <loader's word>, in hex.
      std::istringstream words(value);
      std::string address, game, loader, extra;
      Profile::KeptCode kept;
      if (!(words >> address >> game >> loader) || (words >> extra) ||
          !ParseNumber(address, &kept.address) || !ParseNumber(game, &kept.game) ||
          !ParseNumber(loader, &kept.loader))
      {
        return fail("KeepGameCode must be <address> <game's word> <loader's word>");
      }
      if (kept.address < 0x80000000 || kept.address > 0x817FFFFC || kept.address % 4 != 0)
        return fail("KeepGameCode's address must be a word in MEM1 (0x80000000-0x817FFFFC)");
      if (kept.game == kept.loader)
        return fail("KeepGameCode's two words are the same");
      if (std::ranges::any_of(profile.kept_code, [&](const Profile::KeptCode& k) {
            return k.address == kept.address;
          }))
      {
        return fail(fmt::format("KeepGameCode {:#010x} twice", kept.address));
      }
      profile.kept_code.push_back(kept);
    }
    else if (section == "Profile")
    {
      u32 number = 0;
      if (!ParseNumber(value, &number))
        return fail(fmt::format("{} is not a number", value));
      if (key == "Revision")
        profile_revision = static_cast<u16>(number);
      else if (key == "FrameHook")
        profile.frame_hook = number;
      else if (key == "FrameHookWord")
        profile.frame_hook_word = number;
      else if (key == "BootNandFrame")
        profile.boot_nand_frame = number;
      else if (key == "JitClearFrame")
        profile.jit_clear_frame = number;
      else
        return fail(fmt::format("unknown key {}", key));
    }
    else if (section == "Forced")
    {
      // <System>.<Section>.<Key>, as on the command line.
      const size_t dot1 = key.find('.');
      const size_t dot2 = dot1 == std::string::npos ? dot1 : key.find('.', dot1 + 1);
      if (dot2 == std::string::npos)
        return fail(fmt::format("{} is not <System>.<Section>.<Key>", key));
      const std::optional<Config::System> system = Config::GetSystemFromName(key.substr(0, dot1));
      if (!system)
        return fail(fmt::format("unknown system in {}", key));
      profile.forced.emplace_back(Config::Location{*system, key.substr(dot1 + 1, dot2 - dot1 - 1),
                                                   key.substr(dot2 + 1)},
                                  value);
    }
    else
    {
      return fail("setting outside a section");
    }
  }

  if (!profile_revision)
  {
    *error = fmt::format("Orca profile {} has no Revision", path);
    return std::nullopt;
  }
  if (revision && *profile_revision != *revision)
  {
    *error = fmt::format("This disc is {} revision {}; YG Dolphin's profile is for revision {}",
                         game_id, *revision, *profile_revision);
    return std::nullopt;
  }
  profile.revision = *profile_revision;
  // A launcher profile names the whole boot: loader, disc and SD image.
  const bool any_launcher = !profile.launcher_hash.empty() || !profile.disc.empty() ||
                            !profile.sd_image.empty() || !profile.sd_image_hash.empty();
  if (any_launcher && (profile.launcher_hash.empty() || profile.disc.empty() ||
                       profile.sd_image.empty() != profile.sd_image_hash.empty() ||
                       !profile.frame_hook || !profile.frame_hook_word))
  {
    // Without the word, the boundary would fire inside the loader.
    *error = fmt::format("Orca profile {}: a launcher profile needs Launcher, Disc, FrameHook and "
                         "FrameHookWord, and SDImage with SDImageHash",
                         path);
    return std::nullopt;
  }
  return profile;
}

std::string LauncherProfileName()
{
  const char* name = std::getenv("ORCA_PROFILE");
  if (!name)
    return {};
  // A profile file name in Data/Sys/Orca, nothing else.
  const std::string value(name);
  if (value.empty() || value.size() > 32 ||
      !std::ranges::all_of(value,
                           [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }))
  {
    return {};
  }
  return value;
}

std::string HashFile(const std::string& path, const std::atomic<bool>* stop)
{
  File::IOFile file(path, "rb");
  if (!file)
    return {};
  XXH3_state_t* state = XXH3_createState();
  if (!state)
    return {};
  XXH3_128bits_reset(state);
  std::vector<u8> buffer(8 << 20);
  u64 remaining = file.GetSize();
  bool ok = true;
  while (remaining > 0)
  {
    if (stop && stop->load(std::memory_order_relaxed))
    {
      ok = false;
      break;
    }
    const size_t n = static_cast<size_t>(std::min<u64>(remaining, buffer.size()));
    if (!file.ReadBytes(buffer.data(), n))
    {
      ok = false;
      break;
    }
    XXH3_128bits_update(state, buffer.data(), n);
    remaining -= n;
  }
  const XXH128_hash_t hash = XXH3_128bits_digest(state);
  XXH3_freeState(state);
  return ok && (!stop || !stop->load(std::memory_order_relaxed)) ?
             fmt::format("{:016x}{:016x}", hash.high64, hash.low64) :
             std::string{};
}

// The system's reason for the last failed open or read. On Windows that's its own code and words
// (a full disk, OneDrive not running, a sharing violation), which the C library folds into one
// errno; the number keeps a report readable when Windows answers in the player's language.
static std::string LastFileError()
{
#ifdef _WIN32
  const unsigned long code = _doserrno;
  if (code != 0)
    return fmt::format("{} (error {})", StripWhitespace(Common::GetWin32ErrorString(code)), code);
#endif
  return Common::LastStrerrorString();
}

std::string DiscOpenProblem(const std::string& disc_path)
{
  if (disc_path.empty())
    return "no disc is set";
  // Shared like DiscIO's own open (CreateBlobReader), so another program holding the image open
  // isn't blamed for a failure it didn't cause.
#ifdef _WIN32
  // LastFileError reads the CRT's last Windows code: clear it, so an older failure isn't reported.
  _doserrno = 0;
#endif
  File::IOFile file(disc_path, "rb", File::SharedAccess::ReadWrite);
  if (!file.IsOpen())
    return fmt::format("its disc won't open: {}", LastFileError());
  u8 head[4];
#ifdef _WIN32
  _doserrno = 0;
#endif
  if (!file.ReadBytes(head, sizeof(head)))
  {
    if (std::feof(file.GetHandle()))
      return "its disc file is cut short";
    // A file Windows has to fetch first (a OneDrive copy kept online only) fails here.
    return fmt::format("its disc won't read: {}", LastFileError());
  }
  // Only a plain extension goes in: the app passes a native path, and on Windows SplitPath doesn't
  // split at a backslash, so a name without one would hand back part of a folder's name.
  std::string path = disc_path;
  UnifyPathSeparators(path);
  std::string extension;
  SplitPath(path, nullptr, nullptr, &extension);
  const bool plain = extension.size() >= 2 && extension.size() <= 6 &&
                     std::all_of(extension.begin() + 1, extension.end(),
                                 [](unsigned char c) { return std::isalnum(c) != 0; });
  return fmt::format("its disc file ({}, {} bytes) isn't a disc image Orca reads",
                     plain ? extension : "no plain extension", file.GetSize());
}

bool PrepareLauncherBoot(Profile* profile, const std::string& executable_path,
                         const std::string& disc_path, std::string* error)
{
  const std::unique_ptr<DiscIO::VolumeDisc> disc =
      disc_path.empty() ? nullptr : DiscIO::CreateDisc(disc_path);
  if (!disc)
  {
    *error = fmt::format("Orca: profile {} boots {} from the drive, and {}", profile->game_id,
                         profile->disc, DiscOpenProblem(disc_path));
    return false;
  }
  // The boot inserts the disc with the same alias (Boot.cpp).
  AliasRevision(*disc);
  const u16 disc_revision = disc->GetRevision().value_or(0);
  if (disc->GetGameID() != profile->disc || disc_revision != profile->revision)
  {
    *error = fmt::format("Orca: profile {} needs {} revision {}; the disc is {} revision {}",
                         profile->game_id, profile->disc, profile->revision, disc->GetGameID(),
                         disc_revision);
    return false;
  }

  BootPhase::Mark(BootPhase::Phase::Disc);
  std::vector<BootAudit::FileToVerify> files{{
      executable_path, profile->launcher_hash,
      fmt::format("Orca: {} is not the loader profile {} names", executable_path, profile->game_id)}};
  if (!profile->sd_image.empty())
  {
    std::string folder;
    SplitPath(executable_path, &folder, nullptr, nullptr);
    const std::string image = folder + profile->sd_image;
    files.push_back({image, profile->sd_image_hash,
                     fmt::format("Orca: {} is missing or is not the SD card profile {} names", image,
                                 profile->game_id)});
    // Read-only, as before; a cache hit can run offline while its audit is pending.
    profile->sd_image_path = image;
  }
  profile->boot_audit = BootAudit::Audit::Start(std::move(files),
                                              File::GetUserPath(D_CACHE_IDX) + "OrcaBoot");
  if (!profile->boot_audit->CacheHit() && !profile->boot_audit->Wait())
  {
    *error = profile->boot_audit->Error();
    return false;
  }
  BootPhase::Mark(profile->boot_audit->CacheHit() ? BootPhase::Phase::HashCached :
                                                 BootPhase::Phase::HashFull);
  return true;
}

bool FrameHookLive(const Core::CPUThreadGuard& guard, const Profile& profile)
{
  if (!profile.IsLauncher() || !profile.frame_hook || !profile.frame_hook_word)
    return true;
  return PowerPC::MMU::HostRead<u32>(guard, *profile.frame_hook) == *profile.frame_hook_word;
}

std::unique_ptr<Config::ConfigLayerLoader> GenerateConfigLoader(const Profile& profile)
{
  return std::make_unique<ForcedConfigLoader>(profile);
}

bool SessionShaderModeAllowed(ShaderCompilationMode mode)
{
  return mode == ShaderCompilationMode::AsynchronousUberShaders ||
         mode == ShaderCompilationMode::AsynchronousSkipRendering;
}

bool SessionShaderThreadsAllowed(int threads)
{
  // Auto, or 1 to 4 compile threads (the app's choices): never 0 (the emulation thread), and never
  // hundreds of threads from a hand-edited GFX.ini.
  return threads == -1 || (threads >= 1 && threads <= 4);
}

std::vector<Config::Location> UniversalForcedLocations()
{
  std::vector<Config::Location> locations;
  ForEachUniversalSetting(
      [&locations](const auto& info, auto) { locations.push_back(info.GetLocation()); });
  return locations;
}

std::optional<std::string> VerifyForcedSettings(const Profile& profile)
{
  // The shader settings are the player's, but only within what a session allows.
  if (const ShaderCompilationMode mode = Config::Get(Config::GFX_SHADER_COMPILATION_MODE);
      !SessionShaderModeAllowed(mode))
  {
    return fmt::format("Orca: shader compilation mode {} is not allowed in a session",
                       static_cast<int>(mode));
  }
  if (const int threads = Config::Get(Config::GFX_SHADER_COMPILER_THREADS);
      !SessionShaderThreadsAllowed(threads))
  {
    return fmt::format("Orca: {} shader compiler threads is not allowed in a session", threads);
  }

  std::vector<Config::Location> locations = UniversalForcedLocations();
  for (const auto& [location, value] : profile.forced)
    locations.push_back(location);
  if (!profile.sd_image_path.empty())
    locations.push_back(Config::MAIN_WII_SD_CARD_IMAGE_PATH.GetLocation());

  for (const Config::Location& location : locations)
  {
    const Config::LayerType active = Config::GetActiveLayerForConfig(location);
    if (active != Config::LayerType::Netplay)
    {
      return fmt::format("Orca: {}.{}.{} is overridden by another config layer ({})",
                         Config::GetSystemName(location.system), location.section, location.key,
                         static_cast<int>(active));
    }
  }
  return std::nullopt;
}

std::string DescribeForcedSettings(const Profile& profile)
{
  return fmt::format("Orca: profile {} rev {}{}: CPUThread={} CPUCore={} FMA={} RTC={}:{} "
                     "SI={},{},{},{} EFBToTexture={} XFBToTexture={} EFBAccess={} BBox={} "
                     "ImmediateXFB={} VSync={} SmoothEarlyPresentation={} AudioBufferSize={} "
                     "ShaderCompilationMode={} WaitForShaders={} ShaderCompilerThreads={} "
                     "GFXBackend={}",
                     profile.game_id, profile.revision,
                     TestOverridesActive() ? " (TEST overrides active, not for real sessions)" : "",
                     Config::Get(Config::MAIN_CPU_THREAD),
                     static_cast<int>(Config::Get(Config::MAIN_CPU_CORE)),
                     Config::Get(Config::SESSION_USE_FMA), Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE),
                     Config::Get(Config::MAIN_CUSTOM_RTC_VALUE),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(0))),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(1))),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(2))),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(3))),
                     Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM),
                     Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM),
                     Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE),
                     Config::Get(Config::GFX_HACK_BBOX_ENABLE),
                     Config::Get(Config::GFX_HACK_IMMEDIATE_XFB), Config::Get(Config::GFX_VSYNC),
                     Config::Get(Config::MAIN_SMOOTH_EARLY_PRESENTATION),
                     Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE),
                     static_cast<int>(Config::Get(Config::GFX_SHADER_COMPILATION_MODE)),
                     Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING),
                     Config::Get(Config::GFX_SHADER_COMPILER_THREADS),
                     Config::Get(Config::MAIN_GFX_BACKEND));
}

std::string DescribeGraphics()
{
  const int resolution = Config::Get(Config::GFX_EFB_SCALE);
  return fmt::format(
      "Orca: graphics: GFXBackend={} Adapter={} InternalResolution={} MaxInternalResolution={} "
      "MSAA={} SSAA={} MaxAnisotropy={} ForceTextureFiltering={} OutputResampling={} "
      "EFBScaledCopy={} ShaderCompilationMode={} ShaderCompilerThreads={} GPUTextureDecoding={} "
      "CPUCull={} BackendMultithreading={} PostProcessingShader=\"{}\"",
      Config::Get(Config::MAIN_GFX_BACKEND), Config::Get(Config::GFX_ADAPTER),
      resolution == 0 ? std::string("auto") : fmt::format("{}x", resolution),
      Config::Get(Config::GFX_MAX_EFB_SCALE), Config::Get(Config::GFX_MSAA),
      Config::Get(Config::GFX_SSAA),
      static_cast<int>(Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY)),
      static_cast<int>(Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING)),
      static_cast<int>(Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING)),
      Config::Get(Config::GFX_HACK_COPY_EFB_SCALED),
      static_cast<int>(Config::Get(Config::GFX_SHADER_COMPILATION_MODE)),
      Config::Get(Config::GFX_SHADER_COMPILER_THREADS),
      Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING), Config::Get(Config::GFX_CPU_CULL),
      Config::Get(Config::GFX_BACKEND_MULTITHREADING),
      Config::Get(Config::GFX_ENHANCE_POST_SHADER));
}
}  // namespace Orca
