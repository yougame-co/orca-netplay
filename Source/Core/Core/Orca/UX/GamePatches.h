// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// Orca's own game patches from Data/Sys/Orca/<profile>.patches. They are always on in a session
// (the player's own GameSettings codes are dropped) and are hashed into the lobby compatibility
// key. A launcher profile's file (e.g. PPLUS32.patches) may only hold guarded groups, since the
// launcher's own codes own most memory. One 32-bit word per line, all hex:
//
//   <address> <original> <value>   # guarded: written only if memory holds <original>
//   <address> * <value>             # data: written every frame
//
// Consecutive guarded lines (4 bytes apart) form a group, written only while every word still
// holds its original. So a code patch lands once each time its module loads, and never into some
// other module that later occupies the same memory (one word could match by chance; a run of words
// won't). A data group can include a marker word ('ORCA') so it lands only once and later player
// choices stick. A line whose original equals its value is a pure guard.
//
// Toggled groups switch on and off with a condition on memory instead of landing once:
//
//   when <address> == <value>
//   when <address> & <mask> != <value>
//   <address> <original> <value>   # guarded lines, grouped as above
//   end
//
// While all `when` lines hold, a group holding its originals gets its values; otherwise a group
// holding its values gets its originals back. Anything else is left alone. Rules: blocks don't
// nest, contain no `*` lines, and each group has at least two words with at least one changing (it
// matches in both states, so it needs more words to avoid false matches). A condition may not read
// a word the file writes. Meant for branch flips while an online mode is on; real code goes in a
// cave.
//
// Patches apply at every frame boundary, including resimulated frames, and depend only on emulated
// memory, so both machines and every rollback make the same writes. Code words go through
// Dolphin's memory-patch path so the JIT drops stale copies. Toggled groups apply last, after
// every other frame-hook writer.
namespace Orca::UX
{
class GuestMemory;  // Core/Orca/Engine/GuestMemory.h

// One `when` line: (word & mask) == value, or != when `equal` is false. Unmapped addresses
// never hold.
struct PatchCondition
{
  u32 address = 0;
  u32 mask = 0xFFFFFFFF;
  u32 value = 0;
  bool equal = true;

  bool operator==(const PatchCondition&) const = default;
};

struct GamePatch
{
  u32 address = 0;
  std::optional<u32> original;
  u32 value = 0;
  // True when this line continues the previous line's group.
  bool joins_previous = false;
  // The enclosing block's `when` conditions; empty for untoggled lines.
  std::vector<PatchCondition> when;
};

// Nullopt, with a reason, for the first malformed line or a repeated address.
std::optional<std::vector<GamePatch>> ParseGamePatches(std::string_view text, std::string* error);

// Whether a group (or `*` line) would be written given the words now in memory. For a toggled
// group, assumes its conditions hold.
bool GroupApplies(std::span<const GamePatch> group, std::span<const u32> now);

// True if every condition holds (or there are none).
bool ConditionsHold(std::span<const PatchCondition> when, const GuestMemory& memory);

// What a toggled group does this frame. `now` may be shorter than the group if not all is mapped.
enum class Toggle
{
  Leave,  // already in the wanted state, or memory doesn't match the group
  On,     // originals present and conditions hold: write the values
  Off,    // values present and conditions don't hold: restore the originals
};
Toggle ToggleGroup(std::span<const GamePatch> group, std::span<const u32> now, bool holds);

// One word to write at a boundary.
struct PatchWrite
{
  u32 address = 0;
  u32 value = 0;
  // True for guarded (code) words: written via the memory-patch path so the JIT drops its copy.
  // False for `*` data lines.
  bool code = false;
};

// One frame's writes (changed words only) and the groups they belong to, for logging.
struct PatchPlan
{
  struct Group
  {
    u32 address = 0;  // first line's address
    u32 words = 0;
    bool operator==(const Group&) const = default;
  };
  std::vector<PatchWrite> writes;
  std::vector<Group> landed;  // guarded groups that land
  std::vector<Group> on;      // toggled groups switched on
  std::vector<Group> off;     // toggled groups switched back off
};

// Plans the writes for the current memory without writing: guarded groups and `*` lines, or
// with `toggled`, only toggled groups.
PatchPlan PlanGamePatches(std::span<const GamePatch> patches, const GuestMemory& memory,
                          bool toggled);

// Apply the active profile's patches from the frame hook: ApplyGamePatches first,
// ApplyToggledGamePatches after every other writer. A malformed file is reported as a session
// error and not applied.
void ApplyGamePatches(const Core::CPUThreadGuard& guard);
void ApplyToggledGamePatches(const Core::CPUThreadGuard& guard);
}  // namespace Orca::UX
