// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Orca::BootThrottle
{
// Host pacing only. Begin before BootCore, Finish at the game's first boundary (or on failed
// boot). A match or harness that has taken over pacing keeps its own throttle state.
void Begin();
void Finish(bool pacing_owned_elsewhere);
// Called by the headless first-frame listener, before an online match creates its ring.
void FinishAtFirstFrame();
}  // namespace Orca::BootThrottle
