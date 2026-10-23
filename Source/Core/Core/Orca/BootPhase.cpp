// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/BootPhase.h"

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"

namespace Orca::BootPhase
{
const char* Name(Phase phase)
{
  static constexpr std::array names = {
      "main",   "config",     "disc",       "hash-cached", "hash-full", "video", "shaders",
      "audio",  "window",     "emu-thread", "ios-reload",  "launcher",  "first-frame"};
  static_assert(names.size() == static_cast<std::size_t>(Phase::Count));
  const auto index = static_cast<std::size_t>(phase);
  return index < names.size() ? names[index] : nullptr;
}

Clock::Clock(std::FILE* output, Time::time_point start) : m_start(start), m_output(output)
{
}

void Clock::SetOutput(std::FILE* output)
{
  std::lock_guard lock(m_mutex);
  m_output = output;
}

void Clock::Mark(Phase phase)
{
  const char* const name = Name(phase);
  if (m_finished.load(std::memory_order_relaxed) || !name)
    return;
  std::lock_guard lock(m_mutex);
  const auto index = static_cast<std::size_t>(phase);
  if (m_finished.load(std::memory_order_relaxed) || m_seen[index])
    return;
  m_seen[index] = true;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Time::now() - m_start).count();
  const Record record{phase, static_cast<long long>(elapsed),
                       static_cast<long long>(elapsed) - m_previous};
  m_records[m_count++] = record;
  m_previous = record.elapsed;
  if (m_output)
  {
    std::fprintf(m_output, "orca boot phase %s %lld %lld\n", name, record.elapsed, record.delta);
    std::fflush(m_output);
  }
  LogPendingLocked();
  if (phase == Phase::FirstFrame)
    m_finished.store(true, std::memory_order_relaxed);
}

void Clock::LogPendingLocked()
{
  auto* const logs = Common::Log::LogManager::GetInstance();
  if (!logs)
    return;
  while (m_logged < m_count)
  {
    const auto& record = m_records[m_logged++];
    // Records only ever hold named phases (Mark); the check keeps gcc's -Wformat-truncation sure.
    const char* const name = Name(record.phase);
    if (!name)
      continue;
    std::array<char, 128> line{};
    std::snprintf(line.data(), line.size(), "orca boot phase %s %lld %lld", name, record.elapsed,
                  record.delta);
    // Like BootMemory, these few diagnostics reach the normal listeners even with BOOT off.
    logs->LogWithFullPath(Common::Log::LogLevel::LNOTICE, Common::Log::LogType::BOOT,
                          "Orca/BootPhase.cpp", 0, line.data());
  }
}

void Clock::LogPending()
{
  std::lock_guard lock(m_mutex);
  LogPendingLocked();
}

static Clock& ProcessClock()
{
  static Clock clock(stdout);
  return clock;
}

void Start()
{
  ProcessClock();
}

void SetOutput(std::FILE* output)
{
  ProcessClock().SetOutput(output);
}

void Mark(Phase phase)
{
  ProcessClock().Mark(phase);
}

void LogPending()
{
  ProcessClock().LogPending();
}
}  // namespace Orca::BootPhase
