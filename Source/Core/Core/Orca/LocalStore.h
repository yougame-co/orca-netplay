// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

// What Orca keeps for this player between runs, in the user folder's Config (the -u folder the
// app gives each game). Engine side of the split (S2): game modules ask here and never open a file
// themselves (Tools/orca/forbidden-calls' file-open rule in game logic).
//
// Later (S6) the app owns this state (`seen <key>` in, `orca seen <key>` out), so it follows the
// account across machines; until then it is two small files.
namespace Orca::LocalStore
{
// ---- One-time "seen" keys (tips): <User>/Config/OrcaSeen.txt, one key per line ----
// The file's text holds `key` on a line of its own (spaces and a CR around it are ignored).
bool SeenIn(std::string_view text, std::string_view key);
// `text` with `key` on a line of its own added at the end, or `text` as is when it holds it.
std::string SeenWith(std::string text, std::string_view key);
// <User>/Config/OrcaSeen.txt, beside the kept controls.
std::string SeenPath();
// Whether `key` was kept as seen. The file is read once, at the first call of the run; later calls
// answer from that read and from MarkSeen. Any thread.
bool Seen(std::string_view key);
// Keeps `key` as seen: reads the file again (another run may have kept other keys since), adds the
// key and writes it. False when it couldn't. Any thread.
bool MarkSeen(std::string_view key);

// ---- This player's own controls for the active game: <User>/Config/OrcaControls-<id>.txt ----
// Empty without an active profile with a game id.
std::string ControlsPath();
// The kept text, if there is a file and it could be read.
std::optional<std::string> KeptControls();
// Keeps `text` (it replaces what was kept). False when there is no path or it couldn't be written.
bool KeepControls(std::string_view text);
}  // namespace Orca::LocalStore
