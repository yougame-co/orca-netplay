// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/HookableEvent.h"
#include "Core/Core.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/OrbCombo.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/UX/Search.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/UX.h"
#include "Core/Orca/UX/YgCard.h"
#include "Core/Orca/UX/YgOrbDraw.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

namespace Orca::UX
{
namespace
{
// The window of recent stalls that decides the signal.
constexpr double STALL_WINDOW_MS = 3000;

double NowMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Read these fields only if LinkStats has them; otherwise the stall text stays off.
template <typename T>
int StalledMs(const T& stats)
{
  if constexpr (requires { stats.stalled_ms; })
    return stats.stalled_ms;
  else
    return -1;
}
template <typename T>
int WaitingSeat(const T& stats)
{
  if constexpr (requires { stats.waiting_seat; })
    return stats.waiting_seat;
  else
    return -1;
}

std::string NameOf(const Events::PortInfo& p)
{
  return p.name.empty() ? fmt::format("Player {}", p.port + 1) : p.name;
}

// The frame meter's colour, from frames shown in a second and whether any was a hitch or late.
Signal LevelOf(int fps, bool hitch, bool late)
{
  if (fps < OverlayModel::PERF_BAD_FPS || hitch)
    return Signal::Bad;
  if (fps < OverlayModel::PERF_OK_FPS || late)
    return Signal::Ok;
  return Signal::Good;
}

Signal Worse(Signal a, Signal b)
{
  return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}
}  // namespace

LinkView ViewOf(const Events::LinkStats& stats)
{
  LinkView v;
  v.online = stats.online;
  v.delay = stats.delay;
  v.rollbacks_per_second = stats.rollbacks_per_second;
  v.deepest_rollback = stats.deepest_rollback;
  v.stalls = stats.stalls;
  // Inputs travel the direct (or TURN) link once it is up, so its round trip is the ping shown.
  v.transport = stats.link_rtt_ms >= 0 ? stats.transport : 0;
  v.round_trip_ms = v.transport > 0 ? stats.link_rtt_ms : stats.round_trip_ms;
  v.stalled_ms = StalledMs(stats);
  v.waiting_seat = WaitingSeat(stats);
  return v;
}

void OverlayModel::OnPorts(int frame, const std::vector<Events::PortInfo>& ports, double now_ms)
{
  std::lock_guard lk(m_lock);
  // A drop-in join can move the frame number backwards, so frames don't order the lists.
  (void)frame;
  const auto find = [](const std::vector<Events::PortInfo>& list, int port) {
    return std::find_if(list.begin(), list.end(),
                        [port](const Events::PortInfo& p) { return p.port == port; });
  };
  const bool first = !m_seen_ports;
  m_seen_ports = true;
  for (const Events::PortInfo& p : ports)
  {
    if (!p.remote)
      continue;
    const auto was = find(m_ports, p.port);
    if (was != m_ports.end() && was->name == p.name && was->remote)
      continue;
    // A joiner's first list already has the host, who is not joining.
    m_toasts.emplace_back(now_ms, first ? fmt::format("{} is on port {}", NameOf(p), p.port + 1) :
                                          fmt::format("{} joined on port {}", NameOf(p), p.port + 1));
  }
  for (const Events::PortInfo& p : m_ports)
  {
    if (!p.remote)
      continue;
    const auto now = find(ports, p.port);
    if (now == ports.end() || now->name != p.name)
      m_toasts.emplace_back(now_ms, fmt::format("{} left", NameOf(p)));
  }
  m_ports = ports;
  while (m_toasts.size() > 3)
    m_toasts.pop_front();
}

void OverlayModel::AddToast(std::string text, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_toasts.emplace_back(now_ms, std::move(text));
  while (m_toasts.size() > 3)
    m_toasts.pop_front();
}

void OverlayModel::AddChat(std::string name, std::string text, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_chats.push_back({now_ms, std::move(name), std::move(text)});
  while (m_chats.size() > CHAT_KEEP)
    m_chats.pop_front();
}

void OverlayModel::Update(const LinkView& link, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_link = link;
  m_stall_history.emplace_back(now_ms, link.stalls);
  while (m_stall_history.size() > 1 && m_stall_history.front().first < now_ms - STALL_WINDOW_MS)
    m_stall_history.pop_front();
  while (!m_toasts.empty() && m_toasts.front().first < now_ms - TOAST_MS)
    m_toasts.pop_front();
  while (!m_chats.empty() && m_chats.front().at < now_ms - CHAT_MS)
    m_chats.pop_front();
}

Signal OverlayModel::Quality(double) const
{
  const int recent_stalls = m_stall_history.empty() ?
                                0 :
                                m_stall_history.back().second - m_stall_history.front().second;
  const int rtt = m_link.round_trip_ms;
  if (m_link.stalled_ms > WAITING_AFTER_MS || rtt >= 150 || recent_stalls >= 10)
    return Signal::Bad;
  if (rtt >= 80 || recent_stalls > 0)
    return Signal::Ok;
  return Signal::Good;
}

double OverlayModel::RunningMs(double now_ms) const
{
  return (m_paused ? m_paused_at : now_ms) - m_paused_ms;
}

void OverlayModel::OnFrame(double now_ms, bool shown, double game_held_ms, int game_held_frames)
{
  std::lock_guard lk(m_lock);
  if (!shown)
  {
    ++m_unshown;
    return;
  }
  const double now = RunningMs(now_ms);
  // A long run of unshown frames is a joiner catching up, not the game's pace. Restart the meter.
  if (std::exchange(m_unshown, 0) > PERF_MAX_UNSHOWN)
  {
    m_frames.clear();
    m_first_frame = -1;
    m_last_frame = -1;
  }
  if (m_first_frame < 0)
  {
    if (m_last_frame >= 0)
      m_first_frame = now;
  }
  else
  {
    // A frame the game itself held (a load) reads as the video frames it held, spread over it, and
    // only time past the hold is this machine's.
    constexpr double frame_ms = 1000 / 59.94;
    const double ms = now - m_last_frame;
    const int held = std::max(0, game_held_frames);
    for (int k = 1; k <= held; ++k)
      m_frames.emplace_back(m_last_frame + ms * k / (held + 1), std::min(ms, frame_ms));
    m_frames.emplace_back(now, std::max(ms - game_held_ms, std::min(ms, frame_ms)));
  }
  m_last_frame = now;
  while (!m_frames.empty() && m_frames.front().first <= now - PERF_WINDOW_MS - PERF_HOLD_MS)
    m_frames.pop_front();
}

void OverlayModel::Pause(double now_ms)
{
  std::lock_guard lk(m_lock);
  if (m_paused)
    return;
  m_paused = true;
  m_paused_at = now_ms;
}

void OverlayModel::Resume(double now_ms)
{
  std::lock_guard lk(m_lock);
  if (!m_paused)
    return;
  m_paused = false;
  m_paused_ms += std::max(0.0, now_ms - m_paused_at);
}

void OverlayModel::Meter(double now_ms, Stats stats, Perf perf, Frame* f) const
{
  const double now = RunningMs(now_ms);
  if (perf == Perf::Off || m_first_frame < 0 || now < m_first_frame + PERF_WINDOW_MS)
    return;
  // The colour is the worst readout at any boundary in the last PERF_HOLD_MS. `hitch` and `late`
  // index the newest frames past each threshold (-1 if none).
  std::size_t first = 0;
  std::ptrdiff_t hitch = -1, late = -1;
  const auto level_at = [&](std::size_t end, double t) {
    while (first < end && m_frames[first].first <= t - PERF_WINDOW_MS)
      ++first;
    const auto oldest = static_cast<std::ptrdiff_t>(first);
    return LevelOf(static_cast<int>(end - first), hitch >= oldest, late >= oldest);
  };
  Signal level = Signal::Good;
  std::size_t end = 0;
  for (; end < m_frames.size() && m_frames[end].first <= now; ++end)
  {
    const auto [at, ms] = m_frames[end];
    if (ms > PERF_BAD_FRAME_MS)
      hitch = static_cast<std::ptrdiff_t>(end);
    if (ms > PERF_OK_FRAME_MS)
      late = static_cast<std::ptrdiff_t>(end);
    if (at > now - PERF_HOLD_MS && at >= m_first_frame + PERF_WINDOW_MS)
      level = Worse(level, level_at(end + 1, at));
  }
  // A frame still running (waiting for a friend) counts as at least this long.
  level_at(end, now);
  const int fps = static_cast<int>(end - first);
  double longest_ms = std::max(0.0, now - m_last_frame);
  for (std::size_t i = first; i < end; ++i)
    longest_ms = std::max(longest_ms, m_frames[i].second);
  f->perf_level = Worse(level, LevelOf(fps, longest_ms > PERF_BAD_FRAME_MS,
                                       longest_ms > PERF_OK_FRAME_MS));
  f->perf = fmt::format("{} fps · {:.0f} ms", fps, longest_ms);
  if (perf != Perf::Detailed)
    return;
  // Stats::Full already shows these.
  if (m_link.online && stats != Stats::Full)
    f->perf += fmt::format(" · delay {} · {:.1f} rb/s", m_link.delay, m_link.rollbacks_per_second);
  for (std::size_t i = 0; i < end; ++i)
  {
    if (m_frames[i].first > now - PERF_GRAPH_MS)
      f->graph.push_back(static_cast<float>(m_frames[i].second));
  }
}

OverlayModel::Frame OverlayModel::View(double now_ms, Stats stats, Perf perf) const
{
  std::lock_guard lk(m_lock);
  Frame f;
  f.show_signal = m_link.online;
  f.signal = Quality(now_ms);
  if (m_link.online && m_link.stalled_ms > WAITING_AFTER_MS)
  {
    std::string who = "your friend";
    for (const Events::PortInfo& p : m_ports)
    {
      if (p.port == m_link.waiting_seat && p.remote)
        who = NameOf(p);
    }
    f.waiting = fmt::format("Waiting for {}…", who);
  }
  for (const auto& [at, text] : m_toasts)
  {
    const double left = at + TOAST_MS - now_ms;
    if (left <= 0)
      continue;
    f.toasts.push_back(
        {text, static_cast<float>(std::clamp(left / TOAST_FADE_MS, 0.0, 1.0)), at});
  }
  for (const ChatEntry& c : m_chats)
  {
    const double left = c.at + CHAT_MS - now_ms;
    if (left <= 0)
      continue;
    f.chats.push_back(
        {c.name, c.text, static_cast<float>(std::clamp(left / TOAST_FADE_MS, 0.0, 1.0)), c.at});
  }
  if (stats != Stats::Off && m_link.online)
  {
    const std::string ping = m_link.round_trip_ms >= 0 ?
                                 fmt::format("{} ms", m_link.round_trip_ms) :
                                 std::string("– ms");
    const char* path = m_link.transport == 1 ? " direct" : m_link.transport == 2 ? " TURN" : "";
    f.stats = stats == Stats::Full ? fmt::format("{}{} · delay {} · {:.1f} rb/s", ping, path,
                                                 m_link.delay, m_link.rollbacks_per_second) :
                                     ping;
  }
  Meter(now_ms, stats, perf, &f);
  return f;
}

namespace
{
std::mutex s_rules_lock;
std::string s_rules_line;
std::string s_rules_note;
OverlayModel s_overlay;
std::atomic<Stats> s_stats{Stats::Off};
// On until the app turns it off.
std::atomic<Perf> s_perf{Perf::Fps};
Common::EventHook s_state_hook;  // core pause/resume, for the frame meter
std::atomic<bool> s_visible{true};  // whether the app shows the game
// The requested dim percent, and the level last drawn, eased toward it on the video thread.
// `s_dim_at` is the ImGui time of that draw (-1 if none yet).
std::atomic<int> s_dim{0};
float s_dim_shown = 0;
double s_dim_at = -1;
std::optional<PictureArea> s_test_picture;
std::mutex s_rule_line_lock;
std::string s_rule_line;
std::mutex s_queue_lock;
std::string s_queue_line, s_queue_note;
std::array<std::string, 4> s_queue_labels;
std::array<bool, 4> s_queue_locked{};
// ORCA_UX_OVERLAY_DEMO=1, for design checks: a fake friend whose link cycles good, ok, then bad
// with a stall every 12 s, in place of the session's stats.
bool s_demo = false;
double s_demo_start = -1;

LinkView DemoLink(double now)
{
  if (s_demo_start < 0)
  {
    s_demo_start = now;
    s_overlay.OnPorts(0, {{0, "bo", false}}, now);
    s_overlay.OnPorts(1, {{0, "bo", false}, {1, "ada", true}}, now);
  }
  const double t = std::fmod(now - s_demo_start, 12000);
  static int s_chat_round = -1;
  const int round = static_cast<int>((now - s_demo_start) / 12000);
  if (round != s_chat_round && t >= 1500)
  {
    s_chat_round = round;
    s_overlay.AddToast("ada joined on port 2", now);
    s_overlay.AddChat("ada", "gl hf!", now);
    s_overlay.AddChat("ada", "one more after this? I want a rematch on Battlefield", now);
  }
  LinkView v;
  v.online = true;
  v.delay = 2;
  v.rollbacks_per_second = t < 4000 ? 0.8 : 6.5;
  v.round_trip_ms = t < 4000 ? 35 : t < 8000 ? 95 : 180;
  v.stalled_ms = t >= 8000 ? static_cast<int>(t - 8000) : 0;
  v.waiting_seat = 1;
  return v;
}

ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// The frame meter's colours: white at full pace, then yellow and red.
ImU32 LevelColour(Signal level, float alpha)
{
  return level == Signal::Bad ? Rgba(255, 72, 64, alpha) :
         level == Signal::Ok  ? Rgba(255, 208, 64, alpha) :
                                Rgba(255, 255, 255, alpha);
}

// The frame-time graph: a bar per shown frame of the last two seconds, newest on the right, capped
// at 50 ms, with a line at one frame period.
void FrameGraph(ImDrawList* dl, ImVec2 pos, ImVec2 size, float unit,
                const std::vector<float>& frames)
{
  constexpr float TOP_MS = 50;
  constexpr float SLOTS = OverlayModel::PERF_GRAPH_MS * 60 / 1000;
  Kit::Plate(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), unit, Kit::Tone::Panel, 0.9f);
  const float pad = 0.9f * unit;
  pos = ImVec2(pos.x + pad, pos.y + pad);
  size = ImVec2(size.x - 2 * pad, size.y - 2 * pad);
  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Rgba(0, 0, 0, 0.45f),
                    0.4f * unit);
  const float slot = size.x / SLOTS;
  // Only bars a few pixels wide get a gap. Thinner ones would alias away.
  const float gap = slot >= 3 ? slot * 0.2f : 0;
  const float bottom = pos.y + size.y;
  float x = pos.x + size.x;
  for (auto it = frames.rbegin(); it != frames.rend() && x - slot >= pos.x - 0.5f; ++it)
  {
    const float ms = *it;
    const Signal level = ms > OverlayModel::PERF_BAD_FRAME_MS ? Signal::Bad :
                         ms > OverlayModel::PERF_OK_FRAME_MS  ? Signal::Ok :
                                                                Signal::Good;
    const float h = std::max(1.0f, std::min(ms, TOP_MS) / TOP_MS * size.y);
    dl->AddRectFilled(ImVec2(x - slot, bottom - h), ImVec2(x - gap, bottom),
                      level == Signal::Good ? Rgba(170, 220, 255, 0.85f) : LevelColour(level, 1));
    x -= slot;
  }
  const float period_y = bottom - 1000.0f / 60 / TOP_MS * size.y;
  dl->AddLine(ImVec2(pos.x, period_y), ImVec2(pos.x + size.x, period_y),
              Rgba(255, 255, 255, 0.4f), std::max(1.0f, size.y / 40));
}

// The centre of a port's character select panel, as a fraction of the picture's width. Measured
// on widescreen 1280x720 in both games.
float PanelCentre(int port)
{
  return 0.236f + 0.177f * static_cast<float>(port);
}

// The top of the name tab over a panel, as a fraction of the picture's height.
constexpr float NAME_TAB_Y = 0.575f;

// A port's portrait on the character select, where the lock-in frame and ribbon go. Measured on
// 1280x720 screenshots: 200 px wide, Brawl y 466-622, Project+ y 432-628.
Kit::Box PortraitRect(int port, ImVec2 pic, ImVec2 pic_size)
{
  const bool pplus = Kit::CurrentLook() == Kit::Look::ProjectPlus;
  const float cx = pic.x + pic_size.x * PanelCentre(port);
  const float half = pic_size.x * 0.078f;
  return {ImVec2(cx - half, pic.y + pic_size.y * (pplus ? 0.6f : 0.647f)),
          ImVec2(cx + half, pic.y + pic_size.y * (pplus ? 0.872f : 0.864f))};
}

// The vertical middle of the set's score dots, in the games' dark top band.
constexpr float DOTS_Y = 1.9f;  // overlay units
// Where the top-centre banners start, lower when the dots show.
constexpr float TOP_FIRST = 1.6f, TOP_UNDER_DOTS = 3.9f;  // overlay units

// Timers drawn in the game's lettering, measured on 1280x720 screenshots. The band timer sits at
// the right end of READY TO FIGHT!; the line timer starts just past the STAGE SELECT label.
struct GameTimerPlace
{
  float x, y, size;
};
constexpr GameTimerPlace BAND_TIMER_BRAWL{0.927f, 0.6125f, 5.2f};
constexpr GameTimerPlace BAND_TIMER_PPLUS{0.922f, 0.567f, 5.2f};
constexpr GameTimerPlace LINE_TIMER_BRAWL{0.371f, 0.935f, 4.4f};
constexpr GameTimerPlace LINE_TIMER_PPLUS{0.414f, 0.064f, 5.4f};
// The control guide's right edge and middle, where the games put their own button legends.
constexpr float GUIDE_RIGHT = 0.975f, GUIDE_Y = 0.969f;

// Uppercases ASCII letters, as the game's own name plates do.
std::string Upper(std::string_view name)
{
  std::string out(name);
  for (char& c : out)
  {
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

// The game's picture in the window (letterboxed or pillarboxed), or the whole display if the
// presenter has none.
void PictureRect(ImVec2* pic, ImVec2* pic_size)
{
  *pic = ImVec2(0, 0);
  *pic_size = ImGui::GetIO().DisplaySize;
  if (s_test_picture)
  {
    *pic = ImVec2(s_test_picture->x, s_test_picture->y);
    *pic_size = ImVec2(s_test_picture->w, s_test_picture->h);
  }
  else if (g_presenter)
  {
    const MathUtil::Rectangle<int>& r = g_presenter->GetTargetRectangle();
    if (r.GetWidth() > 0 && r.GetHeight() > 0)
    {
      *pic = ImVec2(static_cast<float>(r.left), static_cast<float>(r.top));
      *pic_size = ImVec2(static_cast<float>(r.GetWidth()), static_cast<float>(r.GetHeight()));
    }
  }
}

// The stage select's two cursors: each player's pointer and proposed stage in their colour, or one
// gold frame when both propose the same stage. Positions come from the match block.
void DrawStageCursors(ImDrawList* dl, const Orca::Engine::Display& d, ImVec2 pic, ImVec2 pic_size,
                      float unit, double now_s)
{
  namespace StageCursors = Orca::Engine::Cursors;
  const StageCursors::View& v = d.cursors;
  const StageCursors::Layout* layout = d.layout;
  if (!layout)
    return;
  const auto at16 = [&](int x16, int y16) {
    return ImVec2(
        pic.x + pic_size.x * (layout->u0 + layout->du * static_cast<float>(x16) / 16.0f),
        pic.y + pic_size.y * (layout->v0 + layout->dv * static_cast<float>(y16) / 16.0f));
  };
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (!p.present || !Ranked::Named(p.proposal))
      continue;
    // A stage's tile, or the Random button as drawn for a casual Random.
    const StageCursors::Rect* rect = d.proposals[port];
    if (p.proposal == Ranked::PREF_RANDOM)
      rect = layout->random_art.x0 < layout->random_art.x1 ? &layout->random_art :
                                                             &layout->random_button;
    if (!rect || rect->x0 >= rect->x1 || rect->y0 >= rect->y1)
      continue;
    const bool both = v.players[1 - port].present && v.players[1 - port].proposal == p.proposal;
    if (both && port == 1)
      continue;
    const ImVec2 c0 = at16(rect->x0, rect->y1), c1 = at16(rect->x1, rect->y0);
    Kit::Frame(dl, ImVec2(std::min(c0.x, c1.x), std::min(c0.y, c1.y)),
               ImVec2(std::max(c0.x, c1.x), std::max(c0.y, c1.y)), unit,
               both ? Kit::Tone::Gold : Kit::PortTone(port),
               both ? Upper(p.name) + " + " + Upper(v.players[1].name) : Upper(p.name), port == 0);
  }
  // Make whose turn it is very obvious with a blinking cursor on the acting player. The blink's
  // phase starts with the turn; a stage select that returns after a gap starts afresh.
  static std::array<StageCursors::TurnCue::Mode, 2> s_mode{};
  static std::array<u8, 2> s_kind{};
  static std::array<double, 2> s_since{};
  static double s_last = -1, s_opened = 0;
  if (s_last < 0 || now_s - s_last > 0.5)
  {
    s_mode = {};
    s_kind = {};
    s_opened = now_s;
  }
  s_last = now_s;
  std::array<StageCursors::TurnCue, 2> cues;
  for (int port = 0; port < 2; ++port)
  {
    cues[port] = d.cues[port];
    const bool acting = cues[port].mode == StageCursors::TurnCue::Mode::Acting;
    if (acting && (s_mode[port] != StageCursors::TurnCue::Mode::Acting || s_kind[port] != v.kind))
      s_since[port] = now_s;
    s_mode[port] = cues[port].mode;
    s_kind[port] = v.kind;
  }
  // Opaque and slightly larger than the game's own crosshair, to hide it. Port 1's tab goes left
  // and port 2's right, so two cursors on one tile both stay readable.
  const float radius = 1.7f * std::abs(layout->du) * pic_size.x;
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (p.present)
    {
      const Kit::PointerMode mode =
          cues[port].mode == StageCursors::TurnCue::Mode::Acting  ? Kit::PointerMode::Acting :
          cues[port].mode == StageCursors::TurnCue::Mode::Waiting ? Kit::PointerMode::Waiting :
                                                                    Kit::PointerMode::Plain;
      Kit::Pointer(dl, at16(p.x, p.y), radius, unit, port,
                   fmt::format("P{} {}", port + 1, Upper(p.name)), mode, port == 0, now_s,
                   s_since[port]);
    }
  }
  // Turn tags: the acting player's says what to do (gold and blinking on the local player's own
  // turn, steady in port colour otherwise); a waiting local player gets a small grey tab.
  const ImVec2 clamp_min(pic.x + 1.0f * unit, pic.y + 1.0f * unit);
  const ImVec2 clamp_max(pic.x + pic_size.x - 1.0f * unit, pic.y + pic_size.y - 1.0f * unit);
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    const StageCursors::TurnCue& cue = cues[port];
    if (!p.present || cue.mode == StageCursors::TurnCue::Mode::Idle)
      continue;
    if (cue.mode == StageCursors::TurnCue::Mode::Waiting)
    {
      Kit::TurnTag(dl, at16(p.x, p.y), radius, unit, cue.title, {}, Kit::Tone::Grey, -1, now_s,
                   port == 0, clamp_min, clamp_max, 0, 2.6f, true);
      continue;
    }
    const Kit::Blink blink = Kit::Attention(now_s, s_since[port]);
    const bool gold = cue.local && !v.both;
    Kit::TurnTag(dl, at16(p.x, p.y), radius, unit, cue.title, cue.detail,
                 gold ? Kit::Tone::Gold : Kit::PortTone(port), cue.seconds, now_s, port == 0,
                 clamp_min, clamp_max, cue.local ? blink.on : 0.0f);
  }
  // Brawl's Meta Knight clause (no strikes this game), shown large for the first 3 s.
  const std::string& mk = d.mk_note;
  const double shown = now_s - s_opened;
  if (!mk.empty() && shown < 3.0)
  {
    const float alpha = static_cast<float>(std::clamp(std::min(shown / 0.15, (3.0 - shown) / 0.4),
                                                      0.0, 1.0));
    const float height = 9.0f * unit;
    const char* title = "META KNIGHT CLAUSE";
    const float width = std::min(pic_size.x - 4.0f * unit,
                                 Kit::BannerWidth(height, unit, title, mk) + 6.0f * unit);
    const float cx = pic.x + pic_size.x / 2;
    Kit::Banner(dl, cx - width / 2, cx + width / 2, pic.y + pic_size.y * 0.45f - height / 2,
                height, unit, Kit::Tone::Panel, title, mk, alpha);
  }
}

// ORCA_UX_KIT_DEMO=1: draws the kit's primitives over the game, for design checks.
void DrawKitDemo()
{
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  const float unit = OverlayUnit(pic_size.y);
  if (unit <= 0 || !(pic_size.x >= 1))
    return;
  Kit::DrawDemo(ImGui::GetForegroundDrawList(), pic, pic_size, unit, ImGui::GetTime());
}

// Strips a trailing " · m:ss" timer from the line and returns its seconds, or -1 if none.
int TakeTimer(std::string* line)
{
  static const std::string sep = " · ";
  const std::size_t at = line->rfind(sep);
  if (at == std::string::npos)
    return -1;
  const std::string tail = line->substr(at + sep.size());
  const std::size_t colon = tail.find(':');
  if (colon == std::string::npos || colon == 0 || tail.size() != colon + 3)
    return -1;
  int minutes = 0;
  for (std::size_t i = 0; i < colon; ++i)
  {
    if (tail[i] < '0' || tail[i] > '9')
      return -1;
    minutes = minutes * 10 + (tail[i] - '0');
  }
  if (tail[colon + 1] < '0' || tail[colon + 1] > '5' || tail[colon + 2] < '0' || tail[colon + 2] > '9')
    return -1;
  line->resize(at);
  return minutes * 60 + (tail[colon + 1] - '0') * 10 + (tail[colon + 2] - '0');
}

// Everything the overlay shows this frame.
struct Scene
{
  OverlayModel::Frame f;
  SetEnd::View set_end;
  // Top-centre lines (search or queue, then the set's). `hint` is the local control guide.
  std::string search, search_note, rules, rules_note, hint;
  // Turn timers drawn by relabelled titles (-1 if not shown).
  int band_seconds = -1;
  int line_seconds = -1;
  bool legend = false;  // Project+'s stage select legend is relabelled
  std::array<std::string, 4> labels;
  std::array<bool, 4> locked{};
  // The game module's display: the stage select cursors and what is drawn from them.
  Orca::Engine::Display display;
  int picker = -1;
  std::string picker_ribbon;  // "YOUR PICK" / "BO PICKS" on the picker's portrait
  Orca::Engine::SetDots dots;
  bool searching = false;

  bool Empty() const
  {
    return !f.show_signal && f.waiting.empty() && f.toasts.empty() && f.chats.empty() &&
           f.stats.empty() && f.perf.empty() && search.empty() && search_note.empty() &&
           rules.empty() && rules_note.empty() && hint.empty() &&
           std::all_of(labels.begin(), labels.end(),
                       [](const std::string& l) { return l.empty(); }) &&
           std::none_of(locked.begin(), locked.end(), [](bool l) { return l; }) &&
           !display.cursors.on &&
           picker < 0 && !dots.on && !searching && band_seconds < 0 && line_seconds < 0 &&
           set_end.Empty();
  }
};

Scene Gather(double now)
{
  Scene s;
  // What the game module shows, read once for this frame.
  s.display = Game().Show();
  const Orca::Engine::Display& game = s.display;
  s_overlay.Update(s_demo ? DemoLink(now) : ViewOf(Events::GetLinkStats()), now);
  s.f = s_overlay.View(now, s_stats.load(std::memory_order_relaxed),
                       s_perf.load(std::memory_order_relaxed));
  // The set-end or disconnect notice replaces "Waiting for <name>...".
  s.set_end = SetEnd::Current().At(SetEnd::DemoFrame(now));
  if (s.set_end.hides_waiting)
    s.f.waiting.clear();
  // The matchmaking search lines, replaced by the queue's character select lines when it has any.
  std::tie(s.search, s.search_note) = Search::Lines();
  // A matched joiner's "Opponent found" lines win over its own select's search lines, which the
  // game's bar (and the queue's lines) still show until the host's game is loaded.
  const bool joining = Search::Joining();
  {
    std::lock_guard lk(s_queue_lock);
    if (!joining && (!s_queue_line.empty() || !s_queue_note.empty()))
    {
      s.search = s_queue_line;
      s.search_note = s_queue_note;
    }
    s.labels = s_queue_labels;
    s.locked = s_queue_locked;
  }
  // With READY TO FIGHT! relabelled, the turn timer moves onto the band.
  const bool band_clock = game.band_clock;
  const auto take = [](int* seconds, std::string* line) {
    const int t = TakeTimer(line);
    if (*seconds < 0)
      *seconds = t;
  };
  if (band_clock)
  {
    take(&s.band_seconds, &s.search);
    take(&s.band_seconds, &s.search_note);
  }
  // Skip what the game's own text boxes already say: a named plate needs no tab, and a rules bar
  // showing the queue line replaces the top lines. Local hints move to the control guide.
  for (int port = 0; port < 4; ++port)
  {
    if (game.plates & (1 << port))
      s.labels[port].clear();
  }
  if (game.rules_bar && !joining)
  {
    // On the solo queue character select the bar says it all (but a matched joiner's bar still
    // says it is searching).
    s.hint = QueueApp::OnSoloCss() ? std::string() : std::move(s.search_note);
    s.search.clear();
    s.search_note.clear();
  }
  // The one-time "New · Hold B to leave" on the queue's own select, in the control guide.
  if (QueueApp::OnSoloCss())
  {
    if (const std::string& tip = game.tip; !tip.empty())
      s.hint = s.hint.empty() ? tip : s.hint + " \u00b7 " + tip;
  }
  // A ranked set's lines: the stage flow's, else the set score; under it the stage flow's turn,
  // else the character order's.
  std::string rule_line;
  {
    std::lock_guard lk(s_rules_lock);
    s.rules = s_rules_line;
    s.rules_note = s_rules_note;
  }
  {
    std::lock_guard lk(s_rule_line_lock);
    rule_line = s_rule_line;
  }
  if (s.rules.empty() && s.rules_note.empty())
  {
    s.rules = game.ranked_set;
    s.rules_note = game.ranked_turn;
  }
  if (s.rules.empty())
    s.rules = game.score_line;
  if (s.rules_note.empty())
    s.rules_note = rule_line;
  if (band_clock)
  {
    take(&s.band_seconds, &s.rules);
    take(&s.band_seconds, &s.rules_note);
  }
  // With STAGE SELECT relabelled to show the turn, the timer moves beside the label, the score to
  // the dots and the turn's buttons to the guide.
  if (game.line_relabelled)
  {
    take(&s.line_seconds, &s.rules);
    take(&s.line_seconds, &s.rules_note);
    const auto join = [](std::string* to, const std::string& more) {
      if (!more.empty())
        *to = to->empty() ? more : *to + " \u00b7 " + more;
    };
    join(&s.hint, s.rules_note);
    // The label says P1 PICKS; the Meta Knight clause explains why any stage is allowed.
    if (s.rules.find("MK clause") != std::string::npos)
      join(&s.hint, "Meta Knight clause: any legal stage");
    s.rules.clear();
    s.rules_note.clear();
  }
  s.legend = game.legend;
  // The rules bar already shows these, and the solo queue character select has no set yet.
  if (game.rules_bar || QueueApp::OnSoloCss())
  {
    s.rules.clear();
    s.rules_note.clear();
  }
  // Picker and dots on the character and stage selects only, not on the solo queue character
  // select, which has no opponent yet.
  if (QueueApp::OnQueueCss())
  {
    s.picker = game.picker;
    if (s.picker >= 0)
      s.picker_ribbon = game.picker_ribbon;
  }
  if ((QueueApp::OnQueueCss() && !QueueApp::OnSoloCss()) || game.cursors.on)
    s.dots = game.dots;
  s.searching = QueueApp::OnSoloCss() && QueueApp::SoloReady();
  return s;
}

// Top left: the connection circle, ping, frame meter and graph. Returns the bottom.
float DrawCorner(ImDrawList* dl, const OverlayModel::Frame& f, ImVec2 at, float unit)
{
  constexpr float SIZE = 2.6f;
  const float line_h = Kit::LabelLook(unit, SIZE).px;
  float y = at.y;
  if (f.show_signal)
  {
    const float r = 3.0f * unit;
    const ImVec2 c(at.x + r, at.y + r);
    Kit::Connection(dl, c, r, unit,
                    f.signal == Signal::Good ? 3 :
                    f.signal == Signal::Ok   ? 2 :
                                               1);
    const int lines = (f.stats.empty() ? 0 : 1) + (f.perf.empty() ? 0 : 1);
    float ty = c.y - static_cast<float>(lines) * line_h / 2;
    const float tx = c.x + r + 0.9f * unit;
    if (!f.stats.empty())
      ty = Kit::Label(dl, ImVec2(tx, ty), unit, f.stats, SIZE).max.y;
    if (!f.perf.empty())
      ty = Kit::Label(dl, ImVec2(tx, ty), unit, f.perf, SIZE, Kit::Align::Left,
                      LevelColour(f.perf_level, 1))
               .max.y;
    y = std::max(c.y + r, ty) + 1.2f * unit;
  }
  else if (!f.perf.empty())
  {
    y = Kit::Label(dl, at, unit, f.perf, SIZE, Kit::Align::Left, LevelColour(f.perf_level, 1))
            .max.y +
        0.8f * unit;
  }
  if (!f.graph.empty())
  {
    const ImVec2 size(40 * unit, 9 * unit);
    FrameGraph(dl, ImVec2(at.x, y), size, unit, f.graph);
    y += size.y + 1.2f * unit;
  }
  return y;
}

float CornerWidth(const OverlayModel::Frame& f, float unit)
{
  constexpr float SIZE = 2.6f;
  const Kit::TextLook look = Kit::LabelLook(unit, SIZE);
  float text = 0;
  if (!f.stats.empty())
    text = std::max(text, Kit::Measure(look, f.stats).x);
  if (!f.perf.empty())
    text = std::max(text, Kit::Measure(look, f.perf).x);
  float w = f.show_signal ? 2 * 3.0f * unit + 0.9f * unit + text : text;
  if (!f.graph.empty())
    w = std::max(w, 40 * unit);
  return w;
}

// Wraps `note` to `width`, breaking at " · " first and at spaces only inside a part too wide.
std::string FitNote(const Kit::TextLook& look, const std::string& note, float width)
{
  static const std::string sep = " \u00b7 ";
  std::vector<std::string> parts;
  for (std::size_t at = 0;;)
  {
    const std::size_t next = note.find(sep, at);
    parts.push_back(note.substr(at, next == std::string::npos ? std::string::npos : next - at));
    if (next == std::string::npos)
      break;
    at = next + sep.size();
  }
  std::vector<std::string> lines;
  for (const std::string& part : parts)
  {
    const std::string longer = lines.empty() ? part : lines.back() + sep + part;
    if (!lines.empty() && Kit::Measure(look, longer).x <= width)
      lines.back() = longer;
    else if (Kit::Measure(look, part).x <= width)
      lines.push_back(part);
    else
      for (std::string& l : Kit::Wrap(look, part, width))
        lines.push_back(std::move(l));
  }
  std::string out;
  for (const std::string& l : lines)
    out += (out.empty() ? "" : "\n") + l;
  return out;
}

// Fits a banner's note into `width`. Returns the wrapped note and the banner's height.
std::pair<std::string, float> FitBanner(const std::string& title, const std::string& note,
                                        float row, float width, float unit)
{
  if (note.empty() || Kit::BannerWidth(row, unit, title, note) <= width)
    return {note, row};
  const std::string fitted =
      FitNote(Kit::InkLook(unit, row * 0.3f / unit), note, width - row * 0.6f - 4.0f * unit);
  const auto lines = static_cast<std::size_t>(std::count(fitted.begin(), fitted.end(), '\n') + 1);
  return {fitted, Kit::BannerHeight(row, lines)};
}

// Top centre: each pair of lines on a banner, with the turn timer beside the first. Sets `guide`
// to the local hint. Returns the drawn bounds so toasts and chat can keep clear.
Kit::Box DrawTopCentre(ImDrawList* dl, const Scene& s, ImVec2 pic, ImVec2 pic_size, ImVec2 vis,
                       ImVec2 vis_size, float unit, double now_s, float first_top,
                       std::string* guide, float right_edge)
{
  *guide = s.hint;
  Kit::Box drawn{ImVec2(vis.x + vis_size.x, vis.y), ImVec2(vis.x, vis.y)};
  const auto add = [&](const Kit::Box& b) {
    drawn.min.x = std::min(drawn.min.x, b.min.x);
    drawn.max.y = std::max(drawn.max.y, b.max.y);
  };
  std::vector<std::pair<std::string, std::string>> groups;
  for (const auto& [title, note] : {std::pair{s.search, s.search_note},
                                    std::pair{s.rules, s.rules_note}})
  {
    if (!title.empty())
      groups.emplace_back(title, note);
    else if (!note.empty())
      groups.emplace_back(note, std::string());
  }
  int seconds = -1;
  for (auto& [title, note] : groups)
  {
    if (seconds < 0)
      seconds = TakeTimer(&title);
    if (seconds < 0)
      seconds = TakeTimer(&note);
  }
  if (Kit::CurrentLook() == Kit::Look::ProjectPlus && QueueApp::OnQueueCss() && !groups.empty())
  {
    // Project+'s queue character select: draw the lines over its own rules bar, which is locked
    // in a queue room anyway. The timer goes under the bar's right end and the local hint in the
    // guide. The bar spans x 520-1265, y 42-105 on a 1280x720 picture.
    std::string title = groups[0].first, note, hint = s.hint;
    if (!s.search.empty())
    {
      note = s.rules;
      const std::string& more = s.search_note.empty() ? s.rules_note : s.search_note;
      hint = hint.empty() ? more : more.empty() ? hint : hint + " \u00b7 " + more;
    }
    else
    {
      note = s.rules_note;
    }
    TakeTimer(&note);
    TakeTimer(&hint);
    // Stops short of Orca's corner when that is moved to the top right.
    const float left = pic.x + pic_size.x * 0.4f;
    const float right = std::min(pic.x + pic_size.x * 0.99f, right_edge);
    const float top = pic.y + pic_size.y * 0.056f;
    const auto [fitted, h] = FitBanner(title, note, pic_size.y * 0.092f, right - left, unit);
    add(Kit::Banner(dl, left, right, top, h, unit, Kit::Tone::Panel, title, fitted));
    const float x = right - 1.5f * unit;
    const float y = top + h + 1.0f * unit;
    if (seconds >= 0)
      add(Kit::Timer(dl, ImVec2(x - 5.5f * unit, y + 2.4f * unit), unit, seconds, now_s));
    *guide = hint;
    return drawn;
  }
  // Orca's own banners are centred on the visible part of the picture.
  const float cx = vis.x + vis_size.x / 2;
  float top = first_top;
  for (std::size_t i = 0; i < groups.size(); ++i)
  {
    const auto& [title, note] = groups[i];
    // The first banner is larger. At most 62% of the picture wide, leaving room for the timer.
    const float row =
        i == 0 ? (note.empty() ? 6.4f : 8.6f) * unit : (note.empty() ? 5.0f : 7.0f) * unit;
    // At least 26 units, or a narrow window would wrap the note one word per line.
    const float max_w = std::max(0.62f * vis_size.x, 26.0f * unit);
    const auto [fitted, h] = FitBanner(title, note, row, max_w, unit);
    const float w = std::clamp(Kit::BannerWidth(h, unit, title, fitted), 26.0f * unit, max_w);
    const Kit::Box b = Kit::Banner(dl, cx - w / 2, cx + w / 2, top, h, unit, Kit::Tone::Panel,
                                   title, fitted);
    add(b);
    if (i == 0 && seconds >= 0)
    {
      add(Kit::Timer(dl, ImVec2(b.max.x + 6.0f * unit, (b.min.y + b.max.y) / 2), unit, seconds,
                     now_s));
    }
    top = b.max.y + 1.0f * unit;
  }
  return drawn;
}

void DrawScene()
{
  Kit::BeginFrame();
  const double now = NowMs();
  const Scene s = Gather(now);
  if (Kit::DemoMode() == 1)
  {
    DrawKitDemo();
    return;
  }
  // Load the chat font before any chat needs it.
  Yg::Warm();
  const YgOrb::Model::View yg = YgOrb::Current().At(now);
  // Dolphin's OSD messages are drawn top left before this. While the YouGame button shows they
  // move below it, from the next frame.
  if (!yg.on)
    OSD::SetObscuredPixelsTop(0);
  if (s.Empty() && !yg.on && !yg.showing)
    return;
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  const float unit = OverlayUnit(pic_size.y);
  // Nothing to draw on yet, or the window is too thin.
  if (unit <= 0 || !(pic_size.x >= 1))
    return;
  const PictureArea visible = VisiblePicture({pic.x, pic.y, pic_size.x, pic_size.y},
                                      ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
  const ImVec2 vis(visible.x, visible.y), vis_size(visible.w, visible.h);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const double now_s = ImGui::GetTime();
  const OverlayModel::Frame& f = s.f;
  // Stage select cursors first, so all text draws over them.
  if (s.display.cursors.on)
    DrawStageCursors(dl, s.display, pic, pic_size, unit, now_s);
  // Character select panels: a name tab where the game's plate shows no name, a gold LOCKED IN
  // frame for locked-in players, and a blinking frame in port colour on the current picker with
  // YOUR PICK / BO PICKS. The empty seat gets a ring while the queue searches.
  static int s_picker_was = -1;
  static double s_picker_since = 0;
  if (s.picker != s_picker_was)
  {
    s_picker_was = s.picker;
    s_picker_since = now_s;
  }
  for (int port = 0; port < 4; ++port)
  {
    const float centre = pic.x + pic_size.x * PanelCentre(port);
    if (!s.labels[port].empty())
    {
      Kit::LabelPlate(dl, ImVec2(centre, pic.y + pic_size.y * NAME_TAB_Y), unit, s.labels[port],
                      2.8f, Kit::Align::Centre, Kit::PortTone(port));
    }
    const Kit::Box portrait = PortraitRect(port, pic, pic_size);
    if (s.locked[port])
    {
      Kit::PortraitFrame(dl, portrait.min, portrait.max, unit, Kit::Tone::Gold);
      Kit::Ribbon(dl, ImVec2(portrait.min.x + 1.9f * unit, portrait.min.y + 1.4f * unit),
                  ImVec2(portrait.max.x - 1.9f * unit, portrait.min.y + 5.8f * unit), unit,
                  "LOCKED IN", Kit::Tone::Gold);
    }
    else if (port == s.picker)
    {
      const Kit::Blink blink = Kit::Attention(now_s, s_picker_since);
      const float g = 0.9f * unit;
      dl->AddRect(ImVec2(portrait.min.x - g, portrait.min.y - g),
                  ImVec2(portrait.max.x + g, portrait.max.y + g),
                  IM_COL32(0, 0, 0, static_cast<int>(115 * blink.on)), 1.2f * unit, 0,
                  0.9f * unit);
      dl->AddRect(ImVec2(portrait.min.x - g, portrait.min.y - g),
                  ImVec2(portrait.max.x + g, portrait.max.y + g),
                  IM_COL32(255, 255, 255, static_cast<int>(255 * blink.on)), 1.2f * unit, 0,
                  0.55f * unit);
      Kit::PortraitFrame(dl, portrait.min, portrait.max, unit, Kit::PortTone(port));
      if (!s.picker_ribbon.empty())
      {
        Kit::Ribbon(dl, ImVec2(portrait.min.x + 1.9f * unit, portrait.min.y + 1.4f * unit),
                    ImVec2(portrait.max.x - 1.9f * unit, portrait.min.y + 5.8f * unit), unit,
                    s.picker_ribbon, Kit::PortTone(port));
      }
    }
  }
  if (s.searching)
  {
    const Kit::Box seat = PortraitRect(1, pic, pic_size);
    Kit::SearchRing(dl, ImVec2((seat.min.x + seat.max.x) / 2, (seat.min.y + seat.max.y) / 2),
                    pic_size.y * 0.072f, unit, now_s);
  }
  if (s.dots.on)
  {
    Kit::SetDots(dl, ImVec2(pic.x + pic_size.x / 2, pic.y + DOTS_Y * unit), unit, s.dots.games,
                 s.dots.current);
  }
  // The YouGame button holds the top left with its pill along the top edge. Orca's corner then
  // moves to the top right, and Project+'s queue bar ends short of it.
  const float margin = 2.5f * unit;
  const Kit::Box orb = YgOrb::ButtonBox(yg);
  const bool yg_top = orb.max.x > orb.min.x;
  // Grow at once but shrink only after 4 s, so changing digits don't cause jitter.
  static float s_corner_w = 0;
  static double s_corner_at = 0;
  const float corner_now = CornerWidth(f, unit);
  if (corner_now >= s_corner_w || now - s_corner_at > 4000)
    s_corner_w = corner_now;
  if (corner_now >= s_corner_w)
    s_corner_at = now;
  const bool corner_right = yg_top && s_corner_w > 0;
  const float corner_x =
      corner_right ? vis.x + vis_size.x - margin - s_corner_w : vis.x + margin;
  std::string guide;
  const Kit::Box centre =
      DrawTopCentre(dl, s, pic, pic_size, vis, vis_size, unit, now_s,
                    std::max(pic.y + (s.dots.on ? TOP_UNDER_DOTS : TOP_FIRST) * unit,
                             vis.y + TOP_FIRST * unit),
                    &guide, corner_right ? corner_x - 1.0f * unit : pic.x + pic_size.x);
  const bool pplus = Kit::CurrentLook() == Kit::Look::ProjectPlus;
  if (s.band_seconds >= 0)
  {
    const GameTimerPlace& at = pplus ? BAND_TIMER_PPLUS : BAND_TIMER_BRAWL;
    Kit::GameTimer(dl, ImVec2(pic.x + pic_size.x * at.x, pic.y + pic_size.y * at.y), unit,
                   s.band_seconds, now_s, Kit::Lettering::Band, at.size);
  }
  if (s.line_seconds >= 0)
  {
    const GameTimerPlace& at = pplus ? LINE_TIMER_PPLUS : LINE_TIMER_BRAWL;
    const float half = Kit::Measure(Kit::TitleLook(unit, at.size), "0:00").x / 2;
    Kit::GameTimer(dl, ImVec2(pic.x + pic_size.x * at.x + half, pic.y + pic_size.y * at.y), unit,
                   s.line_seconds, now_s, Kit::Lettering::Line, at.size);
  }
  if (!guide.empty())
  {
    // The control guide: one line along the bottom edge, shrunk to fit. Skips X, A and B when
    // Project+'s relabelled legend already shows them. Parsed only when the hint changes.
    static std::string s_hint;
    static std::vector<Kit::GuideItem> s_items;
    if (guide != s_hint)
    {
      s_hint = guide;
      s_items = Kit::GuideOf(guide);
    }
    std::vector<Kit::GuideItem> items;
    for (const Kit::GuideItem& item : s_items)
    {
      if (!(s.legend && (item.button == "X" || item.button == "A" || item.button == "B")))
        items.push_back(item);
    }
    const float right = std::min(pic.x + pic_size.x * GUIDE_RIGHT,
                                 vis.x + vis_size.x - pic_size.x * (1 - GUIDE_RIGHT));
    const float cy =
        std::min(pic.y + pic_size.y * GUIDE_Y, vis.y + vis_size.y - pic_size.y * (1 - GUIDE_Y));
    Kit::Guide(dl, right, cy, unit, items, 1,
               std::max(right - vis.x - pic_size.x * 0.025f, 1.0f));
  }
  const float left = vis.x + margin;
  float y = DrawCorner(dl, f, ImVec2(corner_x, vis.y + margin), unit);
  // Without the button, Dolphin's messages start below the corner.
  if (!yg_top && corner_now > 0)
    OSD::SetObscuredPixelsTop(static_cast<int>(y));
  if (!f.waiting.empty())
  {
    const float top = std::max(vis.y + vis_size.y * 0.18f, centre.max.y + 1.5f * unit);
    Kit::Title(dl, ImVec2(vis.x + vis_size.x / 2, top), unit, f.waiting, 5.0f, Kit::Align::Centre);
  }
  // Toasts and chat go below the corner, Dolphin's messages and the top-centre banners.
  if (yg_top)
  {
    // With the button, toasts and chat go in its pill. Rows are views of this frame's lines.
    std::array<YgOrb::PillRow, 12> rows;
    std::size_t n = 0;
    for (const OverlayModel::Toast& t : f.toasts)
    {
      if (n < rows.size())
        rows[n++] = YgOrb::PillRow{YgOrb::PillRow::Type::Toast, {}, t.text, {}, {},
                                   YgOrb::Kind::Join, t.alpha, t.at};
    }
    for (const OverlayModel::ChatToast& c : f.chats)
    {
      if (n < rows.size())
        rows[n++] = YgOrb::PillRow{YgOrb::PillRow::Type::Chat, c.name, c.text, {}, {},
                                   YgOrb::Kind::Join, c.alpha, c.at};
    }
    const float pill_right = centre.max.y > vis.y && centre.min.x > orb.min.x ?
                                 centre.min.x - 1.2f * unit :
                                 vis.x + vis_size.x;
    const Kit::Box pill =
        YgOrb::DrawPill(dl, yg, std::span<const YgOrb::PillRow>(rows.data(), n), now, pill_right);
    const float under = std::max(orb.max.y, pill.max.y > pill.min.y ? pill.max.y : 0.0f);
    OSD::SetObscuredPixelsTop(static_cast<int>(under + 0.8f * unit));
  }
  else if (!f.toasts.empty() || !f.chats.empty())
  {
    y = std::max(y, OSD::MessagesBottom() + unit);
    const float wrap =
        std::max(std::min(0.36f * pic_size.x, vis.x + vis_size.x - margin - left), 12.0f * unit);
    if (centre.max.y > vis.y && centre.min.x < left + wrap + 3.0f * unit)
      y = std::max(y, centre.max.y + 1.2f * unit);
    const float bottom = vis.y + vis_size.y - margin;
    for (const OverlayModel::Toast& t : f.toasts)
    {
      if (y >= bottom)
        break;
      y = Yg::NoticeCard(dl, ImVec2(left, y), unit, t.text, wrap, t.alpha).max.y +
          0.8f * unit;
    }
    for (const OverlayModel::ChatToast& c : f.chats)
    {
      if (y >= bottom)
        break;
      y = Yg::ChatCard(dl, ImVec2(left, y), unit, c.name, c.text, wrap, c.alpha).max.y +
          0.8f * unit;
    }
  }
  if (!s.set_end.Empty())
    SetEnd::Draw(dl, unit, vis.x, vis.y, vis_size.x, vis_size.y, s.set_end);
  // The YouGame button goes on top, where the app's own button is.
  YgOrb::DrawButton(dl, yg);
}

// The dim (SetDim) over the whole display, last on the foreground list so it covers everything.
// Eases toward the target by half black per DIM_EASE_S.
void DrawDim()
{
  const float target = static_cast<float>(s_dim.load(std::memory_order_relaxed)) / 100.0f;
  const double now = ImGui::GetTime();
  const double since = s_dim_at < 0 || now < s_dim_at ? DIM_EASE_S : now - s_dim_at;
  s_dim_at = now;
  const float step = static_cast<float>(since / DIM_EASE_S * 0.5);
  s_dim_shown = s_dim_shown < target ? std::min(target, s_dim_shown + step) :
                                       std::max(target, s_dim_shown - step);
  if (!(s_dim_shown > 0))
    return;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  if (!(display.x > 0 && display.y > 0))
    return;
  ImGui::GetForegroundDrawList()->AddRectFilled(
      ImVec2(0, 0), display,
      IM_COL32(0, 0, 0, static_cast<int>(std::lround(255.0f * std::min(s_dim_shown, 1.0f)))));
}

// The host overlay: the scene, then the dim, even when the scene returned early.
void Draw()
{
  DrawScene();
  DrawDim();
}
}  // namespace

OverlayModel& Overlay()
{
  return s_overlay;
}

PortColour PortColourOf(int port)
{
  switch (port)
  {
  case 0:
    return {224, 44, 44};
  case 1:
    return {40, 104, 236};
  case 2:
    return {232, 180, 24};
  default:
    return {44, 168, 64};
  }
}

PictureArea VisiblePicture(const PictureArea& picture, float display_w, float display_h)
{
  const float x0 = std::max(picture.x, 0.0f), y0 = std::max(picture.y, 0.0f);
  const float x1 = std::min(picture.x + picture.w, display_w);
  const float y1 = std::min(picture.y + picture.h, display_h);
  // !(>) also rejects NaN.
  if (!(x1 > x0 && y1 > y0))
    return picture;
  return {x0, y0, x1 - x0, y1 - y0};
}

void SetPictureForTests(std::optional<PictureArea> picture)
{
  s_test_picture = picture;
}

void SetDim(int percent)
{
  s_dim.store(std::clamp(percent, 0, 100), std::memory_order_relaxed);
}

int Dim()
{
  return s_dim.load(std::memory_order_relaxed);
}

float OverlayUnit(float display_height)
{
  // !(>=) also rejects NaN.
  if (!(display_height >= OVERLAY_MIN_HEIGHT))
    return 0;
  return display_height / 100;
}

void SetRulesLines(std::string line, std::string note)
{
  std::lock_guard lk(s_rules_lock);
  s_rules_line = std::move(line);
  s_rules_note = std::move(note);
}

void SetStats(Stats stats)
{
  s_stats = stats;
}

void SetPerf(Perf perf)
{
  s_perf = perf;
}

void FrameBoundary(bool shown, double game_held_ms, int game_held_frames)
{
  s_overlay.OnFrame(NowMs(), shown, game_held_ms, game_held_frames);
}

void ShowToast(std::string text)
{
  s_overlay.AddToast(std::move(text), NowMs());
}

void ShowRestartForMatch()
{
  ShowToast("Restarting the game for this match");
}

void ShowRestartForJoin(std::string_view name)
{
  ShowToast(fmt::format("Restarting the game in 3 seconds so {} can join. "
                       "Unsaved progress will be lost.", name.empty() ? "your friend" : name));
}

void SetQueueLines(std::string line, std::string note, std::array<std::string, 4> labels,
                   std::array<bool, 4> locked)
{
  std::lock_guard lk(s_queue_lock);
  s_queue_line = std::move(line);
  s_queue_note = std::move(note);
  s_queue_labels = std::move(labels);
  s_queue_locked = locked;
}

void ShowChat(std::string name, std::string text)
{
  s_overlay.AddChat(std::move(name), std::move(text), NowMs());
}

void SetRuleLine(std::string text)
{
  std::lock_guard lk(s_rule_line_lock);
  s_rule_line = std::move(text);
}

void ShutdownOverlay()
{
  OSD::SetHostOverlay({});
  Kit::Shutdown();
  s_state_hook.reset();
  s_test_picture.reset();
}

void SetHostVisible(bool visible)
{
  s_visible = visible;
}

bool HostVisible()
{
  return s_visible;
}

void RepresentDuringStall(int stalled_ms)
{
  // Sessions run single core, so this presents on the CPU thread inside the stall wait, which
  // hears nothing (input, a stop) while it presents. So skip short stalls and hidden windows,
  // present ~15 times a second (4 if slow), and stop for this stall once a present takes over
  // 50 ms (vsync can hold a Metal drawable that long).
  static double s_last = 0;
  static double s_every = 66;
  static int s_stall_ms = 0;
  static bool s_off_this_stall = false;
  if (stalled_ms < s_stall_ms)
    s_off_this_stall = false;  // a new stall
  s_stall_ms = stalled_ms;
  if (s_off_this_stall || !s_visible.load(std::memory_order_relaxed) ||
      !VideoCommon::Presenter::IsSurfaceVisible() ||
      stalled_ms < OverlayModel::WAITING_AFTER_MS - 50)
  {
    return;
  }
  const double now = NowMs();
  if (now - s_last < s_every)
    return;
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->RepresentLast();
  });
  const double took = NowMs() - now;
  s_every = took > 12 ? 250 : 66;
  if (took > 50)
    s_off_this_stall = true;
  s_last = NowMs();
}

void InitOverlay()
{
  s_dim = 0;
  s_dim_shown = 0;
  s_dim_at = -1;
  if (const char* v = std::getenv("ORCA_UX_STATS"); v && (v[0] == '1' || v[0] == '2'))
    s_stats = v[0] == '2' ? Stats::Full : Stats::Ping;
  if (const char* v = std::getenv("ORCA_UX_OVERLAY_DEMO"); v && v[0] == '1')
    s_demo = true;
  // A pause must not count as a slow frame. Runs on whichever thread pauses. A paused or stopping
  // game reads no pad, so a held controller shortcut ends there (hold-to-talk, UX/OrbCombo.h).
  s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
    if (state == Core::State::Paused)
      s_overlay.Pause(NowMs());
    else if (state == Core::State::Running)
      s_overlay.Resume(NowMs());
    if (state == Core::State::Paused || state == Core::State::Stopping)
      LocalPadStopped();
  });
  OSD::SetHostOverlay(&Draw, &Kit::ReleaseGpu);
}
}  // namespace Orca::UX
