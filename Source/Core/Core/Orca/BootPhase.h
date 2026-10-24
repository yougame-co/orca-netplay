// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace Orca::BootPhase
{
// Fixed names: no caller-provided strings, allocations, or protocol line injection.
enum class Phase
{
  Main,
  Config,
  Disc,
  HashCached,
  HashFull,
  Video,
  Shaders,
  Audio,
  Window,
  EmuThread,
  IOSReload,
  Launcher,
  FirstFrame,
  SystemInit,
  WiiRoot,
  Sysconf,
  CoreInit,
  VideoInfo,
  WindowCreated,
  VideoDevice,
  VideoSwapchain,
  GuestHook,
  FirstPresent,
  Count
};
const char* Name(Phase phase);

class Clock final
{
public:
  using Time = std::chrono::steady_clock;
  explicit Clock(std::FILE* output, Time::time_point start = Time::now());
  void SetOutput(std::FILE* output);
  void Mark(Phase phase);
  // Finish and emit the guest counters in the same critical section, once.
  // Frames are the presented unique XFB count since console boot (including a launcher).
  void FirstFrame(std::uint64_t frames, std::size_t jit_blocks);
  // Replay early marks to Dolphin's logger once UICommon has initialized it.
  void LogPending();

private:
  struct Record
  {
    Phase phase;
    long long elapsed;
    long long delta;
  };
  struct GuestStats
  {
    std::uint64_t frames;
    std::size_t jit_blocks;
  };
  void Mark(Phase phase, const GuestStats* guest);
  void LogPendingLocked();
  static constexpr std::size_t COUNT = static_cast<std::size_t>(Phase::Count);
  const Time::time_point m_start;
  std::FILE* m_output;
  std::mutex m_mutex;
  std::atomic<bool> m_finished{false};
  std::array<bool, COUNT> m_seen{};
  std::array<Record, COUNT> m_records{};
  std::size_t m_count = 0;
  std::size_t m_logged = 0;
  long long m_previous = 0;
  std::array<char, 128> m_guest_summary{};
  bool m_summary_logged = false;
};

// Start at the first statement of main (wmain on Windows). Never resets in this process.
void Start();
void SetOutput(std::FILE* output);
void Mark(Phase phase);
void FirstFrame(std::uint64_t frames, std::size_t jit_blocks);
void LogPending();
}  // namespace Orca::BootPhase
