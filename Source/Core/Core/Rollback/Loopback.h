// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Test only: runs a real Orca session against a virtual remote player in the same process. A plain
// run records every pad and the RAM checksum per frame (YG_PADREC). A second run replays that
// recording through Orca::Net::Session with the remote's inputs arriving late, so the session
// predicts, mispredicts and rolls back. The remote also sends the plain run's checksums, so any
// divergence ends the session as a desync.

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

namespace Rollback::Loopback
{
// One recorded frame: every port's pad in wire form, and the RAM checksum at its start.
struct RecordedFrame
{
  Orca::Net::Pads pads{};
  u64 checksum = 0;
};

// One line per frame: "<frame> <pad 1> <pad 2> <pad 3> <pad 4> <checksum>", all hex.
std::string FormatRecordedFrame(int frame, const Orca::Net::Pads& pads, u64 checksum);
// Frames 0..n-1 of a recording; empty if the file is missing, malformed or has a gap.
std::vector<RecordedFrame> LoadRecording(const std::string& path);

// Remote seats replaying a recording. Their clock advances on first-pass frames and stall retries,
// never on rollback re-runs. At tick t a seat has sent inputs up to frame t - lag + input_delay,
// where lag is `lag` plus a deterministic per-seat jitter in 0..`jitter`. Every `checksum_every`
// frames each seat also sends the recorded checksum.
class VirtualRemote final : public Orca::Net::Transport
{
public:
  VirtualRemote(const std::vector<RecordedFrame>& recording, std::vector<int> seats, int local_seat,
                int lag, int jitter, int input_delay, int checksum_every);

  // Call on each first-pass local frame. After a spike, first pays back any lead the remote took,
  // as time sync would.
  void Tick();
  // Call on each retry while the local player stalls. Without spikes, one tick per retry; with
  // them, one tick per frame of wall time, at most max_lead ticks ahead.
  void TickWhileWaiting();
  // From this frame on, remote seats send nothing (a frozen game with its connection still open).
  // Negative means never.
  void SilenceFrom(int frame) { m_silent_from = frame; }
  // Every `period` of wall time, hold all incoming packets for `length`, then deliver them in one
  // burst, like a real latency spike.
  void StartSpikes(std::chrono::milliseconds period, std::chrono::milliseconds length,
                   int max_lead);
  int Spikes() const { return m_spikes; }
  // True once every recorded input has been sent.
  bool Exhausted() const;

  void Send(const Orca::Net::Packet& packet) override;
  std::vector<Orca::Net::Packet> Receive() override;
  bool Connected() const override { return true; }

private:
  struct Seat
  {
    int seat = 0;
    int delivered = -1;  // newest frame whose input was sent
    int last_checksum = 0;
  };
  int Lag(int seat) const;

  const std::vector<RecordedFrame>& m_recording;
  std::vector<Seat> m_seats;
  int m_local_seat;
  int m_lag;
  int m_jitter;
  int m_input_delay;
  int m_checksum_every;
  int m_ticks = 0;
  int m_silent_from = -1;
  int m_local_newest = -1;  // newest local frame received, echoed back as the ack
  // Zero period means no spikes.
  std::chrono::milliseconds m_spike_period{0};
  std::chrono::milliseconds m_spike_length{0};
  std::chrono::steady_clock::time_point m_spike_start;
  std::chrono::steady_clock::time_point m_last_tick;
  int m_max_lead = 0;
  int m_lead = 0;  // ticks the remote ran ahead during a stall, not yet paid back
  bool m_spiking = false;
  int m_spikes = 0;
};
}  // namespace Rollback::Loopback
