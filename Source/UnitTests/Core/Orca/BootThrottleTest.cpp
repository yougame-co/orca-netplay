// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "Core/Core.h"
#include "Core/Orca/BootThrottle.h"
#include "Core/Orca/Profile.h"

namespace
{
void Session(const std::string& value)
{
#ifdef _WIN32
  _putenv_s("ORCA_SESSION", value.c_str());
#else
  setenv("ORCA_SESSION", value.c_str(), 1);
#endif
}

class OrcaBootThrottle : public testing::Test
{
protected:
  void SetUp() override
  {
    m_session = Orca::GetEnv("ORCA_SESSION");
    m_disabled = Core::GetIsThrottlerTempDisabled();
    Core::SetIsThrottlerTempDisabled(false);
    Session("1");
  }
  void TearDown() override
  {
    Orca::BootThrottle::Finish(false);
    Core::SetIsThrottlerTempDisabled(m_disabled);
    Session(m_session);
  }
  std::string m_session;
  bool m_disabled = false;
};
}

TEST_F(OrcaBootThrottle, RestoresPacingAtTheFirstFrameOnly)
{
  Orca::BootThrottle::Begin();
  EXPECT_TRUE(Core::GetIsThrottlerTempDisabled());
  Orca::BootThrottle::Finish(false);
  EXPECT_FALSE(Core::GetIsThrottlerTempDisabled());
  Core::SetIsThrottlerTempDisabled(true);
  Orca::BootThrottle::Finish(false);
  EXPECT_TRUE(Core::GetIsThrottlerTempDisabled());
}

TEST_F(OrcaBootThrottle, LeavesMatchAndHarnessPacingAlone)
{
  for (const bool disabled : {false, true})
  {
    Core::SetIsThrottlerTempDisabled(false);
    Orca::BootThrottle::Begin();
    Core::SetIsThrottlerTempDisabled(disabled);
    Orca::BootThrottle::Finish(true);
    EXPECT_EQ(Core::GetIsThrottlerTempDisabled(), disabled);
  }
}

TEST_F(OrcaBootThrottle, LeavesAnExistingOwnerAlone)
{
  Core::SetIsThrottlerTempDisabled(true);
  Orca::BootThrottle::Begin();
  Orca::BootThrottle::Finish(false);
  EXPECT_TRUE(Core::GetIsThrottlerTempDisabled());
}

TEST_F(OrcaBootThrottle, DoesNotChangeNonSessionBoots)
{
  Session("0");
  Orca::BootThrottle::Begin();
  EXPECT_FALSE(Core::GetIsThrottlerTempDisabled());
  Orca::BootThrottle::Finish(false);
  EXPECT_FALSE(Core::GetIsThrottlerTempDisabled());
}
