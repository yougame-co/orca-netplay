// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Activity.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/Controllers.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Rollback/Harness.h"
#include "InputCommon/ControlReference/ControlReference.h"

namespace Orca::Activity
{
namespace
{
bool StickOut(bool was, u8 x, u8 y)
{
  const int dx = int{x} - GCPadStatus::MAIN_STICK_CENTER_X;
  const int dy = int{y} - GCPadStatus::MAIN_STICK_CENTER_Y;
  const int r2 = dx * dx + dy * dy;
  if (r2 >= STICK_OUT * STICK_OUT)
    return true;
  if (r2 < STICK_IN * STICK_IN)
    return false;
  return was;
}

bool TriggerDown(bool was, u8 analog, bool click)
{
  if (click || analog >= TRIGGER_PRESS)
    return true;
  if (analog <= TRIGGER_RELEASE)
    return false;
  return was;
}

s64 Round(s64 ms)
{
  return (ms + 500) / 1000;
}
}  // namespace

int Edges::Feed(const GCPadStatus& pad)
{
  const u16 buttons = pad.button & BUTTONS;
  const std::array<bool, 2> trigger{
      TriggerDown(m_trigger_down[0], pad.triggerLeft, (pad.button & PAD_TRIGGER_L) != 0),
      TriggerDown(m_trigger_down[1], pad.triggerRight, (pad.button & PAD_TRIGGER_R) != 0)};
  const std::array<bool, 2> stick{StickOut(m_stick_out[0], pad.stickX, pad.stickY),
                                  StickOut(m_stick_out[1], pad.substickX, pad.substickY)};
  int edges = 0;
  if (m_primed)
  {
    edges += std::popcount(static_cast<u16>(buttons ^ m_buttons));
    for (std::size_t i = 0; i < 2; ++i)
      edges += (trigger[i] != m_trigger_down[i]) + (stick[i] != m_stick_out[i]);
  }
  m_primed = true;
  m_buttons = buttons;
  m_trigger_down = trigger;
  m_stick_out = stick;
  return edges;
}

std::string_view ModeName(Mode mode)
{
  switch (mode)
  {
  case Mode::Training:
    return "training";
  case Mode::Friends:
    return "friends";
  case Mode::Casual:
    return "casual";
  case Mode::Ranked:
    return "ranked";
  case Mode::Solo:
    break;
  }
  return "solo";
}

Mode DecideMode(std::string_view room_queue, bool friend_plugged, bool training)
{
  // A matchmade room is that queue's from its welcome to its end (the set over screen too).
  if (room_queue == "ranked")
    return Mode::Ranked;
  if (room_queue == "casual")
    return Mode::Casual;
  if (friend_plugged)
    return Mode::Friends;
  return training ? Mode::Training : Mode::Solo;
}

std::string FormatLine(const Line& line)
{
  return fmt::format("orca active {} {} {} {} {}", Round(line.interval_ms), Round(line.active_ms),
                     line.inputs, ModeName(line.mode), line.pads);
}

bool IsEmpty(const Line& line)
{
  // active_ms is never more than interval_ms, so it rounds to 0 too.
  return Round(line.interval_ms) == 0 && line.inputs == 0;
}

Clock::Clock(s64 start_ms, Mode mode)
    : m_start(start_ms), m_last(start_ms), m_active_until(start_ms), m_mode(mode)
{
}

void Clock::Advance(s64 now_ms)
{
  if (now_ms <= m_last)
    return;
  const std::size_t mode = static_cast<std::size_t>(m_mode);
  m_wall_ms[mode] += now_ms - m_last;
  m_active_ms[mode] += std::max<s64>(0, std::min(now_ms, m_active_until) - m_last);
  m_last = now_ms;
}

void Clock::SetMode(s64 now_ms, Mode mode)
{
  if (mode == m_mode)
    return;
  Advance(now_ms);
  m_mode = mode;
}

void Clock::Input(s64 now_ms, int edges, u32 pad)
{
  if (edges <= 0)
    return;
  Advance(now_ms);
  m_active_until = std::max(m_active_until, m_last + ACTIVE_AFTER_INPUT_MS);
  m_inputs += static_cast<u32>(edges);
  if (std::find(m_pads.begin(), m_pads.end(), pad) == m_pads.end())
    m_pads.push_back(pad);
}

Line Clock::Take(s64 now_ms)
{
  Advance(now_ms);
  Line line;
  line.interval_ms = m_last - m_start;
  line.inputs = m_inputs;
  line.pads = static_cast<u32>(m_pads.size());
  // The mode with the most active time, else the most time, else the current one; on a tie the
  // later mode in Mode's order (the more specific).
  s64 active = 0;
  for (s64 ms : m_active_ms)
    active += ms;
  line.active_ms = active;
  const std::array<s64, MODES>& by = active > 0 ? m_active_ms : m_wall_ms;
  line.mode = m_mode;
  s64 best = 0;
  for (std::size_t i = 0; i < MODES; ++i)
  {
    if (by[i] > 0 && by[i] >= best)
    {
      best = by[i];
      line.mode = static_cast<Mode>(i);
    }
  }
  m_start = m_last;
  m_wall_ms = {};
  m_active_ms = {};
  m_inputs = 0;
  m_pads.clear();
  return line;
}

void SnapshotPads(const UX::Snapshot& snapshot, double age_ms, std::vector<Pad>* out)
{
  // Same freshness rules as the local pad (UX::ChooseLocalPad).
  if (!(age_ms <= UX::STALE_MS) || snapshot.suspended)
    return;
  if (snapshot.owned)
  {
    const double report_age = snapshot.sent_at - snapshot.received_at + age_ms;
    if (snapshot.received_at > 0 && report_age <= UX::STALE_MS)
    {
      for (const UX::AdapterPort& port : snapshot.ports)
      {
        if (port.connected)
          out->push_back({PAD_ADAPTER | (static_cast<u32>(port.port) & 0xFF), UX::MapAdapterPort(port)});
      }
    }
  }
  // The app's keyboard pad is one of these. The app renumbers it as keys go down and up
  // (desktop virtual-pad.ts), so at those moments an index can compare one controller with
  // another: extra edges, only ever at a real key's press or release.
  for (const UX::StandardPad& pad : snapshot.pads)
    out->push_back({PAD_GAMEPAD | (static_cast<u32>(pad.index) & 0xFF), UX::MapStandardPad(pad)});
}

void Reader::Note(u32 id, const GCPadStatus& pad)
{
  for (Pad& noted : m_noted)
  {
    if (noted.id == id)
    {
      noted.status = pad;
      return;
    }
  }
  m_noted.push_back({id, pad});
}

void Reader::Feed(const Pad& pad, std::vector<std::pair<u32, int>>* edges)
{
  if (!pad.status.isConnected)
    return;
  auto it = std::find_if(m_edges.begin(), m_edges.end(),
                         [&pad](const auto& entry) { return entry.first == pad.id; });
  if (it == m_edges.end())
  {
    m_edges.emplace_back(pad.id, Edges{});
    it = m_edges.end() - 1;
  }
  if (const int count = it->second.Feed(pad.status); count > 0)
    edges->emplace_back(pad.id, count);
}

void Reader::Boundary(bool first_run, bool gate, const std::vector<Pad>& direct,
                      std::vector<std::pair<u32, int>>* edges)
{
  if (first_run)
  {
    for (const Pad& pad : m_noted)
    {
      if (gate || pad.id != PAD_DOLPHIN)
        Feed(pad, edges);
    }
    for (const Pad& pad : direct)
      Feed(pad, edges);
  }
  m_noted.clear();
}

// ---- The running game ----

namespace
{
std::mutex s_lock;
std::condition_variable s_wake;
std::optional<Clock> s_clock;
bool s_finished = false;
// HoldUntilReady: the clock waits for the app's "ready" (s_ready).
bool s_hold = false;
bool s_ready = false;
std::thread s_printer;

// CPU thread only.
Reader s_reader;
std::vector<Pad> s_direct;
std::vector<std::pair<u32, int>> s_edges;

s64 NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// One line every INTERVAL_MS, on its own thread so stalls, loads and pauses don't delay it.
void PrintLines()
{
  std::unique_lock lk(s_lock);
  while (!s_finished)
  {
    const s64 now = NowMs();
    const s64 due = s_clock->DueAt();
    if (now < due)
    {
      s_wake.wait_for(lk, std::chrono::milliseconds(due - now));
      continue;
    }
    const Line line = s_clock->Take(now);
    lk.unlock();
    Status::Line(FormatLine(line));
    lk.lock();
  }
}

// Stops the printer without a last line, at exit if Finish never ran.
void StopPrinter()
{
  {
    std::lock_guard lk(s_lock);
    s_finished = true;
  }
  s_wake.notify_all();
  if (s_printer.joinable())
    s_printer.join();
}

struct Teardown
{
  ~Teardown() { StopPrinter(); }
} s_teardown;

Mode CurrentMode()
{
  const bool training = UX::InTraining();
  if (!Online::Enabled())
    return training ? Mode::Training : Mode::Solo;
  // A room that ended (a network error, the backoff before it reopens) is no queue any more, as
  // for the rules (OnlineMatch's WantedRulesMode). A set that is over hasn't ended: still ranked.
  return DecideMode(Online::RoomEnded() ? std::string() : Online::RoomQueue(),
                    Events::GetLinkStats().online, training);
}
}  // namespace

void HoldUntilReady()
{
  std::lock_guard lk(s_lock);
  s_hold = true;
}

void Ready()
{
  std::lock_guard lk(s_lock);
  s_ready = true;
}

void NoteLocalPad(u32 id, const GCPadStatus& pad)
{
  s_reader.Note(id, pad);
}

void OnBoundary(bool first_run)
{
  s_direct.clear();
  s_edges.clear();
  // What the game gets: nothing from the player's controllers while the window doesn't have the
  // input (UX::LocalPad), so the app's stream isn't read then either.
  const bool gate = first_run && ControlReference::GetInputGate();
  if (gate)
  {
    double age_ms = 0;
    if (const std::shared_ptr<const UX::Snapshot> snapshot = UX::LatestSnapshot(&age_ms))
      SnapshotPads(*snapshot, age_ms, &s_direct);
  }
  // Outside an online match the test harness's script plays every port, all of them local; an
  // online match notes its local seat's script itself (NoteLocalPad).
  if (first_run && Rollback::Harness::Active() && !Online::Enabled())
  {
    for (int port = 0; port < 4; ++port)
    {
      if (const std::optional<GCPadStatus> pad = Rollback::Harness::InputOverride(port))
        s_direct.push_back({PAD_SCRIPT | static_cast<u32>(port), *pad});
    }
  }
  s_reader.Boundary(first_run, gate, s_direct, &s_edges);
  if (!first_run)
    return;
  const Mode mode = CurrentMode();
  std::lock_guard lk(s_lock);
  if (s_finished)
    return;
  const s64 now = NowMs();
  if (!s_clock)
  {
    // Edges until then only set each controller's baseline.
    if (s_hold && !s_ready)
      return;
    s_clock.emplace(now, mode);
    s_printer = std::thread(PrintLines);
  }
  s_clock->SetMode(now, mode);
  for (const auto& [pad, count] : s_edges)
    s_clock->Input(now, count, pad);
}

void Finish()
{
  std::optional<Line> last;
  {
    std::lock_guard lk(s_lock);
    if (s_finished)
      return;
    s_finished = true;
    if (s_clock)
      last = s_clock->Take(NowMs());
  }
  s_wake.notify_all();
  // A line the printer is writing goes first.
  if (s_printer.joinable())
    s_printer.join();
  if (last && !IsEmpty(*last))
    Status::Line(FormatLine(*last));
}
}  // namespace Orca::Activity
