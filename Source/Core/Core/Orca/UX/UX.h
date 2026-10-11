// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// Orca's in-game UX for online play: the local controller fed by the YouGame app, the overlay,
// and the players' names in the game's own name tags.
namespace Orca::UX
{
// Bump whenever anything here that writes emulated memory, masks inputs or changes how a session
// ends changes. The session hashes it into the compatibility key as `ux=<n>`, so two builds that
// would diverge never meet in a room.
constexpr int kCompatVersion = 28;

// The disc the game boots from. The overlay reads the game's own fonts from it. Call before Init.
void SetGameDisc(std::string path);
// Call once before the game boots. Registers the pad source, overlay and session callbacks.
void Init();
// Call at exit, before the UI and logging shut down.
void Shutdown();
}  // namespace Orca::UX
