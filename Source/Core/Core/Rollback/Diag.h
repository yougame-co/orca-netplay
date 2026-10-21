// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
class System;
}

// Opt-in measurements for rollback performance work, each enabled by an environment variable:
//   YG_FRAMETIME=<frame>  wall time between first-pass frame boundaries from that frame on
//   YG_THROTTLE=1         keep Dolphin's throttle in offline harness runs (normally unthrottled)
//   YG_SNAPTIME=1         time snapshot saves and loads, and count copy-on-write faults per frame
//   YG_NTCOPY=1           copy RAM into snapshots with non-temporal stores (x86-64 only)
//   YG_PAGEDIFF=<frame>   from that frame, count 4 KB pages of RAM that differ from 1, 2 and 4
//                         frames earlier, bucketed per 1 MB
//   YG_JITCODE_LOG=<path> log instruction words a JIT compile reads that differ from the previous
//                         compile ("chg") or from RAM ("stale"), plus what invalidated that address
//                         in between, cache clears, and free JIT code space
// Results are logged when the harness stops at YG_EXIT_AFTER.
namespace Rollback::Diag
{
bool KeepThrottle();
bool SnapTime();

// Called at every first-pass frame boundary.
void OnFirstPassBoundary(Core::System& system, int frame);
void LogSummary();

// Copies RAM into a snapshot buffer, using non-temporal stores if YG_NTCOPY is set.
void CopyRam(std::vector<u8>* dst, const u8* src, std::size_t size);
void AddSnapshotSave(double state_ms, double mem1_ms, double mem2_ms);
void AddCowSave(double state_ms, double protect_ms);
void AddRingLoad(double ms, std::size_t changed_blocks);
void AddPortSave(double ms);
void AddPortLoad(double ms);

// True when YG_JITCODE_LOG is set. A plain bool so the per-compile check stays cheap.
extern const bool g_jit_code_log;
void JitCodeSetFrame(int frame);
void JitCodeCompiled(u32 block_start, const std::vector<std::pair<u32, u32>>& words);
void JitCodeInvalidated(u32 address, u32 length, bool forced);
void JitCodeCleared(const char* why);
void JitCodeNote(const std::string& line);
// Logs ("stalecode") every word a live JIT block was compiled from that no longer matches what a
// fetch would read: code the guest rewrote without invalidating, which the JIT still runs.
void JitCodeCensus(const Core::CPUThreadGuard& guard, const char* why);
}  // namespace Rollback::Diag
