// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <imgui.h>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/Orca/UX/GameFont.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/YgCard.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/TextureConfig.h"

using namespace Orca::UX;

namespace
{
void Put16(std::vector<u8>* v, std::size_t at, u16 x)
{
  (*v)[at] = static_cast<u8>(x >> 8);
  (*v)[at + 1] = static_cast<u8>(x);
}
void Put32(std::vector<u8>* v, std::size_t at, u32 x)
{
  for (int k = 0; k < 4; ++k)
    (*v)[at + static_cast<std::size_t>(k)] = static_cast<u8>(x >> (24 - 8 * k));
}
std::size_t Block(std::vector<u8>* v, const char* magic, std::size_t data_size)
{
  // Blocks are padded to 4 bytes, as the font files' are.
  const std::size_t size = (8 + data_size + 3) / 4 * 4;
  const std::size_t at = v->size();
  v->resize(at + size, 0);
  std::memcpy(v->data() + at, magic, 4);
  Put32(v, at + 4, static_cast<u32>(size));
  return at + 8;
}

// A made-up font in the Wii's format (nothing of the game's): cells 6x8, 2 per row and 2 rows on
// one I4 sheet of 16x24, so four glyphs. Glyph g's texel (x, y) is (g * 4 + x + y) & 15 (I4), so
// a test can tell every glyph and texel apart. Maps: 'A'..'C' direct to 0..2, 'a'..'b' by table
// to 3 and none, U+2026 by scan to 1. Widths per glyph: left g, glyph 5 - g, advance 6 + g.
std::vector<u8> MadeUpFont()
{
  std::vector<u8> f(16, 0);
  std::memcpy(f.data(), "RFNT", 4);
  Put16(&f, 4, 0xFEFF);
  Put16(&f, 6, 0x0104);
  Put16(&f, 12, 16);
  const std::size_t finf = Block(&f, "FINF", 24);
  f[finf + 0] = 1;
  f[finf + 1] = 9;    // line feed
  Put16(&f, finf + 2, 0);  // the stand-in glyph
  f[finf + 4] = 0;
  f[finf + 5] = 6;
  f[finf + 6] = 6;
  f[finf + 20] = 8;  // height
  f[finf + 21] = 6;
  f[finf + 22] = 7;  // ascent
  // The sheet is in the TGLP block, 32-byte aligned in the file, as the Wii's fonts have it.
  const u32 sheet_size = 16 * 24 / 2;
  const std::size_t tglp_at = f.size();
  const std::size_t sheet = (tglp_at + 8 + 24 + 31) / 32 * 32;
  const std::size_t tglp = Block(&f, "TGLP", sheet + sheet_size - tglp_at - 8);
  f[tglp + 0] = 6;
  f[tglp + 1] = 8;
  f[tglp + 2] = 7;
  f[tglp + 3] = 6;
  Put32(&f, tglp + 4, sheet_size);
  Put16(&f, tglp + 8, 1);
  Put16(&f, tglp + 10, 0);  // I4
  Put16(&f, tglp + 12, 2);
  Put16(&f, tglp + 14, 2);
  Put16(&f, tglp + 16, 16);
  Put16(&f, tglp + 18, 24);
  const std::size_t cwdh = Block(&f, "CWDH", 8 + 4 * 3);
  Put16(&f, cwdh, 0);
  Put16(&f, cwdh + 2, 3);
  for (int g = 0; g < 4; ++g)
  {
    f[cwdh + 8 + static_cast<std::size_t>(g) * 3] = static_cast<u8>(g);
    f[cwdh + 9 + static_cast<std::size_t>(g) * 3] = static_cast<u8>(5 - g);
    f[cwdh + 10 + static_cast<std::size_t>(g) * 3] = static_cast<u8>(6 + g);
  }
  const std::size_t direct = Block(&f, "CMAP", 12 + 2);
  Put16(&f, direct, 'A');
  Put16(&f, direct + 2, 'C');
  Put16(&f, direct + 4, 0);
  Put16(&f, direct + 12, 0);
  const std::size_t table = Block(&f, "CMAP", 12 + 4);
  Put16(&f, table, 'a');
  Put16(&f, table + 2, 'b');
  Put16(&f, table + 4, 1);
  Put16(&f, table + 12, 3);
  Put16(&f, table + 14, 0xFFFF);
  const std::size_t scan = Block(&f, "CMAP", 12 + 2 + 4);
  Put16(&f, scan, 0);
  Put16(&f, scan + 2, 0xFFFF);
  Put16(&f, scan + 4, 2);
  Put16(&f, scan + 12, 1);
  Put16(&f, scan + 14, 0x2026);
  Put16(&f, scan + 16, 1);
  // The sheet, I4 in 8x8 blocks: 2 across, 3 down.
  Put32(&f, tglp + 20, static_cast<u32>(sheet));
  std::vector<u8> texels(16 * 24, 0);
  for (int g = 0; g < 4; ++g)
  {
    const int x0 = (g % 2) * 7, y0 = (g / 2) * 9;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 6; ++x)
        texels[static_cast<std::size_t>(y0 + y) * 16 + x0 + x] = static_cast<u8>((g * 4 + x + y) & 15);
  }
  std::size_t p = sheet;
  for (int by = 0; by < 24; by += 8)
    for (int bx = 0; bx < 16; bx += 8)
      for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; x += 2, ++p)
        {
          const u8 hi = texels[static_cast<std::size_t>(by + y) * 16 + bx + x];
          const u8 lo = texels[static_cast<std::size_t>(by + y) * 16 + bx + x + 1];
          f[p] = static_cast<u8>((hi << 4) | lo);
        }
  Put32(&f, 8, static_cast<u32>(f.size()));
  Put16(&f, 14, 6);
  return f;
}
}  // namespace

TEST(OrcaKitFont, Lz10LiteralsAndBackReferences)
{
  // "abcabcabcX": three literals, a back reference of 6 bytes 3 back, a literal.
  const std::vector<u8> packed = {0x10, 10, 0, 0, 0x10, 'a', 'b', 'c', 0x30, 0x02, 'X'};
  const auto out = GameFont::DecompressLz10(packed);
  ASSERT_TRUE(out);
  EXPECT_EQ(std::string(out->begin(), out->end()), "abcabcabcX");
  // Truncated, a reference before the start, the wrong type: refused.
  EXPECT_FALSE(GameFont::DecompressLz10(std::vector<u8>{0x10, 10, 0, 0, 0x08, 'a'}));
  EXPECT_FALSE(GameFont::DecompressLz10(std::vector<u8>{0x10, 4, 0, 0, 0x80, 0x10, 0x05}));
  EXPECT_FALSE(GameFont::DecompressLz10(std::vector<u8>{0x11, 1, 0, 0, 0, 'a'}));
}

TEST(OrcaKitFont, Utf8CodePoints)
{
  const std::string s = "a\xc2\xb7\xe2\x80\xa6\xff\xe2\x80";
  std::size_t at = 0;
  EXPECT_EQ(GameFont::NextCodePoint(s, &at), U'a');
  EXPECT_EQ(GameFont::NextCodePoint(s, &at), U'·');
  EXPECT_EQ(GameFont::NextCodePoint(s, &at), U'…');
  EXPECT_EQ(GameFont::NextCodePoint(s, &at), char32_t{0xFFFD});
  EXPECT_EQ(GameFont::NextCodePoint(s, &at), char32_t{0xFFFD});
  EXPECT_EQ(at, s.size());
}

TEST(OrcaKitFont, ReadsTheMapsTheWidthsAndTheI4Sheet)
{
  std::string error;
  const auto font = GameFont::Rfnt::Parse(MadeUpFont(), &error);
  ASSERT_TRUE(font) << error;
  EXPECT_EQ(font->CellWidth(), 6);
  EXPECT_EQ(font->CellHeight(), 8);
  EXPECT_EQ(font->Ascent(), 7);
  EXPECT_EQ(font->LineFeed(), 9);
  EXPECT_EQ(font->GlyphCount(), 4u);
  EXPECT_EQ(font->IndexOf('A'), 0);
  EXPECT_EQ(font->IndexOf('C'), 2);
  EXPECT_EQ(font->IndexOf('a'), 3);
  EXPECT_FALSE(font->IndexOf('b'));  // the table's "none"
  EXPECT_FALSE(font->IndexOf('Z'));
  EXPECT_EQ(font->IndexOf(U'…'), 1);
  for (u16 g = 0; g < 4; ++g)
  {
    const GameFont::Widths w = font->WidthsOf(g);
    EXPECT_EQ(w.left, g);
    EXPECT_EQ(w.glyph, 5 - g);
    EXPECT_EQ(w.advance, 6 + g);
    std::vector<u8> alpha;
    ASSERT_TRUE(font->GlyphAlpha(g, &alpha));
    ASSERT_EQ(alpha.size(), 48u);
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 6; ++x)
        ASSERT_EQ(alpha[static_cast<std::size_t>(y) * 6 + x], ((g * 4 + x + y) & 15) * 17)
            << g << " " << x << " " << y;
  }
  std::vector<u8> alpha;
  EXPECT_FALSE(font->GlyphAlpha(4, &alpha));
}

TEST(OrcaKitFont, RefusesWhatItCantRead)
{
  std::vector<u8> f = MadeUpFont();
  EXPECT_FALSE(GameFont::Rfnt::Parse(std::vector<u8>(f.begin(), f.begin() + 12)));
  std::vector<u8> magic = f;
  magic[3] = 'X';
  EXPECT_FALSE(GameFont::Rfnt::Parse(magic));
  std::vector<u8> version = f;
  version[7] = 0x02;
  EXPECT_FALSE(GameFont::Rfnt::Parse(version));
  // The sheet cut short: the TGLP points past the end.
  std::vector<u8> cut(f.begin(), f.end() - 40);
  Put32(&cut, 8, static_cast<u32>(cut.size()));
  EXPECT_FALSE(GameFont::Rfnt::Parse(cut));
  // A format the parser doesn't decode (RGB565).
  std::vector<u8> format = f;
  const std::size_t tglp = 16 + 8 + 24 + 8;
  ASSERT_EQ(std::string(format.begin() + tglp - 8, format.begin() + tglp - 4), "TGLP");
  format[tglp + 11] = 4;
  EXPECT_FALSE(GameFont::Rfnt::Parse(format));
}

TEST(OrcaKitFont, AtlasCellsApartWithMipLevels)
{
  const auto font = GameFont::Rfnt::Parse(MadeUpFont());
  ASSERT_TRUE(font);
  const std::vector<char32_t> codes = {'A', 'B', 'C', 'a', 'b', U'…', 'Z'};
  const GameFont::Atlas atlas = GameFont::BuildAtlas(*font, codes, 4, 3);
  // 'b' and 'Z' aren't in the font.
  EXPECT_EQ(atlas.glyphs.size(), 5u);
  EXPECT_FALSE(atlas.glyphs.count('b'));
  ASSERT_EQ(atlas.levels.size(), 3u);
  EXPECT_EQ(atlas.levels[0].size(), static_cast<std::size_t>(atlas.width) * atlas.height * 4);
  EXPECT_EQ(atlas.levels[1].size(), static_cast<std::size_t>(atlas.width / 2) * (atlas.height / 2) * 4);
  EXPECT_FLOAT_EQ(atlas.cell_height, 8);
  // Each glyph's cell where its widths say, its texels the font's, and nothing of another glyph
  // within `pad` of it.
  for (const auto& [code, g] : atlas.glyphs)
  {
    const u16 index = *font->IndexOf(code);
    EXPECT_FLOAT_EQ(g.left, index);
    EXPECT_FLOAT_EQ(g.width, 5 - index);
    EXPECT_FLOAT_EQ(g.advance, 6 + index);
    const int x0 = static_cast<int>(std::lround(g.u0 * atlas.width));
    const int y0 = static_cast<int>(std::lround(g.v0 * atlas.height));
    EXPECT_EQ(std::lround((g.u1 - g.u0) * atlas.width), 5 - index);
    EXPECT_EQ(std::lround((g.v1 - g.v0) * atlas.height), 8);
    for (int y = -4; y < 12; ++y)
    {
      for (int x = -4; x < 10; ++x)
      {
        const u8 a = atlas.levels[0][(static_cast<std::size_t>(y0 + y) * atlas.width + x0 + x) * 4 + 3];
        const int want = (x >= 0 && x < 6 && y >= 0 && y < 8) ? ((index * 4 + x + y) & 15) * 17 : 0;
        ASSERT_EQ(a, want) << static_cast<int>(code) << " " << x << " " << y;
      }
    }
  }
  // White everywhere; a level's alpha the mean of the four under it.
  EXPECT_EQ(atlas.levels[1][0], 255);
  const auto& l0 = atlas.levels[0];
  const auto& l1 = atlas.levels[1];
  const int w0 = atlas.width, w1 = atlas.width / 2;
  for (int y = 0; y < atlas.height / 2; ++y)
  {
    for (int x = 0; x < w1; ++x)
    {
      const int sum = l0[(static_cast<std::size_t>(2 * y) * w0 + 2 * x) * 4 + 3] +
                      l0[(static_cast<std::size_t>(2 * y) * w0 + 2 * x + 1) * 4 + 3] +
                      l0[(static_cast<std::size_t>(2 * y + 1) * w0 + 2 * x) * 4 + 3] +
                      l0[(static_cast<std::size_t>(2 * y + 1) * w0 + 2 * x + 1) * 4 + 3];
      ASSERT_EQ(l1[(static_cast<std::size_t>(y) * w1 + x) * 4 + 3], (sum + 2) / 4);
    }
  }
}

TEST(OrcaKitFont, OverlayPunctuationHasSubstitutes)
{
  for (const char32_t c : {U'·', U'–', U'—', U'…', U'’', U'“'})
    EXPECT_FALSE(GameFont::Substitute(c).empty()) << static_cast<int>(c);
  EXPECT_EQ(GameFont::Substitute(U'…'), U"...");
  EXPECT_TRUE(GameFont::Substitute(U'Q').empty());
}

// Without the game's fonts (no disc or GPU here), every primitive draws in ImGui's font, in a frame
// set up the way OnScreenUI does, without an ImGui assert.
TEST(OrcaKitDraw, EveryPrimitiveDrawsWithoutTheGamesFont)
{
  ImGuiContext* context = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  io.Fonts->AddFontDefault();
  const std::size_t failures = ImGuiAssertFailures();
  for (const float h : {100.0f, 720.0f, 2160.0f})
  {
    io.DisplaySize = ImVec2(h * 16 / 9, h);
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    Kit::BeginFrame();
    EXPECT_FALSE(Kit::HasGameFont(Kit::Face::Body));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = h / 100;
    const int before = dl->VtxBuffer.Size;
    const Kit::TextLook look = Kit::LabelLook(u);
    const ImVec2 s = Kit::Measure(look, "ada · 1532");
    EXPECT_GT(s.x, 0);
    EXPECT_GT(s.y, 0);
    const ImVec2 drawn = Kit::Text(dl, ImVec2(10, 10), look, "ada · 1532", 1, Kit::Align::Centre);
    EXPECT_FLOAT_EQ(drawn.x, s.x);
    // Both looks, every primitive.
    for (const Kit::Look look_now : {Kit::Look::Brawl, Kit::Look::ProjectPlus})
    {
      Kit::SetLook(look_now);
      const int demo = dl->VtxBuffer.Size;
      Kit::DrawDemo(dl, ImVec2(0, 0), io.DisplaySize, u, 3.25);
      EXPECT_GT(dl->VtxBuffer.Size, demo);
      for (int bars = 0; bars <= 4; ++bars)
        Kit::Connection(dl, ImVec2(50, 50), 3 * u, u, bars);
    }
    Kit::SetLook(Kit::Look::Brawl);
    EXPECT_GT(dl->VtxBuffer.Size, before);
    // Wrapping: every line within the width but a lone long word, the words all kept in order.
    const std::string chat = "bo: that was close, one more? supercalifragilisticexpialidocious ok";
    const std::vector<std::string> lines = Kit::Wrap(look, chat, 20 * u);
    ASSERT_GE(lines.size(), 2u);
    std::string joined;
    for (const std::string& line : lines)
    {
      if (line.find(' ') != std::string::npos)
        EXPECT_LE(Kit::Measure(look, line).x, 20 * u) << line;
      joined += (joined.empty() ? "" : " ") + line;
    }
    EXPECT_EQ(joined, chat);
    // The chat's plate: one plate round every line, the sender's name in the accent.
    const Kit::Box chat_plate = Kit::TextPlate(dl, ImVec2(10, 10), u, lines, 3.0f, 3);
    EXPECT_GT(chat_plate.max.y - chat_plate.min.y,
              static_cast<float>(lines.size()) * Kit::InkLook(u, 3.0f).px);
    for (const std::string& line : lines)
      EXPECT_GE(chat_plate.max.x - chat_plate.min.x, Kit::Measure(Kit::InkLook(u, 3.0f), line).x);
    // A banner as wide as BannerWidth fits its words; a badge as big as BadgeSize says.
    const float bw = Kit::BannerWidth(8 * u, u, "Press Start to lock in", "Ranked - Game 1");
    EXPECT_GT(bw, Kit::Measure(Kit::InkLook(u, 8 * 0.44f), "Press Start to lock in").x);
    const ImVec2 bs = Kit::BadgeSize(u, "+16", Kit::Tone::Green, 3.8f);
    const Kit::Box badge = Kit::Badge(dl, ImVec2(100, 100), u, "+16", Kit::Tone::Green, 3.8f);
    EXPECT_NEAR(badge.max.x - badge.min.x, bs.x, 0.01f);
    EXPECT_NEAR(badge.max.y - badge.min.y, bs.y, 0.01f);
    Kit::Disc(dl, ImVec2(60, 60), 5 * u, u, Kit::Tone::Panel);
    // A fade to nothing draws nothing.
    const int faded = dl->VtxBuffer.Size;
    Kit::Text(dl, ImVec2(10, 10), look, "gone", 0);
    EXPECT_EQ(dl->VtxBuffer.Size, faded);
    ImGui::Render();
  }
  EXPECT_EQ(ImGuiAssertFailures(), failures);
  ImGui::DestroyContext(context);
}

// The overlay's pieces: the turn pulse on a one-second beat, the portrait ribbon keeping its words
// inside it, the set dots, the searching ring, and the YouGame cards (the site's avatar colours,
// chat wrapped inside its card), all drawn in an ImGui frame without an assert.
TEST(OrcaKitDraw, DesignFPieces)
{
  const Kit::Beat rest = Kit::TurnPulse(7.0), peak = Kit::TurnPulse(7.5), late = Kit::TurnPulse(7.9);
  EXPECT_NEAR(rest.swell, 0, 1e-4);
  EXPECT_NEAR(peak.swell, 1, 1e-4);
  EXPECT_NEAR(rest.ripple, 0, 1e-4);
  EXPECT_NEAR(late.ripple, 0.9, 1e-3);
  EXPECT_NEAR(Kit::TurnPulse(8.25).swell, Kit::TurnPulse(1.25).swell, 1e-4);

  // The site's avatarColor (web/src/lib/site.ts): ada #1D4ED8, bo #0F766E, case-blind; a Latin-1
  // name hashed as JavaScript's UTF-16 units of its lower case (Ángel #DC2626, byte-wise it isn't).
  EXPECT_EQ(Yg::AvatarColour("ada"), IM_COL32(29, 78, 216, 255));
  EXPECT_EQ(Yg::AvatarColour("ADA"), IM_COL32(29, 78, 216, 255));
  EXPECT_EQ(Yg::AvatarColour("bo"), IM_COL32(15, 118, 110, 255));
  EXPECT_EQ(Yg::AvatarColour("\u00c1ngel"), IM_COL32(220, 38, 38, 255));
  EXPECT_EQ(Yg::AvatarColour("\u00c1NGEL"), IM_COL32(220, 38, 38, 255));

  ImGuiContext* context = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  io.Fonts->AddFontDefault();
  const std::size_t failures = ImGuiAssertFailures();
  for (const float h : {360.0f, 720.0f, 2160.0f})
  {
    io.DisplaySize = ImVec2(h * 16 / 9, h);
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    Kit::BeginFrame();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = h / 100;
    for (const Kit::Look look : {Kit::Look::Brawl, Kit::Look::ProjectPlus})
    {
      Kit::SetLook(look);
      // On their turn the pointer draws its glow and ripple too.
      const int a = dl->VtxBuffer.Size;
      Kit::Pointer(dl, ImVec2(100, 100), 3 * u, u, 1, "P2 BO", false, false, 3.4);
      const int idle = dl->VtxBuffer.Size - a;
      const int b = dl->VtxBuffer.Size;
      Kit::Pointer(dl, ImVec2(100, 100), 3 * u, u, 1, "P2 BO", true, false, 3.4);
      EXPECT_GT(dl->VtxBuffer.Size - b, idle);
      // Acting draws its blink and its turn's burst; waiting draws no more than plain.
      const int c = dl->VtxBuffer.Size;
      Kit::Pointer(dl, ImVec2(100, 100), 3 * u, u, 1, "P2 BO", Kit::PointerMode::Acting, false,
                   3.4, 3.3);
      EXPECT_GT(dl->VtxBuffer.Size - c, idle);
      const int d = dl->VtxBuffer.Size;
      Kit::Pointer(dl, ImVec2(100, 100), 3 * u, u, 1, "P2 BO", Kit::PointerMode::Waiting, false,
                   3.4, 0);
      EXPECT_EQ(dl->VtxBuffer.Size - d, idle);
      // The turn's tag: above the cursor, inside the clamp; below it near the top; the timer
      // beside it.
      const ImVec2 lo(0, 0), hi(h * 16 / 9, h);
      const ImVec2 mid(hi.x / 2, hi.y / 2);
      const Kit::Box tag = Kit::TurnTag(dl, mid, 3 * u, u, "YOUR TURN",
                                        "X STRIKES A STAGE \u00b7 2 LEFT", Kit::Tone::Gold, 24,
                                        3.4, true, lo, hi, 1.0f);
      EXPECT_LT(tag.max.y, mid.y);
      EXPECT_GE(tag.min.x, 0);
      EXPECT_LT(tag.max.x, mid.x + 3 * u);
      const Kit::Box low = Kit::TurnTag(dl, ImVec2(5, 5), 3 * u, u, "BO PICKS", {},
                                        Kit::PortTone(1), -1, 3.4, false, lo, hi);
      EXPECT_GT(low.min.y, 5);
      EXPECT_GE(low.min.x, 0);
      // A waiting player's tab goes under its cursor.
      const Kit::Box under = Kit::TurnTag(dl, mid, 3 * u, u, "WAITING FOR BO", {},
                                          Kit::Tone::Grey, -1, 3.4, true, lo, hi, 0, 2.6f, true);
      EXPECT_GT(under.min.y, mid.y);
      Kit::PortraitFrame(dl, ImVec2(10, 10), ImVec2(10 + 28 * u, 10 + 22 * u), u, Kit::Tone::Gold);
      Kit::PortraitFrame(dl, ImVec2(10, 10), ImVec2(10 + 28 * u, 10 + 22 * u), u, Kit::Tone::Red);
      // A ribbon narrower than its words still draws them (shrunk), the ribbon as asked.
      for (const float w : {24.0f, 8.0f})
      {
        const Kit::Box r = Kit::Ribbon(dl, ImVec2(20, 20), ImVec2(20 + w * u, 20 + 4.4f * u), u,
                                       "LOCKED IN", Kit::Tone::Gold);
        EXPECT_NEAR(r.max.x - r.min.x, w * u, 0.01f);
      }
      const Kit::Box dots = Kit::SetDots(dl, ImVec2(300, 20), u, {0, 1, -1}, 2);
      EXPECT_NEAR((dots.min.x + dots.max.x) / 2, 300, 0.01f);
      EXPECT_NEAR(dots.max.y - dots.min.y, 1.8f * u, 0.01f);
      Kit::SetDots(dl, ImVec2(300, 20), u, {-1, -1, -1}, -1);
      Kit::SearchRing(dl, ImVec2(200, 200), 7 * u, u, 12.3);
      // The timers in the game's lettering: the box of "0:00" round the centre, the same box in
      // the last seconds' swell.
      for (const Kit::Lettering lettering : {Kit::Lettering::Band, Kit::Lettering::Line})
      {
        const int before = dl->VtxBuffer.Size;
        const Kit::Box t = Kit::GameTimer(dl, ImVec2(400, 300), u, 43, 5.0, lettering);
        EXPECT_GT(dl->VtxBuffer.Size, before);
        EXPECT_NEAR((t.min.x + t.max.x) / 2, 400, 0.01f);
        const Kit::Box urgent = Kit::GameTimer(dl, ImVec2(400, 300), u, 3, 5.0, lettering);
        EXPECT_NEAR(urgent.max.x - urgent.min.x, t.max.x - t.min.x, 0.01f);
      }
      // The control guide ends where asked and grows leftwards.
      const Kit::Box guide = Kit::Guide(dl, 600, 100, u, Kit::GuideOf("Pick with A, then press Start to lock in"));
      EXPECT_NEAR(guide.max.x, 600, 0.01f);
      EXPECT_LT(guide.min.x, 600 - 10 * u);
      EXPECT_LE(guide.max.y - guide.min.y, 3.4f * u);
      // Longer than allowed: the line drawn smaller (to 70%), its right end where asked.
      const std::vector<Kit::GuideItem> long_hint = Kit::GuideOf(
          "A on the stage you want \u00b7 the same pick plays there, else a coin takes one of the "
          "two \u00b7 bo wants Final Destination \u00b7 A on it to agree");
      const Kit::Box wide = Kit::Guide(dl, 600, 100, u, long_hint);
      const float room = 0.8f * (600 - wide.min.x);
      const Kit::Box capped = Kit::Guide(dl, 600, 100, u, long_hint, 1, room);
      EXPECT_NEAR(capped.max.x, 600, 0.01f);
      EXPECT_LE(600 - capped.min.x, room + 0.01f);
      EXPECT_GT(600 - capped.min.x, 0.9f * room);
      // Never under 70%: then it is the line at 70% of the size.
      const Kit::Box floor = Kit::Guide(dl, 600, 100, u, long_hint, 1, 0.1f * (600 - wide.min.x));
      const Kit::Box seventy = Kit::Guide(dl, 600, 100, 0.7f * u, long_hint);
      EXPECT_NEAR(floor.min.x, seventy.min.x, 0.5f);
    }
    Kit::SetLook(Kit::Look::Brawl);
    // The chat's card: wider than nothing, no wider than asked, its lines within its width.
    const float width = 0.36f * io.DisplaySize.x;
    const std::string text = "gg, that last stock though. one more after this? rematch on Battlefield";
    const Kit::Box card = Yg::ChatCard(dl, ImVec2(10, 10), u, "BO", text, width);
    EXPECT_GT(card.max.x - card.min.x, 10 * u);
    EXPECT_LE(card.max.x - card.min.x, width + 0.01f);
    const std::vector<std::string> lines = Yg::Wrap(text, 1.95f * u, width - 9 * u);
    EXPECT_GE(lines.size(), 1u);
    const Kit::Box tall = Yg::ChatCard(dl, ImVec2(10, 10), u, "BO", text, 30 * u);
    EXPECT_GT(tall.max.y - tall.min.y, card.max.y - card.min.y);
    // A message without spaces (a link, a laugh) wraps through the word: every line fits.
    const float room = 30 * u;
    const std::vector<std::string> long_word = Yg::Wrap(std::string(140, 'h'), 1.95f * u, room);
    EXPECT_GT(long_word.size(), 1u);
    std::size_t letters = 0;
    for (const std::string& line : long_word)
    {
      EXPECT_LE(Yg::TextWidth(line, 1.95f * u), room + 0.5f);
      letters += line.size();
    }
    EXPECT_EQ(letters, 140u);
    const Kit::Box notice = Yg::NoticeCard(dl, ImVec2(10, 10), u, "bo joined", width);
    EXPECT_LE(notice.max.x - notice.min.x, width + 0.01f);
    // Faded out: the box, nothing drawn.
    const int faded = dl->VtxBuffer.Size;
    Yg::ChatCard(dl, ImVec2(10, 10), u, "BO", "gone", width, 0);
    EXPECT_EQ(dl->VtxBuffer.Size, faded);
    ImGui::Render();
  }
  EXPECT_EQ(ImGuiAssertFailures(), failures);
  ImGui::DestroyContext(context);
}

// Orca's hints as the control guide's buttons and words (the bottom-right legend).
// The acting player's blink: 2.5 beats a second from the turn's start, lit 60% of each beat, a
// burst of three ripples in the first 0.8 s and none after.
TEST(OrcaKit, AttentionBlinks)
{
  const Kit::Blink start = Kit::Attention(10.0, 10.0);
  EXPECT_EQ(start.on, 1.0f);
  EXPECT_NEAR(start.ripples[0], 0, 1e-4f);
  EXPECT_EQ(start.ripples[1], -1);
  EXPECT_NEAR(Kit::Attention(10.3, 10.0).on, 0.15f, 1e-4f);
  EXPECT_EQ(Kit::Attention(10.42, 10.0).on, 1.0f);
  const Kit::Blink burst = Kit::Attention(10.5, 10.0);
  EXPECT_EQ(burst.ripples[0], -1);
  EXPECT_GE(burst.ripples[1], 0);
  EXPECT_GE(burst.ripples[2], 0);
  const Kit::Blink later = Kit::Attention(11.0, 10.0);
  for (const float r : later.ripples)
    EXPECT_EQ(r, -1);
  EXPECT_GT(Kit::Attention(10.12, 10.0).swell, 0.5f);
  EXPECT_EQ(Kit::Attention(10.3, 10.0).swell, 0.0f);
}

TEST(OrcaKit, GuideOfOrcasHints)
{
  using G = Kit::GuideItem;
  EXPECT_EQ(Kit::GuideOf("B to change \u00b7 Hold B to leave"),
            (std::vector<G>{{"B", "Change"}, {"B", "Hold: leave"}}));
  EXPECT_EQ(Kit::GuideOf("Pick with A, then press Start to lock in"),
            (std::vector<G>{{"A", "Pick"}, {"Start", "Lock in"}}));
  EXPECT_EQ(Kit::GuideOf("Press Start to lock in"), (std::vector<G>{{"Start", "Lock in"}}));
  EXPECT_EQ(Kit::GuideOf("Start locks it in \u00b7 B to change"),
            (std::vector<G>{{"Start", "Lock it in"}, {"B", "Change"}}));
  EXPECT_EQ(Kit::GuideOf("A on a character, then Start"),
            (std::vector<G>{{"A", "Pick a character"}, {"Start", ""}}));
  // The stage select's: strikes, bans, skips, proposals.
  EXPECT_EQ(Kit::GuideOf("X on a stage strikes it \u00b7 A proposes a stage"),
            (std::vector<G>{{"X", "Strike"}, {"A", "Propose a stage"}}));
  EXPECT_EQ(Kit::GuideOf("X on a stage bans it \u00b7 Y skips"),
            (std::vector<G>{{"X", "Ban"}, {"Y", "Skip"}}));
  EXPECT_EQ(Kit::GuideOf("bo wants Smashville \u00b7 A on it to agree"),
            (std::vector<G>{{"", "bo wants Smashville"}, {"A", "Agree"}}));
  EXPECT_EQ(Kit::GuideOf("Hold B to leave"), (std::vector<G>{{"B", "Hold: leave"}}));
  // The one-time tip on the queue's own select: a word, then the hold.
  EXPECT_EQ(Kit::GuideOf("New \u00b7 Hold B to leave"),
            (std::vector<G>{{"", "New"}, {"B", "Hold: leave"}}));
  EXPECT_EQ(Kit::GuideOf("Your turn: X on a stage strikes it \u00b7 A proposes a stage"),
            (std::vector<G>{{"X", "Strike"}, {"A", "Propose a stage"}}));
  EXPECT_TRUE(Kit::GuideOf("").empty());
}

// Each game's look: Brawl's titles cream into gold, Project+'s white into mint; the labels white in
// both, and the look follows the last SetLook.
TEST(OrcaKitDraw, TitlesFollowTheLook)
{
  Kit::SetLook(Kit::Look::Brawl);
  EXPECT_EQ(Kit::CurrentLook(), Kit::Look::Brawl);
  const Kit::TextLook brawl = Kit::TitleLook(7.2f);
  Kit::SetLook(Kit::Look::ProjectPlus);
  EXPECT_EQ(Kit::CurrentLook(), Kit::Look::ProjectPlus);
  const Kit::TextLook pplus = Kit::TitleLook(7.2f);
  Kit::SetLook(Kit::Look::Brawl);
  const auto channel = [](ImU32 c, int shift) { return static_cast<int>((c >> shift) & 0xFF); };
  // Gold: more red than blue at the bottom. Mint: more green and blue than red.
  EXPECT_GT(channel(brawl.bottom, IM_COL32_R_SHIFT), channel(brawl.bottom, IM_COL32_B_SHIFT) + 100);
  EXPECT_GT(channel(pplus.bottom, IM_COL32_G_SHIFT), channel(pplus.bottom, IM_COL32_R_SHIFT) + 100);
  EXPECT_GT(channel(pplus.bottom, IM_COL32_B_SHIFT), channel(pplus.bottom, IM_COL32_R_SHIFT) + 50);
  EXPECT_EQ(brawl.px, pplus.px);
  EXPECT_EQ(Kit::LabelLook(7.2f).top, IM_COL32_WHITE);
}

namespace
{
// A backend that only counts the textures it has made and not yet destroyed.
class CountingTexture final : public AbstractTexture
{
public:
  CountingTexture(const TextureConfig& config, int* live) : AbstractTexture(config), m_live(live)
  {
    ++*m_live;
  }
  ~CountingTexture() override { --*m_live; }
  void CopyRectangleFromTexture(const AbstractTexture*, const MathUtil::Rectangle<int>&, u32, u32,
                                const MathUtil::Rectangle<int>&, u32, u32) override
  {
  }
  void ResolveFromTexture(const AbstractTexture*, const MathUtil::Rectangle<int>&, u32,
                          u32) override
  {
  }
  void Load(u32, u32, u32, u32, const u8*, size_t, u32) override {}

private:
  int* m_live;
};

class CountingGfx final : public AbstractGfx
{
public:
  bool IsHeadless() const override { return true; }
  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view) override
  {
    return std::make_unique<CountingTexture>(config, &live);
  }
  std::unique_ptr<AbstractStagingTexture> CreateStagingTexture(StagingTextureType,
                                                               const TextureConfig&) override
  {
    return nullptr;
  }
  std::unique_ptr<AbstractFramebuffer> CreateFramebuffer(AbstractTexture*, AbstractTexture*,
                                                         std::vector<AbstractTexture*>) override
  {
    return nullptr;
  }
  std::unique_ptr<AbstractShader> CreateShaderFromSource(ShaderStage, std::string_view,
                                                         VideoCommon::ShaderIncluder*,
                                                         std::string_view) override
  {
    return nullptr;
  }
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage, const void*, size_t,
                                                         std::string_view) override
  {
    return nullptr;
  }
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration&) override
  {
    return nullptr;
  }
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig&, const void*,
                                                   size_t) override
  {
    return nullptr;
  }
  SurfaceInfo GetSurfaceInfo() const override { return {}; }

  int live = 0;
};
}  // namespace

// The overlay's font textures go when the backend shuts down (VideoBackendBase::ShutdownShared
// calls OSD::ReleaseHostOverlay, the overlay registers Kit::ReleaseGpu), not at process exit. One
// kept past the backend held a D3D11 device reference each (texture and view): the device and the
// driver lived on after D3D::Destroy unloaded d3d11.dll, and a driver thread calling back into it
// ended Orca with 0xC0000005 at exit; outside the app, the textures' destructors ran after their
// backend at exit and crashed every time.
TEST(OrcaKit, FontTexturesGoWithTheBackend)
{
  GameFont::Atlas atlas;
  atlas.width = 4;
  atlas.height = 4;
  atlas.levels = {std::vector<u8>(4 * 4 * 4, 0xff), std::vector<u8>(2 * 2 * 4, 0xff)};
  atlas.cell_height = 4;
  Kit::UseFontsForTests(&atlas, &atlas);
  auto gfx = std::make_unique<CountingGfx>();
  CountingGfx* counting = gfx.get();
  g_gfx = std::move(gfx);
  OSD::SetHostOverlay([] {}, &Kit::ReleaseGpu);

  Kit::BeginFrame();
  EXPECT_EQ(counting->live, 2);
  EXPECT_TRUE(Kit::HasGameFont(Kit::Face::Body));
  EXPECT_TRUE(Kit::HasGameFont(Kit::Face::Heavy));
  Kit::BeginFrame();
  EXPECT_EQ(counting->live, 2);

  // The backend shuts down: the textures go while it is alive.
  OSD::ReleaseHostOverlay();
  EXPECT_EQ(counting->live, 0);
  EXPECT_FALSE(Kit::HasGameFont(Kit::Face::Body));
  EXPECT_FALSE(Kit::HasGameFont(Kit::Face::Heavy));

  // A backend started again uploads them again.
  Kit::BeginFrame();
  EXPECT_EQ(counting->live, 2);
  OSD::ReleaseHostOverlay();
  EXPECT_EQ(counting->live, 0);

  OSD::SetHostOverlay({});
  g_gfx.reset();
  Kit::UseFontsForTests(nullptr, nullptr);
}

// With a Brawl disc image (ORCA_TEST_DISC): both font faces read, unpack and parse, with every
// letter and digit the overlay writes, and nothing from them is kept outside this process.
TEST(OrcaKitFont, ReadsBothFacesFromTheDisc)
{
  const char* disc = std::getenv("ORCA_TEST_DISC");
  if (!disc || !*disc)
    GTEST_SKIP() << "set ORCA_TEST_DISC to a Brawl (USA, Rev 2) image";
  const std::array<const char*, 3> files = {"system/font/font_latin1.arc",
                                            "system/font/font_hira.brfnt", "system/nothing.brfnt"};
  std::string error;
  const auto fonts = GameFont::ReadFromDisc(disc, files, "RSBE01", &error);
  ASSERT_EQ(fonts.size(), 3u);
  ASSERT_TRUE(fonts[0]);
  ASSERT_TRUE(fonts[1]);
  EXPECT_FALSE(fonts[2]);
  EXPECT_NE(error.find("nothing"), std::string::npos) << error;
  EXPECT_EQ(fonts[0]->CellHeight(), 40);
  EXPECT_EQ(fonts[1]->CellHeight(), 32);
  const std::vector<char32_t> codes = GameFont::OverlayCodePoints();
  // The heavy face has no brackets, no # $ * < > _ | and little of Latin-1's punctuation.
  for (const char c : std::string("()[]#*/"))
    EXPECT_TRUE(fonts[0]->IndexOf(static_cast<char32_t>(c))) << c;
  for (const auto& font : {fonts[0], fonts[1]})
  {
    for (const char c : std::string("ABCXYZabcxyz0123456789:+-.,!?'"))
      EXPECT_TRUE(font->IndexOf(static_cast<char32_t>(c))) << c;
    const GameFont::Atlas atlas = GameFont::BuildAtlas(*font, codes);
    EXPECT_GT(atlas.glyphs.size(), 150u);
    EXPECT_LE(atlas.width, 2048);
    EXPECT_LE(atlas.height, 2048);
    EXPECT_GT(atlas.glyphs.at('W').advance, atlas.glyphs.at('i').advance);
  }
  // The wrong game: nothing read.
  const auto wrong = GameFont::ReadFromDisc(disc, files, "GALE01", &error);
  EXPECT_FALSE(wrong[0]);
}

// Design checks only (ORCA_KIT_DUMP=<dir> and ORCA_TEST_DISC): the demo drawn at 1280x720 with the
// disc's fonts, its draw data and textures written to <dir> for a rasterizer to show without the
// game running. What it writes holds the game's glyphs: <dir> must be a scratch folder, deleted
// after, never committed.
TEST(OrcaKitDraw, DumpTheDemoForDesignChecks)
{
  const char* dir = std::getenv("ORCA_KIT_DUMP");
  const char* disc = std::getenv("ORCA_TEST_DISC");
  if (!dir || !*dir || !disc || !*disc)
    GTEST_SKIP() << "design checks only: ORCA_KIT_DUMP=<dir> ORCA_TEST_DISC=<disc>";
  const std::array<const char*, 2> files = {"system/font/font_latin1.arc",
                                            "system/font/font_hira.brfnt"};
  const auto fonts = GameFont::ReadFromDisc(disc, files, "RSBE01");
  ASSERT_TRUE(fonts[0] && fonts[1]);
  const std::vector<char32_t> codes = GameFont::OverlayCodePoints();
  const std::array<GameFont::Atlas, 2> atlases = {GameFont::BuildAtlas(*fonts[0], codes),
                                                 GameFont::BuildAtlas(*fonts[1], codes)};
  Kit::UseAtlasForTests(Kit::Face::Body, &atlases[0], 1);
  Kit::UseAtlasForTests(Kit::Face::Heavy, &atlases[1], 2);
  ImGuiContext* context = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  io.Fonts->AddFontDefault();
  io.DisplaySize = ImVec2(1280, 720);
  io.DeltaTime = 1.0f / 60;
  const double now = std::getenv("ORCA_KIT_DUMP_TIME") ? std::atof(std::getenv("ORCA_KIT_DUMP_TIME")) : 3.25;
  ImGui::NewFrame();
  Kit::DrawDemo(ImGui::GetForegroundDrawList(), ImVec2(15, 0), ImVec2(1250, 720), 7.2f, now);
  ImGui::Render();
  ImDrawData* data = ImGui::GetDrawData();
  // ImGui's own font atlas: an id of its own.
  ImTextureID next = 100;
  const std::string out(dir);
  const auto write = [](const std::string& path, const void* bytes, std::size_t size) {
    File::IOFile f(path, "wb");
    ASSERT_TRUE(f.WriteBytes(bytes, size)) << path;
  };
  std::string index = "";
  if (data->Textures)
  {
    for (ImTextureData* tex : *data->Textures)
    {
      tex->SetTexID(next);
      tex->SetStatus(ImTextureStatus_OK);
      write(fmt::format("{}/tex-{}-0.rgba", out, next), tex->Pixels,
            static_cast<std::size_t>(tex->Width) * tex->Height * 4);
      index += fmt::format("tex {} 0 {} {}\n", next, tex->Width, tex->Height);
      ++next;
    }
  }
  for (int a = 0; a < 2; ++a)
  {
    for (std::size_t level = 0; level < atlases[a].levels.size(); ++level)
    {
      write(fmt::format("{}/tex-{}-{}.rgba", out, a + 1, level), atlases[a].levels[level].data(),
            atlases[a].levels[level].size());
      index += fmt::format("tex {} {} {} {}\n", a + 1, level, atlases[a].width >> level,
                           atlases[a].height >> level);
    }
  }
  for (int n = 0; n < data->CmdListsCount; ++n)
  {
    const ImDrawList* dl = data->CmdLists[n];
    std::vector<float> vtx;
    for (const ImDrawVert& v : dl->VtxBuffer)
    {
      u32 col = v.col;
      float c;
      std::memcpy(&c, &col, 4);
      vtx.insert(vtx.end(), {v.pos.x, v.pos.y, v.uv.x, v.uv.y, c});
    }
    std::vector<u32> idx(dl->IdxBuffer.begin(), dl->IdxBuffer.end());
    write(fmt::format("{}/list-{}.vtx", out, n), vtx.data(), vtx.size() * 4);
    write(fmt::format("{}/list-{}.idx", out, n), idx.data(), idx.size() * 4);
    for (const ImDrawCmd& cmd : dl->CmdBuffer)
    {
      if (cmd.UserCallback || cmd.ElemCount == 0)
        continue;
      index += fmt::format("cmd {} {} {} {} {}\n", n, static_cast<u64>(cmd.GetTexID()),
                           cmd.IdxOffset, cmd.VtxOffset, cmd.ElemCount);
    }
  }
  write(out + "/index.txt", index.data(), index.size());
  ImGui::DestroyContext(context);
  Kit::UseAtlasForTests(Kit::Face::Body, nullptr, 0);
  Kit::UseAtlasForTests(Kit::Face::Heavy, nullptr, 0);
}
