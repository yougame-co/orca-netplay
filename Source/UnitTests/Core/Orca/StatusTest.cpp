// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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

TEST(OrcaStatus, ReportsBrandOnlyTheSentence)
{
  const auto old_session = Orca::GetEnv("ORCA_SESSION");
  Common::ScopeGuard restore([&] {
    Orca::Status::SetOutput(nullptr);
    Orca::Status::SetAppCaps("");
    SetEnv("ORCA_SESSION", old_session);
  });
  SetEnv("ORCA_SESSION", "1");
  Orca::Status::SetAppCaps("join");
  std::FILE* out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  Common::ScopeGuard close([&] { std::fclose(out); });
  Orca::Status::SetOutput(out);
  Orca::Status::Report("orca_code", "Restart Orca from /Applications/Orca.app.\nUpdate YG Baiji.");
  std::rewind(out);
  char buffer[256]{};
  ASSERT_NE(std::fgets(buffer, sizeof(buffer), out), nullptr);
  EXPECT_EQ(std::string(buffer), "orca error orca_code Restart YG Dolphin from "
                                "/Applications/Orca.app. Update YG Dolphin.\n");
}

TEST(OrcaStatus, CoSeatsCapsAreAppendOnlyAndFitLegacyReaders)
{
  const auto old_session = Orca::GetEnv("ORCA_SESSION");
  const auto old_source = Orca::GetEnv("ORCA_REPLAY_SOURCE");
  const auto old_switch = Orca::GetEnv("ORCA_COSEATS");
  const auto old_profile = Orca::ActiveProfile() ? std::optional{*Orca::ActiveProfile()} : std::nullopt;
  Common::ScopeGuard restore([&] {
    SetEnv("ORCA_SESSION", old_session);
    SetEnv("ORCA_REPLAY_SOURCE", old_source);
    SetEnv("ORCA_COSEATS", old_switch);
    Orca::SetActiveProfile(old_profile);
    Orca::Status::SetAppCaps("");
  });
  SetEnv("ORCA_SESSION", "1");
  const auto sys = (std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/").string();
  for (const char* game : {"RSBE01", "PPLUS32", "PPLUS321"})
  {
    std::string error;
    const auto profile = Orca::LoadProfileFrom(sys, game, std::nullopt, &error);
    ASSERT_TRUE(profile) << error;
    Orca::SetActiveProfile(profile);
    for (const bool product : {false, true})
    {
      SCOPED_TRACE(std::string(game) + (product ? " product" : " session"));
      SetEnv("ORCA_REPLAY_SOURCE", product ? "fixture-replay.orca" : "");
      std::string before = product ?
          "leave stats pause delay perf chat yougame gfx" :
          "join leave stats pause delay perf direct host chat yougame gfx";
      before += " music results locks queue2 pick-casual pick-ranked talk rumble";
      if (product)
      {
        // Core always includes BroadcastSink.cpp; its private define is not a test define.
        before += " replay broadcast";
      }
      before += " record hostui";
      SetEnv("ORCA_COSEATS", "0");
      EXPECT_EQ(Orca::Status::OfferedCaps(), before + " qdoubles rdoubles");
      SetEnv("ORCA_COSEATS", "1");
      const auto after = Orca::Status::OfferedCaps();
      EXPECT_EQ(after, before + " coseats qdoubles rdoubles");
      EXPECT_EQ(after.find("pick-doubles"), std::string::npos);
      std::FILE* output = std::tmpfile();
      ASSERT_NE(output, nullptr);
      Orca::Status::SetOutput(output);
      Orca::Status::PrintCaps();
      std::rewind(output);
      char buffer[1024]{};
      ASSERT_NE(std::fgets(buffer, sizeof(buffer), output), nullptr);
      EXPECT_EQ(std::string(buffer), "orca caps " + before + " coseats qdoubles rdoubles\n");
      Orca::Status::SetOutput(nullptr);
      std::fclose(output);
      std::istringstream words(after);
      unsigned count = 0;
      for (std::string word; words >> word;)
        ++count;
      const unsigned expected = product ? 23 : 24;
      EXPECT_EQ(count, expected);
      EXPECT_LE(count, 24u);
      Orca::Status::SetAppCaps("join");
      EXPECT_FALSE(Orca::Status::Cap("coseats"));
      EXPECT_FALSE(Orca::Status::Cap("qdoubles"));
      EXPECT_FALSE(Orca::Status::Cap("rdoubles"));
      Orca::Status::SetAppCaps("join coseats qdoubles rdoubles pick-doubles");
      EXPECT_TRUE(Orca::Status::Cap("coseats"));
      EXPECT_TRUE(Orca::Status::Cap("qdoubles"));
      EXPECT_TRUE(Orca::Status::Cap("rdoubles"));
      EXPECT_FALSE(Orca::Status::Cap("pick-doubles"));
      SetEnv("ORCA_COSEATS", "0");
      Orca::Status::SetAppCaps("join coseats qdoubles rdoubles pick-doubles");
      EXPECT_FALSE(Orca::Status::Cap("coseats"));
      EXPECT_TRUE(Orca::Status::Cap("qdoubles"));
      EXPECT_TRUE(Orca::Status::Cap("rdoubles"));
      EXPECT_FALSE(Orca::Status::Cap("pick-doubles"));
    }
  }
}

TEST(OrcaStatusDeathTest, ErrorsAndLegacyReportsBrandTheOutputAndListener)
{
  // Error's first-failure state lasts for the process, so exercise each route in a fresh child.
  for (bool report : {false, true})
  {
    ASSERT_EXIT(
        {
          SetEnv("ORCA_SESSION", "1");
          Orca::Status::SetAppCaps("");
          std::FILE* out = std::tmpfile();
          if (!out)
            std::_Exit(2);
          Orca::Status::SetOutput(out);
          Orca::Status::SetErrorListener([](std::string_view code, std::string_view sentence) {
            if (code != "orca_code" || sentence != "YG Dolphin needs /Applications/Orca.app")
              std::_Exit(3);
          });
          if (report)
            Orca::Status::Report("orca_code", "Orca needs /Applications/Orca.app");
          else
            Orca::Status::Error("orca_code", "Orca needs /Applications/Orca.app");
          std::rewind(out);
          char buffer[256]{};
          const bool ok = std::fgets(buffer, sizeof(buffer), out) &&
              std::string(buffer) == "orca error orca_code YG Dolphin needs /Applications/Orca.app\n" &&
              Orca::Status::ExitCode() == 1;
          std::fclose(out);
          std::_Exit(ok ? 0 : 4);
        },
        ::testing::ExitedWithCode(0), "");
  }
}

#if defined(__APPLE__) || defined(__linux__)
TEST(OrcaStatus, CapabilityReadsContinueWhileStatusOutputIsBlocked)
{
  // Stall inside the stream's write callback: the output thread definitely holds the status
  // output lock, without relying on pipe capacity, scheduler ordering, or a real app.
  struct Output
  {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool released = false;
    void Write()
    {
      std::unique_lock lock(mutex);
      entered = true;
      wake.notify_all();
      wake.wait(lock, [&] { return released; });
    }
  } output;
#ifdef __APPLE__
  auto* stream = funopen(&output, nullptr,
      +[](void* cookie, const char*, int size) {
        static_cast<Output*>(cookie)->Write();
        return size;
      }, nullptr, nullptr);
#else
  cookie_io_functions_t io{};
  io.write = +[](void* cookie, const char*, size_t size) -> ssize_t {
    static_cast<Output*>(cookie)->Write();
    return static_cast<ssize_t>(size);
  };
  auto* stream = fopencookie(&output, "w", io);
#endif
  ASSERT_NE(stream, nullptr);
  std::setvbuf(stream, nullptr, _IONBF, 0);
  const auto session = Orca::GetEnv("ORCA_SESSION");
  SetEnv("ORCA_SESSION", "1");
  Orca::Status::ResetOnline();
  Orca::Status::SetAppCaps("direct");
  Orca::Status::SetOutput(stream);
  std::thread writer([] { Orca::Status::Shaders(1, 2); });
  const Common::ScopeGuard restore([&] {
    {
      std::lock_guard lock(output.mutex);
      output.released = true;
    }
    output.wake.notify_all();
    writer.join();
    Orca::Status::SetOutput(nullptr);
    std::fclose(stream);
    Orca::Status::SetAppCaps("");
    SetEnv("ORCA_SESSION", session);
  });
  {
    std::unique_lock lock(output.mutex);
    ASSERT_TRUE(output.wake.wait_for(lock, std::chrono::seconds(5), [&] { return output.entered; }));
  }
  auto capability = std::async(std::launch::async, [] { return Orca::Status::Cap("direct"); });
  const auto ready = capability.wait_for(std::chrono::milliseconds(200));
  // Release before inspecting the assertion/future, including on the old implementation.
  {
    std::lock_guard lock(output.mutex);
    output.released = true;
  }
  output.wake.notify_all();
  EXPECT_EQ(ready, std::future_status::ready);
  EXPECT_TRUE(capability.get());
}
#endif
