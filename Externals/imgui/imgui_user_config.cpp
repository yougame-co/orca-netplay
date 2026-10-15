// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "imgui_user_config.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

namespace
{
std::atomic<std::size_t> s_failures{0};
std::mutex s_lock;
// Where an assert failed already: __FILE__ strings live as long as the program.
std::set<std::pair<std::string_view, int>> s_said;
}  // namespace

void ImGuiAssertFailed(const char* expression, const char* file, int line)
{
  s_failures.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock{s_lock};
    if (!s_said.emplace(file, line).second)
      return;
  }
  // stderr is what a nogui run the YouGame app started writes to the game's log, whichever log
  // types are on (embedded, Orca turns on only the netplay and rollback ones).
  fmt::print(stderr,
             "ImGui assert failed, ignored (said once for this place): {}\n"
             "  File: {}\n  Line: {}\n",
             expression, file, line);
  Common::Log::GenericLogFmt<1>(Common::Log::LogLevel::LERROR, Common::Log::LogType::VIDEO, file,
                                line, FMT_STRING("ImGui assert failed, ignored: {}"), expression);
}

std::size_t ImGuiAssertFailures()
{
  return s_failures.load(std::memory_order_relaxed);
}
