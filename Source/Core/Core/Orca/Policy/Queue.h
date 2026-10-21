// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Rollback/InputGate.h"

// The queue's character select machine, Slippi-style (ORCA.md "The queue's character select"):
// ready flags, the ready timer, the skip's hold, port 2's pick steered in with integer
// steering, and the gate's masks and presses. Pure functions of what a frame sees (View, State)
// and the frame number. Engine-side competitive policy (Orca split, S2 commit 7, Luis's decision
// 11): a person reviews a change here. Its timers are ruleset data (S5): the ruleset's queue
// section, which the Brawl module puts in View. The Brawl module
// (Games/Brawl/Queue.h) reads View and State from memory, calls these at the same positions as
// before, and writes the result back.
namespace Orca::UX::Queue
{
using QueueApp::Identity;

constexpr u8 FLAG_GO = 0x01;         // both ready (or ranked timer expired): Orca presses Start
constexpr u8 FLAG_TIMED_OUT = 0x02;  // casual timer expired: frozen until the players leave
// The B that cancelled the search is still held. Hide it from the game until released, or the
// token would be picked back up. Held SOLO_LEAVE_FRAMES, it reaches the game, which backs out to
// the menus as held B does (hold B to leave).
constexpr u8 FLAG_SWALLOW_B = 0x04;
// After returning from a room not ready, the Start or A that left the results screen may still be
// held. It must not start a search until released.
constexpr u8 FLAG_SWALLOW_START = 0x08;
// A on BACK while ready un-readies at this frame, then the game sees the same A on the next frame
// and backs out, so one press does both. Idempotent if the frame is re-run.
constexpr u8 FLAG_BACK_A = 0x10;

// The skip's bar shows only after this much of a hold, so an ordinary B tap never flashes it.
constexpr int SKIP_BAR_FRAMES = 15;
// The queue's own select while searching: B cancels the search; still held this long, it reaches
// the game, whose own held B backs out to the menus about half a second later.
constexpr int SOLO_LEAVE_FRAMES = 30;
// Give up steering port 2's hand after this long; the player then picks by hand.
constexpr int STEER_LIMIT_FRAMES = 10 * 60;
// Keep port 2's stick centred this long first. The game calibrates a newly plugged pad's stick
// centre from its first reading, so steering too early would leave the stick reading as pushed.
constexpr int STEER_SETTLE_FRAMES = 20;
// Give up turning port 2's costume (X) after this long; the pick stands in any colour.
constexpr int COSTUME_LIMIT_FRAMES = 3 * 60;

// The character select's id for "no character" (muSelCharPlayerArea +0x1B8, Selch_SelectNone):
// QueueApp::NO_CHARACTER, beside the queue identity that carries it.
using QueueApp::NO_CHARACTER;
// The id for Random (Selch_Random, same in Project+). The game resolves it from its own emulated
// RNG, so it is the same on every machine. X does not change Random's colour, so any costume is
// accepted.
constexpr int RANDOM_CHARACTER = 0x29;

// ---- What a frame sees ----
struct CssPort
{
  bool readable = false;  // the area could be read
  bool human = false;
  int character = NO_CHARACTER;  // under the hand while holding the token, else the token's
  bool placed = false;           // the token is down on a character
  int costume = 0;
  bool name_list = false;  // its name list is open (Rules::ReadCssNameList)
  // The hand (OnlineRules.h CssHand).
  u32 hand_target = 0;
  u32 hand_button = 0;
  float hand_x = 0;
  float hand_y = 0;
  bool operator==(const CssPort&) const = default;
};

struct View
{
  bool queue2 = false;  // the header is the queue's (FLAG_QUEUE2) and locks the rules
  bool solo = false;    // the queue's own character select (FLAG_SOLO)
  bool ranked = false;
  bool css = false;      // on the character select
  bool game1 = true;     // no fight since the header was written
  bool order_done = true;  // ranked later games: the character order is done (CharOrder.h)
  // Ranked later games: both picks are locked in, so the stage select comes at once. And which
  // ports have locked in so far (bit p, for the overlay's badges).
  bool order_locked_in = false;
  u8 order_locked = 0;
  u8 plugged = 0;        // the ports in play (MatchBlock::PLUGGED)
  std::array<CssPort, 2> ports{};
  std::array<u16, 2> raw{};
  std::array<u16, 2> raw_prev{};
  // Port 2's hand at the start of the last frame (HAND_PREV).
  float hand2_prev_x = 0;
  float hand2_prev_y = 0;
  std::optional<Identity> pick2;  // port 2's queued pick, from its plug frame on
  // The ruleset's queue timers (queue.readySeconds x 60, queue.skipFrames: Brawl's and Project+'s
  // 30 s and 90 frames), set with queue2.
  u16 ready_frames = 0;
  u16 skip_frames = 0;
  bool operator==(const View&) const = default;
};

struct State
{
  u8 ready = 0;
  u8 flags = 0;
  u8 timeout_who = 0;
  u8 locked = 0;
  u32 timer_start = 0;
  u16 timer = 0;
  u16 steer = 0;
  u32 steer_start = 0;
  u32 costume_start = 0;
  bool pick_set = false;
  u8 pick_character = 0;
  u8 pick_costume = 0;
  float pick_x = 0;
  float pick_y = 0;
  bool operator==(const State&) const = default;
};

// The room's character select with both players in, at the ready step.
bool ReadyPhase(const View& view);
// Ranked later games once both character-order picks are locked in; no ready step.
bool OrderLockedIn(const View& view);

// Whether the ready step has a 30 s timer: every ranked game, and only casual's first game. Later
// casual games wait for both Starts with no clock, to give players time to talk.
bool Timed(const View& view);

// Whether holding B may ask to skip this opponent: casual only, both players in, before the go or a
// timeout. Ranked matches cannot be skipped; leaving any other way is a forfeit.
bool MaySkip(const View& view, const State& state);

// The state after this frame. Pure and idempotent: feeding the result back in changes nothing.
State Advance(const View& view, const State& state, int frame);

// Why the own select's ready (port 1) dropped from `before` to `after`, for the log.
std::string_view UnreadyReason(const View& view, const State& before, const State& after);

// The gate's masks and presses for the frame about to run. Pure.
Rollback::InputGate::Masks Gate(const View& view, const State& state);

// B held for `skip_frames` (View::skip_frames) in a row fires one skip; B must be released before
// the next. A tap keeps its own meaning (un-pick, un-ready). Local only.
struct SkipHold
{
  int frames = 0;
  bool sent = false;
  // Returns whether a skip fires on this frame.
  bool Step(bool may_skip, bool held, int skip_frames);
  // 0..1 progress toward a skip, for the overlay: 0 until SKIP_BAR_FRAMES.
  float Progress(int skip_frames) const;
};

// ---- Fixed point (the steering's arithmetic) ----
// Steering is integer-only because host floating point differs between compilers (for example
// ARM64 clang fuses a multiply-add that x86-64 MSVC does not), which would desync the machines.
// See ORCA.md, "Determinism across hosts". Positions are in FIXED_ONEs, 1/65536 of a unit.
constexpr int FIXED_BITS = 16;
constexpr s64 FIXED_ONE = s64{1} << FIXED_BITS;
// Positions this far out are invalid (the character select spans about -30..30, -20..20).
constexpr s64 FIXED_LIMIT = s64{4096} << FIXED_BITS;
// A float in FIXED_ONEs, truncated toward zero, decoded from its bits with no float instructions.
// nullopt for NaN, infinity, or a magnitude of FIXED_LIMIT or more.
std::optional<s64> ToFixed(float value);
// floor(sqrt(n)), exactly.
u64 IntSqrt(u64 n);

// Port 2's stick to move its hand from (x, y) to (tx, ty): full tilt when far, gentler when near,
// centred when close enough or on an invalid position. Integer only.
std::pair<u8, u8> SteerToward(float x, float y, float tx, float ty);
// Whether port 2's hand has nearly stopped: under 0.05 a frame on both axes (Brawl's hand creeps
// about 0.03 a frame after the stick is released). Integer only.
bool HandStill(float x, float y, float prev_x, float prev_y);
}  // namespace Orca::UX::Queue
