// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "Core/Orca/Bridge.h"
#include "Common/ScopeGuard.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/LocalInput.h"
#include "UICommon/UICommon.h"

namespace
{
class ScopedEnv
{
public:
  ScopedEnv(const char* name, const char* value) : m_name(name)
  {
    if (const char* old = std::getenv(name))
      m_old = old;
    Set(value);
  }
  ~ScopedEnv() { Set(m_old ? m_old->c_str() : nullptr); }

private:
  void Set(const char* value)
  {
#ifdef _WIN32
    _putenv_s(m_name, value ? value : "");
#else
    if (value)
      setenv(m_name, value, 1);
    else
      unsetenv(m_name);
#endif
  }
  const char* m_name;
  std::optional<std::string> m_old;
};

class OrcaVirtualPadOnly : public testing::Test
{
protected:
  void TearDown() override
  {
    Orca::UX::StopControllerStream();
    ControlReference::SetInputGate(m_gate);
  }

  void SendVirtualPad()
  {
    Orca::UX::StartControllerStreamFromStdio();
    std::string buttons;
    for (int i = 0; i < 17; ++i)
    {
      if (i)
        buttons += ',';
      buttons += i == 0 ? R"({"value":1,"pressed":true})" :
                          R"({"value":0,"pressed":false})";
    }
    // The desktop page's virtual pad arrives as a standard pad in the app's snapshot.
    const std::string line =
        R"(yg {"push":{"type":"controllers","snapshot":{"schema":1,"session":"virtual-pad-test",)"
        R"("sequence":1,"sentAt":1000,"receivedAt":1000,"owned":false,"suspended":false,)"
        R"("ports":[],"pads":[{"index":0,"mapping":"standard","axes":[1,0,0,0],"buttons":[)" +
        buttons + "]}]}}}";
    ASSERT_TRUE(Orca::Bridge::Route({line}).empty());
    ASSERT_TRUE(Orca::UX::LatestPad().has_value());
    ASSERT_EQ(Orca::UX::LatestPad()->button, PAD_BUTTON_A);
  }

  const bool m_gate = ControlReference::GetInputGate();
};
}  // namespace

TEST_F(OrcaVirtualPadOnly, ExactOneDisablesAllNativeInputBackends)
{
  const ScopedEnv native("ORCA_NO_LOCAL_INPUT", nullptr);
  for (const char* value : {static_cast<const char*>(nullptr), "", "0", "true", "01", "1 "})
  {
    const ScopedEnv mode("ORCA_VIRTUAL_PAD_ONLY", value);
    EXPECT_FALSE(InputCommon::VirtualPadOnly());
    EXPECT_FALSE(InputCommon::LocalInputDisabled());
  }
  const ScopedEnv mode("ORCA_VIRTUAL_PAD_ONLY", "1");
  EXPECT_TRUE(InputCommon::VirtualPadOnly());
  EXPECT_TRUE(InputCommon::LocalInputDisabled());
  ControllerInterface controllers;
  controllers.Initialize({});
  controllers.RefreshDevices();
  EXPECT_EQ(controllers.GetInputBackendCount(), 0u);
  EXPECT_TRUE(controllers.GetAllDevices().empty());
  controllers.Shutdown();
  ASSERT_FALSE(GCAdapter::IsInitialized());
  GCAdapter::Init();
  EXPECT_FALSE(GCAdapter::IsInitialized());
  GCAdapter::Shutdown();
}

TEST_F(OrcaVirtualPadOnly, PagePadSurvivesFocusLossOnlyWhenEnabled)
{
  const ScopedEnv stdio("YOUGAME_STDIO", "1");
  SendVirtualPad();
  for (const char* value : {static_cast<const char*>(nullptr), "0", "1", "true"})
  {
    const ScopedEnv mode("ORCA_VIRTUAL_PAD_ONLY", value);
    const bool enabled = value && std::string(value) == "1";
    ControlReference::SetInputGate(false);
    const auto unfocused = Orca::UX::LocalPad(true);
    ASSERT_TRUE(unfocused.has_value());
    EXPECT_EQ(unfocused->button, enabled ? PAD_BUTTON_A : 0);
    EXPECT_EQ(unfocused->stickX, enabled ? 255 : 128);
    ControlReference::SetInputGate(true);
    ASSERT_TRUE(Orca::UX::LocalPad(true).has_value());
    EXPECT_EQ(Orca::UX::LocalPad(true)->button, PAD_BUTTON_A);
  }
}

TEST_F(OrcaVirtualPadOnly, ReportsModeOnceAndIsSilentWhenUnset)
{
  testing::internal::CaptureStdout();
  {
    const ScopedEnv mode("ORCA_VIRTUAL_PAD_ONLY", nullptr);
    UICommon::ReportInputMode();
  }
  EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
  testing::internal::CaptureStdout();
  {
    const ScopedEnv mode("ORCA_VIRTUAL_PAD_ONLY", "1");
    UICommon::ReportInputMode();
    UICommon::ReportInputMode();
  }
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "orca input virtual-pad-only\n");
}

TEST_F(OrcaVirtualPadOnly, ExplicitOrdinalsSurviveVirtualPadIndexShuffling)
{
  const ScopedEnv mode("ORCA_VIRTUAL_PAD_ONLY", "1");
  const ScopedEnv stdio("YOUGAME_STDIO", "1");
  Orca::UX::SetLocalPadCount(2);
  const Common::ScopeGuard count([] { Orca::UX::SetLocalPadCount(1); });
  Orca::UX::StartControllerStreamFromStdio();
  ControlReference::SetInputGate(false);
  for (int sequence = 1; sequence <= 3; ++sequence)
  {
    std::string pads;
    for (int ordinal = 0; ordinal < 2; ++ordinal)
    {
      if (ordinal)
        pads += ',';
      const int index = sequence == 1 ? ordinal : 1 - ordinal;
      pads += "{\"index\":" + std::to_string(index) + ",\"seat\":" +
          std::to_string(sequence == 3 ? 3 : ordinal) +
          R"(,"mapping":"standard","axes":[)" + (ordinal ? "-1" : "1") + ",0,0,0],\"buttons\":[";
      for (int button = 0; button < 17; ++button)
      {
        if (button)
          pads += ',';
        pads += button == 0 ? R"({"value":1,"pressed":true})" :
                              R"({"value":0,"pressed":false})";
      }
      pads += "]}";
    }
    const std::string line =
        R"(yg {"push":{"type":"controllers","snapshot":{"schema":1,"session":"co-virtual",)"
        R"("sequence":)" + std::to_string(sequence) +
        R"(,"sentAt":1000,"receivedAt":1000,"owned":false,"suspended":false,"ports":[],"pads":[)" +
        pads + "]}}}";
    ASSERT_TRUE(Orca::Bridge::Route({line}).empty());
    for (int ordinal = 0; ordinal < 2; ++ordinal)
    {
      const auto pad = Orca::UX::LocalPad(ordinal, true);
      ASSERT_TRUE(pad);
      EXPECT_TRUE(pad->isConnected);
      EXPECT_EQ(pad->stickX, sequence == 3 ? 128 : (ordinal ? 1 : 255));
      EXPECT_EQ(pad->button, sequence == 3 ? 0 : PAD_BUTTON_A);
    }
    if (sequence != 3)
    {
      ControlReference::SetInputGate(true);
      EXPECT_EQ(Orca::UX::LocalPad(1, true)->stickX, 1);
      ControlReference::SetInputGate(false);
    }
  }
}
