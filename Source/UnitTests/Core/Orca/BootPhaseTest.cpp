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
}  // namespace
