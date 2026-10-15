// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/MathUtil.h"

// Layout for an embedded window whose picture box (`view`) differs from the window (`rect`).
// When the YouGame page opens a side panel, it shrinks Orca's window to make room but keeps the
// picture laid out in the full box, so the game doesn't shrink or move; the panel just covers
// part of it. See ORCA.md, "Embedding". Pure functions, so tests can drive them.
namespace VideoCommon
{
// The layout box relative to the window's top left, plus the window's size, in the window's own
// units (physical pixels on Windows, backing pixels on macOS).
struct LayoutHint
{
  int win_w = 1;
  int win_h = 1;
  int x = 0;
  int y = 0;
  int w = 1;
  int h = 1;

  constexpr bool operator==(const LayoutHint&) const = default;
};

// A box in backbuffer pixels. It may extend past the backbuffer on any side.
struct LayoutBox
{
  float x = 0;
  float y = 0;
  float w = 0;
  float h = 0;

  constexpr bool operator==(const LayoutBox&) const = default;
};

// How much the horizontal and vertical backbuffer/window scales may differ for a hint to still
// match this backbuffer. macOS rounds window sizes between points and backing pixels.
constexpr float LAYOUT_SHAPE_TOLERANCE = 0.02f;

// `hint`'s box scaled into a bb_w x bb_h backbuffer. nullopt when the hint is for a window of a
// different shape (the backbuffer hasn't caught up with a resize yet) or any size is empty.
std::optional<LayoutBox> LayoutBoxIn(const LayoutHint& hint, int bb_w, int bb_h);

// The box a present uses and the hint it came from (nullopt: the whole window).
struct LayoutChoice
{
  LayoutBox box;
  std::optional<LayoutHint> hint;
};

// Picks the layout box: `latest` if it matches this backbuffer, else `applied` (the last one used,
// kept for the frame or two before the backbuffer follows a resize), else the whole backbuffer.
// With no `latest`, the picture follows the window as in Dolphin.
LayoutChoice ChooseLayout(const std::optional<LayoutHint>& latest,
                          const std::optional<LayoutHint>& applied, int bb_w, int bb_h);

// Centres a draw_w x draw_h picture in `box` the way Dolphin centres it in the window. The result
// may run past the backbuffer; Present() crops it.
MathUtil::Rectangle<int> PlaceInBox(const LayoutBox& box, int draw_w, int draw_h);

// Whether any part of the picture lies in the backbuffer. With a `view` it may not (the window
// shows only a black bar), and cropping would give an inverted rectangle, so Present() skips it.
bool PictureInBackbuffer(const MathUtil::Rectangle<int>& picture, int bb_w, int bb_h);
}  // namespace VideoCommon
