// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <utility>

// Engine state, moved out of the Brawl online-menu module (Games/Brawl/OnlineMenu.h) in S2: the
// app's search after a pick, as the overlay shows it.
namespace Orca::UX
{
// Matchmaking search state after a Casual or Ranked pick, for the overlay. The search ends when the
// app matches this game (`host <code>` or `join <code>`), the app cancels (`queue-cancel`), or the
// player backs out to the main menu, which prints "orca menu cancel" once. Any thread.
namespace Search
{
enum class State
{
  None,
  Searching,
  Found,
};
void Begin(bool ranked, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
// Host-only wait hint. QueueApp also starts/stops it when solo readiness changes; repeated ready
// frames must not restart it. A match or the end of a search clears it.
void SetWaiting(bool waiting,
                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
std::string WaitingLine(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
// The app matched this game: Found when hosting, None when joining. A joiner that was searching
// shows "Opponent found · joining <host>…" (Joining) until JoinOver, so its own select's search
// lines don't stay up while the host's game comes over.
void Matched(bool hosting);
// The opponent plugged in, the app ended the search, or the room went away. Ends Joining too.
void End();
State Current();
bool Ranked();
// A matched join began while this player was searching or on the queue's own select: the overlay
// says the opponent was found until JoinOver (or End).
void JoinStarted();
// A joiner's "Opponent found" lines are up: from a matched join until the host's game is loaded
// (JoinOver) or the join ends. Display only: Current() stays None meanwhile.
bool Joining();
// The joiner loaded the host's game, or its join ended.
void JoinOver();
// The overlay's two lines (empty: nothing to show).
std::pair<std::string, std::string> Lines();
// Lines() for the joining state, given the host's name (empty while the room hasn't named it).
std::pair<std::string, std::string> JoiningLines(const std::string& host, bool ranked);
}  // namespace Search
}  // namespace Orca::UX
