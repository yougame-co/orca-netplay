// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include <gtest/gtest.h>

#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/Search.h"

namespace
{
using namespace Orca::UX;
using namespace std::chrono_literals;

class OrcaUXSearchWait : public ::testing::Test
{
protected:
  void SetUp() override
  {
    QueueApp::SetSoloReady(false);
    QueueApp::SetOnSoloCss(false);
    Search::End();
  }
  void TearDown() override
  {
    QueueApp::SetSoloReady(false);
    QueueApp::SetOnSoloCss(false);
    Search::End();
  }
  const std::chrono::steady_clock::time_point m_start{100s};
};

TEST_F(OrcaUXSearchWait, FormatsElapsedWholeMinutesAndSeconds)
{
  Search::Begin(false, m_start);
  EXPECT_EQ(Search::WaitingLine(m_start), "Waiting for another player… 0:00");
  EXPECT_EQ(Search::WaitingLine(m_start + 36s), "Waiting for another player… 0:36");
  EXPECT_EQ(Search::WaitingLine(m_start + 65s), "Waiting for another player… 1:05");
  EXPECT_EQ(Search::WaitingLine(m_start + 600s), "Waiting for another player… 10:00");
  EXPECT_EQ(Search::WaitingLine(m_start + 3605s), "Waiting for another player… 60:05");
  EXPECT_EQ(Search::WaitingLine(m_start + 36999ms), "Waiting for another player… 0:36");
}

TEST_F(OrcaUXSearchWait, NewSearchRestartsAndMatchOrEndClears)
{
  EXPECT_TRUE(Search::WaitingLine(m_start).empty());
  for (const bool hosting : {false, true})
  {
    Search::Begin(false, m_start);
    EXPECT_EQ(Search::WaitingLine(m_start + 36s), "Waiting for another player… 0:36");
    Search::Begin(true, m_start + 36s);
    EXPECT_EQ(Search::WaitingLine(m_start + 36s), "Waiting for another player… 0:00");
    Search::Matched(hosting);
    EXPECT_TRUE(Search::WaitingLine(m_start + 65s).empty());
  }
  Search::Begin(false, m_start);
  Search::End();
  EXPECT_TRUE(Search::WaitingLine(m_start + 65s).empty());
  Search::Begin(false, m_start);
  Search::JoinStarted();
  EXPECT_TRUE(Search::WaitingLine(m_start + 65s).empty());
}

TEST_F(OrcaUXSearchWait, SoloReadinessStartsOnceCancelsAndRestarts)
{
  QueueApp::SetSoloReady(true);
  EXPECT_EQ(Search::WaitingLine(), "Waiting for another player… 0:00");
  // Set an earlier start without sleeping, then exercise the per-frame ready publication.
  Search::SetWaiting(true, m_start);
  QueueApp::SetSoloReady(true);
  EXPECT_EQ(Search::WaitingLine(m_start + 36s), "Waiting for another player… 0:36");
  Search::Matched(true);
  QueueApp::SetSoloReady(true);
  EXPECT_TRUE(Search::WaitingLine().empty());
  QueueApp::SetSoloReady(false);
  EXPECT_TRUE(Search::WaitingLine().empty());
  QueueApp::SetSoloReady(true);
  EXPECT_EQ(Search::WaitingLine(), "Waiting for another player… 0:00");
  QueueApp::End();
  EXPECT_TRUE(Search::WaitingLine().empty());
}

TEST_F(OrcaUXSearchWait, ReturningReadyFromARoomStartsANewWait)
{
  QueueApp::SetOnSoloCss(true);
  QueueApp::SetSoloReady(true);
  Search::SetWaiting(true, m_start);
  QueueApp::SetOnSoloCss(true);
  EXPECT_EQ(Search::WaitingLine(m_start + 36s), "Waiting for another player… 0:36");
  Search::Matched(false);
  QueueApp::SetOnSoloCss(false);
  EXPECT_TRUE(Search::WaitingLine().empty());
  QueueApp::SetOnSoloCss(true);
  QueueApp::SetSoloReady(true);
  EXPECT_EQ(Search::WaitingLine(), "Waiting for another player… 0:00");
}
}  // namespace
