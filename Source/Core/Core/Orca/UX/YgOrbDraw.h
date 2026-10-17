// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/YgOrb.h"

// Draws the YouGame button and its lines (YgOrb.h) with ImGui. Only Overlay.cpp includes this,
// since the platform layer has no ImGui. Video thread only, inside the overlay's ImGui frame.
namespace Orca::UX::YgOrb
{
// The button's bounds including its dot and badge; empty (min == max) when off.
Kit::Box ButtonBox(const Model::View& view);
// Draws the button with its blink ring (or the green listening pulse), held-seat dot and unread
// badge.
void DrawButton(ImDrawList* dl, const Model::View& view);
// One row of the pill beside the button: a chat line (name over up to two lines of text), an Orca
// toast (text beside the logo), a notice ("<name> <text>" on one line, with an optional hint), or
// hold-to-talk's row (as a notice; live words keep their end).
struct PillRow
{
  enum class Type
  {
    Chat,
    Toast,
    Notice,
    Talk,
  };
  Type type = Type::Chat;
  std::string_view name;
  std::string_view text;
  std::string_view key;
  std::string_view verb;
  Kind kind = Kind::Join;  // notices only
  float alpha = 1;
  double at = 0;  // arrival time on the overlay's clock; rows are sorted by it
  Talk::Phase talk = Talk::Phase::Off;  // talk rows only
};
inline constexpr std::size_t PILL_ROWS = 3;

// Draws the pill that extends right from under the button, showing the newest PILL_ROWS of `rows`
// and Current()'s notices, oldest first, then hold-to-talk's row, which none of them push out.
// Does not allocate per frame. The pill ends before `right_limit`. Returns its box, or an empty box
// when nothing is drawn.
Kit::Box DrawPill(ImDrawList* dl, const Model::View& view, std::span<const PillRow> rows,
                  double now_ms, float right_limit);

// Length of the longest prefix of `text` that fits in `width`, cut at a UTF-8 character boundary.
// Prefers to cut at a space in the second half, so words split only when one alone is too wide.
template <typename Measure>
std::size_t FitPrefix(std::string_view text, float width, Measure&& measure)
{
  if (measure(text) <= width)
    return text.size();
  const auto continuation = [&](std::size_t i) {
    return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80;
  };
  // Invariant: a prefix of `lo` bytes fits, `hi` does not; both are character boundaries.
  std::size_t lo = 0, hi = text.size();
  while (true)
  {
    std::size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && continuation(mid))
      --mid;
    if (mid == lo)
    {
      mid = lo + 1;
      while (mid < hi && continuation(mid))
        ++mid;
    }
    if (mid >= hi)
      break;
    if (measure(text.substr(0, mid)) <= width)
      lo = mid;
    else
      hi = mid;
  }
  // The word ends exactly at the limit, so keep it whole.
  if (lo < text.size() && text[lo] == ' ')
    return lo;
  const std::size_t space = text.substr(0, lo).rfind(' ');
  if (space != std::string_view::npos && space > lo / 2)
    return space;
  return lo;
}

// Start of the longest suffix of `text` that fits in `width`, at a UTF-8 character boundary: the
// newest of live words. Prefers to start after a space in the suffix's first half, so words split
// only when one alone is too wide.
template <typename Measure>
std::size_t FitSuffix(std::string_view text, float width, Measure&& measure)
{
  if (measure(text) <= width)
    return 0;
  const auto continuation = [&](std::size_t i) {
    return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80;
  };
  // Invariant: the suffix from `hi` fits (the empty one does), the one from `lo` does not; both are
  // character boundaries.
  std::size_t lo = 0, hi = text.size();
  while (true)
  {
    std::size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && continuation(mid))
      --mid;
    if (mid == lo)
    {
      mid = lo + 1;
      while (mid < hi && continuation(mid))
        ++mid;
    }
    if (mid >= hi)
      break;
    if (measure(text.substr(mid)) <= width)
      hi = mid;
    else
      lo = mid;
  }
  // A word that starts exactly at the cut stays whole.
  if (hi > 0 && text[hi - 1] == ' ')
    return hi;
  const std::size_t space = text.find(' ', hi);
  if (space != std::string_view::npos && space + 1 < text.size() &&
      space - hi < (text.size() - hi) / 2)
    return space + 1;
  return hi;
}
}  // namespace Orca::UX::YgOrb
