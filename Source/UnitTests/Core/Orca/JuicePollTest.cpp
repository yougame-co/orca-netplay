// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "JuicePollHarness.h"

TEST(OrcaJuicePoll, DroppedDatagramContinuesToNextDatagram)
{
  const auto result = OrcaJuicePollRun(1, 0, 1);
  EXPECT_EQ(result.receive_calls, 3);
  EXPECT_EQ(result.delivered, 1);
  EXPECT_EQ(result.failures, 0);
  EXPECT_EQ(result.updates, 1);
  EXPECT_EQ(result.ready, 1);
  EXPECT_EQ(result.error_logs, 0);
  EXPECT_EQ(result.drop_logs, 1);
}

TEST(OrcaJuicePoll, DroppedDatagramsCountAgainstFairnessLimit)
{
  const auto result = OrcaJuicePollRun(1000, 0, 1);
  EXPECT_EQ(result.receive_calls, 1000);
  EXPECT_EQ(result.delivered, 0);
  EXPECT_EQ(result.failures, 0);
  EXPECT_EQ(result.updates, 1);
  EXPECT_EQ(result.ready, 1);
  EXPECT_EQ(result.error_logs, 0);
  EXPECT_EQ(result.drop_logs, 1000);
}

TEST(OrcaJuicePoll, DatagramsAfterFairnessLimitAreDeliveredOnNextPoll)
{
  for (const int dropped : {1000, 1001})
  {
    const auto result = OrcaJuicePollRun(dropped, 0, 2);
    EXPECT_EQ(result.receive_calls, dropped + 2);
    EXPECT_EQ(result.delivered, 1);
    EXPECT_EQ(result.failures, 0);
    EXPECT_EQ(result.updates, 2);
    EXPECT_EQ(result.ready, 1);
    EXPECT_EQ(result.error_logs, 0);
    EXPECT_EQ(result.drop_logs, dropped);
  }
}

TEST(OrcaJuicePoll, OtherReceiveErrorsStillFail)
{
  const auto result = OrcaJuicePollRun(0, 1, 1);
  EXPECT_EQ(result.receive_calls, 1);
  EXPECT_EQ(result.delivered, 0);
  EXPECT_EQ(result.failures, 1);
  EXPECT_EQ(result.updates, 0);
  EXPECT_EQ(result.ready, 0);
  EXPECT_EQ(result.error_logs, 1);
  EXPECT_EQ(result.drop_logs, 0);
}

TEST(OrcaJuicePoll, NormalDatagramsStillDrainUntilWouldBlock)
{
  const auto result = OrcaJuicePollRun(0, 0, 1);
  EXPECT_EQ(result.receive_calls, 2);
  EXPECT_EQ(result.delivered, 1);
  EXPECT_EQ(result.failures, 0);
  EXPECT_EQ(result.updates, 1);
  EXPECT_EQ(result.ready, 1);
  EXPECT_EQ(result.error_logs, 0);
  EXPECT_EQ(result.drop_logs, 0);
}
