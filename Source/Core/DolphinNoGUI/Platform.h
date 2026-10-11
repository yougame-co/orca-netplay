// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "Common/Flag.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Orca/Bridge.h"
#include "DolphinNoGUI/Embed.h"

class Platform
{
public:
  virtual ~Platform();

  bool IsRunning() const { return m_running.IsSet(); }
  bool IsWindowFocused() const { return m_window_focus; }
  bool IsWindowFullscreen() const { return m_window_fullscreen; }

  virtual bool Init();
  virtual void SetTitle(const std::string& title);
  virtual void MainLoop() = 0;

  virtual WindowSystemInfo GetWindowSystemInfo() const = 0;

  // Requests a window title from any thread; the main loop applies it.
  void ShowTitle(const std::string& title);

  // Requests a graceful shutdown, from SIGINT/SIGTERM.
  void RequestShutdown();

  // Request an immediate shutdown.
  void Stop();

  // Embed mode (Embed.h): draw inside the parent window. Call before Init.
  void SetEmbed(const Embed::Options& options);
  bool IsEmbedded() const { return m_embed.enabled; }
  // Tests: accept embed commands on stdin without embedding (ORCA_TEST_COMMANDS=1).
  void EnableTestCommands() { m_test_commands = true; }
  // Called when the first frame is presented; "ready" is sent on the next main loop pass.
  void NotifyFirstFrame(int width, int height, double fps);
  // Hides the view and drops focus once the main loop ends, while emulation stops.
  void EmbedTeardown();

  static std::unique_ptr<Platform> CreateHeadlessPlatform();
#ifdef HAVE_X11
  static std::unique_ptr<Platform> CreateX11Platform();
#endif

#ifdef __linux__
  static std::unique_ptr<Platform> CreateFBDevPlatform();
#endif

#ifdef _WIN32
  static std::unique_ptr<Platform> CreateWin32Platform();
#endif

#ifdef __APPLE__
  static std::unique_ptr<Platform> CreateMacOSPlatform();

  // Orca.app: native dialogs shown before the game window opens. ChooseFileMacOS returns nullopt
  // if the player cancels.
  static std::optional<std::string> ChooseFileMacOS(const std::string& message);
  static void ShowErrorMacOS(const std::string& title, const std::string& message);
#endif

protected:
  void UpdateRunningFlag();
  // On the main loop's thread: runs commands read from stdin and sends "ready" once.
  void ProcessEmbedCommands();
  // Ends a solo pause when drop-in needs the game running (a friend arriving, an invite, a join or
  // leave) and tells the app "state running".
  void EndSoloPauseForDropIn();
  // During a solo pause, every 2 s: watches this game's room as a boundary would
  // (Rollback::OnlineMatch::WatchRoomWhilePaused), so a room lost while paused opens again.
  void WatchRoomWhilePaused();
  // Per-platform handlers for the window commands. The rect is relative to the parent; its units
  // depend on the OS (see ORCA.md, "Embedding").
  virtual void EmbedSetRect(const Embed::Rect& rect) {}
  virtual void EmbedSetVisible(bool visible) {}
  virtual void EmbedSetFocus(bool focus) {}
  // Shows the view only when the app said "show" and the rect is big enough.
  void ApplyEmbedVisible();
  // Tells the presenter where to lay out the picture: the `view` box relative to the window
  // (`rect`), or the whole window when there is no view.
  void ApplyEmbedView();
  // Applies the newest requested title on the main loop's thread. In a session it also refreshes
  // the title (at most every 250 ms) to follow the room status.
  void UpdateTitle();

  Common::Flag m_running{true};
  Common::Flag m_shutdown_requested{false};
  Common::Flag m_tried_graceful_shutdown{false};

  // Atomic: the CPU thread reads it through Host_RendererHasFocus.
  std::atomic<bool> m_window_focus = true;
  bool m_window_fullscreen = false;

  Embed::Options m_embed;
  bool m_test_commands = false;

private:
  // Titles are set only on the main loop's thread. On Win32, setting one from the emulation thread
  // blocks on the main thread, which could deadlock with this lock.
  std::mutex m_title_lock;
  std::optional<std::string> m_requested_title;
  std::string m_shown_title;
  std::chrono::steady_clock::time_point m_next_title_check{};
  std::chrono::steady_clock::time_point m_next_room_watch{};
  bool m_room_watch_lost = false;
  // `orca alive` to the app on the stdio bridge (Orca/Bridge.h Heartbeat): UpdateRunningFlag.
  Orca::Bridge::Heartbeat m_heartbeat;

  std::mutex m_ready_lock;
  std::optional<std::string> m_ready_line;
  bool m_ready_sent = false;
  // The view is visible only when the app's last command was "show" and the last rect wasn't
  // TooSmall.
  bool m_embed_shown = true;
  bool m_embed_too_small = false;
  // Where the picture is laid out, in `rect`'s units and origin. Unset means the whole window.
  // m_embed_rect_known is false until --rect or a `rect` command gives one.
  std::optional<Embed::Rect> m_embed_view;
  bool m_embed_rect_known = false;
  // A "pause" that arrived before the game was running, applied once it is. Never set in a session.
  bool m_pause_pending = false;
};
