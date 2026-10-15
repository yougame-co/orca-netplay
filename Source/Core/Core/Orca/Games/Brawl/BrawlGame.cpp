// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/BrawlGame.h"

#include <array>
#include <string_view>

#include "Core/Orca/Games/Brawl/FreeSpace.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"

namespace Orca::UX::BrawlGame
{
namespace
{
// Super Smash Bros. Brawl (NTSC-U), the revision whose executable the modules' addresses are for,
// and Project+'s launcher profile (Data/Sys/Orca/PPLUS32.ini).
constexpr std::string_view BRAWL_DISC = "RSBE01";
constexpr u16 BRAWL_REVISION = 2;
constexpr std::string_view PPLUS_PROFILE = "PPLUS32";

Orca::Engine::Manifest Game(std::string_view id, Orca::Engine::Ruleset ruleset, int compat)
{
  Orca::Engine::Manifest game;
  game.id = id;
  game.ruleset = ruleset;
  game.results = true;
  game.compat = compat;
  return game;
}
}  // namespace

Orca::Engine::Manifest Identify(const Orca::Profile* profile)
{
  Orca::Engine::Manifest game;
  if (profile && profile->mod)
  {
    // Mod mode: the builtin mod.json declares (ParseManifest allows one only on RSBE01 rev 2).
    switch (profile->mod->manifest.builtin)
    {
    case Orca::Mod::Builtin::BrawlRev2:
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl, BRAWL_REV2_COMPAT);
      break;
    case Orca::Mod::Builtin::BrawlRev2PPlus:
      game = Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus, BRAWL_REV2_PPLUS_COMPAT);
      break;
    default:
      break;
    }
  }
  else if (profile && profile->revision == BRAWL_REVISION)
  {
    // Legacy: Brawl rev 2 itself, or a launcher profile that boots it (Project+, or another build,
    // which plays no ruleset).
    if (!profile->IsLauncher() && profile->game_id == BRAWL_DISC)
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl, BRAWL_REV2_COMPAT);
    else if (profile->IsLauncher() && profile->disc == BRAWL_DISC)
    {
      game = profile->game_id == PPLUS_PROFILE ?
                 Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus, BRAWL_REV2_PPLUS_COMPAT) :
                 Game(BRAWL_REV2_LAUNCHER, Orca::Engine::Ruleset::None, BRAWL_REV2_LAUNCHER_COMPAT);
    }
  }
  // Brawl's disc, any revision and in either mode, builtin or not: its music is on stream voices.
  game.music =
      profile && (profile->IsLauncher() ? profile->disc : profile->game_id) == BRAWL_DISC;
  return game;
}

bool ScenesReadable(const Orca::Profile* profile, std::string_view running_game,
                    u16 running_revision)
{
  // Dolphin runs Brawl rev 2, a mod declares a builtin, or a launcher profile boots Brawl rev 2
  // (its running revision comes from the TMD and may not match, but the profile verified the disc).
  if (running_game == BRAWL_DISC && running_revision == BRAWL_REVISION)
    return true;
  if (profile && profile->mod && !Identify(profile).id.empty())
    return true;
  return profile && profile->IsLauncher() && profile->disc == BRAWL_DISC &&
         profile->revision == BRAWL_REVISION;
}

std::span<const Orca::Engine::Region> Regions()
{
  using Orca::Engine::RegionKind;
  // Every write the six legacy hashlog inputs and both TEST packs make lands in one of these
  // (S3's discovery run: 395 distinct 256-byte blocks). The heaps are what the game's own objects
  // live in (character and stage selects, message buffers, the rules, the records with the tags),
  // between the RELs the game loads into MEM1 (sora_* below 0x80B90000; fighters and stages at
  // about 0x8107D000-0x811A9000; Project+'s at 0x817C5000) and up to MEM2's arena. They still hold
  // the code of whatever REL loads inside them: telling code from data there is the enforcement
  // step's (modules.md section 4.2), not this record's.
  static constexpr std::array<Orca::Engine::Region, 7> REGIONS{{
      // Project+'s stage select data (RSS_EXDATA, the page index, the strike table)
      {0x8042C4E8, 0x8042C840, RegionKind::Data, "pplus-stage-data"},
      // Project+'s Code Menu (OnlineRules.h CODE_MENU_END)
      {0x804E0000, 0x804E2520, RegionKind::Data, "pplus-code-menu"},
      // g_mtRandDefault and the menu's mtRand, a fresh start's seed (OnlineMenu.h)
      {0x805A00B8, 0x805A00C0, RegionKind::Data, "rng-default"},
      {0x805A0420, 0x805A0428, RegionKind::Data, "rng-menu"},
      // The match block in sora_scene's free space, up to its limit (FreeSpace.h)
      {FreeSpace::kMatchBlock.begin, FreeSpace::kMatchBlockLimit, RegionKind::Scratch,
       "match-block"},
      {0x81200000, 0x817C0000, RegionKind::Data, "mem1-heap"},
      {0x90000800, 0x93600000, RegionKind::Data, "mem2-heap"},
  }};
  return REGIONS;
}

bool On(const Orca::Engine::Manifest& game)
{
  if (game.id.empty())
    return false;
  if (Orca::Mod::ActiveMod())
    return Orca::Mod::BuiltinOn(Orca::Mod::DeclaredBuiltin());
  return true;
}
}  // namespace Orca::UX::BrawlGame
