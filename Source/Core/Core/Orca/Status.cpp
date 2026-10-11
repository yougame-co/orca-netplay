// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Status.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "Core/Orca/Activity.h"
#include "Core/Orca/Music.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/Results.h"

namespace Orca::Status
{
namespace
{
std::mutex s_mutex;
std::string s_state;
bool s_errored = false;
std::atomic<bool> s_started{false};
std::atomic<bool> s_stopping{false};
std::FILE* s_out = nullptr;
std::set<std::string, std::less<>> s_app_caps;
void (*s_error_listener)(std::string_view, std::string_view) = nullptr;
void (*s_first_frame_listener)() = nullptr;

void Print(const std::string& line)
{
  std::FILE* const out = s_out ? s_out : stdout;
  std::fputs(line.c_str(), out);
  std::fflush(out);
}

// One line per status: a sentence can't break the protocol.
std::string OneLine(std::string_view text)
{
  std::string out(text);
  for (char& c : out)
  {
    if (c == '\n' || c == '\r')
      c = ' ';
  }
  return out;
}
}  // namespace

void State(std::string_view state)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  if (s_state == state)
    return;
  s_state = state;
  Print(fmt::format("orca state {}\n", state));
}

void Event(std::string_view state)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  s_state = state;
  Print(fmt::format("orca state {}\n", state));
}

void Menu(std::string_view event)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  Print(fmt::format("orca menu {}\n", OneLine(event)));
}

void Result(std::string_view json)
{
  if (!SessionActive() || !Cap(RESULTS_CAP))
    return;
  std::string line = fmt::format("orca result {}", OneLine(json));
  // The app forwards at most 300 characters of a line.
  if (line.size() > 300)
    return;
  std::lock_guard lock(s_mutex);
  Print(line + "\n");
}

void Line(std::string_view line)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  Print(OneLine(line) + "\n");
}

void Report(std::string_view code, std::string_view sentence)
{
  if (!Cap("join"))
  {
    Error(code, sentence);
    return;
  }
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  Print(fmt::format("orca error {} {}\n", code, OneLine(sentence)));
}

std::string OfferedCaps()
{
  std::string caps = CAPS;
  if (Music::Supported())
    caps += fmt::format(" {}", MUSIC_CAP);
  if (UX::ResultsVerified())
    caps += fmt::format(" {}", RESULTS_CAP);
  if (UX::Rules::ProfileRuleset() != UX::Rules::Ruleset::None)
    caps += fmt::format(" {} {} {}", LOCKS_CAP, QUEUE2_CAP, PICK_CAPS);
  return caps;
}

void PrintCaps()
{
  if (!SessionActive())
    return;
  const std::string caps = OfferedCaps();
  std::lock_guard lock(s_mutex);
  Print(fmt::format("orca caps {}\n", caps));
}

void SetAppCaps(std::string_view list)
{
  std::set<std::string, std::less<>> ours;
  std::istringstream mine{OfferedCaps()};
  for (std::string name; mine >> name;)
    ours.insert(name);
  std::set<std::string, std::less<>> both;
  std::istringstream theirs{std::string(list)};
  for (std::string name; theirs >> name;)
  {
    if (ours.count(name))
      both.insert(name);
  }
  std::lock_guard lock(s_mutex);
  s_app_caps = std::move(both);
}

bool Cap(std::string_view name)
{
  std::lock_guard lock(s_mutex);
  return s_app_caps.find(name) != s_app_caps.end();
}

void Stats(std::string_view json)
{
  if (!SessionActive() || !Cap("stats"))
    return;
  std::lock_guard lock(s_mutex);
  Print(fmt::format("orca stats {}\n", OneLine(json)));
}

void Shaders(std::size_t compiled, std::size_t total)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  Print(fmt::format("orca shaders {} {}\n", compiled, total));
}

void Error(std::string_view code, std::string_view sentence)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  if (s_errored)
    return;
  s_errored = true;
  Print(fmt::format("orca error {} {}\n", code, OneLine(sentence)));
  if (s_error_listener)
    s_error_listener(code, OneLine(sentence));
}

void SetOutput(std::FILE* out)
{
  std::lock_guard lock(s_mutex);
  s_out = out;
}

void SetErrorListener(void (*listener)(std::string_view code, std::string_view sentence))
{
  std::lock_guard lock(s_mutex);
  s_error_listener = listener;
}

int ExitCode()
{
  std::lock_guard lock(s_mutex);
  return s_errored ? 1 : 0;
}

void GameStarted()
{
  if (s_started.load(std::memory_order_relaxed) ||
      s_started.exchange(true, std::memory_order_relaxed))
  {
    return;
  }
  if (s_first_frame_listener)
    s_first_frame_listener();
}

void SetFirstFrameListener(void (*listener)())
{
  s_first_frame_listener = listener;
}

void Stopping()
{
  s_stopping.store(true, std::memory_order_relaxed);
}

int Finish()
{
  if (!SessionActive())
    return 0;
  // The activity lines' last one, if the game ran and nothing printed it yet.
  Activity::Finish();
  bool stuck;
  {
    std::lock_guard lock(s_mutex);
    stuck = !s_errored && !s_started.load(std::memory_order_relaxed) &&
            !s_stopping.load(std::memory_order_relaxed);
  }
  if (stuck)
    Error("boot", "Orca couldn't start the game");
  State("ended");
  return ExitCode();
}
}  // namespace Orca::Status
