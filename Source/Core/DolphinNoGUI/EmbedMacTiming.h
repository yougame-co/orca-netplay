// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

namespace Embed::Mac
{
// Reduce WindowServer load after the 10-18 hang: following a window does not need 120 Hz queries.
constexpr auto PARENT_QUERY_INTERVAL = std::chrono::milliseconds(100);
constexpr auto ABOVE_QUERY_INTERVAL = std::chrono::milliseconds(250);
constexpr auto FORCED_QUERY_FLOOR = std::chrono::milliseconds(20);

class QueryTimer
{
public:
  using Clock = std::chrono::steady_clock;

  explicit QueryTimer(std::chrono::milliseconds interval) : m_interval(interval) {}

  // Keep a command refresh pending if it arrives inside the force floor.
  void Request() { m_requested = true; }

  // Owner activation must check ordering immediately, independent of the periodic/command timer.
  void Reset()
  {
    m_last.reset();
    m_requested = false;
  }

  bool Due(Clock::time_point now)
  {
    if (m_last && now - *m_last < (m_requested ? FORCED_QUERY_FLOOR : m_interval))
      return false;
    m_last = now;
    m_requested = false;
    return true;
  }

  bool Force(Clock::time_point now)
  {
    Request();
    return Due(now);
  }

private:
  std::chrono::milliseconds m_interval;
  std::optional<Clock::time_point> m_last;
  bool m_requested = false;
};

class StartupBackoff
{
public:
  std::chrono::milliseconds Next()
  {
    constexpr std::array delays{20, 50, 100, 200};
    const auto delay = std::chrono::milliseconds(delays[m_step]);
    if (m_step + 1 < delays.size())
      ++m_step;
    return delay;
  }

private:
  std::size_t m_step = 0;
};
}  // namespace Embed::Mac
