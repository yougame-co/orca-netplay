// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Queue.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/CharOrder.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/Overlay.h"

namespace Orca::UX::Queue
{
namespace
{
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;
using Rollback::InputGate::Masks;

// Brawl's character select (BrawlHeaders' muSelCharPlayerArea).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x04;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_KIND = 0x1B4;
constexpr u32 AREA_CHARACTER = 0x1B8;
constexpr u32 AREA_COSTUME = 0x1BC;  // m_charColorNo
constexpr u32 AREA_IN_HAND = 0x1F8;  // u8: 1 the token is in the hand, 0 down
constexpr u32 AREA_FLYING = 0x1F9;   // u8: 1 while the token flies
constexpr u32 KIND_HUMAN = 1;

constexpr u16 BUTTON_START = PAD_BUTTON_START;
constexpr u16 BUTTON_B = PAD_BUTTON_B;
constexpr u16 BUTTON_Z = PAD_TRIGGER_Z;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

void Put32(std::vector<u8>& out, u32 v)
{
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

u32 Get32(const std::vector<u8>& b, size_t at)
{
  return u32(b[at]) << 24 | u32(b[at + 1]) << 16 | u32(b[at + 2]) << 8 | u32(b[at + 3]);
}

// Writes only what differs, counting the bytes changed.
struct Writer
{
  GuestMemory& m;
  int changed = 0;
  void U8(u32 a, u8 v)
  {
    if (!m.Valid(a) || m.Read8(a) == v)
      return;
    m.Write8(a, v);
    ++changed;
  }
  void U16(u32 a, u16 v)
  {
    U8(a, static_cast<u8>(v >> 8));
    U8(a + 1, static_cast<u8>(v));
  }
  void U32(u32 a, u32 v)
  {
    U16(a, static_cast<u16>(v >> 16));
    U16(a + 2, static_cast<u16>(v));
  }
};

bool RegionMapped(const GuestMemory& m)
{
  return m.Valid(MatchBlock::QUEUE) && m.Valid(REGION_END - 1);
}

std::string Clock(int frames)
{
  const int seconds = (std::max(frames, 0) + 59) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}
}  // namespace

// ---- The queue identity ----

std::vector<u8> EncodeIdentity(const Identity& id)
{
  std::vector<u8> out;
  out.push_back(1);
  const u16 rating = id.rating < 0 || id.rating > 0xFFFE ? 0xFFFF : static_cast<u16>(id.rating);
  out.push_back(static_cast<u8>(rating >> 8));
  out.push_back(static_cast<u8>(rating));
  out.push_back(id.HasPick() && id.character <= 0xFE ? static_cast<u8>(id.character) : 0xFF);
  out.push_back(static_cast<u8>(std::clamp(id.costume, 0, 0xFF)));
  Put32(out, std::bit_cast<u32>(id.x));
  Put32(out, std::bit_cast<u32>(id.y));
  return out;
}

std::optional<Identity> DecodeIdentity(const std::vector<u8>& b)
{
  // Newer versions may append bytes; only the known ones are read.
  if (b.size() < 13 || b[0] != 1)
    return std::nullopt;
  Identity id;
  const u16 rating = static_cast<u16>(b[1] << 8 | b[2]);
  id.rating = rating == 0xFFFF ? -1 : rating;
  id.character = b[3] == 0xFF ? -1 : b[3];
  id.costume = b[4];
  id.x = std::bit_cast<float>(Get32(b, 5));
  id.y = std::bit_cast<float>(Get32(b, 9));
  // An out-of-range position is dropped, and the player picks by hand.
  if (!std::isfinite(id.x) || !std::isfinite(id.y) || std::fabs(id.x) > 100 ||
      std::fabs(id.y) > 100)
  {
    id.character = -1;
    id.x = id.y = 0;
  }
  return id;
}

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
    // still held from leaving a room, are hidden from the game until released.
    const u8 ready = s.ready;
    const bool swallowing = (s.flags & FLAG_SWALLOW_B) && (v.raw[0] & BUTTON_B);
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
      now - s.timer_start > static_cast<u32>(READY_FRAMES) * 4)
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
  else if (Timed(v) && t >= static_cast<u32>(READY_FRAMES))
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

bool SkipHold::Step(bool may_skip, bool z_held)
{
  if (!may_skip || !z_held)
  {
    frames = 0;
    sent = false;
    return false;
  }
  ++frames;
  if (frames >= SKIP_FRAMES && !sent)
  {
    sent = true;
    return true;
  }
  return false;
}

float SkipHold::Progress() const
{
  if (sent || frames <= 0)
    return 0;
  return std::min(1.0f, static_cast<float>(frames) / SKIP_FRAMES);
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
    // Port 1 only. Start (ready) and Z (skip) never reach the game. While ready, B only cancels
    // the search.
    masks[0].buttons =
        BUTTON_START | BUTTON_Z | ((s.ready & 1) || (s.flags & FLAG_SWALLOW_B) ? BUTTON_B : 0);
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
  masks[0].buttons = BUTTON_START | BUTTON_Z;
  masks[1].buttons = BUTTON_START | BUTTON_Z;
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

// ---- The host's own pick after a fresh start ----

OwnStep StepOwnPick(const CssPort& port, const Identity& pick, OwnSteer* st)
{
  OwnStep out;
  const int t = st->frames++;
  const float prev_x = st->has_prev ? st->prev_x : port.hand_x;
  const float prev_y = st->has_prev ? st->prev_y : port.hand_y;
  st->has_prev = true;
  st->prev_x = port.hand_x;
  st->prev_y = port.hand_y;
  if (!pick.HasPick() || t >= STEER_LIMIT_FRAMES)
  {
    out.done = true;
    return out;
  }
  // Press every fourth frame, released between, as the joiner's steering does.
  const bool edge = (t & 3) == 0;
  // The select's first frames: the hand may still be coming in.
  if (t < STEER_SETTLE_FRAMES)
    return out;
  if (port.placed)
  {
    if (port.character != pick.character)
    {
      st->costume_from = -1;
      out.buttons = edge ? BUTTON_B : 0;
      return out;
    }
    if (port.costume == pick.costume || pick.character == RANDOM_CHARACTER)
    {
      out.done = true;
      return out;
    }
    if (st->costume_from < 0)
      st->costume_from = t;
    if (t - st->costume_from >= COSTUME_LIMIT_FRAMES)
    {
      out.done = true;
      return out;
    }
    out.buttons = edge ? PAD_BUTTON_X : 0;
    return out;
  }
  if (port.hand_target == Rules::CSS_HAND_GRID_HOLDING && port.character == pick.character)
  {
    // Over the pick: A once the hand has nearly stopped (the character read lags the hand).
    out.buttons = edge && HandStill(port.hand_x, port.hand_y, prev_x, prev_y) ? PAD_BUTTON_A : 0;
    return out;
  }
  const auto [x, y] = SteerToward(port.hand_x, port.hand_y, pick.x, pick.y);
  out.stick_x = x;
  out.stick_y = y;
  return out;
}

namespace
{
struct OwnPickLocal
{
  bool armed = false;
  Identity pick;
  OwnSteer steer;
  // The first first-run frame seen while armed (-1: none yet).
  int first = -1;
  std::optional<GCPadStatus> pad;
};
OwnPickLocal& Own()
{
  static OwnPickLocal own;
  return own;
}

// First runs only: one frame of the host's own pick.
void SteerOwnPickFrame(const View& v, int frame)
{
  OwnPickLocal& o = Own();
  o.pad.reset();
  if (!o.armed)
    return;
  if (o.first < 0 || frame < o.first)
    o.first = frame;
  if (!(v.queue2 && !v.solo && v.css && v.game1 && v.ports[0].readable))
  {
    if (frame - o.first >= STEER_LIMIT_FRAMES)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port 1's own pick never met the room's character "
                               "select; the player picks by hand",
                     frame);
      o.armed = false;
    }
    return;
  }
  const OwnStep step = StepOwnPick(v.ports[0], o.pick, &o.steer);
  if (step.done)
  {
    const CssPort& one = v.ports[0];
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port 1's own pick is {} (character {:#x} costume {}, "
                             "{} frames)",
                   frame,
                   one.placed && one.character == o.pick.character ? "back in" :
                                                                     "left to the player",
                   one.character, one.costume, o.steer.frames);
    o.armed = false;
    return;
  }
  GCPadStatus pad;
  pad.button = step.buttons;
  pad.stickX = step.stick_x;
  pad.stickY = step.stick_y;
  pad.substickX = GCPadStatus::C_STICK_CENTER_X;
  pad.substickY = GCPadStatus::C_STICK_CENTER_Y;
  pad.triggerLeft = 0;
  pad.triggerRight = 0;
  pad.isConnected = true;
  o.pad = pad;
}
}  // namespace

void ArmOwnPick(const Identity& pick)
{
  OwnPickLocal& o = Own();
  o = {};
  o.armed = pick.HasPick();
  o.pick = pick;
  if (o.armed)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Queue: port 1's own pick (character {:#x} costume {}) goes back on "
                             "the room's character select",
                   pick.character, pick.costume);
  }
}

void DisarmOwnPick()
{
  Own() = {};
}

bool OwnPickArmed()
{
  return Own().armed;
}

std::optional<GCPadStatus> OwnPickPad()
{
  return Own().armed ? Own().pad : std::nullopt;
}

// ---- Where a pick's square is ----

std::optional<PickPlaces::Placed> PickPlaces::Frame(int port, const CssPort& css)
{
  if (port < 0 || port >= 2)
    return std::nullopt;
  Hover& hover = m_hover[port];
  if (!css.placed && css.hand_target == Rules::CSS_HAND_GRID_HOLDING &&
      css.character != NO_CHARACTER)
  {
    if (hover.character != css.character)
    {
      hover.character = css.character;
      hover.first_x = css.hand_x;
      hover.first_y = css.hand_y;
    }
    hover.last_x = css.hand_x;
    hover.last_y = css.hand_y;
  }
  std::optional<Placed> out;
  if (css.placed && !m_was_placed[port])
  {
    Placed p;
    p.character = css.character;
    p.costume = css.costume;
    p.learned = hover.character == css.character;
    p.last_x = hover.last_x;
    p.last_y = hover.last_y;
    p.first_x = hover.first_x;
    p.first_y = hover.first_y;
    if (p.learned)
    {
      m_places[css.character] = {(hover.first_x + hover.last_x) / 2,
                                 (hover.first_y + hover.last_y) / 2};
    }
    out = p;
  }
  m_was_placed[port] = css.placed;
  return out;
}

std::optional<std::pair<float, float>> PickPlaces::Find(int character) const
{
  const auto it = m_places.find(character);
  if (it == m_places.end())
    return std::nullopt;
  return it->second;
}

Identity Publishable(Identity pick, const std::optional<std::pair<float, float>>& place)
{
  if (!pick.HasPick())
    return pick;
  if (!place)
  {
    pick.character = -1;
    pick.x = pick.y = 0;
    return pick;
  }
  pick.x = place->first;
  pick.y = place->second;
  return pick;
}

// ---- Memory ----

bool CssTaskReadable(const GuestMemory& m)
{
  if (ReadSceneName(m) != "scSelctCharacter" || !Pointer(m, SCENE_MANAGER))
    return false;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return false;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return false;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  return Pointer(m, task + TASK_AREAS + 4);
}

View ReadView(const GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  View v;
  const Rules::Header h = Rules::ReadHeader(m);
  if (!h.Queue2() || !RegionMapped(m))
    return v;
  v.queue2 = true;
  v.solo = h.Solo();
  v.ranked = h.mode == Rules::Mode::Ranked;
  v.game1 = (m.Read8(MatchBlock::FOUGHT) & 1) == 0;
  v.plugged = m.Read8(MatchBlock::PLUGGED);
  for (int p = 0; p < 2; ++p)
  {
    v.raw[p] = m.Read16(RAW + 2 * static_cast<u32>(p));
    v.raw_prev[p] = m.Read16(RAW_PREV + 2 * static_cast<u32>(p));
  }
  v.hand2_prev_x = std::bit_cast<float>(m.Read32(HAND_PREV));
  v.hand2_prev_y = std::bit_cast<float>(m.Read32(HAND_PREV + 4));
  if (v.ranked && !v.game1)
  {
    const CharOrder::State order = CharOrder::ReadState(m);
    v.order_done = order.step != CharOrder::Step::First && order.step != CharOrder::Step::Second;
    v.order_locked = CharOrder::LockedPorts(CharOrder::ReadSet(m), CharOrder::ReadCss(m), order);
    v.order_locked_in = order.step == CharOrder::Step::Done && v.order_locked == 3;
  }
  for (const Events::PortInfo& info : ports)
  {
    if (info.port != 1 || info.queue.empty())
      continue;
    if (const auto id = DecodeIdentity(info.queue); id && id->HasPick())
      v.pick2 = id;
  }
  if (ReadSceneName(m) != "scSelctCharacter" || !Pointer(m, SCENE_MANAGER))
    return v;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return v;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return v;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  if (!Pointer(m, task + TASK_AREAS + 4))
    return v;
  v.css = true;
  for (u32 i = 0; i < 2; ++i)
  {
    const u32 area = m.Read32(task + TASK_AREAS + 4 * i);
    CssPort& p = v.ports[i];
    if (!Pointer(m, area + AREA_KIND) || !Pointer(m, area + AREA_CHARACTER) ||
        !Pointer(m, area + AREA_COSTUME) || !m.Valid(area + AREA_FLYING))
    {
      continue;
    }
    p.readable = true;
    p.human = m.Read32(area + AREA_KIND) == KIND_HUMAN;
    p.character = static_cast<int>(m.Read32(area + AREA_CHARACTER));
    p.costume = static_cast<int>(m.Read32(area + AREA_COSTUME));
    p.placed = p.human && p.character != NO_CHARACTER && m.Read8(area + AREA_IN_HAND) == 0 &&
               m.Read8(area + AREA_FLYING) == 0;
    p.name_list = Rules::ReadCssNameList(m, static_cast<int>(i));
    const Rules::CssHand hand = Rules::ReadCssHand(m, static_cast<int>(i));
    if (hand.valid)
    {
      p.hand_target = hand.target;
      p.hand_button = hand.button;
      p.hand_x = hand.x;
      p.hand_y = hand.y;
    }
  }
  return v;
}

State ReadState(const GuestMemory& m)
{
  State s;
  if (!RegionMapped(m))
    return s;
  s.ready = m.Read8(READY);
  s.flags = m.Read8(FLAGS);
  s.timeout_who = m.Read8(TIMEOUT_WHO);
  s.locked = m.Read8(LOCKED);
  s.timer_start = m.Read32(TIMER_START);
  s.timer = m.Read16(TIMER);
  s.steer = m.Read16(STEER);
  s.steer_start = m.Read32(STEER_START);
  s.costume_start = m.Read32(COSTUME_START);
  s.pick_set = m.Read8(PICK) == 1;
  s.pick_character = m.Read8(PICK + 1);
  s.pick_costume = m.Read8(PICK + 2);
  s.pick_x = std::bit_cast<float>(m.Read32(PICK + 4));
  s.pick_y = std::bit_cast<float>(m.Read32(PICK + 8));
  return s;
}

int WriteState(GuestMemory& m, const State& s)
{
  if (!RegionMapped(m))
    return 0;
  Writer w{m};
  w.U8(READY, s.ready);
  w.U8(FLAGS, s.flags);
  w.U8(TIMEOUT_WHO, s.timeout_who);
  w.U8(LOCKED, s.locked);
  w.U32(TIMER_START, s.timer_start);
  w.U16(TIMER, s.timer);
  w.U16(STEER, s.steer);
  w.U32(STEER_START, s.steer_start);
  w.U32(COSTUME_START, s.costume_start);
  w.U8(PICK, s.pick_set ? 1 : 0);
  w.U8(PICK + 1, s.pick_character);
  w.U8(PICK + 2, s.pick_costume);
  w.U8(PICK + 3, 0);
  w.U32(PICK + 4, std::bit_cast<u32>(s.pick_x));
  w.U32(PICK + 8, std::bit_cast<u32>(s.pick_y));
  return w.changed;
}

int Latch(GuestMemory& m, const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  // Runs under any header that locks the rules, since OnlineRules.h and CharOrder.h read RAW too,
  // and with the character order's test knob on its character select.
  if (!RegionMapped(m) ||
      !(Rules::ReadHeader(m).Locked() ||
        (CharOrder::TestActive() && MatchBlock::Present(m) && CharOrder::ReadCss(m).on_css)))
  {
    return 0;
  }
  Writer w{m};
  for (u32 p = 0; p < 2; ++p)
  {
    const u16 old = m.Read16(RAW + 2 * p);
    const u16 now =
        raw[p] && raw[p]->isConnected ? static_cast<u16>(raw[p]->button & Rollback::InputGate::ALL_BUTTONS) :
                                        0;
    w.U16(RAW_PREV + 2 * p, old);
    w.U16(RAW + 2 * p, now);
  }
  // Port 2's hand at frame start; the steering's A waits for it to be still.
  const Rules::CssHand hand = Rules::ReadCssHand(m, 1);
  w.U32(HAND_PREV, std::bit_cast<u32>(hand.valid ? hand.x : 0.0f));
  w.U32(HAND_PREV + 4, std::bit_cast<u32>(hand.valid ? hand.y : 0.0f));
  return w.changed;
}

// ---- The app's side and the local lines ----

namespace
{
std::mutex s_mutex;
bool s_active = false;
bool s_ranked = false;
int s_rating = -1;
// This player's pick on the queue's own character select, and where it was placed.
Identity s_pick;
std::atomic<bool> s_solo_ready{false};
std::atomic<bool> s_on_solo_css{false};
std::atomic<bool> s_on_queue_css{false};
std::atomic<bool> s_clear_ready{false};
// For the log. Guarded by s_mutex.
std::string s_clear_ready_why;
std::atomic<bool> s_last_timeout_mine{false};

// Local state updated on first runs only: what was printed, the skip hold, token positions.
struct Local
{
  bool printed_ready = false;
  Identity printed;
  SkipHold skip;
  // Each character's place on the select, learned from the hands (PickPlaces).
  PickPlaces places;
};
Local& L()
{
  static Local l;
  return l;
}

// The casual timeout per frame, until that frame is confirmed (like OnlineRules' no-show).
struct TimeoutTracker
{
  std::map<int, u8> pending;  // bit 7: timed out; bits 0-1: who wasn't ready
  u64 resyncs = 0;
  bool started = false;
  u8 last = 0;
  int last_frame = -1;
};
TimeoutTracker& Timeouts()
{
  static TimeoutTracker t;
  return t;
}

void PublishIdentity()
{
  Identity id;
  {
    std::lock_guard lk(s_mutex);
    id = s_pick;
    id.rating = s_rating;
  }
  Orca::Events::SetOwnQueue(id.rating < 0 && !id.HasPick() ? std::vector<u8>{} :
                                                              EncodeIdentity(id));
}

std::string NameFor(const std::vector<Events::PortInfo>& ports, int port, int* rating)
{
  for (const Events::PortInfo& p : ports)
  {
    if (p.port != port)
      continue;
    if (rating)
    {
      if (const auto id = DecodeIdentity(p.queue))
        *rating = id->rating;
    }
    if (!p.name.empty())
      return p.name;
  }
  return fmt::format("Player {}", port + 1);
}

}  // namespace

Text TextFor(const View& v, const State& s, const std::vector<Events::PortInfo>& ports, int local,
             float skip_progress)
{
  Text out;
  std::string& line = out.line;
  std::string& note = out.note;
  std::array<std::string, 4>& labels = out.labels;
  std::array<bool, 4>& locked = out.locked;
  const std::string mode = v.ranked ? "Ranked" : "Casual";
  if (v.queue2 && v.css && v.solo)
  {
    if (s.ready & 1)
    {
      line = fmt::format("{} · Searching for an opponent…", mode);
      note = "B to cancel";
    }
    else
    {
      line = mode;
      note = "Pick your character, then press Start to search";
    }
  }
  else if (v.queue2 && v.css)
  {
    // Each player's name by their panel, with no rating. A locked-in player gets a LOCKED IN
    // badge.
    for (int p = 0; p < 2; ++p)
    {
      if (!(v.plugged & (1 << p)))
        continue;
      labels[p] = NameFor(ports, p, nullptr);
      locked[p] = ((s.ready | v.order_locked) >> p) & 1;
    }
    const int other = local == 0 ? 1 : 0;
    const std::string them = NameFor(ports, other, nullptr);
    if ((v.plugged & 3) != 3)
    {
      line = fmt::format("{} · Opponent found", mode);
      note = fmt::format("Waiting for {}…", them);
    }
    else if (s.flags & FLAG_TIMED_OUT)
    {
      line = "Time's up";
      note = (s.timeout_who >> local) & 1 ? "You didn't ready up" :
                                            fmt::format("{} didn't ready up", them);
    }
    else if (s.flags & FLAG_GO)
    {
      line = fmt::format("{} · Both locked in · Here we go", mode);
    }
    else if (ReadyPhase(v) && !OrderLockedIn(v))
    {
      const bool mine = (s.ready >> local) & 1;
      const bool theirs = (s.ready >> other) & 1;
      // In game 1 the picks are already in, so only Start. Later games pick with A, then Start.
      const bool picks = !v.game1;
      const std::string clock = Timed(v) ? " · " + Clock(READY_FRAMES - s.timer) : std::string();
      if (mine && theirs)
        line = "Both locked in";
      else if (mine)
        line = fmt::format("You're locked in · {}: {}{}", them,
                           picks ? "pick and press Start" : "press Start", clock);
      else if (theirs && !picks)
        line = fmt::format("{} locked in · Press Start to lock in{}", them, clock);
      else if (theirs)
        line = fmt::format("{} locked in · Your pick: {}{}", them,
                           v.ports[local].placed ? "Start locks it in" :
                                                   "A on a character, then Start",
                           clock);
      else
        line = (picks ? (v.ports[local].placed ? "Start locks in your pick · B to change" :
                                                  "Pick with A, then press Start to lock in") :
                        "Press Start to lock in") +
               clock;
      note = mine && picks ? "B to change" : "";
      // Casual only. Ranked never offers the skip.
      if (MaySkip(v, s) && skip_progress > 0)
      {
        const int bars = std::clamp(static_cast<int>(skip_progress * 10), 0, 10);
        note = "Finding someone else " + std::string(static_cast<size_t>(bars), '|') +
               std::string(static_cast<size_t>(10 - bars), '.');
      }
      else if (MaySkip(v, s))
      {
        note += note.empty() ? "Hold Z to find someone else" : " · Hold Z to find someone else";
      }
    }
  }
  return out;
}

void Begin(bool ranked)
{
  {
    std::lock_guard lk(s_mutex);
    s_active = true;
    s_ranked = ranked;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Queue: on the {} queue's own character select", ranked ? "ranked" :
                                                                                       "casual");
}

void End()
{
  {
    std::lock_guard lk(s_mutex);
    s_active = false;
    s_pick = {};
  }
  s_solo_ready = false;
  PublishIdentity();
}

bool Active()
{
  std::lock_guard lk(s_mutex);
  return s_active;
}

bool Ranked()
{
  std::lock_guard lk(s_mutex);
  return s_ranked;
}

void SetOwnRating(int rating)
{
  {
    std::lock_guard lk(s_mutex);
    s_rating = rating;
  }
  PublishIdentity();
}

int OwnRating()
{
  std::lock_guard lk(s_mutex);
  return s_rating;
}

bool SoloReady()
{
  return s_solo_ready;
}

Identity OwnIdentity()
{
  std::lock_guard lk(s_mutex);
  Identity id = s_pick;
  id.rating = s_rating;
  return id;
}

bool OnSoloCss()
{
  return s_on_solo_css;
}

bool OnQueueCss()
{
  return s_on_queue_css;
}

void AfterRestore(bool keep_ready, const char* why)
{
  if (keep_ready)
    return;
  {
    std::lock_guard lk(s_mutex);
    s_clear_ready_why = why && *why ? why : "after the room";
  }
  s_clear_ready = true;
}

void ClearReady()
{
  s_clear_ready = true;
}

bool TakeTimeoutWasMine()
{
  return s_last_timeout_mine.exchange(false);
}

void ConfirmTimeout(int confirmed, int local_port)
{
  TimeoutTracker& t = Timeouts();
  for (auto it = t.pending.begin(); it != t.pending.end() && it->first <= confirmed;
       it = t.pending.erase(it))
  {
    if ((it->second & 0x80) && !(t.last & 0x80))
    {
      const bool mine = local_port >= 0 && local_port < 2 && ((it->second >> local_port) & 1);
      s_last_timeout_mine = mine;
      NOTICE_LOG_FMT(ROLLBACK, "Queue: the ready timer ran out at frame {}: orca queue timeout {}",
                     it->first, mine ? "me" : "them");
      Orca::Status::Line(mine ? "orca queue timeout me" : "orca queue timeout them");
    }
    t.last = it->second;
    t.last_frame = it->first;
  }
}

void ResetTimeouts()
{
  Timeouts() = {};
  s_last_timeout_mine = false;
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory m(guard);
  // After a room that should not keep this player ready, clear ready at a solo frame.
  std::string cleared;
  const bool replay_clear = Orca::Net::ReplayScope::Playing() &&
                            Orca::Net::ReplayScope::Current()->clear_ready;
  if (replay_clear || (alone && !resimulating && s_clear_ready.exchange(false)))
  {
    if (auto* replay = Orca::Net::ReplayScope::Current(); replay && !Orca::Net::ReplayScope::Playing())
      replay->clear_ready = true;
    const Rules::Header h = Rules::ReadHeader(m);
    if (h.Solo() && RegionMapped(m))
    {
      // A Start still held from the room's last screen must be released before it counts.
      m.Write8(FLAGS, static_cast<u8>(m.Read8(FLAGS) | FLAG_SWALLOW_START));
      if (m.Read8(READY) & 1)
      {
        m.Write8(READY, static_cast<u8>(m.Read8(READY) & ~1));
        std::string why;
        {
          std::lock_guard lk(s_mutex);
          why = s_clear_ready_why;
        }
        NOTICE_LOG_FMT(ROLLBACK, "Queue: not ready after the room ({})", why);
        cleared = "cleared after the room: " + why;
      }
    }
  }
  const View v = ReadView(m, ports);
  if (!resimulating)
  {
    s_on_queue_css = v.queue2 && v.css;
    SteerOwnPickFrame(v, frame);
  }
  const State before = ReadState(m);
  const State after = Advance(v, before, frame);
  if (after != before)
  {
    WriteState(m, after);
    if (!resimulating)
    {
      if ((after.flags & FLAG_GO) && !(before.flags & FLAG_GO))
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: both ready, on to the stage", frame);
      if ((after.locked & 2) && !(before.locked & 2))
      {
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port 2's pick is in (character {:#x}, costume "
                       "{}, {} frames)",
                       frame, v.ports[1].character, v.ports[1].costume, after.steer);
      }
      if (!v.solo && after.ready != before.ready)
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: ready {:#x}", frame, after.ready);
    }
  }

  // Record the casual timeout for this frame until it is confirmed. A re-run replaces the first
  // run's value.
  TimeoutTracker& t = Timeouts();
  const u64 resyncs = Orca::Events::Resyncs();
  if (!t.started || t.resyncs != resyncs)
  {
    t = {};
    t.started = true;
    t.resyncs = resyncs;
  }
  if (frame > t.last_frame)
  {
    const u8 value = (after.flags & FLAG_TIMED_OUT) && v.queue2 && !v.solo ?
                         static_cast<u8>(0x80 | (after.timeout_who & 3)) : 0;
    if (Orca::Net::ReplayScope::NetworkPlaying())
    {
      t.pending.clear();
      t.last = value;
      t.last_frame = frame;
      s_last_timeout_mine = false;
    }
    else
      t.pending[frame] = value;
    while (t.pending.size() > 4096)
      t.pending.erase(t.pending.begin());
  }

  if (resimulating)
    return;
  if (after.pick_set && !(after.locked & 2) && after.steer_start != 0 && after.steer % 10 == 0 &&
      after.steer <= STEER_LIMIT_FRAMES)
  {
    const Rollback::InputGate::Masks g = Gate(v, after);
    const CssPort& two = v.ports[1];
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: steering port 2 ({} frames): hand {} at {:.2f}, "
                   "{:.2f} (was {:.2f}, {:.2f}) over {:#x}{}, stick {} {}, press {:#x}",
                   frame, after.steer, two.hand_target, two.hand_x, two.hand_y, v.hand2_prev_x,
                   v.hand2_prev_y, two.character, two.placed ? " (down)" : "", g[1].steer_x,
                   g[1].steer_y, g[1].press);
  }
  int local = 0;
  for (const Events::PortInfo& p : ports)
  {
    if (!p.remote)
      local = p.port;
  }
  Local& l = L();
  for (int p = 0; p < 2 && v.queue2 && v.css; ++p)
  {
    const auto placed = l.places.Frame(p, v.ports[p]);
    if (!placed)
      continue;
    if (placed->learned)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port {} put its token on character {:#x} costume "
                     "{} (the hand held it at {:.2f}, {:.2f}; over it from {:.2f}, {:.2f})",
                     frame, p + 1, placed->character, placed->costume, placed->last_x,
                     placed->last_y, placed->first_x, placed->first_y);
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port {}'s token is on character {:#x} costume {} "
                     "(no hand over it, so no place learned)",
                     frame, p + 1, placed->character, placed->costume);
    }
  }
  const bool solo_css = v.queue2 && v.solo && v.css;
  // Only a solo frame can set this. A non-solo frame (a pending `host` or `join`) leaves it.
  if (!solo_css)
    s_on_solo_css = false;
  else if (alone)
    s_on_solo_css = true;
  if (solo_css && alone)
  {
    const CssPort& own = v.ports[0];
    const bool ready = after.ready & 1;
    s_solo_ready = ready;
    Identity pick;
    pick.character = own.placed ? own.character : -1;
    pick.costume = own.costume;
    if (ready && (!l.printed_ready || l.printed.character != pick.character ||
                  l.printed.costume != pick.costume))
    {
      // Only a pick whose place is known is put back by steering; otherwise the player picks by
      // hand in the room.
      const Identity published = Publishable(pick, l.places.Find(pick.character));
      {
        std::lock_guard lk(s_mutex);
        s_pick = published;
      }
      PublishIdentity();
      l.printed_ready = true;
      l.printed = pick;
      const std::string text = fmt::format("orca queue ready {} {} {}", v.ranked ? "ranked" : "casual",
                                           pick.character, pick.costume);
      if (published.HasPick())
      {
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: {} (the hand at {:.1f}, {:.1f})", frame, text,
                       published.x, published.y);
      }
      else
      {
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: {} (its place is unknown: picked by hand in the "
                       "room)",
                       frame, text);
      }
      Orca::Status::Line(text);
    }
    else if (!ready && l.printed_ready)
    {
      l.printed_ready = false;
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: orca queue unready ({})", frame,
                     cleared.empty() ? UnreadyReason(v, before, after) : cleared);
      Orca::Status::Line("orca queue unready");
    }
  }
  else if (v.queue2 && !v.solo)
  {
    // In a room. Ready is printed again once back on the player's own character select.
    l.printed_ready = false;
    s_solo_ready = false;
  }
  else if (!v.queue2)
  {
    l.printed_ready = false;
    s_solo_ready = false;
  }

  // Skip: Z held 1.5 s on a casual room's character select, read from this player's own latched
  // input for the frame that just ran.
  const bool may_skip = MaySkip(v, after) && local >= 0 && local < 2;
  if (l.skip.Step(may_skip, may_skip && (v.raw[local] & BUTTON_Z) != 0))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: Z held {} frames: orca queue skip", frame,
                   l.skip.frames);
    Orca::Status::Line("orca queue skip");
  }
  Text text = TextFor(v, after, ports, local, l.skip.Progress());
  SetQueueLines(std::move(text.line), std::move(text.note), std::move(text.labels), text.locked);
}

Masks GateFrame(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const Rules::Header h = Rules::ReadHeader(m);
  if (!h.Queue2())
    return {};
  // The gate reads memory only. Port 2's pick comes from the region's copy (PICK).
  const View v = ReadView(m, {});
  return Gate(v, ReadState(m));
}

bool OwnsStart(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const Rules::Header h = Rules::ReadHeader(m);
  return h.Queue2() && !h.Solo() && ReadSceneName(m) == "scSelctCharacter";
}

void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory m(guard);
  Latch(m, raw);
}
}  // namespace Orca::UX::Queue
