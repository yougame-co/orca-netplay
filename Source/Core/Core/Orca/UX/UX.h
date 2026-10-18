// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Core/Orca/Engine/Module.h"

namespace Orca
{
struct Profile;
}

// Orca's in-game UX for online play: the local controller fed by the YouGame app, the overlay,
// and the players' names in the game's own name tags.
namespace Orca::UX
{
// Bump whenever anything here that writes emulated memory, masks inputs or changes how a session
// ends changes, for every game. The session hashes it into the compatibility key as `ux=<n>`, so
// two builds that would diverge never meet in a room. A change to one game's module only bumps that
// game's own revision instead (Engine/Module.h Manifest::compat, Games/Brawl/BrawlGame.h), which
// the key adds as `ux=<n>.<game>` (Session/Online.h UxKeyPart).
// 31 keeps player-selected tag controls (including C-stick) instead of rewriting worn defaults.
constexpr int kCompatVersion = 31;

// The disc the game boots from. The overlay reads the game's own fonts from it. Call before Init.
void SetGameDisc(std::string path);
// Call once before the game boots. Registers the pad source, overlay and session callbacks.
void Init();
// Call immediately after setting the active boot profile. nullptr clears the selection.
void SelectModule(const Profile* profile);
// Call at exit, before the UI and logging shut down.
void Shutdown();
// The game module the session (Rollback/OnlineMatch.cpp) asks: the selected boot module, else
// the builtin (a session run without the UX, as in tests, asks the same code it always did).
Orca::Engine::Module& Game();
}  // namespace Orca::UX
