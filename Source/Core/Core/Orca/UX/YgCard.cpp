// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/YgCard.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

namespace Orca::UX::Yg
{
namespace
{
ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// The YouGame site's dark card colours.
constexpr std::array<int, 3> CARD{15, 23, 42};
constexpr std::array<int, 3> EDGE{148, 163, 184};
constexpr std::array<int, 3> INK{248, 250, 252};
constexpr std::array<int, 3> INK2{203, 213, 225};
constexpr std::array<int, 3> ONLINE{74, 222, 128};

ImU32 Of(const std::array<int, 3>& c, float a)
{
  return Rgba(c[0], c[1], c[2], a);
}

constexpr const char* FONT_NAME = "Orca YouGame Roboto";

// Roboto, added to ImGui's atlas on first use, or ImGui's default font if it can't load. The video
// backend can replace the atlas, so the font is looked up by name each time and re-added if gone.
// A missing or broken file is tried once. Video thread only.
ImFont* Face()
{
  static bool s_unusable = false;
  ImGuiIO& io = ImGui::GetIO();
  ImFontAtlas* atlas = io.Fonts;
  for (ImFont* font : atlas->Fonts)
  {
    if (font && std::string_view(font->GetDebugName()) == FONT_NAME)
      return font;
  }
  if (!s_unusable && (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures))
  {
    const std::string path = File::GetSysDirectory() + "Orca/Fonts/Roboto-Medium.ttf";
    ImFont* font = nullptr;
    if (File::Exists(path))
    {
      ImFontConfig config;
      config.Flags |= ImFontFlags_NoLoadError;
      std::snprintf(config.Name, sizeof(config.Name), "%s", FONT_NAME);
      font = atlas->AddFontFromFileTTF(path.c_str(), 16.0f, &config);
    }
    s_unusable = font == nullptr;
    if (font)
      return font;
  }
  return ImGui::GetFont();
}

ImVec2 Measure(std::string_view text, float px)
{
  return Face()->CalcTextSizeA(std::max(1.0f, px), FLT_MAX, 0, text.data(),
                               text.data() + text.size());
}

void Put(ImDrawList* dl, ImVec2 pos, float px, ImU32 colour, std::string_view text)
{
  if (!text.empty())
  {
    dl->AddText(Face(), std::max(1.0f, px), ImVec2(std::floor(pos.x + 0.5f), std::floor(pos.y + 0.5f)),
                colour, text.data(), text.data() + text.size());
  }
}

// Card background: soft shadow, slate fill, hairline edge.
void Slab(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, float alpha)
{
  const float r = 1.9f * unit;
  dl->AddRectFilled(ImVec2(min.x, min.y + 0.6f * unit), ImVec2(max.x, max.y + 1.0f * unit),
                    Rgba(0, 0, 0, 0.3f * alpha), r);
  dl->AddRectFilled(min, max, Of(CARD, 0.93f * alpha), r);
  dl->AddRect(min, max, Of(EDGE, 0.28f * alpha), r, 0, std::max(1.0f, 0.14f * unit));
}

// Case mapping for ASCII and Latin-1 (U+00C0..U+00DE except U+00D7), matching JavaScript's.
char16_t Lower(char16_t c)
{
  if ((c >= u'A' && c <= u'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
    return static_cast<char16_t>(c + 0x20);
  return c;
}
char16_t Upper(char16_t c)
{
  if ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
    return static_cast<char16_t>(c - 0x20);
  return c;
}

// The site's avatar initial: the trimmed name's first character, uppercased, or "?".
std::string Initial(std::string_view name)
{
  const std::u16string wide = UTF8ToUTF16(name);
  for (std::size_t i = 0; i < wide.size(); ++i)
  {
    const char16_t c = wide[i];
    if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r')
      continue;
    // Keep a surrogate pair whole, so an emoji's initial is the emoji.
    const bool pair = c >= 0xD800 && c <= 0xDBFF && i + 1 < wide.size();
    return UTF16ToUTF8(pair ? wide.substr(i, 2) : std::u16string(1, Upper(c)));
  }
  return "?";
}
}  // namespace

void Warm()
{
  Face();
}

ImU32 AvatarColour(std::string_view name)
{
  static constexpr std::array<std::array<int, 3>, 7> COLOURS{{{124, 58, 237},
                                                              {15, 118, 110},
                                                              {190, 24, 93},
                                                              {29, 78, 216},
                                                              {180, 83, 9},
                                                              {220, 38, 38},
                                                              {14, 116, 144}}};
  // FNV-1a over UTF-16 code units, mod 2^32 like the site's Math.imul. Names the app truncated to 24
  // characters may hash differently from the site.
  u32 h = 2166136261u;
  for (const char16_t c : UTF8ToUTF16(name))
  {
    h ^= static_cast<u32>(Lower(c));
    h *= 16777619u;
  }
  return Of(COLOURS[h % COLOURS.size()], 1);
}

float TextWidth(std::string_view text, float px)
{
  return Measure(text, px).x;
}

std::vector<std::string> Wrap(std::string_view text, float px, float width)
{
  // ImGui's word wrap, which also breaks words wider than the line, so every line fits.
  std::vector<std::string> lines;
  const char* at = text.data();
  const char* const end = text.data() + text.size();
  ImFont* font = Face();
  const float size = std::max(1.0f, px);
  while (at < end)
  {
    while (at < end && *at == ' ')
      ++at;
    if (at == end)
      break;
    const char* stop = font->CalcWordWrapPosition(size, at, end, std::max(1.0f, width));
    if (stop <= at)
      stop = at + 1;  // always move on
    std::string_view line(at, static_cast<std::size_t>(stop - at));
    while (!line.empty() && line.back() == ' ')
      line.remove_suffix(1);
    if (!line.empty())
      lines.emplace_back(line);
    at = stop;
  }
  return lines;
}

void Logo(ImDrawList* dl, ImVec2 pos, float size, float alpha)
{
  // The site icon on a 28-unit grid: red tile, white pill, red cross and two red buttons.
  const float k = size / 28;
  const auto at = [&](float x, float y) { return ImVec2(pos.x + x * k, pos.y + y * k); };
  const ImU32 red = Rgba(255, 0, 0, alpha), white = Rgba(255, 255, 255, alpha);
  dl->AddRectFilled(at(0, 0), at(28, 28), red, 7 * k);
  dl->AddRectFilled(at(5, 9.5f), at(23, 18.5f), white, 4.5f * k);
  dl->AddRectFilled(at(9.25f, 11.5f), at(10.75f, 16.5f), red, 0.6f * k);
  dl->AddRectFilled(at(7.5f, 13.25f), at(12.5f, 14.75f), red, 0.6f * k);
  dl->AddCircleFilled(at(17.4f, 12.7f), 1.1f * k, red, 12);
  dl->AddCircleFilled(at(19.8f, 15.3f), 1.1f * k, red, 12);
}

Kit::Box ChatCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view name,
                  std::string_view text, float width, float alpha)
{
  const float pad = 1.6f * unit, avatar = 2.0f * unit, px = 1.95f * unit, mark = 1.9f * unit;
  const float text_x = pad + 2 * avatar + 1.2f * unit;
  const float room = std::max(10.0f * unit, width - text_x - pad);
  const std::vector<std::string> lines = Wrap(text, px, room);
  float text_w = Measure(name, px).x + mark + 1.6f * unit;
  for (const std::string& l : lines)
    text_w = std::max(text_w, Measure(l, px).x);
  const float line_h = px * 1.28f;
  const float h = pad + line_h * static_cast<float>(1 + std::max<std::size_t>(lines.size(), 1)) +
                  0.9f * unit;
  const float w = std::min(width, text_x + text_w + pad);
  const Kit::Box box{pos, ImVec2(pos.x + w, pos.y + std::max(h, 2 * avatar + 2 * pad))};
  if (alpha <= 0)
    return box;
  Slab(dl, box.min, box.max, unit, alpha);
  // Avatar with a green online dot.
  const ImVec2 c(pos.x + pad + avatar, pos.y + pad + avatar);
  dl->AddCircleFilled(c, avatar,
                      (AvatarColour(name) & ~IM_COL32_A_MASK) |
                          (static_cast<ImU32>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255))
                           << IM_COL32_A_SHIFT),
                      32);
  const std::string initial = Initial(name);
  const float ipx = 1.1f * avatar;
  const ImVec2 is = Measure(initial, ipx);
  Put(dl, ImVec2(c.x - is.x / 2, c.y - is.y / 2), ipx, Rgba(255, 255, 255, alpha), initial);
  const ImVec2 dot(c.x + avatar * 0.72f, c.y + avatar * 0.72f);
  dl->AddCircleFilled(dot, avatar * 0.34f, Of(CARD, alpha), 16);
  dl->AddCircleFilled(dot, avatar * 0.24f, Of(ONLINE, alpha), 16);
  // Name, logo at the right end, message below.
  const float tx = pos.x + text_x;
  float y = pos.y + pad - 0.15f * unit;
  Put(dl, ImVec2(tx, y), px, Of(INK, alpha), name);
  Logo(dl, ImVec2(box.max.x - pad - mark, y + (px - mark) / 2), mark, alpha);
  for (const std::string& l : lines)
  {
    y += line_h;
    Put(dl, ImVec2(tx, y), px, Of(INK2, alpha), l);
  }
  return box;
}

Kit::Box NoticeCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float width,
                    float alpha)
{
  const float pad = 1.4f * unit, mark = 2.6f * unit, px = 1.95f * unit;
  const float text_x = pad + mark + 1.1f * unit;
  const std::vector<std::string> lines = Wrap(text, px, std::max(10.0f * unit, width - text_x - pad));
  float text_w = 0;
  for (const std::string& l : lines)
    text_w = std::max(text_w, Measure(l, px).x);
  const float line_h = px * 1.28f;
  const float h = std::max(mark, line_h * static_cast<float>(std::max<std::size_t>(lines.size(), 1))) +
                  2 * pad;
  const Kit::Box box{pos, ImVec2(pos.x + std::min(width, text_x + text_w + pad), pos.y + h)};
  if (alpha <= 0)
    return box;
  Slab(dl, box.min, box.max, unit, alpha);
  Logo(dl, ImVec2(pos.x + pad, pos.y + (h - mark) / 2), mark, alpha);
  float y = pos.y + (h - line_h * static_cast<float>(lines.size())) / 2 + 0.1f * unit;
  for (const std::string& l : lines)
  {
    Put(dl, ImVec2(pos.x + text_x, y), px, Of(INK2, alpha), l);
    y += line_h;
  }
  return box;
}

void RoundLogo(ImDrawList* dl, ImVec2 pos, float size, float alpha)
{
  // On a 40-unit grid: red disc, white pill, red cross and two red buttons.
  const float k = size / 40;
  const auto at = [&](float x, float y) { return ImVec2(pos.x + x * k, pos.y + y * k); };
  const ImU32 red = Rgba(255, 0, 0, alpha), white = Rgba(255, 255, 255, alpha);
  dl->AddCircleFilled(at(20, 20), 20 * k, red, 48);
  dl->AddRectFilled(at(8, 14), at(32, 26), white, 6 * k);
  dl->AddRectFilled(at(13.67f, 16.67f), at(15.67f, 23.34f), red, 0.8f * k);
  dl->AddRectFilled(at(11.33f, 19), at(18, 21), red, 0.8f * k);
  dl->AddCircleFilled(at(24.5f, 18.3f), 1.47f * k, red, 12);
  dl->AddCircleFilled(at(27.7f, 21.7f), 1.47f * k, red, 12);
}

void Text(ImDrawList* dl, ImVec2 pos, float px, ImU32 colour, std::string_view text)
{
  Put(dl, pos, px, colour, text);
}

void CardSlab(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, float alpha)
{
  Slab(dl, min, max, unit, alpha);
}

ImU32 Ink(float alpha)
{
  return Of(INK, alpha);
}
ImU32 Ink2(float alpha)
{
  return Of(INK2, alpha);
}
ImU32 Edge(float alpha)
{
  return Of(EDGE, alpha);
}

void Avatar(ImDrawList* dl, ImVec2 centre, float r, std::string_view name, float alpha)
{
  const ImU32 a = static_cast<ImU32>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255));
  dl->AddCircleFilled(centre, r, (AvatarColour(name) & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT),
                      32);
  const std::string initial = Initial(name);
  const float ipx = 1.1f * r;
  const ImVec2 is = Measure(initial, ipx);
  Put(dl, ImVec2(centre.x - is.x / 2, centre.y - is.y / 2), ipx, Rgba(255, 255, 255, alpha),
      initial);
}

std::string InitialOf(std::string_view name)
{
  return Initial(name);
}

void AvatarOf(ImDrawList* dl, ImVec2 centre, float r, ImU32 colour, std::string_view initial,
              float alpha)
{
  const ImU32 a = static_cast<ImU32>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255));
  dl->AddCircleFilled(centre, r, (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT), 32);
  const float ipx = 1.1f * r;
  const ImVec2 is = Measure(initial, ipx);
  Put(dl, ImVec2(centre.x - is.x / 2, centre.y - is.y / 2), ipx, Rgba(255, 255, 255, alpha),
      initial);
}
}  // namespace Orca::UX::Yg
