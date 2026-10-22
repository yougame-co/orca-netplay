// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <OptionParser.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <fmt/format.h>

#include "AudioCommon/CubebUtils.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/Orca/Activity.h"
#include "Core/Orca/BootLogs.h"
#include "Core/Orca/BootMemory.h"
#include "Core/Orca/BootThrottle.h"
#include "Core/Orca/Branding.h"
#include "Core/Orca/Bridge.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/CrashNote.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/DiscVerify.h"
#include "Core/Orca/Launch.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Perf.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Sandbox.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/UX.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"
#include "DolphinNoGUI/Embed.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"

#include "UICommon/CommandLineParse.h"
#ifdef USE_DISCORD_PRESENCE
#include "UICommon/DiscordPresence.h"
#endif
#include "UICommon/UICommon.h"

#ifdef __APPLE__
#include "InputCommon/ControllerInterface/Quartz/Quartz.h"
#endif

static std::unique_ptr<Platform> s_platform;
// True in embed mode (Embed.h): drawn inside the YouGame app's window, controlled over stdin.
static bool s_embedded = false;
static bool s_single_line_panics = false;
// Embed::ClaimStdout redirects stdout to stderr; diagnostics for the app use its private stream.
static std::FILE* s_panic_output = nullptr;
// The disc of a --verify run (DiscVerify.h), which checks it and exits without booting.
static std::string s_verify_disc;
// --sandbox-probe (ORCA.md "The sandbox"): SandboxProbe instead of a boot.
static bool s_sandbox_probe = false;

static void signal_handler(int)
{
  constexpr char message[] =
      "A signal was received. A second signal will force YG Dolphin to stop.\n";
#ifdef _WIN32
  fputs(message, stderr);
#else
  if (write(STDERR_FILENO, message, sizeof(message)) < 0)
  {
  }
#endif

  s_platform->RequestShutdown();
}

// An alert's text on stderr, with Orca branding. Never throws: alerts come from the CPU thread
// inside JIT code (an invalid access), where a C++ exception can't unwind, and fmt::print throws
// when stderr takes fewer bytes than it was given (Orca/CrashNote.h). Only Windows builds have
// exceptions; elsewhere fmt's failure would abort.
static void PrintAlert(const char* text) noexcept
{
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try
  {
    Orca::CrashNote::WriteLine(stderr, Orca::BrandText(text));
  }
  catch (...)
  {
    Orca::CrashNote::WriteLine(stderr, text);
  }
#else
  Orca::CrashNote::WriteLine(stderr, Orca::BrandText(text));
#endif
}

static void PrintPanicLine(const char* text) noexcept
{
  Orca::CrashNote::WriteLine(s_panic_output ? s_panic_output : stdout, text);
}

// At the game's first frame: restore pacing; embedded boot logs go off (Orca/BootLogs.h).
// On the CPU thread, so std::fprintf: a throw from there can't unwind (Orca/CrashNote.h).
static void EndBootLogs()
{
  Orca::BootThrottle::Finish(Rollback::RingPort::Active() ||
                             (Rollback::Harness::Active() && !Rollback::Diag::KeepThrottle()));
  if (!s_embedded)
    return;
  const std::string ended = Orca::BootLogs::EndAtFirstFrame();
  if (!ended.empty())
    std::fprintf(stderr, "Orca: boot logs off at the first frame: %s\n", ended.c_str());
}

#ifdef _WIN32
// Like the default handler (a message box), but with Orca branding.
static bool MsgAlertHandler(const char* caption, const char* text, bool yes_no,
                            Common::MsgType style)
{
  if (s_single_line_panics &&
      (style == Common::MsgType::Warning || style == Common::MsgType::Critical))
    return false;  // Already emitted and logged before the handler was called.
  // When embedded, verifying or probing, log the alert instead of popping a message box.
  if (s_embedded || !s_verify_disc.empty() || s_sandbox_probe)
  {
    PrintAlert(text);
    return false;
  }
  UINT window_style = MB_ICONINFORMATION;
  if (style == Common::MsgType::Question)
    window_style = MB_ICONQUESTION;
  if (style == Common::MsgType::Warning)
    window_style = MB_ICONWARNING;

  return IDYES == MessageBox(nullptr, UTF8ToTStr(Orca::BrandText(text)).c_str(),
                             UTF8ToTStr(caption).c_str(),
                             window_style | (yes_no ? MB_YESNO : MB_OK));
}
#else
// Like the default handler (print to stderr), but with Orca branding.
static bool MsgAlertHandler(const char*, const char* text, bool, Common::MsgType style)
{
  if (s_single_line_panics &&
      (style == Common::MsgType::Warning || style == Common::MsgType::Critical))
    return false;
  PrintAlert(text);
  // Answer no to any question, like the default handler.
  return false;
}
#endif

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(const HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_platform->Stop();
}

void Host_UpdateTitle(const std::string& title)
{
  s_platform->ShowTitle(title);
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return s_platform->IsWindowFocused();
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return s_platform->IsWindowFullscreen();
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateClientID(client_id);
#endif
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
#ifdef USE_DISCORD_PRESENCE
  return Discord::UpdateDiscordPresenceRaw(details, state, large_image_key, large_image_text,
                                           small_image_key, small_image_text, start_timestamp,
                                           end_timestamp, party_size, party_max);
#else
  return false;
#endif
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}

static std::unique_ptr<Platform> GetPlatform(const optparse::Values& options)
{
  std::string platform_name = static_cast<const char*>(options.get("platform"));

#if HAVE_X11
  if (platform_name == "x11" || platform_name.empty())
    return Platform::CreateX11Platform();
#endif

#ifdef __linux__
  if (platform_name == "fbdev" || platform_name.empty())
    return Platform::CreateFBDevPlatform();
#endif

#ifdef _WIN32
  if (platform_name == "win32" || platform_name.empty())
    return Platform::CreateWin32Platform();
#endif
#ifdef __APPLE__
  if (platform_name == "macos" || platform_name.empty())
    return Platform::CreateMacOSPlatform();
#endif

  if (platform_name == "headless" || platform_name.empty())
    return Platform::CreateHeadlessPlatform();

  return nullptr;
}

#ifdef ORCA_MACOS_BUNDLE
// Orca.app started with no disc: use the disc remembered in <user>/Config/Orca.ini if it still
// checks out, otherwise ask the player to pick one (again after a wrong revision). Returns nullopt,
// after reporting the error, if they give up.
static std::optional<std::string> ChooseSessionDisc(const std::string& user_directory)
{
  const std::string ini_path = user_directory + DIR_SEP "Config" DIR_SEP "Orca.ini";
  Common::IniFile ini;
  ini.Load(ini_path);
  auto* const disc_section = ini.GetOrCreateSection("Disc");
  std::string remembered;
  disc_section->Get("Path", &remembered);
  if (!remembered.empty() && Orca::CheckDisc(remembered, Orca::APP_GAME_ID).ok)
    return remembered;

  std::optional<Orca::DiscCheck> refused;
  while (true)
  {
    const std::optional<std::string> path =
        Platform::ChooseFileMacOS(std::string(Orca::APP_DISC_PROMPT));
    if (!path)
    {
      if (refused)
        Orca::Status::Error(refused->code, refused->sentence);
      else
        Orca::Status::Error("cancelled", "No disc image was chosen.");
      return std::nullopt;
    }
    Orca::DiscCheck check = Orca::CheckDisc(*path, Orca::APP_GAME_ID);
    if (check.ok)
    {
      disc_section->Set("Path", *path);
      File::CreateFullPath(ini_path);
      ini.Save(ini_path);
      return path;
    }
    fmt::print(stderr, "Orca: {}: {}\n", *path, check.sentence);
    // Only a wrong disc revision is worth asking again; other errors won't change with a new pick.
    if (check.code != "disc_revision")
    {
      Orca::Status::Error(check.code, check.sentence);
      return std::nullopt;
    }
    Platform::ShowErrorMacOS("YG Dolphin can't play that disc", check.sentence);
    refused = std::move(check);
  }
}
#endif

#ifdef _WIN32
#define main app_main
#endif

// Ends the run early when the boot file is missing or unreadable. Reports through the session
// status when a session is active, otherwise on stderr.
static int RefuseBootFile(const Orca::DiscCheck& check)
{
  if (Orca::SessionActive())
    Orca::Status::Error(check.code, check.sentence);
  else
    fmt::print(stderr, "Orca: {} {}\n", check.code, check.sentence);
  Orca::Status::Finish();
  return 1;
}

// The rest of main, after the embed arguments are removed. Returns the exit code.
// --sandbox-probe (ORCA.md "The sandbox"): what a boot asks of the system before its first frame,
// with nothing of the player's: no disc, and only the empty folder the app gives it (-u). The desktop
// app runs it inside its macOS sandbox once per macOS version and Orca version, under the strict
// profile and, only if that ends the probe, the relaxed one, and plays under the profile that
// passed. So the profile is chosen by this run alone, never by a player's run, which a crafted disc
// or setting could end on purpose to be retried under the relaxed one. Says PROBE_OK on stdout.
static constexpr const char* PROBE_OK = "orca probe ok";
static int SandboxProbe(const std::string& user_directory)
{
  if (user_directory.empty())
  {
    fmt::print(stderr, "--sandbox-probe needs -u <folder>\n");
    return 1;
  }
  UICommon::SetUserDirectory(user_directory);
  UICommon::CreateDirectories();
  UICommon::Init();
  Common::ScopeGuard shutdown([] { UICommon::Shutdown(); });
  // The GPU, as a boot opens it (Metal on a Mac), with no window.
  const WindowSystemInfo wsi(WindowSystemType::Headless, nullptr, nullptr, nullptr);
  VideoBackendBase::PopulateBackendInfo(wsi);
  // The controllers, without the keyboard's global state (on the stdio bridge the app sends it).
#ifdef __APPLE__
  ciface::Quartz::SetKeyboardEnabled(false);
#endif
  UICommon::InitControllers(wsi);
  UICommon::ShutdownControllers();
  // The sound out, as a boot opens it.
  const bool sound = CubebUtils::GetContext() != nullptr;
  fmt::print(stdout, "{}{}\n", PROBE_OK, sound ? "" : " (no sound)");
  std::fflush(stdout);
  return 0;
}

static int Run(const int argc, char* argv[], const Embed::Options& embed)
{
#ifdef ORCA_MACOS_BUNDLE
  // Orca.app always runs a session unless ORCA_SESSION is already set. Sessions start from an empty
  // NAND, not the player's save. TODO: seed from a canonical save once one exists.
  setenv("ORCA_SESSION", "1", 0);
  Orca::SetSeedSessionSave(false);
#endif

  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::OmitGUIOptions);
  parser->add_option("--pack")
      .action("store")
      .metavar("<folder>")
      .help("Play an unsigned local pack offline (tree format 1 or flat format 2)");
  parser->add_option("-p", "--platform")
      .action("store")
      .help("Window platform to use [%choices]")
      .choices({"headless"
#ifdef __linux__
                ,
                "fbdev"
#endif
#if HAVE_X11
                ,
                "x11"
#endif
#ifdef _WIN32
                ,
                "win32"
#endif
#ifdef __APPLE__
                ,
                "macos"
#endif
      });

  // Reject a missing pack folder before optparse, whose error path throws through code built
  // without exception support.
  for (int i = 1; i < argc; ++i)
  {
    const std::string_view arg(argv[i]);
    if (arg == "--")
      break;
    const size_t equals = arg.find('=');
    const std::string_view option = arg.substr(0, equals);
    // Optparse accepts --pa and --pac too; --p is ambiguous with --platform.
    const bool pack = option.size() >= 4 && std::string_view("--pack").starts_with(option);
    if (pack &&
        ((equals != std::string_view::npos && equals + 1 == arg.size()) ||
         (equals == std::string_view::npos &&
          (i + 1 == argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-'))))
    {
      fmt::print(stderr, "orca error profile --pack needs a pack folder\n");
      return 1;
    }
  }
  optparse::Values& options = CommandLineParse::ParseArguments(parser.get(), argc, argv);
  std::vector<std::string> args = parser->args();

  // Register before reading the boot file, so an embedded alert never becomes a message box hidden
  // behind the app's window.
  Common::RegisterMsgAlertHandler(MsgAlertHandler);
  s_single_line_panics = embed.enabled || Orca::SessionActive() || options.is_set("pack");
  Common::RegisterPanicAlertLogHandler(s_single_line_panics ? PrintPanicLine : nullptr);

  if (options.is_set("pack"))
  {
    const std::string folder = static_cast<const char*>(options.get("pack"));
    if (folder.empty())
    {
      fmt::print(stderr, "orca error profile --pack needs a pack folder\n");
      return 1;
    }
    Orca::Mod::SetLocalPack(folder);
    Orca::SetSeedSessionSave(false);
  }

  std::optional<std::string> save_state_path;
  if (options.is_set("save_state"))
  {
    save_state_path = static_cast<const char*>(options.get("save_state"));
  }

  std::string user_directory;
  if (options.is_set("user"))
    user_directory = static_cast<const char*>(options.get("user"));
#ifdef ORCA_MACOS_BUNDLE
  // Use Orca's own user directory, never Dolphin's.
  else if (const char* home = std::getenv("HOME"); home && *home)
    user_directory = std::string(home) + "/Library/Application Support/Orca";
#endif

  if (s_sandbox_probe)
    return SandboxProbe(user_directory);

  if (!s_verify_disc.empty())
  {
    UICommon::SetUserDirectory(user_directory);
    // The verifier's IOS checks signatures in a NAND of its own, so the player's is never written.
    // Clears those of verifies that were killed first (DiscVerify.h).
    const std::string nand = Orca::DiscVerify::MakeNand();
    if (!nand.empty())
      File::SetUserPath(D_WIIROOT_IDX, nand);
    const int code = Orca::DiscVerify::Run(s_verify_disc, stdout);
    if (!nand.empty())
      File::DeleteDirRecursively(nand);
    return code;
  }

  std::unique_ptr<BootParameters> boot;
  bool game_specified = false;
  // When orca-launch.ini boots a loader, the disc that loader boots from the drive.
  [[maybe_unused]] std::string launch_disc;
  if (options.is_set("exec"))
  {
    const std::list<std::string> paths_list = options.all("exec");
    const std::vector<std::string> paths{std::make_move_iterator(std::begin(paths_list)),
                                         std::make_move_iterator(std::end(paths_list))};
    if (const Orca::DiscCheck check = Orca::CheckBootFile(paths.front()); !check.ok)
      return RefuseBootFile(check);
    boot = BootParameters::GenerateFromFile(
        paths, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    game_specified = true;
  }
  else if (options.is_set("nand_title"))
  {
    const std::string hex_string = static_cast<const char*>(options.get("nand_title"));
    if (hex_string.length() != 16)
    {
      fprintf(stderr, "Invalid title ID\n");
      parser->print_help();
      return 1;
    }
    const u64 title_id = std::stoull(hex_string, nullptr, 16);
    boot = std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
  }
  else if (args.size())
  {
    if (const Orca::DiscCheck check = Orca::CheckBootFile(args.front()); !check.ok)
      return RefuseBootFile(check);
    boot = BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    args.erase(args.begin());
    game_specified = true;
  }
#ifdef ORCA_MACOS_BUNDLE
  else if (Orca::SessionActive() && !user_directory.empty())
  {
    // A mod build ships orca-launch.ini next to Orca.app (Core/Orca/Launch.h), naming a profile and
    // a loader to boot with the player's disc as the default disc. Without it, boot the disc.
    std::string bundle_parent;
    SplitPath(File::GetBundleDirectory(), &bundle_parent, nullptr, nullptr);
    std::string launch_error;
    const std::optional<Orca::LaunchSpec> launch =
        bundle_parent.empty() ? std::nullopt : Orca::ReadLaunchIni(bundle_parent, &launch_error);
    if (!launch_error.empty())
    {
      Orca::Status::Error("profile", launch_error);
      return Orca::Status::Finish();
    }
    const std::optional<std::string> disc = ChooseSessionDisc(user_directory);
    if (!disc)
      return Orca::Status::Finish();
    if (launch)
    {
      setenv("ORCA_PROFILE", launch->profile.c_str(), 0);
      launch_disc = *disc;
      boot = BootParameters::GenerateFromFile(
          launch->executable, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    }
    else
    {
      boot = BootParameters::GenerateFromFile(
          *disc, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    }
    game_specified = true;
  }
#endif
  else
  {
    parser->print_help();
    return 0;
  }

  // Dolphin couldn't parse the boot file (its alert said why), so don't open a window.
  if (game_specified && !boot)
  {
    fprintf(stderr, "Could not boot the specified file\n");
    if (embed.enabled && !Orca::SessionActive())
      Embed::Out("error YG Dolphin couldn't start the game");
    Orca::Status::Finish();
    return 1;
  }

  // Inside the app's sandbox (Orca/Sandbox.h): say so in the log, and refuse a user folder Orca may
  // not write, which would otherwise fail somewhere later with a reason nobody could act on.
  if (Orca::SessionActive() && Orca::Sandbox::Jailed())
  {
    fmt::print(stderr, "Orca: in the app's sandbox (network {}{})\n",
               Orca::Sandbox::NetModeName(Orca::Sandbox::NetModeFromEnv()),
               Orca::Bridge::Enabled() ? ", stdio bridge" : "");
    if (!user_directory.empty() && Orca::Sandbox::WriteDenied(user_directory))
    {
      Orca::Status::Error(Orca::Sandbox::DENIED_ERROR, Orca::Sandbox::DENIED_SENTENCE);
      return Orca::Status::Finish();
    }
  }

  s_platform = GetPlatform(options);
  if (s_platform && embed.enabled)
    s_platform->SetEmbed(embed);
  if (!s_platform || !s_platform->Init())
  {
    fprintf(stderr, "No platform found, or failed to initialize.\n");
    // Unless the platform already said why (sandbox_embed: nothing adopted the window).
    if (embed.enabled && Orca::Status::ExitCode() == 0)
      Embed::Out("error YG Dolphin couldn't open its view in the app's window");
    return 1;
  }

  const WindowSystemInfo wsi = s_platform->GetWindowSystemInfo();

  UICommon::SetUserDirectory(user_directory);
  UICommon::CreateDirectories();
  UICommon::Init();
  // When embedded, turn on the netplay and rollback logs at INFO so a player's bug report shows
  // what the room and match did. The app saves stderr to the game's log. A more verbose Logger.ini
  // wins.
  if (embed.enabled)
  {
    auto* const logs = Common::Log::LogManager::GetInstance();
    logs->SetEnable(Common::Log::LogType::NETPLAY, true);
    logs->SetEnable(Common::Log::LogType::ROLLBACK, true);
    if (logs->GetEffectiveLogLevel() < Common::Log::LogLevel::LINFO)
      logs->SetConfigLogLevel(Common::Log::LogLevel::LINFO);
  }
#ifdef ORCA_MACOS_BUNDLE
  // Until the player has their own mapping, port 1 uses the keyboard and the first SDL gamepad
  // (Sys/Orca/Input/GCPadNew.ini).
  if (Orca::SessionActive())
  {
    const std::string pad_ini = File::GetUserPath(D_CONFIG_IDX) + "GCPadNew.ini";
    if (!File::Exists(pad_ini) &&
        !(File::CreateFullPath(pad_ini) &&
          File::Copy(File::GetSysDirectory() + "Orca/Input/GCPadNew.ini", pad_ini)))
    {
      fmt::print(stderr, "Orca: couldn't write the default controller mapping to {}\n", pad_ini);
    }
  }
#endif
#ifdef ORCA_MACOS_BUNDLE
  // For this run only; the player's Dolphin.ini keeps its own default disc.
  if (!launch_disc.empty())
    Config::SetCurrent(Config::MAIN_DEFAULT_ISO, launch_disc);
#endif
  // When embedded, ignore input unless the game has focus. The keyboard is read from raw HID
  // state, so typing in the app's chat would otherwise reach the game.
  if (embed.enabled)
    Config::SetCurrent(Config::MAIN_INPUT_BACKGROUND_INPUT, false);
#ifdef __APPLE__
  // On the stdio bridge the page sends the keyboard as a pad, so read no global key state at all.
  if (!Orca::Sandbox::OsKeyboardAllowed(Orca::Sandbox::ThisOs(), Orca::SessionActive(),
                                        Orca::Bridge::Enabled()))
  {
    ciface::Quartz::SetKeyboardEnabled(false);
    fmt::print(stderr, "Orca: no keyboard of its own: the page sends it as a pad\n");
  }
#endif
  UICommon::InitControllers(wsi);
  // The in-game UI: overlay, name tags and the app's controllers as the local pad.
  if (Orca::SessionActive())
  {
    // The overlay reads the game's fonts from its disc: the boot disc, or the default disc a loader
    // boots.
    std::string disc = Config::Get(Config::MAIN_DEFAULT_ISO);
    if (boot)
    {
      if (const auto* d = std::get_if<BootParameters::Disc>(&boot->parameters))
        disc = d->path;
    }
    Orca::UX::SetGameDisc(std::move(disc));
    Orca::UX::Init();
  }

  Common::ScopeGuard ui_common_guard([] {
    Orca::UX::Shutdown();
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
  });

  if (save_state_path && !game_specified)
  {
    fprintf(stderr, "A save state cannot be loaded without specifying a game to launch.\n");
    return 1;
  }

  auto core_state_changed_hook = Core::AddOnStateChangedCallback([](const Core::State state) {
    if (state == Core::State::Uninitialized)
      s_platform->Stop();
  });

#ifdef _WIN32
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
#else
  // Shut down cleanly on SIGINT and SIGTERM
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif

  DolphinAnalytics::Instance().ReportDolphinStart("nogui");

  // The stdio bridge (Orca/Bridge.h): requests go out as protocol lines, and the app's replies come
  // in with the commands on stdin, so it needs stdin read (embedded, or a test harness's commands).
  if (Orca::Bridge::Enabled())
  {
    if (embed.enabled || Orca::GetEnv("ORCA_TEST_COMMANDS") == "1")
      Orca::Bridge::SetWriter(Embed::Out);
    else
      fmt::print(stderr, "Orca: YOUGAME_STDIO=1 without --embed: the app's bridge is off\n");
  }

  // Tell the app "ready" once the first frame is on screen.
  Common::EventHook first_frame_hook;
  if (embed.enabled)
  {
    // Until "ready" the app takes any line for boot progress: no activity lines before it.
    Orca::Activity::HoldUntilReady();
    first_frame_hook = GetVideoEvents().after_present_event.Register([](PresentInfo& info) {
      static std::atomic<bool> seen{false};
      if (seen.exchange(true))
        return;
      s_platform->NotifyFirstFrame(
          static_cast<int>(info.frame_buffer_width), static_cast<int>(info.frame_buffer_height),
          Core::System::GetInstance().GetVideoInterface().GetTargetRefreshRate());
    });
    Embed::StartReading();
  }
  const bool test_commands = !embed.enabled && Orca::GetEnv("ORCA_TEST_COMMANDS") == "1";
  if (test_commands)
  {
    s_platform->EnableTestCommands();
    Embed::StartReading();
    // A harness run steered like the app prints no "ready"; with ORCA_TEST_PERF it reports its
    // speed from the first frame, so the speed lines can be tested (Orca/Perf.h).
    if (!Orca::GetEnv("ORCA_TEST_PERF").empty())
      Orca::Perf::Ready();
  }

  bool booted = false;
  Orca::BootThrottle::Begin();
  Orca::Status::SetFirstFrameListener(EndBootLogs);
  Orca::BootMemory::Run([&] {
    booted = BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi);
  });
  if (!booted)
  {
    Orca::BootThrottle::Finish(false);
    fprintf(stderr, "Could not boot the specified file\n");
    // In a session, the session status has already reported the error.
    if (embed.enabled && !Orca::SessionActive())
      Embed::Out("error YG Dolphin couldn't start the game");
    Orca::Status::Finish();
    return 1;
  }

  // Print the caps now that the boot has chosen a profile. Mod mode: from the main loop once they
  // are settled (Status::CapsSettled: a builtin's caps wait for its guard words).
  if (test_commands)
  {
    if (Orca::Mod::Requested())
      s_platform->RequestTestCaps();
    else
      Orca::Status::PrintCaps();
  }

#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif

  s_platform->MainLoop();
  // The game has stopped: the activity lines' last one, before leaving the room takes its time.
  Orca::Activity::Finish();
  // Stopping can take a while (a session leaves its room), so get out of the app's way first.
  s_platform->EmbedTeardown();
  Core::Stop(Core::System::GetInstance());

  Core::Shutdown(Core::System::GetInstance());
  first_frame_hook.reset();
  s_platform.reset();

  // In a session, the reported error explains a non-zero exit.
  return Orca::Status::Finish();
}

int main(const int raw_argc, char* raw_argv[])
{
  // Windows: a C++ exception names itself on stderr as it is thrown, so a run it ends says why.
  Orca::CrashNote::InstallThrowReporter();
  // And so does a fault (an access violation, a failed assert): its module, offset and the
  // graphics device's state.
  Orca::CrashNote::InstallFaultReporter();
  // Remove --embed, --parent, --rect and --verify before the regular parser sees the arguments.
  std::vector<char*> arg_list(raw_argv, raw_argv + raw_argc);
  std::string embed_error;
  const std::optional<Embed::Options> embed = Embed::TakeArguments(&arg_list, &embed_error);
  if (!embed)
  {
    fmt::print(stderr, "{}\n", embed_error);
    Embed::Out("error " + embed_error);
    Embed::Out("exit 2");
    return 2;
  }
  std::string verify_error;
  const std::optional<std::string> verify =
      Orca::DiscVerify::TakeArgument(&arg_list, &verify_error);
  if (!verify || (!verify->empty() && embed->enabled))
  {
    fmt::print(stderr, "{}\n", verify ? "--verify can't be used with --embed" : verify_error);
    return 1;
  }
  s_verify_disc = *verify;
  // --sandbox-probe: alone, never with --embed or --verify (SandboxProbe).
  if (const auto it = std::find_if(arg_list.begin() + 1, arg_list.end(),
                                   [](const char* a) { return std::string_view(a) == "--sandbox-probe"; });
      it != arg_list.end())
  {
    arg_list.erase(it);
    if (embed->enabled || !s_verify_disc.empty())
    {
      fmt::print(stderr, "--sandbox-probe can't be used with --embed or --verify\n");
      return 1;
    }
    s_sandbox_probe = true;
  }
  s_embedded = embed->enabled;
  if (s_embedded)
  {
    s_panic_output = Embed::ClaimStdout();
    Orca::Status::SetOutput(s_panic_output);
    // Also send every error to the app as "error <sentence>".
    Orca::Status::SetErrorListener([](std::string_view, std::string_view sentence) {
      Embed::Out(fmt::format("error {}", sentence));
    });
    // The app's boot logs have told the boot's story once the game draws (Orca/BootLogs.h).
    Orca::Status::SetFirstFrameListener(EndBootLogs);
  }
  const int argc = static_cast<int>(arg_list.size());
  arg_list.push_back(nullptr);

  const int code = Run(argc, arg_list.data(), *embed);
  if (s_embedded)
  {
    // Covers every exit path, after all of Run's cleanup.
    Embed::Out(fmt::format("exit {}", code));
#ifdef _WIN32
    // The stdin reader may still hold stdin's lock, and the CRT's exit-time flush would wait on it.
    // Everything that matters is written, so exit without that flush.
    std::fflush(stderr);
    _exit(code);
#endif
  }
  return code;
}

#ifdef _WIN32
int wmain(int, wchar_t*[], wchar_t*[])
{
  std::vector<std::string> args = Common::CommandLineToUtf8Argv(GetCommandLineW());
  const int argc = static_cast<int>(args.size());
  std::vector<char*> argv(args.size());
  for (size_t i = 0; i < args.size(); ++i)
    argv[i] = args[i].data();

  return main(argc, argv.data());
}

#undef main
#endif
