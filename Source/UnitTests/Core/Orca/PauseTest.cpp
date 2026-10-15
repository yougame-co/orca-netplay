// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The player's pause in a session (Rollback::OnlineMatch::SoloPauseAllowed, ORCA.md "Embedding"):
// what may pause, what ends a pause, and the time paused that the stats line leaves out.

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Rollback/OnlineMatch.h"

namespace
{
void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

// A session's environment for one test, put back as it was afterwards.
class SessionEnv
{
public:
  explicit SessionEnv(const char* dev_game)
  {
    Save("ORCA_SESSION", &m_session);
    Save("ORCA_TEST_DEV_GAME", &m_dev_game);
    Save("YOUGAME_BRIDGE", &m_bridge);
    SetEnv("ORCA_SESSION", "1");
    SetEnv("ORCA_TEST_DEV_GAME", dev_game);
    SetEnv("YOUGAME_BRIDGE", nullptr);
  }
  ~SessionEnv()
  {
    SetEnv("ORCA_SESSION", m_session ? m_session->c_str() : nullptr);
    SetEnv("ORCA_TEST_DEV_GAME", m_dev_game ? m_dev_game->c_str() : nullptr);
    SetEnv("YOUGAME_BRIDGE", m_bridge ? m_bridge->c_str() : nullptr);
  }
  SessionEnv(const SessionEnv&) = delete;
  SessionEnv& operator=(const SessionEnv&) = delete;

private:
  static void Save(const char* name, std::optional<std::string>* out)
  {
    if (const char* value = std::getenv(name))
      *out = value;
  }
  std::optional<std::string> m_session, m_dev_game, m_bridge;
};

// Nothing left over from another test: no drop-in request, no pause, no paused time.
void Clear()
{
  Orca::Online::TakePrepareJoin();
  Orca::Online::TakeJoinRequest();
  Orca::Online::TakeLeaveRequest();
  Rollback::OnlineMatch::EndPause();
  Rollback::OnlineMatch::TakePausedTime();
}
}  // namespace

TEST(OrcaPause, SessionWithoutARoomMayPause)
{
  // A session with no room (the offline harness): a pause stops nobody else's game.
  SessionEnv env(nullptr);
  Clear();
  ASSERT_FALSE(Orca::Online::Enabled());
  EXPECT_TRUE(Rollback::OnlineMatch::SoloPauseAllowed());
  EXPECT_FALSE(Rollback::OnlineMatch::Pausing());
  EXPECT_TRUE(Rollback::OnlineMatch::BeginPause());
  EXPECT_TRUE(Rollback::OnlineMatch::Pausing());
  Rollback::OnlineMatch::EndPause();
  EXPECT_FALSE(Rollback::OnlineMatch::Pausing());
}

TEST(OrcaPause, OnlineSessionWaitsForItsFirstBoundary)
{
  // Online, nothing is known before the first frame boundary (a host or a joiner, a friend in the
  // room or not): no pause.
  SessionEnv env("orca-pause-test");
  Clear();
  ASSERT_TRUE(Orca::Online::Enabled());
  EXPECT_FALSE(Rollback::OnlineMatch::SoloPauseAllowed());
  EXPECT_FALSE(Rollback::OnlineMatch::BeginPause());
  EXPECT_FALSE(Rollback::OnlineMatch::Pausing());
  // A refused pause counts no paused time.
  Rollback::OnlineMatch::EndPause();
  EXPECT_EQ(Rollback::OnlineMatch::TakePausedTime().count(), 0);
}

TEST(OrcaPause, DropInRequestsWaitForTheNextBoundary)
{
  // A pending request needs a frame boundary, so it ends a solo pause until it is taken. Only
  // requests a boundary can act on count: join or leave once the app's caps allow them, and an
  // invite only once a room is up.
  SessionEnv env(nullptr);
  Clear();
  Orca::Status::SetAppCaps("");
  EXPECT_FALSE(Orca::Online::DropInPending());

  Orca::Online::RequestJoin("abcdef");
  Orca::Online::RequestLeave();
  EXPECT_FALSE(Orca::Online::DropInPending()) << "no caps yet: the boundary holds them";
  Orca::Status::SetAppCaps("join leave stats");
  EXPECT_TRUE(Orca::Online::DropInPending());
  EXPECT_EQ(Orca::Online::TakeJoinRequest(), "abcdef");
  EXPECT_TRUE(Orca::Online::DropInPending());
  EXPECT_TRUE(Orca::Online::TakeLeaveRequest());
  EXPECT_FALSE(Orca::Online::DropInPending());

  // No room to make a keyframe for: an invite waits for one.
  Orca::Online::PrepareJoin();
  EXPECT_FALSE(Orca::Online::DropInPending());
  EXPECT_TRUE(Orca::Online::TakePrepareJoin());
  Orca::Status::SetAppCaps("");
}

TEST(OrcaPause, PausedTimeIsLeftOutOfTheStats)
{
  SessionEnv env(nullptr);
  Clear();
  ASSERT_TRUE(Rollback::OnlineMatch::BeginPause());
  // A second pause while paused (the app's "pause" twice) doesn't restart the clock.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  ASSERT_TRUE(Rollback::OnlineMatch::BeginPause());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Rollback::OnlineMatch::EndPause();
  const auto paused = Rollback::OnlineMatch::TakePausedTime();
  EXPECT_GE(paused, std::chrono::milliseconds(60));
  EXPECT_LT(paused, std::chrono::seconds(5));
  // Taken once; a resume with no pause adds nothing.
  EXPECT_EQ(Rollback::OnlineMatch::TakePausedTime().count(), 0);
  Rollback::OnlineMatch::EndPause();
  EXPECT_EQ(Rollback::OnlineMatch::TakePausedTime().count(), 0);
}
