// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/SessionPort.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>

#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/Diag.h"
#include "Core/System.h"

namespace Rollback
{
namespace
{
RingPort* s_active = nullptr;

// How long a time-sync wait holds this machine: one NTSC (60 Hz) frame.
constexpr std::chrono::microseconds FRAME_PERIOD{16'667};
}  // namespace

RingPort::RingPort(Core::System& system, int max_rollback, bool unthrottled)
    : m_system(system), m_ring(static_cast<std::size_t>(max_rollback) + 2),
      m_unthrottled(unthrottled)
{
  // Until the session sets them, every port reads as connected with nothing pressed.
  m_pads.fill(Orca::Net::DecodePad(Orca::Net::Pad{}));
  s_active = this;
  if (m_unthrottled)
    Core::SetIsThrottlerTempDisabled(true);
}

RingPort::~RingPort()
{
  if (s_active == this)
    s_active = nullptr;
  m_ring.Reset(m_system);
  Rollback::SetResimulating(false);
  if (m_unthrottled)
    Core::SetIsThrottlerTempDisabled(false);
}

RingPort* RingPort::Active()
{
  return s_active;
}

bool RingPort::Save(int frame)
{
  // A stalled boundary asks for the same frame again; nothing has run since.
  if (m_saved_since_run == frame)
    return true;
  const auto start = std::chrono::steady_clock::now();
  const bool allocated = m_ring.Save(m_system, frame);
  double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (Diag::SnapTime())
    Diag::AddPortSave(ms);
  // A slot's first use allocates ~100 MB, so it is not a representative sample. Otherwise blend in
  // 1/8 of each sample, capped at 4x the estimate so one slow save barely moves it.
  if (!allocated)
  {
    if (m_save_ms > 0)
      ms = std::min(ms, 4 * m_save_ms);
    m_save_ms = m_save_ms == 0 ? ms : m_save_ms + (ms - m_save_ms) / 8;
  }
  m_saved_since_run = frame;
  return true;
}

bool RingPort::Load(int frame)
{
  // Loading re-anchors the throttle. Keep the current reference so the time the rollback takes is
  // made up afterwards.
  if (!m_resimulating && !m_reference_before_load)
    m_reference_before_load = m_system.GetCoreTiming().GetThrottleReference();
  m_saved_since_run = -1;
  const auto start = std::chrono::steady_clock::now();
  const bool loaded = m_ring.Load(m_system, frame);
  if (Diag::SnapTime())
  {
    Diag::AddPortLoad(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  return loaded;
}

void RingPort::SetPads(int frame, const Orca::Net::Pads& pads)
{
  Orca::Net::ReplayRecordingScope::RecordPads(frame, pads);
  // Called once for every frame about to run.
  for (std::size_t port = 0; port < pads.size(); ++port)
    m_pads[port] = Orca::Net::DecodePad(pads[port]);
  m_saved_since_run = -1;
}

void RingPort::SetCatchingUp(bool catching_up)
{
  if (catching_up == m_catching_up)
    return;
  m_catching_up = catching_up;
  Rollback::SetResimulating(m_catching_up || m_resimulating);
  if (!catching_up)
  {
    m_reference_before_load.reset();
    ResetPacing();
  }
}

bool RingPort::LoadImage(MachineImage image, int frame)
{
  m_saved_since_run = -1;
  return m_ring.LoadImage(m_system, std::move(image), frame);
}

void RingPort::SetResimulating(bool resimulating)
{
  // While re-running, emulation is unthrottled, rendering is skipped and audio is dropped.
  m_resimulating = resimulating;
  Rollback::SetResimulating(resimulating || m_catching_up);
  if (!resimulating && m_reference_before_load)
  {
    m_system.GetCoreTiming().SetThrottleReference(*m_reference_before_load);
    m_reference_before_load.reset();
  }
}

std::optional<u64> RingPort::SnapshotChecksum(int frame)
{
  return m_ring.RamChecksum(frame);
}

void RingPort::ResetPacing()
{
  m_reference_before_load.reset();
  m_system.GetCoreTiming().ResetThrottleToNow();
}

Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad()>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after, bool* gave_up,
                            std::chrono::nanoseconds* waited_time)
{
  Orca::Net::Pad local_pad = sample_local();
  bool waited = false;
  // From the end of the first call that said wait or stall to the start of the last call.
  std::optional<std::chrono::steady_clock::time_point> wait_from;
  const auto end_wait = [&](std::chrono::steady_clock::time_point until) {
    if (waited_time && wait_from && until > *wait_from)
      *waited_time += until - *wait_from;
  };
  std::optional<std::chrono::steady_clock::time_point> held_since;
  std::chrono::steady_clock::time_point last_poll;
  // Whether this call has held the emulator longer than give_up_after. A gap of over a second
  // between polls means this process was suspended (e.g. the computer slept), not that the other
  // side went away, so the timer restarts.
  const auto held_too_long = [&] {
    const auto now = std::chrono::steady_clock::now();
    if (!held_since || now - last_poll > std::chrono::seconds(1))
      held_since = now;
    last_poll = now;
    if (give_up_after.count() <= 0 || now - *held_since <= give_up_after)
      return false;
    if (gave_up)
      *gave_up = true;
    return true;
  };
  while (true)
  {
    const auto call_start = std::chrono::steady_clock::now();
    const Orca::Net::Step step = session.OnBoundary(completed_frame, local_pad);
    if (step.kind == Orca::Net::StepKind::Wait || step.kind == Orca::Net::StepKind::Stall)
    {
      if (!wait_from)
        wait_from = std::chrono::steady_clock::now();
    }
    if (step.kind == Orca::Net::StepKind::Wait)
    {
      // Time sync: this machine is ahead. Hold one frame, then pace from now so the throttle
      // doesn't race to make up the held frame.
      if (stopping() || held_too_long())
      {
        end_wait(std::chrono::steady_clock::now());
        return {Orca::Net::StepKind::Ended, step.frame};
      }
      std::this_thread::sleep_for(FRAME_PERIOD);
      waited = true;
      // The session decides a wait before sampling, so the frame uses the pad read now.
      local_pad = sample_local();
      continue;
    }
    if (step.kind != Orca::Net::StepKind::Stall)
    {
      end_wait(call_start);
      if (waited)
        game.ResetPacing();
      return step;
    }
    if (stopping() || held_too_long())
    {
      end_wait(std::chrono::steady_clock::now());
      return {Orca::Net::StepKind::Ended, step.frame};
    }
    if (stall_wait.count() > 0)
      std::this_thread::sleep_for(stall_wait);
  }
}
}  // namespace Rollback
