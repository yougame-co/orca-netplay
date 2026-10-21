// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <optional>

#include <gtest/gtest.h>

#include "Core/Orca/UX/Overlay.h"
#include "VideoCommon/PresentLayout.h"

using VideoCommon::ChooseLayout;
using VideoCommon::LayoutChoice;
using VideoCommon::LayoutBox;
using VideoCommon::LayoutBoxIn;
using VideoCommon::LayoutHint;
using VideoCommon::PictureInBackbuffer;
using VideoCommon::PlaceInBox;

namespace
{
LayoutBox ChooseLayoutBox(const std::optional<LayoutHint>& latest,
                          const std::optional<LayoutHint>& applied, int bb_w, int bb_h)
{
  return ChooseLayout(latest, applied, bb_w, bb_h).box;
}

// The page's player box, 1280x720 at (100, 80) in the app's window, with the play menu's panel
// covering its left 440 pixels: `view 100 80 1280 720`, `rect 540 80 840 720`.
constexpr LayoutHint WHOLE{1280, 720, 0, 0, 1280, 720};
constexpr LayoutHint CUT{840, 720, -440, 0, 1280, 720};
}  // namespace

// Without a `view` the picture is laid out in the window, as Dolphin lays it out.
TEST(OrcaLayout, NoHintIsTheWholeBackbuffer)
{
  EXPECT_EQ(ChooseLayoutBox(std::nullopt, std::nullopt, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  // A hint before it (`view off`) changes nothing.
  EXPECT_EQ(ChooseLayoutBox(std::nullopt, CUT, 840, 720), (LayoutBox{0, 0, 840, 720}));
  // A view equal to the window is the window.
  EXPECT_EQ(ChooseLayoutBox(WHOLE, std::nullopt, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
}

// The panel open: the window starts right of it, the picture keeps the whole box, so it is as big
// as before and in the same place on screen, its left 440 pixels under the panel.
TEST(OrcaLayout, ThePanelsCutKeepsThePictureWhereItWas)
{
  const LayoutBox box = ChooseLayoutBox(CUT, WHOLE, 840, 720);
  EXPECT_EQ(box, (LayoutBox{-440, 0, 1280, 720}));
  // 16:9 in a 16:9 box: the whole box, from 440 left of the window.
  const MathUtil::Rectangle<int> pic = PlaceInBox(box, 1280, 720);
  EXPECT_EQ(pic, (MathUtil::Rectangle<int>(-440, 0, 840, 720)));
  // A 4:3 picture is pillarboxed in the box, not in the window.
  EXPECT_EQ(PlaceInBox(box, 960, 720), (MathUtil::Rectangle<int>(-280, 0, 680, 720)));
  // An invite toast's cut from above (the page's other cut): the picture runs above the window.
  const LayoutHint below_toast{1280, 600, 0, -120, 1280, 720};
  EXPECT_EQ(PlaceInBox(ChooseLayoutBox(below_toast, WHOLE, 1280, 600), 1280, 720),
            (MathUtil::Rectangle<int>(0, -120, 1280, 600)));
}

// PlaceInBox centres as Dolphin's UpdateDrawRectangle does in the window (round(w/2 - draw/2)), so
// with no view nothing moves by a pixel.
TEST(OrcaLayout, PlacementInTheWholeWindowIsDolphins)
{
  for (const int w : {640, 641, 1280, 1281, 1919})
  {
    for (const int h : {360, 480, 481, 720, 1080})
    {
      for (const int dw : {w, w - 1, w / 2, (h * 4) / 3})
      {
        const int dh = h - (dw % 3);
        const MathUtil::Rectangle<int> r =
            PlaceInBox(LayoutBox{0, 0, static_cast<float>(w), static_cast<float>(h)}, dw, dh);
        EXPECT_EQ(r.left, static_cast<int>(std::round(w / 2.0 - dw / 2.0))) << w << " " << dw;
        EXPECT_EQ(r.top, static_cast<int>(std::round(h / 2.0 - dh / 2.0))) << h << " " << dh;
        EXPECT_EQ(r.GetWidth(), dw);
        EXPECT_EQ(r.GetHeight(), dh);
      }
    }
  }
}

// The window system resizes the surface a present or two after the `rect`: meanwhile the
// backbuffer still has the old window's size, and the layout the last present used is the one
// that fits it.
TEST(OrcaLayout, TheBackbufferFollowsTheWindowLate)
{
  // The presents of one run, each with the latest hint and the one the last present used.
  std::optional<LayoutHint> applied;
  const auto present = [&](const std::optional<LayoutHint>& latest, int bb_w, int bb_h) {
    const LayoutChoice choice = ChooseLayout(latest, applied, bb_w, bb_h);
    applied = choice.hint;
    return choice.box;
  };
  // The menu shut, `view` the box: the box is the window.
  EXPECT_EQ(present(WHOLE, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_EQ(applied, WHOLE);
  // Opening: the cut is asked for, the backbuffer is still whole: the whole box, as before.
  EXPECT_EQ(present(CUT, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_EQ(applied, WHOLE);
  // Then the backbuffer is cut: the cut's layout.
  EXPECT_EQ(present(CUT, 840, 720), (LayoutBox{-440, 0, 1280, 720}));
  EXPECT_EQ(applied, CUT);
  // Closing: whole again, the backbuffer still cut: the cut's layout until it follows.
  EXPECT_EQ(present(WHOLE, 840, 720), (LayoutBox{-440, 0, 1280, 720}));
  EXPECT_EQ(present(WHOLE, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  // `view` and the cut's `rect` in one batch, the run's first `view`: nothing used before, the
  // backbuffer still whole: the window, which is the box.
  applied.reset();
  EXPECT_EQ(present(CUT, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_FALSE(applied);
  EXPECT_EQ(present(CUT, 840, 720), (LayoutBox{-440, 0, 1280, 720}));
  // `view off`: the window at once, whatever was used before.
  EXPECT_EQ(present(std::nullopt, 840, 720), (LayoutBox{0, 0, 840, 720}));
  EXPECT_FALSE(applied);
  // Neither fits (a hint for a window never shown at that size): the backbuffer, never a guess.
  EXPECT_EQ(ChooseLayoutBox(CUT, std::nullopt, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_EQ(ChooseLayoutBox(CUT, CUT, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
}

// The backbuffer's pixels need not be the window's units: a uniform scale is applied (a window the
// app scales as a whole), and macOS's rounding of backing pixels to points and back still fits.
TEST(OrcaLayout, TheHintIsScaledToTheBackbuffer)
{
  ASSERT_TRUE(LayoutBoxIn(CUT, 1680, 1440));
  EXPECT_EQ(*LayoutBoxIn(CUT, 1680, 1440), (LayoutBox{-880, 0, 2560, 1440}));
  // An odd-sized Retina window: 841 x 721 backing pixels, a drawable a pixel smaller each way.
  const LayoutHint odd{841, 721, -440, 0, 1281, 721};
  const std::optional<LayoutBox> box = LayoutBoxIn(odd, 840, 720);
  ASSERT_TRUE(box);
  EXPECT_NEAR(box->x, -440 * 840.0 / 841, 1e-3);
  EXPECT_NEAR(box->w, 1281 * 840.0 / 841, 1e-3);
  EXPECT_NEAR(box->h, 720, 1e-3);
  // A window of another shape is not this backbuffer's.
  EXPECT_FALSE(LayoutBoxIn(CUT, 1280, 720));
  EXPECT_FALSE(LayoutBoxIn(WHOLE, 840, 720));
  // Anything empty: no layout.
  EXPECT_FALSE(LayoutBoxIn(LayoutHint{0, 720, 0, 0, 1280, 720}, 840, 720));
  EXPECT_FALSE(LayoutBoxIn(LayoutHint{840, 720, 0, 0, 0, 720}, 840, 720));
  EXPECT_FALSE(LayoutBoxIn(CUT, 0, 720));
}

// A window cut to the box's pillarbox bar shows none of the picture. Cropping it to the backbuffer
// would invert its rectangle (a negative viewport), so Present() draws no picture there, only the
// black it clears the backbuffer to.
TEST(OrcaLayout, AWindowOnlyOnTheBarDrawsNoPicture)
{
  using Rect = MathUtil::Rectangle<int>;
  // A 4:3 picture in the 1280 box spans 160..1120 of it; the window is the box's right 120 pixels.
  const LayoutHint bar{120, 720, -1160, 0, 1280, 720};
  const Rect pic = PlaceInBox(ChooseLayoutBox(bar, std::nullopt, 120, 720), 960, 720);
  EXPECT_EQ(pic, Rect(-1000, 0, -40, 720));
  EXPECT_FALSE(PictureInBackbuffer(pic, 120, 720));
  // Above, below or right of the backbuffer, or touching only its edge: none of it either.
  EXPECT_FALSE(PictureInBackbuffer(Rect(0, -720, 1280, 0), 1280, 720));
  EXPECT_FALSE(PictureInBackbuffer(Rect(0, 720, 1280, 1440), 1280, 720));
  EXPECT_FALSE(PictureInBackbuffer(Rect(1280, 0, 2560, 720), 1280, 720));
  EXPECT_FALSE(PictureInBackbuffer(Rect(0, 0, 0, 720), 1280, 720));
  // One column in it is enough; the panel's cut and the whole window are drawn.
  EXPECT_TRUE(PictureInBackbuffer(Rect(-1000, 0, 1, 720), 120, 720));
  EXPECT_TRUE(
      PictureInBackbuffer(PlaceInBox(ChooseLayoutBox(CUT, WHOLE, 840, 720), 960, 720), 840, 720));
  EXPECT_TRUE(PictureInBackbuffer(Rect(0, 0, 1280, 720), 1280, 720));
}

// The overlay's own elements stay in the part of the picture the window shows.
TEST(OrcaLayout, VisiblePictureIsThePictureInTheWindow)
{
  using Orca::UX::PictureArea;
  using Orca::UX::VisiblePicture;
  const auto same = [](const PictureArea& a, const PictureArea& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
  };
  // All of it in the window, letterboxed or not: the picture itself.
  EXPECT_TRUE(same(VisiblePicture({0, 0, 1280, 720}, 1280, 720), {0, 0, 1280, 720}));
  EXPECT_TRUE(same(VisiblePicture({160, 0, 960, 720}, 1280, 720), {160, 0, 960, 720}));
  // The panel's cut: the right 840 pixels.
  EXPECT_TRUE(same(VisiblePicture({-440, 0, 1280, 720}, 840, 720), {0, 0, 840, 720}));
  // A 4:3 picture in that box: from the window's edge to the picture's right end.
  EXPECT_TRUE(same(VisiblePicture({-280, 0, 960, 720}, 840, 720), {0, 0, 680, 720}));
  // A toast's cut from above.
  EXPECT_TRUE(same(VisiblePicture({0, -120, 1280, 720}, 1280, 600), {0, 0, 1280, 600}));
  // Nothing of it in the window (or a NaN): the picture, so the overlay still has a place.
  EXPECT_TRUE(same(VisiblePicture({-2000, 0, 1280, 720}, 840, 720), {-2000, 0, 1280, 720}));
  EXPECT_TRUE(same(VisiblePicture({0, 0, 1280, 720}, 0, 0), {0, 0, 1280, 720}));
  const PictureArea nan{std::nanf(""), 0, 1280, 720};
  EXPECT_TRUE(std::isnan(VisiblePicture(nan, 840, 720).x));
}
