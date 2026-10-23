// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "Common/CommonTypes.h"

// The guest memory a game module declares it writes, and what it actually writes (Orca split S3;
// modules.md section 4.2: "Writes: declared regions only ... Record-only for one release, then
// enforced"). Every write a module makes through GuardMemory (Engine/GuestMemory.h) is counted
// against the active module's regions; one outside all of them is counted and the first few are
// logged, and none is refused, so recording changes nothing a session computes or hashes.
namespace Orca::Engine
{
// What a region holds (modules.md section 4.1): game data the module edits in place, what its latch
// writes from the raw pads, text it writes for the game to show, or its own scratch state.
enum class RegionKind : u8
{
  Data,
  Latch,
  Text,
  Scratch,
};

// [begin, end) in guest memory, by effective address.
struct Region
{
  u32 begin = 0;
  u32 end = 0;
  RegionKind kind = RegionKind::Data;
  std::string_view name;

  constexpr bool Contains(u32 address, u32 bytes) const
  {
    return address >= begin && address < end && bytes <= end - address;
  }
};

struct WriteCounts
{
  u64 writes = 0;   // every write while regions are set
  u64 outside = 0;  // of those, outside every region
};

// The regions writes are checked against, from Engine::SetModule (empty: nothing is recorded).
// `regions` must stay valid for the process (a module's are static). Clears the counts. Called
// while no hook runs; switching one non-empty set for another under a running hook is not
// supported (a write may be checked against a mix of the two).
void SetWriteRegions(std::string_view module, std::span<const Region> regions);
// A module wrote `bytes` bytes at `address` (CPU thread).
void NoteWrite(u32 address, u32 bytes);
WriteCounts Writes();
}  // namespace Orca::Engine
