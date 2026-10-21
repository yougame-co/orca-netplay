// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

// When to show a frame that Immediate XFB hands over the moment the game copies it. Each frame is
// due at the throttle's host time for the VI field after its copy; it is shown at a steady offset
// from that time that covers all but the latest few arrivals of the last 3 s. A rollback's re-run
// makes its frame arrive late: when re-runs are frequent the offset grows to cover them, so they
// don't move the picture; when they are rare only that frame is late, and the frames after it
// aren't. See ORCA.md, "Input latency". Pure, so tests can drive it.
namespace VideoCommon
{
class PresentPacer
{
public:
  using Duration = std::chrono::nanoseconds;

  struct Tuning
  {
    // Arrivals kept, in frames (3 s).
    std::size_t window = 180;
    // The quantile of them the offset follows, in thousandths (980 of 180: all but the latest 4).
    int quantile_permille = 980;
    // How far the offset moves towards it per frame, up and down.
    Duration rise_per_frame = std::chrono::microseconds(500);
    Duration fall_per_frame = std::chrono::microseconds(200);
    // Arrivals are counted between these, relative to the due time: a frame never waits past
    // `latest`, and a stray early estimate can't drag the offset down.
    Duration earliest = std::chrono::milliseconds(-34);
    Duration latest = Duration::zero();
  };

  PresentPacer();
  explicit PresentPacer(const Tuning& tuning);

  // Takes one frame's arrival minus its due time (negative: early) and returns the offset from
  // its due time to present at. A frame that arrives after that presents at once.
  Duration Next(Duration arrival);

  Duration Offset() const { return m_offset; }
  const Tuning& GetTuning() const { return m_tuning; }

private:
  Tuning m_tuning;
  std::vector<Duration> m_arrivals;  // a ring of the last `window` arrivals
  std::size_t m_next = 0;
  std::vector<Duration> m_scratch;
  Duration m_offset{};
};
}  // namespace VideoCommon
