// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace DiscIO
{
class VolumeDisc;
}

namespace Orca
{
// The one game Orca.app plays for now, and how the disc picker asks for it.
inline constexpr std::string_view APP_GAME_ID = "RSBE01";
inline constexpr std::string_view APP_DISC_PROMPT =
    "Choose your own Super Smash Bros. Brawl disc image (USA).";

struct DiscCheck
{
  bool ok = false;
  // For a refusal: the status code (Orca::Status) and a sentence for the player.
  std::string code;
  std::string sentence;
};

// Checks before boot that the disc is this game, at the revision its Orca profile expects or one
// that AliasRevision plays as it.
DiscCheck CheckDisc(const std::string& path, std::string_view game_id);

// A known dump of an older revision that differs from the profile's revision only in a few bytes
// the console reads from the disc: patches those reads (VolumeDisc::SetReadPatches) so the disc
// reads as the profile's revision and both players' consoles run the same game. True if `volume`
// is such a dump and now reads as that revision. Sessions call it on every disc they boot.
bool AliasRevision(DiscIO::VolumeDisc& volume);

// False if `volume` is Brawl (USA) but doesn't read as Rev 2, the revision every Brawl profile
// plays: AliasRevision didn't apply (e.g. a read failed). For a disc whose check read another
// volume, like a loader's disc.
bool SessionDiscReady(const DiscIO::VolumeDisc& volume);

// Checks the boot file (-e) before anything else: "disc_missing" if nothing is at the path,
// "disc_unreadable" if it won't open; the sentence is the path. Dolphin's own check raises a panic
// alert, which is invisible inside the app's window on Windows and would hang the run.
DiscCheck CheckBootFile(const std::string& path);
}  // namespace Orca
