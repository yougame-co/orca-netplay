// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// Loads the game's own NW4R fonts (RFNT) from the player's disc and builds a glyph atlas for the
// overlay (Kit.h). The fonts are read at run time, kept in memory only, and never written out, so
// Orca ships none of the game's art.
namespace Orca::UX::GameFont
{
// Decompresses Nintendo's LZ77 (type 0x10). Nullopt when malformed.
std::optional<std::vector<u8>> DecompressLz10(std::span<const u8> data);

// Reads one UTF-8 code point at `*at` and advances past it. U+FFFD for a malformed byte.
char32_t NextCodePoint(std::string_view text, std::size_t* at);

struct Widths
{
  int left = 0;     // glyph image offset from the pen
  int glyph = 0;    // glyph image width
  int advance = 0;  // pen advance
};

class Rfnt
{
public:
  // Supports RFNT version 1.4 with I4, I8, IA4 or IA8 sheets. Nullopt (with `error`) otherwise.
  static std::optional<Rfnt> Parse(std::vector<u8> data, std::string* error = nullptr);

  int CellWidth() const { return m_cell_w; }
  int CellHeight() const { return m_cell_h; }
  int Ascent() const { return m_ascent; }
  int LineFeed() const { return m_linefeed; }
  int Baseline() const { return m_baseline; }
  std::size_t GlyphCount() const { return m_glyphs; }
  // Nullopt when the font lacks the code point (never the font's fallback glyph).
  std::optional<u16> IndexOf(char32_t code) const;
  Widths WidthsOf(u16 index) const;
  // Glyph coverage (0-255), CellWidth() x CellHeight(), row major. False if out of range.
  bool GlyphAlpha(u16 index, std::vector<u8>* out) const;

private:
  struct Map
  {
    u16 begin, end, method;
    std::size_t info;  // offset of the map's table
  };
  struct WidthBlock
  {
    u16 begin, end;
    std::size_t table;
  };
  u16 ReadU16(std::size_t at) const;
  const std::vector<u8>& Sheet(int sheet) const;

  std::vector<u8> m_data;
  int m_cell_w = 0, m_cell_h = 0, m_ascent = 0, m_linefeed = 0, m_baseline = 0;
  u16 m_alter = 0;
  Widths m_default;
  std::size_t m_glyphs = 0;
  u32 m_sheet_size = 0;
  int m_sheet_count = 0, m_sheet_format = 0, m_per_row = 0, m_per_column = 0;
  int m_sheet_w = 0, m_sheet_h = 0;
  std::size_t m_sheet_data = 0;
  std::vector<Map> m_maps;
  std::vector<WidthBlock> m_widths;
  mutable std::map<int, std::vector<u8>> m_sheets;  // decoded sheets, cached lazily
};

// Reads fonts from the disc image (paths like "system/font/font_latin1.arc"), one entry per file,
// nullopt for any it couldn't read. Empty if the disc isn't `game_id`. Safe on any thread.
std::vector<std::optional<Rfnt>> ReadFromDisc(const std::string& disc,
                                              std::span<const char* const> files,
                                              std::string_view game_id, std::string* error = nullptr);

// Packed RGBA8 glyphs (white, coverage in alpha) with mip levels, so they scale cleanly.
struct Atlas
{
  struct Glyph
  {
    float u0, v0, u1, v1;    // cell in the atlas
    float left, width;       // font pixels, from the pen
    float advance;           // font pixels
  };
  int width = 0, height = 0;
  std::vector<std::vector<u8>> levels;  // level n is (width >> n) x (height >> n)
  std::map<char32_t, Glyph> glyphs;
  float cell_height = 0;  // font pixels: line height and glyph quad height
  float ascent = 0;
  float space = 0;  // a space's advance
};

// Code points the overlay may use: ASCII, Latin-1 and some punctuation and arrows.
std::vector<char32_t> OverlayCodePoints();
// Fallbacks for a code point the font lacks, in order of preference. Empty if none.
std::u32string Substitute(char32_t code);

// Builds an atlas of the requested code points the font has. `pad` keeps texture filtering from
// bleeding into neighbours; `levels` is the mip count (at least 1).
Atlas BuildAtlas(const Rfnt& font, std::span<const char32_t> codes, int pad = 4, int levels = 4);
}  // namespace Orca::UX::GameFont
