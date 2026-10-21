// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Activity.h"
#include "Core/Orca/UX/Controllers.h"

namespace
{
using LineClock = Orca::Activity::Clock;
using Orca::Activity::Edges;
using Orca::Activity::Line;
using Orca::Activity::Mode;
using Orca::Activity::Pad;
using Orca::Activity::Reader;

constexpr s64 S = 1000;  // ms

GCPadStatus Neutral()
{
  GCPadStatus pad;
  pad.isConnected = true;
  return pad;
}

GCPadStatus WithButtons(u16 buttons)
{
  GCPadStatus pad = Neutral();
  pad.button = buttons;
  return pad;
}

GCPadStatus WithStick(int x, int y)
{
  GCPadStatus pad = Neutral();
  pad.stickX = static_cast<u8>(std::clamp(x, 0, 255));
  pad.stickY = static_cast<u8>(std::clamp(y, 0, 255));
  return pad;
}

GCPadStatus WithTrigger(int left, bool click = false)
{
  GCPadStatus pad = Neutral();
  pad.triggerLeft = static_cast<u8>(std::clamp(left, 0, 255));
  if (click)
    pad.button |= PAD_TRIGGER_L;
  return pad;
}

// Edges fed one pad after another, summed (the first pad is the baseline).
int SumEdges(const std::vector<GCPadStatus>& pads)
{
  Edges edges;
  int total = 0;
  for (const GCPadStatus& pad : pads)
    total += edges.Feed(pad);
  return total;
}
}  // namespace

// ---- Edges ----

TEST(OrcaActivity, PressAndReleaseAreTwoEdges)
{
  Edges e;
  EXPECT_EQ(e.Feed(Neutral()), 0);
  EXPECT_EQ(e.Feed(WithButtons(PAD_BUTTON_A)), 1);
  EXPECT_EQ(e.Feed(WithButtons(PAD_BUTTON_A)), 0);  // held
  EXPECT_EQ(e.Feed(Neutral()), 1);
  // Two buttons down at once, then one up and another down.
  EXPECT_EQ(e.Feed(WithButtons(PAD_BUTTON_B | PAD_BUTTON_UP)), 2);
  EXPECT_EQ(e.Feed(WithButtons(PAD_BUTTON_B | PAD_TRIGGER_Z)), 2);
  EXPECT_EQ(e.Feed(WithButtons(PAD_BUTTON_START | PAD_BUTTON_X | PAD_BUTTON_Y)), 5);
}

TEST(OrcaActivity, FirstPadIsTheBaseline)
{
  // A button already held when a controller is first seen is no press; letting it go is a release.
  Edges e;
  EXPECT_EQ(e.Feed(WithButtons(PAD_BUTTON_A)), 0);
  EXPECT_EQ(e.Feed(Neutral()), 1);
  Edges stick;
  EXPECT_EQ(stick.Feed(WithStick(255, 128)), 0);
  EXPECT_EQ(stick.Feed(WithStick(255, 128)), 0);
}

TEST(OrcaActivity, StatusBitsAndAnalogFaceButtonsAreNotInput)
{
  GCPadStatus a = Neutral();
  GCPadStatus b = Neutral();
  b.button = PAD_GET_ORIGIN | PAD_USE_ORIGIN | PAD_ERR_STATUS;
  b.analogA = 200;
  b.analogB = 90;
  EXPECT_EQ(SumEdges({a, b, a, b, a}), 0);
}

TEST(OrcaActivity, TriggerThresholdWithHysteresis)
{
  Edges e;
  EXPECT_EQ(e.Feed(WithTrigger(0)), 0);
  // Resting and light touches below the press point: nothing.
  for (int v : {10, 40, 90, 127, 100, 70})
    EXPECT_EQ(e.Feed(WithTrigger(v)), 0) << v;
  EXPECT_EQ(e.Feed(WithTrigger(128)), 1);  // pressed
  // Wobbling around the press point while held: nothing.
  for (int v : {127, 129, 120, 135, 90, 65, 140})
    EXPECT_EQ(e.Feed(WithTrigger(v)), 0) << v;
  EXPECT_EQ(e.Feed(WithTrigger(64)), 1);  // let go
  for (int v : {66, 63, 100, 127})
    EXPECT_EQ(e.Feed(WithTrigger(v)), 0) << v;
}

TEST(OrcaActivity, TriggerClickCountsOnceWithItsAnalog)
{
  // A GameCube trigger's click comes with the analog at the bottom; one press, one release.
  Edges e;
  EXPECT_EQ(e.Feed(WithTrigger(0)), 0);
  EXPECT_EQ(e.Feed(WithTrigger(200)), 1);
  EXPECT_EQ(e.Feed(WithTrigger(255, true)), 0);
  EXPECT_EQ(e.Feed(WithTrigger(200)), 0);
  EXPECT_EQ(e.Feed(WithTrigger(0)), 1);
  // A digital-only trigger (keyboard, a script): the click alone.
  EXPECT_EQ(e.Feed(WithTrigger(0, true)), 1);
  EXPECT_EQ(e.Feed(WithTrigger(0)), 1);
  // The right trigger is its own.
  GCPadStatus r = Neutral();
  r.triggerRight = 255;
  r.button = PAD_TRIGGER_R;
  EXPECT_EQ(e.Feed(r), 1);
}

TEST(OrcaActivity, StickDeadzoneCrossingWithHysteresis)
{
  Edges e;
  EXPECT_EQ(e.Feed(WithStick(128, 128)), 0);
  // Inside the centre: nothing, however it wanders.
  for (int d : {5, -12, 20, 27, -27, 40, 47})
    EXPECT_EQ(e.Feed(WithStick(128 + d, 128)), 0) << d;
  EXPECT_EQ(e.Feed(WithStick(128 + 48, 128)), 1);  // out
  // Wobbling on the edge and anywhere outside: nothing.
  for (int d : {47, 49, 30, 100, 127, 29})
    EXPECT_EQ(e.Feed(WithStick(128 + d, 128)), 0) << d;
  EXPECT_EQ(e.Feed(WithStick(128 + 27, 128)), 1);  // back in
  // Diagonals use the distance from the centre.
  EXPECT_EQ(e.Feed(WithStick(128 + 35, 128 - 35)), 1);  // 49.5 out
  EXPECT_EQ(e.Feed(WithStick(128 + 19, 128 + 19)), 1);  // 26.9 in
  // The C-stick is its own.
  GCPadStatus c = Neutral();
  c.substickY = 0;
  EXPECT_EQ(e.Feed(c), 1);
  EXPECT_EQ(e.Feed(Neutral()), 1);
}

TEST(OrcaActivity, DriftAndHeldSticksNeverCount)
{
  // A stick held still anywhere, or drifting slowly, or held out with the hand's tremor.
  std::vector<GCPadStatus> pads;
  for (int i = 0; i < 2000; ++i)
    pads.push_back(WithStick(128 + 38 + (i % 9) - 4, 128 + 3));  // parked in the band
  EXPECT_EQ(SumEdges(pads), 0);
  pads.clear();
  for (int i = 0; i < 2000; ++i)
    pads.push_back(WithStick(128 + 90 + (i % 21) - 10, 128 - 20 + (i % 7)));  // held out
  EXPECT_EQ(SumEdges(pads), 0);
  pads.clear();
  for (int i = 0; i < 2000; ++i)
    pads.push_back(WithStick(128 + i / 100, 128));  // drifting from 0 to 19
  EXPECT_EQ(SumEdges(pads), 0);
}

TEST(OrcaActivity, GamepadNoiseNeverCounts)
{
  // A DualSense's stream through the app: every axis and both analog triggers jitter a few percent
  // around rest (and a worn stick sits off centre), with nothing pressed. Mapped the way the local
  // pad is, for a minute of frames.
  std::mt19937 rng(12345);
  std::normal_distribution<float> jitter(0.0f, 0.02f);
  Edges e;
  int edges = 0;
  for (int frame = 0; frame < 3600; ++frame)
  {
    Orca::UX::StandardPad pad;
    pad.axes = {0.12f + jitter(rng), -0.08f + jitter(rng), jitter(rng), jitter(rng)};
    pad.buttons[6] = std::max(0.0f, 0.03f + jitter(rng));  // L2
    pad.buttons[7] = std::max(0.0f, 0.03f + jitter(rng));  // R2
    edges += e.Feed(Orca::UX::MapStandardPad(pad));
  }
  EXPECT_EQ(edges, 0);
}

TEST(OrcaActivity, RealPlayOnAGamepadCounts)
{
  Orca::UX::StandardPad pad;
  Edges e;
  EXPECT_EQ(e.Feed(Orca::UX::MapStandardPad(pad)), 0);
  pad.buttons[0] = 1;
  pad.pressed[0] = true;  // A
  EXPECT_EQ(e.Feed(Orca::UX::MapStandardPad(pad)), 1);
  pad.axes[0] = -1;  // stick left
  EXPECT_EQ(e.Feed(Orca::UX::MapStandardPad(pad)), 1);
  pad.buttons[7] = 0.8f;  // R2 most of the way
  EXPECT_EQ(e.Feed(Orca::UX::MapStandardPad(pad)), 1);
}

// ---- Mode ----

TEST(OrcaActivity, ModeMapping)
{
  using Orca::Activity::DecideMode;
  EXPECT_EQ(DecideMode("private", false, false), Mode::Solo);
  EXPECT_EQ(DecideMode("private", false, true), Mode::Training);
  EXPECT_EQ(DecideMode("private", true, false), Mode::Friends);
  // A friend held at the door of a Training game isn't plugged in; one who is makes it friends.
  EXPECT_EQ(DecideMode("private", true, true), Mode::Friends);
  EXPECT_EQ(DecideMode("casual", false, false), Mode::Casual);
  EXPECT_EQ(DecideMode("casual", true, false), Mode::Casual);
  EXPECT_EQ(DecideMode("ranked", true, false), Mode::Ranked);
  EXPECT_EQ(DecideMode("ranked", false, true), Mode::Ranked);
  EXPECT_EQ(DecideMode("", false, false), Mode::Solo);
  EXPECT_EQ(DecideMode("something-new", true, false), Mode::Friends);
  EXPECT_EQ(Orca::Activity::ModeName(Mode::Solo), "solo");
  EXPECT_EQ(Orca::Activity::ModeName(Mode::Training), "training");
  EXPECT_EQ(Orca::Activity::ModeName(Mode::Friends), "friends");
  EXPECT_EQ(Orca::Activity::ModeName(Mode::Casual), "casual");
  EXPECT_EQ(Orca::Activity::ModeName(Mode::Ranked), "ranked");
}

// ---- Clock ----

TEST(OrcaActivity, NoInputIsNoActivity)
{
  LineClock c(5 * S, Mode::Solo);
  EXPECT_EQ(c.DueAt(), 65 * S);
  const Line line = c.Take(65 * S);
  EXPECT_EQ(line.interval_ms, 60 * S);
  EXPECT_EQ(line.active_ms, 0);
  EXPECT_EQ(line.inputs, 0u);
  EXPECT_EQ(line.pads, 0u);
  EXPECT_EQ(line.mode, Mode::Solo);
  EXPECT_EQ(Orca::Activity::FormatLine(line), "orca active 60 0 0 solo 0");
}

TEST(OrcaActivity, ThirtySecondsAfterAnInput)
{
  LineClock c(0, Mode::Solo);
  c.Input(10 * S, 1, 1);
  const Line line = c.Take(60 * S);
  EXPECT_EQ(line.active_ms, 30 * S);
  EXPECT_EQ(line.inputs, 1u);
  EXPECT_EQ(line.pads, 1u);
}

TEST(OrcaActivity, OverlappingWindowsMerge)
{
  LineClock c(0, Mode::Solo);
  c.Input(0, 2, 1);
  c.Input(10 * S, 1, 1);
  c.Input(20 * S, 3, 1);
  // 0 to 50 s.
  const Line line = c.Take(60 * S);
  EXPECT_EQ(line.active_ms, 50 * S);
  EXPECT_EQ(line.inputs, 6u);
  EXPECT_EQ(line.pads, 1u);
}

TEST(OrcaActivity, WindowCarriesAcrossTheIntervalBoundary)
{
  LineClock c(0, Mode::Solo);
  c.Input(50 * S, 1, 1);
  Line first = c.Take(60 * S);
  EXPECT_EQ(first.active_ms, 10 * S);
  EXPECT_EQ(c.DueAt(), 120 * S);
  // No input in the second interval: the 20 s left of the window still count, and inputs are 0.
  Line second = c.Take(120 * S);
  EXPECT_EQ(second.interval_ms, 60 * S);
  EXPECT_EQ(second.active_ms, 20 * S);
  EXPECT_EQ(second.inputs, 0u);
  EXPECT_EQ(second.pads, 0u);
  Line third = c.Take(180 * S);
  EXPECT_EQ(third.active_ms, 0);
}

TEST(OrcaActivity, InputExactlyAtTheBoundary)
{
  LineClock c(0, Mode::Solo);
  c.Input(60 * S, 1, 1);  // the last instant of the first interval
  EXPECT_EQ(c.Take(60 * S).active_ms, 0);
  EXPECT_EQ(c.Take(120 * S).active_ms, 30 * S);
}

TEST(OrcaActivity, PartialLastInterval)
{
  LineClock c(0, Mode::Solo);
  c.Input(55 * S, 1, 1);
  EXPECT_EQ(c.Take(60 * S).active_ms, 5 * S);
  c.Input(70 * S, 1, 1);
  // The game stops at 75 s: 15 s covered, all of it within 30 s of an input.
  const Line last = c.Take(75 * S);
  EXPECT_EQ(last.interval_ms, 15 * S);
  EXPECT_EQ(last.active_ms, 15 * S);
  EXPECT_EQ(Orca::Activity::FormatLine(last), "orca active 15 15 1 solo 1");
  EXPECT_FALSE(Orca::Activity::IsEmpty(last));
  // Stopping right after a line: an empty last line, which is left out.
  LineClock d(0, Mode::Solo);
  d.Take(60 * S);
  EXPECT_TRUE(Orca::Activity::IsEmpty(d.Take(60 * S)));
}

TEST(OrcaActivity, EmptyLastLine)
{
  using Orca::Activity::IsEmpty;
  LineClock c(0, Mode::Solo);
  c.Input(30 * S, 1, 1);
  EXPECT_FALSE(IsEmpty(c.Take(60 * S)));
  // 0.4 s after a line, with no input: rounds to 0.
  EXPECT_TRUE(IsEmpty(c.Take(60 * S + 400)));
  // Half a second rounds to 1: kept.
  EXPECT_FALSE(IsEmpty(c.Take(60 * S + 900)));
  // A press in that last moment: kept, for its input.
  c.Input(60 * S + 1000, 1, 1);
  const Line pressed = c.Take(60 * S + 1100);
  EXPECT_FALSE(IsEmpty(pressed));
  EXPECT_EQ(Orca::Activity::FormatLine(pressed), "orca active 0 0 1 solo 1");
  // A full interval is never empty, input or not.
  EXPECT_FALSE(IsEmpty(c.Take(120 * S + 1100)));
}

TEST(OrcaActivity, SecondsRoundToTheNearest)
{
  Line line;
  line.interval_ms = 60'004;
  line.active_ms = 42'499;
  line.inputs = 118;
  line.mode = Mode::Ranked;
  line.pads = 2;
  EXPECT_EQ(Orca::Activity::FormatLine(line), "orca active 60 42 118 ranked 2");
  line.active_ms = 42'500;
  EXPECT_EQ(Orca::Activity::FormatLine(line), "orca active 60 43 118 ranked 2");
}

TEST(OrcaActivity, CouchPadsShareTheSeconds)
{
  // Two players on one machine (a controller and the keyboard pad): a second counts once whoever
  // pressed, inputs add up and pads counts both.
  LineClock c(0, Mode::Solo);
  c.Input(0, 3, Orca::Activity::PAD_ADAPTER | 0);
  c.Input(0, 2, Orca::Activity::PAD_GAMEPAD | 1);
  c.Input(5 * S, 1, Orca::Activity::PAD_ADAPTER | 0);
  c.Input(20 * S, 4, Orca::Activity::PAD_GAMEPAD | 1);
  const Line line = c.Take(60 * S);
  EXPECT_EQ(line.active_ms, 50 * S);
  EXPECT_EQ(line.inputs, 10u);
  EXPECT_EQ(line.pads, 2u);
  // Next interval: only one of them.
  c.Input(61 * S, 1, Orca::Activity::PAD_GAMEPAD | 1);
  EXPECT_EQ(c.Take(120 * S).pads, 1u);
}

TEST(OrcaActivity, ActiveNeverExceedsTheInterval)
{
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> gap(0, 45'000);
  std::uniform_int_distribution<int> pads(1, 4);
  LineClock c(0, Mode::Solo);
  s64 t = 0;
  s64 next_line = c.DueAt();
  for (int i = 0; i < 5000; ++i)
  {
    t += gap(rng) / (i % 3 == 0 ? 1 : 50);
    while (t >= next_line)
    {
      const Line line = c.Take(next_line);
      EXPECT_LE(line.active_ms, line.interval_ms);
      EXPECT_EQ(line.interval_ms, Orca::Activity::INTERVAL_MS);
      next_line = c.DueAt();
    }
    // Several pads at the same instant.
    for (int p = pads(rng); p > 0; --p)
      c.Input(t, 1, static_cast<u32>(p));
  }
  const Line last = c.Take(t + 1);
  EXPECT_LE(last.active_ms, last.interval_ms);
}

TEST(OrcaActivity, ModeWithTheMostActiveTime)
{
  LineClock c(0, Mode::Solo);
  c.Input(0, 1, 1);  // 0-30 s solo...
  c.SetMode(10 * S, Mode::Ranked);
  c.Input(15 * S, 1, 1);  // ...then ranked 10-45 s
  c.SetMode(50 * S, Mode::Solo);
  Line line = c.Take(60 * S);
  EXPECT_EQ(line.active_ms, 45 * S);
  EXPECT_EQ(line.mode, Mode::Ranked);  // 35 s ranked, 10 s solo
  // No activity at all: the mode with the most time.
  c.SetMode(70 * S, Mode::Training);
  c.SetMode(90 * S, Mode::Solo);
  line = c.Take(120 * S);
  EXPECT_EQ(line.active_ms, 0);
  EXPECT_EQ(line.mode, Mode::Solo);  // 40 s solo, 20 s training
  c.SetMode(120 * S, Mode::Training);
  c.SetMode(150 * S, Mode::Casual);
  // A tie goes to the more specific mode.
  EXPECT_EQ(c.Take(180 * S).mode, Mode::Casual);
  // An empty interval: the current one.
  EXPECT_EQ(c.Take(180 * S).mode, Mode::Casual);
}

// ---- Reader: which reads count ----

TEST(OrcaActivity, ReRunsAndRebuildsReadNothing)
{
  // The session notes the pad it read at each boundary. Boundaries before a frame that isn't a
  // first run (a rollback re-run, a joiner's rebuild or catch-up, a fresh start's unseen tail,
  // a launch joiner's own boot: Rollback::IsResimulating()) count nothing, and what they read
  // doesn't move the baseline either.
  Reader r;
  std::vector<std::pair<u32, int>> edges;
  const u32 dolphin = Orca::Activity::PAD_DOLPHIN;
  r.Note(dolphin, Neutral());
  r.Boundary(true, true, {}, &edges);
  EXPECT_TRUE(edges.empty());  // baseline
  // A rebuild replaying a friend's history with buttons flying: nothing.
  for (int i = 0; i < 600; ++i)
  {
    r.Note(dolphin, WithButtons(i % 2 ? PAD_BUTTON_A : PAD_BUTTON_B));
    r.Boundary(false, true, {{Orca::Activity::PAD_GAMEPAD, WithStick(i % 255, 0)}}, &edges);
  }
  EXPECT_TRUE(edges.empty());
  // A note left from a re-run boundary isn't carried into the next one.
  r.Note(dolphin, WithButtons(PAD_BUTTON_A));
  r.Boundary(false, true, {}, &edges);
  r.Boundary(true, true, {}, &edges);
  EXPECT_TRUE(edges.empty());
  // The next first run compares with the last first run's pad.
  r.Note(dolphin, WithButtons(PAD_BUTTON_X));
  r.Boundary(true, true, {}, &edges);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0], std::make_pair(dolphin, 1));
}

TEST(OrcaActivity, UnfocusedDolphinPadIsSkipped)
{
  // Dolphin's own pad reads neutral while the window doesn't have the input: not a release.
  Reader r;
  std::vector<std::pair<u32, int>> edges;
  const u32 dolphin = Orca::Activity::PAD_DOLPHIN;
  r.Note(dolphin, WithButtons(PAD_BUTTON_A));
  r.Boundary(true, true, {}, &edges);
  r.Note(dolphin, Neutral());
  r.Boundary(true, false, {}, &edges);
  EXPECT_TRUE(edges.empty());
  r.Note(dolphin, WithButtons(PAD_BUTTON_A));
  r.Boundary(true, true, {}, &edges);
  EXPECT_TRUE(edges.empty());
  // A script isn't gated.
  r.Note(Orca::Activity::PAD_SCRIPT, Neutral());
  r.Boundary(true, false, {}, &edges);
  r.Note(Orca::Activity::PAD_SCRIPT, WithButtons(PAD_BUTTON_START));
  r.Boundary(true, false, {}, &edges);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0].first, Orca::Activity::PAD_SCRIPT);
}

TEST(OrcaActivity, ControllersMissingFromARead)
{
  // A controller absent from a read (a stale stream, unplugged, not noted) keeps its state; a
  // disconnected status is no read.
  Reader r;
  std::vector<std::pair<u32, int>> edges;
  const u32 pad = Orca::Activity::PAD_ADAPTER | 2;
  r.Boundary(true, true, {{pad, WithButtons(PAD_BUTTON_A)}}, &edges);
  r.Boundary(true, true, {}, &edges);
  GCPadStatus gone = Neutral();
  gone.isConnected = false;
  r.Boundary(true, true, {{pad, gone}}, &edges);
  r.Boundary(true, true, {{pad, WithButtons(PAD_BUTTON_A)}}, &edges);
  EXPECT_TRUE(edges.empty());
  r.Boundary(true, true, {{pad, Neutral()}, {Orca::Activity::PAD_GAMEPAD, Neutral()}}, &edges);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0], std::make_pair(pad, 1));
}

TEST(OrcaActivity, SnapshotPads)
{
  using Orca::Activity::SnapshotPads;
  Orca::UX::Snapshot s;
  s.owned = true;
  s.sent_at = 1000;
  s.received_at = 990;
  for (int i = 0; i < 4; ++i)
  {
    Orca::UX::AdapterPort port;
    port.port = i;
    port.seat = i;
    port.connected = i == 1 || i == 3;
    s.ports.push_back(port);
  }
  Orca::UX::StandardPad gamepad;
  gamepad.index = 0;
  gamepad.buttons[0] = 1;
  gamepad.pressed[0] = true;
  s.pads.push_back(gamepad);
  gamepad.index = 1;
  s.pads.push_back(gamepad);

  std::vector<Pad> out;
  SnapshotPads(s, 5, &out);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0].id, Orca::Activity::PAD_ADAPTER | 1);
  EXPECT_EQ(out[1].id, Orca::Activity::PAD_ADAPTER | 3);
  EXPECT_EQ(out[2].id, Orca::Activity::PAD_GAMEPAD | 0);
  EXPECT_EQ(out[3].id, Orca::Activity::PAD_GAMEPAD | 1);
  EXPECT_EQ(out[2].status.button & PAD_BUTTON_A, PAD_BUTTON_A);

  // Stale or suspended: nothing is known, so nothing is read.
  out.clear();
  SnapshotPads(s, Orca::UX::STALE_MS + 1, &out);
  EXPECT_TRUE(out.empty());
  s.suspended = true;
  SnapshotPads(s, 5, &out);
  EXPECT_TRUE(out.empty());
  s.suspended = false;
  // The adapter's last report too old: its ports are skipped, the gamepads still read.
  s.received_at = 1000 - Orca::UX::STALE_MS - 50;
  SnapshotPads(s, 5, &out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].id, Orca::Activity::PAD_GAMEPAD | 0);
}

TEST(OrcaActivity, ThreeMinutesOfFrames)
{
  // 60 frames a second through the reader and the clock, lines taken every 60 s as the printer
  // does: a player mashing for 20 s, idle at a menu, then a second pad joining, then stopping.
  Reader r;
  LineClock c(0, Mode::Solo);
  std::vector<Line> lines;
  std::vector<std::pair<u32, int>> edges;
  const u32 one = Orca::Activity::PAD_SCRIPT | 0, two = Orca::Activity::PAD_SCRIPT | 1;
  const int frames = 60 * 200;  // 200 s, the game stops at the end
  for (int f = 0; f < frames; ++f)
  {
    const s64 now = static_cast<s64>(std::llround(f * 1000.0 / 60.0));
    if (now >= c.DueAt())
      lines.push_back(c.Take(c.DueAt()));
    const int second = f / 60;
    std::vector<Pad> direct;
    // Pad one mashes A (pressed 4 frames of 8) for the first 20 s, and again from 150 s.
    const bool mashing = second < 20 || second >= 150;
    direct.push_back({one, mashing && (f % 8) < 4 ? WithButtons(PAD_BUTTON_A) : Neutral()});
    // Pad two holds its stick left from 160 s to 170 s.
    direct.push_back({two, second >= 160 && second < 170 ? WithStick(0, 128) : Neutral()});
    edges.clear();
    // Every fifth boundary is a rollback re-run whose read must not count.
    r.Boundary(f % 5 != 4, true, direct, &edges);
    for (const auto& [pad, count] : edges)
      c.Input(now, count, pad);
  }
  lines.push_back(c.Take(static_cast<s64>(std::llround(frames * 1000.0 / 60.0))));
  ASSERT_EQ(lines.size(), 4u);
  std::vector<std::string> text;
  for (const Line& line : lines)
    text.push_back(Orca::Activity::FormatLine(line));
  // 0-60 s: mashing until 20 s, active until 50 s. 60-120 s: idle at a menu. 120-180 s: mashing
  // from 150 s (and the stick at 160 and 170 s). 180-200 s: the last 20 s, within 30 s of input.
  EXPECT_EQ(text[0].substr(0, 18), "orca active 60 50 ");
  EXPECT_EQ(text[1], "orca active 60 0 0 solo 0");
  EXPECT_EQ(text[2].substr(0, 18), "orca active 60 30 ");
  EXPECT_EQ(text[3].substr(0, 18), "orca active 20 20 ");
  EXPECT_EQ(lines[0].pads, 1u);
  EXPECT_EQ(lines[2].pads, 2u);
  EXPECT_EQ(lines[3].pads, 1u);
  // Mashing: a press and a release every 8 frames, about 300 edges in 20 s (re-runs drop some
  // reads but none of the edges, which the next first run sees).
  EXPECT_GE(lines[0].inputs, 290u);
  EXPECT_LE(lines[0].inputs, 300u);
}
