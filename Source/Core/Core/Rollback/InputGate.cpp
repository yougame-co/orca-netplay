// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/InputGate.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"

namespace Rollback::InputGate
{
static_assert(ALL_BUTTONS == Orca::Net::PAD_WIRE_BUTTONS,
              "the gate masks exactly the buttons a session's pads carry");

namespace
{
std::mutex s_lock;
Source s_source;
Latch s_latch;
// CPU thread only: set at the boundary, read when the SI polls.
Masks s_current{};
bool s_any = false;  // some mask in s_current is not empty

// ORCA_TEST_GATE, parsed once.
struct TestKnob
{
  std::vector<TestRule> rules;
  bool memory = false;  // some rule is `mem/<n>`
};
const std::optional<TestKnob>& Knob()
{
  static const std::optional<TestKnob> knob = []() -> std::optional<TestKnob> {
    const std::string spec = Orca::TestGateSpec();
    if (spec.empty())
      return std::nullopt;
    std::string error;
    auto rules = ParseTestSpec(spec, &error);
    if (!rules)
    {
      ERROR_LOG_FMT(ROLLBACK, "Input gate: ORCA_TEST_GATE={}: {}; no test masks", spec, error);
      return std::nullopt;
    }
    TestKnob parsed;
    parsed.rules = std::move(*rules);
    for (const TestRule& rule : parsed.rules)
      parsed.memory = parsed.memory || rule.every != 0;
    NOTICE_LOG_FMT(ROLLBACK, "Input gate: test masks ORCA_TEST_GATE={}", spec);
    return parsed;
  }();
  return knob;
}

u64 TestMemoryHash(const Core::CPUThreadGuard& guard)
{
  auto& memory = guard.GetSystem().GetMemory();
  const u32 offset = TEST_HASH_START & 0x01FFFFFF;
  if (!memory.GetRAM() || memory.GetRamSize() < offset + TEST_HASH_SIZE)
    return 0;
  return XXH3_64bits(memory.GetRAM() + offset, TEST_HASH_SIZE);
}

// Splits at every `delim`, keeping empty fields so a stray separator is an error.
std::vector<std::string> Split(std::string_view text, char delim)
{
  std::vector<std::string> fields;
  while (true)
  {
    const std::size_t at = text.find(delim);
    fields.emplace_back(text.substr(0, at));
    if (at == std::string_view::npos)
      return fields;
    text.remove_prefix(at + 1);
  }
}

// Every 600 first-run boundaries, logs how often each port was masked and the masks changed.
void LogTest(const Masks& masks)
{
  static Masks s_last{};
  static int s_boundaries = 0, s_changes = 0;
  static std::array<int, PORTS> s_masked{};
  if (IsResimulating())
    return;
  ++s_boundaries;
  if (masks != s_last)
    ++s_changes;
  s_last = masks;
  for (int port = 0; port < PORTS; ++port)
    s_masked[port] += masks[port].Empty() ? 0 : 1;
  if (s_boundaries < 600)
    return;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Input gate (test): last 600 first-run boundaries: masked ports 1-4 {}/{}/{}/{}, "
                 "{} changes",
                 s_masked[0], s_masked[1], s_masked[2], s_masked[3], s_changes);
  s_boundaries = s_changes = 0;
  s_masked = {};
}
}  // namespace

GCPadStatus Apply(const GCPadStatus& pad, const Mask& mask)
{
  if (!pad.isConnected || mask.Empty())
    return pad;
  GCPadStatus out = pad;
  u16 buttons = mask.buttons & ALL_BUTTONS;
  if (TriggerHeld((pad.button & PAD_TRIGGER_L) != 0, pad.triggerLeft) &&
      TriggerHeld((pad.button & PAD_TRIGGER_R) != 0, pad.triggerRight))
  {
    // Only digital bits: L and R keep their analog values.
    buttons |= mask.drop_with_lr & ALL_BUTTONS & ~(PAD_TRIGGER_L | PAD_TRIGGER_R);
  }
  out.button = static_cast<u16>(pad.button & ~buttons);
  if (buttons & PAD_TRIGGER_L)
    out.triggerLeft = 0;
  if (buttons & PAD_TRIGGER_R)
    out.triggerRight = 0;
  if (buttons & PAD_BUTTON_A)
    out.analogA = 0;
  if (buttons & PAD_BUTTON_B)
    out.analogB = 0;
  if (mask.main_stick)
  {
    out.stickX = GCPadStatus::MAIN_STICK_CENTER_X;
    out.stickY = GCPadStatus::MAIN_STICK_CENTER_Y;
  }
  if (mask.c_stick)
  {
    out.substickX = GCPadStatus::C_STICK_CENTER_X;
    out.substickY = GCPadStatus::C_STICK_CENTER_Y;
  }
  if (mask.steer)
  {
    out.stickX = mask.steer_x;
    out.stickY = mask.steer_y;
  }
  u16 press = mask.press & ALL_BUTTONS;
  if (pad.button & PAD_BUTTON_A)
    press |= mask.a_as & ALL_BUTTONS;
  out.button = static_cast<u16>(out.button | press);
  if (press & PAD_TRIGGER_L)
    out.triggerLeft = 0xFF;
  if (press & PAD_TRIGGER_R)
    out.triggerRight = 0xFF;
  if (press & PAD_BUTTON_A)
    out.analogA = 0xFF;
  if (press & PAD_BUTTON_B)
    out.analogB = 0xFF;
  if (mask.a_centres_stick && ((pad.button | press) & PAD_BUTTON_A))
  {
    out.stickX = GCPadStatus::MAIN_STICK_CENTER_X;
    out.stickY = GCPadStatus::MAIN_STICK_CENTER_Y;
  }
  return out;
}

void SetSource(Source source)
{
  std::lock_guard lk(s_lock);
  s_source = std::move(source);
}

void SetLatch(Latch latch)
{
  std::lock_guard lk(s_lock);
  s_latch = std::move(latch);
}

void OnBoundary(const Core::CPUThreadGuard& guard)
{
  Source source;
  Latch latch;
  {
    std::lock_guard lk(s_lock);
    source = s_source;
    latch = s_latch;
  }
  Masks masks{};
  if (source)
    masks = source(guard);
  if (const auto& knob = Knob())
  {
    const Masks test = TestMasks(knob->rules, knob->memory ? TestMemoryHash(guard) : 0);
    for (int port = 0; port < PORTS; ++port)
    {
      masks[port].buttons |= test[port].buttons;
      masks[port].press |= test[port].press;
      masks[port].a_as |= test[port].a_as;
      masks[port].a_centres_stick = masks[port].a_centres_stick || test[port].a_centres_stick;
      masks[port].main_stick = masks[port].main_stick || test[port].main_stick;
      masks[port].c_stick = masks[port].c_stick || test[port].c_stick;
    }
    LogTest(masks);
  }
  s_current = masks;
  s_any = false;
  for (const Mask& mask : s_current)
    s_any = s_any || !mask.Empty();
  if (latch)
  {
    std::array<std::optional<GCPadStatus>, PORTS> raw;
    for (int port = 0; port < PORTS; ++port)
      raw[port] = RawInputOverride(port);
    latch(guard, raw);
  }
}

void Clear()
{
  s_current = {};
  s_any = false;
}

const Masks& Current()
{
  return s_current;
}

GCPadStatus GatePad(int port, const GCPadStatus& pad)
{
  if (!s_any || port < 0 || port >= PORTS)
    return pad;
  return Apply(pad, s_current[port]);
}

std::optional<std::vector<TestRule>> ParseTestSpec(std::string_view spec, std::string* error)
{
  static constexpr std::pair<std::string_view, u16> BUTTONS[] = {
      {"A", PAD_BUTTON_A},       {"B", PAD_BUTTON_B},         {"X", PAD_BUTTON_X},
      {"Y", PAD_BUTTON_Y},       {"Z", PAD_TRIGGER_Z},        {"L", PAD_TRIGGER_L},
      {"R", PAD_TRIGGER_R},      {"START", PAD_BUTTON_START}, {"UP", PAD_BUTTON_UP},
      {"DOWN", PAD_BUTTON_DOWN}, {"LEFT", PAD_BUTTON_LEFT},   {"RIGHT", PAD_BUTTON_RIGHT},
  };
  std::vector<TestRule> rules;
  for (const std::string& rule_text : Split(spec, ','))
  {
    const std::vector<std::string> parts = Split(rule_text, ':');
    if (parts.size() < 2 || parts.size() > 3)
    {
      *error = fmt::format("'{}' is not <port>:<what>[:<when>]", rule_text);
      return std::nullopt;
    }
    TestRule rule;
    if (parts[0].size() != 1 || parts[0][0] < '1' || parts[0][0] > '4')
    {
      *error = fmt::format("'{}': the port is 1-4", rule_text);
      return std::nullopt;
    }
    rule.port = parts[0][0] - '1';
    for (const std::string& name : Split(parts[1], '+'))
    {
      if (name == "ALL")
      {
        rule.mask = ALL;
        continue;
      }
      if (name == "STICK")
      {
        rule.mask.main_stick = true;
        continue;
      }
      if (name == "CSTICK")
      {
        rule.mask.c_stick = true;
        continue;
      }
      bool found = false;
      for (const auto& [button_name, bit] : BUTTONS)
      {
        if (name == button_name)
        {
          rule.mask.buttons |= bit;
          found = true;
        }
      }
      if (!found)
      {
        *error = fmt::format("'{}': unknown button '{}'", rule_text, name);
        return std::nullopt;
      }
    }
    if (parts.size() == 3)
    {
      const std::string& when = parts[2];
      if (when == "mem")
      {
        rule.every = 2;
      }
      else if (when.starts_with("mem/") && when.size() >= 5 && when.size() <= 6 &&
               std::all_of(when.begin() + 4, when.end(),
                           [](char c) { return c >= '0' && c <= '9'; }))
      {
        rule.every = std::stoi(when.substr(4));
        if (rule.every < 2 || rule.every > 64)
        {
          *error = fmt::format("'{}': mem/<n> takes n from 2 to 64", rule_text);
          return std::nullopt;
        }
      }
      else if (when != "always")
      {
        *error = fmt::format("'{}': <when> is always, mem or mem/<n>", rule_text);
        return std::nullopt;
      }
    }
    if (rule.mask.Empty())
    {
      *error = fmt::format("'{}' masks nothing", rule_text);
      return std::nullopt;
    }
    rules.push_back(rule);
  }
  if (rules.empty())
  {
    *error = "no rules";
    return std::nullopt;
  }
  return rules;
}

Masks TestMasks(const std::vector<TestRule>& rules, u64 memory_hash)
{
  Masks masks{};
  for (const TestRule& rule : rules)
  {
    if (rule.every != 0 && ((memory_hash >> (8 * rule.port)) & 0xFF) % rule.every != 0)
      continue;
    Mask& mask = masks[rule.port];
    mask.buttons |= rule.mask.buttons;
    mask.main_stick = mask.main_stick || rule.mask.main_stick;
    mask.c_stick = mask.c_stick || rule.mask.c_stick;
  }
  return masks;
}
}  // namespace Rollback::InputGate
