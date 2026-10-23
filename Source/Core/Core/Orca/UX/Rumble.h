// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// The game's rumble, for the YouGame page to play on the controller the player holds (cap
// "rumble"). Orca drives no motor itself: the app's controller helper owns the devices, and the
// page sends it the commands.
//
// The motor is the local port's SI channel output as the game last wrote it (PADControlMotor:
// 0x0040_03_0m, m = 1 on), read at frame boundaries that are not re-runs. The register is in every
// snapshot, so a rollback's correction shows at the next live boundary, a re-run never buzzes the
// pad twice, and nothing is written to emulated memory: no input, determinism or compatibility key
// changes.
//
// Line, on change only: "orca rumble <port> <0|1> <adapter|pad|none> <index>"
//   port     the local player's GameCube port, 1-4 (information: the device is what rumbles)
//   0|1      the motor off or on
//   adapter  the adapter's physical port 0-3, as the app's snapshot numbers it
//   pad      the standard pad's index in the app's snapshot, as the app arranged it for Orca
//   none     Orca reads no app device (Dolphin's own input, a test script): never on
// The motor goes on at once and off after OFF_FRAMES live boundaries off, so the game's on/off
// patterns read as one buzz. An off goes out as soon as it is due. On lines (a new buzz, another
// port or device, a renewal) are at least MIN_GAP_FRAMES live boundaries apart, so at most 10 on
// and 10 off lines a second. While the game holds the motor on, the on line is said again every
// RESEND_FRAMES: the page holds the motor between lines for a short time limit of its own, so a
// long rumble lasts and a frozen Orca stops it soon. A halt (a stall, the exit) says off at once.
namespace Orca::UX::Rumble
{
enum class Device : u8
{
  None,
  Adapter,
  Pad,
};

struct Target
{
  Device device = Device::None;
  int index = 0;
  bool operator==(const Target&) const = default;
};

struct State
{
  bool on = false;
  int port = 0;  // 0-based
  Target target;
  bool operator==(const State&) const = default;
};

constexpr int OFF_FRAMES = 3;
constexpr int MIN_GAP_FRAMES = 6;
constexpr int RESEND_FRAMES = 30;
// A stall this long stops the motor (Halt) until the game runs again.
constexpr int STALL_HALT_MS = 100;

// Whether an SI channel's output holds a GameCube controller's motor-on command.
bool MotorOn(u32 channel_out);

// What to print, fed one live boundary at a time. No clocks or I/O, so tests drive it directly.
class Tracker
{
public:
  // A live (first-run) boundary. `motor`: the game's motor for the local port; `live`: input
  // reaches the game (Orca has the focus); `port` (0-based) and `target`: the local player's port
  // and app device. Returns the state to print, if any.
  std::optional<State> Boundary(bool motor, bool live, int port, Target target);
  // The game stopped advancing (a stall, leaving, the exit): off now, if it was on.
  std::optional<State> Halt();
  const State& Sent() const { return m_sent; }

private:
  State m_sent;
  // Live boundaries in a row with the motor off, and since the last on line (up to RESEND_FRAMES).
  int m_off_run = 0;
  int m_since_on = RESEND_FRAMES;
};

std::string FormatLine(const State& state);

// CPU thread, from the UX frame callback at a boundary that is not a re-run: reads `local_port`'s
// motor and prints a line when the app agreed "rumble".
void Frame(const Core::CPUThreadGuard& guard, int local_port);
// The game has been stalled on a friend for `stalled_ms` (the stall callback): off once it lasts.
void Stall(int stalled_ms);
// Leaving, the exit: off.
void Halt();
}  // namespace Orca::UX::Rumble
