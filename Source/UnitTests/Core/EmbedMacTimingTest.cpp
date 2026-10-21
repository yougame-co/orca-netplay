// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include <gtest/gtest.h>

#include "DolphinNoGUI/EmbedMacTiming.h"

using namespace std::chrono_literals;

namespace
{
constexpr Embed::Mac::QueryTimer::Clock::time_point START{};
}  // namespace

TEST(EmbedMacTiming, ParentQueriesAtMostTenTimesPerSecond)
{
  Embed::Mac::QueryTimer timer(Embed::Mac::PARENT_QUERY_INTERVAL);
  int queries = 0;
  for (int ms = 0; ms < 1000; ++ms)
  {
    const bool due = timer.Due(START + std::chrono::milliseconds(ms));
    EXPECT_EQ(due, ms % 100 == 0) << ms;
    queries += due;
  }
  EXPECT_EQ(queries, 10);
  EXPECT_TRUE(timer.Due(START + 1000ms));
}

TEST(EmbedMacTiming, AboveQueriesAtMostFourTimesPerSecond)
{
  Embed::Mac::QueryTimer timer(Embed::Mac::ABOVE_QUERY_INTERVAL);
  int queries = 0;
  for (int ms = 0; ms < 1000; ++ms)
  {
    const bool due = timer.Due(START + std::chrono::milliseconds(ms));
    EXPECT_EQ(due, ms % 250 == 0) << ms;
    queries += due;
  }
  EXPECT_EQ(queries, 4);
  EXPECT_TRUE(timer.Due(START + 1000ms));
}

TEST(EmbedMacTiming, CommandBurstsRespectTwentyMillisecondFloor)
{
  for (const auto interval : {Embed::Mac::PARENT_QUERY_INTERVAL, Embed::Mac::ABOVE_QUERY_INTERVAL})
  {
    Embed::Mac::QueryTimer timer(interval);
    EXPECT_TRUE(timer.Force(START));
    for (int ms = 1; ms <= 1000; ++ms)
      EXPECT_EQ(timer.Force(START + std::chrono::milliseconds(ms)), ms % 20 == 0) << ms;
  }
}

TEST(EmbedMacTiming, DeferredForceIsRetainedUntilFloorThenReturnsToNormalCadence)
{
  Embed::Mac::QueryTimer timer(Embed::Mac::PARENT_QUERY_INTERVAL);
  EXPECT_TRUE(timer.Due(START));
  EXPECT_FALSE(timer.Force(START + 1ms));
  EXPECT_FALSE(timer.Due(START + 19ms));
  EXPECT_TRUE(timer.Due(START + 20ms));
  EXPECT_FALSE(timer.Due(START + 119ms));
  EXPECT_TRUE(timer.Due(START + 120ms));
}

TEST(EmbedMacTiming, OwnerActivationRefreshesAboveWithoutRefreshingParent)
{
  Embed::Mac::QueryTimer parent(Embed::Mac::PARENT_QUERY_INTERVAL);
  Embed::Mac::QueryTimer above(Embed::Mac::ABOVE_QUERY_INTERVAL);
  EXPECT_TRUE(parent.Due(START));
  EXPECT_TRUE(above.Due(START));
  above.Reset();
  EXPECT_TRUE(above.Due(START + 1ms));
  EXPECT_FALSE(parent.Due(START + 1ms));
  EXPECT_FALSE(above.Due(START + 250ms));
  EXPECT_TRUE(above.Due(START + 251ms));
}

TEST(EmbedMacTiming, SlowPassDoesNotCatchUpWithABurst)
{
  Embed::Mac::QueryTimer timer(Embed::Mac::PARENT_QUERY_INTERVAL);
  EXPECT_TRUE(timer.Due(START));
  EXPECT_TRUE(timer.Due(START + 250ms));
  EXPECT_FALSE(timer.Due(START + 250ms));
  EXPECT_FALSE(timer.Due(START + 349ms));
  EXPECT_TRUE(timer.Due(START + 350ms));
}

TEST(EmbedMacTiming, StartupBackoffCapsAtTwoHundredMilliseconds)
{
  Embed::Mac::StartupBackoff backoff;
  EXPECT_EQ(backoff.Next(), 20ms);
  EXPECT_EQ(backoff.Next(), 50ms);
  EXPECT_EQ(backoff.Next(), 100ms);
  for (int i = 0; i < 20; ++i)
    EXPECT_EQ(backoff.Next(), 200ms);
}
