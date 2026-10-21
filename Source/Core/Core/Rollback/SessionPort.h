// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The emulator side of an Orca rollback session: implements Orca::Net::Game on the snapshot ring.

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Rollback/Rollback.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class System;
}

namespace Rollback
{
namespace OnlineMatch
{
class StallBudget;
}
// Create and destroy only on the CPU thread at a frame boundary, or after emulation stops: SI
// polls read the active port's pads, and the destructor resets the ring and the NAND journal.
class RingPort final : public Orca::Net::Game
{
public:
  // `max_rollback` is the session's Config::max_rollback. `unthrottled` is for tests.
  RingPort(Core::System& system, int max_rollback, bool unthrottled = false);
  ~RingPort() override;
  RingPort(const RingPort&) = delete;
  RingPort& operator=(const RingPort&) = delete;

  bool Save(int frame) override;
  bool Load(int frame) override;
  void SetPads(int frame, const Orca::Net::Pads& pads) override;
  void SetResimulating(bool resimulating) override;
  std::optional<u64> SnapshotChecksum(int frame) override;
  void ResetPacing() override;
  double SaveMs() const override { return m_save_ms; }

  // What port `port` (0-3) reports this frame. The local pad also comes from the session, so both
  // machines see the same bytes.
  GCPadStatus Pad(int port) const { return m_pads[port]; }

  // Drop-in: a joining player replays the host's frames at full speed, without rendering or sound,
  // until it catches up. Pacing restarts when it does.
  void SetCatchingUp(bool catching_up);
  bool CatchingUp() const { return m_catching_up; }
  // True during a rollback re-run. Rollback::IsResimulating() also covers catch-up.
  bool Resimulating() const { return m_resimulating; }
  // Drop-in: loads the host's keyframe as the state at the start of `frame`. Call from the
  // frame-boundary hook, like Load.
  bool LoadImage(MachineImage image, int frame);

  // The port of the running session, or null.
  static RingPort* Active();

private:
  Core::System& m_system;
  // Holds the newest max_rollback + 2 saves. That is enough: the session stalls once it is
  // max_rollback frames past the confirmed one, so any frame it loads or checksums has at most
  // max_rollback newer saves. This also holds with sparse snapshots (Config::snapshot_every).
  SnapshotRing m_ring;
  // Smoothed wall time of one Save; 0 until measured. Saves that allocate are skipped.
  double m_save_ms = 0;
  std::array<GCPadStatus, Orca::Net::MAX_SEATS> m_pads;
  bool m_unthrottled;
  bool m_resimulating = false;
  bool m_catching_up = false;
  // The frame saved since the emulator last ran, since a stalled boundary asks to save it again.
  int m_saved_since_run = -1;
  // The throttle reference from before a rollback load, restored when the re-run ends.
  std::optional<std::pair<s64, TimePoint>> m_reference_before_load;
};

// Runs one session frame boundary: reports the completed frame and the local pad, then loops on
// stalls (sleeping `stall_wait`) and time-sync waits (sleeping one frame) until the session moves
// on or `stopping()` returns true.
//
// Returns Run or Rollback (after a Rollback the state is the start of step.frame), or Ended (see
// Session::Error). Holding longer than `give_up_after` (zero = never) also returns Ended and sets
// *gave_up, unless `hold_silence(now)` says the room is still holding the peer.
// `sample_local` is called once, and again after each wait so the next frame uses a fresh
// pad; stall retries reuse the first sample. `waited`, if given, gets the time spent waiting (from
// the first stall or time-sync step to the last call), which is not this machine's work.
// The optional budget observes actual stalls and room recovery through the same polling path.
Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad()>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr,
                            std::chrono::nanoseconds* waited = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence = {},
                            OnlineMatch::StallBudget* stall_budget = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering = {});
}  // namespace Rollback
