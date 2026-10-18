// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

struct JitBlock;
namespace Core
{
class CPUThreadGuard;
}

// JIT warm-up. The first frames of a match make Dolphin's JIT compile tens of thousands of blocks,
// which shows up as long frames as the stage and fighters appear. Orca records which blocks a match
// compiled (address, instruction ranges, and a hash of their words) and, in later matches, compiles
// those whose words are back in RAM a few ms per frame while the match loads. See ORCA.md,
// "Match start".
//
// Host-side only, and it must produce exactly the block the JIT would have compiled: same words
// and addresses, same feature flags, guest float mode and the same instruction ranges, or it is
// thrown away. Like JIT blocks kept across rollback, this relies on the game invalidating code it
// rewrites, so blocks whose words changed soon after compiling are never recorded. Only
// instruction-BAT-mapped code is touched, and nothing compiles when it could clear code memory
// under the running block (JitBase::CanCompileFromHook).
namespace Orca::JitWarm
{
struct Block
{
  u32 effective = 0;  // block start, where the game branches to
  u32 physical = 0;   // address an instruction BAT maps it to
  u32 flags = 0;      // JIT feature flags it was compiled with (MSR.DR, MSR.IR, HID2.PSE)
  u64 hash = 0;       // hash of the ranges and their instruction words at compile time
  u16 when = 0;       // first-pass frames from match start to its first compile (capped)
  u8 age = 0;         // matches since one last compiled it or found it compiled
  std::vector<std::pair<u32, u32>> ranges;  // physical [start, end) of its instructions
};

// The hash a block would have with RAM as it is now; nullopt if a range is outside MEM1/MEM2.
std::optional<u64> HashRanges(std::span<const u8> mem1, std::span<const u8> mem2,
                              const std::vector<std::pair<u32, u32>>& ranges);

// File format (<user>/Cache/<profile>.jitwarm): header, blocks, checksum. Decode takes nothing it
// doesn't expect from a JIT: a file over MAX_FILE_BYTES, an address or range outside MEM1 and MEM2
// or not word-aligned, or a block covering more instructions than the JIT puts in one is refused,
// whoever wrote the file. (The list only says what to compile ahead: each block is still checked
// against RAM before it compiles, and the warm-up never changes what the game computes.)
constexpr std::size_t MAX_FILE_BYTES = std::size_t{16} << 20;
std::vector<u8> Encode(const std::vector<Block>& blocks);
std::optional<std::vector<Block>> Decode(std::span<const u8> bytes);

// After a match: known blocks seen during it (`seen`, by index) reset their age and the rest age by
// one; `learned` blocks are added or refreshed. Blocks older than MAX_AGE are dropped, then the
// oldest beyond MAX_BLOCKS.
constexpr int MAX_AGE = 24;
constexpr std::size_t MAX_BLOCKS = 200'000;
std::vector<Block> Merge(std::vector<Block> known, const std::vector<bool>& seen,
                         std::vector<Block> learned);

// Pacing for the first sweep. `due` lists, in queue order, each first-run frame (`when`) and the
// queue index past its last block; the sweep is at `cursor` before `frame`. Returns how far it must
// get at this boundary so each block is compiled `lead` frames before its `when`, spreading the
// remaining work evenly over the boundaries left.
std::size_t PacedTarget(std::span<const std::pair<int, std::size_t>> due, std::size_t cursor,
                        int frame, int lead);

// Called for every block the JIT finishes. Records during a match's first RECORD_FRAMES frames,
// never while warming.
void OnBlockFinalized(const JitBlock& block);

// Called at each frame boundary whose next frame is a first pass, in host float mode.
void OnBoundary(const Core::CPUThreadGuard& guard);

// Whether the running scene is a match, for a game whose scenes Orca reads (Brawl rev 2 and the
// launchers that boot it); nullopt while the scene can't be read, and for any other game.
// Whether the active module can read this game's scenes for warm-up and host diagnostics.
bool ScenesReadable();
std::optional<bool> SceneIsMatch(const Core::CPUThreadGuard& guard);

// The running scene, for the same games: its object and its name's address; nullopt while it
// can't be read and for any other game. Reads only.
struct Scene
{
  u32 object = 0;
  u32 name = 0;
  bool operator==(const Scene&) const = default;
};
std::optional<Scene> CurrentScene(const Core::CPUThreadGuard& guard);
// The scene name at `name` (CurrentScene's), at most 31 characters; empty if unreadable.
std::string SceneName(const Core::CPUThreadGuard& guard, u32 name);
}  // namespace Orca::JitWarm
