// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <random>

#include <gtest/gtest.h>

#include "VideoCommon/PresentPacing.h"

using VideoCommon::PresentPacer;
using namespace std::chrono_literals;

namespace
{
using Duration = PresentPacer::Duration;

// A frame copied about 14 ms before its due time (the copy right after the game's logic), give or
// take a millisecond.
Duration Bare(std::mt19937& rng)
{
  return -14ms + Duration(std::uniform_int_distribution<long long>(-1'000'000, 1'000'000)(rng));
}
}  // namespace

TEST(OrcaPresentPacing, FollowsSteadyArrivalsNearTheirLatest)
{
  std::mt19937 rng(1);
  PresentPacer pacer;
  Duration offset{};
  for (int i = 0; i < 2000; ++i)
    offset = pacer.Next(Bare(rng));
  // The 98th percentile of -15..-13 ms.
  EXPECT_GT(offset, -13200us);
  EXPECT_LT(offset, -12900us);
}

TEST(OrcaPresentPacing, RareLateFramesMoveNothing)
{
  // One rollback's re-run every 200 frames, 10 ms long: only that frame is late; the frames after
  // it present when they would have.
  std::mt19937 rng(2);
  PresentPacer pacer;
  Duration lowest = Duration::max(), highest = Duration::min();
  for (int i = 0; i < 6000; ++i)
  {
    const bool late = i % 200 == 199;
    const Duration offset = pacer.Next(Bare(rng) + (late ? 10ms : 0ms));
    if (i >= 1000)
    {
      lowest = std::min(lowest, offset);
      highest = std::max(highest, offset);
    }
  }
  EXPECT_LT(highest, -12500us);
  EXPECT_LT(highest - lowest, 600us);
}

TEST(OrcaPresentPacing, FrequentLateFramesAreAbsorbed)
{
  // A re-run every 10 frames: the offset rises to cover them and then holds, so the frames between
  // them wait for the same time instead of following each one.
  std::mt19937 rng(3);
  PresentPacer pacer;
  int covered = 0, counted = 0;
  Duration lowest = Duration::max(), highest = Duration::min();
  for (int i = 0; i < 6000; ++i)
  {
    const bool late = i % 10 == 9;
    const Duration arrival = Bare(rng) + (late ? 8ms : 0ms);
    const Duration offset = pacer.Next(arrival);
    if (i >= 1000)
    {
      lowest = std::min(lowest, offset);
      highest = std::max(highest, offset);
      if (late)
      {
        ++counted;
        covered += arrival <= offset;
      }
    }
  }
  EXPECT_GT(covered, counted * 2 / 3);
  EXPECT_LT(highest - lowest, 1500us);
  EXPECT_LT(highest, -4ms);  // no later than the re-runs need
}

TEST(OrcaPresentPacing, MovesAtMostItsStepPerFrame)
{
  PresentPacer::Tuning tuning;
  PresentPacer pacer(tuning);
  Duration offset = pacer.Next(-14ms);
  EXPECT_EQ(offset, -14ms);
  for (int i = 0; i < 400; ++i)
  {
    const Duration next = pacer.Next(-2ms);
    EXPECT_LE(next - offset, tuning.rise_per_frame);
    EXPECT_GE(next, offset);
    offset = next;
  }
  EXPECT_EQ(offset, -2ms);
  int frames = 0;
  while (offset > -14ms && frames < 5000)
  {
    const Duration next = pacer.Next(-14ms);
    EXPECT_LE(offset - next, tuning.fall_per_frame);
    EXPECT_LE(next, offset);
    offset = next;
    ++frames;
  }
  EXPECT_EQ(offset, -14ms);
  // The late arrivals leave the window first, then it walks down 12 ms at 200 us a frame.
  EXPECT_GT(frames, 200);
  EXPECT_LT(frames, 260);
}

TEST(OrcaPresentPacing, NeverWaitsPastTheDueTime)
{
  PresentPacer pacer;
  for (int i = 0; i < 1000; ++i)
    EXPECT_LE(pacer.Next(40ms), 0ms);  // a stall's frame, or a re-run longer than a frame
  EXPECT_EQ(pacer.Offset(), 0ms);
  PresentPacer early;
  EXPECT_EQ(early.Next(-500ms), early.GetTuning().earliest);
}
