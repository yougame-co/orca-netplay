// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PresentLayout.h"

#include <algorithm>
#include <cmath>

namespace VideoCommon
{
std::optional<LayoutBox> LayoutBoxIn(const LayoutHint& hint, int bb_w, int bb_h)
{
  if (hint.win_w <= 0 || hint.win_h <= 0 || hint.w <= 0 || hint.h <= 0 || bb_w <= 0 || bb_h <= 0)
    return std::nullopt;
  const float kx = static_cast<float>(bb_w) / static_cast<float>(hint.win_w);
  const float ky = static_cast<float>(bb_h) / static_cast<float>(hint.win_h);
  if (std::abs(kx - ky) > LAYOUT_SHAPE_TOLERANCE * std::max(kx, ky))
    return std::nullopt;
  return LayoutBox{static_cast<float>(hint.x) * kx, static_cast<float>(hint.y) * ky,
                   static_cast<float>(hint.w) * kx, static_cast<float>(hint.h) * ky};
}

LayoutChoice ChooseLayout(const std::optional<LayoutHint>& latest,
                          const std::optional<LayoutHint>& applied, int bb_w, int bb_h)
{
  const LayoutBox window{0, 0, static_cast<float>(bb_w), static_cast<float>(bb_h)};
  if (!latest)
    return {window, std::nullopt};
  if (const std::optional<LayoutBox> box = LayoutBoxIn(*latest, bb_w, bb_h))
    return {*box, latest};
  if (applied)
  {
    if (const std::optional<LayoutBox> box = LayoutBoxIn(*applied, bb_w, bb_h))
      return {*box, applied};
  }
  return {window, std::nullopt};
}

MathUtil::Rectangle<int> PlaceInBox(const LayoutBox& box, int draw_w, int draw_h)
{
  // Same rounding as Dolphin's UpdateDrawRectangle, offset by the box's corner.
  const int left = static_cast<int>(std::round(box.x + box.w / 2.0 - draw_w / 2.0));
  const int top = static_cast<int>(std::round(box.y + box.h / 2.0 - draw_h / 2.0));
  return {left, top, left + draw_w, top + draw_h};
}

bool PictureInBackbuffer(const MathUtil::Rectangle<int>& picture, int bb_w, int bb_h)
{
  return picture.left < bb_w && picture.right > 0 && picture.top < bb_h && picture.bottom > 0 &&
         picture.right > picture.left && picture.bottom > picture.top;
}
}  // namespace VideoCommon
