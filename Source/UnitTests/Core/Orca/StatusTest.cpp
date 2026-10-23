// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "Common/ScopeGuard.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Rollback/OnlineMatch.h"

namespace
{
void SetEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}
}  // namespace

TEST(OrcaStatus, ReportsRemainSurvivableAfterOnlineWithdrawal)
{
  const auto old_session = Orca::GetEnv("ORCA_SESSION");
  SetEnv("ORCA_SESSION", "1");
  auto* out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  Orca::Status::SetOutput(out);
  Orca::Status::ResetOnline();
  Orca::Status::SetAppCaps(Orca::Status::CAPS);
  EXPECT_TRUE(Orca::Status::Cap("join"));
  Orca::Status::Report("no_origin", "No start point");
  Orca::Status::WithdrawOnline();
  EXPECT_FALSE(Orca::Status::Cap("join"));
  EXPECT_FALSE(Orca::Status::Cap("host"));
  EXPECT_FALSE(Orca::Status::Cap("direct"));
  for (const char* code : {"network", "peer_left", "room_mismatch"})
    Orca::Status::Report(code, "Still playing solo");
  EXPECT_EQ(Orca::Status::ExitCode(), 0);
  std::rewind(out);
  char buffer[512];
  std::string lines;
  while (std::fgets(buffer, sizeof(buffer), out))
    lines += buffer;
  EXPECT_EQ(lines, "orca error no_origin No start point\n"
                   "orca error network Still playing solo\n"
                   "orca error peer_left Still playing solo\n"
                   "orca error room_mismatch Still playing solo\n");
  // Withdrawal keeps every other base capability, including future additions, in order.
  std::istringstream base{Orca::Status::CAPS};
  std::string expected;
  for (std::string cap; base >> cap;)
  {
    if (cap != "join" && cap != "host" && cap != "direct")
      expected += (expected.empty() ? "" : " ") + cap;
  }
  EXPECT_TRUE(Orca::Status::OfferedCaps().starts_with(expected + " "));
  Orca::Status::SetOutput(nullptr);
  std::fclose(out);
  Orca::Status::SetAppCaps("");
  Orca::Status::ResetOnline();
  SetEnv("ORCA_SESSION", old_session);
}

TEST(OrcaStatus, LegacyMenuOriginFailureKeepsItsExistingLogOnly)
{
  EXPECT_TRUE(Rollback::OnlineMatch::OriginFailureReport(nullptr).empty());
  Orca::Mod::Manifest pack;
  EXPECT_EQ(Rollback::OnlineMatch::OriginFailureReport(&pack),
            "This pack's game didn't reach the start point its pack names, so friends can't join.");
}

TEST(OrcaStatus, LegacyRebootReagreesOnlyTheAppsListedCapabilities)
{
  const auto old_session = Orca::GetEnv("ORCA_SESSION");
  SetEnv("ORCA_SESSION", "1");
  Orca::SetActiveProfile(std::nullopt);
  Orca::Status::SetAppCaps("join host stats unknown");
  Orca::Status::WithdrawOnline();
  EXPECT_FALSE(Orca::Status::Cap("join"));
  EXPECT_FALSE(Orca::Status::Cap("host"));
  EXPECT_TRUE(Orca::Status::Cap("stats"));
  // SetActiveProfile is the in-process boot path and calls ResetOnline after resetting guards.
  Orca::SetActiveProfile(Orca::Profile{});
  EXPECT_TRUE(Orca::Status::Cap("join"));
  EXPECT_TRUE(Orca::Status::Cap("host"));
  EXPECT_TRUE(Orca::Status::Cap("stats"));
  EXPECT_FALSE(Orca::Status::Cap("direct"));
  EXPECT_FALSE(Orca::Status::Cap("unknown"));
  Orca::Status::WithdrawOnline();
  Orca::SetActiveProfile(Orca::Profile{});
  EXPECT_TRUE(Orca::Status::Cap("join"));
  Orca::Status::SetAppCaps("");
  Orca::SetActiveProfile(std::nullopt);
  EXPECT_FALSE(Orca::Status::Cap("join"));
  SetEnv("ORCA_SESSION", old_session);
}

TEST(OrcaStatus, AppSetResultsMarkHarnessInputAndPreserveNormalBytes)
{
  const std::string old_session = Orca::GetEnv("ORCA_SESSION");
  const std::string old_input = Orca::GetEnv("YG_INPUT");
  const std::string old_commands = Orca::GetEnv("ORCA_TEST_COMMANDS");
  const auto old_profile = Orca::ActiveProfile() ? std::optional{*Orca::ActiveProfile()} : std::nullopt;
  Common::ScopeGuard restore([&] {
    Orca::Status::SetOutput(nullptr);
    Orca::Status::SetAppCaps("");
    Orca::SetActiveProfile(old_profile);
    SetEnv("ORCA_SESSION", old_session);
    SetEnv("YG_INPUT", old_input);
    SetEnv("ORCA_TEST_COMMANDS", old_commands);
  });
  SetEnv("ORCA_SESSION", "1");
  Orca::Profile profile;
  profile.game_id = "RSBE01";
  profile.revision = 2;
  Orca::SetActiveProfile(profile);
  Orca::Status::SetAppCaps("results");
  ASSERT_TRUE(Orca::Status::Cap("results"));
  for (int mode = 0; mode < 3; ++mode)
  {
    SetEnv("YG_INPUT", mode == 1 ? "fixture-pads.txt" : "");
    SetEnv("ORCA_TEST_COMMANDS", mode == 2 ? "1" : "");
    std::FILE* output = std::tmpfile();
    ASSERT_NE(output, nullptr);
    Orca::Status::SetOutput(output);
    Orca::Status::Result(R"({"q":"ranked","k":"set","out":"win"})");
    std::rewind(output);
    char buffer[512]{};
    ASSERT_NE(std::fgets(buffer, sizeof(buffer), output), nullptr);
    EXPECT_EQ(std::string(buffer), mode == 0 ?
        "orca result {\"q\":\"ranked\",\"k\":\"set\",\"out\":\"win\"}\n" :
        "orca result {\"q\":\"ranked\",\"k\":\"set\",\"out\":\"win\",\"harness\":true}\n");
    Orca::Status::SetOutput(nullptr);
    std::fclose(output);
  }
}
