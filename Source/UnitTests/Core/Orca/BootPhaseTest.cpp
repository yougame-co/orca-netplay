// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Core/Orca/BootPhase.h"

namespace
{
using Orca::BootPhase::Clock;
using Orca::BootPhase::Name;
using Orca::BootPhase::Phase;

class TempStream final
{
public:
  TempStream() : m_dir(File::CreateTempDir())
  {
    if (!m_dir.empty())
      m_file = std::fopen((m_dir + "/phases.log").c_str(), "w+b");
  }
  ~TempStream()
  {
    if (m_file)
      std::fclose(m_file);
    if (!m_dir.empty())
      File::DeleteDirRecursively(m_dir);
  }
  std::FILE* Get() const { return m_file; }

private:
  std::string m_dir;
  std::FILE* m_file = nullptr;
};

TEST(OrcaBootPhase, OnceInArrivalOrderWithMonotonicMillisecondsAndExactLines)
{
  const TempStream stream;
  std::FILE* const output = stream.Get();
  ASSERT_NE(output, nullptr);
  Clock clock(output);
  clock.Mark(static_cast<Phase>(-1));
  clock.Mark(Phase::Count);
  clock.Mark(Phase::Main);
  clock.Mark(Phase::Main);
  clock.Mark(Phase::EmuThread);
  clock.Mark(Phase::Audio);
  clock.Mark(Phase::FirstFrame);
  clock.Mark(Phase::Config);  // All diagnostics stop after the first frame.
  std::rewind(output);
  long long previous = 0;
  for (const Phase phase : {Phase::Main, Phase::EmuThread, Phase::Audio, Phase::FirstFrame})
  {
    std::array<char, 128> line{};
    ASSERT_NE(std::fgets(line.data(), static_cast<int>(line.size()), output), nullptr);
    std::array<char, 32> name{};
    long long elapsed = -1;
    long long delta = -1;
    ASSERT_EQ(std::sscanf(line.data(), "orca boot phase %31s %lld %lld", name.data(), &elapsed,
                          &delta), 3);
    EXPECT_STREQ(name.data(), Name(phase));
    std::array<char, 128> expected{};
    std::snprintf(expected.data(), expected.size(), "orca boot phase %s %lld %lld\n", Name(phase),
                  elapsed, delta);
    EXPECT_STREQ(line.data(), expected.data());
    EXPECT_GE(elapsed, previous);
    EXPECT_EQ(delta, elapsed - previous);
    previous = elapsed;
  }
  EXPECT_EQ(std::fgetc(output), EOF);
}

TEST(OrcaBootPhase, ElapsedUsesTheSuppliedStart)
{
  const TempStream stream;
  std::FILE* const output = stream.Get();
  ASSERT_NE(output, nullptr);
  Clock clock(output, Clock::Time::now() - std::chrono::milliseconds(42));
  clock.Mark(Phase::Main);
  std::rewind(output);
  long long elapsed = -1;
  long long delta = -1;
  ASSERT_EQ(std::fscanf(output, "orca boot phase main %lld %lld", &elapsed, &delta), 2);
  EXPECT_GE(elapsed, 42);
  EXPECT_EQ(delta, elapsed);
}

TEST(OrcaBootPhase, NamesAreSingleProtocolWords)
{
  for (int i = 0; i < static_cast<int>(Phase::Count); ++i)
  {
    const std::string name(Name(static_cast<Phase>(i)));
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz-"), std::string::npos);
  }
  EXPECT_EQ(Name(Phase::Count), nullptr);
}

TEST(OrcaBootPhase, ConcurrentDuplicatesEmitOnlyOneLine)
{
  const TempStream stream;
  std::FILE* const output = stream.Get();
  ASSERT_NE(output, nullptr);
  Clock clock(output);
  auto mark = [&clock] {
    for (int i = 0; i < 100; ++i)
      clock.Mark(Phase::Main);
  };
  std::thread first(mark), second(mark);
  first.join();
  second.join();
  std::rewind(output);
  std::array<char, 128> line{};
  EXPECT_NE(std::fgets(line.data(), static_cast<int>(line.size()), output), nullptr);
  EXPECT_EQ(std::fgetc(output), EOF);
}
TEST(OrcaBootPhase, SubmarksUseFixedNamesOnceInAnyArrivalOrder)
{
  const TempStream stream;
  std::FILE* const output = stream.Get();
  ASSERT_NE(output, nullptr);
  Clock clock(output);
  const std::array phases = {Phase::FirstPresent, Phase::VideoSwapchain, Phase::Sysconf,
                            Phase::GuestHook, Phase::WindowCreated, Phase::VideoDevice,
                            Phase::WiiRoot, Phase::VideoInfo, Phase::CoreInit, Phase::SystemInit};
  const std::array names = {"first-present", "video-swapchain", "sysconf", "guest-hook",
                           "window-created", "video-device", "wii-root", "video-info",
                           "core-init", "system-init"};
  for (const Phase phase : phases)
    clock.Mark(phase);
  for (auto it = phases.rbegin(); it != phases.rend(); ++it)
    clock.Mark(*it);
  std::rewind(output);
  long long previous = 0;
  for (std::size_t i = 0; i < phases.size(); ++i)
  {
    ASSERT_NE(Name(phases[i]), nullptr);
    EXPECT_STREQ(Name(phases[i]), names[i]);
    std::array<char, 128> line{};
    ASSERT_NE(std::fgets(line.data(), static_cast<int>(line.size()), output), nullptr);
    std::array<char, 32> name{};
    long long elapsed = -1;
    long long delta = -1;
    ASSERT_EQ(std::sscanf(line.data(), "orca boot phase %31s %lld %lld", name.data(), &elapsed,
                          &delta), 3);
    EXPECT_STREQ(name.data(), names[i]);
    std::array<char, 128> expected{};
    std::snprintf(expected.data(), expected.size(), "orca boot phase %s %lld %lld\n", names[i],
                  elapsed, delta);
    EXPECT_STREQ(line.data(), expected.data());
    EXPECT_GE(elapsed, previous);
    EXPECT_EQ(delta, elapsed - previous);
    previous = elapsed;
  }
  EXPECT_EQ(std::fgetc(output), EOF);
}

TEST(OrcaBootPhase, ConcurrentSubmarksStayOnceAndMonotonic)
{
  const TempStream stream;
  std::FILE* const output = stream.Get();
  ASSERT_NE(output, nullptr);
  Clock clock(output);
  const std::array phases = {Phase::SystemInit, Phase::WiiRoot, Phase::Sysconf, Phase::CoreInit,
                            Phase::VideoInfo, Phase::WindowCreated, Phase::VideoDevice,
                            Phase::VideoSwapchain, Phase::GuestHook, Phase::FirstPresent};
  std::thread first([&] {
    for (const Phase phase : phases)
      clock.Mark(phase);
  });
  std::thread second([&] {
    for (auto it = phases.rbegin(); it != phases.rend(); ++it)
      clock.Mark(*it);
  });
  first.join();
  second.join();
  std::rewind(output);
  std::array<bool, phases.size()> seen{};
  long long previous = 0;
  for (std::size_t i = 0; i < phases.size(); ++i)
  {
    std::array<char, 128> line{};
    ASSERT_NE(std::fgets(line.data(), static_cast<int>(line.size()), output), nullptr);
    std::array<char, 32> name{};
    long long elapsed = -1;
    long long delta = -1;
    ASSERT_EQ(std::sscanf(line.data(), "orca boot phase %31s %lld %lld", name.data(), &elapsed,
                          &delta), 3);
    bool found = false;
    for (std::size_t j = 0; j < phases.size(); ++j)
    {
      if (std::string(name.data()) == Name(phases[j]))
      {
        EXPECT_FALSE(seen[j]);
        seen[j] = true;
        found = true;
      }
    }
    EXPECT_TRUE(found);
    EXPECT_GE(elapsed, previous);
    EXPECT_EQ(delta, elapsed - previous);
    previous = elapsed;
  }
  EXPECT_EQ(std::fgetc(output), EOF);
}

TEST(OrcaBootPhase, FirstFrameSummaryIsExactOnceAndStopsAllSubmarks)
{
  const TempStream stream;
  std::FILE* const output = stream.Get();
  ASSERT_NE(output, nullptr);
  Clock clock(output);
  std::thread first([&clock] { clock.FirstFrame(42, 12345); });
  std::thread second([&clock] { clock.FirstFrame(42, 12345); });
  first.join();
  second.join();
  for (int i = 0; i < static_cast<int>(Phase::Count); ++i)
    clock.Mark(static_cast<Phase>(i));
  clock.FirstFrame(99, 999);
  std::rewind(output);
  std::array<char, 128> line{};
  ASSERT_NE(std::fgets(line.data(), static_cast<int>(line.size()), output), nullptr);
  long long elapsed = -1;
  long long delta = -1;
  ASSERT_EQ(std::sscanf(line.data(), "orca boot phase first-frame %lld %lld", &elapsed, &delta), 2);
  EXPECT_GE(elapsed, 0);
  EXPECT_EQ(delta, elapsed);
  std::array<char, 128> expected{};
  std::snprintf(expected.data(), expected.size(), "orca boot phase first-frame %lld %lld\n",
                elapsed, delta);
  EXPECT_STREQ(line.data(), expected.data());
  ASSERT_NE(std::fgets(line.data(), static_cast<int>(line.size()), output), nullptr);
  EXPECT_STREQ(line.data(), "orca boot guest frames=42 jit-blocks=12345\n");
  EXPECT_EQ(std::fgetc(output), EOF);
}

TEST(OrcaBootPhase, SummaryNeedsNoOutputStream)
{
  Clock clock(nullptr);
  clock.FirstFrame(0, 0);
  const TempStream stream;
  std::FILE* const output = stream.Get();
  ASSERT_NE(output, nullptr);
  clock.SetOutput(output);
  clock.FirstFrame(1, 1);
  clock.Mark(Phase::GuestHook);
  std::rewind(output);
  EXPECT_EQ(std::fgetc(output), EOF);
}
}  // namespace
