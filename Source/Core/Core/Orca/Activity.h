// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX
{
struct Snapshot;
}

// How much the player is playing, for the YouGame app: in a session, one stdout line every 60 s
// while a game runs, and a last one for the rest of the interval when the game stops:
//
//   orca active <interval_s> <active_s> <inputs> <mode> <pads>
//
// - interval_s: the wall-clock seconds the line covers (60; the last line, what was left).
// - active_s: those of them within 30 s after a real local input, the page's rule
//   (web/src/lib/play-activity.ts: activeUntil = lastInput + 30 s). Never more than interval_s.
//   Both are rounded to the nearest second.
// - inputs: the real input edges in the interval, on all local controllers together.
// - mode: ranked, casual, friends, training or solo: the one with the most active time in the
//   interval (the most time when there was none).
// - pads: how many local controllers had at least one edge in the interval.
//
// The points belong to the one signed-in player, so several local controllers (couch play, the
// keyboard pad) make a second active when any of them had an edge in the 30 s before it; they never
// multiply it.
//
// A real input is a button pressed or released, a trigger pressed past its threshold or let back,
// or a stick pushed out of its centre or let back in, each with hysteresis: analog noise, a drifting
// stick, a stick or trigger held still, or a gamepad's constant stream of near-identical reports
// never count. Only this machine's controllers, read once per frame that runs for the first time in
// real time: never a peer's input, a rollback re-run, a joiner's rebuild or catch-up, a fresh
// start's unseen tail (all of which run with Rollback::IsResimulating()), or recorded pads replayed.
//
// Observation only: wall-clock time (steady_clock) and host memory. Nothing here feeds emulation,
// the replay, checksums, the compatibility key or anything a peer sees.
namespace Orca::Activity
{
constexpr s64 INTERVAL_MS = 60'000;
constexpr s64 ACTIVE_AFTER_INPUT_MS = 30'000;

// Hysteresis, in GCPadStatus units. A stick (127 = all the way) is out of its centre from
// STICK_OUT and back in under STICK_IN; a trigger (255 = all the way, or its digital click) is
// pressed from TRIGGER_PRESS and let go at TRIGGER_RELEASE or under. A worn stick's drift and a
// gamepad's jitter stay well inside either band.
constexpr int STICK_OUT = 48;
constexpr int STICK_IN = 28;
constexpr int TRIGGER_PRESS = 128;
constexpr int TRIGGER_RELEASE = 64;
// The digital buttons that count: A B X Y Start Z and the D-pad. L and R count as triggers.
constexpr u16 BUTTONS = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y |
                        PAD_BUTTON_START | PAD_TRIGGER_Z | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT |
                        PAD_BUTTON_DOWN | PAD_BUTTON_UP;

// One controller's edges, frame to frame.
class Edges
{
public:
  // The real input edges between the pad this controller last fed and `pad`. The first pad only
  // sets the baseline: a button already held when a controller is first seen is no press.
  int Feed(const GCPadStatus& pad);

private:
  bool m_primed = false;
  u16 m_buttons = 0;
  std::array<bool, 2> m_trigger_down{};  // L, R
  std::array<bool, 2> m_stick_out{};     // main stick, C-stick
};

enum class Mode : u8
{
  Solo,
  Training,
  Friends,
  Casual,
  Ranked,
};
constexpr std::size_t MODES = 5;
std::string_view ModeName(Mode mode);
// From what Orca knows: the room's queue (Online::RoomQueue(): "private", "casual" or "ranked"),
// whether a friend is plugged into the game, and whether the game is in its Training mode.
Mode DecideMode(std::string_view room_queue, bool friend_plugged, bool training);

struct Line
{
  s64 interval_ms = 0;
  s64 active_ms = 0;
  u32 inputs = 0;
  Mode mode = Mode::Solo;
  u32 pads = 0;
};
// "orca active 60 42 118 solo 1".
std::string FormatLine(const Line& line);
// A line with nothing to say: under half a second and no input (a game that stops right after a
// periodic line). The last line is left out then.
bool IsEmpty(const Line& line);

// The clock behind the lines, on one monotonic clock in milliseconds. Pure, for tests.
class Clock
{
public:
  Clock(s64 start_ms, Mode mode);
  // The game is in `mode` from `now_ms` on.
  void SetMode(s64 now_ms, Mode mode);
  // `edges` real input edges at `now_ms` on the local controller `pad` (an id unique to it).
  void Input(s64 now_ms, int edges, u32 pad);
  // When the current interval is INTERVAL_MS old.
  s64 DueAt() const { return m_start + INTERVAL_MS; }
  // Closes the interval at `now_ms` and starts the next there. What is left of the 30 s after the
  // last input carries over into it.
  Line Take(s64 now_ms);

private:
  void Advance(s64 now_ms);

  s64 m_start;
  s64 m_last;
  s64 m_active_until;
  Mode m_mode;
  std::array<s64, MODES> m_wall_ms{};
  std::array<s64, MODES> m_active_ms{};
  u32 m_inputs = 0;
  std::vector<u32> m_pads;  // with an edge in this interval
};

// Ids of local controllers, unique within a run.
constexpr u32 PAD_ADAPTER = 0x100;  // | the adapter's port (the app's controller stream)
constexpr u32 PAD_GAMEPAD = 0x200;  // | the app's index for a standard gamepad (incl. its keyboard)
constexpr u32 PAD_DOLPHIN = 0x300;  // Dolphin's own port 1 (keyboard and SDL pads mapped to it)
constexpr u32 PAD_SCRIPT = 0x400;   // | port: a test harness script (YG_INPUT)

struct Pad
{
  u32 id = 0;
  GCPadStatus status;
};

// Reads the local controllers at each frame boundary for the clock.
class Reader
{
public:
  // The local pad the session itself read at this boundary (Dolphin's own, or a test script's),
  // for a frame it reads as a first run. Kept until Boundary.
  void Note(u32 id, const GCPadStatus& pad);
  // One frame boundary. `first_run`: the frame about to run runs for the first time, in real time;
  // anything else (a rollback re-run, a joiner's rebuild or catch-up, a fresh start's unseen tail,
  // a launch joiner's own boot) reads nothing and drops what was noted for it. `gate`: the game
  // gets the player's input (the window has it); Dolphin's own pad reads neutral otherwise, so it
  // is skipped. `direct`: controllers read directly at this boundary (the app's stream, a harness
  // script). Appends each controller's edges, when it has any, to `edges`.
  void Boundary(bool first_run, bool gate, const std::vector<Pad>& direct,
                std::vector<std::pair<u32, int>>* edges);

private:
  void Feed(const Pad& pad, std::vector<std::pair<u32, int>>* edges);

  std::vector<Pad> m_noted;
  std::vector<std::pair<u32, Edges>> m_edges;
};
// Every controller in the app's snapshot, as the local pad would map it (UX/Controllers.h):
// connected adapter ports with a fresh report and the standard gamepads. Nothing while the snapshot
// is stale or the helper suspended: nothing is known then, which is not "nothing pressed".
void SnapshotPads(const UX::Snapshot& snapshot, double age_ms, std::vector<Pad>* out);

// ---- The running game ----

// Embedded, the app takes the game as started at its "ready" line (the first picture on screen),
// and until then counts every line from Orca as boot progress that keeps its boot watchdog from
// stopping a start that hangs. So with the app the clock starts only after that line: call this
// before the boot, and Ready once "ready" is out. Without it the clock starts at the first frame.
void HoldUntilReady();
// The app has its "ready" line (written just before this call). Any thread.
void Ready();
// The local pad the session read at this boundary for a first-run frame, when it came from
// Dolphin's own input (PAD_DOLPHIN) or a test script (PAD_SCRIPT). CPU thread.
void NoteLocalPad(u32 id, const GCPadStatus& pad);
// Every frame boundary in a session, on the CPU thread, after the session has decided the frame
// about to run. `first_run`: that frame runs for the first time, in real time
// (!Rollback::IsResimulating()); only those are read. The first such boundary (after "ready" with
// HoldUntilReady) starts the clock and its line printer.
void OnBoundary(bool first_run);
// The game stopped: prints the last line, for the time since the one before, and stops the
// printer. Nothing is counted after it. Any thread; safe to call again.
void Finish();
}  // namespace Orca::Activity
