// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Config
{
struct Location;
}

// This machine's own speed, reported to the YouGame page, and the one switch to the Performance
// graphics preset a run makes when the machine can't hold 60 fps. See ORCA.md, "Graphics".
//
// What it measures. A forward frame is a frame boundary whose frame ran for the first time in real
// time (not a rollback re-run, a catch-up or a fresh start's unseen frames). For each, `interval`
// is the time since the previous forward boundary. Re-runs fall inside it on purpose: they are
// this machine's cost. Taken out of it is every wait: the CPU thread asleep in CoreTiming (the
// throttle and the present pacing) and the session's stall and time-sync waits. What is left is
// `work`, the time the frame cost this machine. `gpu` is the part of it spent blocked on the
// graphics card (binding the back buffer and presenting), which tells a GPU-bound machine from a
// CPU-bound one. The interval is also split at the frame's last re-run boundary: what came before
// it (the rollback's load and its re-run frames, waits out) is re-run work, the rest forward work.
// Re-runs depend on the opponent (their distance, how often their inputs change), forward work only
// on this machine, so the page's verdict and the switch below read this machine's speed at a
// reference re-run load whoever it played (CapAtRef).
//
// A frame counts (is "valid") once the game has been in a match for WARM frames (menus and each
// match's start are left out), when nothing paused or hid the game since the last one, and when
// the game didn't hold the frame for a load. A window is WINDOW_S seconds of running time:
//   n    valid frames
//   cap  n / their total work: the frames per second this machine could run, not capped at 60
//   fc   n / their total forward work: the same without the re-runs
//   rw   re-run work per re-run frame (ms) inside those frames' intervals; rv how many re-run
//        frames that was
//   w50, w95  median and 95th percentile work (ms); g95: 95th percentile gpu (ms)
//   fps  frames shown per running second, as the overlay's meter counts them
//   rbf  rollback re-run frames (all of the window's)
//   m    peers plugged in: solo (0), 1v1 (1), multi (2-3)
//   t    1 when a test switch (ORCA_TEST_PERF, ORCA_PERF_AUTO=0, ORCA_TEST_SLOW) shaped the run
//
// The page gets one `orca perf {json}` line per window (stdout, embedded only, at most 300 bytes),
// versioned with "v":1:
//   {"v":1,"k":"info",...}  once, after "ready": backend, graphics card, adapters, CPU, threads
//   {"v":1,"k":"win",...}   every window, menus included (n is 0 there)
//   {"v":1,"k":"auto","cap":C}  once, when the switch to Performance fires (C: CapAtRef's median)
//
// Observation only: wall-clock time and the graphics settings, which never reach emulation, the
// replay, checksums or the compatibility key.
namespace Orca::Perf
{
// The rules, in one place. Tests and ORCA_TEST_PERF shorten them.
struct Rules
{
  // A window is this much running time.
  double window_s = 15.0;
  // In-match forward frames before frames count: a match's first 15 s are left out.
  int warm_frames = 900;
  // A window counts toward the switch with at least this many valid frames, solo or 1v1.
  int count_frames = 600;
  // The switch fires after this many counted windows in a row under auto_cap. 58: time sync gives
  // back at most one frame in 30, so the faster side can only slow to 59.94 * 30/31 ~ 58.0 fps
  // (Session.cpp TIME_SYNC_FRAMES); under that, the other player stalls.
  double auto_cap = 58.0;
  int auto_windows = 4;
  // The switch reads each window at one reference re-run load, as the page reads its verdict
  // (orca-perf.ts perfCapAtRef, RERUN_REF and RERUN_MIN): forward work per frame plus rerun_ref
  // re-run frames at the re-run cost the machine measured. Re-runs grow with the distance to the
  // opponent, not with this machine's speed: read with all its re-runs, an i7-8700 / RTX 2080 at
  // 40 ms each way measured 51-56 and switched in every match, though it ran 63-71 forward frames
  // a second.
  double rerun_ref = 0.1;
  // A window's re-run cost (rw) is trusted once it held this many re-run frames.
  int rerun_min = 30;
};

constexpr std::size_t LINE_BYTES = 300;  // the app forwards at most 300 characters of a line
constexpr std::size_t NAME_CHARS = 40;   // a name in the info line
constexpr double CAP_MAX = 999.9;        // cap when a window's work rounds to nothing

enum class Mode : u8
{
  Solo,
  OneVsOne,
  Multi,
};
Mode ModeFor(int peers);
const char* ModeName(Mode mode);

// One forward frame's times, in ms.
struct Frame
{
  double interval_ms = 0;
  double sleep_ms = 0;  // CPU thread asleep in CoreTiming
  double wait_ms = 0;   // the session's stall and time-sync waits
  double gpu_ms = 0;    // blocked binding the back buffer and presenting
  // The part of the work before the frame's last re-run boundary (a rollback's load and re-runs),
  // and how many re-run frames it held.
  double rerun_ms = 0;
  int reruns = 0;
};
// max(0, interval - sleep - wait).
double Work(const Frame& frame);
// Work without the re-runs: max(0, Work - rerun_ms).
double ForwardWork(const Frame& frame);

// Nearest-rank percentile (p in 0..100) of `values`; 0 when empty.
double Percentile(std::vector<double> values, double p);

struct Window
{
  Mode mode = Mode::Solo;
  int n = 0;
  double cap = 0;
  double fc = 0;  // forward frames per second (re-runs left out)
  double rw = 0;  // ms per re-run frame inside the valid frames' intervals, 0 with none
  int rv = 0;     // those re-run frames
  double w50 = 0;
  double w95 = 0;
  double g95 = 0;
  double fps = 0;
  int rbf = 0;
  int res = 0;      // the EFB scale in use (Auto resolved)
  bool pf = false;  // the picture settings are Performance's, with no anti-aliasing
  // The switch to Performance fired this run and the picture settings are still Performance's
  // (anti-aliasing aside, which waits for the next start); a player who changed them since is
  // measured on their own settings again.
  bool as = false;
  bool test = false;  // a test switch shaped the run (TestRulesActive)
  bool Counts(const Rules& rules) const;
};

// One window's numbers as frames come in.
class WindowMeter
{
public:
  // A valid frame: its work, the GPU share, and the part of it that was re-runs (and how many).
  void AddValid(double work_ms, double gpu_ms, double rerun_ms = 0, int reruns = 0);
  void AddShown(int frames);
  void AddRunning(double ms);
  void AddRerun(int frames);
  void SeePeers(int peers);
  double RunningMs() const { return m_running_ms; }
  bool Due(const Rules& rules) const { return m_running_ms >= rules.window_s * 1000; }
  // The window so far (res, pf and as are the caller's); the meter starts the next.
  Window Take();

private:
  std::vector<double> m_work;
  std::vector<double> m_gpu;
  double m_work_ms = 0;
  double m_forward_ms = 0;
  double m_rerun_ms = 0;
  int m_reruns = 0;
  double m_running_ms = 0;
  int m_shown = 0;
  int m_rerun = 0;
  int m_peers = 0;
};

// The re-run cost a window measured (ms per re-run frame), once it held rules.rerun_min re-run
// frames; nullopt with fewer.
std::optional<double> RerunCost(const Window& window, const Rules& rules);
// A window's speed at the reference re-run load (rules.rerun_ref): its forward work per frame plus
// that many re-run frames at the cost it measured, else at `known_rw` (the last cost measured);
// with neither (a calm link), its forward speed alone, so a slow machine is slow even with re-runs
// free. Never above its forward speed; a window without one (fc 0) reads its all-in cap. The page's
// perfCapAtRef toward slow.
double CapAtRef(const Window& window, const Rules& rules,
                std::optional<double> known_rw = std::nullopt);

// When to switch to Performance. A window counts when Window::Counts; menus and 3-4 player rooms
// (which dip normally) neither count nor break a run. The switch fires once, when the last
// auto_windows counted windows are all under auto_cap at the reference re-run load (CapAtRef) with
// the picture settings not already Performance's. A counted window at auto_cap or above, one
// already on Performance, or a settings change (Restart) starts the count over.
class AutoSwitch
{
public:
  explicit AutoSwitch(Rules rules = {}) : m_rules(rules) {}
  // True once: this run should switch now.
  bool Feed(const Window& window);
  void Restart() { m_run.clear(); }
  bool Fired() const { return m_fired; }
  // The median CapAtRef of the windows that fired it.
  double TriggerCap() const { return m_trigger_cap; }

private:
  Rules m_rules;
  std::vector<double> m_run;
  // The re-run cost the last counted window that measured one did (RerunCost).
  std::optional<double> m_rerun_cost;
  bool m_fired = false;
  double m_trigger_cap = 0;
};

// What a boundary knows about the frame that just ended.
struct Boundary
{
  // It ran for the first time in real time: not a re-run or a catch-up.
  bool first_run = false;
  // It ran unthrottled or unseen (a fresh start's tail or hold, the canonical boot, a replay, a
  // catch-up).
  bool excluded = false;
  // The page hid the game.
  bool hidden = false;
  // The game held it past one video frame (a load), and for how many more frames.
  bool held = false;
  int held_frames = 0;
  int peers = 0;
  // The session's re-run frames so far (restarts with each session).
  u64 rerun_total = 0;
  // Time the player has paused the game, in total.
  std::chrono::nanoseconds paused_total{};
};

// The frame rules, fed with clock readings: what OnBoundary decides about each frame, without the
// clock, so tests can drive it.
class Meter
{
public:
  // Running totals of the waits taken out of a frame.
  struct Totals
  {
    std::chrono::nanoseconds sleep{};    // AddSleep
    std::chrono::nanoseconds present{};  // AddPresent
    std::chrono::nanoseconds wait{};     // AddWait
  };
  explicit Meter(Rules rules = {}) : m_rules(rules) {}
  // A boundary at `now` (one steady clock throughout) with the totals so far. Returns the window
  // when one is due; its res, pf and as are the caller's.
  std::optional<Window> OnBoundary(const Boundary& boundary, std::chrono::nanoseconds now,
                                   const Totals& totals);
  // The scene, as NoteScene gets it.
  void NoteScene(std::optional<bool> in_match);
  const Rules& GetRules() const { return m_rules; }

private:
  Rules m_rules;
  std::optional<std::chrono::nanoseconds> m_last_forward;
  Totals m_totals;
  // The last re-run boundary since the last forward one, its totals, and how many there were.
  std::optional<std::chrono::nanoseconds> m_last_rerun;
  Totals m_rerun_totals;
  int m_reruns = 0;
  std::chrono::nanoseconds m_paused{};
  u64 m_rerun_total = 0;
  // Something since the last forward frame (a catch-up) makes the next one's interval meaningless.
  bool m_spoiled = false;
  bool m_scene_match = false;
  int m_match_frames = 0;
  WindowMeter m_window;
};

struct Info
{
  std::string backend;  // the active backend's config name (D3D, D3D12, Vulkan, OGL, Metal)
  std::string gpu;      // the graphics card it uses
  std::vector<std::string> adapters;
  int adapter = 0;
  std::string cpu;
  int threads = 0;  // logical CPU threads
  std::string os;   // win, mac, linux
  std::string rev;  // this build's source revision (short)
};
// A name for the info line: printable ASCII without quotes or backslashes, trimmed, at most
// NAME_CHARS characters.
std::string CleanName(std::string_view name);
// The lines' JSON, without the "orca perf " prefix. Each fits LINE_BYTES with the prefix: the info
// line drops the CPU name, then adapters past the first, if it would not.
std::string InfoJson(const Info& info);
std::string WindowJson(const Window& window);
std::string AutoJson(double cap);

// `gfx <key> <value>`, the app's live graphics changes (cap "gfx"). A fixed map: the CurrentRun
// layer it writes outranks the forced one, and VerifyForcedSettings runs only at boot, so no other
// setting may ever be writable this way.
//   res 0..4          Graphics.Settings.InternalResolution (0 Auto)
//   aniso -1..4       Graphics.Enhancements.MaxAnisotropy (-1 the game's own)
//   filter 0..2       Graphics.Enhancements.ForceTextureFiltering
//   resample 0..6     Graphics.Enhancements.OutputResampling
//   efb 0|1           Graphics.Hacks.EFBScaledCopy
//   gpudecode 0|1     Graphics.Settings.EnableGPUTextureDecoding
//   cpucull 0|1       Graphics.Settings.CPUCull
enum class GfxKey : u8
{
  Res,
  Aniso,
  Filter,
  Resample,
  Efb,
  GpuDecode,
  CpuCull,
};
struct GfxChange
{
  GfxKey key = GfxKey::Res;
  int value = 0;
  bool operator==(const GfxChange&) const = default;
};
// The whole line ("gfx res 1"); nullopt for anything else.
std::optional<GfxChange> ParseGfx(std::string_view line);
// Where each key writes.
Config::Location GfxLocation(GfxKey key);
std::vector<Config::Location> GfxLocations();
// The Performance preset's picture settings: native resolution, unscaled EFB copies, the game's
// own texture filtering and the default scaling filter. (Anti-aliasing is a next-start setting:
// the page turns it off for the next start.)
std::array<GfxChange, 5> PerformancePreset();
// Writes one change to the CurrentRun layer. CPU thread, at a frame boundary.
void ApplyGfx(const GfxChange& change);
// Whether the live picture settings are Performance's (anti-aliasing aside).
bool PerformancePicture();
// Whether they are, with MSAA 1 and SSAA off too.
bool PerformanceNow();

// ---- At run time ----

// The app saw "ready": lines may print, and measuring starts. Embedded runs only.
void Ready();
// Whether measuring has started (Ready, then a boundary).
bool Measuring();
// A new boot in the same process: measuring starts over (the info line is not sent again).
void NewBoot();
// The test switches, each read only with ORCA_TEST_COMMANDS=1 in a session (a harness run: the
// app passes Orca neither) and marked "t":1 on every window line:
//   ORCA_TEST_PERF=<window_s>,<warm_frames>  shorter rules (count_frames scales with the window);
//                     it also starts the lines without the app's "ready"
//   ORCA_PERF_AUTO=0  no switch to Performance
//   ORCA_TEST_SLOW=<ms>[,<ms_on_performance>]  a slow machine: each forward frame spins the CPU
//                     thread that long (the second while the picture is Performance's, default 0)
bool TestRulesActive();
// The rules in use: Rules{}, or ORCA_TEST_PERF's.
const Rules& ActiveRules();
// Time the CPU thread slept in CoreTiming, and spent binding and presenting the back buffer.
void AddSleep(std::chrono::nanoseconds time);
void AddPresent(std::chrono::nanoseconds time);
// The session's stall and time-sync waits at a boundary (Rollback::StepSession).
void AddWait(std::chrono::nanoseconds time);
// While one is alive (around StepSession), sleeps and presents are part of the wait, not of a
// frame.
class StepScope
{
public:
  StepScope();
  ~StepScope();
  StepScope(const StepScope&) = delete;
  StepScope& operator=(const StepScope&) = delete;
};
// Whether the running scene is a match (Brawl and Project+), at a forward boundary; nullopt when it
// can't be read.
void NoteScene(std::optional<bool> in_match);
// A `gfx` line's change, applied at the next forward boundary. Any thread. Only the last value
// queued for each key is kept, so a burst of lines (while paused, say) is at most one change per
// key when it applies.
void QueueGfx(const GfxChange& change);

// Every frame boundary of a session, on the CPU thread, before the session steps.
void OnBoundary(const Boundary& boundary);
}  // namespace Orca::Perf
