// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Status.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/Rumble.h"

namespace
{
using Orca::UX::Rumble::Device;
using Orca::UX::Rumble::FormatLine;
using Orca::UX::Rumble::MIN_GAP_FRAMES;
using Orca::UX::Rumble::MotorOn;
using Orca::UX::Rumble::OFF_FRAMES;
using Orca::UX::Rumble::RESEND_FRAMES;
using Orca::UX::Rumble::State;
using Orca::UX::Rumble::Target;
using Orca::UX::Rumble::Tracker;

constexpr Target ADAPTER0{Device::Adapter, 0};
constexpr Target PAD1{Device::Pad, 1};

// Feeds `motor` for each boundary (port 1, focused, `target`) and returns the lines printed, as
// the frame they came on and the line.
std::vector<std::pair<int, std::string>> Feed(Tracker& tracker, const std::vector<bool>& motor,
                                              Target target = ADAPTER0, bool live = true,
                                              int port = 0)
{
  std::vector<std::pair<int, std::string>> lines;
  for (int f = 0; f < static_cast<int>(motor.size()); ++f)
  {
    if (const std::optional<State> s = tracker.Boundary(motor[f], live, port, target))
      lines.emplace_back(f, FormatLine(*s));
  }
  return lines;
}

std::vector<bool> Repeat(const std::vector<bool>& pattern, int times)
{
  std::vector<bool> out;
  for (int i = 0; i < times; ++i)
    out.insert(out.end(), pattern.begin(), pattern.end());
  return out;
}
}  // namespace

TEST(OrcaRumble, TheMotorIsOutput1OfTheControllersDirectCommand)
{
  // PADControlMotor: 0x0040_03_0m. Only m = 1 turns the motor on.
  EXPECT_TRUE(MotorOn(0x00400301));
  EXPECT_FALSE(MotorOn(0x00400300));
  EXPECT_FALSE(MotorOn(0x00400302));  // a hard stop
  EXPECT_FALSE(MotorOn(0));           // never written (no controller set up yet)
  EXPECT_FALSE(MotorOn(0x00410301));  // another command
  EXPECT_TRUE(MotorOn(0x00400001));   // any analog mode
}

TEST(OrcaRumble, TheLineNamesThePortTheStateAndTheDevice)
{
  EXPECT_EQ(FormatLine({true, 1, ADAPTER0}), "orca rumble 2 1 adapter 0");
  EXPECT_EQ(FormatLine({false, 0, PAD1}), "orca rumble 1 0 pad 1");
  EXPECT_EQ(FormatLine({false, 3, {Device::None, 7}}), "orca rumble 4 0 none 0");
}

TEST(OrcaRumble, OnAtOnceOffAfterThreeFramesOff)
{
  Tracker t;
  std::vector<bool> motor{false};
  motor.insert(motor.end(), 8, true);
  motor.insert(motor.end(), 5, false);
  const auto lines = Feed(t, motor);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], std::make_pair(1, std::string("orca rumble 1 1 adapter 0")));
  // On from frame 1, off from 9: the line comes on the third frame off.
  EXPECT_EQ(lines[1].second, "orca rumble 1 0 adapter 0");
  EXPECT_EQ(lines[1].first, 9 + OFF_FRAMES - 1);
  EXPECT_FALSE(t.Sent().on);
}

TEST(OrcaRumble, ASteadyMotorIsSaidAgainEveryHalfSecond)
{
  // The page holds the motor only a short while after a line: a long rumble is renewed, the same
  // line every RESEND_FRAMES and nothing between.
  Tracker t;
  const auto lines = Feed(t, std::vector<bool>(600, true));
  ASSERT_EQ(lines.size(), 600u / RESEND_FRAMES);
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    EXPECT_EQ(lines[i].first, static_cast<int>(i) * RESEND_FRAMES);
    EXPECT_EQ(lines[i].second, "orca rumble 1 1 adapter 0");
  }
  static_assert(RESEND_FRAMES <= 30, "the page's hold is about a second");
}

TEST(OrcaRumble, TheGamesOnOffPatternsAreOneBuzz)
{
  // On one frame, off two: never three frames off in a row, so it stays on, renewed on an on
  // frame once RESEND_FRAMES have passed.
  Tracker t;
  const auto lines = Feed(t, Repeat({true, false, false}, 100));
  ASSERT_EQ(lines.size(), 10u);
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    EXPECT_EQ(lines[i].second, "orca rumble 1 1 adapter 0");
    if (i > 0)
      EXPECT_EQ(lines[i].first - lines[i - 1].first, RESEND_FRAMES);
  }
}

TEST(OrcaRumble, NoRenewalWhileTheMotorIsOff)
{
  // Renewals come only while the game holds the motor on: not in the off frames before the off
  // line, and not while the window has no focus.
  Tracker t;
  Feed(t, std::vector<bool>(RESEND_FRAMES - 1, true));
  const auto off = Feed(t, std::vector<bool>(OFF_FRAMES + 20, false));
  ASSERT_EQ(off.size(), 1u);
  EXPECT_EQ(off[0], std::make_pair(OFF_FRAMES - 1, std::string("orca rumble 1 0 adapter 0")));
  Tracker unfocused;
  Feed(unfocused, std::vector<bool>(RESEND_FRAMES - 1, true));
  const auto lines = Feed(unfocused, std::vector<bool>(60, true), ADAPTER0, false);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].second, "orca rumble 1 0 adapter 0");
}

TEST(OrcaRumble, AtMostTenOnLinesAndTwentyLinesASecond)
{
  // Patterns that would flip every few frames: on lines are MIN_GAP_FRAMES apart at least, an off
  // only follows an on, so any second of play has at most 20 lines.
  for (const std::vector<bool>& pattern :
       {std::vector<bool>{true, false, false, false},
        std::vector<bool>{true, true, false, false, false},
        std::vector<bool>{true, false, false, false, false, false},
        std::vector<bool>{true, false, false, false, false, false, false, false}})
  {
    Tracker t;
    const auto lines = Feed(t, Repeat(pattern, 120));
    ASSERT_FALSE(lines.empty());
    int last_on = -MIN_GAP_FRAMES;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
      // Never two lines in a row that say the same (no renewal in a pattern this short).
      if (i > 0)
        EXPECT_NE(lines[i].second, lines[i - 1].second);
      if (lines[i].second == "orca rumble 1 1 adapter 0")
      {
        EXPECT_GE(lines[i].first - last_on, MIN_GAP_FRAMES);
        last_on = lines[i].first;
      }
      std::size_t in_second = 0;
      for (const auto& line : lines)
        in_second += line.first >= lines[i].first && line.first < lines[i].first + 60;
      EXPECT_LE(in_second, 20u);
    }
  }
  static_assert(2 * (60 / MIN_GAP_FRAMES) <= 20);
}

TEST(OrcaRumble, AnOffNeverWaitsForTheGap)
{
  // A one-frame pulse (a light hit, a misprediction's phantom): on at 0, off on the third frame
  // off, not held to MIN_GAP_FRAMES.
  Tracker t;
  const auto lines = Feed(t, {true, false, false, false, false, false, false, false});
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1], std::make_pair(OFF_FRAMES, std::string("orca rumble 1 0 adapter 0")));
  static_assert(OFF_FRAMES < MIN_GAP_FRAMES);
}

TEST(OrcaRumble, TheNextHitAfterALongBuzzIsOnAtOnce)
{
  // The gap counts from the last on line: a combo's next hit after a long buzz is not held back.
  Tracker t;
  std::vector<bool> motor(20, true);
  motor.insert(motor.end(), OFF_FRAMES, false);
  motor.push_back(true);
  const auto lines = Feed(t, motor);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[1],
            std::make_pair(20 + OFF_FRAMES - 1, std::string("orca rumble 1 0 adapter 0")));
  EXPECT_EQ(lines[2], std::make_pair(20 + OFF_FRAMES, std::string("orca rumble 1 1 adapter 0")));
}

TEST(OrcaRumble, NoRumbleWithoutTheFocus)
{
  // The window lost the focus mid-buzz: off after the same three frames, and nothing while it
  // stays unfocused.
  Tracker t;
  ASSERT_TRUE(t.Boundary(true, true, 0, ADAPTER0));
  for (int f = 0; f < MIN_GAP_FRAMES; ++f)
    t.Boundary(true, true, 0, ADAPTER0);
  std::vector<std::string> lines;
  for (int f = 0; f < 60; ++f)
  {
    if (const auto s = t.Boundary(true, false, 0, ADAPTER0))
      lines.push_back(FormatLine(*s));
  }
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "orca rumble 1 0 adapter 0");
}

TEST(OrcaRumble, NoAppDeviceNoRumble)
{
  // Dolphin's own input (the keyboard fallback) or a test script: nothing to rumble.
  Tracker t;
  EXPECT_TRUE(Feed(t, std::vector<bool>(120, true), Target{}).empty());
}

TEST(OrcaRumble, AnotherDeviceMidBuzzIsSaid)
{
  // The player picked up another pad while the motor ran: the new device is named, still on.
  Tracker t;
  Feed(t, std::vector<bool>(MIN_GAP_FRAMES, true), ADAPTER0);
  const auto lines = Feed(t, {true}, PAD1);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].second, "orca rumble 1 1 pad 1");
  // Off then names the device it was on.
  const auto off = Feed(t, std::vector<bool>(MIN_GAP_FRAMES + OFF_FRAMES, false), PAD1);
  ASSERT_EQ(off.size(), 1u);
  EXPECT_EQ(off[0].second, "orca rumble 1 0 pad 1");
}

TEST(OrcaRumble, AnOffDeviceChangeSaysNothing)
{
  Tracker t;
  EXPECT_TRUE(Feed(t, std::vector<bool>(30, false), ADAPTER0).empty());
  EXPECT_TRUE(Feed(t, std::vector<bool>(30, false), PAD1, true, 2).empty());
}

TEST(OrcaRumble, ThePortFollowsTheLocalPlayer)
{
  // A joiner moved home to port 1 mid-buzz (drop-in): the line says so.
  Tracker t;
  const auto first = Feed(t, std::vector<bool>(MIN_GAP_FRAMES, true), ADAPTER0, true, 1);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].second, "orca rumble 2 1 adapter 0");
  const auto moved = Feed(t, {true}, ADAPTER0, true, 0);
  ASSERT_EQ(moved.size(), 1u);
  EXPECT_EQ(moved[0].second, "orca rumble 1 1 adapter 0");
}

TEST(OrcaRumble, AHaltSaysOffAtOnceAndOnlyOnce)
{
  Tracker t;
  ASSERT_TRUE(t.Boundary(true, true, 0, ADAPTER0));
  // Right after the on line: a halt does not wait out the gap.
  const std::optional<State> halted = t.Halt();
  ASSERT_TRUE(halted);
  EXPECT_EQ(FormatLine(*halted), "orca rumble 1 0 adapter 0");
  EXPECT_FALSE(t.Halt());
  // The game runs again with the motor still on: on again, after the gap.
  const auto lines = Feed(t, std::vector<bool>(20, true));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].first, MIN_GAP_FRAMES - 1);
  EXPECT_EQ(lines[0].second, "orca rumble 1 1 adapter 0");
}

TEST(OrcaRumble, NothingToHaltWhenOff)
{
  Tracker t;
  EXPECT_FALSE(t.Halt());
  Feed(t, {true, false, false, false, false, false, false, false, false});
  EXPECT_FALSE(t.Sent().on);
  EXPECT_FALSE(t.Halt());
}

TEST(OrcaRumble, NoLinesWithoutTheCap)
{
  // Offered by every profile, before "record" and "hostui"; on only once the app's answer lists it.
  const std::string offered = Orca::Status::OfferedCaps();
  EXPECT_TRUE(offered.ends_with(" rumble record hostui")) << offered;
  Orca::Status::SetAppCaps("join leave");
  EXPECT_FALSE(Orca::Status::Cap(Orca::Status::RUMBLE_CAP));
  // A halt with nothing sent prints nothing either way.
  Orca::UX::Rumble::Halt();
  Orca::Status::SetAppCaps("join rumble");
  EXPECT_TRUE(Orca::Status::Cap(Orca::Status::RUMBLE_CAP));
  Orca::Status::SetAppCaps("");
}

TEST(OrcaRumble, NoAppDeviceBeforeTheAppsStream)
{
  // No stream: the local pad is Dolphin's own, and the rumble has no device to name.
  Orca::UX::StopControllerStream();
  EXPECT_FALSE(Orca::UX::LocalPad(true));
  EXPECT_FALSE(Orca::UX::LastLocalDevice());
}
