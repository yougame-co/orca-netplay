// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
// Runs the ORCA_UX_PROBE research script (scripted RAM writes and dumps). Does nothing unless the
// variable is set.
void ProbeFrame(const Core::CPUThreadGuard& guard, int frame);
// Test knobs only work where no real player can be met: offline, or in a dev game's room
// (ORCA_TEST_DEV_GAME).
bool TestKnobsAllowed();
}  // namespace Orca::UX
