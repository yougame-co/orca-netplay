// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Events.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

namespace Orca::Events
{
namespace
{
std::mutex s_lock;
PlugInCallback s_plug_in;
LinkStats s_stats;
LocalPadSource s_pad_source;
InputAgeSource s_input_age;
FrameCallback s_frame;
StallCallback s_stall;
BoundaryCallback s_boundary;
std::atomic<int> s_ux_compat{0};
std::atomic<u64> s_resyncs{0};
std::vector<u8> s_own_controls;
std::vector<u8> s_own_queue;
}  // namespace

void SetOwnControls(std::vector<u8> controls)
{
  std::lock_guard lk(s_lock);
  s_own_controls = std::move(controls);
}

std::vector<u8> OwnControls()
{
  std::lock_guard lk(s_lock);
  return s_own_controls;
}

void SetOwnQueue(std::vector<u8> queue)
{
  std::lock_guard lk(s_lock);
  s_own_queue = std::move(queue);
}

std::vector<u8> OwnQueue()
{
  std::lock_guard lk(s_lock);
  return s_own_queue;
}

void SetPlugInCallback(PlugInCallback callback)
{
  std::lock_guard lk(s_lock);
  s_plug_in = std::move(callback);
}

LinkStats GetLinkStats()
{
  std::lock_guard lk(s_lock);
  return s_stats;
}

void PublishLinkStats(const LinkStats& stats)
{
  std::lock_guard lk(s_lock);
  s_stats = stats;
}

void SetLocalPadSource(LocalPadSource source)
{
  std::lock_guard lk(s_lock);
  s_pad_source = std::move(source);
}

std::optional<GCPadStatus> LocalPad(bool for_frame)
{
  LocalPadSource source;
  {
    std::lock_guard lk(s_lock);
    source = s_pad_source;
  }
  return source ? source(for_frame) : std::nullopt;
}

void SetInputAgeSource(InputAgeSource source)
{
  std::lock_guard lk(s_lock);
  s_input_age = std::move(source);
}

InputAge GetInputAge()
{
  InputAgeSource source;
  {
    std::lock_guard lk(s_lock);
    source = s_input_age;
  }
  return source ? source() : InputAge{};
}

void NotifyPlugIn(int frame, const std::vector<PortInfo>& ports)
{
  PlugInCallback callback;
  {
    std::lock_guard lk(s_lock);
    callback = s_plug_in;
  }
  if (callback)
    callback(frame, ports);
}
}  // namespace Orca::Events

namespace Orca::Events
{
void SetFrameCallback(FrameCallback callback)
{
  std::lock_guard lk(s_lock);
  s_frame = std::move(callback);
}

void SetStallCallback(StallCallback callback)
{
  std::lock_guard lk(s_lock);
  s_stall = std::move(callback);
}

void SetBoundaryCallback(BoundaryCallback callback)
{
  std::lock_guard lk(s_lock);
  s_boundary = std::move(callback);
}

void SetUXCompatVersion(int version)
{
  s_ux_compat.store(version, std::memory_order_relaxed);
}

int UXCompatVersion()
{
  return s_ux_compat.load(std::memory_order_relaxed);
}

void NotifyFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                 const std::vector<PortInfo>& ports, bool alone)
{
  FrameCallback callback;
  {
    std::lock_guard lk(s_lock);
    callback = s_frame;
  }
  if (callback)
    callback(guard, frame, resimulating, ports, alone);
}

void NoteResync()
{
  s_resyncs.fetch_add(1, std::memory_order_relaxed);
}

u64 Resyncs()
{
  return s_resyncs.load(std::memory_order_relaxed);
}

void NotifyStall(int stalled_ms)
{
  StallCallback callback;
  {
    std::lock_guard lk(s_lock);
    callback = s_stall;
  }
  if (callback)
    callback(stalled_ms);
}

void NotifyBoundary(bool shown, double game_held_ms, int game_held_frames)
{
  BoundaryCallback callback;
  {
    std::lock_guard lk(s_lock);
    callback = s_boundary;
  }
  if (callback)
    callback(shown, game_held_ms, game_held_frames);
}

double GameHeldMs(double emulated_ms, double wall_ms, double frame_ms)
{
  if (!(frame_ms > 0) || !(emulated_ms >= 1.5 * frame_ms) || !(wall_ms > frame_ms))
    return 0;
  return std::min(emulated_ms, wall_ms) - frame_ms;
}

int GameHeldFrames(double game_held_ms, double frame_ms, double& carry)
{
  if (!(frame_ms > 0) || !(game_held_ms > 0))
    return 0;
  const double frames = game_held_ms / frame_ms + carry;
  const int whole = std::max(0, static_cast<int>(std::lround(frames)));
  carry = frames - whole;
  return whole;
}
}  // namespace Orca::Events
