// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Policy/Queue.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

#include "Core/Orca/Policy/CssRules.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX::Queue
{
namespace
{
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;
using Rollback::InputGate::Masks;

constexpr u16 BUTTON_START = PAD_BUTTON_START;
constexpr u16 BUTTON_B = PAD_BUTTON_B;
}  // namespace

// ---- The step machine ----

bool ReadyPhase(const View& v)
{
  return v.queue2 && !v.solo && v.css && (v.plugged & 3) == 3 && (v.game1 || v.order_done);
}

bool OrderLockedIn(const View& v)
{
  return ReadyPhase(v) && v.ranked && !v.game1 && v.order_locked_in;
}

bool Timed(const View& v)
{
  return (v.ranked || v.game1) && !OrderLockedIn(v);
}

bool MaySkip(const View& v, const State& s)
{
  return v.queue2 && !v.solo && !v.ranked && v.css && (v.plugged & 3) == 3 &&
         !(s.flags & (FLAG_TIMED_OUT | FLAG_GO));
}

std::string_view UnreadyReason(const View& v, const State& before, const State& after)
{
  if (!(before.ready & 1) || (after.ready & 1))
    return "dropped on an earlier frame";
  if (after.flags & FLAG_BACK_A)
    return "A on Back cancelled the search";
  if ((v.raw[0] & ~v.raw_prev[0]) & BUTTON_B)
    return "B";
  if (!v.ports[0].placed)
    return "the token was picked up";
  if (!v.queue2 || !v.solo || !v.css)
    return "left the character select";
  return "unknown";
}

State Advance(const View& v, const State& state, int frame)
{
  State s = state;
  if (!v.queue2)
    return State{};
  if (!v.css)
  {
    s = State{};
    return s;
  }
  std::array<u16, 2> newly{};
  for (int p = 0; p < 2; ++p)
    newly[p] = static_cast<u16>(v.raw[p] & ~v.raw_prev[p]);
  const u32 now = static_cast<u32>(frame) + 1;
  // Start with the token down readies a port. B, or picking the token back up, un-readies it.
  // B wins if both happen in one frame.
  const auto follow = [&](int p, bool may_ready) {
    bool ready = (s.ready >> p) & 1;
    const bool placed = v.ports[p].placed;
    if (ready && ((newly[p] & BUTTON_B) || !placed || !may_ready))
      ready = false;
    else if (!ready && may_ready && placed && (newly[p] & BUTTON_START) && !(newly[p] & BUTTON_B))
      ready = true;
    s.ready = static_cast<u8>((s.ready & ~(1 << p)) | (ready ? 1 << p : 0));
  };

  if (v.solo)
  {
    // The queue's own character select, port 1 only. The B that cancels the search, and a Start
    // still held from leaving a room, are hidden from the game until released. That B still held
    // SOLO_LEAVE_FRAMES later reaches the game, which backs out as for any held B: hold B leaves
    // whether or not the player was searching.
    const u8 ready = s.ready;
    const bool swallowing = (s.flags & FLAG_SWALLOW_B) && (v.raw[0] & BUTTON_B);
    const u32 swallow_since = s.timer_start;  // TIMER_START's solo meaning
    const bool swallowing_start = (s.flags & FLAG_SWALLOW_START) && (v.raw[0] & BUTTON_START);
    // A new A on BACK while ready cancels the search, then the next frame passes it to the game
    // to back out (FLAG_BACK_A). The flag survives this frame being re-run.
    const bool back_press = Rules::CssHandOnBack(v.ports[0].hand_target, v.ports[0].hand_button) &&
                            (newly[0] & PAD_BUTTON_A);
    const bool back_a = back_press && ((ready & 1) || (s.flags & FLAG_BACK_A));
    if (swallowing_start)
      newly[0] = static_cast<u16>(newly[0] & ~BUTTON_START);
    s = State{};
    s.ready = ready & 1;
    // Never ready with the name list open: a room's select couldn't close it.
    follow(0, !v.ports[0].name_list);
    if (swallowing || ((ready & 1) && !(s.ready & 1) && (newly[0] & BUTTON_B)))
      s.flags |= FLAG_SWALLOW_B;
    if (s.flags & FLAG_SWALLOW_B)
    {
      s.timer_start =
          swallowing && swallow_since != 0 && swallow_since <= now ? swallow_since : now;
      if (now - s.timer_start >= static_cast<u32>(SOLO_LEAVE_FRAMES))
      {
        s.flags = static_cast<u8>(s.flags & ~FLAG_SWALLOW_B);
        s.timer_start = 0;
      }
    }
    if (swallowing_start)
      s.flags |= FLAG_SWALLOW_START;
    if (back_a)
    {
      s.ready = 0;
      s.flags |= FLAG_BACK_A;
    }
    return s;
  }

  // ---- The room's character select ----
  const bool both = (v.plugged & 3) == 3;
  // Port 2's pick from its synced queue identity, game 1 only.
  s.pick_set = v.game1 && v.pick2 && v.pick2->HasPick();
  if (s.pick_set)
  {
    s.pick_character = static_cast<u8>(v.pick2->character);
    s.pick_costume = static_cast<u8>(v.pick2->costume);
    s.pick_x = v.pick2->x;
    s.pick_y = v.pick2->y;
  }
  else
  {
    s.pick_character = s.pick_costume = 0;
    s.pick_x = s.pick_y = 0;
  }
  if (v.game1 && both && s.pick_set && !(s.locked & 2) && s.steer_start == 0)
    s.steer_start = now;
  if (s.steer_start != 0)
  {
    const u32 t = now >= s.steer_start ? now - s.steer_start : 0;
    s.steer = static_cast<u16>(std::min<u32>(t, 0xFFFF));
  }
  if (v.game1 && both)
  {
    // Game 1's picks lock once placed: the host's as is, the joiner's once steering placed it,
    // gave up, or had nothing to steer to.
    if (v.ports[0].placed)
      s.locked |= 1;
    const CssPort& two = v.ports[1];
    const bool steering_over = s.steer_start != 0 && s.steer >= STEER_LIMIT_FRAMES;
    const bool on_pick = s.pick_set && two.placed && two.character == s.pick_character;
    // Accept any colour when the pick's own is unavailable: a mirror match can't share port 1's
    // colour, and Random has no colour.
    const bool any_colour = (v.ports[0].placed && v.ports[0].character == s.pick_character &&
                             v.ports[0].costume == s.pick_costume) ||
                            s.pick_character == RANDOM_CHARACTER;
    if (on_pick && !(s.locked & 2) && s.costume_start == 0)
      s.costume_start = now;
    const bool costume_over =
        s.costume_start != 0 && now - std::min(now, s.costume_start) >= COSTUME_LIMIT_FRAMES;
    if (two.placed && (!s.pick_set || steering_over ||
                       (on_pick &&
                        (two.costume == s.pick_costume || any_colour || costume_over))))
    {
      s.locked |= 2;
    }
  }
  else if (!v.game1)
  {
    s.locked = 0;
  }

  if (!ReadyPhase(v))
  {
    // Not both players in, or ranked's character order still running: nobody is ready and there
    // is no timer. A casual timeout stays set while the players leave the room.
    s.ready = 0;
    s.flags &= FLAG_TIMED_OUT;
    if (!(s.flags & FLAG_TIMED_OUT))
      s.timeout_who = 0;
    s.timer_start = 0;
    s.timer = 0;
    return s;
  }
  // Restart the timer if its start is in the future or implausibly old (another frame
  // numbering). It also counts when untimed, for the alternating Start presses below.
  if (s.timer_start == 0 || s.timer_start > now ||
      now - s.timer_start > u32{v.ready_frames} * 4)
  {
    s.timer_start = now;
  }
  const u32 t = now - s.timer_start;
  s.timer = static_cast<u16>(std::min<u32>(t, 0xFFFF));
  if (s.flags & (FLAG_TIMED_OUT | FLAG_GO))
    return s;
  if (OrderLockedIn(v))
  {
    // Ranked later games: the character order's Starts were the lock-ins, so go to the stage
    // select as soon as both tokens are down.
    if (v.ports[0].placed && v.ports[1].placed)
    {
      s.ready = 3;
      s.flags |= FLAG_GO;
    }
    return s;
  }
  // A joiner whose pick is still being put in can't ready yet.
  const bool steering = v.game1 && s.pick_set && !(s.locked & 2);
  follow(0, true);
  follow(1, !steering);
  if ((s.ready & 3) == 3)
  {
    s.flags |= FLAG_GO;
  }
  else if (Timed(v) && t >= v.ready_frames)
  {
    if (v.ranked)
    {
      // Ranked continues with the current picks. If a character is missing, the no-show rule
      // (OnlineRules.h) decides.
      if (v.ports[0].placed && v.ports[1].placed)
        s.flags |= FLAG_GO;
    }
    else
    {
      s.flags |= FLAG_TIMED_OUT;
      s.timeout_who = static_cast<u8>(~s.ready & 3);
    }
  }
  return s;
}

bool SkipHold::Step(bool may_skip, bool held, int skip_frames)
{
  if (!may_skip || !held)
  {
    frames = 0;
    sent = false;
    return false;
  }
  ++frames;
  if (frames >= skip_frames && !sent)
  {
    sent = true;
    return true;
  }
  return false;
}

float SkipHold::Progress(int skip_frames) const
{
  if (sent || frames < SKIP_BAR_FRAMES)
    return 0;
  return std::min(1.0f, static_cast<float>(frames) / skip_frames);
}

std::optional<s64> ToFixed(float value)
{
  const u32 bits = std::bit_cast<u32>(value);
  const u32 exponent = (bits >> 23) & 0xFF;
  if (exponent == 0xFF)
    return std::nullopt;  // an infinity or a NaN
  if (exponent == 0)
    return 0;  // zero or a denormal (far below one FIXED_ONE)
  // value = mantissa * 2^(exponent - 150), so value * 2^FIXED_BITS = mantissa * 2^shift.
  const u64 mantissa = (bits & 0x7FFFFF) | 0x800000;
  const int shift = static_cast<int>(exponent) - 150 + FIXED_BITS;
  u64 magnitude;
  if (shift >= 0)
  {
    if (shift > 16)
      return std::nullopt;  // 2^40 and more: far past FIXED_LIMIT
    magnitude = mantissa << shift;
  }
  else
  {
    magnitude = shift <= -24 ? 0 : mantissa >> -shift;  // truncated toward zero
  }
  if (magnitude >= static_cast<u64>(FIXED_LIMIT))
    return std::nullopt;
  const s64 v = static_cast<s64>(magnitude);
  return (bits >> 31) ? -v : v;
}

u64 IntSqrt(u64 n)
{
  // Digit by digit, two bits at a time.
  u64 root = 0;
  u64 bit = u64{1} << 62;
  while (bit > n)
    bit >>= 2;
  while (bit != 0)
  {
    if (n >= root + bit)
    {
      n -= root + bit;
      root = (root >> 1) + bit;
    }
    else
    {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

std::pair<u8, u8> SteerToward(float x, float y, float tx, float ty)
{
  constexpr std::pair<u8, u8> centred{static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_X),
                                      static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_Y)};
  const std::optional<s64> fx = ToFixed(x), fy = ToFixed(y), ftx = ToFixed(tx), fty = ToFixed(ty);
  if (!fx || !fy || !ftx || !fty)
    return centred;
  // In FIXED_ONEs: |dx|, |dy| < 2^29, so dx * dx + dy * dy < 2^59.
  const s64 dx = *ftx - *fx, dy = *fty - *fy;
  const s64 d = static_cast<s64>(IntSqrt(static_cast<u64>(dx * dx) + static_cast<u64>(dy * dy)));
  if (d * 10 < FIXED_ONE)  // within 0.1
    return centred;
  // Full tilt from 3 units out, slower nearer. The larger component is at least 30, just past
  // the game's dead zone (about 0.06 units a frame), so the hand always closes in. Components are
  // in FIXED_ONEs (|dx * mag| < 2^29 * 2^23); divisions truncate toward zero.
  const s64 mag = std::clamp(100 * d / 3, 30 * FIXED_ONE, 100 * FIXED_ONE);
  s64 cx = dx * mag / d, cy = dy * mag / d;
  const s64 larger = std::max(cx < 0 ? -cx : cx, cy < 0 ? -cy : cy);
  if (larger > 0 && larger < 30 * FIXED_ONE)
  {
    cx = cx * (30 * FIXED_ONE) / larger;
    cy = cy * (30 * FIXED_ONE) / larger;
  }
  // Round centre + c half up, matching lround for positive values (the shift is a floor).
  const auto axis = [](s64 c, u8 centre) {
    const s64 v = (static_cast<s64>(centre) * FIXED_ONE + c + FIXED_ONE / 2) >> FIXED_BITS;
    return static_cast<u8>(std::clamp<s64>(v, 1, 255));
  };
  return {axis(cx, GCPadStatus::MAIN_STICK_CENTER_X), axis(cy, GCPadStatus::MAIN_STICK_CENTER_Y)};
}

bool HandStill(float x, float y, float prev_x, float prev_y)
{
  const std::optional<s64> fx = ToFixed(x), fy = ToFixed(y), px = ToFixed(prev_x),
                           py = ToFixed(prev_y);
  if (!fx || !fy || !px || !py)
    return false;
  // |d| < 0.05, written as 20 * |d| < 1.
  const auto under = [](s64 d) { return 20 * (d < 0 ? -d : d) < FIXED_ONE; };
  return under(*fx - *px) && under(*fy - *py);
}

Masks Gate(const View& v, const State& s)
{
  Masks masks{};
  if (!v.queue2 || !v.css)
    return masks;
  if (v.solo)
  {
    // Port 1 only. Start (ready) never reaches the game. While ready, B only cancels the search
    // (until held SOLO_LEAVE_FRAMES). Z is the game's own.
    masks[0].buttons =
        BUTTON_START | ((s.ready & 1) || (s.flags & FLAG_SWALLOW_B) ? BUTTON_B : 0);
    // A on BACK while ready first cancels the search, then reaches the game a frame later
    // (FLAG_BACK_A), so the app hears `orca queue unready` before the game backs out. The stick is
    // centred while a new A is down so the hand stays on BACK. An A already held when the hand
    // arrived leaves the stick free.
    if (Rules::CssHandOnBack(v.ports[0].hand_target, v.ports[0].hand_button))
    {
      if (s.ready & 1)
      {
        masks[0].buttons |= PAD_BUTTON_A;
        if (!(v.raw[0] & PAD_BUTTON_A))
          masks[0].a_centres_stick = true;
      }
      if (s.flags & FLAG_BACK_A)
      {
        masks[0].press |= PAD_BUTTON_A;
        masks[0].a_centres_stick = true;
      }
    }
    for (int port = 1; port < Rollback::InputGate::PORTS; ++port)
      masks[port] = ALL;
    return masks;
  }
  // Start is Orca's (the go). Z reaches the game, locked in or not: Project+ reads it as the
  // select ends for the AltZ costume, which no lock covers (as R's AltR never was masked).
  masks[0].buttons = BUTTON_START;
  masks[1].buttons = BUTTON_START;
  masks[2] = ALL;
  masks[3] = ALL;
  if (s.flags & FLAG_TIMED_OUT)
  {
    masks[0] = ALL;
    masks[1] = ALL;
    return masks;
  }
  if (v.game1)
  {
    // Game 1's characters are locked once in: no token pickup, no costume change.
    for (int p = 0; p < 2; ++p)
    {
      if (s.locked & (1 << p))
        masks[p].buttons |= PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y;
    }
    // Steer port 2's pick into place. Its own controller is ignored meanwhile.
    if (s.pick_set && !(s.locked & 2) && s.steer < STEER_LIMIT_FRAMES && (v.plugged & 2))
    {
      Mask& two = masks[1];
      two = ALL;
      const CssPort& p = v.ports[1];
      const bool edge = (s.steer & 3) == 0;  // press every fourth frame, released between
      two.steer = s.steer >= STEER_SETTLE_FRAMES;
      if (!two.steer)
      {
        // Stick centred while the game calibrates the new pad (ALL centres it).
      }
      else if (p.placed)
      {
        // On the wrong character, pick the token back up. On the right one in the wrong costume,
        // press X (except on Random, which accepts any).
        if (p.character != s.pick_character)
          two.press = edge ? BUTTON_B : 0;
        else if (p.costume != s.pick_costume && s.pick_character != RANDOM_CHARACTER)
          two.press = edge ? PAD_BUTTON_X : 0;
      }
      else if (p.hand_target == Rules::CSS_HAND_GRID_HOLDING && p.character == s.pick_character)
      {
        // Over the pick: press A once the hand has nearly stopped. The character read lags the
        // hand by a frame, so a moving hand could drop the token on the next character.
        const bool still = HandStill(p.hand_x, p.hand_y, v.hand2_prev_x, v.hand2_prev_y);
        two.press = edge && still ? PAD_BUTTON_A : 0;
      }
      else
      {
        const auto [x, y] = SteerToward(p.hand_x, p.hand_y, s.pick_x, s.pick_y);
        two.steer_x = x;
        two.steer_y = y;
      }
    }
  }
  // A ready player's pick is frozen (no A, X or Y). B only un-readies; the game never sees it,
  // because the online rules only pass a press's first frame.
  for (int p = 0; p < 2; ++p)
  {
    if (s.ready & (1 << p))
      masks[p].buttons |= PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y;
  }
  if (s.flags & FLAG_GO)
  {
    if ((s.timer & 1) == 0)
      masks[0].press |= BUTTON_START;
  }
  return masks;
}
}  // namespace Orca::UX::Queue
