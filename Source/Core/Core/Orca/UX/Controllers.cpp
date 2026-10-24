// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Controllers.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <picojson.h>

#include "Core/Orca/SafeJson.h"
#include "InputCommon/GCAdapter.h"

namespace Orca::UX
{
namespace
{
// A real snapshot is a few KB.
constexpr size_t MAX_EVENT_BYTES = 64 * 1024;

// picojson has no nesting limit and aborts on a number that overflows a double. Snapshots nest
// four deep and use short numbers, so reject anything deeper or longer before parsing.
bool SafeToParse(std::string_view text)
{
  if (text.size() > MAX_EVENT_BYTES)
    return false;
  int depth = 0;
  size_t number_length = 0, exponent_digits = 0;
  bool in_exponent = false;
  for (size_t i = 0; i < text.size(); ++i)
  {
    const char c = text[i];
    if (c == '"')
    {
      for (++i; i < text.size() && text[i] != '"'; ++i)
      {
        if (text[i] == '\\')
          ++i;
      }
      number_length = 0;
      in_exponent = false;
      continue;
    }
    const bool numeric = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                         ((c == 'e' || c == 'E') && number_length > 0);
    if (!numeric)
    {
      number_length = 0;
      in_exponent = false;
    }
    else
    {
      if (++number_length > 40)
        return false;
      if (c == 'e' || c == 'E')
      {
        in_exponent = true;
        exponent_digits = 0;
      }
      else if (in_exponent && c >= '0' && c <= '9' && ++exponent_digits > 2)
      {
        return false;
      }
    }
    if (c == '[' || c == '{')
    {
      if (++depth > 8)
        return false;
    }
    else if (c == ']' || c == '}')
    {
      --depth;
    }
  }
  return true;
}

const picojson::value* Field(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}

std::optional<double> Number(const picojson::object& o, const char* key, double lo, double hi)
{
  const picojson::value* v = Field(o, key);
  if (!v || !v->is<double>())
    return std::nullopt;
  const double d = v->get<double>();
  if (!std::isfinite(d) || d < lo || d > hi)
    return std::nullopt;
  return d;
}

std::optional<int> Integer(const picojson::object& o, const char* key, int lo, int hi)
{
  const auto d = Number(o, key, lo, hi);
  if (!d || *d != std::floor(*d))
    return std::nullopt;
  return static_cast<int>(*d);
}

std::optional<bool> Bool(const picojson::object& o, const char* key)
{
  const picojson::value* v = Field(o, key);
  if (!v || !v->is<bool>())
    return std::nullopt;
  return v->get<bool>();
}

template <size_t N>
bool Bytes(const picojson::object& o, const char* key, std::array<u8, N>* out)
{
  const picojson::value* v = Field(o, key);
  if (!v || !v->is<picojson::array>() || v->get<picojson::array>().size() != N)
    return false;
  const auto& a = v->get<picojson::array>();
  for (size_t i = 0; i < N; ++i)
  {
    if (!a[i].is<double>())
      return false;
    const double d = a[i].get<double>();
    if (!(d >= 0 && d <= 255) || d != std::floor(d))
      return false;
    (*out)[i] = static_cast<u8>(d);
  }
  return true;
}

std::optional<AdapterPort> ParsePort(const picojson::value& v, int expected_port)
{
  if (!v.is<picojson::object>())
    return std::nullopt;
  const auto& o = v.get<picojson::object>();
  AdapterPort p;
  const auto port = Integer(o, "port", 0, 3);
  const auto seat = Integer(o, "seat", 0, 3);
  const auto connected = Bool(o, "connected");
  const auto buttons = Integer(o, "buttons", 0, 4095);
  const picojson::value* type = Field(o, "type");
  if (!port || *port != expected_port || !seat || !connected || !buttons || !type)
    return std::nullopt;
  if (type->is<picojson::null>())
  {
    if (*connected)
      return std::nullopt;
  }
  else if (type->is<std::string>() && *connected &&
           (type->get<std::string>() == "wired" || type->get<std::string>() == "wireless"))
  {
    p.wireless = type->get<std::string>() == "wireless";
  }
  else
  {
    return std::nullopt;
  }
  if (!Bytes(o, "axes", &p.axes) || !Bytes(o, "triggers", &p.triggers) ||
      !Bytes(o, "origin", &p.origin))
  {
    return std::nullopt;
  }
  p.port = *port;
  p.seat = *seat;
  p.connected = *connected;
  p.buttons = static_cast<u16>(*buttons);
  return p;
}

// nullopt if malformed. A pad without the standard mapping gets index -1 and is skipped.
std::optional<StandardPad> ParsePad(const picojson::value& v)
{
  if (!v.is<picojson::object>())
    return std::nullopt;
  const auto& o = v.get<picojson::object>();
  const auto index = Integer(o, "index", 0, 15);
  const picojson::value* mapping = Field(o, "mapping");
  const picojson::value* axes = Field(o, "axes");
  const picojson::value* buttons = Field(o, "buttons");
  if (!index || !mapping || !mapping->is<std::string>() || !axes ||
      !axes->is<picojson::array>() || !buttons || !buttons->is<picojson::array>() ||
      axes->get<picojson::array>().size() > 64 || buttons->get<picojson::array>().size() > 64)
  {
    return std::nullopt;
  }
  StandardPad pad;
  pad.seat = Integer(o, "seat", 0, 3);
  if (mapping->get<std::string>() != "standard")
  {
    pad.index = -1;
    return pad;
  }
  const auto& a = axes->get<picojson::array>();
  const auto& b = buttons->get<picojson::array>();
  if (a.size() != 4 || b.size() != 17)
    return std::nullopt;
  for (size_t i = 0; i < 4; ++i)
  {
    if (!a[i].is<double>() || !(std::abs(a[i].get<double>()) <= 1))
      return std::nullopt;
    pad.axes[i] = static_cast<float>(a[i].get<double>());
  }
  for (size_t i = 0; i < 17; ++i)
  {
    if (!b[i].is<picojson::object>())
      return std::nullopt;
    const auto& button = b[i].get<picojson::object>();
    const auto value = Number(button, "value", 0, 1);
    const auto pressed = Bool(button, "pressed");
    if (!value || !pressed)
      return std::nullopt;
    pad.buttons[i] = static_cast<float>(*value);
    pad.pressed[i] = *pressed;
  }
  pad.index = *index;
  return pad;
}

u8 ClampByte(int v)
{
  return static_cast<u8>(std::clamp(v, 0, 255));
}

// The byte of an 8-bit axis (0..255, resting at 128), if `v` came from one. Box controllers (B0XX,
// HayBox) send exact GameCube stick bytes: over DInput as an 8-bit HID axis, which the OS scales to
// SDL's int16 as 257 * byte - 32768 (IOKit and DirectInput both map 0..255 linearly), and over
// XInput as 257 * (byte - 128) + 128 (HayBox): both 128 above a multiple of 257. The helper sends
// int16 / 32767 (/ 32768 below zero); a negated axis (SDL's XInput Y, an axis inverted in the
// Controls panel) lands on 129 instead. On that lattice floor((s + 32768) / 257) is the byte the
// device sent (the mirrored byte when negated), where 128 + lround(127 * v) comes out one unit
// short of every leftward and downward push. Analog sticks (16-bit, centred on 0) are off the
// lattice but for one value in 257, which then moves by at most one unit.
std::optional<int> EightBitAxis(float v)
{
  for (const double scale : {32767.0, 32768.0})
  {
    const double s = static_cast<double>(v) * scale;
    const double whole = std::round(s);
    // A float holds the helper's value to within 0.002 of an int16 step.
    if (std::abs(s - whole) > 0.004)
      continue;
    const int i = static_cast<int>(whole);
    const int m = ((i % 257) + 257) % 257;
    if (m == 128 || m == 129)
      return (i + 32768) / 257;  // i + 32768 >= 0: floor
  }
  return std::nullopt;
}

// -1..1 to a GameCube axis byte around `centre`; `invert` for Y (the pad's down is the GC's -).
// Never 0: a full push is 1..255 either way, as before.
u8 AxisByte(float v, bool invert)
{
  if (!std::isfinite(v))
    return GCPadStatus::MAIN_STICK_CENTER_X;
  const float c = std::clamp(v, -1.0f, 1.0f);
  if (const auto byte = EightBitAxis(c))
    return static_cast<u8>(std::clamp(invert ? 255 - *byte : *byte, 1, 255));
  const float s = invert ? -c : c;
  return ClampByte(GCPadStatus::MAIN_STICK_CENTER_X +
                   static_cast<int>(std::lround(s * GCPadStatus::MAIN_STICK_RADIUS)));
}

// A standard pad's C-stick inside this radius (of a full push, both axes together) reads as
// centred. Brawl takes a new C-stick attack only once the C-stick is back at neutral: resting 5
// units (4%) off centre, every C-stick smash and aerial after the first is ignored (4 units off is
// still neutral), and analog sticks often rest that far off. The adapter's controllers get their
// rest as their origin (the app's helper); a standard pad has none, hence this (a little under the
// 0.265 XInput recommends for a right stick). Outside it nothing is rescaled: flicks and a box
// controller's exact C-stick values reach the game as before. Only the local player's bytes change.
constexpr float C_STICK_DEAD_ZONE = 0.2f;

// W3C standard gamepad button indices.
enum StandardButton
{
  SOUTH = 0,
  EAST = 1,
  WEST = 2,
  NORTH = 3,
  LEFT_BUMPER = 4,
  RIGHT_BUMPER = 5,
  LEFT_TRIGGER = 6,
  RIGHT_TRIGGER = 7,
  START = 9,
  DPAD_UP = 12,
  DPAD_DOWN = 13,
  DPAD_LEFT = 14,
  DPAD_RIGHT = 15,
};

// A trigger's digital L/R (shield, air dodge) from half its travel, as in Dolphin. A GameCube
// trigger clicks only at the end, but most pads' triggers are shorter, softer and never pulled all
// the way.
constexpr float TRIGGER_CLICK = 0.5f;
}  // namespace

bool SafeJson(std::string_view text)
{
  return SafeToParse(text);
}

std::optional<Snapshot> ParseSnapshotEvent(std::string_view json)
{
  if (!SafeToParse(json))
    return std::nullopt;
  picojson::value root;
  if (!Json::ParseJson(std::string(json), &root, Json::NumberPolicy::Finite))
    return std::nullopt;
  return ParseSnapshotEvent(root);
}

std::optional<Snapshot> ParseSnapshotEvent(const picojson::value& root)
{
  if (!root.is<picojson::object>())
    return std::nullopt;
  const auto& event = root.get<picojson::object>();
  const picojson::value* type = Field(event, "type");
  const picojson::value* body = Field(event, "snapshot");
  if (!type || !type->is<std::string>() || type->get<std::string>() != "controllers" || !body ||
      !body->is<picojson::object>())
  {
    return std::nullopt;
  }
  const auto& o = body->get<picojson::object>();
  constexpr double MAX_SAFE = 9007199254740991.0;
  const auto schema = Integer(o, "schema", 1, 1);
  const auto sequence = Number(o, "sequence", 0, MAX_SAFE);
  const auto sent_at = Number(o, "sentAt", 0, MAX_SAFE);
  const auto received_at = Number(o, "receivedAt", 0, MAX_SAFE);
  const auto owned = Bool(o, "owned");
  const auto suspended = Bool(o, "suspended");
  const picojson::value* session = Field(o, "session");
  const picojson::value* ports = Field(o, "ports");
  const picojson::value* pads = Field(o, "pads");
  if (!session || !session->is<std::string>() || session->get<std::string>().size() > 128 ||
      !schema || !sequence || !sent_at || !received_at || !owned || !suspended || !ports ||
      !ports->is<picojson::array>() || !pads || !pads->is<picojson::array>())
  {
    return std::nullopt;
  }
  Snapshot s;
  s.session = session->get<std::string>();
  s.sequence = static_cast<u64>(*sequence);
  s.sent_at = *sent_at;
  s.received_at = *received_at;
  s.owned = *owned;
  s.suspended = *suspended;
  const auto& port_list = ports->get<picojson::array>();
  if (port_list.size() != (s.owned ? 4u : 0u))
    return std::nullopt;
  for (size_t i = 0; i < port_list.size(); ++i)
  {
    const auto port = ParsePort(port_list[i], static_cast<int>(i));
    if (!port)
      return std::nullopt;
    s.ports.push_back(*port);
  }
  const auto& pad_list = pads->get<picojson::array>();
  if (pad_list.size() > 16)
    return std::nullopt;
  for (const picojson::value& v : pad_list)
  {
    const auto pad = ParsePad(v);
    if (!pad)
      return std::nullopt;
    if (pad->index >= 0)
      s.pads.push_back(*pad);
  }
  std::sort(s.pads.begin(), s.pads.end(),
            [](const StandardPad& a, const StandardPad& b) { return a.index < b.index; });
  return s;
}

GCPadStatus NeutralPad()
{
  return GCPadStatus{};
}

GCPadStatus MapAdapterPort(const AdapterPort& port)
{
  // Rebuild the port's 9 wire bytes (only byte 2's low nibble is used).
  const std::array<u8, 9> wire{port.wireless ? u8(0x20) : u8(0x10),
                               static_cast<u8>(port.buttons & 0xFF),
                               static_cast<u8>((port.buttons >> 8) & 0x0F),
                               port.axes[0],
                               port.axes[1],
                               port.axes[2],
                               port.axes[3],
                               port.triggers[0],
                               port.triggers[1]};
  GCPadStatus pad = GCAdapter::DecodeChannel(wire.data());
  const auto centred = [](u8 raw, u8 origin, u8 centre) {
    return ClampByte(int(raw) - int(origin) + int(centre));
  };
  pad.stickX = centred(pad.stickX, port.origin[0], GCPadStatus::MAIN_STICK_CENTER_X);
  pad.stickY = centred(pad.stickY, port.origin[1], GCPadStatus::MAIN_STICK_CENTER_Y);
  pad.substickX = centred(pad.substickX, port.origin[2], GCPadStatus::C_STICK_CENTER_X);
  pad.substickY = centred(pad.substickY, port.origin[3], GCPadStatus::C_STICK_CENTER_Y);
  pad.triggerLeft = centred(pad.triggerLeft, port.origin[4], 0);
  pad.triggerRight = centred(pad.triggerRight, port.origin[5], 0);
  pad.isConnected = true;
  return pad;
}

GCPadStatus MapStandardPad(const StandardPad& pad)
{
  GCPadStatus status;
  const auto held = [&pad](int i) { return pad.pressed[i] || pad.buttons[i] >= 0.5f; };
  if (held(SOUTH))
    status.button |= PAD_BUTTON_A;
  if (held(WEST))
    status.button |= PAD_BUTTON_B;
  if (held(EAST))
    status.button |= PAD_BUTTON_X;
  if (held(NORTH))
    status.button |= PAD_BUTTON_Y;
  if (held(LEFT_BUMPER) || held(RIGHT_BUMPER))
    status.button |= PAD_TRIGGER_Z;
  if (held(START))
    status.button |= PAD_BUTTON_START;
  if (held(DPAD_UP))
    status.button |= PAD_BUTTON_UP;
  if (held(DPAD_DOWN))
    status.button |= PAD_BUTTON_DOWN;
  if (held(DPAD_LEFT))
    status.button |= PAD_BUTTON_LEFT;
  if (held(DPAD_RIGHT))
    status.button |= PAD_BUTTON_RIGHT;
  const auto trigger = [&pad](int i) {
    // A digital trigger reports 0 or 1; an analog one its travel.
    const float v = std::max(pad.buttons[i], pad.pressed[i] && pad.buttons[i] == 0 ? 1.0f : 0.0f);
    return std::clamp(v, 0.0f, 1.0f);
  };
  const float left = trigger(LEFT_TRIGGER), right = trigger(RIGHT_TRIGGER);
  status.triggerLeft = ClampByte(static_cast<int>(std::lround(left * 255)));
  status.triggerRight = ClampByte(static_cast<int>(std::lround(right * 255)));
  if (left >= TRIGGER_CLICK)
    status.button |= PAD_TRIGGER_L;
  if (right >= TRIGGER_CLICK)
    status.button |= PAD_TRIGGER_R;
  status.stickX = AxisByte(pad.axes[0], false);
  status.stickY = AxisByte(pad.axes[1], true);
  const float cx = pad.axes[2], cy = pad.axes[3];
  if (cx * cx + cy * cy >= C_STICK_DEAD_ZONE * C_STICK_DEAD_ZONE)
  {
    status.substickX = AxisByte(cx, false);
    status.substickY = AxisByte(cy, true);
  }
  status.isConnected = true;
  return status;
}

std::optional<LocalChoice> ChooseLocalPad(const Snapshot& snapshot, double age_ms)
{
  const bool stale = !(age_ms <= STALE_MS) || snapshot.suspended;
  if (snapshot.owned)
  {
    const AdapterPort* best = nullptr;
    for (const AdapterPort& p : snapshot.ports)
    {
      if (p.connected && (!best || p.seat < best->seat))
        best = &p;
    }
    if (best)
    {
      // Age of the last adapter report: helper clock until the snapshot, our clock after.
      const double report_age = snapshot.sent_at - snapshot.received_at + age_ms;
      const bool raw_stale = snapshot.received_at <= 0 || !(report_age <= STALE_MS);
      if (stale || raw_stale)
        return LocalChoice{PadSource::Adapter, best->port, NeutralPad(), true};
      return LocalChoice{PadSource::Adapter, best->port, MapAdapterPort(*best), false};
    }
  }
  if (!snapshot.pads.empty())
  {
    const StandardPad& pad = snapshot.pads.front();
    if (stale)
      return LocalChoice{PadSource::Gamepad, pad.index, NeutralPad(), true};
    return LocalChoice{PadSource::Gamepad, pad.index, MapStandardPad(pad), false};
  }
  return std::nullopt;
}
std::array<std::optional<LocalChoice>, MAX_LOCAL> ChooseLocalPads(const Snapshot& snapshot,
                                                               double age_ms, int n)
{
  std::array<std::optional<LocalChoice>, MAX_LOCAL> choices{};
  if (n == 1)
  {
    choices[0] = ChooseLocalPad(snapshot, age_ms);
    return choices;
  }
  const bool stale = !(age_ms <= STALE_MS) || snapshot.suspended;
  for (int ordinal = 0; ordinal < std::clamp(n, 0, MAX_LOCAL); ++ordinal)
  {
    const AdapterPort* adapter = nullptr;
    bool ambiguous = false;
    if (snapshot.owned)
      for (const auto& port : snapshot.ports)
        if (port.connected && port.seat == ordinal)
        {
          ambiguous |= adapter != nullptr;
          adapter = &port;
        }
    if (ambiguous)
      continue;
    if (adapter)
    {
      const double report_age = snapshot.sent_at - snapshot.received_at + age_ms;
      const bool neutral = stale || snapshot.received_at <= 0 || !(report_age <= STALE_MS);
      choices[ordinal] = LocalChoice{PadSource::Adapter, adapter->port,
          neutral ? NeutralPad() : MapAdapterPort(*adapter), neutral};
    }
    else
    {
      const StandardPad* assigned = nullptr;
      for (const auto& pad : snapshot.pads)
        if (pad.seat == ordinal)
        {
          ambiguous |= assigned != nullptr;
          assigned = &pad;
        }
      if (assigned && !ambiguous)
        choices[ordinal] = LocalChoice{PadSource::Gamepad, assigned->index,
            stale ? NeutralPad() : MapStandardPad(*assigned), stale};
    }
    for (int previous = 0; previous < ordinal; ++previous)
      if (choices[ordinal] && choices[previous] &&
          choices[ordinal]->source == choices[previous]->source &&
          choices[ordinal]->index == choices[previous]->index)
        choices[ordinal].reset();
  }
  return choices;
}
}  // namespace Orca::UX
