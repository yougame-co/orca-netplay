// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/MenuEvents.h"

#include <fmt/format.h>

#include "Core/Orca/Status.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/Search.h"

namespace Orca::UX
{
std::string_view MenuEventText(OnlinePick pick, bool queue)
{
  switch (pick)
  {
  case OnlinePick::Casual:
    return queue ? "online casual" : "online casual local";
  case OnlinePick::Ranked:
    return queue ? "online ranked" : "online ranked local";
  case OnlinePick::Friends:
    return "online friends";
  }
  return {};
}

void AnnounceOnlinePick(OnlinePick pick, bool arm)
{
  // With the app's "host" capability, the queue searches while the player waits on the character
  // select.
  const bool queue = Orca::Status::Cap("host");
  if (!arm)
  {
    // Kept: the friends stay, nothing starts, and the app knows this pick never searches here.
    Orca::Status::Menu(fmt::format("{} kept", MenuEventText(pick, queue)));
    return;
  }
  Orca::Status::Menu(MenuEventText(pick, queue));
  // With `queue2` the app searches once the player is ready on the queue's own character select
  // (Games/Brawl/Queue.h, UX/QueueApp.h). Without it, the search starts at the pick.
  if (queue && pick != OnlinePick::Friends && Orca::Status::Cap("queue2"))
  {
    Search::End();
    QueueApp::Begin(pick == OnlinePick::Ranked);
  }
  else if (queue && pick != OnlinePick::Friends)
  {
    QueueApp::End();
    Search::Begin(pick == OnlinePick::Ranked);
  }
  else
  {
    QueueApp::End();
    Search::End();
  }
}

bool PickSearchable(OnlinePick pick)
{
  switch (pick)
  {
  case OnlinePick::Casual:
    return Orca::Status::Cap("pick-casual");
  case OnlinePick::Ranked:
    return Orca::Status::Cap("pick-ranked");
  case OnlinePick::Friends:
    break;
  }
  return false;
}

void AnnounceMenuCancel()
{
  Search::End();
  QueueApp::End();
  Orca::Status::Menu("cancel");
}
}  // namespace Orca::UX
