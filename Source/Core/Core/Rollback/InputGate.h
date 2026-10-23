// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The input gate: per-port masks over what the game reads from its controllers, for rules that
// take input away from a player (e.g. whose turn it is in a stage strike, locked buttons).
//
// Determinism: a frame's masks are a pure function of emulated memory at the start of that frame.
// They are recomputed at every frame boundary on every pass (first runs, rollback re-runs, keyframe
// loads), after the snapshot and the frame's pads are set, and nothing is kept across a load. Masks
// apply only where the game reads its ports; pads sent over the network and recorded stay raw. So
// both machines mask the same pads the same way. See ORCA.md, "Input gate".

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Rollback::InputGate
{
constexpr int PORTS = 4;

// What one port's player may not do for a frame: masked buttons read released, masked sticks
// centred.
struct Mask
{
  // PAD_BUTTON_* and PAD_TRIGGER_* bits; other bits are ignored. Masking L, R, A or B also zeroes
  // its analog value.
  u16 buttons = 0;
  bool main_stick = false;
  bool c_stick = false;
  // Buttons forced pressed regardless of the player, e.g. a default pick when their timer runs
  // out. Applied after the mask; A, B, L and R also read fully pressed.
  u16 press = 0;
  // Buttons that read pressed while the player holds A. Combined with masking A, this turns A into
  // another button, e.g. X on the stage select so a rule sees the pick without the game acting on
  // it.
  u16 a_as = 0;
  // Forces the main stick to (steer_x, steer_y) regardless of the player, e.g. to move a joining
  // player's cursor to their pick. Applied after the mask.
  bool steer = false;
  u8 steer_x = GCPadStatus::MAIN_STICK_CENTER_X;
  u8 steer_y = GCPadStatus::MAIN_STICK_CENTER_Y;
  // Centres the main stick while A is held (raw or forced), so a cursor stays where it is through
  // the A press that selects. Applied last.
  bool a_centres_stick = false;
  // Buttons that read released while the player holds both L and R (TriggerHeld), e.g. a cheat
  // menu's L+R+Down chord. L and R themselves stay the player's.
  u16 drop_with_lr = 0;

  bool Empty() const
  {
    return buttons == 0 && !main_stick && !c_stick && press == 0 && a_as == 0 && !steer &&
           !a_centres_stick && drop_with_lr == 0;
  }
  bool operator==(const Mask&) const = default;
};
// Masks everything: the port reads as a connected pad with nothing pressed.
constexpr u16 ALL_BUTTONS = 0x1F7F;
constexpr Mask ALL{ALL_BUTTONS, true, true};

using Masks = std::array<Mask, PORTS>;

// A trigger counts as held when its button is down or its analog value reaches a quarter press,
// above a worn trigger's rest value.
constexpr u8 TRIGGER_HELD = 0x40;
constexpr bool TriggerHeld(bool button, u8 analog)
{
  return button || analog >= TRIGGER_HELD;
}

// `pad` as the game sees it through `mask`. Pure. Origin/error bits and isConnected are kept, and
// an unplugged pad is returned unchanged.
GCPadStatus Apply(const GCPadStatus& pad, const Mask& mask);

// Returns the masks for the frame about to run. Called on the CPU thread at every frame boundary.
//
// It must be a pure function of emulated memory: no host state, clock, or per-machine data. It must
// not write emulated memory, since the boundary's snapshot is already taken. Session data it needs
// must be written to memory earlier, from the frame callback. Changing its output changes the
// simulation, so bump Orca::UX::kCompatVersion with it.
//
// SetSource replaces the source; an empty function clears it. Callable from any thread.
using Source = std::function<Masks(const Core::CPUThreadGuard& guard)>;
void SetSource(Source source);

// Sees the raw pads of the frame about to run, right after the masks are decided, so a rule can
// react to presses the gate hides from the game.
//
// Unlike the source it may write emulated memory, but only as a pure function of that memory and
// those pads. It runs after the snapshot, so every pass over the boundary (first run or re-run)
// must write the same thing again; the next boundary's snapshot then captures it.
//
// SetLatch replaces the latch; an empty function clears it. Callable from any thread.
using Latch = std::function<void(const Core::CPUThreadGuard& guard,
                                 const std::array<std::optional<GCPadStatus>, PORTS>& raw)>;
void SetLatch(Latch latch);

// Called last in Rollback::OnFrameBoundary, before the SI relatch: computes the masks (plus any
// ORCA_TEST_GATE masks), then runs the latch.
void OnBoundary(const Core::CPUThreadGuard& guard);
// Removes all masks, e.g. before the game is running. CPU thread.
void Clear();
// The masks for the current frame. CPU thread.
const Masks& Current();
// `pad` through the current mask for `port` (0-based). CPU thread.
GCPadStatus GatePad(int port, const GCPadStatus& pad);

// ---- Test knob: ORCA_TEST_GATE ----
// Comma-separated rules `<port 1-4>:<what>[:<when>]`. <what> joins with '+' any of A B X Y Z L R
// START UP DOWN LEFT RIGHT STICK CSTICK ALL. <when> is `always` (default) or `mem/<n>` (n 2-64;
// `mem` means `mem/2`): mask on frames where byte (port - 1) of an XXH3 of 1 MB of game memory is
// a multiple of n. That varies per frame, so masks computed from the wrong frame's memory show up
// as desyncs in sync or keyframe tests. Example: `1:A+STICK:mem/8,2:START`.
struct TestRule
{
  int port = 0;  // 0-based
  Mask mask;
  int every = 0;  // n for `mem/<n>`, 0 for `always`
};
std::optional<std::vector<TestRule>> ParseTestSpec(std::string_view spec, std::string* error);
// The test masks for a frame whose memory hashes to `memory_hash`.
Masks TestMasks(const std::vector<TestRule>& rules, u64 memory_hash);
constexpr u32 TEST_HASH_START = 0x80500000;
constexpr u32 TEST_HASH_SIZE = 0x100000;
}  // namespace Rollback::InputGate
