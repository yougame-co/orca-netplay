// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <string_view>

#include <gtest/gtest.h>
#include <pugixml.hpp>

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

TEST(EmbedMacTiming, StableParentSettlesAtFiveHundredMilliseconds)
{
  Embed::Mac::SettleGate gate;
  EXPECT_FALSE(gate.Ready(START));
  gate.Observe(START, true, {{10, 20, 800, 600}});
  for (int ms = 100; ms <= 400; ms += 100)
    gate.Observe(START + std::chrono::milliseconds(ms), true, {{10, 20, 800, 600}});
  EXPECT_FALSE(gate.Ready(START + 499ms));
  EXPECT_TRUE(gate.Ready(START + 500ms));
}

TEST(EmbedMacTiming, EveryBoundsChangeRestartsSettling)
{
  for (std::size_t component = 0; component < 4; ++component)
  {
    Embed::Mac::SettleGate gate;
    Embed::Mac::SettleGate::Bounds bounds{10, 20, 800, 600};
    gate.Observe(START, true, bounds);
    ASSERT_TRUE(gate.Ready(START + 500ms));
    bounds[component] += 0.5;
    gate.Observe(START + 600ms, true, bounds);
    EXPECT_FALSE(gate.Ready(START + 1099ms)) << component;
    EXPECT_TRUE(gate.Ready(START + 1100ms)) << component;
  }
}

TEST(EmbedMacTiming, OffScreenParentRestartsSettling)
{
  Embed::Mac::SettleGate gate;
  gate.Observe(START, true, {{10, 20, 800, 600}});
  ASSERT_TRUE(gate.Ready(START + 500ms));
  gate.Observe(START + 600ms, false, {{10, 20, 800, 600}});
  EXPECT_FALSE(gate.Ready(START + 1200ms));
  gate.Observe(START + 1200ms, true, {{10, 20, 800, 600}});
  EXPECT_FALSE(gate.Ready(START + 1699ms));
  EXPECT_TRUE(gate.Ready(START + 1700ms));
}

TEST(EmbedMacTiming, MissingParentRestartsSettling)
{
  Embed::Mac::SettleGate gate;
  gate.Observe(START, true, {{10, 20, 800, 600}});
  ASSERT_TRUE(gate.Ready(START + 500ms));
  gate.Observe(START + 600ms, true, std::nullopt);
  EXPECT_FALSE(gate.Ready(START + 1200ms));
  gate.Observe(START + 1200ms, true, {{10, 20, 800, 600}});
  EXPECT_FALSE(gate.Ready(START + 1699ms));
  EXPECT_TRUE(gate.Ready(START + 1700ms));
}

TEST(EmbedMacTiming, StableParentStaysReadyWithoutNewEvents)
{
  Embed::Mac::SettleGate gate;
  gate.Observe(START, true, {{10, 20, 800, 600}});
  ASSERT_TRUE(gate.Ready(START + 500ms));
  EXPECT_TRUE(gate.Ready(START + 1h));
  gate.Observe(START + 1h, true, {{10, 20, 800, 600}});
  EXPECT_TRUE(gate.Ready(START + 1h));
}

TEST(EmbedMacTiming, BundleStartsAsAnAgentWithoutAGameCategory)
{
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "../../Core/DolphinNoGUI/Orca-Info.plist.in";
  pugi::xml_document plist;
  ASSERT_TRUE(plist.load_file(path.string().c_str())) << path.string();
  const auto dict = plist.child("plist").child("dict");
  ASSERT_TRUE(dict);
  int agent_keys = 0;
  for (const auto key : dict.children("key"))
  {
    const std::string_view name = key.child_value();
    EXPECT_NE(name, "LSApplicationCategoryType");
    if (name == "LSUIElement")
    {
      ++agent_keys;
      EXPECT_STREQ(key.next_sibling().name(), "true");
    }
  }
  EXPECT_EQ(agent_keys, 1);
}
