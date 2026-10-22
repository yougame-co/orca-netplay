// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"

namespace
{
std::string s_app_line;
std::string s_alert_text;
bool s_answer;
int s_alert_calls;

bool FakeAlert(const char*, const char* text, bool, Common::MsgType)
{
  s_alert_text = text;
  ++s_alert_calls;
  return s_answer;  // Answer yes for ASSERT tests, so the real Crash() is never called.
}

void CapturePanic(const char* text)
{
  s_app_line = text;
}

class CaptureLog final : public Common::Log::LogListener
{
public:
  void Log(Common::Log::LogLevel level, const char* text) override
  {
    levels.push_back(level);
    lines += text;
  }

  std::vector<Common::Log::LogLevel> levels;
  std::string lines;
};

class MemoryLayer final : public Config::ConfigLayerLoader
{
public:
  MemoryLayer() : ConfigLayerLoader(Config::LayerType::Base) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

class MsgHandlerPanicLogging : public testing::Test
{
protected:
  void SetUp() override
  {
    m_root = File::CreateTempDir();
    ASSERT_FALSE(m_root.empty());
    m_old_user = File::GetUserPath(D_USER_IDX);
    File::SetUserPath(D_USER_IDX, m_root + "/");
    Config::Init();
    Config::AddLayer(std::make_unique<MemoryLayer>());
    Common::Log::LogManager::Init();
    auto* logs = Common::Log::LogManager::GetInstance();
    auto capture = std::make_unique<CaptureLog>();
    m_log = capture.get();
    logs->RegisterListener(Common::Log::LogListener::FILE_LISTENER, std::move(capture));
    logs->EnableListener(Common::Log::LogListener::FILE_LISTENER, true);
    logs->EnableListener(Common::Log::LogListener::CONSOLE_LISTENER, false);
    logs->EnableListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, false);
    // Leave categories disabled and verbosity at NOTICE: panic diagnostics must still be logged.
    m_old_handler = Common::RegisterMsgAlertHandler(FakeAlert);
    m_old_panic_handler = Common::RegisterPanicAlertLogHandler(CapturePanic);
    s_app_line.clear();
    s_alert_text.clear();
    s_answer = true;
    s_alert_calls = 0;
  }

  void TearDown() override
  {
    Common::RegisterPanicAlertLogHandler(m_old_panic_handler);
    Common::RegisterMsgAlertHandler(m_old_handler);
    Common::SetEnableAlert(true);
    Common::Log::LogManager::Shutdown();
    Config::Shutdown();
    File::SetUserPath(D_USER_IDX, m_old_user);
    File::DeleteDirRecursively(m_root);
  }

  void ExpectSingleLine() const
  {
    EXPECT_TRUE(s_app_line.starts_with("orca error assert "));
    EXPECT_EQ(s_app_line.find_first_of("\r\n"), std::string::npos);
    ASSERT_EQ(m_log->levels.size(), size_t{1});
    EXPECT_EQ(m_log->levels.front(), Common::Log::LogLevel::LERROR);
    EXPECT_NE(m_log->lines.find(s_app_line + "\n"), std::string::npos);
    EXPECT_EQ(std::count(m_log->lines.begin(), m_log->lines.end(), '\n'), 1);
    EXPECT_EQ(m_log->lines.find('\r'), std::string::npos);
  }

  CaptureLog* m_log = nullptr;

private:
  std::string m_root;
  std::string m_old_user;
  Common::MsgAlertHandler m_old_handler = nullptr;
  Common::PanicAlertLogHandler m_old_panic_handler = nullptr;
};

TEST_F(MsgHandlerPanicLogging, AssertKeepsTheCheckAndCallsite)
{
  const bool check_passed = false;
  const int alert_line = __LINE__ + 1;
  ASSERT(check_passed);

  ExpectSingleLine();
  EXPECT_NE(s_app_line.find("Condition: check_passed"), std::string::npos);
  EXPECT_NE(s_app_line.find("File: UnitTests/Common/MsgHandlerTest.cpp"), std::string::npos);
  EXPECT_NE(s_app_line.find(fmt::format("Line: {}", alert_line)), std::string::npos);
  EXPECT_NE(s_app_line.find("Function: TestBody"), std::string::npos);
  EXPECT_NE(s_app_line.find("Ignore and continue?"), std::string::npos);
  EXPECT_EQ(s_app_line.find(__FILE__), std::string::npos);
  EXPECT_EQ(s_alert_calls, 1);
  EXPECT_NE(s_alert_text.find('\n'), std::string::npos);  // The normal handler is unchanged.
}

TEST_F(MsgHandlerPanicLogging, AssertMessageIsNotTruncated)
{
  const bool check_passed = false;
  const std::string detail(1024, 'x');
  ASSERT_MSG(COMMON, check_passed, "Before\n{}\nafter", detail);

  ExpectSingleLine();
  EXPECT_NE(s_app_line.find("Before | " + detail + " | after"), std::string::npos);
  EXPECT_NE(s_app_line.find("Condition: check_passed"), std::string::npos);
  EXPECT_NE(s_app_line.find("Function: TestBody"), std::string::npos);
}

TEST_F(MsgHandlerPanicLogging, WindowsPathsAndCrLfAreNormalized)
{
  const char* file = "C:\\builder\\checkout\\Source\\Core\\VideoCommon\\Check.cpp";
  EXPECT_TRUE(Common::MsgAlertFmt<1>(
      true, Common::MsgType::Warning, Common::Log::LogType::VIDEO, file, 73,
      FMT_STRING("An error occurred.\r\n\r\nBad state\r\n  Condition: ready\r\n  File: {}\r\n"
                 "  Line: 73\r\n  Function: EmitPacket\r\n\r\nIgnore and continue?"),
      file));

  ExpectSingleLine();
  EXPECT_NE(s_app_line.find("at Core/VideoCommon/Check.cpp:73"), std::string::npos);
  EXPECT_NE(s_app_line.find("File: Core/VideoCommon/Check.cpp"), std::string::npos);
  EXPECT_NE(s_app_line.find("Condition: ready"), std::string::npos);
  EXPECT_NE(s_app_line.find("Function: EmitPacket"), std::string::npos);
  EXPECT_EQ(m_log->lines.find("builder"), std::string::npos);
  EXPECT_EQ(m_log->lines.find("checkout"), std::string::npos);
}

TEST_F(MsgHandlerPanicLogging, PlainPanicsUseTheSamePathAndKeepTheirAnswer)
{
  s_answer = false;
  EXPECT_FALSE(PanicAlertFmt("First\nsecond\rthird\r\nfourth"));
  ExpectSingleLine();
  EXPECT_NE(s_app_line.find("First | second | third | fourth"), std::string::npos);
  EXPECT_EQ(s_alert_text, "First\nsecond\rthird\r\nfourth");

  m_log->levels.clear();
  m_log->lines.clear();
  EXPECT_FALSE(PanicYesNoFmt("Question\nwith detail"));
  ExpectSingleLine();
  EXPECT_NE(s_app_line.find("Question | with detail"), std::string::npos);
  EXPECT_EQ(s_alert_calls, 2);
}

TEST_F(MsgHandlerPanicLogging, DisabledAlertsStillLogWithoutChangingTheAnswer)
{
  Common::SetEnableAlert(false);
  EXPECT_TRUE(PanicYesNoFmt("Hidden\ncheck"));
  ExpectSingleLine();
  EXPECT_EQ(s_alert_calls, 0);
}

TEST_F(MsgHandlerPanicLogging, OrdinaryMessagesDoNotBecomePanicDiagnostics)
{
  EXPECT_TRUE(SuccessAlertFmt("Information\non two lines"));
  EXPECT_TRUE(AskYesNoFmt("Ordinary\nquestion"));
  EXPECT_TRUE(s_app_line.empty());
  EXPECT_TRUE(m_log->lines.empty());
  EXPECT_EQ(s_alert_text, "Ordinary\nquestion");
  EXPECT_EQ(s_alert_calls, 2);
}

TEST_F(MsgHandlerPanicLogging, StandaloneHandlerAndLogRemainMultiline)
{
  Common::RegisterPanicAlertLogHandler(nullptr);
  auto* logs = Common::Log::LogManager::GetInstance();
  logs->SetEnable(Common::Log::LogType::MASTER_LOG, true);
  logs->SetConfigLogLevel(Common::Log::LogLevel::LWARNING);
  EXPECT_TRUE(PanicYesNoFmt("Standalone\nquestion"));

  EXPECT_TRUE(s_app_line.empty());
  EXPECT_EQ(s_alert_text, "Standalone\nquestion");
  ASSERT_EQ(m_log->levels.size(), size_t{1});
  EXPECT_EQ(m_log->levels.front(), Common::Log::LogLevel::LWARNING);
  EXPECT_NE(m_log->lines.find("Warning: Standalone\nquestion"), std::string::npos);
}
}  // namespace
