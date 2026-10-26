// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// File description
// -------------
// Purpose of this file: Collect boot settings for Core::Init()

// Call sequence: This file has one of the first function called when a game is booted,
// the boot sequence in the code is:

// DolphinWX:    FrameTools.cpp         StartGame
// Core          BootManager.cpp        BootCore
//               Core.cpp               Init                     Thread creation
//                                      EmuThread                Calls CBoot::BootUp
//               Boot.cpp               CBoot::BootUp()
//                                      CBoot::EmulatedBS2_Wii() / GC() or Load_BS2()

#include "Core/BootManager.h"

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Sram.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/Orca/BootAudit.h"
#include "Core/Orca/BootMemory.h"
#include "Core/Orca/BootPhase.h"
#include "Core/Orca/Branding.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/UX.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

#include "DiscIO/Enums.h"

namespace BootManager
{
// Boot the ISO or file
bool BootCore(Core::System& system, std::unique_ptr<BootParameters> boot,
              const WindowSystemInfo& wsi)
{
  Orca::BootMemory::BeginBoot();
  if (!boot)
    return false;

  SConfig& StartUp = SConfig::GetInstance();

  // Orca: the bundle this boot plays, if any (Core/Orca/Bundle.h), decided before anything
  // reads a game setting (mod mode leaves the player's game INIs out). Once per process.
  {
    const auto* executable = std::get_if<BootParameters::Executable>(&boot->parameters);
    // A launcher is checked as the bytes its reader already holds: those are what run.
    std::span<const u8> launcher_bytes;
    if (executable && executable->reader)
      launcher_bytes = executable->reader->GetBytes();
    Orca::Mod::ResolveBundle(executable ? &executable->path : nullptr, launcher_bytes);
    if ((!Orca::SessionActive() || Orca::Mod::LocalPackRequested()) &&
        Orca::Mod::ResolvedKind() == Orca::Mod::BundleKind::Refused)
    {
      const std::string error = Orca::Mod::ResolvedError();
      Orca::Status::Error("profile", error);
      // Local CLI failures return to the frontend immediately; a modal alert would wait for input.
      if (!Orca::Mod::LocalPackRequested())
        PanicAlertFmt("{}", Orca::BrandWords(error));
      return false;
    }
  }

  if (!StartUp.SetPathsAndGameMetadata(system, *boot))
    return false;
  if (std::holds_alternative<BootParameters::Disc>(boot->parameters))
    Orca::BootPhase::Mark(Orca::BootPhase::Phase::Disc);

  // Movie settings
  auto& movie = system.GetMovie();
  if (movie.IsPlayingInput() && movie.IsConfigSaved())
  {
    for (ExpansionInterface::Slot slot : ExpansionInterface::MEMCARD_SLOTS)
    {
      if (movie.IsUsingMemcard(slot) && movie.IsStartingFromClearSave() && !system.IsWii())
      {
        const auto raw_path =
            File::GetUserPath(D_GCUSER_IDX) +
            fmt::format("Movie{}.raw", slot == ExpansionInterface::Slot::A ? 'A' : 'B');
        if (File::Exists(raw_path))
          File::Delete(raw_path);

        const auto movie_path = File::GetUserPath(D_GCUSER_IDX) + "Movie";
        if (File::Exists(movie_path))
          File::DeleteDirRecursively(movie_path);
      }
    }
  }

  // Orca rollback session: force the universal and the game profile's settings on both players.
  std::optional<Orca::Profile> orca_profile;
  Common::ScopeGuard cancel_audit([&] {
    if (orca_profile && orca_profile->boot_audit)
      orca_profile->boot_audit->Cancel();
  });
  if (Orca::SessionActive())
  {
    Orca::Status::State("booting");
    std::string error;
    // A disc boots with its own profile; a loader executable (a mod's launcher) only with the
    // launcher profile ORCA_PROFILE names, which checks the loader, the disc and the SD card.
    const auto* executable = std::get_if<BootParameters::Executable>(&boot->parameters);
    bool mod_wrong_disc = false;
    if (Orca::Mod::Requested())
    {
      // Mod mode (a tree bundle in ORCA_MOD_DIR, or a verified flat bundle): the game comes from
      // the bundle, never from Data/Sys/Orca, and ORCA_PROFILE is ignored.
      if (const Orca::Mod::FlatBundle* flat = Orca::Mod::ResolvedFlat())
        orca_profile = Orca::LoadFlatProfile(*flat, &error);
      else if (Orca::Mod::ResolvedKind() == Orca::Mod::BundleKind::Refused)
        error = Orca::Mod::ResolvedError();
      else
        orca_profile = Orca::LoadModProfile(Orca::Mod::Dir(), &error);
      if (orca_profile &&
          !Orca::PrepareModBoot(&*orca_profile, executable ? &executable->path : nullptr,
                                Config::Get(Config::MAIN_DEFAULT_ISO), StartUp.GetGameID(),
                                StartUp.GetRevision(), &error, &mod_wrong_disc))
      {
        orca_profile.reset();
      }
      if (orca_profile)
      {
        const Orca::Mod::ModInfo& mod = *orca_profile->mod;
        NOTICE_LOG_FMT(BOOT, "Orca mod: {} ({}, {} {} rev {}), tree {}, builtin {}, SD {}",
                       mod.manifest.title.empty() ? "untitled" : mod.manifest.title,
                       mod.flat ? "flat bundle" : "tree bundle",
                       mod.manifest.launcher.empty() ? "disc" : "launcher on",
                       mod.manifest.disc_id, mod.manifest.disc_revision, mod.tree.hash,
                       Orca::Mod::BuiltinName(mod.manifest.builtin),
                       mod.sd_image_hash.empty() ? "none" : mod.sd_image_hash);
      }
    }
    else if (executable)
    {
      const std::string name = Orca::LauncherProfileName();
      if (name.empty())
        error = "Orca: booting a loader in a session needs ORCA_PROFILE (a launcher profile)";
      else
        orca_profile = Orca::LoadProfile(name, std::nullopt, &error);
      if (orca_profile && !orca_profile->IsLauncher())
      {
        error = fmt::format("Orca: profile {} is not a launcher profile", name);
        orca_profile.reset();
      }
      else if (orca_profile && !Orca::PrepareLauncherBoot(&*orca_profile, executable->path,
                                                          Config::Get(Config::MAIN_DEFAULT_ISO),
                                                          &error))
      {
        orca_profile.reset();
      }
    }
    else
    {
      orca_profile = Orca::LoadProfile(StartUp.GetGameID(), StartUp.GetRevision(), &error);
      if (!orca_profile && StartUp.GetGameID() == "RSBE01" && StartUp.GetRevision() == 1)
      {
        if (const auto* disc = std::get_if<BootParameters::Disc>(&boot->parameters))
          error = Orca::DiscRevisionRefusal(std::move(error), disc->alias_revision_reason);
      }
      if (orca_profile && orca_profile->IsLauncher())
      {
        error = fmt::format("Orca: profile {} boots through its loader, not the disc",
                            orca_profile->game_id);
        orca_profile.reset();
      }
    }
    // A mod's refusal is the disc's only when PrepareModBoot says so (mod.json's own errors can
    // name a revision too).
    const char* error_code = !orca_profile && (Orca::Mod::Requested() ?
                                                   mod_wrong_disc :
                                                   error.find("revision") != std::string::npos) ?
                                 "disc_revision" :
                                 (!orca_profile ? "profile" : "settings");
    if (orca_profile && NetPlay::IsNetPlayRunning())
      error = "Orca: a session can't run alongside Dolphin netplay";
    else if (orca_profile && !boot->riivolution_patches.empty())
      error = "Orca: Riivolution patches are not allowed in a session";
    else if (orca_profile && Config::Get(Config::MAIN_GFX_BACKEND) == "Software Renderer")
      error = "Orca: the Software renderer can't keep EFB copies on the GPU";
    if (!error.empty())
    {
      Orca::Status::Error(error_code, error);
      PanicAlertFmt("{}", Orca::BrandWords(error));
      return false;
    }
    orca_profile->boot_game_id = StartUp.GetGameID();
    Config::AddLayer(Orca::GenerateConfigLoader(*orca_profile));
    // A loader executable has no region of its own, so it got a fallback region from the host's
    // locale, which differs between machines. Use the session's forced region so every player's
    // console matches.
    if (executable)
      StartUp.m_region = Config::Get(Config::MAIN_FALLBACK_REGION);
  }

  if (NetPlay::IsNetPlayRunning())
  {
    const NetPlay::NetSettings* netplay_settings = boot->boot_session_data.GetNetplaySettings();
    if (!netplay_settings)
      return false;

    Config::AddLayer(ConfigLoaders::GenerateNetPlayConfigLoader(*netplay_settings));
    StartUp.bCopyWiiSaveNetplay = netplay_settings->savedata_load;
  }

  // Override out-of-region languages/countries to prevent games from crashing or behaving oddly
  if (!Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS))
  {
    Config::SetCurrent(
        Config::MAIN_GC_LANGUAGE,
        DiscIO::ToGameCubeLanguage(StartUp.GetLanguageAdjustedForRegion(false, StartUp.m_region)));

    if (system.IsWii())
    {
      const u32 wii_language =
          static_cast<u32>(StartUp.GetLanguageAdjustedForRegion(true, StartUp.m_region));
      if (wii_language != Config::Get(Config::SYSCONF_LANGUAGE))
        Config::SetCurrent(Config::SYSCONF_LANGUAGE, wii_language);

      const u8 country_code = static_cast<u8>(Config::Get(Config::SYSCONF_COUNTRY));
      if (StartUp.m_region != DiscIO::SysConfCountryToRegion(country_code))
      {
        switch (StartUp.m_region)
        {
        case DiscIO::Region::NTSC_J:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x01);  // Japan
          break;
        case DiscIO::Region::NTSC_U:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x31);  // United States
          break;
        case DiscIO::Region::PAL:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x6c);  // Switzerland
          break;
        case DiscIO::Region::NTSC_K:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x88);  // South Korea
          break;
        default:
          break;
        }
      }
    }
  }

  // Some NTSC Wii games such as Doc Louis's Punch-Out!! and
  // 1942 (Virtual Console) crash if the PAL60 option is enabled
  if (system.IsWii() && DiscIO::IsNTSC(StartUp.m_region) && Config::Get(Config::SYSCONF_PAL60))
    Config::SetCurrent(Config::SYSCONF_PAL60, false);

  // Disable loading time emulation for Riivolution-patched games until we have proper emulation.
  if (!boot->riivolution_patches.empty())
    Config::SetCurrent(Config::MAIN_FAST_DISC_SPEED, true);

  if (system.IsTriforce())
  {
    // Attach Triforce Baseboard hardware if not overridden in any layer.
    // Users may set these in their GameConfig if they want them not automatically attached.
    if (GetActiveLayerForConfig(Config::MAIN_SERIAL_PORT_1) == Config::LayerType::Base)
    {
      Config::SetCurrent(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::Baseboard);
    }
    if (GetActiveLayerForConfig(Config::GetInfoForSIDevice(0)) == Config::LayerType::Base)
    {
      Config::SetCurrent(Config::GetInfoForSIDevice(0),
                         SerialInterface::SIDevices::SIDEVICE_AM_BASEBOARD);
    }

    // Mario Kart Arcade GP has widescreen heuristic issues.
    // All Triforce games are 4:3 so we'll just disable the heuristic for now.
    if (GetActiveLayerForConfig(Config::GFX_SUGGESTED_ASPECT_RATIO) == Config::LayerType::Base)
    {
      Config::SetCurrent(Config::GFX_SUGGESTED_ASPECT_RATIO, AspectMode::ForceStandard);
    }
  }

  // Orca: nothing set after the forced layer may outrank it.
  if (orca_profile)
  {
    if (const auto problem = Orca::VerifyForcedSettings(*orca_profile))
    {
      Orca::Status::Error("settings", *problem);
      PanicAlertFmt("{}", *problem);
      return false;
    }
    NOTICE_LOG_FMT(BOOT, "{}", Orca::DescribeForcedSettings(*orca_profile));
    NOTICE_LOG_FMT(BOOT, "{}", Orca::DescribeGraphics());
  }
  Orca::SetActiveProfile(orca_profile);
  Orca::BootPhase::Mark(Orca::BootPhase::Phase::Config);
  Orca::UX::SelectModule(Orca::ActiveProfile());

  system.Initialize();
  Orca::BootPhase::Mark(Orca::BootPhase::Phase::SystemInit);

  Core::UpdateWantDeterminism(system, /*initial*/ true);

  ConfigLoaders::TransferSYSCONFControlToGuest();

  if (system.IsWii())
  {
    Core::InitializeWiiRoot(Core::WantsDeterminism());
    Orca::BootPhase::Mark(Orca::BootPhase::Phase::WiiRoot);

    // Ensure any new settings are written to the SYSCONF
    if (!Core::WantsDeterminism())
    {
      Core::BackupWiiSettings();
      ConfigLoaders::SaveToSYSCONF(Config::LayerType::Meta,
                                   ConfigLoaders::SkipIfControlledByGuest::No);
    }
    else
    {
      ConfigLoaders::SaveToSYSCONF(
          Config::LayerType::Meta, ConfigLoaders::SkipIfControlledByGuest::No,
          [](const Config::Location& location) {
            return Config::GetActiveLayerForConfig(location) >= Config::LayerType::Movie;
          });
    }
    Orca::BootPhase::Mark(Orca::BootPhase::Phase::Sysconf);
  }

  AchievementManager::GetInstance().CloseGame();

  const bool load_ipl = !system.IsWii() && !Config::Get(Config::MAIN_SKIP_IPL) &&
                        std::holds_alternative<BootParameters::Disc>(boot->parameters);
  if (load_ipl)
  {
    return Core::Init(
        system,
        std::make_unique<BootParameters>(
            BootParameters::IPL{StartUp.m_region,
                                std::move(std::get<BootParameters::Disc>(boot->parameters))},
            std::move(boot->boot_session_data)),
        wsi);
  }
  const bool initialized = Core::Init(system, std::move(boot), wsi);
  if (initialized)
    cancel_audit.Dismiss();
  if (initialized && orca_profile && orca_profile->boot_audit)
    Orca::BootAudit::WatchSession(orca_profile->boot_audit);
  return initialized;
}

void RestoreConfig()
{
  Orca::BootAudit::CancelSession();
  Core::ShutdownWiiRoot();

  if (Core::WiiRootIsTemporary())
  {
    ConfigLoaders::TransferSYSCONFControlFromGuest(ConfigLoaders::WriteBackChangedValues::No);
  }
  else
  {
    Core::RestoreWiiSettings(Core::RestoreReason::EmulationEnd);
    ConfigLoaders::TransferSYSCONFControlFromGuest(ConfigLoaders::WriteBackChangedValues::Yes);
  }

  Config::ClearCurrentRunLayer();
  Config::RemoveLayer(Config::LayerType::Movie);
  Config::RemoveLayer(Config::LayerType::Netplay);
  Config::RemoveLayer(Config::LayerType::GlobalGame);
  Config::RemoveLayer(Config::LayerType::LocalGame);
  SConfig::GetInstance().ResetRunningGameMetadata();
}

}  // namespace BootManager
