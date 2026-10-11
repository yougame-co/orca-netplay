// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace picojson
{
class value;
}

// Parses the YouGame desktop app's controller snapshots and maps them to the local player's
// GameCube pad. Pure functions with no I/O or clocks, so unit tests cover all of it. This only
// decides which bytes the local player sends, so it is not part of the compatibility key.
namespace Orca::UX
{
// One physical GameCube adapter port, raw from the adapter's 0x21 report.
struct AdapterPort
{
  int port = 0;  // 0-3, the adapter's physical port
  int seat = 0;  // 0-3, assigned in the app's Controls panel (seat 0 is the local player)
  bool connected = false;
  bool wireless = false;
  u16 buttons = 0;  // byte 1 | (byte 2 & 0xF) << 8: A B X Y Left Right Down Up, Start Z R L
  std::array<u8, 4> axes{128, 128, 128, 128};  // stick X, Y, C-stick X, Y
  std::array<u8, 2> triggers{0, 0};            // L, R
  std::array<u8, 6> origin{128, 128, 128, 128, 0, 0};  // calibrated neutral position
};

// An SDL gamepad with the W3C "standard" layout (17 buttons, 4 axes).
struct StandardPad
{
  int index = 0;
  std::array<float, 4> axes{};      // -1..1: left X, left Y (down +), right X, right Y (down +)
  std::array<float, 17> buttons{};  // 0..1
  std::array<bool, 17> pressed{};
};

struct Snapshot
{
  std::string session;  // helper process run; sequence numbers are only ordered within one
  u64 sequence = 0;
  double sent_at = 0;      // helper clock, epoch ms
  double received_at = 0;  // helper clock: last valid adapter report (0 = none yet)
  bool owned = false;      // helper holds an adapter (ports then has all four)
  bool suspended = false;
  std::vector<AdapterPort> ports;
  std::vector<StandardPad> pads;  // standard-mapped pads only, lowest index first
};

// Parses one SSE `data:` payload. Anything malformed or out of range rejects the whole snapshot.
std::optional<Snapshot> ParseSnapshotEvent(std::string_view json);
// The same event already parsed ({type: "controllers", snapshot}), from the stdio bridge
// (Orca/Bridge.h).
std::optional<Snapshot> ParseSnapshotEvent(const picojson::value& event);
// Whether picojson may parse `text`: at most 64 KB, nested at most 8 deep, and no number long
// enough to overflow a double (picojson has no nesting limit and aborts on such a number).
bool SafeJson(std::string_view text);

// Input older than this is treated as neutral. The helper republishes every 100 ms.
constexpr double STALE_MS = 250;

// Connected, nothing pressed, sticks centred. Used for stale or suspended input.
GCPadStatus NeutralPad();

// Decodes a port exactly like Dolphin's GCAdapter::DecodeChannel, then subtracts the calibration
// origin. With the default origin the result matches Dolphin's adapter path byte for byte.
GCPadStatus MapAdapterPort(const AdapterPort& port);

// Maps a standard gamepad: A = south, B = west, X = east, Y = north, Z = either bumper,
// L/R = triggers (digital press from half their travel).
GCPadStatus MapStandardPad(const StandardPad& pad);

enum class PadSource
{
  Adapter,
  Gamepad,
};

struct LocalChoice
{
  PadSource source;
  int index;  // adapter port or gamepad index
  GCPadStatus pad;
  bool neutral;  // stale or suspended
};

// Picks the local player's pad: the connected adapter port with the lowest seat, else the first
// standard gamepad. Returns nullopt when there is neither, so the caller falls back to Dolphin's
// own input mapping instead of a stuck neutral pad. `age_ms` is measured on this machine's steady
// clock, so staleness never compares two processes' wall clocks.
std::optional<LocalChoice> ChooseLocalPad(const Snapshot& snapshot, double age_ms);
}  // namespace Orca::UX
