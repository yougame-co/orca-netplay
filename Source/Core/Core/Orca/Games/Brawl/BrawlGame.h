// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "Core/Orca/Engine/Module.h"

namespace Orca
{
struct Profile;
}

// Which of the builtin module's games a session runs (Orca split, S3 "game ids out"): the one place
// that compares game ids, disc ids, revisions and mod.json builtins. Every game file and the engine
// ask the module (Module::Describe, Active, ProfileRuleset) instead.
namespace Orca::UX::BrawlGame
{
// Mod.h's builtin names, and the legacy-only fourth game: another launcher build that boots Brawl
// rev 2 (not Project+'s PPLUS32), which gets the menus, name tags and results but no ruleset.
// brawl-rev2-results is its mod-mode counterpart (a Brawl-engine mod without Project+'s code).
inline constexpr std::string_view BRAWL_REV2 = "brawl-rev2";
inline constexpr std::string_view BRAWL_REV2_PPLUS = "brawl-rev2-pplus";
inline constexpr std::string_view BRAWL_REV2_RESULTS = "brawl-rev2-results";
inline constexpr std::string_view BRAWL_REV2_LAUNCHER = "brawl-rev2-launcher";

// Each game's own compatibility revision (Manifest::compat; the key's `ux=<engine>.<game>`, or
// `ux=<engine>` alone while it is 0). The games share this module's code: a step change that
// reaches more than one of them bumps each. Brawl and Project+ revision 1 let a player return
// their held token to their own panel in a multi-friend room; older masks would diverge.
inline constexpr int BRAWL_REV2_COMPAT = 1;
inline constexpr int BRAWL_REV2_PPLUS_COMPAT = 1;
inline constexpr int BRAWL_REV2_RESULTS_COMPAT = 1;
inline constexpr int BRAWL_REV2_LAUNCHER_COMPAT = 0;

// The game `profile` runs (null: no session). Static for a boot: the profile and mod.json only,
// never memory.
Orca::Engine::Manifest Identify(const Orca::Profile* profile);
// Host only: whether the harness can read Brawl rev 2's scene names (Module::ScenesReadable), also
// without a profile when Dolphin itself runs Brawl rev 2 (`running_game`, `running_revision`).
bool ScenesReadable(const Orca::Profile* profile, std::string_view running_game,
                    u16 running_revision);
// The guest memory the module's steps write, for every game here (record-only, Engine/Regions.h).
std::span<const Orca::Engine::Region> Regions();
// Whether Orca may keep its own data in sora_scene's free space (FreeSpace.h, verified dead code in
// Brawl rev 2 and Project+; the legacy launcher game keeps what it had). Not brawl-rev2-results: a
// Brawl-engine mod without Project+'s code may run its own code there.
bool UsesFreeSpace(const Orca::Engine::Manifest& game);
// Whether the module's code applies to memory now, for the game `game` describes: in mod mode while
// the builtin's guard words held at the last Mod::RefreshGuards; in legacy mode always.
bool On(const Orca::Engine::Manifest& game);
}  // namespace Orca::UX::BrawlGame
