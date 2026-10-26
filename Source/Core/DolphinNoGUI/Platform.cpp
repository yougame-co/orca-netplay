// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/STM/STM.h"
#include "Core/Orca/Activity.h"
#include "Core/Orca/Branding.h"
#include "Core/Orca/Music.h"
#include "Core/Orca/Perf.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Chat.h"
#include "Core/Orca/Games/Brawl/NameTags.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/Search.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/YgOrb.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/System.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/Present.h"

Platform::~Platform() = default;

bool Platform::Init()
{
  return true;
}

void Platform::SetTitle(const std::string& title)
{
}

void Platform::UpdateRunningFlag()
{
  // Every main-loop pass comes through here, on the thread that owns the window: the stdio
  // bridge's heartbeat (the app stops an Orca that said it and then goes quiet). To the app, or a
  // test harness playing it (ORCA_TEST_COMMANDS: Tools/orca/stdio-app.py counts them).
  if ((m_embed.enabled || m_test_commands) && Orca::Bridge::Ready() &&
      m_heartbeat.Due(std::chrono::steady_clock::now()))
  {
    Embed::Out(std::string(Orca::Bridge::HEARTBEAT_LINE));
  }
  ProcessEmbedCommands();
  EndSoloPauseForDropIn();
  WatchRoomWhilePaused();
  UpdateTitle();
  if (m_shutdown_requested.TestAndClear())
  {
    const auto& system = Core::System::GetInstance();
    const auto ios = system.GetIOS();
    const auto stm = ios ? ios->GetDeviceByName("/dev/stm/eventhook") : nullptr;
    // Orca: a session stops at once. The power button would reach only this player's game, which
    // can't answer it while the session waits for the room.
    if (!Orca::SessionActive() && !m_tried_graceful_shutdown.IsSet() && stm &&
        std::static_pointer_cast<IOS::HLE::STMEventHookDevice>(stm)->HasHookInstalled())
    {
      system.GetProcessorInterface().PowerButton_Tap();
      m_tried_graceful_shutdown.Set();
    }
    else
    {
      m_running.Clear();
    }
  }
}

void Platform::EndSoloPauseForDropIn()
{
  auto& system = Core::System::GetInstance();
  // Only end the player's own pause (BeginPause). The core also pauses at boot, after an HLE reload
  // and on a panic, and those aren't ours to end.
  if (!Orca::SessionActive() || !Rollback::OnlineMatch::Pausing() ||
      Core::GetState(system) != Core::State::Paused || Rollback::OnlineMatch::SoloPauseAllowed())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Orca: the pause ends for drop-in (a friend arriving, an invite, a join "
                           "or a leave)");
  Core::SetState(system, Core::State::Running);
  if (m_embed.enabled || m_test_commands)
    Embed::Out("state running");
}

void Platform::WatchRoomWhilePaused()
{
  auto& system = Core::System::GetInstance();
  if (!Rollback::OnlineMatch::Pausing() || !Orca::SessionActive() || !Orca::Online::Enabled() ||
      Core::GetState(system) != Core::State::Paused)
  {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < m_next_room_watch)
    return;
  m_next_room_watch = now + std::chrono::seconds(2);
  // A room that ended gets the boundary's handling on the CPU thread, which the pause leaves idle;
  // so does the one that replaced it, once, to clear the lost room's notice.
  const bool ended = Orca::Online::RoomEnded();
  if (!ended && !(m_room_watch_lost && Orca::Online::InRoom()))
    return;
  m_room_watch_lost = ended;
  Core::RunOnCPUThread(system, [] { Rollback::OnlineMatch::WatchRoomWhilePaused(); });
}

void Platform::UpdateTitle()
{
  if (Orca::SessionActive() &&
      Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized)
  {
    const auto now = std::chrono::steady_clock::now();
    if (now >= m_next_title_check)
    {
      m_next_title_check = now + std::chrono::milliseconds(250);
      ShowTitle(Orca::WindowTitle());
    }
  }

  std::optional<std::string> title;
  {
    std::lock_guard lock(m_title_lock);
    title.swap(m_requested_title);
  }
  if (!title || *title == m_shown_title)
    return;
  m_shown_title = std::move(*title);
  SetTitle(m_shown_title);
}

void Platform::ShowTitle(const std::string& title)
{
  std::lock_guard lock(m_title_lock);
  m_requested_title = title;
}

void Platform::Stop()
{
  m_running.Clear();
}

void Platform::RequestShutdown()
{
  m_shutdown_requested.Set();
}

void Platform::NotifyFirstFrame(int width, int height, double fps)
{
  std::lock_guard lock(m_ready_lock);
  if (m_ready_sent || m_ready_line)
    return;
  m_ready_line = fmt::format("ready {} {} {:.4f}", width, height, fps);
}

void Platform::EmbedTeardown()
{
  if (!IsEmbedded())
    return;
  EmbedSetFocus(false);
  EmbedSetVisible(false);
}

void Platform::SetEmbed(const Embed::Options& options)
{
  m_embed = options;
  m_embed_result = Embed::Result(options.enabled && !options.adopted);
  m_embed_rect_known = options.enabled;
  // If --rect was too small, the platform creates the view hidden (Embed.h).
  m_embed_too_small = options.too_small;
  Orca::UX::SetHostVisible(!m_embed_too_small);
}

void Platform::ApplyEmbedVisible()
{
  const bool visible = m_embed_shown && !m_embed_too_small;
  EmbedSetVisible(visible);
  Orca::UX::SetHostVisible(visible);
}

void Platform::ApplyEmbedView()
{
  if (!m_embed_view || !m_embed_rect_known)
  {
    VideoCommon::Presenter::SetLayoutHint(std::nullopt);
    return;
  }
  const Embed::Rect& r = m_embed.rect;
  const Embed::Rect& v = *m_embed_view;
  VideoCommon::Presenter::SetLayoutHint(
      VideoCommon::LayoutHint{r.w, r.h, v.x - r.x, v.y - r.y, v.w, v.h});
}

void Platform::EmbedSucceeded()
{
  m_embed_result.Decide();
}

void Platform::EmbedFallback(std::string_view reason)
{
  m_embed_result.Decide(reason);
  m_embed_fallback = true;
}

void Platform::TraceEmbedRect(const Embed::Rect& requested) const
{
  if (!IsEmbedded() || !Embed::TraceEnabled())
    return;
  if (const auto actual = EmbedGetBounds())
    Embed::Trace(requested, *actual);
}

void Platform::ProcessEmbedCommands()
{
  if (!m_embed.enabled && !m_test_commands)
    return;
  {
    // A mod's builtin offers its caps only once its guards have held (Status::CapsSettled): until
    // then "ready" and the caps before it wait. Never in legacy mode.
    const bool settled = Orca::Status::CapsSettled();
    std::optional<std::string> ready;
    {
      std::lock_guard lock(m_ready_lock);
      if (settled)
        ready.swap(m_ready_line);
      if (ready)
        m_ready_sent = true;
    }
    if (ready)
    {
      // Announce this build's extra features before "ready"; the app answers with "caps <list>".
      // Prints nothing outside a session.
      Orca::Status::PrintCaps();
      Embed::Out(*ready);
      if (const auto result = m_embed_result.TakeLine(true))
        Embed::Out(*result);
      // The activity lines may start now (Orca/Activity.h, HoldUntilReady), and so may the speed
      // lines (Orca/Perf.h).
      Orca::Activity::Ready();
      Orca::Perf::Ready();
    }
    if (const auto result = m_embed_result.TakeLine(m_ready_sent))
      Embed::Out(*result);
    // Test commands: the caps once the boot chose its profile and they are settled.
    if (m_test_caps_pending && settled)
    {
      m_test_caps_pending = false;
      Orca::Status::PrintCaps();
    }
    // An offer that changed since (a builtin's guards failing after they held): printed again.
    Orca::Status::UpdateCaps();
  }

  auto& system = Core::System::GetInstance();
  if (m_pause_pending && Core::GetState(system) == Core::State::Running)
  {
    m_pause_pending = false;
    Core::SetState(system, Core::State::Paused);
  }
  const auto report_state = [this, &system] {
    Embed::Out(m_pause_pending || Core::GetState(system) == Core::State::Paused ? "state paused" :
                                                                                  "state running");
  };
  for (const std::string& line : Embed::TakeCommands())
  {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "quit")
    {
      // Stop at once, session or not: the app is closing the player or has died. Before the first
      // frame that is a stop, not a boot that failed (a Stop while shaders compile).
      Orca::Status::Stopping();
      Stop();
    }
    else if (cmd == "rect")
    {
      std::string rest;
      std::getline(in, rest);
      if (const std::optional<Embed::Rect> rect = Embed::ParseRect(rest, false))
      {
        // A rect too small for the game hides it at its last size instead (Embed.h).
        const bool too_small = Embed::TooSmall(*rect);
        if (!too_small)
        {
          m_embed.rect = *rect;
          m_embed_rect_known = true;
          // Update the layout first: the presenter keeps using it for the old surface until the
          // window system resizes it.
          ApplyEmbedView();
          EmbedSetRect(*rect);
          // The YouGame button is placed in the page's device pixels (UX/YgOrb.h).
          Orca::UX::YgOrb::SetViewSize(rect->w, rect->h);
        }
        if (too_small != m_embed_too_small)
        {
          m_embed_too_small = too_small;
          ApplyEmbedVisible();
        }
        TraceEmbedRect(*rect);
      }
    }
    else if (cmd == "view")
    {
      // Where to lay out the picture, in `rect`'s units and origin, while `rect` stays the window.
      // When the page's menu panel covers part of the player box, the window shrinks but the view
      // keeps the whole box, so the game neither shrinks nor moves. `view off` uses the window
      // again.
      std::string rest;
      std::getline(in, rest);
      std::istringstream words(rest);
      std::string first, more;
      words >> first;
      if (first == "off" && !(words >> more))
      {
        m_embed_view.reset();
        ApplyEmbedView();
        TraceEmbedRect(m_embed.rect);
      }
      else if (const std::optional<Embed::Rect> view = Embed::ParseRect(rest);
               view && !Embed::TooSmall(*view))
      {
        m_embed_view = *view;
        ApplyEmbedView();
        TraceEmbedRect(*view);
      }
      else
      {
        Embed::Out("unsupported view");
      }
    }
    else if (cmd == "dim")
    {
      // Dims everything Orca draws by N percent (0-100, 0 is off) while the page's menu is open.
      std::string value;
      in >> value;
      int percent = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), percent);
      std::string more;
      if (error == std::errc{} && end == value.data() + value.size() && percent >= 0 &&
          percent <= 100 && !(in >> more))
      {
        Orca::UX::SetDim(percent);
      }
      else
      {
        Embed::Out("unsupported dim");
      }
    }
    else if (cmd == "pause")
    {
      // A session pauses only while this player is alone in it (Core::SetState checks with
      // Rollback::OnlineMatch::BeginPause), since a friend's game must keep going.
      if (Core::GetState(system) == Core::State::Running)
        Core::SetState(system, Core::State::Paused);
      else if (Core::GetState(system) != Core::State::Paused && !Orca::SessionActive())
        m_pause_pending = true;  // Still starting.
      if (!m_pause_pending && Core::GetState(system) != Core::State::Paused)
        Embed::Out("unsupported pause");
      report_state();
    }
    else if (cmd == "resume")
    {
      m_pause_pending = false;
      if (Core::GetState(system) == Core::State::Paused)
        Core::SetState(system, Core::State::Running);
      report_state();
    }
    else if (cmd == "focus" || cmd == "blur")
    {
      // Focus means the page's menu is closed, so clear the dim even if a `dim 0` was missed.
      if (cmd == "focus")
        Orca::UX::SetDim(0);
      EmbedSetFocus(cmd == "focus");
    }
    else if (cmd == "hide" || cmd == "show")
    {
      m_embed_shown = cmd == "show";
      ApplyEmbedVisible();
    }
    else if (cmd == "volume")
    {
      // Linear 0..1. Set for this run only; Dolphin.ini keeps its own volume.
      float volume = 1.0f;
      if (in >> volume && std::isfinite(volume))
      {
        volume = std::clamp(volume, 0.0f, 1.0f);
        Config::SetCurrent(Config::MAIN_AUDIO_VOLUME, static_cast<int>(std::lround(volume * 100)));
        AudioCommon::UpdateSoundStream(system);
      }
    }
    else if (cmd == "music")
    {
      // "music on|off": the player's Music switch. Only what this machine plays changes
      // (Orca/Music.h), so it never reaches the game, a friend or the checksums.
      std::string value, more;
      in >> value;
      if ((value == "on" || value == "off") && !(in >> more) && Orca::Status::Cap("music"))
        Orca::Music::SetOn(value == "on");
      else
        Embed::Out("unsupported music");
    }
    else if (cmd == "replay")
    {
      if (!Orca::Status::Cap(Orca::Status::REPLAY_CAP) ||
          !Rollback::ReplayStream::Command(line))
        Embed::Out("unsupported replay");
    }
    else if (cmd == "record")
    {
      // "record on|off|auto": whether this player's match streams to the room for watchers and
      // the saved replay (Orca::Online::RecordWanted). "auto" records ranked rooms only.
      std::string value, more;
      in >> value;
      if ((value == "on" || value == "off" || value == "auto") && !(in >> more) &&
          Orca::Status::Cap(Orca::Status::RECORD_CAP))
      {
        Orca::Online::SetRecordChoice(value == "on"  ? Orca::Online::RecordChoice::On :
                                      value == "off" ? Orca::Online::RecordChoice::Off :
                                                       Orca::Online::RecordChoice::Auto);
      }
      else
      {
        Embed::Out("unsupported record");
      }
    }
    else if (cmd == "gfx")
    {
      // "gfx <key> <value>": a live change to one of the player's drawing-only graphics settings,
      // from a fixed list (Orca/Perf.h ParseGfx), with the app's "gfx" cap. It applies at the next
      // frame boundary, for this run only; the app passes the stored choice at the next start.
      const std::optional<Orca::Perf::GfxChange> change = Orca::Perf::ParseGfx(line);
      if (change && Orca::Status::Cap("gfx"))
        Orca::Perf::QueueGfx(*change);
      else
        Embed::Out("unsupported gfx");
    }
    else if (cmd == "save" || cmd == "load")
    {
      // Not supported: Orca keeps the game's own saves in its NAND.
      int slot = 0;
      in >> slot;
      Embed::Out(fmt::format("unsupported {} {}", cmd, slot));
    }
    else if (cmd == "caps")
    {
      // The features the app supports (Orca::Status::CAPS lists ours).
      std::string rest;
      std::getline(in, rest);
      Orca::Status::SetAppCaps(rest);
    }
    else if (cmd == "join")
    {
      // Join a friend's game from the running one (Orca::Online::ValidCode checks the code too).
      std::string code;
      in >> code;
      const bool valid = code.size() >= 6 && code.size() <= 12 &&
                         std::all_of(code.begin(), code.end(), [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                         });
      // A join sent before the app's "caps" line waits; the emulator thread takes it once the
      // app's caps include "join".
      if (!valid)
        Embed::Out("unsupported join");
      else
        Orca::Online::RequestJoin(code);
    }
    else if (cmd == "host")
    {
      // Host the room matchmaking put this game into (see ORCA.md, "Matchmaking"). Taken once the
      // app's caps include "host". Same code check as join.
      std::string code;
      in >> code;
      const bool valid = code.size() >= 6 && code.size() <= 12 &&
                         std::all_of(code.begin(), code.end(), [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                         });
      if (!valid)
        Embed::Out("unsupported host");
      else
        Orca::Online::RequestHost(code);
    }
    else if (cmd == "queue-cancel")
    {
      // The app's search ended without a match: stop showing the search and un-ready the player
      // on the queue's character select (Start searches again).
      Orca::UX::Search::End();
      Orca::UX::QueueApp::ClearReady();
    }
    else if (cmd == "queue")
    {
      // "queue rating <n|->": this player's ranked rating, or none, sent before host or join. It is
      // shared with the opponent along with the player's queue identity (UX/Queue.h).
      std::string what, value;
      in >> what >> value;
      int rating = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), rating);
      if (what == "rating" && value == "-")
        Orca::UX::QueueApp::SetOwnRating(-1);
      else if (what == "rating" && error == std::errc{} && end == value.data() + value.size() &&
               rating >= 0 && rating <= 0xFFFE)
      {
        Orca::UX::QueueApp::SetOwnRating(rating);
        // After a set, this is the new rating; the end-of-set panel counts up to it (UX/SetEnd.h).
        Orca::UX::SetEnd::Current().QueueRating(rating, Orca::UX::SetEnd::NowMs());
      }
      else
        Embed::Out("unsupported queue");
    }
    else if (cmd == "leave")
    {
      // Like join: taken once the app's caps include "leave".
      Orca::Online::RequestLeave();
    }
    else if (cmd == "delay")
    {
      // This player's input delay: a fixed 1 to max_delay frames, or "auto" for Orca's own default
      // (a fixed 3, Rollback/OnlineMatch.cpp DEFAULT_DELAY; never adaptive). Applies while the
      // app's caps include "delay"; mid-match, from the next frame.
      std::string value;
      in >> value;
      int frames = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frames);
      if (value == "auto")
        Orca::Online::SetChosenDelay(std::nullopt);
      else if (error == std::errc{} && end == value.data() + value.size() && frames >= 1 &&
               frames <= Orca::Net::Config{}.max_delay)
        Orca::Online::SetChosenDelay(frames);
      else
        Embed::Out("unsupported delay");
    }
    else if (cmd == "direct")
    {
      // "direct on|off": the player's switch for direct links to other players. Applies at once
      // while the app's caps include "direct"; the relay carries every input either way.
      // See ORCA.md, "Direct links".
      std::string value;
      in >> value;
      if (value == "on" || value == "off")
        Orca::Online::SetDirectWanted(value == "on");
      else
        Embed::Out("unsupported direct");
    }
    else if (cmd == "ping")
    {
      // Online stats in the overlay, off by default: "on" shows ping, "full" adds delay and
      // rollbacks per second.
      std::string value;
      in >> value;
      if (value == "on" || value == "off" || value == "full")
        Orca::UX::SetStats(value == "on"   ? Orca::UX::Stats::Ping :
                           value == "full" ? Orca::UX::Stats::Full :
                                             Orca::UX::Stats::Off);
    }
    else if (cmd == "perf")
    {
      // The overlay's frame meter, solo too: "fps" shows frames in the last second and the longest
      // frame, "detailed" adds a graph of the last 2 s.
      std::string value;
      in >> value;
      if (value == "off" || value == "fps" || value == "detailed")
        Orca::UX::SetPerf(value == "fps"      ? Orca::UX::Perf::Fps :
                          value == "detailed" ? Orca::UX::Perf::Detailed :
                                                Orca::UX::Perf::Off);
      else
        Embed::Out("unsupported perf");
    }
    else if (cmd == "controls")
    {
      // The player's in-game controls from the page (UX/NameTags.h): their tag gets them at the
      // next character select, and a game they join or host carries them.
      std::string hex;
      in >> hex;
      if (!Orca::UX::SetOwnControlsFromApp(hex))
        Embed::Out("unsupported controls");
    }
    else if (cmd == "chat")
    {
      // A chat line from the page, shown as a toast while Orca fills the screen (UX/Chat.h).
      // Players' words are never printed or logged.
      if (std::optional<Orca::UX::Chat::Line> chat = Orca::UX::Chat::Parse(line))
        Orca::UX::ShowChat(std::move(chat->name), std::move(chat->text));
      else
        Embed::Out("unsupported chat");
    }
    else if (cmd == "feedback")
    {
      // The app sends this shortcut only from its focused view; Orca never polls for it.
      if (Orca::Status::Cap(Orca::Status::HOSTUI_CAP))
        Orca::UX::YgOrb::ActivateHost(Orca::UX::YgOrb::HostAction::Feedback);
    }
    else if (cmd == "host-menu")
    {
      Orca::UX::YgOrb::ToggleHostMenu(
          IsEmbedded() ? +[](std::string_view line) { Embed::Out(std::string(line)); } : nullptr);
    }
    else if (cmd == "orb")
    {
      // Draws the YouGame button where the page's own sits under Orca (UX/YgOrb.h).
      if (std::optional<Orca::UX::YgOrb::Command> orb = Orca::UX::YgOrb::ParseOrb(line))
        Orca::UX::YgOrb::Apply(*orb);
      else
        Embed::Out("unsupported orb");
    }
    else if (cmd == "notice")
    {
      // A notice beside the YouGame button, e.g. someone joined or wrote. Players' words are never
      // printed or logged.
      if (std::optional<Orca::UX::YgOrb::Notice> notice = Orca::UX::YgOrb::ParseNotice(line))
        Orca::UX::YgOrb::Show(std::move(*notice));
      else
        Embed::Out("unsupported notice");
    }
    else if (cmd == "talk")
    {
      // Hold-to-talk's row beside the YouGame button, replaced by each line, with the app's "talk"
      // cap (UX/YgOrb.h). Players' words are never printed or logged.
      std::optional<Orca::UX::YgOrb::Talk> talk = Orca::UX::YgOrb::ParseTalk(line);
      if (talk && Orca::Status::Cap(Orca::Status::TALK_CAP))
        Orca::UX::YgOrb::SetTalk(std::move(*talk));
      else
        Embed::Out("unsupported talk");
    }
    else if (cmd == "test-input" && Orca::GetEnv("ORCA_TEST_COMMANDS") == "1")
    {
      std::string path;
      std::getline(in >> std::ws, path);
      if (!Rollback::Harness::SetInputScriptForTest(path))
        Embed::Out("unsupported test-input");
    }
    else if (cmd == "test-shot" && Orca::GetEnv("ORCA_TEST_COMMANDS") == "1")
    {
      // Tests only: save a screenshot <name>.png (a-z 0-9 - _) at the next present into the user
      // ScreenShots folder. With ORCA_TEST_PRESENT the overlay is included.
      std::string name;
      in >> name;
      const bool valid = !name.empty() && name.size() <= 64 &&
                         std::all_of(name.begin(), name.end(), [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                                  c == '_';
                         });
      const std::string dir = File::GetUserPath(D_SCREENSHOTS_IDX);
      if (valid && g_frame_dumper && File::CreateFullPath(dir))
        g_frame_dumper->SaveScreenshot(dir + name + ".png");
      else
        Embed::Out("unsupported test-shot");
    }
    else if (cmd == "test-drop-room" && Orca::GetEnv("ORCA_TEST_COMMANDS") == "1")
    {
      // Tests only: drop the room connection as a network failure would.
      Orca::Online::TestDropRoom();
    }
    else if (cmd == "test-direct" && Orca::GetEnv("ORCA_TEST_COMMANDS") == "1")
    {
      // Tests only: make direct links drop datagrams ("off", "in", "out"), stop dropping ("on"),
      // or reconnect ("rebuild").
      std::string value;
      in >> value;
      Orca::Online::TestDirect(value);
    }
    else if (cmd == "prepare-join")
    {
      // The host invited a friend who will drop into this game.
      Orca::Online::PrepareJoin();
    }
    else if (cmd == "reset")
    {
      // Refused in a session: the reset button would reach only this player's game.
      if (Orca::SessionActive() || !Core::IsRunning(system))
        Embed::Out("unsupported reset");
      else
        system.GetProcessorInterface().ResetButton_Tap();
    }
    else if (!cmd.empty())
    {
      fmt::print(stderr, "Orca: unknown embed command: {}\n", cmd);
    }
  }
}
