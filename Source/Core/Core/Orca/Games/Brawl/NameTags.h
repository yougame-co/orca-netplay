// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// Puts each player's YouGame username into Brawl's own name tags, so the game shows it over the
// fighter and on the results. Each tag also carries that player's own controls, so they keep their
// layout in someone else's game.
//
// Memory layout (Brawl rev 2; Project+ runs the same executable):
//   - Save tags: g_GameGlobal (0x805A00E0) +0x28 -> records (0x90172D40), one heap block of
//     120 entries of 0x124 bytes (0x88E0; the next block's header and gmGlobalRecord follow).
//     Entry i starts at +i * 0x124 with a 0xE0-byte per-name block; its tag (below) is at
//     +0xE0 + i * 0x124. Name first, up to 5 UTF-16BE characters and a 0. Empty name = unused.
//   - Tag controls: +0x0C rumble (1 on), +0x14 layout, 0x2D bytes, defaults at 0x80406938.
//     GameCube 12 bytes (L, R, Z, D-pad up, side, down, A, B, C-stick, Y, X, then flags: tap jump
//     0x80, set up 0x70; the page's Save also sets 0x01, not tap jump, which profiles neither
//     carry nor clear), Wii Remote 8, with Nunchuk 12 (last: shake smash 0x80, tap jump 0x40,
//     not set up 0x03), Classic 13 (last: tap jump 0x80). Other bytes are actions (0 attack,
//     1 special, 2 jump, 3 shield, 4 grab, 5 smash, 9 taunt, 0xA-0xC taunts, 0xE none).
//   - On scMelee's first frame the game copies each port's tag layout into the per-port table
//     (0x805B7480, pointed to by 0x805A00C8), which the match reads. So whatever a port's tag holds
//     when the match starts is what that port plays with.
//   - Character select: scene manager (0x805A0060) +4 -> scene ("scSelctCharacter"), +0x400 ->
//     muSelCharTask, +0x44 + 4 * i -> player area i: +0x1B0 port i, +0x1B4 kind (1: human joined),
//     +0x1C8 tag (-1: none). Joining clears the tag to -1. Leaving the screen copies each tag into
//     gmPlayerInitData (+0x0C name, +0x18 tag index).
//
// On every character-select frame, a joined port with a named player and no tag gets one: the tag
// already holding that name, else the lowest unused one, built as the game builds one. A duplicate
// name gets its port number as the last character (SAND2). The player's controls are written into
// it. A port with controls but no showable name gets "P<port>". A port with neither gets nothing.
// The game's own name list types tags in full-width characters (U+FF21 for A) while Orca writes
// ASCII, so names match as they read (width and case folded), and a tag the player typed in the
// game wins over one Orca made of that name: a player who made their tag in Options > Controls
// plays with it and gets no second tag of that name (before, Orca made one at the game's defaults,
// put them in it, and never read their own).
// A tag the player selected keeps its rumble and layout, including the exact defaults. Account
// controls apply when Orca assigns a tag to an unworn port.
//
// Determinism: writes depend only on emulated memory and the session's synced port list, and are
// idempotent (a re-run frame writes the same bytes; a port that wears a tag is left alone once that
// tag no longer holds the game's defaults). Names and controls come from the host (OnlineMatch.cpp,
// "Ports") and every machine has them before the frame they apply from.
//
// TODO: the character select's name plate keeps "PLAYER n" until the game redraws it (its setter,
// 0x8069B1CC, only runs when the player picks a tag). Needs a game-side code.
namespace Orca::UX
{
constexpr int TAG_CHARS = 5;
constexpr int TAG_SLOTS = 120;

// A YouGame username as a Brawl tag: letters uppercased, digits and '-', '.', '!', '?' kept, '_'
// and spaces become '-', anything else dropped, at most 5 characters. Locale-independent, so
// identical on every machine. Empty when nothing usable is left.
std::u16string BrawlTag(std::string_view username, bool view_names = false);

// A controls profile (PortInfo::controls): a tag's rumble byte, then its layout. Any other size is
// no profile.
constexpr u32 CONTROLS_LAYOUT_SIZE = 0x2D;
constexpr u32 CONTROLS_PROFILE_SIZE = 1 + CONTROLS_LAYOUT_SIZE;

// One frame's writes for these ports. Returns how many ports got a tag. Profile bytes are
// clamped to values the game's own menus
// could set (rumble 0 or 1, the flag bytes' own bits only, actions up to 0xE), else the game's
// default, since profiles come from other machines. With more than one port (a room), a tag the
// player saved in the game's Options > Controls keeps what they saved for the rest of the room.
// `view_names`: replay-only; replaces even a worn tag's name while preserving its controls.
int ApplyNameTags(GuestMemory& memory, const std::vector<Events::PortInfo>& ports,
                  bool view_names = false);

// A profile as text (lowercase hex, 92 characters) and back. Parsing refuses any other size, and
// any byte the game's menus couldn't set.
std::string ControlsHex(const std::vector<u8>& profile);
std::optional<std::vector<u8>> ParseControlsHex(std::string_view hex);
bool ControlsValid(const std::vector<u8>& profile);

// This player's own controls, read from their own save, to carry into an online game. Uses the tag
// `port` wears on the character select (which clears `last_edited`), else the tag named exactly by
// `last_edited` (the one they last set up on the game's Controls page, kept by the caller), else the
// tag named by `last_worn` (kept by the caller), else the tag that reads as `own_name` (one they
// typed in the game first). A `last_worn` that reads as `own_name` is looked up as `own_name`.
// Empty means the game's defaults. `read_tag`, if given, gets the name of the tag read (empty when
// none).
std::vector<u8> ReadOwnControls(const GuestMemory& memory, int port, std::string_view own_name,
                                std::u16string* last_worn, std::u16string* read_tag = nullptr,
                                std::u16string* last_edited = nullptr);

// Which tag the player sets up on the game's own Controls page (scButton: Options > Controls, a
// tag, then its controller's page). The page writes into the save's tag table, so the tag whose
// controls change there (same slot, same name) is the one they set up, whatever its name. Read-only.
class ControlsPageEdits
{
public:
  // One frame: the name of the tag whose controls changed on the page since the frame before
  // (the page's first frame is the reference; the first frame after it is its last look).
  std::optional<std::u16string> Next(const GuestMemory& memory);

private:
  struct Seen
  {
    std::u16string name;
    std::vector<u8> profile;
  };
  // Every tag as last seen on the page; empty while not on it.
  std::vector<Seen> m_seen;
};

// Which reads of the player's own tag are the player's own changes. A read that only shows what
// Orca put in the tag, a save that isn't this player's (after a resync, or before the app's
// controls reached the tag), or a switch to a tag at the game's defaults, is no change.
class OwnControlsWatch
{
public:
  // The read becomes the reference without counting as a change: after a resync, and after the app
  // sets the controls (the worn tag still holds the old ones until the next character select).
  void Rebase() { m_rebase = true; }
  // This frame's read from the tag named `tag`; returns the controls to publish when the player
  // changed them. `own` is the current profile, `is_default` whether the read is the game's
  // defaults with rumble on.
  std::optional<std::vector<u8>> Next(const std::vector<u8>& read, std::u16string_view tag,
                                      const std::vector<u8>& own, bool is_default);

private:
  std::vector<u8> m_last;
  std::u16string m_tag;
  bool m_rebase = false;
};

// The frame hook's reader of this player's own controls: the tag their port wears (ReadOwnControls,
// remembering the tag they last wore and the one they last set up on the Controls page) through an
// OwnControlsWatch.
class OwnControlsReader
{
public:
  // The app just set the controls: the next read is the reference (OwnControlsWatch::Rebase).
  void Rebase() { m_rebase = true; }
  // One read, on the port that isn't remote. `own` is the current profile, `resyncs` the session's
  // resync count (when it moves, the remembered tag is forgotten: a tag with its name may be in
  // someone else's save). Returns the controls to publish when the player changed them.
  std::optional<std::vector<u8>> Read(const GuestMemory& memory,
                                      const std::vector<Events::PortInfo>& ports,
                                      const std::vector<u8>& own, u64 resyncs);

private:
  std::u16string m_last_worn;
  std::u16string m_last_edited;
  ControlsPageEdits m_edits;
  OwnControlsWatch m_watch;
  u64 m_resyncs = 0;
  bool m_rebase = false;
};

// One frame of name tags: ApplyNameTags with `write_ports`, then, with a `reader` (first runs while
// playing alone), the read of this player's own controls from `ports`. The read comes after the
// writes, so it sees what Orca just put in the tag they wear (a tag they just made, given their
// controls) rather than the game's defaults that tag held before. Returns the controls to publish.
std::optional<std::vector<u8>> NameTagsFrame(GuestMemory& memory,
                                             const std::vector<Events::PortInfo>& write_ports,
                                             const std::vector<Events::PortInfo>& ports,
                                             OwnControlsReader* reader, const std::vector<u8>& own,
                                             u64 resyncs);

// Runs from Events' frame callback on every frame (first runs and re-runs), before the session
// saves a snapshot or keyframe, so those include what it wrote: NameTagsFrame on emulated memory,
// reading only when `read_own` (first runs while playing alone). A change the player made to their
// controls in the game is published to the session (Events::SetOwnControls), printed as
// "orca controls <hex>" for the app and kept in the user folder. Brawl rev 2 and Project+ only.
void NameTagsFrameHook(const Core::CPUThreadGuard& guard,
                       const std::vector<Events::PortInfo>& write_ports,
                       const std::vector<Events::PortInfo>& ports, bool read_own);

// The controls this player starts with, at boot: ORCA_CONTROLS=<hex> from the app, else the ones
// kept from an earlier run of this game (printed as "orca controls <hex>" for the page). Unset or
// invalid leaves none (the game's defaults).
void LoadOwnControls();
// "controls <hex>" from the app: the player changed them on the page. Returns false if invalid.
bool SetOwnControlsFromApp(std::string_view hex);
}  // namespace Orca::UX
