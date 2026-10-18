// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/RangeSet.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

// A one-time dump at the first invalid guest access without MMU emulation (the "Invalid write"
// panic). The Mac boot crash of 2026-10-06 (M1/M2 on macOS 15: Brawl's OSSleepThread writes
// through a NULL current thread, then the external-interrupt vector at 0x500 reads zero) needs
// to know whether low MEM1 was never written or whether the host views of RAM disagree. The
// lines go to stderr only, which the desktop app keeps in the run's log and sends with its crash
// report. Nothing in emulation changes.
namespace Orca::PanicDiag
{
// One host mapping of guest RAM, as the dump reads it.
struct View
{
  std::string name;  // "ram", "phys", "logical 80000000", ...
  const u8* base;
  u32 physical_address;
  u32 size;
};

// The physical words the dump reads through every view: the exception vectors (system reset,
// DSI, external interrupt, decrementer, syscall) and the OS globals around the current thread.
extern const std::vector<u32> WORDS;

// One `orca diag word` line: the big-endian word at `physical_address` through every view that
// covers it, and "differ" when they don't all agree.
std::string DescribeWord(u32 physical_address, const std::vector<View>& views);

// Once per process: registers, BATs, RAM views and WORDS, as `orca diag` lines on stderr. Then
// what the P+ crash at 0x80558678 (a wild return with r1 inside main.dol) needs to be pinned: all
// GPRs, the words around r1 and its back-chain, the OS's current context and thread, the module
// list, the words at pc, lr and srr0 with the JIT blocks compiled over them, which JIT blocks' RAM
// changed since they were compiled, the last exceptions taken, and the scene. Reads only.
void DumpOnce(Core::System& system, u32 effective_address, bool write);

// Guest RAM as plain host bytes, so the crash path reads it without the MMU (which could panic
// again). Effective addresses use the OS's default BATs: 0x80000000 and 0xC0000000 map MEM1,
// 0x90000000 and 0xD0000000 map MEM2 (physical 0x10000000).
struct GuestRam
{
  std::span<const u8> mem1;
  std::span<const u8> mem2;

  static constexpr u32 MEM2_PHYSICAL = 0x10000000;

  // The physical address behind an effective one, or nullopt outside MEM1/MEM2.
  std::optional<u32> Physical(u32 address) const;
  // The big-endian word there; nullopt unless word-aligned and inside MEM1/MEM2.
  std::optional<u32> Word(u32 address) const;
  bool Valid(u32 address) const { return Word(address).has_value(); }
};

// The PowerPC EABI back-chain from `sp` (r1): each frame's word 0 points to its caller's frame,
// and the word after that pointer is the LR its owner saved (a return address, if it saved one).
struct StackFrame
{
  u32 back_chain;
  std::optional<u32> saved_lr;
};
struct BackChain
{
  std::vector<StackFrame> frames;
  // Why the walk ended: "end" (a zero back-chain), "cap", "bad <word>" (not a RAM pointer),
  // "not above" (a back-chain at or below its frame), or "sp not ram".
  std::string stop;
};
constexpr int MAX_CHAIN_DEPTH = 10;
BackChain WalkBackChain(const GuestRam& ram, u32 sp, int max_depth = MAX_CHAIN_DEPTH);

// `orca diag chain` lines, five frames each: " <back-chain>><saved lr>" ("?" for an unreadable LR
// word), then the stop reason.
std::vector<std::string> DescribeBackChain(const BackChain& chain, u32 sp);

// `count` words from `address` as `orca diag <label> <address>: w w w ...` lines of eight.
std::vector<std::string> DescribeWords(const GuestRam& ram, std::string_view label, u32 address,
                                       int count);

// The OS's module list (OSModuleInfo chain at 0x800030C8, as Games/Brawl/RankedSet.cpp walks it): which
// loaded module section holds `address`, as "m<id>s<section>+<offset>"; "-" in none.
constexpr u32 OS_MODULE_LIST = 0x800030C8;
std::string ModuleOf(const GuestRam& ram, u32 address);
// One `orca diag mods` line: each module's id and header address (at most 16).
std::string DescribeModules(const GuestRam& ram);

// The OS's current context (0x800000D4) and thread (0x800000E4), as Dolphin's thread view reads
// them: pointers, the context's saved srr0/srr1/lr/r1, the thread's state, priority and stack
// bounds, whether its stack magic holds, and whether `r1` is inside that stack.
constexpr u32 OS_CURRENT_CONTEXT = 0x800000D4;
constexpr u32 OS_CURRENT_THREAD = 0x800000E4;
std::vector<std::string> DescribeOS(const GuestRam& ram, u32 r1);

// A hash of the RAM a JIT block was compiled from (its physical instruction ranges), taken when
// the block is finalized and again on the crash path: a block whose RAM changed without an
// invalidation is stale code the JIT may still run. 0 when a range is outside MEM1/MEM2. Host-only
// (one hash per compiled block); nothing in emulation reads it.
u64 CodeHash(const GuestRam& ram, const Common::RangeSet<u32>& physical_ranges);
GuestRam RamOf(Core::System& system);

// The last exceptions taken (vector, srr0, r1, lr), oldest first, for the dump. Written by
// PowerPCManager::CheckExceptions/CheckExternalExceptions on the CPU thread: four stores into a
// fixed ring, no emulated state. Branch-to-register targets are not kept: that would mean code in
// every JIT'd blr/bctr, which the normal path can't afford.
struct ExceptionNote
{
  u32 vector;
  u32 srr0;
  u32 r1;
  u32 lr;
};
constexpr u32 EXCEPTION_RING = 16;
inline std::array<ExceptionNote, EXCEPTION_RING> g_exceptions{};
inline u32 g_exception_count = 0;
inline void NoteException(u32 vector, u32 srr0, u32 r1, u32 lr)
{
  g_exceptions[g_exception_count++ % EXCEPTION_RING] = {vector, srr0, r1, lr};
}
// `orca diag exc` lines, four entries each as <vector>:<srr0>/<r1>/<lr>, oldest first.
std::vector<std::string> DescribeExceptions(std::span<const ExceptionNote> ring, u32 count);

// At a first-run frame boundary of a game whose scenes Orca reads (Orca/JitWarm.h): one
// `orca scene <name> at frame <n>` log line when the scene changes, and the scene the dump names.
// Reads only.
void NoteScene(const Core::CPUThreadGuard& guard, int frame);
}  // namespace Orca::PanicDiag
