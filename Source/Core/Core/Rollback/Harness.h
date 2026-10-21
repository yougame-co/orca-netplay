// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Test harness for the rollback core, driven by environment variables. Does nothing when none is
// set. Needs an Orca session (ORCA_SESSION=1) for the frame-boundary hook. Runs unthrottled unless
// YG_THROTTLE=1. "Frame" means the harness counter, one per pass through the frame boundary; it
// rewinds on a rollback, so re-run frames read the same scripted input.
//
//   YG_INPUT=<file>       scripted GameCube pads, one rule per line (# starts a comment):
//                           <from> <to> <port 1-4> <buttons/axes...>            hold
//                           mash <from> <to> <port> <period> <buttons/axes...>  3 of every period
//                           fuzz <from> <to> <port> <seed>      pseudo-random, new every 8 frames
//                         @<scene> before a rule counts from/to from entering that scene;
//                         @<scene>:<n> applies only on the n-th entry (1 is the first).
//                         Buttons A B X Y Z L R START UP DOWN LEFT RIGHT; axes SX= SY= CX= CY=
//                         (0-255, 128 is centre).
//   YG_SCENES=1           log scene changes (needs a profile scene reader; Brawl only for now)
//   YG_EXIT_AFTER=<n>     stop emulation at the end of first-pass frame n
//   YG_HASHLOG=<file>     per-frame RAM hash; ignored under YG_SYNCTEST
//   YG_SHOT_EVERY=<n>     screenshot every n first-pass frames, starting at YG_SHOT_FROM
//   YG_SHOT_AT=<f>[,<f>...]  also screenshot at these first-pass frames
//   YG_SYNCTEST=<k>       save every frame, rewind k, re-run, compare RAM and device-state hashes
//   YG_SYNCTEST_FROM=<n>  first frame of the sync test (default 300)
//   YG_SYNCTEST_DIAG=1    for the first mismatches: byte-level RAM diffs, DoState section diffs and
//                         a CoreTiming event trace; also a one-time snapshot size breakdown
//   YG_SYNCTEST_SECTIONS=1  count, per DoState section, the re-run frames where it differs. The
//                         whole device-state hash differs on almost every re-run (host counters,
//                         icache, GPU dirty flags), so real changes only show per section.
//   YG_SYNCTEST_RENDER=1  also render re-run frames (skipped by default, as in a match)
//   YG_SYNCTEST_CLEARJIT=1  clear the JIT after each load, so re-runs compile different blocks
//   YG_BENCH=<k>          save every frame and load k back every 60th; logs frame, save and load
//                         timings
//   YG_DUMP_FRAMES_FROM=<n> / _TO=<m>  Dolphin frame dump for first-pass frames n..m (add
//                         -C Graphics.Settings.DumpFramesAsImages=True; note Graphics, not GFX).
//                         Re-runs present nothing, so dumps pair 1:1 with a plain run's.
//   YG_PADREC=<file>      record every port's pad and the RAM checksum per frame
//   YG_LOOPBACK=<file>    replay a YG_PADREC recording through a real Orca session (Loopback.h).
//                         Seat 1 is local; the others are virtual remotes whose inputs arrive late
//                         and who send the recorded checksums, so any divergence is a desync.
//                         Tuning: _LAG (6) + _JITTER (3) frames late, _DELAY input delay (2),
//                         _MAX_ROLLBACK (7), _CHECKSUM_EVERY (10), _SEATS (2, or 4 with
//                         ORCA_TEST_PADS=4). A lag above max rollback plus input delay stalls the
//                         session. Exit well before the recording ends.
//                         _SILENT_FROM=<frame>: remotes go silent; PASS means the session gave up
//                         after _GIVE_UP_MS (10000; 0 is never). Any stall that long ends the run.
//                         _SPIKE=<period ms>,<length ms>[,<from frame>]: hold remote packets for
//                         length ms of every period, then deliver them at once. Use with
//                         YG_THROTTLE=1 to see stalls and the catch-up after them.
// In a session, the harness also logs a hash of the game's session-NAND save at exit.

#pragma once

#include <optional>

#include "Core/Rollback/Rollback.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Rollback::Harness
{
bool Active();
std::optional<GCPadStatus> InputOverride(int port);
// Tells the harness a session rolled back to the start of `frame` at this boundary.
void RewindTo(int frame);
// Tells the harness this boundary's rewind (RewindTo) put back this game's own earlier state as a
// new timeline (a fresh start, a join's restore of the origin, a way back to the player's own
// game): the frames from it run for the first time, though their numbers ran before.
void Restart();
void OnFrameBoundary(const Core::CPUThreadGuard& guard);
// The last first-pass frame the harness reached, or -1. Safe from any thread.
int ShownFrame();
}  // namespace Rollback::Harness
