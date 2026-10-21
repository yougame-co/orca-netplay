// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The queue's character select (Core/Orca/Games/Brawl/Queue.h): ready, unready, the ready timer and
// its timeouts, game 1's locks, the joiner's pick steered in, the skip's hold, the raw latch.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::Queue;
namespace MB = Orca::UX::MatchBlock;
using Rollback::InputGate::ALL;
using Rollback::InputGate::Masks;

namespace
{
// The stick's centre, as values (the header's constants have no definition to bind to).
constexpr int CX = GCPadStatus::MAIN_STICK_CENTER_X;
constexpr int CY = GCPadStatus::MAIN_STICK_CENTER_Y;
// The queue's timers, from the builtin rulesets' data (both 30 s and 90 frames).
const int READY_FRAMES = Rulesets::Brawl2025().queue_ready_seconds * 60;
const int SKIP_FRAMES = Rulesets::Brawl2025().queue_skip_frames;

constexpr u16 START = PAD_BUTTON_START, B = PAD_BUTTON_B, A = PAD_BUTTON_A, X = PAD_BUTTON_X,
              Y = PAD_BUTTON_Y, Z = PAD_TRIGGER_Z;

CssPort Placed(int character, int costume = 0)
{
  CssPort p;
  p.readable = true;
  p.human = true;
  p.character = character;
  p.costume = costume;
  p.placed = true;
  p.hand_target = Rules::CSS_HAND_GRID;
  return p;
}

View Solo()
{
  View v;
  v.ready_frames = static_cast<u16>(READY_FRAMES);
  v.skip_frames = static_cast<u16>(SKIP_FRAMES);
  v.queue2 = true;
  v.solo = true;
  v.css = true;
  v.plugged = 1;
  v.ports[0] = Placed(0x05, 2);
  return v;
}

View Room(bool ranked = false)
{
  View v;
  v.ready_frames = static_cast<u16>(READY_FRAMES);
  v.skip_frames = static_cast<u16>(SKIP_FRAMES);
  v.queue2 = true;
  v.css = true;
  v.ranked = ranked;
  v.plugged = 3;
  v.ports[0] = Placed(0x05, 2);
  v.ports[1] = Placed(0x10, 1);
  return v;
}

// One boundary with these newly pressed buttons (the latch's raw view: the frame before had none).
View Press(View v, u16 port1, u16 port2 = 0)
{
  v.raw = {port1, port2};
  v.raw_prev = {0, 0};
  return v;
}

// The state after `view` at `frame`, checking that the boundary run again changes nothing.
State Step(const View& view, const State& state, int frame)
{
  const State after = Advance(view, state, frame);
  EXPECT_EQ(Advance(view, after, frame), after) << "frame " << frame << " isn't idempotent";
  return after;
}

class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override { bytes[a] = v; }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  std::map<u32, u8> bytes;
};

FakeMemory Block(u8 flags)
{
  FakeMemory m;
  for (u32 i = 0; i < MB::FULL_SIZE; ++i)
    m.bytes[MB::BASE + i] = 0;
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, MB::MODE_CASUAL);
  m.Write8(MB::RULESET, 1);
  m.Write8(MB::FLAGS, flags);
  return m;
}

GCPadStatus Pad(u16 buttons)
{
  GCPadStatus pad;
  pad.isConnected = true;
  pad.button = buttons;
  return pad;
}
}  // namespace

TEST(OrcaQueue, TheIdentityRoundTrips)
{
  Identity id;
  id.rating = 1532;
  id.character = 0x12;
  id.costume = 3;
  id.x = -7.25f;
  id.y = 11.5f;
  const std::vector<u8> bytes = EncodeIdentity(id);
  EXPECT_EQ(bytes.size(), 13u);
  EXPECT_EQ(DecodeIdentity(bytes), id);
  // No rating, no pick.
  const std::vector<u8> none = EncodeIdentity(Identity{});
  ASSERT_TRUE(DecodeIdentity(none).has_value());
  EXPECT_EQ(DecodeIdentity(none)->rating, -1);
  EXPECT_FALSE(DecodeIdentity(none)->HasPick());
  // Too short, another version: none. A place no game holds: no pick (the player picks by hand).
  EXPECT_FALSE(DecodeIdentity({1, 2, 3}).has_value());
  std::vector<u8> v2 = bytes;
  v2[0] = 2;
  EXPECT_FALSE(DecodeIdentity(v2).has_value());
  id.x = std::nanf("");
  EXPECT_FALSE(DecodeIdentity(EncodeIdentity(id))->HasPick());
  EXPECT_EQ(DecodeIdentity(EncodeIdentity(id))->rating, 1532);
}

TEST(OrcaQueue, OnItsOwnCharacterSelectStartReadiesAndBUnreadies)
{
  State s;
  // Start without a character down: nothing.
  View v = Solo();
  v.ports[0].placed = false;
  s = Step(Press(v, START), s, 100);
  EXPECT_EQ(s.ready, 0);
  // With one: ready, and it stays while nothing new is pressed.
  s = Step(Press(Solo(), START), s, 101);
  EXPECT_EQ(s.ready, 1);
  s = Step(Solo(), s, 102);
  EXPECT_EQ(s.ready, 1);
  // B: not ready; Start and B together: B wins.
  s = Step(Press(Solo(), B), s, 103);
  EXPECT_EQ(s.ready, 0);
  s = Step(Press(Solo(), START | B), s, 104);
  EXPECT_EQ(s.ready, 0);
  // Taking the token up (another pick): not ready.
  s = Step(Press(Solo(), START), s, 105);
  EXPECT_EQ(s.ready, 1);
  v = Solo();
  v.ports[0].placed = false;
  s = Step(v, s, 106);
  EXPECT_EQ(s.ready, 0);
  // A held Start isn't a new press.
  View held = Solo();
  held.raw = {START, 0};
  held.raw_prev = {START, 0};
  s = Step(held, s, 107);
  EXPECT_EQ(s.ready, 0);
  // No timer, no go on the queue's own character select.
  s = Step(Press(Solo(), START), s, 108 + READY_FRAMES * 2);
  EXPECT_EQ(s.flags, 0);
  EXPECT_EQ(s.timer_start, 0u);
}

TEST(OrcaQueue, NeverReadyWithTheNameListOpen)
{
  View listing = Solo();
  listing.ports[0].name_list = true;
  State s = Step(Press(listing, START), State{}, 100);
  EXPECT_EQ(s.ready, 0);
  // Closed, Start readies as before.
  s = Step(Press(Solo(), START), s, 101);
  EXPECT_EQ(s.ready, 1);
  // A list open while ready (it can't be opened then) un-readies.
  s = Step(listing, s, 102);
  EXPECT_EQ(s.ready, 0);
}

TEST(OrcaQueue, TheBThatStopsTheSearchIsSwallowedUntilLetGo)
{
  State s = Step(Press(Solo(), START), State{}, 400);
  ASSERT_EQ(s.ready, 1);
  // B: not ready, and B stays from the game while it is held (the token stays down).
  s = Step(Press(Solo(), B), s, 401);
  EXPECT_EQ(s.ready, 0);
  EXPECT_TRUE(s.flags & FLAG_SWALLOW_B);
  View held = Solo();
  held.raw = {B, 0};
  held.raw_prev = {B, 0};
  s = Step(held, s, 402);
  EXPECT_TRUE(s.flags & FLAG_SWALLOW_B);
  EXPECT_TRUE(Gate(held, s)[0].buttons & B);
  // Let go: the game has B again (held B backs out to the menus).
  View free = Solo();
  free.raw_prev = {B, 0};
  s = Step(free, s, 403);
  EXPECT_FALSE(s.flags & FLAG_SWALLOW_B);
  EXPECT_FALSE(Gate(free, s)[0].buttons & B);
  // A B pressed while not ready is the game's from the first frame.
  s = Step(Press(Solo(), B), s, 404);
  EXPECT_FALSE(s.flags & FLAG_SWALLOW_B);
  EXPECT_FALSE(Gate(Solo(), s)[0].buttons & B);
}

// Hold B to leave works while searching too: the tap cancels the search and is kept from the game,
// and the same B still held SOLO_LEAVE_FRAMES later reaches it, so the game's own held B backs out
// to the menus. Each frame is a pure, idempotent function of the state (TIMER_START holds when the
// swallow began).
TEST(OrcaQueue, HoldingBWhileSearchingLeaves)
{
  State s = Step(Press(Solo(), START), State{}, 500);
  ASSERT_EQ(s.ready, 1);
  s = Step(Press(Solo(), B), s, 501);
  EXPECT_EQ(s.ready, 0);
  EXPECT_TRUE(s.flags & FLAG_SWALLOW_B);
  EXPECT_EQ(s.timer_start, 502u);
  View held = Solo();
  held.raw = {B, 0};
  held.raw_prev = {B, 0};
  for (int f = 502; f < 501 + SOLO_LEAVE_FRAMES; ++f)
  {
    s = Step(held, s, f);
    EXPECT_TRUE(s.flags & FLAG_SWALLOW_B) << f;
    EXPECT_EQ(s.timer_start, 502u) << f;
    EXPECT_TRUE(Gate(held, s)[0].buttons & B) << f;
  }
  // SOLO_LEAVE_FRAMES after the press: B is the game's (it picks the token up and, held, backs
  // out). Never swallowed again while it stays held.
  s = Step(held, s, 501 + SOLO_LEAVE_FRAMES);
  EXPECT_FALSE(s.flags & FLAG_SWALLOW_B);
  EXPECT_EQ(s.timer_start, 0u);
  EXPECT_EQ(s.ready, 0);
  EXPECT_FALSE(Gate(held, s)[0].buttons & B);
  for (int f = 502 + SOLO_LEAVE_FRAMES; f < 540 + SOLO_LEAVE_FRAMES; ++f)
  {
    s = Step(held, s, f);
    EXPECT_FALSE(s.flags & FLAG_SWALLOW_B) << f;
    EXPECT_FALSE(Gate(held, s)[0].buttons & B) << f;
  }
  // A tap that is let go sooner only cancels: nothing reaches the game.
  State t = Step(Press(Solo(), START), State{}, 700);
  t = Step(Press(Solo(), B), t, 701);
  for (int f = 702; f < 710; ++f)
    t = Step(held, t, f);
  View free = Solo();
  free.raw_prev = {B, 0};
  t = Step(free, t, 710);
  EXPECT_FALSE(t.flags & FLAG_SWALLOW_B);
  EXPECT_EQ(t.timer_start, 0u);
  EXPECT_EQ(t.ready, 0);
  // A stamp from the future (another frame numbering) starts the count again, never lifts early.
  State odd;
  odd.flags = FLAG_SWALLOW_B;
  odd.timer_start = 9000;
  odd = Step(held, odd, 800);
  EXPECT_TRUE(odd.flags & FLAG_SWALLOW_B);
  EXPECT_EQ(odd.timer_start, 801u);
}

// The one-time tip, "New · Hold B to leave", on the queue's own select.
TEST(OrcaQueue, TheHoldBTipShowsOnce)
{
  EXPECT_EQ(TIP_TEXT, "New \u00b7 Hold B to leave");
  // Shown TIP_FRAMES on the select, kept as seen once at TIP_SEEN_FRAMES.
  OnceTip tip;
  int keeps = 0, shown = 0;
  for (int f = 0; f < TIP_FRAMES + 100; ++f)
  {
    const OnceTip::Step step = tip.Frame(true, false, 0);
    shown += step.show;
    keeps += step.keep;
    if (step.keep)
      EXPECT_EQ(f + 1, TIP_SEEN_FRAMES);
  }
  EXPECT_EQ(shown, TIP_FRAMES);
  EXPECT_EQ(keeps, 1);
  EXPECT_TRUE(tip.over);
  // Off the select it waits, and goes on where it was when the player comes back.
  OnceTip away;
  for (int f = 0; f < 60; ++f)
    away.Frame(true, false, 0);
  EXPECT_EQ(away.Frame(false, false, 0), OnceTip::Step{});
  EXPECT_EQ(away.shown, 60);
  EXPECT_TRUE(away.Frame(true, false, 0).show);
  // Readying (Start) ends it; before TIP_SEEN_FRAMES that isn't kept as seen, so the next run
  // shows it again.
  OnceTip start;
  for (int f = 0; f < 30; ++f)
    EXPECT_TRUE(start.Frame(true, false, 0).show);
  EXPECT_EQ(start.Frame(true, true, 0), OnceTip::Step{});
  EXPECT_TRUE(start.over);
  EXPECT_FALSE(start.kept);
  EXPECT_FALSE(start.Frame(true, false, 0).show);
  // Holding B (the lesson) ends it and keeps it as seen at once. A tap doesn't.
  OnceTip hold;
  EXPECT_TRUE(hold.Frame(true, false, TIP_HOLD_FRAMES - 1).show);
  const OnceTip::Step held = hold.Frame(true, false, TIP_HOLD_FRAMES);
  EXPECT_FALSE(held.show);
  EXPECT_TRUE(held.keep);
  EXPECT_EQ(hold.Frame(true, false, TIP_HOLD_FRAMES + 1), OnceTip::Step{});
  // Already seen in an earlier run: never.
  OnceTip seen;
  seen.over = true;
  for (int f = 0; f < 10; ++f)
    EXPECT_EQ(seen.Frame(true, false, 0), OnceTip::Step{});
}

// Back on its own character select not ready after a room (a ranked set played there): the Start
// that left the results screen, still held, readies nobody; a new Start does.
TEST(OrcaQueue, AStartHeldThroughTheWayBackIsSwallowedUntilLetGo)
{
  State s;
  s.flags = FLAG_SWALLOW_START;
  // Its press lands on the restored game: Start in this frame, not in the image's last.
  s = Step(Press(Solo(), START), s, 500);
  EXPECT_EQ(s.ready, 0);
  EXPECT_TRUE(s.flags & FLAG_SWALLOW_START);
  View held = Solo();
  held.raw = {START, 0};
  held.raw_prev = {START, 0};
  s = Step(held, s, 501);
  EXPECT_EQ(s.ready, 0);
  EXPECT_TRUE(s.flags & FLAG_SWALLOW_START);
  // Let go: over; the next Start readies.
  View free = Solo();
  free.raw_prev = {START, 0};
  s = Step(free, s, 502);
  EXPECT_FALSE(s.flags & FLAG_SWALLOW_START);
  EXPECT_EQ(s.ready, 0);
  s = Step(Press(Solo(), START), s, 503);
  EXPECT_EQ(s.ready, 1);
  // Not held at all when it came back: the first Start readies.
  State t;
  t.flags = FLAG_SWALLOW_START;
  t = Step(Solo(), t, 600);
  EXPECT_FALSE(t.flags & FLAG_SWALLOW_START);
  t = Step(Press(Solo(), START), t, 601);
  EXPECT_EQ(t.ready, 1);
  // Idempotent (a boundary run again computes the same).
  State u;
  u.flags = FLAG_SWALLOW_START;
  const State once = Step(Press(Solo(), START), u, 700);
  EXPECT_EQ(Step(Press(Solo(), START), once, 700), once);
}

TEST(OrcaQueue, ItsOwnCharacterSelectsMasks)
{
  State s;
  Masks m = Gate(Solo(), s);
  // Start is Orca's; B backs out while not ready; Z is the game's; nobody else plays.
  EXPECT_EQ(m[0].buttons, START);
  EXPECT_EQ(m[0].press, 0);
  for (int port = 1; port < 4; ++port)
    EXPECT_EQ(m[port], ALL);
  s.ready = 1;
  m = Gate(Solo(), s);
  EXPECT_EQ(m[0].buttons, START | B);
  // Off the character select, or without the queue's header: nothing.
  View off = Solo();
  off.css = false;
  EXPECT_EQ(Gate(off, s), Masks{});
  View no = Solo();
  no.queue2 = false;
  EXPECT_EQ(Gate(no, s), Masks{});
  EXPECT_EQ(Advance(no, s, 5), State{});
}

// A on BACK works wherever B backs out. While searching, one press first stops the search, then
// the next frame hands the game that A to back out, so the page never sees a back-out mid-search.
TEST(OrcaQueue, AOnBackWhileSearchingUnreadiesThenBacksOut)
{
  const auto on_back = [](View v) {
    v.ports[0].hand_target = Rules::CSS_HAND_EXIT;
    v.ports[0].hand_button = Rules::CSS_BUTTON_BACK;
    return v;
  };
  State s = Step(Press(Solo(), START), State{}, 800);
  ASSERT_EQ(s.ready, 1);
  // Hovering BACK while ready: A stays from the game (the search goes on until it stops), and the
  // stick reads centred while A is down so the hand stays there.
  Masks m = Gate(on_back(Solo()), s);
  EXPECT_TRUE(m[0].buttons & A);
  EXPECT_TRUE(m[0].a_centres_stick);
  EXPECT_EQ(m[0].press, 0);
  // An A already held when the hand slid onto BACK: still kept from the game, but the stick is
  // left alone, so the hand can move on.
  View slid = on_back(Solo());
  slid.raw = {A, 0};
  slid.raw_prev = {A, 0};
  m = Gate(slid, s);
  EXPECT_TRUE(m[0].buttons & A);
  EXPECT_FALSE(m[0].a_centres_stick);
  // The press: not ready, and the next frame presses A for the game.
  s = Step(Press(on_back(Solo()), A), s, 801);
  EXPECT_EQ(s.ready, 0);
  EXPECT_TRUE(s.flags & FLAG_BACK_A);
  m = Gate(on_back(Solo()), s);
  EXPECT_EQ(m[0].press, A);
  EXPECT_TRUE(m[0].a_centres_stick);
  EXPECT_FALSE(m[0].buttons & A);
  // A hand that left BACK meanwhile gets no press (an A anywhere else is the game's own business).
  EXPECT_EQ(Gate(Solo(), s)[0].press, 0);
  // The boundary after: the press is over (still held, not new), nothing more is pressed.
  View held = on_back(Solo());
  held.raw = {A, 0};
  held.raw_prev = {A, 0};
  s = Step(held, s, 802);
  EXPECT_FALSE(s.flags & FLAG_BACK_A);
  EXPECT_EQ(s.ready, 0);
  EXPECT_EQ(Gate(held, s)[0].press, 0);
  // Not ready: nothing of the queue's (the rules pass a new A on BACK as the game's own).
  s = Step(Press(on_back(Solo()), A), s, 803);
  EXPECT_FALSE(s.flags & FLAG_BACK_A);
  m = Gate(on_back(Solo()), s);
  EXPECT_FALSE(m[0].buttons & A);
  EXPECT_EQ(m[0].press, 0);
  // Ready, A on Rules (the other leaving button) or in the grid: no stop, no press for the game.
  s = Step(Press(Solo(), START), s, 804);
  ASSERT_EQ(s.ready, 1);
  View rules = on_back(Solo());
  rules.ports[0].hand_button = 0x04;
  State t = Step(Press(rules, A), s, 805);
  EXPECT_EQ(t.ready, 1);
  EXPECT_FALSE(t.flags & FLAG_BACK_A);
  EXPECT_FALSE(Gate(rules, t)[0].buttons & A);
  t = Step(Press(Solo(), A), s, 805);
  EXPECT_EQ(t.ready, 1);
  EXPECT_FALSE(t.flags & FLAG_BACK_A);
  // A queue room's character select: none of it (BACK keeps the rules' A mask, OnlineRulesTest).
  View room = Room();
  room.ports[0].hand_target = Rules::CSS_HAND_EXIT;
  room.ports[0].hand_button = Rules::CSS_BUTTON_BACK;
  State r;
  r.ready = 1;
  r = Step(Press(room, A), r, 806);
  EXPECT_FALSE(r.flags & FLAG_BACK_A);
  EXPECT_FALSE(Gate(room, r)[0].a_centres_stick);
}

// The log says why the own select's ready dropped.
TEST(OrcaQueue, TheLogSaysWhyReadyDropped)
{
  const State ready = Step(Press(Solo(), START), State{}, 100);
  ASSERT_EQ(ready.ready, 1);
  const auto why = [&](const View& v) {
    const State after = Advance(v, ready, 101);
    EXPECT_EQ(after.ready & 1, 0);
    return std::string(UnreadyReason(v, ready, after));
  };
  EXPECT_EQ(why(Press(Solo(), B)), "B");
  View up = Solo();
  up.ports[0].placed = false;
  EXPECT_EQ(why(up), "the token was picked up");
  View back = Solo();
  back.ports[0].hand_target = Rules::CSS_HAND_EXIT;
  back.ports[0].hand_button = Rules::CSS_BUTTON_BACK;
  EXPECT_EQ(why(Press(back, A)), "A on Back cancelled the search");
  // Still ready, or already not ready before this frame.
  EXPECT_EQ(UnreadyReason(Solo(), ready, ready), "dropped on an earlier frame");
  EXPECT_EQ(UnreadyReason(Solo(), State{}, State{}), "dropped on an earlier frame");
}

TEST(OrcaQueue, BothReadyGoOnAndPort1PressesStart)
{
  State s;
  s = Step(Room(), s, 1000);
  EXPECT_EQ(s.timer_start, 1001u);
  EXPECT_EQ(s.ready, 0);
  s = Step(Press(Room(), START), s, 1010);
  EXPECT_EQ(s.ready, 1);
  EXPECT_EQ(s.timer, 10);
  EXPECT_FALSE(s.flags & FLAG_GO);
  // Port 2's B un-readies only port 2; then its Start: both, and on.
  s = Step(Press(Room(), 0, B), s, 1011);
  EXPECT_EQ(s.ready, 1);
  s = Step(Press(Room(), 0, START), s, 1020);
  EXPECT_EQ(s.ready, 3);
  EXPECT_TRUE(s.flags & FLAG_GO);
  // Start for port 1 every other frame until the screen goes; Start never passes, Z does (Project+
  // reads it as the select ends for the AltZ costume).
  Masks m = Gate(Room(), s);
  EXPECT_EQ(m[0].press & START, s.timer % 2 == 0 ? START : 0);
  EXPECT_TRUE(m[0].buttons & START);
  EXPECT_TRUE(m[1].buttons & START);
  EXPECT_FALSE(m[0].buttons & Z);
  EXPECT_FALSE(m[1].buttons & Z);
  s = Step(Room(), s, 1021);
  m = Gate(Room(), s);
  EXPECT_EQ(m[0].press & START, s.timer % 2 == 0 ? START : 0);
  // Off the character select: all fresh.
  View off = Room();
  off.css = false;
  EXPECT_EQ(Step(off, s, 1030), State{});
}

TEST(OrcaQueue, CasualTimeoutNamesWhoWasntReadyAndFreezes)
{
  State s = Step(Room(), State{}, 2000);
  s = Step(Press(Room(), START), s, 2001);
  s = Step(Room(), s, 2000 + READY_FRAMES - 1);
  EXPECT_FALSE(s.flags & FLAG_TIMED_OUT);
  s = Step(Room(), s, 2000 + READY_FRAMES);
  EXPECT_TRUE(s.flags & FLAG_TIMED_OUT);
  EXPECT_EQ(s.timeout_who, 2);  // port 2 wasn't ready
  // Frozen: a late Start changes nothing, and nobody's controller does anything.
  const State frozen = Step(Press(Room(), 0, START), s, 2000 + READY_FRAMES + 5);
  EXPECT_EQ(frozen.ready, s.ready);
  EXPECT_FALSE(frozen.flags & FLAG_GO);
  const Masks m = Gate(Room(), frozen);
  EXPECT_EQ(m[0], ALL);
  EXPECT_EQ(m[1], ALL);
  // Neither ready: both named.
  State n = Step(Room(), State{}, 5000);
  n = Step(Room(), n, 5000 + READY_FRAMES);
  EXPECT_EQ(n.timeout_who, 3);
}

TEST(OrcaQueue, RankedTimeoutGoesOnWithThePicks)
{
  State s = Step(Room(true), State{}, 3000);
  s = Step(Room(true), s, 3000 + READY_FRAMES);
  EXPECT_TRUE(s.flags & FLAG_GO);
  EXPECT_FALSE(s.flags & FLAG_TIMED_OUT);
  // Without a character on both, the no-show rule decides: no go.
  View half = Room(true);
  half.ports[1].placed = false;
  State h = Step(half, State{}, 4000);
  h = Step(half, h, 4000 + READY_FRAMES);
  EXPECT_FALSE(h.flags & (FLAG_GO | FLAG_TIMED_OUT));
}

TEST(OrcaQueue, RankedLaterGamesWaitForTheCharacterOrder)
{
  View v = Room(true);
  v.game1 = false;
  v.order_done = false;
  State s = Step(Press(v, START, START), State{}, 6000);
  EXPECT_EQ(s.timer_start, 0u);
  EXPECT_EQ(s.ready, 0);
  EXPECT_FALSE(ReadyPhase(v));
  v.order_done = true;
  s = Step(v, s, 6001);
  EXPECT_EQ(s.timer_start, 6002u);
  s = Step(Press(v, START, START), s, 6002);
  EXPECT_TRUE(s.flags & FLAG_GO);
}

TEST(OrcaQueue, RankedLaterGamesGoOnTheMomentTheOrderHasBothLockIns)
{
  // Each player's Start in the character order is their lock-in: once both are in, go straight on.
  View v = Room(true);
  v.game1 = false;
  v.order_done = false;
  v.order_locked = 1;  // the winner's lock-in, the loser's turn
  State s = Step(v, State{}, 6000);
  EXPECT_FALSE(s.flags & FLAG_GO);
  EXPECT_EQ(s.ready, 0);
  v.order_done = true;
  v.order_locked = 3;
  v.order_locked_in = true;
  EXPECT_TRUE(OrderLockedIn(v));
  EXPECT_FALSE(Timed(v));
  s = Step(v, s, 6001);
  EXPECT_EQ(s.ready, 3);
  EXPECT_TRUE(s.flags & FLAG_GO);
  const Masks m = Gate(v, s);
  EXPECT_EQ(m[0].press & START, s.timer % 2 == 0 ? START : 0);
  // A token still in a hand (a turn that ran out with it there): on once it is down.
  View up = v;
  up.ports[1].placed = false;
  State t = Step(up, State{}, 7000);
  EXPECT_FALSE(t.flags & FLAG_GO);
  t = Step(up, t, 7000 + READY_FRAMES * 2);
  EXPECT_FALSE(t.flags & (FLAG_GO | FLAG_TIMED_OUT));
  t = Step(v, t, 7000 + READY_FRAMES * 2 + 1);
  EXPECT_TRUE(t.flags & FLAG_GO);
  // Game 1 never: its ready step is the picks' (OrderLockedIn needs a later game).
  View one = v;
  one.game1 = true;
  EXPECT_FALSE(OrderLockedIn(one));
}

TEST(OrcaQueue, ALockedInPlayerKeepsTheirPickAndBUnlocks)
{
  // Casual's later games: A picks and B takes the token up (the game's, OnlineRules.h); Start
  // locks in. Locked in, A, B, X and Y never reach the game, and B unlocks.
  View v = Room();
  v.game1 = false;
  State s = Step(v, State{}, 100);
  Masks m = Gate(v, s);
  EXPECT_EQ(m[0].buttons & (A | B | X | Y), 0);
  EXPECT_EQ(m[1].buttons & (A | B | X | Y), 0);
  s = Step(Press(v, START), s, 101);
  EXPECT_EQ(s.ready, 1);
  m = Gate(v, s);
  EXPECT_EQ(m[0].buttons & (A | B | X | Y), A | B | X | Y);
  EXPECT_EQ(m[1].buttons & (A | B | X | Y), 0);
  // Start with the token in the hand locks nothing in.
  View up = v;
  up.ports[1].placed = false;
  s = Step(Press(up, 0, START), s, 102);
  EXPECT_EQ(s.ready, 1);
  // B unlocks; the game never sees that B (masked while locked in, then held).
  s = Step(Press(v, B), s, 103);
  EXPECT_EQ(s.ready, 0);
  EXPECT_EQ(Gate(v, s)[0].buttons & (A | X | Y), 0);
  // Both lock in: on at once, no clock.
  s = Step(Press(v, START, START), s, 104);
  EXPECT_EQ(s.ready, 3);
  EXPECT_TRUE(s.flags & FLAG_GO);
}

TEST(OrcaQueue, CasualLaterGamesWaitForBothStartsWithNoTimer)
{
  // Later games with the same opponent: no clock, no timeout, nobody frozen.
  View v = Room();
  v.game1 = false;
  EXPECT_TRUE(ReadyPhase(v));
  EXPECT_FALSE(Timed(v));
  State s = Step(v, State{}, 9000);
  s = Step(Press(v, START), s, 9001);
  for (const int frame : {9000 + READY_FRAMES, 9000 + READY_FRAMES * 3, 9000 + READY_FRAMES * 10})
  {
    s = Step(v, s, frame);
    EXPECT_FALSE(s.flags & (FLAG_TIMED_OUT | FLAG_GO)) << frame;
    EXPECT_EQ(s.timeout_who, 0) << frame;
    EXPECT_EQ(s.ready, 1) << frame;
    EXPECT_NE(Gate(v, s)[1], ALL) << frame;
  }
  // Port 2's Start, however late: both ready, on, Start for port 1 every other frame.
  s = Step(Press(v, 0, START), s, 9000 + READY_FRAMES * 10 + 1);
  EXPECT_TRUE(s.flags & FLAG_GO);
  const State next = Step(v, s, 9000 + READY_FRAMES * 10 + 2);
  EXPECT_EQ((Gate(v, s)[0].press | Gate(v, next)[0].press) & START, START);
  // Casual's game 1 and every ranked game keep the 30 s.
  EXPECT_TRUE(Timed(Room()));
  View ranked = Room(true);
  ranked.game1 = false;
  EXPECT_TRUE(Timed(ranked));
  State r = Step(ranked, State{}, 12000);
  r = Step(ranked, r, 12000 + READY_FRAMES);
  EXPECT_TRUE(r.flags & FLAG_GO);
  // From the match block: a fight since the room's header was written is game 2 on.
  FakeMemory m = Block(MB::FLAG_QUEUE2);
  EXPECT_TRUE(Timed(ReadView(m, {})));
  m.Write8(MB::FOUGHT, 1);
  EXPECT_FALSE(ReadView(m, {}).game1);
  EXPECT_FALSE(Timed(ReadView(m, {})));
  m.Write8(MB::MODE, MB::MODE_RANKED);
  EXPECT_TRUE(Timed(ReadView(m, {})));
}

TEST(OrcaQueue, OnlyBothPlayersStartTheTimer)
{
  View v = Room();
  v.plugged = 1;
  State s = Step(Press(v, START), State{}, 7000);
  EXPECT_EQ(s.timer_start, 0u);
  EXPECT_EQ(s.ready, 0);
}

TEST(OrcaQueue, Game1PicksLockOnceIn)
{
  State s = Step(Room(), State{}, 8000);
  EXPECT_EQ(s.locked, 3);
  Masks m = Gate(Room(), s);
  for (int p = 0; p < 2; ++p)
    EXPECT_EQ(m[p].buttons & (A | B | X | Y), A | B | X | Y) << p;
  // Later games: free to change.
  View later = Room();
  later.game1 = false;
  s = Step(later, s, 8001);
  EXPECT_EQ(s.locked, 0);
  m = Gate(later, s);
  EXPECT_EQ(m[0].buttons & (A | B | X | Y), 0);
}

TEST(OrcaQueue, ThePort2PickIsSteeredIn)
{
  Identity pick;
  pick.character = 0x10;
  pick.costume = 2;
  pick.x = 8.0f;
  pick.y = 6.0f;
  View v = Room();
  v.pick2 = pick;
  // Port 2 just plugged in: its hand below the grid, nothing joined.
  v.ports[1] = CssPort{};
  v.ports[1].readable = true;
  v.ports[1].hand_target = Rules::CSS_HAND_NOTHING;
  v.ports[1].hand_x = -10.0f;
  v.ports[1].hand_y = -20.0f;
  State s = Step(v, State{}, 9000);
  EXPECT_TRUE(s.pick_set);
  EXPECT_EQ(s.locked, 1);
  EXPECT_EQ(s.steer_start, 9001u);
  // The first frames, the stick stays centred (the game takes the new pad's origin).
  Masks m = Gate(v, s);
  EXPECT_EQ(m[1], ALL);
  s = Step(v, s, 9000 + STEER_SETTLE_FRAMES);
  m = Gate(v, s);
  // Its controller does nothing; the stick goes up and right, full tilt.
  EXPECT_EQ(m[1].buttons, Rollback::InputGate::ALL_BUTTONS);
  EXPECT_TRUE(m[1].steer);
  EXPECT_GT(m[1].steer_x, CX + 40);
  EXPECT_GT(m[1].steer_y, CY + 40);
  EXPECT_EQ(m[1].press, 0);
  // It can't ready while being steered.
  s = Step(Press(v, 0, START), s, 9021);
  EXPECT_EQ(s.ready & 2, 0);
  // Over the pick with the token in the hand: still, and A on every fourth frame.
  v.ports[1].hand_target = Rules::CSS_HAND_GRID_HOLDING;
  v.ports[1].human = true;
  v.ports[1].character = 0x10;
  v.ports[1].hand_x = 7.5f;
  v.ports[1].hand_y = 6.2f;
  // Still sliding (it stood elsewhere as the last frame began): no A yet.
  v.hand2_prev_x = 7.1f;
  v.hand2_prev_y = 6.2f;
  for (int f = 9022; f < 9026; ++f)
    EXPECT_EQ(Gate(v, Step(v, s, f))[1].press, 0) << f;
  // Creeping (Brawl's hand with the stick let go): as good as still.
  v.hand2_prev_x = 7.47f;
  v.hand2_prev_y = 6.23f;
  int presses = 0;
  for (int f = 9022; f < 9030; ++f)
  {
    s = Step(v, s, f);
    m = Gate(v, s);
    EXPECT_TRUE(m[1].steer);
    EXPECT_EQ(m[1].steer_x, CX);
    presses += (m[1].press & A) != 0;
  }
  EXPECT_EQ(presses, 2);
  // Down with another costume: X turns it; on the right one: in, and locked.
  v.ports[1].placed = true;
  v.ports[1].costume = 0;
  s = Step(v, s, 9032);
  m = Gate(v, s);
  EXPECT_EQ(s.locked & 2, 0);
  EXPECT_EQ(m[1].press, s.steer % 4 == 0 ? X : 0);
  // Down on another character: B takes it back.
  View wrong = v;
  wrong.ports[1].character = 0x11;
  const State w = Step(wrong, s, 9036);
  EXPECT_EQ(Gate(wrong, w)[1].press, w.steer % 4 == 0 ? B : 0);
  v.ports[1].costume = 2;
  s = Step(v, s, 9040);
  EXPECT_EQ(s.locked, 3);
  m = Gate(v, s);
  EXPECT_FALSE(m[1].steer);
  EXPECT_EQ(m[1].buttons & (A | X | Y), A | X | Y);
  // Now it may ready.
  s = Step(Press(v, 0, START), s, 9041);
  EXPECT_EQ(s.ready & 2, 2);
}

TEST(OrcaQueue, SteeringGivesUpAfterItsLimit)
{
  Identity pick;
  pick.character = 0x10;
  pick.x = 8.0f;
  pick.y = 6.0f;
  View v = Room();
  v.pick2 = pick;
  v.ports[1] = CssPort{};
  State s = Step(v, State{}, 100);
  s = Step(v, s, 100 + STEER_LIMIT_FRAMES + 1);
  EXPECT_FALSE(Gate(v, s)[1].steer);
  // Whatever it put down then is its pick.
  v.ports[1] = Placed(0x20);
  s = Step(v, s, 100 + STEER_LIMIT_FRAMES + 2);
  EXPECT_EQ(s.locked & 2, 2);
}

TEST(OrcaQueue, TheCostumeGivesUpAfterItsLimit)
{
  Identity pick;
  pick.character = 0x10;
  pick.costume = 4;
  pick.x = 8.0f;
  pick.y = 6.0f;
  View v = Room();
  v.pick2 = pick;
  v.ports[1] = Placed(0x10, 0);  // down on the pick, in another colour
  State s = Step(v, State{}, 200);
  EXPECT_EQ(s.costume_start, 201u);
  EXPECT_EQ(s.locked & 2, 0);
  s = Step(v, s, 200 + COSTUME_LIMIT_FRAMES - 1);
  EXPECT_EQ(s.locked & 2, 0);
  s = Step(v, s, 200 + COSTUME_LIMIT_FRAMES);
  EXPECT_EQ(s.locked & 2, 2);
  EXPECT_FALSE(Gate(v, s)[1].steer);
}

TEST(OrcaQueue, PortOnesColourIsAnyColour)
{
  // The same character as port 1 in port 1's colour: the game skips that colour, so any will do.
  Identity pick;
  pick.character = 0x05;
  pick.costume = 2;
  View v = Room();
  v.pick2 = pick;
  v.ports[1] = Placed(0x05, 3);
  const State s = Step(v, State{}, 300);
  EXPECT_EQ(s.locked & 2, 2);
}

namespace
{
// Port 1 on the room's character select after a fresh start: the token in its hand, below the grid.
CssPort FreshPort1()
{
  CssPort p;
  p.readable = true;
  p.human = true;
  p.hand_target = Rules::CSS_HAND_NOTHING;
  p.hand_x = -2.0f;
  p.hand_y = -18.0f;
  return p;
}

Identity Mario(int costume = 2)
{
  Identity pick;
  pick.character = 0x05;
  pick.costume = costume;
  pick.x = -12.0f;
  pick.y = 9.5f;
  return pick;
}
}  // namespace

TEST(OrcaQueue, PortOnesOwnPickIsPutBackByItsOwnInputs)
{
  const Identity pick = Mario();
  OwnSteer steer;
  CssPort one = FreshPort1();
  // The select's first frames: centred, nothing pressed.
  for (int f = 0; f < STEER_SETTLE_FRAMES; ++f)
  {
    const OwnStep step = StepOwnPick(one, pick, &steer);
    EXPECT_FALSE(step.done);
    EXPECT_EQ(step, OwnStep{}) << f;
  }
  // Then the hand heads for the pick: up and left, full tilt.
  OwnStep step = StepOwnPick(one, pick, &steer);
  EXPECT_FALSE(step.done);
  EXPECT_LT(step.stick_x, CX - 20);
  EXPECT_GT(step.stick_y, CY + 40);
  EXPECT_EQ(step.buttons, 0);
  // Over the pick, still sliding: no A.
  one.hand_target = Rules::CSS_HAND_GRID_HOLDING;
  one.character = 0x05;
  one.hand_x = -12.4f;
  one.hand_y = 9.3f;
  step = StepOwnPick(one, pick, &steer);
  EXPECT_EQ(step.buttons, 0);
  EXPECT_EQ(step.stick_x, CX);
  // Still: A on every fourth frame, released between.
  int presses = 0;
  for (int i = 0; i < 8; ++i)
  {
    step = StepOwnPick(one, pick, &steer);
    EXPECT_FALSE(step.done);
    EXPECT_EQ(step.buttons & ~A, 0);
    presses += (step.buttons & A) != 0;
  }
  EXPECT_EQ(presses, 2);
  // Down on another character: B on the edges.
  CssPort wrong = one;
  wrong.placed = true;
  wrong.character = 0x06;
  presses = 0;
  for (int i = 0; i < 4; ++i)
  {
    step = StepOwnPick(wrong, pick, &steer);
    EXPECT_EQ(step.buttons & ~B, 0);
    presses += step.buttons != 0;
  }
  EXPECT_EQ(presses, 1);
  // Down on the pick in another colour: X; in its colour: done.
  one.placed = true;
  one.costume = 0;
  presses = 0;
  for (int i = 0; i < 4; ++i)
  {
    step = StepOwnPick(one, pick, &steer);
    EXPECT_FALSE(step.done);
    EXPECT_EQ(step.buttons & ~X, 0);
    presses += step.buttons != 0;
  }
  EXPECT_EQ(presses, 1);
  one.costume = 2;
  EXPECT_TRUE(StepOwnPick(one, pick, &steer).done);
}

TEST(OrcaQueue, PortOnesOwnPickGivesUpAndNeedsAPick)
{
  // No pick: nothing to steer.
  OwnSteer none;
  EXPECT_TRUE(StepOwnPick(FreshPort1(), Identity{}, &none).done);
  // The hand never gets there: done at the limit.
  OwnSteer steer;
  int frames = 0;
  while (!StepOwnPick(FreshPort1(), Mario(), &steer).done)
    ASSERT_LE(++frames, STEER_LIMIT_FRAMES);
  EXPECT_EQ(frames, STEER_LIMIT_FRAMES);
  // The colour never turns: done after COSTUME_LIMIT_FRAMES of X.
  OwnSteer colour;
  colour.frames = STEER_SETTLE_FRAMES;
  CssPort one = FreshPort1();
  one.placed = true;
  one.character = 0x05;
  one.costume = 5;
  int x = 0;
  while (!StepOwnPick(one, Mario(), &colour).done)
    ASSERT_LE(++x, COSTUME_LIMIT_FRAMES);
  EXPECT_EQ(x, COSTUME_LIMIT_FRAMES);
  // Random takes any colour.
  Identity random;
  random.character = RANDOM_CHARACTER;
  random.costume = 3;
  one.character = RANDOM_CHARACTER;
  OwnSteer r;
  r.frames = STEER_SETTLE_FRAMES;
  EXPECT_TRUE(StepOwnPick(one, random, &r).done);
}

TEST(OrcaQueue, PortOnesOwnPickSteersOnlyOnTheRoomsFirstCharacterSelect)
{
  ArmOwnPick(Mario());
  EXPECT_TRUE(OwnPickArmed());
  EXPECT_FALSE(OwnPickPad().has_value());
  DisarmOwnPick();
  EXPECT_FALSE(OwnPickArmed());
  // No pick: never armed.
  ArmOwnPick(Identity{});
  EXPECT_FALSE(OwnPickArmed());
}

TEST(OrcaQueue, SteeringAimsAtThePick)
{
  const auto [cx, cy] = SteerToward(1, 1, 1.05f, 1.05f);
  EXPECT_EQ(cx, CX);
  EXPECT_EQ(cy, CY);
  const auto [lx, ly] = SteerToward(10, 0, 0, 0);
  EXPECT_LT(lx, CX - 90);
  EXPECT_EQ(ly, CY);
  // Near: gentler, but past the dead zone.
  const auto [nx, ny] = SteerToward(0, 0, 0, -1.0f);
  EXPECT_EQ(nx, CX);
  EXPECT_LE(ny, CY - 30);
  EXPECT_GT(ny, CY - 60);
  // A short way off on both axes still moves: the larger component past the dead zone.
  const auto [dx, dy] = SteerToward(-22.22f, 1.49f, -22.5f, 1.78f);
  EXPECT_LE(std::min(dx - CX, dy - CY), -1);
  EXPECT_TRUE(dx <= CX - 29 || dy >= CY + 29) << int(dx) << " " << int(dy);
}

TEST(OrcaQueue, FixedPointIsTheFloatsValueTruncated)
{
  // Read from the bits alone; on this host, v * 65536 is exact and the cast truncates, so the two
  // must agree everywhere ToFixed gives a place.
  const auto reference = [](float v) -> std::optional<s64> {
    if (!std::isfinite(v) || !(std::fabs(v) < 4096.0f))
      return std::nullopt;
    return static_cast<s64>(v * 65536.0f);
  };
  const float edges[] = {0.0f,
                         -0.0f,
                         1.0f,
                         -1.0f,
                         0.5f,
                         -7.5f,
                         14.23f,
                         1.0f / 65536,
                         -1.0f / 65536,
                         1.0f / 131072,
                         std::numeric_limits<float>::denorm_min(),
                         -std::numeric_limits<float>::min(),
                         4095.9998f,
                         -4095.9998f,
                         4096.0f,
                         -4096.0f,
                         1e30f,
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN(),
                         std::bit_cast<float>(0xFFC00001u)};
  for (const float v : edges)
    EXPECT_EQ(ToFixed(v), reference(v)) << v;
  EXPECT_EQ(ToFixed(-7.5f), -7 * FIXED_ONE - FIXED_ONE / 2);
  EXPECT_EQ(ToFixed(1.0f / 131072), 0);
  EXPECT_EQ(ToFixed(-1.0f / 65536), -1);
  std::mt19937 rng(11);
  for (int i = 0; i < 1000000; ++i)
  {
    const float v = std::bit_cast<float>(static_cast<u32>(rng()));
    ASSERT_EQ(ToFixed(v), reference(v)) << std::bit_cast<u32>(v);
  }
}

TEST(OrcaQueue, IntSqrtIsExact)
{
  const auto check = [](u64 n) {
    const u64 r = IntSqrt(n);
    EXPECT_LE(r * r, n) << n;
    EXPECT_GT((r + 1) * (r + 1), n) << n;
  };
  for (u64 n = 0; n < 5000; ++n)
    check(n);
  for (u64 k : {u64{65535}, u64{65536}, u64{1} << 29, (u64{1} << 30) - 1, u64{3037000499}})
  {
    check(k * k - 1);
    check(k * k);
    check(k * k + 1);
  }
  std::mt19937_64 rng(5);
  for (int i = 0; i < 200000; ++i)
    check(rng() >> 2);  // under 2^62: (r + 1)^2 can't overflow
}

TEST(OrcaQueue, SteeringIsTheSameOnEveryHost)
{
  // Golden sticks every host must give exactly (integer arithmetic only). The last rows are cases
  // where float math with a fused multiply-add and without one rounded a unit apart.
  struct Row
  {
    float x, y, tx, ty;
    int sx, sy;
  };
  const Row rows[] = {
      // Brawl's joiner after the settle, the room's real flows' picks, near and nearer.
      {-10.0f, -20.0f, 8.0f, 6.0f, 185, 210},
      {0.0f, -19.8f, -7.5f, 14.23f, 106, 226},
      {-7.0f, 13.5f, -7.5f, 14.23f, 107, 158},
      {-7.4f, 14.1f, -7.5f, 14.23f, 105, 158},
      {-7.45f, 14.2f, -7.5f, 14.23f, 128, 128},  // within 0.1: centred
      {-23.4f, -19.3f, -7.4f, 7.7f, 179, 214},   // Project+
      {-6.9f, 7.2f, -7.4f, 7.7f, 98, 158},
      {-22.22f, 1.49f, -22.5f, 1.78f, 99, 158},  // the larger component taken up to 30
      {10.0f, 0.0f, 0.0f, 0.0f, 28, 128},
      {0.0f, 0.0f, 0.0f, -1.0f, 128, 95},
      {1.0f, 1.0f, 1.05f, 1.05f, 128, 128},
      {29.5f, -19.5f, -29.5f, 19.5f, 45, 183},
      {3.0f, 4.0f, 3.6f, 4.8f, 151, 158},  // exactly 150.5: half rounds up
      // Fused and unfused float math disagreed by one on these.
      {20.1365986f, 11.0321474f, 17.4719582f, 12.5501671f, 41, 178},
      {-8.26101685f, -3.44133377f, -27.370079f, 13.1229248f, 52, 194},
      {-4.39935303f, 15.3992043f, 11.6091461f, 12.5947876f, 227, 111},
      {6.56852341f, -13.0638084f, -1.84294319f, -12.2194958f, 28, 138},
      {-28.3290997f, 5.4784584f, -19.0617294f, 6.40869331f, 227, 138},
  };
  for (const Row& r : rows)
  {
    const auto [sx, sy] = SteerToward(r.x, r.y, r.tx, r.ty);
    EXPECT_EQ(sx, r.sx) << r.x << ", " << r.y << " -> " << r.tx << ", " << r.ty;
    EXPECT_EQ(sy, r.sy) << r.x << ", " << r.y << " -> " << r.tx << ", " << r.ty;
  }
  // No place (a NaN, an infinity, far off the screen): centred.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (const auto& [x, y] : {std::pair{nan, 0.0f}, std::pair{0.0f, inf}, std::pair{5000.0f, 0.0f}})
  {
    EXPECT_EQ(SteerToward(x, y, 8.0f, 6.0f), std::make_pair(u8{CX}, u8{CY})) << x << " " << y;
    EXPECT_EQ(SteerToward(8.0f, 6.0f, x, y), std::make_pair(u8{CX}, u8{CY})) << x << " " << y;
  }
}

TEST(OrcaQueue, SteeringFollowsTheFloatFormula)
{
  // The same steering as the float version (worked out here in double precision): over the
  // character select, every stick within a unit of it (only where the exact value sits on a half),
  // pointing the same way, and its larger component past the dead zone.
  const auto reference = [](double x, double y, double tx, double ty) {
    const double dx = tx - x, dy = ty - y;
    const double d = std::sqrt(dx * dx + dy * dy);
    if (d < 0.1)
      return std::pair<int, int>{CX, CY};
    const double mag = std::clamp(100.0 * d / 3.0, 30.0, 100.0);
    double cx = dx / d * mag, cy = dy / d * mag;
    const double larger = std::max(std::fabs(cx), std::fabs(cy));
    if (larger < 30.0)
    {
      cx *= 30.0 / larger;
      cy *= 30.0 / larger;
    }
    return std::pair<int, int>{static_cast<int>(std::lround(CX + cx)),
                               static_cast<int>(std::lround(CY + cy))};
  };
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> across(-30, 30), down(-20, 20), near(-3, 3);
  int same = 0;
  constexpr int N = 200000;
  for (int i = 0; i < N; ++i)
  {
    const float tx = across(rng), ty = down(rng);
    // Half anywhere, half within 3 units (where the steering spends its last frames).
    const float x = (i & 1) ? across(rng) : tx + near(rng);
    const float y = (i & 1) ? down(rng) : ty + near(rng);
    const auto [sx, sy] = SteerToward(x, y, tx, ty);
    const auto [rx, ry] = reference(x, y, tx, ty);
    ASSERT_LE(std::abs(sx - rx), 1) << x << ", " << y << " -> " << tx << ", " << ty;
    ASSERT_LE(std::abs(sy - ry), 1) << x << ", " << y << " -> " << tx << ", " << ty;
    same += sx == rx && sy == ry;
    if (sx == CX && sy == CY)
      continue;
    // Never away from the pick, and the larger component past the dead zone.
    EXPECT_GE((sx - CX) * (tx - x), 0.0f);
    EXPECT_GE((sy - CY) * (ty - y), 0.0f);
    EXPECT_GE(std::max(std::abs(sx - CX), std::abs(sy - CY)), 30);
  }
  EXPECT_GT(same, N - N / 1000);
}

TEST(OrcaQueue, TheHandIsStillUnderAFrameOfCreep)
{
  EXPECT_TRUE(HandStill(7.5f, 6.2f, 7.47f, 6.23f));
  EXPECT_TRUE(HandStill(-7.5f, 14.23f, -7.46f, 14.27f));
  EXPECT_FALSE(HandStill(7.5f, 6.2f, 7.1f, 6.2f));
  EXPECT_FALSE(HandStill(7.5f, 6.2f, 7.5f, 6.26f));
  EXPECT_FALSE(HandStill(7.5f, std::numeric_limits<float>::quiet_NaN(), 7.5f, 6.2f));
}

TEST(OrcaQueue, HoldingBSkipsOnce)
{
  SkipHold hold;
  for (int i = 1; i < SKIP_FRAMES; ++i)
  {
    EXPECT_FALSE(hold.Step(true, true, SKIP_FRAMES)) << i;
    // No bar for an ordinary B tap: only from SKIP_BAR_FRAMES of a hold.
    if (i < SKIP_BAR_FRAMES)
      EXPECT_EQ(hold.Progress(SKIP_FRAMES), 0.0f) << i;
    else
      EXPECT_GT(hold.Progress(SKIP_FRAMES), 0.0f) << i;
  }
  EXPECT_GT(hold.Progress(SKIP_FRAMES), 0.9f);
  EXPECT_TRUE(hold.Step(true, true, SKIP_FRAMES));
  // Held on: once.
  for (int i = 0; i < 200; ++i)
    EXPECT_FALSE(hold.Step(true, true, SKIP_FRAMES));
  EXPECT_EQ(hold.Progress(SKIP_FRAMES), 0.0f);
  // Let go, held again: again.
  EXPECT_FALSE(hold.Step(true, false, SKIP_FRAMES));
  for (int i = 1; i < SKIP_FRAMES; ++i)
    EXPECT_FALSE(hold.Step(true, true, SKIP_FRAMES));
  EXPECT_TRUE(hold.Step(true, true, SKIP_FRAMES));
  // Where no skip may be asked for, holding counts nothing.
  SkipHold other;
  for (int i = 0; i < SKIP_FRAMES * 2; ++i)
    EXPECT_FALSE(other.Step(false, true, SKIP_FRAMES));
  EXPECT_FALSE(other.Step(true, true, SKIP_FRAMES));
}

TEST(OrcaQueue, RankedHasNoSkip)
{
  // Ranked matches can't be skipped. Casual keeps the skip: the hint, then the bar while B is held.
  State s = Step(Room(), State{}, 100);
  ASSERT_TRUE(ReadyPhase(Room()));
  EXPECT_TRUE(MaySkip(Room(), s));
  Text t = TextFor(Room(), s, {}, 0, 0.0f);
  EXPECT_EQ(t.note, "Hold B to leave");
  t = TextFor(Room(), s, {}, 1, 0.5f);
  EXPECT_NE(t.note.find("Finding someone else |||||....."), std::string::npos) << t.note;
  // Casual's later games too (no timer), and with a player locked in.
  View later = Room();
  later.game1 = false;
  State ls = Step(Press(later, START), State{}, 200);
  ASSERT_EQ(ls.ready, 1);
  EXPECT_TRUE(MaySkip(later, ls));
  EXPECT_EQ(TextFor(later, ls, {}, 0, 0.0f).note, "B to change \u00b7 Hold B to leave");
  // Not once it goes on, nor after a timeout, nor on the player's own character select.
  State go = s;
  go.flags |= FLAG_GO;
  EXPECT_FALSE(MaySkip(Room(), go));
  State out = s;
  out.flags |= FLAG_TIMED_OUT;
  EXPECT_FALSE(MaySkip(Room(), out));
  EXPECT_FALSE(MaySkip(Solo(), State{}));

  // Ranked: game 1 and later games (the order done), either port, ready or not, B held or not:
  // no skip, and nothing on screen offers a hold.
  for (const bool game1 : {true, false})
  {
    View v = Room(true);
    v.game1 = game1;
    v.order_done = true;
    for (const u16 presses : {u16{0}, START, B})
    {
      for (int port = 0; port < 2; ++port)
      {
        State rs = Step(Press(v, port == 0 ? presses : 0, port == 1 ? presses : 0), State{},
                        300 + port);
        EXPECT_FALSE(MaySkip(v, rs)) << game1 << " " << port;
        for (const float progress : {0.0f, 0.4f, 1.0f})
        {
          for (int local = 0; local < 2; ++local)
          {
            const Text rt = TextFor(v, rs, {}, local, progress);
            EXPECT_FALSE(rt.line.empty());
            EXPECT_EQ(rt.line.find("Hold"), std::string::npos) << rt.line;
            EXPECT_EQ(rt.note.find("Hold"), std::string::npos) << rt.note;
            EXPECT_EQ(rt.note.find("someone else"), std::string::npos) << rt.note;
          }
        }
      }
    }
  }
  // Z reaches the game in both modes alike, on every queue select, locked in or not.
  EXPECT_EQ(Gate(Room(true), State{})[0].buttons & Z, 0);
  EXPECT_EQ(Gate(Room(false), State{})[0].buttons & Z, 0);
}

// Players do Project+'s AltZ costume by holding Z as the select ends, so Z is never Orca's on a
// queue character select: own select, rooms of both modes, game 1 locked in, later games.
TEST(OrcaQueue, ZIsTheGamesOnEveryQueueSelect)
{
  for (const u8 ready : {u8{0}, u8{1}})
  {
    State s;
    s.ready = ready;
    for (const Rollback::InputGate::Mask& mask : Gate(Solo(), s))
    {
      if (mask != ALL)
        EXPECT_EQ(mask.buttons & Z, 0);
    }
  }
  for (const bool ranked : {false, true})
  {
    for (const bool game1 : {true, false})
    {
      View v = Room(ranked);
      v.game1 = game1;
      State s;
      s.locked = 3;
      s.ready = 1;
      const Masks m = Gate(v, s);
      EXPECT_EQ(m[0].buttons & Z, 0) << ranked << game1;
      EXPECT_EQ(m[1].buttons & Z, 0) << ranked << game1;
      // A locked game-1 pick still keeps its A, B, X and Y from the game.
      if (game1)
        EXPECT_EQ(m[0].buttons & (A | B | X | Y), A | B | X | Y);
    }
  }
}

TEST(OrcaQueue, RandomIsAPickLikeAnyCharacter)
{
  // Random is a valid pick in casual and ranked. It is the character select's 0x29 in both games
  // (Selch_Random), never "no character".
  ASSERT_NE(RANDOM_CHARACTER, NO_CHARACTER);
  Identity id;
  id.character = RANDOM_CHARACTER;
  id.x = 0.5f;
  id.y = 0.5f;
  id.rating = 1532;
  ASSERT_TRUE(id.HasPick());
  ASSERT_EQ(DecodeIdentity(EncodeIdentity(id)), id);

  // The queue's own character select: the token down on RANDOM, Start readies (and searches).
  View solo = Solo();
  solo.ports[0] = Placed(RANDOM_CHARACTER, 0);
  State s = Step(Press(solo, START), State{}, 100);
  EXPECT_EQ(s.ready, 1);

  // The room's character select, game 1: the joiner's Random is steered onto its panel.
  for (const bool ranked : {false, true})
  {
    Identity pick = id;
    pick.costume = 3;  // whatever colour its own game showed: Random takes any
    View v = Room(ranked);
    v.ports[0] = Placed(RANDOM_CHARACTER, 0);  // the host on Random too
    v.pick2 = pick;
    v.ports[1] = CssPort{};
    v.ports[1].readable = true;
    v.ports[1].human = true;
    v.ports[1].hand_target = Rules::CSS_HAND_GRID_HOLDING;
    v.ports[1].character = RANDOM_CHARACTER;  // the hand over the RANDOM tile, the token in it
    v.ports[1].hand_x = 0.45f;
    v.ports[1].hand_y = 0.52f;
    v.hand2_prev_x = 0.45f;
    v.hand2_prev_y = 0.52f;
    State r = Step(v, State{}, 1000);
    EXPECT_TRUE(r.pick_set);
    EXPECT_EQ(r.pick_character, RANDOM_CHARACTER);
    EXPECT_EQ(r.locked, 1);  // the host's Random is in
    int presses = 0;
    for (int f = 1000 + STEER_SETTLE_FRAMES; f < 1008 + STEER_SETTLE_FRAMES; ++f)
    {
      r = Step(v, r, f);
      presses += (Gate(v, r)[1].press & A) != 0;
    }
    EXPECT_EQ(presses, 2);
    // Down on RANDOM in another colour: in and locked at once, no X turning it.
    v.ports[1] = Placed(RANDOM_CHARACTER, 0);
    r = Step(v, r, 1030);
    EXPECT_EQ(r.locked, 3) << ranked;
    EXPECT_EQ(Gate(v, r)[1].press & X, 0);
    EXPECT_FALSE(Gate(v, r)[1].steer);
    // Both lock in with Start: on to the stage select, the picks Random (the game resolves them).
    r = Step(Press(v, START, START), r, 1031);
    EXPECT_EQ(r.ready, 3);
    EXPECT_TRUE(r.flags & FLAG_GO);
    const Text t = TextFor(Press(v, START, START), r, {}, 0, 0);
    EXPECT_TRUE(t.locked[0] && t.locked[1]);
  }

  // Casual's later games: A put the token on RANDOM, Start locks it in, B unlocks.
  View later = Room();
  later.game1 = false;
  later.ports[0] = Placed(RANDOM_CHARACTER, 0);
  State c = Step(Press(later, START), State{}, 2000);
  EXPECT_EQ(c.ready, 1);
  EXPECT_EQ(Gate(later, c)[0].buttons & (A | B | X | Y), A | B | X | Y);
  c = Step(Press(later, B), c, 2001);
  EXPECT_EQ(c.ready, 0);

  // Ranked's later games: the character order's two lock-ins on RANDOM go on at once.
  View order = Room(true);
  order.game1 = false;
  order.order_done = true;
  order.order_locked_in = true;
  order.order_locked = 3;
  order.ports[0] = Placed(RANDOM_CHARACTER, 0);
  order.ports[1] = Placed(RANDOM_CHARACTER, 1);
  ASSERT_TRUE(OrderLockedIn(order));
  const State o = Step(order, State{}, 3000);
  EXPECT_EQ(o.ready, 3);
  EXPECT_TRUE(o.flags & FLAG_GO);
}

TEST(OrcaQueue, TheLatchKeepsTheRawButtons)
{
  FakeMemory m = Block(MB::FLAG_QUEUE2);
  std::array<std::optional<GCPadStatus>, 4> raw{Pad(START | Z), Pad(B), std::nullopt, Pad(A)};
  EXPECT_GT(Latch(m, raw), 0);
  EXPECT_EQ(m.Read16(RAW), START | Z);
  EXPECT_EQ(m.Read16(RAW + 2), B);
  EXPECT_EQ(m.Read16(RAW_PREV), 0);
  raw = {Pad(0), std::nullopt, std::nullopt, std::nullopt};
  Latch(m, raw);
  EXPECT_EQ(m.Read16(RAW), 0);
  EXPECT_EQ(m.Read16(RAW + 2), 0);
  EXPECT_EQ(m.Read16(RAW_PREV), START | Z);
  EXPECT_EQ(m.Read16(RAW_PREV + 2), B);
  // Any queue room's header, even without queue2, is written too (OnlineRules.h and CharOrder.h
  // read it).
  FakeMemory plain = Block(0);
  EXPECT_GT(Latch(plain, {Pad(START), Pad(B), std::nullopt, std::nullopt}), 0);
  EXPECT_EQ(plain.Read16(RAW), START);
  EXPECT_EQ(plain.Read16(RAW + 2), B);
  // No header (a friends game, solo play): never written.
  FakeMemory none = Block(0);
  none.Write32(MB::MAGIC, 0x3A200000);
  EXPECT_EQ(Latch(none, {Pad(START), Pad(START), std::nullopt, std::nullopt}), 0);
  EXPECT_EQ(none.Read16(RAW), 0);
  // Mode none (never written so): no header that locks, never written.
  FakeMemory unlocked = Block(0);
  unlocked.Write8(MB::MODE, MB::MODE_NONE);
  EXPECT_EQ(Latch(unlocked, {Pad(START), Pad(START), std::nullopt, std::nullopt}), 0);
}

TEST(OrcaQueue, TheStateRoundTripsThroughTheBlock)
{
  FakeMemory m = Block(MB::FLAG_QUEUE2);
  State s;
  s.ready = 3;
  s.flags = FLAG_GO;
  s.timeout_who = 2;
  s.locked = 1;
  s.timer_start = 0x01020304;
  s.timer = 77;
  s.steer = 12;
  s.steer_start = 0x0A0B0C0D;
  s.costume_start = 0x11223344;
  s.pick_set = true;
  s.pick_character = 0x10;
  s.pick_costume = 3;
  s.pick_x = -4.5f;
  s.pick_y = 9.25f;
  EXPECT_GT(WriteState(m, s), 0);
  EXPECT_EQ(ReadState(m), s);
  EXPECT_EQ(WriteState(m, s), 0);
  // The region fits before the saved rules' flags and after Project+'s saved stage data.
  EXPECT_LE(REGION_END, MB::SAVED_FLAGS);
  EXPECT_GE(MB::QUEUE, MB::SAVED_RSS + MB::SAVED_RSS_SIZE);
}

// A player report (Orca 0.3.32): after a ranked set, the next match's auto pick left the hand
// hovering another character, and the player's controller did nothing for seconds. A prod log
// (0.3.38) shows the cause: back on its own select after a ranked set, the player readied with the
// token already down, so Orca never saw the hand over it and published the pick at (0, 0)
// ("orca queue ready ranked 11 6 (the hand at 0.0, 0.0)"). The steering takes the hand there, which
// is another character's square, never presses A, and holds the pad until STEER_LIMIT_FRAMES.
TEST(OrcaQueue, APickAimedAtAnotherSquareHoldsThePadUntilItsLimit)
{
  Identity pick;
  pick.character = 0x1C;
  pick.costume = 3;
  View v = Room(true);
  v.pick2 = pick;  // x, y = 0, 0
  v.ports[1] = CssPort{};
  v.ports[1].readable = true;
  v.ports[1].human = true;
  v.ports[1].hand_target = Rules::CSS_HAND_GRID_HOLDING;
  v.ports[1].character = 0x13;  // the square at (0, 0) is someone else's
  v.hand2_prev_x = v.ports[1].hand_x = 0.0f;
  v.hand2_prev_y = v.ports[1].hand_y = 0.0f;
  State s = Step(v, State{}, 300);
  for (int f = 300 + STEER_SETTLE_FRAMES; f < 300 + STEER_LIMIT_FRAMES; ++f)
  {
    s = Step(v, s, f);
    const Masks m = Gate(v, s);
    ASSERT_TRUE(m[1].steer) << f;
    EXPECT_EQ(m[1].buttons, Rollback::InputGate::ALL_BUTTONS) << f;
    EXPECT_EQ(m[1].steer_x, CX) << f;
    EXPECT_EQ(m[1].press, 0) << f;
  }
}

namespace
{
CssPort Holding(int character, float x, float y)
{
  CssPort p;
  p.readable = true;
  p.human = true;
  p.character = character;
  p.hand_target = Rules::CSS_HAND_GRID_HOLDING;
  p.hand_x = x;
  p.hand_y = y;
  return p;
}
CssPort DownOn(int character, int costume, float x, float y)
{
  CssPort p = Placed(character, costume);
  p.hand_x = x;
  p.hand_y = y;
  return p;
}
}  // namespace

TEST(OrcaQueue, AHandOverTheCharacterTeachesItsPlace)
{
  PickPlaces places;
  EXPECT_FALSE(places.Frame(0, Holding(0x13, -1.0f, 0.0f)));
  EXPECT_FALSE(places.Frame(0, Holding(0x1C, 4.0f, 2.0f)));
  EXPECT_FALSE(places.Frame(0, Holding(0x1C, 6.0f, 3.0f)));
  const auto placed = places.Frame(0, DownOn(0x1C, 3, 6.0f, 3.0f));
  ASSERT_TRUE(placed);
  EXPECT_TRUE(placed->learned);
  EXPECT_EQ(placed->character, 0x1C);
  EXPECT_EQ(places.Find(0x1C), std::make_pair(5.0f, 2.5f));
  EXPECT_FALSE(places.Find(0x13));
  // Staying down is not another placement.
  EXPECT_FALSE(places.Frame(0, DownOn(0x1C, 3, 6.0f, 3.0f)));
  // Either panel teaches: every select has the same grid.
  places.Frame(1, Holding(0x13, -1.0f, 0.0f));
  places.Frame(1, Holding(0x13, -2.0f, 1.0f));
  EXPECT_TRUE(places.Frame(1, DownOn(0x13, 0, -2.0f, 1.0f))->learned);
  EXPECT_EQ(places.Find(0x13), std::make_pair(-1.5f, 0.5f));
  EXPECT_EQ(places.Find(0x1C), std::make_pair(5.0f, 2.5f));
}

TEST(OrcaQueue, ATokenAlreadyDownTeachesNothing)
{
  // The select comes up with the token down (the queue image after a ranked set): no place.
  PickPlaces places;
  const auto placed = places.Frame(0, DownOn(0x1C, 3, 0.0f, 0.0f));
  ASSERT_TRUE(placed);
  EXPECT_FALSE(placed->learned);
  EXPECT_FALSE(places.Find(0x1C));
  // The hand was last over another character: that square is not this one's place (the old code
  // used it).
  PickPlaces other;
  other.Frame(0, Holding(0x13, -1.0f, 0.0f));
  other.Frame(0, CssPort{});
  const auto down = other.Frame(0, DownOn(0x1C, 3, 0.0f, 0.0f));
  ASSERT_TRUE(down);
  EXPECT_FALSE(down->learned);
  EXPECT_FALSE(other.Find(0x1C));
  // A place learned earlier stays when the token later comes back already down.
  PickPlaces kept;
  kept.Frame(0, Holding(0x1C, 4.0f, 2.0f));
  kept.Frame(0, DownOn(0x1C, 3, 4.0f, 2.0f));
  kept.Frame(0, CssPort{});  // a fight, the results, the queue image
  kept.Frame(1, DownOn(0x13, 0, 0.0f, 0.0f));
  EXPECT_TRUE(kept.Frame(0, DownOn(0x1C, 3, 0.0f, 0.0f)));
  EXPECT_EQ(kept.Find(0x1C), std::make_pair(4.0f, 2.0f));
  EXPECT_FALSE(kept.Find(0x13));
}

TEST(OrcaQueue, AnUnknownPlaceIsPublishedWithNoPick)
{
  Identity pick;
  pick.rating = 1234;
  pick.character = 0x1C;
  pick.costume = 3;
  const Identity known = Publishable(pick, std::make_pair(5.0f, 2.5f));
  EXPECT_EQ(known.character, 0x1C);
  EXPECT_EQ(known.costume, 3);
  EXPECT_EQ(known.x, 5.0f);
  EXPECT_EQ(known.y, 2.5f);
  const Identity unknown = Publishable(pick, std::nullopt);
  EXPECT_FALSE(unknown.HasPick());
  EXPECT_EQ(unknown.rating, 1234);
  // No pick is no pick.
  EXPECT_EQ(Publishable(Identity{}, std::make_pair(1.0f, 1.0f)), Identity{});
  // On the wire it still carries the rating, and nobody steers it: the joiner's pad is its own.
  const auto decoded = DecodeIdentity(EncodeIdentity(unknown));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->rating, 1234);
  EXPECT_FALSE(decoded->HasPick());
  View v = Room(true);
  v.ports[1] = CssPort{};
  v.ports[1].readable = true;
  // ReadView only keeps an identity with a pick as port 2's.
  v.pick2 = decoded->HasPick() ? decoded : std::nullopt;
  State s = Step(v, State{}, 400);
  s = Step(v, s, 400 + STEER_SETTLE_FRAMES + 4);
  EXPECT_FALSE(s.pick_set);
  const Masks m = Gate(v, s);
  EXPECT_FALSE(m[1].steer);
  EXPECT_EQ(m[1].buttons & A, 0);
  // And the host's own steering is never armed for it.
  ArmOwnPick(unknown);
  EXPECT_FALSE(OwnPickArmed());
}

// The prod log behind the fix (Orca 0.3.38): in the room, port 1's own pick went down on character
// 0xb costume 6 with the hand over it from (-24.86, 4.83) to (-21.25, 4.95); after the ranked set
// the player was back on their own select with that token already down and readied without moving
// the hand, and 0.3.38 published "orca queue ready ranked 11 6 (the hand at 0.0, 0.0)". The room's
// placement now teaches 0xb's place, so the next room steers to it instead of to (0, 0).
TEST(OrcaQueue, AfterARankedSetTheRoomsPlacementIsTheNextPicksPlace)
{
  PickPlaces places;
  // The room's select: port 1's hand holds the token over 0xb, then puts it down.
  places.Frame(0, Holding(0xb, -24.86f, 4.83f));
  places.Frame(0, Holding(0xb, -21.25f, 4.95f));
  ASSERT_TRUE(places.Frame(0, DownOn(0xb, 6, -21.25f, 4.95f))->learned);
  // The fights and results: no select.
  places.Frame(0, CssPort{});
  // Back on the player's own select after the set: the token is already down, the hand never moved.
  const auto back = places.Frame(0, DownOn(0xb, 6, 0.0f, 0.0f));
  ASSERT_TRUE(back);
  Identity pick;
  pick.rating = 1500;
  pick.character = 0xb;
  pick.costume = 6;
  const Identity published = Publishable(pick, places.Find(0xb));
  ASSERT_TRUE(published.HasPick());
  EXPECT_FLOAT_EQ(published.x, (-24.86f + -21.25f) / 2);
  EXPECT_FLOAT_EQ(published.y, (4.83f + 4.95f) / 2);
  const auto decoded = DecodeIdentity(EncodeIdentity(published));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->character, 0xb);
  EXPECT_NEAR(decoded->x, published.x, 0.01f);
  EXPECT_NEAR(decoded->y, published.y, 0.01f);
  // A different pick on the same select (the player swapped characters by hand in the room's
  // select, so its place was never seen) goes out with no pick rather than 0xb's square.
  pick.character = 0x13;
  EXPECT_FALSE(Publishable(pick, places.Find(0x13)).HasPick());
}
