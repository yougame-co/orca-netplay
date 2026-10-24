// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/Controllers.h"
#include "InputCommon/GCPadStatus.h"

// The local player's controller, read from the YouGame desktop app. The app's helper reads
// GameCube adapters and SDL gamepads and serves them as an authenticated SSE stream
// ($YOUGAME_BRIDGE/controllers/events), or writes them on Orca's stdin (the stdio bridge,
// Orca/Bridge.h). A background thread keeps the newest snapshot in a one-slot cache; the CPU thread
// reads it once per frame and never waits on the network or the pipe.
//
// The session records the pad returned here, so rollback replays the recorded input rather than
// reading again.
namespace Orca::UX
{
// The pad for this frame, or nullopt to fall back to Pad::GetStatus(0). CPU thread only.
// Neutral while unfocused unless ORCA_VIRTUAL_PAD_ONLY=1. `for_frame` marks the read feeding the
// input-age stats.
std::optional<GCPadStatus> LocalPad(bool for_frame = false);
std::optional<GCPadStatus> LocalPad(int ordinal, bool for_frame);
// CPU thread: keep explicit assignment mode for the lifetime of a multi-seat match.
void SetLocalPadCount(int n);

// The app device the newest `for_frame` read took the local pad from: the adapter's physical port
// or the standard pad's index in the app's snapshot. nullopt when that read had no app device
// (Dolphin's own input), or before one. Any thread. For the game's rumble (UX/Rumble.h).
struct LocalDevice
{
  PadSource source;
  int index;
};
std::optional<LocalDevice> LastLocalDevice();
std::optional<LocalDevice> LastLocalDevice(int ordinal);

// Starts the stream when running under the app: on the stdio bridge (YOUGAME_STDIO=1,
// Orca/Bridge.h) the app's snapshots arrive on stdin, otherwise from the loopback bridge
// (YOUGAME_BRIDGE and YOUGAME_TOKEN set). Call on the main thread before the game boots.
void StartControllerStreamFromEnvironment();
// The stdio bridge's stream: the cache takes each snapshot Orca::Bridge routes to it.
void StartControllerStreamFromStdio();

// For tests: start the stream against an explicit bridge.
void StartControllerStream(const std::string& bridge, const std::string& token);
void StopControllerStream();
// The newest snapshot's pad, ignoring window focus (tests and tools).
std::optional<GCPadStatus> LatestPad();
// The newest snapshot, every controller in it, ignoring window focus, and how long ago it came in
// (`age_ms`, this machine's steady clock); null without one. For the activity lines
// (Orca/Activity.h), never the game.
std::shared_ptr<const Snapshot> LatestSnapshot(double* age_ms);

// Latency stats. The p50..max fields time each new adapter report from USB receipt in the helper
// to being cached here; they cross two wall clocks, so they are only good for short runs.
//
// Input age is how old the newest adapter report was when a frame read it, sampled only when the
// pad changed. It sums two spans that are each timed on a single clock (time in the helper, time
// in our cache). The hop between processes crosses clocks, so only its jitter is reported ("hop").
// See ORCA.md, "Input latency".
struct ControllerTiming
{
  u64 snapshots = 0;
  u64 refused = 0;  // malformed events
  u64 samples = 0;
  double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
  bool attached = false;
  // Input age: totals since start (or the last reset), percentiles over the newest 4096 changes.
  Orca::Events::InputAge input;
  u64 age_samples = 0;
  double age_p50_ms = 0, age_p95_ms = 0, age_p99_ms = 0, age_max_ms = 0;
  double helper_p50_ms = 0, cache_p50_ms = 0;
  // Hop jitter: each hop minus the fastest hop of the second before it.
  u64 hop_samples = 0;
  double hop_p50_ms = 0, hop_p95_ms = 0;
};
ControllerTiming GetControllerTiming();
void ResetControllerTiming();
// Input-age totals only, without the cost of percentiles.
Orca::Events::InputAge GetInputAge();
}  // namespace Orca::UX
