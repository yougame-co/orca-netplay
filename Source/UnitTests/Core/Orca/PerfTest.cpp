// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Perf.h"

namespace
{
using namespace std::chrono_literals;
using Orca::Perf::AutoSwitch;
using Orca::Perf::Boundary;
using Orca::Perf::GfxChange;
using Orca::Perf::GfxKey;
using Orca::Perf::Meter;
using Orca::Perf::Mode;
using Orca::Perf::Rules;
using Orca::Perf::Window;

TEST(OrcaPace, NearestRankAndStrictHitchThresholds)
{
  Orca::Perf::PaceMeter meter;
  auto now = 0ns;
  meter.Present(now, 0, false);
  for (int ms = 1; ms <= 100; ++ms)
  {
    now += std::chrono::milliseconds(ms);
    meter.Present(now, 0, false);
  }
  const auto w = meter.Take();
  EXPECT_EQ(w.n, 100u);
  EXPECT_EQ(w.p, (std::array<double, 4>{50, 95, 99, 100}));
  EXPECT_EQ(w.h, (std::array<u64, 3>{75, 67, 50}));
  EXPECT_EQ(meter.Take().p, (std::array<double, 4>{}));
  meter.Present(now + 16667us, 0, false);
  EXPECT_DOUBLE_EQ(meter.Take().p[0], 16.667);
}

TEST(OrcaPace, RollbackHistogramCountsEventsAndKeepsMaximum)
{
  Orca::Perf::PaceMeter meter;
  for (int depth : {-1, 0, 1, 2, 3, 4, 7, 2})
    meter.Rollback(depth);
  const auto w = meter.Take();
  EXPECT_EQ(w.rb, (std::array<u64, 4>{1, 2, 1, 2}));
  EXPECT_EQ(w.rm, 7u);
  EXPECT_EQ(meter.Take().rb, (std::array<u64, 4>{}));
}

TEST(OrcaPace, TransitionUsesLastOldPresentAcrossUnreadableLoadAndWindow)
{
  Orca::Perf::PaceMeter meter;
  const auto old_scene = Orca::Perf::SceneCode("muMenuMain");
  const auto new_scene = Orca::Perf::SceneCode("scSelctCharacter");
  EXPECT_FALSE(meter.Present(0ms, old_scene, false));
  EXPECT_FALSE(meter.Present(17ms, old_scene, false));
  EXPECT_FALSE(meter.Present(50ms, 0, false));
  meter.Take();
  const auto change = meter.Present(100ms, new_scene, false);
  ASSERT_TRUE(change);
  EXPECT_EQ(change->from, old_scene);
  EXPECT_EQ(change->to, new_scene);
  EXPECT_EQ(change->ms, 83);
  EXPECT_FALSE(change->headless);
  EXPECT_FALSE(meter.Present(117ms, new_scene, false));
  EXPECT_EQ(meter.Take().p[3], 50);
}

TEST(OrcaPace, HeadlessSceneSubmissionsNeverClaimPresentedIntervals)
{
  Orca::Perf::PaceMeter meter;
  meter.Present(0ms, Orca::Perf::SceneCode("scBoot"), true);
  const auto scene = meter.Present(500ms, Orca::Perf::SceneCode("scTitle"), true);
  ASSERT_TRUE(scene);
  EXPECT_TRUE(scene->headless);
  EXPECT_EQ(scene->ms, 500);
  const auto w = meter.Take();
  EXPECT_TRUE(w.headless);
  EXPECT_EQ(w.n, 0u);
  EXPECT_EQ(w.p, (std::array<double, 4>{}));
}

TEST(OrcaPace, PauseBreaksIntervalsAndTransitionTiming)
{
  Orca::Perf::PaceMeter meter;
  meter.Present(0ms, Orca::Perf::SceneCode("scBoot"), false);
  meter.BreakInterval();
  EXPECT_FALSE(meter.Present(1h, Orca::Perf::SceneCode("scTitle"), false));
  EXPECT_EQ(meter.Take().n, 0u);
  meter.Present(1h + 16ms, Orca::Perf::SceneCode("scTitle"), false);
  EXPECT_EQ(meter.Take().p[0], 16);
}

TEST(OrcaPace, FixedStorageOverflowIsVisibleAndMaximumStillExact)
{
  Orca::Perf::PaceMeter meter;
  meter.Present(0ms, 0, false);
  for (std::size_t i = 1; i <= Orca::Perf::PaceMeter::SAMPLE_CAPACITY; ++i)
    meter.Present(std::chrono::milliseconds(i), 0, false);
  meter.Present(std::chrono::milliseconds(Orca::Perf::PaceMeter::SAMPLE_CAPACITY + 100), 0, false);
  const auto w = meter.Take();
  EXPECT_EQ(w.n, Orca::Perf::PaceMeter::SAMPLE_CAPACITY + 1);
  EXPECT_EQ(w.dropped, 1u);
  EXPECT_EQ(w.p, (std::array<double, 4>{1, 1, 1, 100}));
  EXPECT_EQ(w.h, (std::array<u64, 3>{1, 1, 1}));
}

TEST(OrcaPace, ProtocolFits300BytesAtAllSaturationsIncludingNewline)
{
  Orca::Perf::PaceWindow w;
  const u64 huge = std::numeric_limits<u64>::max();
  w.scene = Orca::Perf::SceneCode("scSelctCharacter");
  w.n = w.rm = w.sc = w.dropped = huge;
  w.h.fill(huge);
  w.rb.fill(huge);
  w.p.fill(std::numeric_limits<double>::max());
  w.sm = std::numeric_limits<double>::max();
  w.headless = true;
  const std::string line = "orca perf " + Orca::Perf::PaceJson(w) + "\n";
  EXPECT_LE(line.size(), Orca::Perf::LINE_BYTES) << line;
  EXPECT_EQ(line.find("inf"), std::string::npos);
  EXPECT_EQ(line.find("nan"), std::string::npos);
  EXPECT_LE(("orca perf " + Orca::Perf::SceneJson({w.scene, w.scene, w.sm, true}) + "\n").size(),
            Orca::Perf::LINE_BYTES);
  w.p.fill(std::numeric_limits<double>::quiet_NaN());
  w.sm = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Orca::Perf::PaceJson(w).find("nan"), std::string::npos);
}

TEST(OrcaPace, SceneCodesAreBoundedAndSafeJsonText)
{
  EXPECT_EQ(Orca::Perf::SceneCodeName(Orca::Perf::SceneCode("muMenuMain")), "muMenuMa");
  EXPECT_EQ(Orca::Perf::SceneCodeName(Orca::Perf::SceneCode("scMelee")), "scMelee");
  EXPECT_EQ(Orca::Perf::SceneCodeName(Orca::Perf::SceneCode("\"\\\n")), "unknown");
}

TEST(OrcaPace, VersionOneWireContract)
{
  Orca::Perf::PaceWindow w;
  w.scene = Orca::Perf::SceneCode("scMelee");
  w.n = 899;
  w.p = {16.7, 17.1, 33.4, 68.2};
  w.h = {12, 10, 1};
  w.rb = {3, 2, 1, 4};
  w.rm = 7;
  w.sc = 2;
  w.sm = 14.3;
  EXPECT_EQ(Orca::Perf::PaceJson(w),
            R"({"v":1,"k":"pace","s":"scMelee","n":899,"p50":16.7,"p95":17.1,"p99":33.4,"max":68.2,"h25":12,"h33":10,"h50":1,"rb":[3,2,1,4],"rm":7,"sc":2,"sm":14.3,"d":0,"h":0})");
  EXPECT_EQ(Orca::Perf::SceneJson({Orca::Perf::SceneCode("muMenuMain"),
                                 Orca::Perf::SceneCode("scSelctCharacter"), 83.4, false}),
            R"({"v":1,"k":"scene","from":"muMenuMa","to":"scSelctC","ms":83.4,"h":0})");
}

std::chrono::nanoseconds Ms(double ms)
{
  return std::chrono::nanoseconds(static_cast<s64>(ms * 1'000'000));
}

// Short windows, so a test plays a few hundred frames: 1 s windows, 10 frames of warm-up, 40 valid
// frames for a window to count.
Rules TestRules()
{
  Rules rules;
  rules.window_s = 1.0;
  rules.warm_frames = 10;
  rules.count_frames = 40;
  return rules;
}

// Drives a Meter frame by frame on a clock of its own.
class Driver
{
public:
  explicit Driver(Rules rules = TestRules()) : m_meter(rules) {}

  // One forward frame: `interval_ms` since the last forward boundary, of which `sleep_ms` the CPU
  // thread slept, `wait_ms` the session waited and `gpu_ms` went to binding and presenting.
  std::optional<Window> Frame(double interval_ms, double sleep_ms = 0, double wait_ms = 0,
                              double gpu_ms = 0, Boundary boundary = Forward())
  {
    m_now += Ms(interval_ms);
    m_totals.sleep += Ms(sleep_ms);
    m_totals.wait += Ms(wait_ms);
    m_totals.present += Ms(gpu_ms);
    boundary.paused_total = m_paused;
    boundary.rerun_total = m_rerun;
    std::optional<Window> window = m_meter.OnBoundary(boundary, m_now, m_totals);
    if (window)
      m_windows.push_back(*window);
    return window;
  }
  // A re-run boundary (rollback), `ms` after the last boundary.
  void Rerun(double ms, bool excluded = false)
  {
    m_now += Ms(ms);
    m_rerun += 1;
    Boundary boundary;
    boundary.first_run = false;
    boundary.excluded = excluded;
    boundary.paused_total = m_paused;
    boundary.rerun_total = m_rerun;
    m_meter.OnBoundary(boundary, m_now, m_totals);
  }
  // The session waits `ms` (a stall or time sync) without a boundary.
  void Wait(double ms)
  {
    m_now += Ms(ms);
    m_totals.wait += Ms(ms);
  }
  void Pause(double ms)
  {
    m_now += Ms(ms);
    m_paused += Ms(ms);
  }
  void Scene(std::optional<bool> in_match) { m_meter.NoteScene(in_match); }
  // In a match, past the warm-up, and at the start of a window.
  void Warm()
  {
    Scene(true);
    NextWindow(1);
  }
  // Frames of `interval_ms` (of which `sleep_ms` asleep) until the next window comes out.
  Window NextWindow(double interval_ms, double sleep_ms = 0, Boundary boundary = Forward())
  {
    for (int i = 0; i < 100'000; ++i)
    {
      if (std::optional<Window> window = Frame(interval_ms, sleep_ms, 0, 0, boundary))
        return *window;
    }
    ADD_FAILURE() << "no window";
    return {};
  }
  static Boundary Forward()
  {
    Boundary boundary;
    boundary.first_run = true;
    return boundary;
  }
  const std::vector<Window>& Windows() const { return m_windows; }

private:
  Meter m_meter;
  std::chrono::nanoseconds m_now{1'000'000'000};
  std::chrono::nanoseconds m_paused{};
  Meter::Totals m_totals;
  u64 m_rerun = 0;
  std::vector<Window> m_windows;
};

Window Win(double cap, int n = 50, Mode mode = Mode::OneVsOne, bool pf = false)
{
  Window window;
  window.cap = cap;
  window.n = n;
  window.mode = mode;
  window.pf = pf;
  return window;
}
}  // namespace

TEST(OrcaPerf, WorkTakesOutSleepsAndWaits)
{
  EXPECT_DOUBLE_EQ(Orca::Perf::Work({20, 5, 3, 1}), 12);
  // Waits longer than the interval (a wait that began before it) leave no negative work.
  EXPECT_DOUBLE_EQ(Orca::Perf::Work({10, 8, 5, 0}), 0);
}

TEST(OrcaPerf, PercentilesAreNearestRank)
{
  std::vector<double> values;
  for (int i = 100; i >= 1; --i)
    values.push_back(i);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 50), 50);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 95), 95);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 100), 100);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile(values, 0), 1);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile({7}, 95), 7);
  EXPECT_DOUBLE_EQ(Orca::Perf::Percentile({}, 50), 0);
}

TEST(OrcaPerf, CapIsWhatTheMachineCouldRunNotWhatItShowed)
{
  // A fast machine held at 60 by the throttle: 16.7 ms frames, 6.7 of them asleep. It shows 60 but
  // could run 100.
  Driver d;
  d.Warm();
  const Window w = d.NextWindow(16.7, 6.7);
  EXPECT_NEAR(w.cap, 100, 0.5);
  EXPECT_NEAR(w.w50, 10, 0.01);
  EXPECT_NEAR(w.w95, 10, 0.01);
  EXPECT_NEAR(w.fps, 1000 / 16.7, 0.5);
  EXPECT_GE(w.n, 55);
  EXPECT_EQ(w.mode, Mode::Solo);
}

TEST(OrcaPerf, ASlowMachineReadsUnder60)
{
  // 22.7 ms frames with no sleep at all: 44 fps, a3851fbb's rate.
  Driver d;
  d.Warm();
  const Window w = d.NextWindow(22.7, 0);
  EXPECT_NEAR(w.cap, 44, 0.1);
  EXPECT_NEAR(w.fps, 44, 0.5);
}

TEST(OrcaPerf, ReRunsCountAsTheMachinesWork)
{
  // A rollback re-runs 4 frames at 3 ms each between two forward frames: the next forward frame's
  // interval holds them, and so does its work.
  Driver d;
  d.Warm();
  d.Frame(16.7, 6.7);
  for (int i = 0; i < 4; ++i)
    d.Rerun(3);
  // 12 ms of re-runs, then the frame itself: 16.7 ms more, 6.7 of it asleep.
  d.Frame(16.7, 6.7);
  // One long, mostly asleep frame closes the window: three frames that worked 10, 22 and 10 ms.
  const Window w = d.NextWindow(1000, 1000 - 10);
  EXPECT_EQ(w.n, 3);
  EXPECT_NEAR(w.w95, 22, 0.01);
  EXPECT_NEAR(w.w50, 10, 0.01);
  EXPECT_NEAR(w.cap, 3 / 0.042, 0.1);
  EXPECT_EQ(w.rbf, 4);
  // Split at the last re-run boundary: 12 ms of re-run work over 4 frames, and three forward
  // frames of 10 ms each.
  EXPECT_EQ(w.rv, 4);
  EXPECT_NEAR(w.rw, 3, 0.01);
  EXPECT_NEAR(w.fc, 100, 0.1);
}

TEST(OrcaPerf, ReRunWorkLeavesItsWaitsOut)
{
  // A stall before the rollback (8 ms) is a wait, not re-run work; the re-runs themselves are 2 ms
  // each.
  Driver d;
  d.Warm();
  d.Frame(16.7, 6.7);
  d.Wait(8);
  for (int i = 0; i < 3; ++i)
    d.Rerun(2);
  d.Frame(10, 0);
  const Window w = d.NextWindow(1000, 1000 - 10);
  EXPECT_EQ(w.rv, 3);
  EXPECT_NEAR(w.rw, 2, 0.01);
  // Forward work: 10, 10 and 10 ms.
  EXPECT_NEAR(w.fc, 100, 0.1);
  // All of it: 10, 16 and 10 ms.
  EXPECT_NEAR(w.cap, 3 / 0.036, 0.1);
}

TEST(OrcaPerf, ReRunsOfAFrameThatDoesntCountAreLeftOutToo)
{
  // Re-runs before a frame that a pause spoiled count toward rbf (the window's) but not rv.
  Driver d;
  d.Warm();
  d.Frame(16.7, 6.7);
  d.Rerun(3);
  d.Pause(100);
  d.Frame(16.7, 6.7);
  const Window w = d.NextWindow(1000, 1000 - 10);
  EXPECT_EQ(w.rbf, 1);
  EXPECT_EQ(w.rv, 0);
  EXPECT_DOUBLE_EQ(w.rw, 0);
}

TEST(OrcaPerf, SessionWaitsAreNotWork)
{
  // Time sync held this machine a frame: its 33 ms frame was 16.7 ms of waiting and 6.7 asleep.
  Driver d;
  d.Warm();
  for (int i = 0; i < 50; ++i)
    d.Frame(16.7, 6.7);
  d.Frame(33.4, 6.7, 16.7);
  const Window w = d.NextWindow(16.7, 6.7);
  // Counted as work, the wait would bring the cap down to about 97.
  EXPECT_NEAR(w.cap, 100, 0.2);
  EXPECT_NEAR(w.w95, 10, 0.01);
}

TEST(OrcaPerf, GpuShareIsReported)
{
  Driver d;
  d.Warm();
  std::optional<Window> w;
  while (!w)
    w = d.Frame(16.7, 2, 0, 9);
  EXPECT_NEAR(w->g95, 9, 0.01);
  EXPECT_NEAR(w->w95, 14.7, 0.01);
}

TEST(OrcaPerf, MenusAndTheMatchStartAreLeftOut)
{
  Driver d;
  // Menus: no frame counts, but the window still comes out every second.
  d.Scene(false);
  Window w = d.NextWindow(10);
  EXPECT_EQ(w.n, 0);
  EXPECT_EQ(w.cap, 0);
  EXPECT_GT(w.fps, 90);
  // A match starts: its first warm_frames frames don't count either.
  d.Scene(true);
  for (int i = 0; i < 10; ++i)
    d.Frame(10);
  w = d.NextWindow(10);
  EXPECT_GT(w.n, 0);
  const int first = w.n;
  // Back to a menu: the count starts over at the next match.
  d.Scene(false);
  d.Frame(10);
  d.Scene(true);
  w = d.NextWindow(10);
  EXPECT_LT(w.n, first);
  // The scene unreadable (a transition) changes nothing.
  d.Scene(std::nullopt);
  w = d.NextWindow(10);
  EXPECT_EQ(w.n, 100);
}

TEST(OrcaPerf, PausesHidingHoldsAndUnseenFramesAreLeftOut)
{
  Driver d;
  d.Warm();
  d.NextWindow(10);
  // A pause: the frame across it doesn't count, and neither does its time.
  d.Pause(5000);
  std::optional<Window> w = d.Frame(10);
  EXPECT_FALSE(w);
  w = d.NextWindow(10);
  EXPECT_EQ(w->n, 99);
  EXPECT_NEAR(w->fps, 100, 0.5);

  Boundary hidden = Driver::Forward();
  hidden.hidden = true;
  w = d.NextWindow(10, 0, hidden);
  EXPECT_EQ(w->n, 0);
  // The game still ran at speed while hidden.
  EXPECT_NEAR(w->fps, 100, 0.5);

  // A load the game held for 2 more frames: shown frames count it, the cap doesn't.
  Boundary held = Driver::Forward();
  held.held = true;
  held.held_frames = 2;
  d.Frame(50, 0, 0, 0, held);
  w = d.NextWindow(10);
  EXPECT_EQ(w->n, 95);

  Boundary unseen = Driver::Forward();
  unseen.excluded = true;
  w = d.NextWindow(1, 0, unseen);
  EXPECT_EQ(w->n, 0);
  EXPECT_EQ(w->fps, 0);
}

TEST(OrcaPerf, ACatchUpSpoilsTheNextFrame)
{
  Driver d;
  d.Warm();
  d.NextWindow(10);
  // A joiner's catch-up runs frames back to back (not first runs, excluded): the next forward
  // frame's interval spans it, so that frame doesn't count.
  for (int i = 0; i < 30; ++i)
    d.Rerun(1, true);
  const Window w = d.NextWindow(10);
  EXPECT_EQ(w.n, 96);
  EXPECT_NEAR(w.cap, 100, 0.5);
}

TEST(OrcaPerf, ModeFollowsThePeers)
{
  EXPECT_EQ(Orca::Perf::ModeFor(0), Mode::Solo);
  EXPECT_EQ(Orca::Perf::ModeFor(1), Mode::OneVsOne);
  EXPECT_EQ(Orca::Perf::ModeFor(2), Mode::Multi);
  EXPECT_EQ(Orca::Perf::ModeFor(3), Mode::Multi);
  Driver d;
  d.Warm();
  Boundary friend_in = Driver::Forward();
  friend_in.peers = 1;
  d.Frame(10, 0, 0, 0, friend_in);
  // The most peers seen in the window.
  EXPECT_EQ(d.NextWindow(10).mode, Mode::OneVsOne);
  EXPECT_EQ(d.NextWindow(10).mode, Mode::Solo);
}

TEST(OrcaPerf, AWindowCountsWithEnoughFramesSoloOr1v1)
{
  const Rules rules;
  EXPECT_TRUE(Win(50, 600, Mode::Solo).Counts(rules));
  EXPECT_TRUE(Win(50, 858, Mode::OneVsOne).Counts(rules));
  EXPECT_FALSE(Win(50, 599, Mode::OneVsOne).Counts(rules));
  // 3-4 player rooms dip normally.
  EXPECT_FALSE(Win(50, 900, Mode::Multi).Counts(rules));
}

TEST(OrcaPerf, TheSwitchFiresAfterFourSlowWindowsInARow)
{
  AutoSwitch s(TestRules());
  EXPECT_FALSE(s.Feed(Win(44)));
  EXPECT_FALSE(s.Feed(Win(43)));
  EXPECT_FALSE(s.Feed(Win(45)));
  EXPECT_TRUE(s.Feed(Win(46)));
  EXPECT_TRUE(s.Fired());
  EXPECT_DOUBLE_EQ(s.TriggerCap(), 44.5);
  // Once a run.
  for (int i = 0; i < 8; ++i)
    EXPECT_FALSE(s.Feed(Win(40)));
}

TEST(OrcaPerf, MenusAndBigRoomsDontBreakTheRun)
{
  AutoSwitch s(TestRules());
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(0, 0)));  // a menu
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(70, 50, Mode::Multi)));  // a 4-player room, fast or slow
  EXPECT_FALSE(s.Feed(Win(30, 50, Mode::Multi)));
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(50, 39)));  // too few frames
  EXPECT_TRUE(s.Feed(Win(50)));
}

TEST(OrcaPerf, AFastWindowPerformanceOrAChangeStartsOver)
{
  AutoSwitch s(TestRules());
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Feed(Win(50));
  EXPECT_FALSE(s.Feed(Win(58.0)));  // holds 58: not slow
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Feed(Win(50));
  EXPECT_FALSE(s.Feed(Win(50, 50, Mode::Solo, true)));  // already Performance
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Feed(Win(50));
  s.Restart();  // the player changed a setting
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_FALSE(s.Feed(Win(50)));
  EXPECT_TRUE(s.Feed(Win(50)));
}

TEST(OrcaPerf, AFineMachineWithDipsNeverSwitches)
{
  // 2f80e4c8's shape: 58.6 most of the time, 49 in rollback-heavy windows, never four in a row.
  AutoSwitch s;
  for (int i = 0; i < 40; ++i)
  {
    const double cap = i % 4 == 3 ? 58.6 : (i % 2 ? 49.1 : 57.2);
    EXPECT_FALSE(s.Feed(Win(cap, 858))) << i;
  }
  // A normal machine at 60 with one dip.
  AutoSwitch fine;
  for (int i = 0; i < 40; ++i)
    EXPECT_FALSE(fine.Feed(Win(i == 20 ? 51 : 72, 858)));
}

// A window as the `win` line has it: n, cap, fc, rw and rv, 1v1, Performance off.
Window Line(int n, double cap, double fc, double rw, int rv)
{
  Window window = Win(cap, n);
  window.fc = fc;
  window.rw = rw;
  window.rv = rv;
  return window;
}

TEST(OrcaPerf, TheSwitchReadsTheReferenceRerunLoad)
{
  const Rules rules;
  // Forward work per frame plus 0.1 re-run frames at the measured cost: 1000 / (1000/64.9 + 0.93).
  EXPECT_NEAR(Orca::Perf::CapAtRef(Line(787, 53.6, 64.9, 9.3, 275), rules), 61.2, 0.05);
  // Too few re-runs to trust their cost: the last cost measured, else forward speed alone.
  EXPECT_FALSE(Orca::Perf::RerunCost(Line(800, 60, 64.9, 40, 29), rules).has_value());
  EXPECT_NEAR(Orca::Perf::CapAtRef(Line(800, 60, 64.9, 40, 29), rules, 9.3), 61.2, 0.05);
  EXPECT_DOUBLE_EQ(Orca::Perf::CapAtRef(Line(800, 60, 64.9, 40, 29), rules), 64.9);
  // Never above forward speed, and a window without one reads its all-in cap.
  EXPECT_DOUBLE_EQ(Orca::Perf::CapAtRef(Line(800, 70, 64.9, 0, 40), rules), 64.9);
  EXPECT_DOUBLE_EQ(Orca::Perf::CapAtRef(Win(51), rules), 51);
}

TEST(OrcaPerf, ANormalPcAtOnlineLatencyNeverSwitches)
{
  // The counted windows of the 0.3.39 candidate's 11 PC runs on 2026-10-10 (i7-8700, RTX 2080,
  // 40 +- 8 ms each way, 0.35-0.40 re-run frames per frame) up to where the old rule switched: it
  // read the raw cap (44-58, mostly under 58) and switched in every run. Read at the page's
  // reference re-run load, the same windows are 48-72 and never four in a row under 58.
  const std::vector<std::vector<Window>> runs = {
      {Line(788, 54.6, 69.5, 14.5, 214), Line(784, 58.4, 72.0, 8.4, 303),
       Line(738, 51.8, 66.6, 13.4, 236), Line(756, 54.1, 65.2, 9.6, 249),
       Line(805, 56.6, 75.0, 10.9, 322), Line(786, 52.9, 65.7, 9.5, 305)},
      {Line(787, 54.9, 68.1, 8.3, 335), Line(778, 57.6, 77.5, 11.2, 309),
       Line(737, 51.8, 61.2, 9.1, 242), Line(798, 56.2, 67.4, 9.3, 252)},
      {Line(792, 55.9, 69.8, 9.4, 299), Line(751, 53.9, 67.1, 8.2, 333),
       Line(772, 55.0, 64.7, 9.6, 220), Line(790, 51.1, 66.6, 12.2, 293)},
      {Line(803, 55.7, 73.0, 11.9, 288), Line(798, 53.2, 70.9, 11.8, 315),
       Line(787, 55.5, 66.8, 10.1, 238), Line(803, 56.1, 69.0, 9.6, 281)},
      {Line(708, 49.1, 62.7, 12.9, 243), Line(801, 57.9, 71.1, 8.4, 304),
       Line(723, 48.9, 57.6, 9.2, 244), Line(799, 56.0, 73.5, 11.6, 293)},
      {Line(714, 49.1, 58.9, 10.0, 241), Line(800, 57.3, 71.0, 8.7, 307),
       Line(748, 49.2, 57.9, 10.8, 210), Line(787, 53.6, 64.9, 9.3, 275)},
      {Line(780, 55.4, 68.1, 8.3, 318), Line(806, 56.7, 72.2, 9.2, 332),
       Line(736, 50.2, 59.9, 9.2, 257), Line(754, 53.7, 66.7, 10.7, 254)},
      {Line(779, 53.0, 63.8, 10.5, 237), Line(779, 54.2, 68.6, 9.8, 307),
       Line(718, 48.7, 61.8, 13.8, 227), Line(754, 51.1, 65.2, 13.9, 229)},
      {Line(637, 43.9, 50.1, 8.7, 210), Line(789, 55.1, 70.0, 9.4, 323),
       Line(745, 50.2, 70.6, 16.4, 263), Line(786, 52.6, 61.8, 9.4, 236)},
      {Line(768, 53.8, 69.4, 10.6, 302), Line(743, 52.9, 70.5, 11.7, 301),
       Line(768, 52.1, 70.7, 13.9, 278), Line(765, 52.7, 62.7, 9.4, 246)},
      {Line(741, 52.2, 63.5, 9.0, 282), Line(801, 56.6, 78.8, 11.5, 348),
       Line(777, 53.9, 63.4, 9.1, 235), Line(709, 48.5, 56.7, 9.2, 230)},
  };
  AutoSwitch all;  // one long session of all of them
  for (std::size_t r = 0; r < runs.size(); ++r)
  {
    int raw_slow = 0;
    AutoSwitch s;
    for (const Window& w : runs[r])
    {
      ASSERT_TRUE(w.Counts(Rules{}));
      raw_slow += w.cap < 58.0 ? 1 : 0;
      EXPECT_FALSE(s.Feed(w)) << "run " << r;
      EXPECT_FALSE(all.Feed(w)) << "run " << r;
    }
    // The raw cap would have switched: four in a row under 58.
    EXPECT_GE(raw_slow, 4) << "run " << r;
  }
  EXPECT_FALSE(all.Fired());
}

TEST(OrcaPerf, ASlowMachineSwitchesWhateverItsRerunLoad)
{
  // 48 forward frames a second: slow at any re-run load. Rollback-heavy, its reference speed is 45.8.
  AutoSwitch busy;
  for (int i = 0; i < 3; ++i)
    EXPECT_FALSE(busy.Feed(Line(700, 40.1, 48, 10, 250)));
  EXPECT_TRUE(busy.Feed(Line(700, 40.1, 48, 10, 250)));
  EXPECT_NEAR(busy.TriggerCap(), 45.8, 0.05);
  // On a calm link (too few re-runs to cost, none measured before) it reads its forward speed.
  AutoSwitch calm;
  for (int i = 0; i < 3; ++i)
    EXPECT_FALSE(calm.Feed(Line(700, 47.5, 48, 0, 4)));
  EXPECT_TRUE(calm.Feed(Line(700, 47.5, 48, 0, 4)));
  EXPECT_DOUBLE_EQ(calm.TriggerCap(), 48);
  // A calm window after a busy one uses the cost measured then: 1000 / (1000/60 + 0.1 * 30) = 50.8.
  AutoSwitch costly;
  EXPECT_FALSE(costly.Feed(Line(700, 40, 60, 30, 200)));
  for (int i = 0; i < 2; ++i)
    EXPECT_FALSE(costly.Feed(Line(700, 59.5, 60, 0, 2)));
  EXPECT_TRUE(costly.Feed(Line(700, 59.5, 60, 0, 2)));
  EXPECT_NEAR(costly.TriggerCap(), 50.8, 0.05);
}

TEST(OrcaPerf, ASlowMachineSwitchesInItsFirstMatch)
{
  // a3851fbb: 44 fps in 1v1, with the real rules, from the moment a match starts. Its 900 warm-up
  // frames take 20.5 s, so the first window (0-15 s) has no valid frame and the second (15-30 s)
  // too few (420) to count; windows 3 to 6 count, and the switch fires at the end of the sixth:
  // 90 s into the match, after 60 s of counted play.
  Driver d(Rules{});
  d.Scene(true);
  AutoSwitch s;
  std::vector<Window> windows;
  Boundary b = Driver::Forward();
  b.peers = 1;
  bool fired = false;
  while (!fired && windows.size() < 10)
  {
    windows.push_back(d.NextWindow(1000 / 44.0, 0, b));
    fired = s.Feed(windows.back());
  }
  EXPECT_TRUE(fired);
  ASSERT_EQ(windows.size(), 6u);
  EXPECT_EQ(windows[0].n, 0);
  EXPECT_FALSE(windows[1].Counts(Rules{}));
  for (std::size_t i = 2; i < windows.size(); ++i)
    EXPECT_TRUE(windows[i].Counts(Rules{})) << i;
  EXPECT_NEAR(s.TriggerCap(), 44, 0.1);
}

TEST(OrcaPerf, TheLinesAreVersionedJson)
{
  Window w;
  w.mode = Mode::OneVsOne;
  w.n = 858;
  w.cap = 51.66;
  w.fc = 58.04;
  w.rw = 12.42;
  w.rv = 180;
  w.w50 = 18.94;
  w.w95 = 24.1;
  w.g95 = 3.2;
  w.fps = 51.6;
  w.rbf = 212;
  w.res = 1;
  w.pf = true;
  w.as = true;
  EXPECT_EQ(Orca::Perf::WindowJson(w),
            R"({"v":1,"k":"win","m":"1v1","n":858,"cap":51.7,"fc":58.0,"rw":12.4,"rv":180,)"
            R"("w50":18.9,"w95":24.1,"g95":3.2,"fps":51.6,"rbf":212,"res":1,"pf":1,"as":1})");
  // A test switch marks every window.
  w.test = true;
  EXPECT_NE(Orca::Perf::WindowJson(w).find(R"("as":1,"t":1})"), std::string::npos);
  EXPECT_EQ(Orca::Perf::AutoJson(44.25), R"({"v":1,"k":"auto","cap":44.3})");
  Window empty;
  empty.cap = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Orca::Perf::WindowJson(empty),
            R"({"v":1,"k":"win","m":"solo","n":0,"cap":0.0,"fc":0.0,"rw":0.0,"rv":0,"w50":0.0,)"
            R"("w95":0.0,"g95":0.0,"fps":0.0,"rbf":0,"res":0,"pf":0,"as":0})");
  // The longest a window line can be still fits the app's 300 characters.
  Window big;
  big.mode = Mode::Multi;
  big.n = 1'000'000;
  big.cap = big.fc = 999.9;
  big.rw = 1e9;
  big.rv = 1'000'000'000;
  big.w50 = big.w95 = big.g95 = 99'999.9;
  big.fps = 999.9;
  big.rbf = 1'000'000;
  big.res = 12;
  big.pf = big.as = big.test = true;
  EXPECT_LE(Orca::Perf::WindowJson(big).size() + std::string("orca perf ").size(),
            Orca::Perf::LINE_BYTES);
}

TEST(OrcaPerf, TheInfoLineFitsThreeHundredBytes)
{
  Orca::Perf::Info info;
  info.backend = "D3D";
  info.gpu = "NVIDIA GeForce RTX 3060";
  info.adapters = {"NVIDIA GeForce RTX 3060", "Intel(R) UHD Graphics 770"};
  info.adapter = 0;
  info.cpu = "12th Gen Intel(R) Core(TM) i5-12400F";
  info.threads = 12;
  info.os = "win";
  info.rev = "ccdc8e441a";
  EXPECT_EQ(
      Orca::Perf::InfoJson(info),
      R"({"v":1,"k":"info","be":"D3D","gpu":"NVIDIA GeForce RTX 3060",)"
      R"("ad":["NVIDIA GeForce RTX 3060","Intel(R) UHD Graphics 770"],"ai":0,)"
      R"("cpu":"12th Gen Intel(R) Core(TM) i5-12400F","th":12,"os":"win","rev":"ccdc8e441a"})");

  // The driver version, where the backend read one, right after the card.
  info.driver = "32.0.15.6094";
  EXPECT_EQ(
      Orca::Perf::InfoJson(info),
      R"({"v":1,"k":"info","be":"D3D","gpu":"NVIDIA GeForce RTX 3060","drv":"32.0.15.6094",)"
      R"("ad":["NVIDIA GeForce RTX 3060","Intel(R) UHD Graphics 770"],"ai":0,)"
      R"("cpu":"12th Gen Intel(R) Core(TM) i5-12400F","th":12,"os":"win","rev":"ccdc8e441a"})");

  // Long names: cut to 40, then the CPU goes, then adapters past the first.
  const std::string long_name(80, 'x');
  info.gpu = long_name;
  info.adapters = {long_name, long_name, long_name, long_name};
  info.cpu = long_name;
  const std::string json = Orca::Perf::InfoJson(info);
  EXPECT_LE(json.size() + std::string("orca perf ").size(), Orca::Perf::LINE_BYTES);
  EXPECT_EQ(json.find("\"cpu\""), std::string::npos);
  EXPECT_NE(json.find(std::string(40, 'x')), std::string::npos);
  EXPECT_EQ(json.find(std::string(41, 'x')), std::string::npos);
}

TEST(OrcaPerf, NamesAreCleaned)
{
  EXPECT_EQ(Orca::Perf::CleanName("  AMD \"Radeon\"\\  RX\t580  "), "AMD Radeon RX580");
  EXPECT_EQ(Orca::Perf::CleanName("Apple M2 Pro"), "Apple M2 Pro");
  EXPECT_EQ(Orca::Perf::CleanName("caf\xc3\xa9"), "caf");
  EXPECT_EQ(Orca::Perf::CleanName(std::string(50, 'a') + " b").size(), 40u);
}

TEST(OrcaPerf, GfxTakesOnlyItsOwnKeysAndRanges)
{
  using Orca::Perf::ParseGfx;
  EXPECT_EQ(ParseGfx("gfx res 0"), (GfxChange{GfxKey::Res, 0}));
  EXPECT_EQ(ParseGfx("gfx res 4"), (GfxChange{GfxKey::Res, 4}));
  EXPECT_EQ(ParseGfx("gfx aniso -1"), (GfxChange{GfxKey::Aniso, -1}));
  EXPECT_EQ(ParseGfx("gfx aniso 4"), (GfxChange{GfxKey::Aniso, 4}));
  EXPECT_EQ(ParseGfx("gfx filter 2"), (GfxChange{GfxKey::Filter, 2}));
  EXPECT_EQ(ParseGfx("gfx resample 6"), (GfxChange{GfxKey::Resample, 6}));
  EXPECT_EQ(ParseGfx("gfx efb 0"), (GfxChange{GfxKey::Efb, 0}));
  EXPECT_EQ(ParseGfx("gfx gpudecode 1"), (GfxChange{GfxKey::GpuDecode, 1}));
  EXPECT_EQ(ParseGfx("gfx cpucull 1"), (GfxChange{GfxKey::CpuCull, 1}));
  for (const char* bad : {"gfx wireframe 1",
                          "gfx res 9",
                          "gfx res 5",
                          "gfx msaa 4",
                          "gfx res 1 x",
                          "gfx res",
                          "gfx",
                          "gfx aniso -2",
                          "gfx filter 3",
                          "gfx resample 7",
                          "gfx efb 2",
                          "gfx res +1",
                          "gfx res 1.0",
                          "gfx res -0x1",
                          "gfx Res 1",
                          "gfx shadermode 0",
                          "gfx fog 1",
                          "gfx widescreen 1",
                          "gfx postshader x",
                          "gfxres 1",
                          "res 1",
                          "gfx res 1000"})
  {
    EXPECT_FALSE(ParseGfx(bad)) << bad;
  }
}

TEST(OrcaPerf, ThePerformancePresetIsNativeWithoutScaledCopies)
{
  const auto preset = Orca::Perf::PerformancePreset();
  EXPECT_EQ(preset[0], (GfxChange{GfxKey::Res, 1}));
  EXPECT_EQ(preset[1], (GfxChange{GfxKey::Efb, 0}));
  EXPECT_EQ(preset[2], (GfxChange{GfxKey::Aniso, -1}));
  EXPECT_EQ(preset[3], (GfxChange{GfxKey::Filter, 0}));
  EXPECT_EQ(preset[4], (GfxChange{GfxKey::Resample, 0}));
}
