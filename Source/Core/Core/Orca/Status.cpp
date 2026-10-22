// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Status.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "Core/Orca/Activity.h"
#include "Core/Orca/BootMemory.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Music.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/UX.h"
#include "Core/Rollback/ReplayStream.h"

namespace Orca::Status
{
namespace
{
std::mutex s_mutex;
std::string s_state;
bool s_errored = false;
std::atomic<bool> s_started{false};
std::atomic<bool> s_online_withdrawn{false};
std::atomic<bool> s_stopping{false};
std::FILE* s_out = nullptr;
std::set<std::string, std::less<>> s_app_caps;
// The last `orca caps` list printed (nullopt: none yet), under s_mutex.
std::optional<std::string> s_printed_caps;
// The app's last `caps` line, and the offer s_app_caps was last agreed against (under s_mutex).
std::string s_app_list;
std::string s_offer_seen;

// Both lists' names (under s_mutex).
std::set<std::string, std::less<>> Agreed(std::string_view offered, std::string_view app)
{
  std::set<std::string, std::less<>> ours;
  std::istringstream mine{std::string(offered)};
  for (std::string name; mine >> name;)
    ours.insert(name);
  std::set<std::string, std::less<>> both;
  std::istringstream theirs{std::string(app)};
  for (std::string name; theirs >> name;)
  {
    if (ours.count(name))
      both.insert(name);
  }
  return both;
}
// When CapsSettled was first asked (host clock: when "ready" waits, never what the game computes).
std::optional<std::chrono::steady_clock::time_point> s_settle_asked;
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
  std::string line = fmt::format("orca result {}", HarnessReport(OneLine(json)));
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
  bool app_reports;
  {
    std::lock_guard lock(s_mutex);
    // The app's protocol support survives withdrawal of this boot's online features.
    app_reports = Agreed("join", s_app_list).contains("join");
  }
  if (!app_reports)
  {
    Error(code, sentence);
    return;
  }
  BootMemory::LogMemory();
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  Print(fmt::format("orca error {} {}\n", code, OneLine(sentence)));
}

std::string OfferedCaps()
{
  std::string caps;
  std::istringstream base{CAPS};
  for (std::string cap; base >> cap;)
  {
    if ((s_online_withdrawn || Rollback::ReplayStream::ProductMode() || Mod::LocalPackRequested()) &&
        (cap == "join" || cap == "host" || cap == "direct"))
      continue;
    if (!caps.empty())
      caps += ' ';
    caps += cap;
  }
  if (Music::Supported())
    caps += fmt::format(" {}", MUSIC_CAP);
  // Mod mode (Core/Orca/Mod.h): besides the music switch above (host-only, the same rule in both
  // modes), what mod.json offers and its builtin carries, only once the builtin's guard words have
  // held and never after they failed (Mod::Offers). The pick caps go with "locks", as for a legacy
  // ruleset profile.
  if (Mod::ActiveMod())
  {
    for (const char* cap : {RESULTS_CAP, LOCKS_CAP, QUEUE2_CAP})
    {
      if (Mod::Offers(cap))
        caps += fmt::format(" {}", cap);
    }
    if (Mod::Offers(LOCKS_CAP))
      caps += fmt::format(" {}", PICK_CAPS);
  }
  else
  {
    // What the game module says (Engine/Module.h): results where it reads them while its addresses
    // hold, the locks with a ruleset.
    Orca::Engine::Module& game = UX::Game();
    if (game.Active() && game.Describe().results)
      caps += fmt::format(" {}", RESULTS_CAP);
    if (game.ProfileRuleset() != Orca::Engine::Ruleset::None)
      caps += fmt::format(" {} {} {}", LOCKS_CAP, QUEUE2_CAP, PICK_CAPS);
  }
  // Engine caps, like "music": every profile, legacy or mod, whatever its builtin or guards. Last,
  // so an app that reads fewer words drops them before any other.
  caps += fmt::format(" {} {}", TALK_CAP, RUMBLE_CAP);
  if (Rollback::ReplayStream::ProductMode())
  {
    caps += fmt::format(" {}", REPLAY_CAP);
#ifdef HAVE_BROADCAST_SINK
    caps += " broadcast";
#endif
  }
  caps += fmt::format(" {} {}", RECORD_CAP, HOSTUI_CAP);
  return caps;
}

void PrintCaps()
{
  if (!SessionActive())
    return;
  const std::string caps = OfferedCaps();
  std::lock_guard lock(s_mutex);
  s_printed_caps = caps;
  Print(fmt::format("orca caps {}\n", caps));
}

bool CapsSettled()
{
  if (!SessionActive() || Mod::GuardsDecided())
    return true;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(s_mutex);
  if (!s_settle_asked)
    s_settle_asked = now;
  return now - *s_settle_asked >= CAPS_SETTLE_LIMIT;
}

void UpdateCaps()
{
  // Legacy profiles offer the same caps for the whole run: nothing to follow.
  if (!SessionActive() || (!Mod::ActiveMod() && !s_online_withdrawn))
    return;
  const std::string caps = OfferedCaps();
  std::lock_guard lock(s_mutex);
  if (s_offer_seen == caps)
    return;
  s_offer_seen = caps;
  // What is on follows the offer at once (a withdrawn cap off, a cap the app already listed on),
  // before any new answer from the app.
  s_app_caps = Agreed(caps, s_app_list);
  if (s_printed_caps && *s_printed_caps != caps)
  {
    s_printed_caps = caps;
    Print(fmt::format("orca caps {}\n", caps));
  }
}

void WithdrawOnline()
{
  s_online_withdrawn = true;
  UpdateCaps();
}

void ResetOnline()
{
  s_online_withdrawn = false;
  const std::string caps = OfferedCaps();
  std::lock_guard lock(s_mutex);
  // Legacy profiles do not pass through UpdateCaps after a reboot. Restore the agreement here
  // against the app's existing list, without waiting for another caps command.
  s_app_caps = Agreed(caps, s_app_list);
  s_offer_seen.clear();
}

void SetAppCaps(std::string_view list)
{
  const std::string offered = OfferedCaps();
  std::lock_guard lock(s_mutex);
  s_app_list = std::string(list);
  s_app_caps = Agreed(offered, list);
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
  BootMemory::LogMemory();
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

bool IsStopping()
{
  return s_stopping.load(std::memory_order_relaxed);
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
    Error("boot", "YG Dolphin couldn't start the game");
  State("ended");
  return ExitCode();
}
}  // namespace Orca::Status
