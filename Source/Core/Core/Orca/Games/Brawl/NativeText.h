// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// Writes Orca's text into Brawl's own menu text boxes, so it is drawn in the game's font, size and
// colour. Brawl formats a message into a window's buffer once and redraws that buffer every frame,
// so a rewritten buffer stays on screen until the game formats that window again.
//
// Buffer layout (Brawl rev 2): vtable 0x804540E0, +0x28 capacity, +0x2C length, +0x30 data, and an
// 0x01 end byte after the length. The data is the window's set-up (0x17 ... ending in its colours,
// 0x0C rgba 0x04 rgba 0x03 0x00 0x00), then the message: width code 0x11 + 4 bytes, colour code
// 0x12 0x02 0x0C rgba, the UTF-8 words, then trailing codes. Orca replaces only the words and keeps
// every code. Without a width code a longer line is not drawn at all. The font has no middle dot
// or dash, so the words are ASCII.
//
// On the Versus character select (muSelCharTask, scene +0x400) Orca writes each player's name plate
// (task +0x44 + 4i -> area, area +0x174 -> buffer) and the rules bar (task +0x538 -> buffer). The
// rules bar describes the queue room's state in the third person, never "you", so both players see
// the same text.
//
// Determinism: the text is a pure function of emulated memory and the session's synced ports, and
// only differing bytes are written, before the session saves the frame. See ORCA.md, "Native text".
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::NativeText
{
constexpr u32 MESSAGE_BUFFER_VTABLE = 0x804540E0;
constexpr u32 BUFFER_CAPACITY = 0x28;
constexpr u32 BUFFER_LENGTH = 0x2C;
constexpr u32 BUFFER_DATA = 0x30;
constexpr u8 END = 0x01;

// One message buffer's layout: where the words are, and what surrounds them.
struct Box
{
  u32 object = 0;  // 0: not a message buffer this knows
  u32 buffer = 0;
  u32 capacity = 0;
  u32 length = 0;
  u32 words = 0;      // offset of the first word byte, after the set-up and leading codes
  u32 words_end = 0;  // offset of the first byte after the words
};
// Reads and validates the buffer `object` points at; nullopt if it is not a known message buffer.
std::optional<Box> ReadBox(const GuestMemory& memory, u32 object);
// The words the box shows now.
std::string Words(const GuestMemory& memory, const Box& box);
// Replaces the box's words with `text` (printable ASCII), writing only bytes that differ.
// Returns 1 if anything was written, 0 if unchanged or it would not fit.
int SetWords(GuestMemory& memory, const Box& box, std::string_view text);

// Converts text to what the font can draw: printable ASCII, with "·"/"–" as "-" and "…" as "...".
std::string Ascii(std::string_view text, size_t max = 96);
// A YouGame username as a name plate: ASCII capitals, at most 10 characters.
std::string PlateName(std::string_view username);
// "m:ss" of `frames` (60 a second), rounded up.
std::string Clock(int frames);

// What the character select's boxes should say this frame. Empty means leave the game's words.
struct CssWords
{
  bool on_css = false;
  std::array<std::string, 4> plates;
  std::string rules;
};
// `band_clock` is Relabel::ClockInBand for this frame if the caller already has it.
CssWords Wanted(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports,
                std::optional<bool> band_clock = std::nullopt);

// The character select's boxes, nullopt where none was found.
struct CssBoxes
{
  std::array<std::optional<Box>, 4> plates;
  std::optional<Box> rules;
};
CssBoxes FindCssBoxes(const GuestMemory& memory);

// Writes one frame's text and returns how many boxes changed. `shown` gets which boxes show Orca's
// words (bit p for plate p, SHOWN_RULES for the rules bar). Pass `rules_bar` false for Project+,
// whose rules bar has no width code.
int Apply(GuestMemory& memory, const std::vector<Events::PortInfo>& ports, u8* shown = nullptr,
          bool rules_bar = true, std::optional<bool> band_clock = std::nullopt);
constexpr u8 SHOWN_RULES = 0x10;

// Called by the frame hook on every frame, including rollback re-simulation. Outside re-simulation
// it also records what Shown() returns.
void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Events::PortInfo>& ports, std::optional<bool> band_clock = std::nullopt);
// Apply's `shown` from the last non-resimulated frame, so the overlay can hide lines the game shows.
u8 Shown();
}  // namespace Orca::UX::NativeText
