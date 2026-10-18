// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Kit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/UX/GameFont.h"
#include "Core/Orca/UX/Overlay.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"

namespace Orca::UX::Kit
{
namespace
{
constexpr int FACES = 2;
// Font files on the disc (Project+ uses Brawl's unchanged).
constexpr std::array<const char*, FACES> FONT_FILES = {
    "system/font/font_latin1.arc",  // Body: rounded sans, LZ77-compressed
    "system/font/font_hira.brfnt",  // Heavy: display face
};
constexpr const char* GAME_ID = "RSBE01";
constexpr int ATLAS_LEVELS = 4;

std::mutex s_lock;
std::string s_disc;
bool s_started = false;
std::thread s_reader;
std::atomic<bool> s_read{false};
std::array<std::optional<GameFont::Atlas>, FACES> s_read_atlases;  // under s_lock

// Video thread only: each face's atlas on the GPU, freed by ReleaseGpu as the backend shuts down.
// A texture still here from a backend that went without it is leaked, since its device is gone.
struct GpuFace
{
  const AbstractGfx* gfx = nullptr;
  bool tried = false;
  std::unique_ptr<AbstractTexture> texture;
  GameFont::Atlas atlas;  // glyphs and metrics only, no pixels
};
std::array<GpuFace, FACES> s_gpu;
std::atomic<int> s_faces_ready{0};  // bit per face
std::atomic<Look> s_look{Look::Brawl};

void ReadFonts(std::string path)
{
  std::array<std::optional<GameFont::Atlas>, FACES> atlases;
  std::string error;
  std::vector<std::optional<GameFont::Rfnt>> fonts =
      GameFont::ReadFromDisc(path, FONT_FILES, GAME_ID, &error);
  if (!error.empty())
    WARN_LOG_FMT(ROLLBACK, "Orca kit: the game's fonts: {}", error);
  const std::vector<char32_t> codes = GameFont::OverlayCodePoints();
  for (int face = 0; face < FACES; ++face)
  {
    if (!fonts[static_cast<std::size_t>(face)])
      continue;
    atlases[face] = GameFont::BuildAtlas(*fonts[static_cast<std::size_t>(face)], codes, 4,
                                         ATLAS_LEVELS);
    NOTICE_LOG_FMT(ROLLBACK, "Orca kit: {} read, {} glyphs, {}x{}", FONT_FILES[face],
                   atlases[face]->glyphs.size(), atlases[face]->width, atlases[face]->height);
  }
  {
    std::lock_guard lk(s_lock);
    s_read_atlases = std::move(atlases);
  }
  s_read = true;
}

struct FaceRef
{
  const GameFont::Atlas* atlas;
  ImTextureRef texture;
};

std::array<std::pair<const GameFont::Atlas*, ImTextureID>, FACES> s_test_faces{};

std::optional<FaceRef> GameFace(Face face)
{
  if (const auto& [atlas, id] = s_test_faces[static_cast<int>(face)]; atlas)
    return FaceRef{atlas, ImTextureRef(id)};
  const GpuFace& g = s_gpu[static_cast<int>(face)];
  if (!g.texture || !g_gfx || g.gfx != g_gfx.get())
    return std::nullopt;
  return FaceRef{&g.atlas, *g.texture};
}

// ImGui uses a point sampler, but the game's glyphs are drawn downscaled, so switch to trilinear
// for them and restore ImGui's sampler afterwards.
void LinearSampler(const ImDrawList*, const ImDrawCmd*)
{
  if (g_gfx)
    g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
}
void PointSampler(const ImDrawList*, const ImDrawCmd*)
{
  if (g_gfx)
    g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
}

ImU32 Rgb(int r, int g, int b, float a = 1)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// `c` with its alpha scaled by `alpha`.
ImU32 Fade(ImU32 c, float alpha)
{
  const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
  return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::lround(a)) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  ImU32 out = 0;
  for (int shift = 0; shift < 32; shift += 8)
  {
    const float x = static_cast<float>((a >> shift) & 0xFF);
    const float y = static_cast<float>((b >> shift) & 0xFF);
    out |= static_cast<ImU32>(std::lround(x + (y - x) * t)) << shift;
  }
  return out;
}

// Applies a vertical gradient to vertices from `first` on, keeping each vertex's alpha as a factor
// so ImGui's anti-aliased edges still fade out.
void Recolour(ImDrawList* dl, int first, float y0, float y1, ImU32 top, ImU32 bottom)
{
  const float span = std::max(1e-3f, y1 - y0);
  for (int i = first; i < dl->VtxBuffer.Size; ++i)
  {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float own = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    v.col = Fade(Mix(top, bottom, (v.pos.y - y0) / span), own);
  }
}

// How a tone is painted, matching the game's own buttons: a rim lit from above, a dark inner line,
// a gradient body, a gloss over the top half, and the text colour.
struct Paint
{
  ImU32 top, bottom;          // body gradient
  ImU32 rim_top, rim_bottom;  // rim gradient
  ImU32 line;                 // between rim and body
  ImU32 ink, ink_bottom;      // text gradient
  ImU32 ink_outline;          // zero alpha = no outline
  float gloss = 0.3f;         // gloss alpha at the top
};

Paint PaintOf(Tone tone)
{
  const ImU32 rim_top = Rgb(255, 255, 255), rim_bottom = Rgb(186, 190, 202);
  const ImU32 white = Rgb(255, 255, 255), white_bottom = Rgb(236, 236, 240);
  switch (tone)
  {
  case Tone::Gold:
    return {Rgb(255, 236, 96), Rgb(244, 164, 0), rim_top, rim_bottom, Rgb(44, 26, 0),
            Rgb(24, 16, 0), Rgb(24, 16, 0), Rgb(0, 0, 0, 0)};
  case Tone::Yellow:
    return {Rgb(255, 226, 70), Rgb(226, 150, 0), rim_top, rim_bottom, Rgb(44, 26, 0),
            Rgb(24, 16, 0), Rgb(24, 16, 0), Rgb(0, 0, 0, 0)};
  case Tone::Grey:
    return {Rgb(240, 240, 244), Rgb(196, 198, 206), rim_top, Rgb(206, 208, 216),
            Rgb(150, 152, 160), Rgb(138, 140, 150), Rgb(138, 140, 150), Rgb(255, 255, 255, 0.9f)};
  case Tone::Red:
    return {Rgb(255, 112, 98), Rgb(204, 18, 22), rim_top, rim_bottom, Rgb(36, 0, 0), white,
            white_bottom, Rgb(90, 0, 0)};
  case Tone::Blue:
    return {Rgb(104, 176, 255), Rgb(18, 72, 214), rim_top, rim_bottom, Rgb(0, 8, 40), white,
            white_bottom, Rgb(0, 20, 90)};
  case Tone::Green:
    return {Rgb(118, 232, 112), Rgb(22, 146, 40), rim_top, rim_bottom, Rgb(0, 30, 4), white,
            white_bottom, Rgb(0, 60, 10)};
  case Tone::Panel:
  default:
    if (CurrentLook() == Look::ProjectPlus)
    {
      // Project+: flat teal, black edge, white text.
      return {Rgb(28, 174, 128, 0.97f), Rgb(8, 122, 88, 0.97f), Rgb(22, 30, 30),
              Rgb(4, 8, 8), Rgb(0, 0, 0), white, Rgb(222, 244, 236), Rgb(0, 30, 22), 0.1f};
    }
    // Brawl: rose, white rim, black line, cream text.
    return {Rgb(182, 104, 110, 0.96f), Rgb(120, 56, 62, 0.96f), Rgb(252, 250, 250),
            Rgb(194, 188, 188), Rgb(18, 8, 8), Rgb(255, 252, 240), Rgb(246, 224, 168),
            Rgb(30, 10, 10)};
  }
}

// Fallback to ImGui's font until the game's font is loaded.
ImVec2 FallbackMeasure(const TextLook& look, std::string_view text)
{
  return ImGui::GetFont()->CalcTextSizeA(std::max(1.0f, look.px), FLT_MAX, 0, text.data(),
                                         text.data() + text.size());
}

// Minimum word gap as a fraction of line height; the heavy face's own space is too thin.
float MinSpace(Face face)
{
  return face == Face::Heavy ? 0.42f : 0.0f;
}

// Calls `emit(glyph, x)` for each glyph (x in unscaled font pixels), substituting missing ones.
// Returns the line's total advance.
template <typename Emit>
float LayOut(const GameFont::Atlas& atlas, float tracking_font_px, float min_space,
             std::string_view text, Emit&& emit)
{
  float x = 0;
  const auto put = [&](auto&& self, char32_t code, int depth) -> void {
    const auto it = atlas.glyphs.find(code);
    if (it != atlas.glyphs.end())
    {
      emit(it->second, x);
      const float advance =
          code == U' ' ? std::max(it->second.advance, atlas.cell_height * min_space) :
                         it->second.advance;
      x += advance + tracking_font_px;
      return;
    }
    const std::u32string sub = depth < 3 ? GameFont::Substitute(code) : std::u32string();
    if (sub.empty())
    {
      x += atlas.space + tracking_font_px;
      return;
    }
    for (const char32_t c : sub)
      self(self, c, depth + 1);
  };
  for (std::size_t i = 0; i < text.size();)
  {
    const char32_t code = GameFont::NextCodePoint(text, &i);
    if (code == U'\n' || code == U'\r')
      continue;
    put(put, code, 0);
  }
  return std::max(0.0f, x - tracking_font_px);
}

// Draws one pass of glyph quads with a `top`-to-`bottom` gradient.
void GlyphPass(ImDrawList* dl, const GameFont::Atlas& atlas, ImVec2 pos, float scale,
               const TextLook& look, std::string_view text, ImU32 top, ImU32 bottom)
{
  const float h = atlas.cell_height * scale;
  const float skew = look.italic * h;
  LayOut(atlas, look.tracking / scale, MinSpace(look.face), text,
         [&](const GameFont::Atlas::Glyph& g, float x) {
    if (g.width <= 0)
      return;
    const float x0 = pos.x + (x + g.left) * scale;
    const float x1 = x0 + g.width * scale;
    const float y0 = pos.y, y1 = pos.y + h;
    dl->PrimReserve(6, 4);
    const auto base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
    dl->PrimWriteVtx(ImVec2(x0 + skew, y0), ImVec2(g.u0, g.v0), top);
    dl->PrimWriteVtx(ImVec2(x1 + skew, y0), ImVec2(g.u1, g.v0), top);
    dl->PrimWriteVtx(ImVec2(x1, y1), ImVec2(g.u1, g.v1), bottom);
    dl->PrimWriteVtx(ImVec2(x0, y1), ImVec2(g.u0, g.v1), bottom);
  });
}

// Outline offsets: 8 directions, or 12 when thicker than 2 pixels.
template <typename F>
void AroundOutline(float outline, F&& f)
{
  const int n = outline > 2.0f ? 12 : 8;
  for (int k = 0; k < n; ++k)
  {
    const float a = 2 * std::numbers::pi_v<float> * static_cast<float>(k) / static_cast<float>(n);
    f(ImVec2(std::cos(a) * outline, std::sin(a) * outline));
  }
}

ImVec2 Add(ImVec2 a, ImVec2 b)
{
  return ImVec2(a.x + b.x, a.y + b.y);
}

void FillRounded(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, ImU32 top, ImU32 bottom,
                 ImDrawFlags flags = ImDrawFlags_None)
{
  if (max.x <= min.x || max.y <= min.y)
    return;
  const int first = dl->VtxBuffer.Size;
  dl->AddRectFilled(min, max, IM_COL32_WHITE, std::max(0.0f, radius), flags);
  Recolour(dl, first, min.y, max.y, top, bottom);
}

float Snap(float v)
{
  return std::floor(v + 0.5f);
}

// Final-seconds pulse: 1 at the start of each second, easing to 0 by mid-second.
float Pulse(double now_s)
{
  const float t = static_cast<float>(now_s - std::floor(now_s));
  const float k = std::max(0.0f, 1.0f - 2.0f * t);
  return k * k;
}
}  // namespace

Tone PortTone(int port)
{
  switch (port)
  {
  case 0:
    return Tone::Red;
  case 1:
    return Tone::Blue;
  case 2:
    return Tone::Yellow;
  default:
    return Tone::Green;
  }
}

TextLook LabelLook(float unit, float size)
{
  TextLook look;
  look.face = Face::Body;
  look.px = std::max(1.0f, size * unit);
  look.outline = std::max(1.0f, look.px * 0.08f);
  look.outline_colour = Rgb(10, 10, 16);
  look.shadow = ImVec2(0, std::max(1.0f, look.px * 0.06f));
  look.shadow_colour = IM_COL32(0, 0, 0, 110);
  return look;
}

TextLook TitleLook(float unit, float size)
{
  TextLook look;
  look.face = Face::Heavy;
  look.px = std::max(1.0f, size * unit);
  if (CurrentLook() == Look::ProjectPlus)
  {
    // Project+: white to mint.
    look.top = Rgb(240, 248, 248);
    look.bottom = Rgb(96, 226, 196);
    look.outline_colour = Rgb(0, 0, 0);
  }
  else
  {
    // Brawl: cream to gold.
    look.top = Rgb(255, 250, 226);
    look.bottom = Rgb(238, 186, 72);
    look.outline_colour = Rgb(22, 8, 8);
  }
  look.outline = std::max(1.0f, look.px * 0.085f);
  look.shadow = ImVec2(look.px * 0.03f, look.px * 0.07f);
  look.shadow_colour = IM_COL32(0, 0, 0, 120);
  look.italic = 0.1f;
  look.tracking = look.px * 0.01f;
  return look;
}

void SetLook(Look look)
{
  s_look.store(look, std::memory_order_relaxed);
}

Look CurrentLook()
{
  return s_look.load(std::memory_order_relaxed);
}

void SetGameDisc(std::string path)
{
  std::lock_guard lk(s_lock);
  s_disc = std::move(path);
}

void BeginFrame()
{
  {
    std::lock_guard lk(s_lock);
    if (!s_started && !s_disc.empty())
    {
      s_started = true;
      s_reader = std::thread(ReadFonts, s_disc);
    }
  }
  if (!s_read.load(std::memory_order_acquire) || !g_gfx)
    return;
  int ready = 0;
  for (int face = 0; face < FACES; ++face)
  {
    GpuFace& g = s_gpu[face];
    if (g.gfx != g_gfx.get())
    {
      (void)g.texture.release();  // leak the old backend's texture (see GpuFace)
      g.gfx = g_gfx.get();
      g.tried = false;
    }
    if (!g.tried)
    {
      g.tried = true;
      std::lock_guard lk(s_lock);
      const std::optional<GameFont::Atlas>& read = s_read_atlases[face];
      if (read && read->width > 0 && !read->levels.empty())
      {
        const u32 levels = static_cast<u32>(read->levels.size());
        const TextureConfig config(static_cast<u32>(read->width), static_cast<u32>(read->height),
                                   levels, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                   AbstractTextureType::Texture_2DArray);
        g.texture = g_gfx->CreateTexture(config, "Orca kit font");
        if (g.texture)
        {
          for (u32 level = 0; level < levels; ++level)
          {
            const u32 w = std::max(1u, config.width >> level);
            const u32 h = std::max(1u, config.height >> level);
            g.texture->Load(level, w, h, w, read->levels[level].data(), read->levels[level].size());
          }
          g.atlas = *read;
          g.atlas.levels.clear();
        }
      }
    }
    if (g.texture)
      ready |= 1 << face;
  }
  s_faces_ready = ready;
}

void ReleaseGpu()
{
  for (GpuFace& g : s_gpu)
  {
    g.texture.reset();
    g.gfx = nullptr;
    g.tried = false;
  }
  s_faces_ready = 0;
}

void Shutdown()
{
  std::thread reader;
  {
    std::lock_guard lk(s_lock);
    reader = std::move(s_reader);
  }
  if (reader.joinable())
    reader.join();
}

bool HasGameFont(Face face)
{
  return (s_faces_ready.load(std::memory_order_relaxed) >> static_cast<int>(face)) & 1;
}

ImVec2 Measure(const TextLook& look, std::string_view text)
{
  if (const std::optional<FaceRef> face = GameFace(look.face))
  {
    const float scale = look.px / face->atlas->cell_height;
    const float w = LayOut(*face->atlas, look.tracking / scale, MinSpace(look.face), text,
                           [](const GameFont::Atlas::Glyph&, float) {});
    return ImVec2(w * scale + look.italic * look.px * 0.5f + 2 * look.outline, look.px);
  }
  const ImVec2 s = FallbackMeasure(look, text);
  return ImVec2(s.x + 2 * look.outline, s.y);
}

std::vector<std::string> Wrap(const TextLook& look, std::string_view text, float width)
{
  std::vector<std::string> lines;
  std::string line;
  std::size_t at = 0;
  while (at < text.size())
  {
    const std::size_t space = text.find(' ', at);
    const std::string_view word =
        text.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at);
    at = space == std::string_view::npos ? text.size() : space + 1;
    if (word.empty())
      continue;
    std::string longer = line.empty() ? std::string(word) : line + " " + std::string(word);
    if (!line.empty() && Measure(look, longer).x > width)
    {
      lines.push_back(std::move(line));
      line = std::string(word);
    }
    else
    {
      line = std::move(longer);
    }
  }
  if (!line.empty())
    lines.push_back(std::move(line));
  return lines;
}

ImVec2 Text(ImDrawList* dl, ImVec2 pos, const TextLook& look, std::string_view text, float alpha,
            Align align)
{
  const ImVec2 size = Measure(look, text);
  if (text.empty() || alpha <= 0)
    return size;
  if (align == Align::Centre)
    pos.x -= size.x / 2;
  else if (align == Align::Right)
    pos.x -= size.x;
  pos = ImVec2(Snap(pos.x + look.outline), Snap(pos.y));
  // Overlapping passes stack, so each of n outline or shadow passes uses the alpha whose n-fold
  // stack equals the requested one.
  const int passes = look.outline > 2.0f ? 12 : 8;
  const auto per_pass = [passes](float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return a >= 1.0f ? 1.0f : 1.0f - std::pow(1.0f - a, 1.0f / static_cast<float>(passes));
  };
  const float shadow_a = static_cast<float>((look.shadow_colour >> IM_COL32_A_SHIFT) & 0xFF) / 255;
  const float outline_a = static_cast<float>((look.outline_colour >> IM_COL32_A_SHIFT) & 0xFF) / 255;
  const ImU32 shadow = Fade(look.shadow_colour, look.outline > 0 ?
                                                    per_pass(shadow_a * alpha) / std::max(1e-3f, shadow_a) :
                                                    alpha);
  const ImU32 outline = Fade(look.outline_colour, per_pass(outline_a * alpha) / std::max(1e-3f, outline_a));
  const ImU32 top = Fade(look.top, alpha), bottom = Fade(look.bottom, alpha);
  const bool has_shadow = (look.shadow.x != 0 || look.shadow.y != 0) && shadow_a > 0;
  if (const std::optional<FaceRef> face = GameFace(look.face))
  {
    const float scale = look.px / face->atlas->cell_height;
    dl->AddCallback(&LinearSampler, nullptr);
    dl->PushTexture(face->texture);
    if (has_shadow)
    {
      const ImVec2 at = Add(pos, look.shadow);
      if (look.outline > 0)
      {
        AroundOutline(look.outline, [&](ImVec2 d) {
          GlyphPass(dl, *face->atlas, Add(at, d), scale, look, text, shadow, shadow);
        });
      }
      else
      {
        GlyphPass(dl, *face->atlas, at, scale, look, text, shadow, shadow);
      }
    }
    if (look.outline > 0)
    {
      AroundOutline(look.outline, [&](ImVec2 d) {
        GlyphPass(dl, *face->atlas, Add(pos, d), scale, look, text, outline, outline);
      });
    }
    GlyphPass(dl, *face->atlas, pos, scale, look, text, top, bottom);
    dl->PopTexture();
    dl->AddCallback(&PointSampler, nullptr);
    return size;
  }
  ImFont* font = ImGui::GetFont();
  const float px = std::max(1.0f, look.px);
  const char* b = text.data();
  const char* e = text.data() + text.size();
  if (has_shadow && look.outline > 0)
  {
    const ImVec2 at = Add(pos, look.shadow);
    AroundOutline(look.outline,
                  [&](ImVec2 d) { dl->AddText(font, px, Add(at, d), shadow, b, e); });
  }
  else if (has_shadow)
  {
    dl->AddText(font, px, Add(pos, look.shadow), shadow, b, e);
  }
  if (look.outline > 0)
  {
    AroundOutline(look.outline,
                  [&](ImVec2 d) { dl->AddText(font, px, Add(pos, d), outline, b, e); });
  }
  const int first = dl->VtxBuffer.Size;
  dl->AddText(font, px, pos, IM_COL32_WHITE, b, e);
  Recolour(dl, first, pos.y, pos.y + size.y, top, bottom);
  return size;
}

namespace
{
// A game-style button: soft shadow, lit rim, dark inner line, gradient body, top gloss.
void Slab(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, float unit, const Paint& p,
          float alpha)
{
  const float h = max.y - min.y;
  const float rim = std::clamp(h * 0.11f, 1.0f, 0.55f * unit);
  const float line = std::clamp(h * 0.05f, 1.0f, 0.3f * unit);
  dl->AddRectFilled(ImVec2(min.x + 0.1f * unit, min.y + 0.35f * unit),
                    ImVec2(max.x + 0.35f * unit, max.y + 0.55f * unit), Rgb(0, 0, 0, 0.34f * alpha),
                    radius + 0.2f * unit);
  FillRounded(dl, min, max, radius, Fade(p.rim_top, alpha), Fade(p.rim_bottom, alpha));
  const ImVec2 lmin(min.x + rim, min.y + rim), lmax(max.x - rim, max.y - rim);
  FillRounded(dl, lmin, lmax, std::max(0.0f, radius - rim), Fade(p.line, alpha),
              Fade(p.line, alpha));
  const ImVec2 bmin(lmin.x + line, lmin.y + line), bmax(lmax.x - line, lmax.y - line);
  const float r = std::max(0.0f, radius - rim - line);
  FillRounded(dl, bmin, bmax, r, Fade(p.top, alpha), Fade(p.bottom, alpha));
  if (p.gloss > 0)
  {
    FillRounded(dl, bmin, ImVec2(bmax.x, bmin.y + (bmax.y - bmin.y) * 0.5f), r,
                Rgb(255, 255, 255, p.gloss * alpha), Rgb(255, 255, 255, 0.13f * p.gloss * alpha),
                ImDrawFlags_RoundCornersTop);
  }
}

// Text colour for a tone: light text outlined in the tone's dark shade (black on gold and yellow,
// grey with a white outline on grey).
TextLook Inked(TextLook look, const Paint& p)
{
  look.top = p.ink;
  look.bottom = p.ink_bottom;
  look.outline_colour = p.ink_outline;
  if (((p.ink_outline >> IM_COL32_A_SHIFT) & 0xFF) == 0)
  {
    look.outline = 0;
    look.shadow = ImVec2(0, 0);
  }
  return look;
}
}  // namespace

void Plate(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha)
{
  if (!(max.x > min.x && max.y > min.y) || alpha <= 0)
    return;
  const float radius = std::clamp(std::min(max.y - min.y, max.x - min.x) * 0.3f, 0.5f * unit,
                                  2.2f * unit);
  Slab(dl, min, max, radius, unit, PaintOf(tone), alpha);
}

namespace
{
// Height of each extra note line, as a fraction of the first row.
constexpr float BANNER_NOTE_ROW = 0.3f;

// Short titles use the heavy face; sentences use the body face.
bool HeavyTitle(std::string_view title)
{
  std::size_t letters = 0;
  for (std::size_t i = 0; i < title.size();)
  {
    GameFont::NextCodePoint(title, &i);
    ++letters;
  }
  return letters <= 18;
}

// Text styles for a Banner's title and note.
std::pair<TextLook, TextLook> BannerLooks(float height, float unit, bool heavy, bool has_note,
                                          const Paint& p)
{
  if (!has_note)
  {
    return {heavy ? TitleLook(unit, height * 0.6f / unit) :
                    Inked(LabelLook(unit, height * 0.5f / unit), p),
            TextLook{}};
  }
  return {heavy ? TitleLook(unit, height * 0.48f / unit) :
                  Inked(LabelLook(unit, height * 0.44f / unit), p),
          Inked(LabelLook(unit, height * BANNER_NOTE_ROW / unit), p)};
}

// Splits a note on '\n'.
std::vector<std::string_view> NoteLines(std::string_view note)
{
  std::vector<std::string_view> lines;
  while (!note.empty())
  {
    const std::size_t end = note.find('\n');
    lines.push_back(note.substr(0, end));
    if (end == std::string_view::npos)
      break;
    note.remove_prefix(end + 1);
  }
  return lines;
}

// Height of the first row (title plus first note line).
float RowHeight(float height, std::size_t lines)
{
  return lines > 1 ? height / (1 + BANNER_NOTE_ROW * static_cast<float>(lines - 1)) : height;
}
}  // namespace

float BannerHeight(float row, std::size_t note_lines)
{
  return note_lines > 1 ? row * (1 + BANNER_NOTE_ROW * static_cast<float>(note_lines - 1)) : row;
}

float BannerWidth(float height, float unit, std::string_view title, std::string_view note)
{
  const std::vector<std::string_view> lines = NoteLines(note);
  const float row = RowHeight(height, lines.size());
  const auto [look, small] =
      BannerLooks(row, unit, HeavyTitle(title), !note.empty(), PaintOf(Tone::Panel));
  float w = Measure(look, title).x;
  for (const std::string_view line : lines)
    w = std::max(w, Measure(small, line).x);
  return w + height * 0.42f + 4.0f * unit;
}

Box Banner(ImDrawList* dl, float left, float right, float top, float height, float unit, Tone tone,
           std::string_view title, std::string_view note, float alpha)
{
  const Box box{ImVec2(left, top), ImVec2(right, top + height)};
  if (!(right > left && height > 0) || alpha <= 0)
    return box;
  const Paint p = PaintOf(tone);
  const float cut = height * 0.42f;
  const float bottom = top + height;
  // A slanted band with parallel cut ends, layered like the buttons.
  const auto band = [&](float grow_x, float grow_y, ImVec2 d) {
    const float k = cut / height;
    return std::array<ImVec2, 4>{
        ImVec2(left + cut - grow_x + k * grow_y + d.x, top - grow_y + d.y),
        ImVec2(right + grow_x + k * grow_y + d.x, top - grow_y + d.y),
        ImVec2(right - cut + grow_x - k * grow_y + d.x, bottom + grow_y + d.y),
        ImVec2(left - grow_x - k * grow_y + d.x, bottom + grow_y + d.y)};
  };
  const float rim = std::clamp(height * 0.08f, 1.0f, 0.55f * unit);
  const float line = std::clamp(height * 0.04f, 1.0f, 0.3f * unit);
  const auto fill = [&](const std::array<ImVec2, 4>& q, float y0, float y1, ImU32 c0, ImU32 c1) {
    const int first = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(q.data(), 4, IM_COL32_WHITE);
    Recolour(dl, first, y0, y1, c0, c1);
  };
  fill(band(0, 0, ImVec2(0.35f * unit, 0.6f * unit)), top, bottom, Rgb(0, 0, 0, 0.36f * alpha),
       Rgb(0, 0, 0, 0.36f * alpha));
  fill(band(0, 0, ImVec2(0, 0)), top, bottom, Fade(p.rim_top, alpha), Fade(p.rim_bottom, alpha));
  fill(band(-rim, -rim, ImVec2(0, 0)), top, bottom, Fade(p.line, alpha), Fade(p.line, alpha));
  const std::array<ImVec2, 4> body = band(-rim - line, -rim - line, ImVec2(0, 0));
  fill(body, body[0].y, body[3].y, Fade(p.top, alpha), Fade(p.bottom, alpha));
  const float bh = body[3].y - body[0].y;
  const float k = cut / height;
  const std::array<ImVec2, 4> shine = {body[0], body[1],
                                       ImVec2(body[1].x - k * bh * 0.48f, body[0].y + bh * 0.48f),
                                       ImVec2(body[0].x - k * bh * 0.48f, body[0].y + bh * 0.48f)};
  if (p.gloss > 0)
  {
    fill(shine, body[0].y, body[0].y + bh * 0.48f, Rgb(255, 255, 255, 0.87f * p.gloss * alpha),
         Rgb(255, 255, 255, 0.1f * p.gloss * alpha));
  }
  // Title and note, centred.
  const float centre = (left + right) / 2;
  // Body text is used for sentences, since it stays readable at that size.
  const std::vector<std::string_view> lines = NoteLines(note);
  const float row = RowHeight(height, lines.size());
  const auto [look, small] = BannerLooks(row, unit, HeavyTitle(title), !note.empty(), p);
  if (note.empty())
  {
    Text(dl, ImVec2(centre, top + (height - look.px) / 2 + look.px * 0.05f), look, title, alpha,
         Align::Centre);
  }
  else
  {
    float y = top + row * 0.1f;
    Text(dl, ImVec2(centre, y), look, title, alpha, Align::Centre);
    y += look.px * 0.95f;
    for (const std::string_view note_line : lines)
    {
      Text(dl, ImVec2(centre, y), small, note_line, alpha, Align::Centre);
      y += small.px;
    }
  }
  return box;
}

Box Title(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size, Align align,
          float alpha)
{
  const TextLook look = TitleLook(unit, size);
  const ImVec2 s = Text(dl, pos, look, text, alpha, align);
  const float x = align == Align::Centre ? pos.x - s.x / 2 : align == Align::Right ? pos.x - s.x : pos.x;
  return {ImVec2(x, pos.y), ImVec2(x + s.x, pos.y + s.y)};
}

Box Label(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size, Align align,
          ImU32 colour, float alpha)
{
  TextLook look = LabelLook(unit, size);
  look.top = colour;
  look.bottom = Mix(colour, Rgb(0, 0, 0, static_cast<float>((colour >> 24) & 0xFF) / 255), 0.12f);
  const ImVec2 s = Text(dl, pos, look, text, alpha, align);
  const float x = align == Align::Centre ? pos.x - s.x / 2 : align == Align::Right ? pos.x - s.x : pos.x;
  return {ImVec2(x, pos.y), ImVec2(x + s.x, pos.y + s.y)};
}

Box LabelPlate(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size,
               Align align, Tone tone, float alpha)
{
  const Paint p = PaintOf(tone);
  const TextLook look = Inked(LabelLook(unit, size), p);
  const ImVec2 s = Measure(look, text);
  const float pad_x = 1.4f * unit, pad_y = 0.55f * unit;
  const float w = s.x + 2 * pad_x, h = s.y + 2 * pad_y;
  const float x = align == Align::Centre ? pos.x - w / 2 : align == Align::Right ? pos.x - w : pos.x;
  const Box box{ImVec2(x, pos.y), ImVec2(x + w, pos.y + h)};
  Plate(dl, box.min, box.max, unit, tone, alpha);
  Text(dl, ImVec2(x + pad_x, pos.y + pad_y), look, text, alpha);
  return box;
}

Box Timer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s, float alpha)
{
  seconds = std::max(0, seconds);
  const std::string text = fmt::format("{}:{:02}", seconds / 60, seconds % 60);
  TextLook look = TitleLook(unit, 4.2f);
  look.italic = 0.08f;
  const bool urgent = seconds <= 5;
  const bool soon = seconds <= 10;
  // White normally, gold in the last 10 seconds, red in the last 5.
  look.top = Rgb(255, 255, 255);
  look.bottom = Rgb(214, 220, 232);
  if (urgent)
  {
    look.top = Rgb(255, 170, 160);
    look.bottom = Rgb(255, 40, 36);
  }
  else if (soon)
  {
    look.top = Rgb(255, 248, 180);
    look.bottom = Rgb(255, 196, 30);
  }
  // Size the plate for "0:00" so it never changes width.
  const ImVec2 widest = Measure(look, "0:00");
  const float pad_x = 1.3f * unit, pad_y = 0.45f * unit;
  const float w = widest.x + 2 * pad_x, h = widest.y + 2 * pad_y;
  const Box box{ImVec2(centre.x - w / 2, centre.y - h / 2), ImVec2(centre.x + w / 2, centre.y + h / 2)};
  Plate(dl, box.min, box.max, unit, Tone::Panel, alpha);
  if (urgent)
  {
    const float grow = 0.14f * Pulse(now_s);
    look.px *= 1 + grow;
    look.outline *= 1 + grow;
  }
  Text(dl, ImVec2(centre.x, centre.y - look.px / 2 + look.px * 0.04f), look, text, alpha,
       Align::Centre);
  return box;
}

namespace
{
// Badge text style for `tone`.
TextLook BadgeLook(float unit, float size, const Paint& p)
{
  TextLook look = TitleLook(unit, size);
  look.italic = 0.06f;
  look.outline = std::max(1.0f, look.px * 0.085f);
  look.shadow = ImVec2(0, std::max(1.0f, look.px * 0.05f));
  return Inked(look, p);
}
constexpr float BADGE_PAD_X = 1.5f, BADGE_PAD_Y = 0.55f;
}  // namespace

Box Badge(ImDrawList* dl, ImVec2 centre, float unit, std::string_view text, Tone tone, float size,
          float alpha)
{
  const Paint p = PaintOf(tone);
  const TextLook look = BadgeLook(unit, size, p);
  const ImVec2 s = Measure(look, text);
  const float pad_x = BADGE_PAD_X * unit, pad_y = BADGE_PAD_Y * unit;
  const ImVec2 min(centre.x - s.x / 2 - pad_x, centre.y - s.y / 2 - pad_y);
  const ImVec2 max(centre.x + s.x / 2 + pad_x, centre.y + s.y / 2 + pad_y);
  Slab(dl, min, max, (max.y - min.y) * 0.34f, unit, p, alpha);
  Text(dl, ImVec2(centre.x, centre.y - s.y / 2 + look.px * 0.03f), look, text, alpha,
       Align::Centre);
  return {min, max};
}

namespace
{
// A small tab button with body text, top-left at `pos`.
Box Tab(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, Tone tone, bool right_aligned)
{
  const Paint p = PaintOf(tone);
  const TextLook look = Inked(LabelLook(unit, 2.4f), p);
  const ImVec2 s = Measure(look, text);
  const float pad_x = 0.8f * unit, pad_y = 0.3f * unit;
  const float w = s.x + 2 * pad_x, h = s.y + 2 * pad_y;
  const float x = right_aligned ? pos.x - w : pos.x;
  Slab(dl, ImVec2(x, pos.y), ImVec2(x + w, pos.y + h), h * 0.3f, unit * 0.7f, p, 1);
  Text(dl, ImVec2(x + pad_x, pos.y + pad_y), look, text);
  return {ImVec2(x, pos.y), ImVec2(x + w, pos.y + h)};
}
}  // namespace

Beat TurnPulse(double now_s)
{
  const float t = static_cast<float>(now_s - std::floor(now_s));
  return {0.5f - 0.5f * std::cos(2 * std::numbers::pi_v<float> * t), t};
}

Blink Attention(double now_s, double since_s)
{
  constexpr double PERIOD = 0.4;
  const double t = std::max(0.0, now_s - since_s);
  const float phase = static_cast<float>(std::fmod(t, PERIOD) / PERIOD);
  Blink b;
  b.on = phase < 0.6f ? 1.0f : 0.15f;
  b.swell = phase < 0.6f ? std::sin(std::numbers::pi_v<float> * phase / 0.6f) : 0.0f;
  // Three ripples 0.2 s apart, each lasting 0.4 s.
  for (int i = 0; i < 3; ++i)
  {
    const double r = (t - 0.2 * i) / 0.4;
    if (r >= 0 && r < 1)
      b.ripples[static_cast<std::size_t>(i)] = static_cast<float>(r);
  }
  return b;
}

void Pointer(ImDrawList* dl, ImVec2 c, float radius, float unit, int port, std::string_view tag,
             bool turn, bool tag_left, double now_s)
{
  Pointer(dl, c, radius, unit, port, tag, turn ? PointerMode::Acting : PointerMode::Plain,
          tag_left, now_s, 0);
}

void Pointer(ImDrawList* dl, ImVec2 c, float radius, float unit, int port, std::string_view tag,
             PointerMode mode, bool tag_left, double now_s, double since_s)
{
  // A waiting ring goes grey; the face stays opaque so the game's crosshair never shows through.
  const Paint p = PaintOf(mode == PointerMode::Waiting ? Tone::Grey : PortTone(port));
  // The tab stays put while the cursor swells.
  const float rest = radius;
  if (mode == PointerMode::Acting)
  {
    // The acting player's cursor: a glow in their colour, a white ring lit on each beat, a 15%
    // swell, and ripples when the turn starts.
    const Blink blink = Attention(now_s, since_s);
    for (const float r : blink.ripples)
    {
      if (r < 0)
        continue;
      const float out = rest * (1.1f + 1.6f * r);
      const float fade = std::pow(1 - r, 1.2f);
      const float width = std::max(2.0f, rest * 0.22f * (1 - 0.6f * r));
      dl->AddCircle(c, out, Rgb(0, 0, 0, 0.35f * fade), 48, width + std::max(1.0f, 0.3f * width));
      dl->AddCircle(c, out, Fade(p.top, 0.95f * fade), 48, width);
    }
    for (int k = 3; k >= 1; --k)
    {
      dl->AddCircleFilled(c,
                          rest * (1.12f + 0.2f * static_cast<float>(k)) * (1 + 0.15f * blink.swell),
                          Fade(p.top, (0.12f + 0.16f * blink.on) / static_cast<float>(k)), 48);
    }
    radius = rest * (1 + 0.15f * blink.swell);
    const float ring = radius * 1.32f;
    const float width = std::max(2.0f, 0.2f * rest);
    dl->AddCircle(c, ring, Rgb(0, 0, 0, 0.45f * blink.on), 48, width + std::max(1.5f, 0.35f * width));
    dl->AddCircle(c, ring, Rgb(255, 255, 255, blink.on), 48, width);
  }
  const float thick = std::max(2.0f, 0.26f * radius);
  const float rim = std::max(1.0f, 0.12f * radius);
  dl->AddCircleFilled(ImVec2(c.x + 0.15f * unit, c.y + 0.4f * unit), radius + rim,
                      Rgb(0, 0, 0, 0.36f), 48);
  int first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, radius + rim, IM_COL32_WHITE, 48);
  Recolour(dl, first, c.y - radius, c.y + radius, p.rim_top, p.rim_bottom);
  dl->AddCircleFilled(c, radius, p.line, 48);
  first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, radius - std::max(1.0f, 0.06f * radius), IM_COL32_WHITE, 48);
  Recolour(dl, first, c.y - radius, c.y + radius, p.top, p.bottom);
  // Opaque light face, so the game's crosshair underneath never shows.
  dl->AddCircleFilled(c, radius - thick, p.line, 48);
  first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, radius - thick - std::max(1.0f, 0.05f * radius), IM_COL32_WHITE, 48);
  Recolour(dl, first, c.y - radius, c.y + radius, Rgb(255, 255, 255), Rgb(212, 218, 230));
  for (const ImVec2 d : {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)})
  {
    const ImVec2 a(c.x + d.x * (radius - 0.5f * thick), c.y + d.y * (radius - 0.5f * thick));
    const ImVec2 b(c.x + d.x * radius * 0.3f, c.y + d.y * radius * 0.3f);
    dl->AddLine(a, b, p.line, 0.7f * thick);
    dl->AddLine(a, b, p.bottom, 0.4f * thick);
  }
  dl->AddCircleFilled(c, std::max(1.5f, 0.15f * radius), p.line, 16);
  dl->AddCircleFilled(c, std::max(1.0f, 0.1f * radius), p.bottom, 16);
  if (!tag.empty())
  {
    const float ty = c.y + rest * 0.55f;
    Tab(dl, ImVec2(tag_left ? c.x - rest * 0.55f : c.x + rest * 0.55f, ty), unit, tag,
        PortTone(port), tag_left);
  }
}

Box TurnTag(ImDrawList* dl, ImVec2 at, float radius, float unit, std::string_view title,
            std::string_view detail, Tone tone, int seconds, double now_s, bool lean_left,
            ImVec2 clamp_min, ImVec2 clamp_max, float rim, float size, bool below)
{
  const Paint p = PaintOf(tone);
  const TextLook head = BadgeLook(unit, size, p);
  const TextLook small = Inked(LabelLook(unit, size * 0.72f), p);
  const ImVec2 ts = Measure(head, title);
  const ImVec2 ds = detail.empty() ? ImVec2(0, 0) : Measure(small, detail);
  const float pad_x = 1.3f * unit, pad_y = 0.5f * unit;
  const float w = std::max(ts.x, ds.x) + 2 * pad_x;
  const float h = ts.y + ds.y + 2 * pad_y;
  // Reserve the timer's "0:00" width.
  const float gap = 0.7f * unit;
  float chip_w = 0, chip_h = 0;
  if (seconds >= 0)
  {
    const ImVec2 widest = Measure(TitleLook(unit, 4.2f), "0:00");
    chip_w = widest.x + 2.6f * unit + 0.6f * unit;
    chip_h = widest.y + 0.9f * unit;
  }
  const float total_w = w + (seconds >= 0 ? gap + chip_w : 0);
  const float total_h = std::max(h, chip_h);
  // Above the cursor, leaning to its side; below it, clear of its name tab, near the top edge.
  float x = lean_left ? at.x + 0.6f * radius - total_w : at.x - 0.6f * radius;
  const float above = at.y - radius * 1.75f - total_h;
  const float under = at.y + radius * 0.55f + 3.6f * unit;
  float y = below ? (under + total_h > clamp_max.y ? above : under) :
                    (above < clamp_min.y ? under : above);
  x = std::clamp(x, clamp_min.x, std::max(clamp_min.x, clamp_max.x - total_w));
  y = std::clamp(y, clamp_min.y, std::max(clamp_min.y, clamp_max.y - total_h));
  const ImVec2 min(x, y + (total_h - h) / 2), max(x + w, y + (total_h - h) / 2 + h);
  if (rim > 0)
  {
    const float g = 0.55f * unit;
    const float r = (h + 2 * g) * 0.34f;
    dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g),
                Rgb(0, 0, 0, 0.45f * rim), r, 0, 0.75f * unit);
    dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g),
                Rgb(255, 255, 255, rim), r, 0, 0.45f * unit);
  }
  Slab(dl, min, max, h * 0.34f, unit, p, 1);
  const float cx = (min.x + max.x) / 2;
  Text(dl, ImVec2(cx, min.y + pad_y + head.px * 0.03f), head, title, 1, Align::Centre);
  if (!detail.empty())
    Text(dl, ImVec2(cx, min.y + pad_y + ts.y), small, detail, 1, Align::Centre);
  if (seconds >= 0)
    Timer(dl, ImVec2(max.x + gap + chip_w / 2, y + total_h / 2), unit, seconds, now_s);
  return {min, max};
}

void PortraitFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone)
{
  if (!(max.x > min.x && max.y > min.y))
    return;
  const Paint p = PaintOf(tone);
  // Glow: rings around the frame, fainter further out.
  for (int k = 3; k >= 1; --k)
  {
    const float g = 0.55f * unit * static_cast<float>(k);
    dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g),
                Fade(p.top, 0.22f / static_cast<float>(k)), 0.7f * unit + g, 0, 0.8f * unit);
  }
  Frame(dl, min, max, unit, tone, {}, false);
}

Box Ribbon(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, std::string_view text, Tone tone)
{
  const Box box{min, max};
  if (!(max.x > min.x && max.y > min.y))
    return box;
  const Paint p = PaintOf(tone);
  Slab(dl, min, max, (max.y - min.y) * 0.3f, unit, p, 1);
  // White heavy text outlined in the tone's dark shade, shrunk to fit.
  TextLook look = TitleLook(unit, (max.y - min.y) * 0.62f / unit);
  look.italic = 0.08f;
  look.top = Rgb(255, 255, 255);
  look.bottom = Rgb(255, 242, 214);
  look.outline_colour = tone == Tone::Gold || tone == Tone::Yellow ? Rgb(110, 60, 0) : p.line;
  look.outline = std::max(1.0f, look.px * 0.1f);
  look.shadow = ImVec2(0, 0);
  const float room = (max.x - min.x) - 2.4f * unit;
  for (int i = 0; i < 12 && Measure(look, text).x > room; ++i)
  {
    look.px *= 0.92f;
    look.outline = std::max(1.0f, look.px * 0.1f);
  }
  Text(dl, ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2 - look.px / 2 + look.px * 0.04f), look,
       text, 1, Align::Centre);
  return box;
}

Box SetDots(ImDrawList* dl, ImVec2 centre, float unit, const std::array<int, 3>& games, int current)
{
  const float r = 0.9f * unit, gap = 1.25f * unit;
  const float span = 3 * 2 * r + 2 * gap;
  const Box box{ImVec2(centre.x - span / 2, centre.y - r), ImVec2(centre.x + span / 2, centre.y + r)};
  for (int i = 0; i < 3; ++i)
  {
    const ImVec2 c(box.min.x + r + static_cast<float>(i) * (2 * r + gap), centre.y);
    const int won = games[static_cast<std::size_t>(i)];
    if (won == 0 || won == 1)
    {
      // Decided: filled in the winner's colour, white rim.
      const Paint p = PaintOf(PortTone(won));
      dl->AddCircleFilled(ImVec2(c.x, c.y + 0.15f * unit), r + 0.2f * unit, Rgb(0, 0, 0, 0.35f), 24);
      dl->AddCircleFilled(c, r + 0.2f * unit, Rgb(255, 255, 255), 24);
      const int first = dl->VtxBuffer.Size;
      dl->AddCircleFilled(c, r, IM_COL32_WHITE, 24);
      Recolour(dl, first, c.y - r, c.y + r, p.top, p.bottom);
    }
    else
    {
      // Not yet played: hollow, brighter rim for the current game.
      dl->AddCircleFilled(c, r, Rgb(0, 0, 0, 0.35f), 24);
      dl->AddCircle(c, r, i == current ? Rgb(236, 236, 236) : Rgb(130, 130, 130), 24,
                    std::max(1.0f, 0.2f * unit));
    }
  }
  return box;
}

void SearchRing(ImDrawList* dl, ImVec2 c, float radius, float unit, double now_s)
{
  if (radius <= 0)
    return;
  const float w = std::max(2.0f, 0.75f * unit);
  dl->AddCircle(c, radius, Rgb(255, 255, 255, 0.4f), 64, w);
  const float pi = std::numbers::pi_v<float>;
  const float a0 = static_cast<float>(std::fmod(now_s * 0.8, 1.0)) * 2 * pi - pi / 2;
  dl->PathArcTo(c, radius, a0, a0 + 0.28f * 2 * pi, 32);
  dl->PathStroke(Rgb(60, 36, 0, 0.6f), ImDrawFlags_None, w + std::max(1.0f, 0.3f * w));
  dl->PathArcTo(c, radius, a0, a0 + 0.28f * 2 * pi, 32);
  dl->PathStroke(Rgb(255, 214, 40), ImDrawFlags_None, w);
}

Box GameTimer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s,
               Lettering lettering, float size)
{
  seconds = std::max(0, seconds);
  const std::string text = fmt::format("{}:{:02}", seconds / 60, seconds % 60);
  TextLook look = TitleLook(unit, size);
  look.shadow = ImVec2(0, 0);
  const bool pplus = CurrentLook() == Look::ProjectPlus;
  // Colours sampled from the game's own lettering.
  ImU32 edge = 0;
  if (lettering == Lettering::Band)
  {
    look.italic = 0.2f;
    look.top = pplus ? Rgb(210, 255, 238) : Rgb(255, 150, 80);
    look.bottom = pplus ? Rgb(14, 140, 108) : Rgb(240, 70, 36);
    look.outline_colour = pplus ? Rgb(6, 30, 28) : Rgb(255, 255, 255);
    if (!pplus)
      edge = Rgb(70, 20, 10);
  }
  else
  {
    look.italic = 0.25f;
    look.top = pplus ? Rgb(150, 162, 164) : Rgb(176, 112, 104);
    look.bottom = pplus ? Rgb(118, 132, 134) : Rgb(156, 94, 88);
    look.outline = 0;
  }
  if (seconds <= 5)
    look.px *= 1 + 0.14f * Pulse(now_s);
  if (lettering == Lettering::Band)
    look.outline = std::max(1.0f, look.px * 0.09f);
  const ImVec2 widest = Measure(TitleLook(unit, size), "0:00");
  const Box box{ImVec2(centre.x - widest.x / 2, centre.y - widest.y / 2),
                ImVec2(centre.x + widest.x / 2, centre.y + widest.y / 2)};
  const ImVec2 at(centre.x, centre.y - look.px / 2 + look.px * 0.04f);
  if (edge)
  {
    // Brawl's band lettering has a dark edge outside the white one.
    TextLook outer = look;
    outer.outline = look.outline + std::max(1.0f, look.px * 0.06f);
    outer.outline_colour = edge;
    Text(dl, at, outer, text, 1, Align::Centre);
  }
  Text(dl, at, look, text, 1, Align::Centre);
  return box;
}

namespace
{
constexpr std::array<std::string_view, 8> BUTTONS{"A", "B", "X", "Y", "Z", "L", "R", "Start"};

bool IsButton(std::string_view word)
{
  return std::find(BUTTONS.begin(), BUTTONS.end(), word) != BUTTONS.end();
}

std::string Capital(std::string_view text)
{
  std::string out(text);
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z')
    out[0] = static_cast<char>(out[0] - 'a' + 'A');
  return out;
}

bool Starts(std::string_view text, std::string_view prefix)
{
  return text.substr(0, prefix.size()) == prefix;
}

// Strips a trailing "s" from a verb ("strikes" -> "strike").
std::string Plain(std::string_view verb)
{
  if (verb.size() > 2 && verb.back() == 's')
    verb.remove_suffix(1);
  return std::string(verb);
}

// Parses one hint clause. Recognised shapes (after an optional "Your turn: " prefix):
//   "[Press |Hold ]<button> to <what>"   B to change, Hold B to leave
//   "<button> on <thing> to <what>"      A on it to agree
//   "<button> on <thing> <does> it"      X on a stage strikes it
//   "<button> on <thing>"                A on a character
//   "<button> <does> [<what>]"           Start locks it in, Y skips
//   "<what> with <button>"               Pick with A
// Anything else becomes plain words.
GuideItem ClauseOf(std::string_view clause)
{
  // "Your turn: X on a stage strikes it": parse what follows the colon.
  if (const std::size_t colon = clause.find(": "); colon != std::string_view::npos)
  {
    const std::string_view after = clause.substr(colon + 2);
    if (IsButton(after.substr(0, after.find(' '))))
      clause = after;
  }
  bool hold = false;
  if (Starts(clause, "Press ") || Starts(clause, "press "))
    clause.remove_prefix(6);
  else if (Starts(clause, "Hold ") || Starts(clause, "hold "))
    hold = true, clause.remove_prefix(5);
  const std::size_t space = clause.find(' ');
  const std::string_view first = clause.substr(0, space);
  if (IsButton(first))
  {
    const std::string button(first);
    if (space == std::string_view::npos)
      return {button, hold ? std::string("Hold") : std::string()};
    std::string_view rest = clause.substr(space + 1);
    const auto label = [&](std::string_view what) {
      return hold ? "Hold: " + std::string(what) : Capital(what);
    };
    if (Starts(rest, "to "))
      return {button, label(rest.substr(3))};
    if (Starts(rest, "on "))
    {
      rest.remove_prefix(3);
      if (const std::size_t to = rest.find(" to "); to != std::string_view::npos)
        return {button, label(rest.substr(to + 4))};
      if (rest.size() > 3 && rest.substr(rest.size() - 3) == " it")
      {
        const std::string_view head = rest.substr(0, rest.size() - 3);
        const std::size_t verb = head.rfind(' ');
        if (verb != std::string_view::npos)
          return {button, label(Plain(head.substr(verb + 1)))};
      }
      return {button, label("pick " + std::string(rest))};
    }
    const std::size_t verb_end = rest.find(' ');
    const std::string verb = Plain(rest.substr(0, verb_end));
    return {button, label(verb_end == std::string_view::npos ?
                              verb :
                              verb + std::string(rest.substr(verb_end)))};
  }
  if (const std::size_t with = clause.rfind(" with "); with != std::string_view::npos &&
                                                        IsButton(clause.substr(with + 6)))
  {
    return {std::string(clause.substr(with + 6)), Capital(clause.substr(0, with))};
  }
  return {std::string(), std::string(hold ? "Hold " : "") + std::string(clause)};
}
}  // namespace

std::vector<GuideItem> GuideOf(std::string_view hint)
{
  std::vector<GuideItem> items;
  const auto clauses = [&items](std::string_view part) {
    // "Pick with A, then press Start to lock in": one entry per clause.
    for (;;)
    {
      const std::size_t then = part.find(", then ");
      const std::string_view clause = part.substr(0, then);
      if (!clause.empty())
        items.push_back(ClauseOf(clause));
      if (then == std::string_view::npos)
        break;
      part.remove_prefix(then + 7);
    }
  };
  constexpr std::string_view DOT = " · ";
  for (;;)
  {
    const std::size_t dot = hint.find(DOT);
    clauses(hint.substr(0, dot));
    if (dot == std::string_view::npos)
      break;
    hint.remove_prefix(dot + DOT.size());
  }
  return items;
}

namespace
{
// Size of a GameCube button face `h` pixels high (Start is shorter).
ImVec2 ButtonSize(float h, std::string_view button)
{
  if (button == "Start")
    return ImVec2(0.82f * h * 1.9f, 0.82f * h);
  return ImVec2(h * (button == "Z" || button == "L" || button == "R" ? 1.5f : 1.0f), h);
}

// Draws a GameCube button face centred on `c`.
void ButtonFace(ImDrawList* dl, ImVec2 c, float h, float unit, std::string_view button, float alpha)
{
  const bool start = button == "Start", z = button == "Z";
  const ImVec2 size = ButtonSize(h, button);
  const float w = size.x;
  h = size.y;
  ImU32 top = Rgb(240, 240, 244, alpha), bottom = Rgb(160, 160, 170, alpha);
  if (button == "A")
    top = Rgb(90, 214, 140, alpha), bottom = Rgb(20, 140, 70, alpha);
  else if (button == "B")
    top = Rgb(250, 110, 110, alpha), bottom = Rgb(190, 30, 40, alpha);
  else if (z)
    top = Rgb(170, 120, 250, alpha), bottom = Rgb(90, 50, 190, alpha);
  const ImVec2 min(c.x - w / 2, c.y - h / 2), max(c.x + w / 2, c.y + h / 2);
  const float r = h / 2;
  dl->AddRectFilled(ImVec2(min.x, min.y + 0.12f * h), ImVec2(max.x, max.y + 0.12f * h),
                    Rgb(0, 0, 0, 0.45f * alpha), r);
  dl->AddRectFilled(min, max, Rgb(40, 40, 44, alpha), r);
  const float in = std::max(1.5f, h / 12);
  FillRounded(dl, ImVec2(min.x + in, min.y + in), ImVec2(max.x - in, max.y - in), r - in, top,
              bottom);
  TextLook look = TitleLook(unit, (h * (button.size() == 1 ? 0.62f : 0.4f)) / unit);
  look.italic = 0;
  look.top = look.bottom = button == "A" || button == "B" || z ? Rgb(255, 255, 255) :
                                                                  Rgb(60, 60, 66);
  look.outline = 0;
  look.shadow = ImVec2(0, 0);
  look.tracking = 0;
  const std::string text = start ? std::string("START") : std::string(button);
  Text(dl, ImVec2(c.x, c.y - look.px / 2 + look.px * 0.06f), look, text, alpha, Align::Centre);
}
}  // namespace

namespace
{
// Lays out the guide right to left from `right`. With a null `dl` it only measures.
Box LayGuide(ImDrawList* dl, float right, float cy, float unit,
             const std::vector<GuideItem>& items, float alpha)
{
  const float h = 3.3f * unit;
  TextLook look = LabelLook(unit, 2.6f);
  look.outline = std::max(1.0f, look.px * 0.1f);
  look.outline_colour = Rgb(20, 20, 20);
  Box box{ImVec2(right, cy - h / 2), ImVec2(right, cy + h / 2)};
  float x = right;
  // The last entry ends at `right`.
  for (auto it = items.rbegin(); it != items.rend(); ++it)
  {
    if (!it->label.empty())
    {
      const ImVec2 size = Measure(look, it->label);
      if (dl)
        Text(dl, ImVec2(x - size.x, cy - look.px / 2), look, it->label, alpha);
      x -= size.x + (it->button.empty() ? 0.0f : 1.0f * unit);
    }
    if (!it->button.empty())
    {
      const float w = ButtonSize(h, it->button).x;
      if (dl)
        ButtonFace(dl, ImVec2(x - w / 2, cy), h, unit, it->button, alpha);
      x -= w;
    }
    box.min.x = x;
    x -= 2.6f * unit;
  }
  return box;
}
}  // namespace

Box Guide(ImDrawList* dl, float right, float cy, float unit, const std::vector<GuideItem>& items,
          float alpha, float max_width)
{
  // Too wide: shrink the whole line, down to 70%, as the games do with their own legends.
  // Re-measure at each step, since outlines and font sizes don't scale exactly.
  if (max_width > 0)
  {
    const float full = unit;
    for (int i = 0; i < 4; ++i)
    {
      const float width = right - LayGuide(nullptr, right, cy, unit, items, alpha).min.x;
      if (width <= max_width || unit <= 0.7f * full)
        break;
      unit = std::max(0.7f * full, unit * max_width / width * 0.995f);
    }
  }
  return LayGuide(dl, right, cy, unit, items, alpha);
}

void Frame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, std::string_view tag,
           bool tag_left, float inset)
{
  const Paint p = PaintOf(tone);
  min = ImVec2(min.x + inset, min.y + inset);
  max = ImVec2(max.x - inset, max.y - inset);
  const float round = 0.7f * unit;
  // A ring layered like the buttons.
  dl->AddRect(ImVec2(min.x + 0.15f * unit, min.y + 0.35f * unit),
              ImVec2(max.x + 0.15f * unit, max.y + 0.35f * unit), Rgb(0, 0, 0, 0.32f), round, 0,
              1.2f * unit);
  dl->AddRect(min, max, p.rim_top, round, 0, 1.25f * unit);
  dl->AddRect(min, max, p.line, round, 0, 0.9f * unit);
  dl->AddRect(min, max, p.bottom, round, 0, 0.6f * unit);
  dl->AddRect(min, max, p.top, round, 0, 0.25f * unit);
  if (!tag.empty())
  {
    const TextLook look = LabelLook(unit, 2.4f);
    const float h = Measure(look, tag).y + 0.6f * unit;
    Tab(dl, ImVec2(tag_left ? min.x - 0.2f * unit : max.x + 0.2f * unit, min.y - h * 0.8f), unit,
        tag, tone, !tag_left);
  }
}

namespace
{
// Draws a disc's layers and returns the body's radius.
float DiscBody(ImDrawList* dl, ImVec2 c, float radius, float unit, const Paint& p, float alpha)
{
  const float rim = std::clamp(radius * 0.12f, 1.0f, 0.55f * unit);
  const float line = std::clamp(radius * 0.06f, 1.0f, 0.3f * unit);
  dl->AddCircleFilled(ImVec2(c.x + 0.15f * unit, c.y + 0.4f * unit), radius,
                      Rgb(0, 0, 0, 0.34f * alpha), 40);
  int first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, radius, IM_COL32_WHITE, 40);
  Recolour(dl, first, c.y - radius, c.y + radius, Fade(p.rim_top, alpha),
           Fade(p.rim_bottom, alpha));
  dl->AddCircleFilled(c, radius - rim, Fade(p.line, alpha), 40);
  const float body = radius - rim - line;
  first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, body, IM_COL32_WHITE, 40);
  Recolour(dl, first, c.y - body, c.y + body, Fade(p.top, alpha), Fade(p.bottom, alpha));
  if (p.gloss > 0)
  {
    first = dl->VtxBuffer.Size;
    dl->AddEllipseFilled(ImVec2(c.x, c.y - body * 0.42f), ImVec2(body * 0.78f, body * 0.5f),
                         IM_COL32_WHITE, 0, 32);
    Recolour(dl, first, c.y - body * 0.92f, c.y + body * 0.08f, Rgb(255, 255, 255, p.gloss * alpha),
             Rgb(255, 255, 255, 0.1f * p.gloss * alpha));
  }
  return body;
}
}  // namespace

void Disc(ImDrawList* dl, ImVec2 centre, float radius, float unit, Tone tone, float alpha)
{
  if (radius <= 0 || alpha <= 0)
    return;
  DiscBody(dl, centre, radius, unit, PaintOf(tone), alpha);
}

void Connection(ImDrawList* dl, ImVec2 c, float radius, float unit, int bars, float alpha)
{
  if (radius <= 0 || alpha <= 0)
    return;
  bars = std::clamp(bars, 1, 3);
  // Panel disc, layered like the buttons.
  const float body = DiscBody(dl, c, radius, unit, PaintOf(Tone::Panel), alpha);
  // Arcs with a dark outline so they read on any panel.
  const ImU32 on = bars == 3 ? Rgb(96, 206, 255, alpha) :
                   bars == 2 ? Rgb(255, 210, 64, alpha) :
                               Rgb(255, 76, 64, alpha);
  const ImU32 off = Rgb(255, 255, 255, 0.28f * alpha);
  const ImU32 dark = Rgb(0, 0, 0, 0.7f * alpha);
  const ImVec2 base(c.x, c.y + body * 0.5f);
  const float w = std::max(1.5f, body * 0.13f);
  const float pi = std::numbers::pi_v<float>;
  for (int i = 0; i < 3; ++i)
  {
    const float r = body * (0.36f + 0.24f * static_cast<float>(i));
    dl->PathArcTo(base, r, pi * 1.25f, pi * 1.75f, 16);
    dl->PathStroke(dark, ImDrawFlags_None, w + std::max(1.0f, 0.5f * w));
    dl->PathArcTo(base, r, pi * 1.25f, pi * 1.75f, 16);
    dl->PathStroke(i < bars ? on : off, ImDrawFlags_None, w);
  }
  dl->AddCircleFilled(base, w * 0.95f + std::max(0.5f, 0.25f * w), dark, 16);
  dl->AddCircleFilled(base, w * 0.95f, on, 16);
}

ImU32 Accent()
{
  return CurrentLook() == Look::ProjectPlus ? Rgb(110, 236, 204) : Rgb(250, 204, 92);
}

TextLook InkLook(float unit, float size, Tone tone)
{
  return Inked(LabelLook(unit, size), PaintOf(tone));
}

ImVec2 BadgeSize(float unit, std::string_view text, Tone tone, float size)
{
  const ImVec2 s = Measure(BadgeLook(unit, size, PaintOf(tone)), text);
  return ImVec2(s.x + 2 * BADGE_PAD_X * unit, s.y + 2 * BADGE_PAD_Y * unit);
}

Box TextPlate(ImDrawList* dl, ImVec2 pos, float unit, const std::vector<std::string>& lines,
              float size, std::size_t lead, Tone tone, float alpha)
{
  const TextLook look = InkLook(unit, size, tone);
  const float pad_x = 1.4f * unit, pad_y = 0.55f * unit, gap = 0.15f * unit;
  float w = 0;
  for (const std::string& l : lines)
    w = std::max(w, Measure(look, l).x);
  const float line_h = look.px + gap;
  const float h = static_cast<float>(lines.size()) * line_h - gap + 2 * pad_y;
  const Box box{pos, ImVec2(pos.x + w + 2 * pad_x, pos.y + h)};
  if (lines.empty() || alpha <= 0)
    return box;
  Plate(dl, box.min, box.max, unit, tone, alpha);
  float y = pos.y + pad_y;
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    const std::string_view line = lines[i];
    if (i == 0 && lead > 0 && lead <= line.size())
    {
      // Sender's name in the accent colour, the rest in the ink colour.
      TextLook accent = look;
      accent.top = Accent();
      accent.bottom = Mix(Accent(), Rgb(0, 0, 0), 0.1f);
      const ImVec2 s = Text(dl, ImVec2(pos.x + pad_x, y), accent, line.substr(0, lead), alpha);
      Text(dl, ImVec2(pos.x + pad_x + s.x - 2 * look.outline, y), look, line.substr(lead), alpha);
    }
    else
    {
      Text(dl, ImVec2(pos.x + pad_x, y), look, line, alpha);
    }
    y += line_h;
  }
  return box;
}

void UseAtlasForTests(Face face, const GameFont::Atlas* atlas, ImTextureID texture)
{
  s_test_faces[static_cast<int>(face)] = {atlas, texture};
}

void UseFontsForTests(const GameFont::Atlas* body, const GameFont::Atlas* heavy)
{
  std::lock_guard lk(s_lock);
  s_read_atlases = {};
  if (body)
    s_read_atlases[static_cast<int>(Face::Body)] = *body;
  if (heavy)
    s_read_atlases[static_cast<int>(Face::Heavy)] = *heavy;
  s_started = body || heavy;
  s_read = body || heavy;
}

int DemoMode()
{
  static const int mode = [] {
    const char* v = std::getenv("ORCA_UX_KIT_DEMO");
    return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
  }();
  return mode;
}

void DrawDemo(ImDrawList* dl, ImVec2 o, ImVec2 size, float u, double now_s)
{
  const auto at = [&](float fx, float fy) { return ImVec2(o.x + size.x * fx, o.y + size.y * fy); };
  // Label each primitive with its name.
  const auto caption = [&](ImVec2 p, std::string_view name, Align align = Align::Centre) {
    Label(dl, p, u, name, 1.9f, align, Rgb(176, 206, 255));
  };
  // Top centre: set banner and timers.
  const Box banner = Banner(dl, at(0.29f, 0).x, at(0.71f, 0).x, o.y + 2.2f * u, 9.0f * u, u,
                            Tone::Panel, "RANKED  GAME 2", "ada 1 – 0 bo · bo bans a stage");
  caption(ImVec2((banner.min.x + banner.max.x) / 2, banner.max.y + 0.4f * u), "Banner");
  const float mid = (banner.min.y + banner.max.y) / 2;
  const Box t1 = Timer(dl, ImVec2(banner.max.x + 7.0f * u, mid), u, 24, now_s);
  const Box t2 = Timer(dl, ImVec2(t1.max.x + 7.5f * u, mid), u, 4, now_s);
  caption(ImVec2((t1.min.x + t2.max.x) / 2, t1.max.y + 0.6f * u), "Timer");
  // Top left: toasts, a chat line and the ping.
  float y = o.y + 2.5f * u;
  y = LabelPlate(dl, ImVec2(o.x + 2.5f * u, y), u, "bo joined on port 2", 2.7f).max.y + 1.0f * u;
  y = LabelPlate(dl, ImVec2(o.x + 2.5f * u, y), u, "bo: gl hf … one more?", 2.7f).max.y +
      0.3f * u;
  caption(ImVec2(o.x + 2.5f * u, y), "LabelPlate", Align::Left);
  y += 2.8f * u;
  const float lamp = 3.0f * u;
  Connection(dl, ImVec2(o.x + 2.5f * u + lamp, y + lamp), lamp, u, 3);
  Connection(dl, ImVec2(o.x + 4.0f * u + 3 * lamp, y + lamp), lamp, u, 2);
  Connection(dl, ImVec2(o.x + 5.5f * u + 5 * lamp, y + lamp), lamp, u, 1);
  caption(ImVec2(o.x + 2.5f * u, y + 2 * lamp + 0.3f * u), "Connection", Align::Left);
  y += 2 * lamp + 3.1f * u;
  y = Label(dl, ImVec2(o.x + 2.5f * u, y), u, "32 ms · 60 fps", 2.6f).max.y;
  caption(ImVec2(o.x + 2.5f * u, y), "Label", Align::Left);
  // Middle: title, label, pointers and framed tiles.
  const Box title = Title(dl, at(0.5f, 0.19f), u, "Waiting for bo…", 5.5f, Align::Centre);
  caption(ImVec2(title.max.x + 1.5f * u, title.min.y + 2.0f * u), "Title", Align::Left);
  Label(dl, Add(at(0.5f, 0.19f), ImVec2(0, 6.4f * u)), u, "bo forfeits in 0:18", 3.2f,
        Align::Centre, Rgb(255, 226, 120));
  Frame(dl, at(0.40f, 0.34f), at(0.48f, 0.44f), u, Tone::Red, "ada", true);
  Frame(dl, at(0.52f, 0.34f), at(0.60f, 0.44f), u, Tone::Gold, "ada + bo", true);
  caption(ImVec2(at(0.60f, 0).x + 1.5f * u, at(0, 0.34f).y), "Frame", Align::Left);
  Pointer(dl, at(0.44f, 0.40f), 2.6f * u, u, 0, "P1 ada", true, true, now_s);
  Pointer(dl, at(0.585f, 0.415f), 2.6f * u, u, 1, "P2 bo", false, false, now_s);
  caption(ImVec2(at(0.60f, 0).x + 1.5f * u, at(0, 0.40f).y), "Pointer", Align::Left);
  // Lower middle: a results card.
  const ImVec2 c0 = at(0.335f, 0.475f), c1 = at(0.665f, 0.565f);
  Plate(dl, c0, c1, u, Tone::Panel);
  caption(ImVec2(c1.x + 1.5f * u, c0.y + 1.0f * u), "Plate", Align::Left);
  const float cy = (c0.y + c1.y) / 2;
  Title(dl, ImVec2(c0.x + 2.0f * u, cy - 2.4f * u), u, "VICTORY", 4.8f);
  Label(dl, ImVec2(c1.x - 2.0f * u, cy - 3.3f * u), u, "1532 → 1550", 3.2f, Align::Right);
  Badge(dl, ImVec2(c1.x - 6.0f * u, cy + 2.0f * u), u, "+18", Tone::Gold, 2.8f);
  // Character select panels: name plates and LOCKED IN.
  for (int port = 0; port < 2; ++port)
  {
    const float cx = o.x + size.x * (0.236f + 0.177f * static_cast<float>(port));
    LabelPlate(dl, ImVec2(cx, o.y + size.y * 0.58f), u, port == 0 ? "ada · 1532" : "bo · 1498",
               2.8f, Align::Centre, PortTone(port));
    Badge(dl, ImVec2(cx, o.y + size.y * 0.71f), u, "LOCKED IN", PortTone(port), 3.2f);
  }
  Badge(dl, at(0.62f, 0.71f), u, "READY", Tone::Gold, 3.2f);
  const Box waiting = Badge(dl, at(0.77f, 0.71f), u, "WAITING", Tone::Grey, 3.2f);
  caption(ImVec2(waiting.max.x + 1.5f * u, waiting.min.y + 0.8f * u), "Badge", Align::Left);
  LabelPlate(dl, at(0.70f, 0.58f), u, "Hold B to leave", 2.6f, Align::Centre, Tone::Green);
  // Portrait frame, turn frame, set dots and the searching ring.
  const ImVec2 f0 = at(0.80f, 0.79f), f1 = at(0.88f, 0.95f);
  PortraitFrame(dl, f0, f1, u, Tone::Gold);
  Ribbon(dl, ImVec2(f0.x + 1.0f * u, f0.y + 1.2f * u), ImVec2(f1.x - 1.0f * u, f0.y + 5.2f * u), u,
         "LOCKED IN", Tone::Gold);
  PortraitFrame(dl, at(0.90f, 0.79f), at(0.98f, 0.95f), u, Tone::Blue);
  caption(ImVec2(f0.x, f1.y + 0.4f * u), "PortraitFrame, Ribbon", Align::Left);
  const Box dots = SetDots(dl, at(0.5f, 0.62f), u, {0, 1, -1}, 2);
  caption(ImVec2(dots.max.x + 1.5f * u, dots.min.y), "SetDots", Align::Left);
  SearchRing(dl, at(0.72f, 0.87f), 5.0f * u, u, now_s);
  caption(ImVec2(at(0.72f, 0).x, at(0, 0.87f).y + 5.6f * u), "SearchRing");
}
}  // namespace Orca::UX::Kit
