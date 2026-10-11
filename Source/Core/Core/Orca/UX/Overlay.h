// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Core/Orca/Session/Events.h"

// The online overlay: only what the game has nothing native for. A connection circle styled after
// Brawl's Wi-Fi icon, "Waiting for <name>...", join/leave toasts, the ping, a frame meter and the
// app's chat. It is drawn with ImGui over the presented image, never into the emulated framebuffer,
// and reads no game state, so nothing in it can change emulation or desync.
namespace Orca::UX
{
// The readout beside the circle.
enum class Stats
{
  Off,
  Ping,  // "32 ms"
  Full,  // "32 ms · delay 2 · 1.2 rb/s"
};

// What the frame meter shows. Defaults to Fps.
enum class Perf
{
  Off,
  Fps,       // "60 fps · 17 ms"
  Detailed,  // adds a 2 s frame-time graph, and online the delay and rollbacks/s
};

enum class Signal
{
  Good,
  Ok,
  Bad,
};

// The link stats the overlay shows.
struct LinkView
{
  bool online = false;
  int delay = 0;
  double rollbacks_per_second = 0;
  int deepest_rollback = 0;
  int stalls = 0;  // total frames waited
  int round_trip_ms = -1;  // of the direct link when up, else of the relay
  int transport = 0;       // 0 relay, 1 direct, 2 TURN
  int stalled_ms = 0;     // current stall, 0 if none, -1 if unknown
  int waiting_seat = -1;  // seat being waited for (seat s is GameCube port s)
};
LinkView ViewOf(const Events::LinkStats& stats);

// What one frame of the overlay shows. A pure state machine, so tests can drive it with a clock.
class OverlayModel
{
public:
  static constexpr double TOAST_MS = 3000;
  static constexpr double TOAST_FADE_MS = 500;
  static constexpr int WAITING_AFTER_MS = 250;
  // Frame meter thresholds. Ok below 59 fps or a frame over 20 ms; Bad below 50 fps or a frame
  // over two frame periods (a hitch). The colour holds the worst of the last two seconds.
  static constexpr double PERF_WINDOW_MS = 1000;
  static constexpr double PERF_GRAPH_MS = 2000;
  static constexpr double PERF_HOLD_MS = 2000;
  static constexpr int PERF_OK_FPS = 59;
  static constexpr double PERF_OK_FRAME_MS = 20;
  static constexpr int PERF_BAD_FPS = 50;
  static constexpr double PERF_BAD_FRAME_MS = 2000.0 / 60;
  // More unshown frames in a row than any rollback re-runs means a joiner is catching up.
  static constexpr int PERF_MAX_UNSHOWN = 60;

  // A chat line shows this long, fading like a toast. Only the newest CHAT_KEEP show.
  static constexpr double CHAT_MS = 8000;
  static constexpr std::size_t CHAT_KEEP = 3;

  struct Toast
  {
    std::string text;
    float alpha;
    double at = 0;  // arrival time on the overlay's clock, for ordering
  };
  struct ChatToast
  {
    std::string name;
    std::string text;
    float alpha;
    double at = 0;
  };
  struct Frame
  {
    bool show_signal = false;
    Signal signal = Signal::Good;
    std::string waiting;  // empty if not shown
    std::vector<Toast> toasts;
    std::vector<ChatToast> chats;  // oldest first
    std::string stats;  // empty if not shown
    std::string perf;   // frame meter, empty if not shown
    Signal perf_level = Signal::Good;  // Good means plain text
    std::vector<float> graph;          // Detailed only: last 2 s of frame times (ms), oldest first
  };

  // Called when the port list changes. Any thread. An unchanged list shows nothing new.
  void OnPorts(int frame, const std::vector<Events::PortInfo>& ports, double now_ms);
  // A custom toast, such as a match result. Any thread.
  void AddToast(std::string text, double now_ms);
  // A decoded line of the app's chat. Any thread.
  void AddChat(std::string name, std::string text, double now_ms);
  // The newest link stats. Drawing thread, each frame.
  void Update(const LinkView& link, double now_ms);
  // Every frame boundary, on the CPU thread. `shown` is true for a rendered first run, not a
  // rollback re-run or catch-up. Re-runs lengthen the measured frame time but never count as
  // frames. The first shown boundary only arms the meter, since the one before may include a wait.
  // `game_held_ms` (Events::GameHeldMs) is time the game itself held a shown frame, such as a load,
  // and `game_held_frames` those video frames (Events::GameHeldFrames): the meter counts them as
  // shown, so it shows only this machine falling behind.
  void OnFrame(double now_ms, bool shown, double game_held_ms = 0, int game_held_frames = 0);
  // The core paused or resumed (host thread). The meter's clock stands still while paused.
  void Pause(double now_ms);
  void Resume(double now_ms);
  Frame View(double now_ms, Stats stats, Perf perf = Perf::Off) const;

private:
  Signal Quality(double now_ms) const;
  void Meter(double now_ms, Stats stats, Perf perf, Frame* f) const;
  // Host time minus time spent paused.
  double RunningMs(double now_ms) const;

  mutable std::mutex m_lock;
  bool m_seen_ports = false;
  std::vector<Events::PortInfo> m_ports;
  std::deque<std::pair<double, std::string>> m_toasts;
  struct ChatEntry
  {
    double at;
    std::string name;
    std::string text;
  };
  std::deque<ChatEntry> m_chats;
  LinkView m_link;
  std::deque<std::pair<double, int>> m_stall_history;  // (time, total stalls)
  // Shown frames (boundary, frame time ms) over the last PERF_WINDOW_MS + PERF_HOLD_MS, on the
  // RunningMs clock. A frame the game itself held is that many frames here (OnFrame).
  std::deque<std::pair<double, double>> m_frames;
  double m_first_frame = -1;  // meter start; no readout for the first second
  double m_last_frame = -1;   // last shown boundary, -1 if not armed
  int m_unshown = 0;          // unshown frames since then
  bool m_paused = false;
  double m_paused_at = 0;     // host time
  double m_paused_ms = 0;     // total time in finished pauses
};

// Registers the overlay with the OSD layer and the session.
void InitOverlay();
void ShutdownOverlay();
// Called from the session's stall wait on the CPU thread, which blocks presenting. Once the stall
// nears 250 ms it re-presents the last frame with the overlay, about 15 times a second. Host-only.
void RepresentDuringStall(int stalled_ms);
// The app hid or showed the game. No stall redraws while hidden.
void SetHostVisible(bool visible);
bool HostVisible();
// Dims the frame N percent black (0-100, 0 is off) while the app's menu is open over the game.
// Drawn last on every present and eased over DIM_EASE_S. Host-only, so screenshots and frame dumps
// never include it. Any thread.
void SetDim(int percent);
int Dim();
constexpr double DIM_EASE_S = 0.2;
void SetStats(Stats stats);
void SetPerf(Perf perf);
// The game's frame boundary, for the frame meter.
void FrameBoundary(bool shown, double game_held_ms, int game_held_frames);
// A short toast under the circle. Any thread.
void ShowToast(std::string text);
// A line of the app's chat. Never logged. Any thread.
void ShowChat(std::string name, std::string text);
// A ranked set's lines, top centre: the set score, and whose stage-select turn it is with its
// timer. Empty hides them. Any thread.
void SetRulesLines(std::string line, std::string note);
// The character order line under the set score, e.g. "Ada picks first · 0:42". Empty hides it.
// Display only. Any thread.
void SetRuleLine(std::string text);
// The queue's character select: two top-centre lines, a label per port near its panel
// ("ada · 1532"), and a LOCKED IN ribbon for each `locked` port. Empty hides. Display only. Any
// thread.
void SetQueueLines(std::string line, std::string note, std::array<std::string, 4> labels,
                   std::array<bool, 4> locked = {});
// Port 0-3's panel colour in the games: red, blue, yellow, green.
struct PortColour
{
  int r, g, b;
};
PortColour PortColourOf(int port);
OverlayModel& Overlay();
// The overlay's unit, 1% of the picture's height, or 0 (draw nothing) below OVERLAY_MIN_HEIGHT.
// ImGui aborts on a text size that rounds to 0 pixels, which a very short window would reach.
constexpr float OVERLAY_MIN_HEIGHT = 100;
float OverlayUnit(float display_height);
// A box on the display, in pixels.
struct PictureArea
{
  float x = 0, y = 0, w = 0, h = 0;
};
// The part of the picture inside the display. The picture can extend past the window when the
// app covers part of it. Free-standing overlay elements stay in the visible part; marks on the
// game's own art use the whole picture. Returns the whole picture if none of it is visible.
PictureArea VisiblePicture(const PictureArea& picture, float display_w, float display_h);
// Tests only: overrides the presenter's target rectangle. nullopt restores it. Drawing thread.
void SetPictureForTests(std::optional<PictureArea> picture);
}  // namespace Orca::UX
