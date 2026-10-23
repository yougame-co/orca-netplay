// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

// What a pick in the game's Online menu means to the app: the "orca menu ..." lines on stdout and
// what each pick starts (the queue's own character select, the search). Engine side, moved out of
// the Brawl online-menu module (Games/Brawl/OnlineMenu.h, which reads the pick) in S2.
namespace Orca::UX
{
enum class OnlinePick
{
  Casual,
  Ranked,
  Friends,
};

// The words after "orca menu" on stdout: "online casual" or "online ranked" when the app's queue
// searches ("host" capability), with " local" appended when nothing searches; "online friends".
std::string_view MenuEventText(OnlinePick pick, bool queue = false);

// Prints "orca menu online ..." for a pick ("... local" without the app's "host") and starts what
// it means: the queue's own character select (with "queue2"), the search, or neither (Friends).
// `arm` false prints the line alone, marked "kept": a host that keeps its friends because the app
// can't search that queue now (PickSearchable), so the app says why and cancels, and nothing of the
// queue reaches the friends' game.
void AnnounceOnlinePick(OnlinePick pick, bool arm = true);

// Whether the app can search this pick's queue right now ("pick-casual", "pick-ranked":
// Orca::Status PICK_CAPS); never Friends. Any thread.
bool PickSearchable(OnlinePick pick);

// Back on the main menu while searching (or matched but still alone): ends the search and the
// queue, and prints "orca menu cancel".
void AnnounceMenuCancel();
}  // namespace Orca::UX
