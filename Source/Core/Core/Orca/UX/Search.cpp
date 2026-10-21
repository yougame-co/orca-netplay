// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Search.h"

#include <mutex>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "Core/Orca/Session/Online.h"

namespace Orca::UX
{
namespace Search
{
namespace
{
std::mutex s_mutex;
State s_state = State::None;
bool s_ranked = false;
bool s_joining = false;
std::optional<std::chrono::steady_clock::time_point> s_wait_started;
}  // namespace

void Begin(bool ranked, std::chrono::steady_clock::time_point now)
{
  std::lock_guard lock(s_mutex);
  s_state = State::Searching;
  s_ranked = ranked;
  s_joining = false;
  s_wait_started = now;
}

void SetWaiting(bool waiting, std::chrono::steady_clock::time_point now)
{
  std::lock_guard lock(s_mutex);
  if (waiting)
    s_wait_started = now;
  else
    s_wait_started.reset();
}

std::string WaitingLine(std::chrono::steady_clock::time_point now)
{
  std::lock_guard lock(s_mutex);
  if (!s_wait_started)
    return {};
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - *s_wait_started).count();
  return fmt::format("Waiting for another player… {}:{:02}", seconds / 60, seconds % 60);
}

void Matched(bool hosting)
{
  std::lock_guard lock(s_mutex);
  s_wait_started.reset();
  // When joining, the join's own lines replace the search's until the host's game is loaded.
  s_joining = !hosting && s_state != State::None;
  s_state = hosting && s_state != State::None ? State::Found : State::None;
}

void End()
{
  std::lock_guard lock(s_mutex);
  s_wait_started.reset();
  s_state = State::None;
  s_joining = false;
}

bool Joining()
{
  std::lock_guard lock(s_mutex);
  return s_joining;
}

void JoinStarted()
{
  std::lock_guard lock(s_mutex);
  s_wait_started.reset();
  s_joining = true;
}

void JoinOver()
{
  std::lock_guard lock(s_mutex);
  s_joining = false;
}

std::pair<std::string, std::string> JoiningLines(const std::string& host, bool ranked)
{
  return {host.empty() ? std::string("Opponent found · joining their game…") :
                         fmt::format("Opponent found · joining {}…", host),
          ranked ? "RANKED BETA" : ""};
}

State Current()
{
  std::lock_guard lock(s_mutex);
  return s_state;
}

bool Ranked()
{
  std::lock_guard lock(s_mutex);
  return s_ranked;
}

std::pair<std::string, std::string> Lines()
{
  State state;
  bool ranked, joining;
  {
    std::lock_guard lock(s_mutex);
    state = s_state;
    ranked = s_ranked;
    joining = s_joining;
  }
  if (joining)
    return JoiningLines(Orca::Online::SeatName(0), ranked);
  const std::string beta = ranked ? "RANKED BETA" : "";
  if (state == State::Searching)
    return {"Searching for an opponent… (B: back)", beta};
  if (state == State::Found)
  {
    const std::string name = Orca::Online::SeatName(1);
    return {name.empty() ? std::string("Opponent found · waiting for them…") :
                           fmt::format("Opponent found · waiting for {}…", name),
            beta};
  }
  return {};
}
}  // namespace Search
}  // namespace Orca::UX
