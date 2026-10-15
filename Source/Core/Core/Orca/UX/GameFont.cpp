// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/GameFont.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace Orca::UX::GameFont
{
namespace
{
u32 BigU32(std::span<const u8> d, std::size_t at)
{
  return (u32{d[at]} << 24) | (u32{d[at + 1]} << 16) | (u32{d[at + 2]} << 8) | u32{d[at + 3]};
}

u16 BigU16(std::span<const u8> d, std::size_t at)
{
  return static_cast<u16>((d[at] << 8) | d[at + 1]);
}


// Decodes a font sheet to coverage. I4/I8 store it directly; IA4/IA8 store it in alpha.
bool DecodeCoverage(int format, int w, int h, std::span<const u8> src, std::vector<u8>* out)
{
  out->assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
  int bw, bh, bits;
  switch (format)
  {
  case 0:
    bw = 8, bh = 8, bits = 4;
    break;
  case 1:
    bw = 8, bh = 4, bits = 8;
    break;
  case 2:
    bw = 8, bh = 4, bits = 8;
    break;
  case 3:
    bw = 4, bh = 4, bits = 16;
    break;
  default:
    return false;
  }
  const std::size_t need = static_cast<std::size_t>((w + bw - 1) / bw) *
                           static_cast<std::size_t>((h + bh - 1) / bh) *
                           static_cast<std::size_t>(bw * bh * bits / 8);
  if (src.size() < need)
    return false;
  std::size_t p = 0;
  for (int by = 0; by < h; by += bh)
  {
    for (int bx = 0; bx < w; bx += bw)
    {
      for (int y = 0; y < bh; ++y)
      {
        for (int x = 0; x < bw; ++x)
        {
          u8 a;
          switch (format)
          {
          case 0:
          {
            const u8 v = src[p + static_cast<std::size_t>(x / 2)];
            const u8 n = (x & 1) ? (v & 0xF) : (v >> 4);
            a = static_cast<u8>(n * 17);
            break;
          }
          case 1:
            a = src[p + static_cast<std::size_t>(x)];
            break;
          case 2:
            a = static_cast<u8>((src[p + static_cast<std::size_t>(x)] >> 4) * 17);
            break;
          default:
            a = src[p + static_cast<std::size_t>(x) * 2];
            break;
          }
          const int px = bx + x, py = by + y;
          if (px < w && py < h)
            (*out)[static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + px] = a;
        }
        p += static_cast<std::size_t>(bw * bits / 8);
      }
    }
  }
  return true;
}
}  // namespace

std::optional<std::vector<u8>> DecompressLz10(std::span<const u8> d)
{
  if (d.size() < 4 || d[0] != 0x10)
    return std::nullopt;
  const std::size_t size = static_cast<std::size_t>(d[1]) | (static_cast<std::size_t>(d[2]) << 8) |
                           (static_cast<std::size_t>(d[3]) << 16);
  std::vector<u8> out;
  out.reserve(size);
  std::size_t i = 4;
  while (out.size() < size)
  {
    if (i >= d.size())
      return std::nullopt;
    const u8 flags = d[i++];
    for (int b = 0; b < 8 && out.size() < size; ++b)
    {
      if (flags & (0x80 >> b))
      {
        if (i + 1 >= d.size())
          return std::nullopt;
        const int v = (d[i] << 8) | d[i + 1];
        i += 2;
        const std::size_t n = static_cast<std::size_t>(v >> 12) + 3;
        const std::size_t back = static_cast<std::size_t>(v & 0xFFF) + 1;
        if (back > out.size())
          return std::nullopt;
        for (std::size_t k = 0; k < n && out.size() < size; ++k)
          out.push_back(out[out.size() - back]);
      }
      else
      {
        if (i >= d.size())
          return std::nullopt;
        out.push_back(d[i++]);
      }
    }
  }
  return out;
}

char32_t NextCodePoint(std::string_view s, std::size_t* at)
{
  const auto byte = [&](std::size_t k) { return static_cast<u8>(s[k]); };
  const std::size_t i = *at;
  const u8 c = byte(i);
  int extra;
  char32_t code;
  if (c < 0x80)
  {
    *at = i + 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0)
    extra = 1, code = static_cast<char32_t>(c & 0x1F);
  else if ((c & 0xF0) == 0xE0)
    extra = 2, code = static_cast<char32_t>(c & 0x0F);
  else if ((c & 0xF8) == 0xF0)
    extra = 3, code = static_cast<char32_t>(c & 0x07);
  else
  {
    *at = i + 1;
    return 0xFFFD;
  }
  if (i + static_cast<std::size_t>(extra) >= s.size())
  {
    *at = s.size();
    return 0xFFFD;
  }
  for (int k = 1; k <= extra; ++k)
  {
    const u8 cc = byte(i + static_cast<std::size_t>(k));
    if ((cc & 0xC0) != 0x80)
    {
      *at = i + static_cast<std::size_t>(k);
      return 0xFFFD;
    }
    code = static_cast<char32_t>((code << 6) | static_cast<char32_t>(cc & 0x3F));
  }
  *at = i + static_cast<std::size_t>(extra) + 1;
  return code;
}

std::optional<Rfnt> Rfnt::Parse(std::vector<u8> data, std::string* error)
{
  const auto refuse = [error](std::string why) -> std::optional<Rfnt> {
    if (error)
      *error = std::move(why);
    return std::nullopt;
  };
  Rfnt f;
  const std::span<const u8> d(data);
  if (d.size() < 16 || std::memcmp(d.data(), "RFNT", 4) != 0)
    return refuse("not an RFNT");
  if (BigU16(d, 4) != 0xFEFF)
    return refuse("not big endian");
  if (BigU16(d, 6) != 0x0104)
    return refuse(fmt::format("version {:04x}", BigU16(d, 6)));
  const std::size_t file_size = std::min<std::size_t>(BigU32(d, 8), d.size());
  bool finf = false, tglp = false;
  for (std::size_t off = BigU16(d, 12); off + 8 <= file_size;)
  {
    const u32 size = BigU32(d, off + 4);
    if (size < 8 || off + size > file_size)
      return refuse("a block runs past the file");
    const std::size_t b = off + 8;
    const std::string_view magic(reinterpret_cast<const char*>(d.data() + off), 4);
    if (magic == "FINF" && size >= 8 + 24)
    {
      f.m_linefeed = static_cast<s8>(d[b + 1]);
      f.m_alter = BigU16(d, b + 2);
      f.m_default = {static_cast<s8>(d[b + 4]), d[b + 5], static_cast<s8>(d[b + 6])};
      f.m_ascent = d[b + 22];
      finf = true;
    }
    else if (magic == "TGLP" && size >= 8 + 24)
    {
      f.m_cell_w = d[b + 0];
      f.m_cell_h = d[b + 1];
      f.m_baseline = static_cast<s8>(d[b + 2]);
      f.m_sheet_size = BigU32(d, b + 4);
      f.m_sheet_count = BigU16(d, b + 8);
      f.m_sheet_format = BigU16(d, b + 10) & 0x7FFF;
      f.m_per_row = BigU16(d, b + 12);
      f.m_per_column = BigU16(d, b + 14);
      f.m_sheet_w = BigU16(d, b + 16);
      f.m_sheet_h = BigU16(d, b + 18);
      f.m_sheet_data = BigU32(d, b + 20);
      tglp = true;
    }
    else if (magic == "CWDH" && size >= 8 + 8)
    {
      const u16 begin = BigU16(d, b), end = BigU16(d, b + 2);
      if (end >= begin && b + 8 + (static_cast<std::size_t>(end - begin) + 1) * 3 <= off + size)
        f.m_widths.push_back({begin, end, b + 8});
    }
    else if (magic == "CMAP" && size >= 8 + 14)
    {
      f.m_maps.push_back({BigU16(d, b), BigU16(d, b + 2), BigU16(d, b + 4), b + 12});
      const Map& m = f.m_maps.back();
      const std::size_t need =
          m.method == 0 ? 2 :
          m.method == 1 ? (m.end >= m.begin ? (static_cast<std::size_t>(m.end - m.begin) + 1) * 2 :
                                              0) :
          m.method == 2 ? 2 + static_cast<std::size_t>(BigU16(d, m.info)) * 4 :
                          0;
      if (m.method > 2 || m.info + need > off + size)
        f.m_maps.pop_back();
    }
    off += size;
  }
  if (!finf || !tglp)
    return refuse("no FINF or TGLP");
  if (f.m_cell_w <= 0 || f.m_cell_h <= 0 || f.m_per_row <= 0 || f.m_per_column <= 0 ||
      f.m_sheet_count <= 0 || f.m_sheet_format > 3 ||
      f.m_per_row * (f.m_cell_w + 1) > f.m_sheet_w + 1 ||
      f.m_per_column * (f.m_cell_h + 1) > f.m_sheet_h + 1 ||
      f.m_sheet_data + static_cast<std::size_t>(f.m_sheet_size) * f.m_sheet_count > file_size)
  {
    return refuse("glyph sheets out of range");
  }
  f.m_glyphs = static_cast<std::size_t>(f.m_sheet_count) * f.m_per_row * f.m_per_column;
  f.m_data = std::move(data);
  return f;
}

u16 Rfnt::ReadU16(std::size_t at) const
{
  return BigU16(m_data, at);
}

std::optional<u16> Rfnt::IndexOf(char32_t code) const
{
  for (const Map& m : m_maps)
  {
    if (code < m.begin || code > m.end)
      continue;
    std::optional<u16> index;
    const u32 k = static_cast<u32>(code) - m.begin;
    if (m.method == 0)
      index = static_cast<u16>(ReadU16(m.info) + k);
    else if (m.method == 1)
      index = ReadU16(m.info + k * 2);
    else
    {
      const u16 n = ReadU16(m.info);
      for (u16 i = 0; i < n; ++i)
      {
        if (ReadU16(m.info + 2 + i * 4u) == code)
        {
          index = ReadU16(m.info + 4 + i * 4u);
          break;
        }
      }
    }
    if (index && *index != 0xFFFF && *index < m_glyphs)
      return index;
  }
  return std::nullopt;
}

Widths Rfnt::WidthsOf(u16 index) const
{
  for (const WidthBlock& w : m_widths)
  {
    if (index < w.begin || index > w.end)
      continue;
    const std::size_t at = w.table + static_cast<std::size_t>(index - w.begin) * 3;
    return {static_cast<s8>(m_data[at]), m_data[at + 1], static_cast<s8>(m_data[at + 2])};
  }
  return m_default;
}

const std::vector<u8>& Rfnt::Sheet(int sheet) const
{
  auto it = m_sheets.find(sheet);
  if (it != m_sheets.end())
    return it->second;
  std::vector<u8> coverage;
  const std::span<const u8> src(m_data.data() + m_sheet_data +
                                    static_cast<std::size_t>(m_sheet_size) * sheet,
                                m_sheet_size);
  if (!DecodeCoverage(m_sheet_format, m_sheet_w, m_sheet_h, src, &coverage))
    coverage.assign(static_cast<std::size_t>(m_sheet_w) * m_sheet_h, 0);
  return m_sheets.emplace(sheet, std::move(coverage)).first->second;
}

bool Rfnt::GlyphAlpha(u16 index, std::vector<u8>* out) const
{
  if (index >= m_glyphs)
    return false;
  const int per_sheet = m_per_row * m_per_column;
  const int sheet = index / per_sheet;
  const int k = index % per_sheet;
  const int x0 = (k % m_per_row) * (m_cell_w + 1);
  const int y0 = (k / m_per_row) * (m_cell_h + 1);
  const std::vector<u8>& s = Sheet(sheet);
  out->assign(static_cast<std::size_t>(m_cell_w) * m_cell_h, 0);
  for (int y = 0; y < m_cell_h && y0 + y < m_sheet_h; ++y)
  {
    for (int x = 0; x < m_cell_w && x0 + x < m_sheet_w; ++x)
    {
      (*out)[static_cast<std::size_t>(y) * m_cell_w + x] =
          s[static_cast<std::size_t>(y0 + y) * m_sheet_w + x0 + x];
    }
  }
  return true;
}

std::vector<std::optional<Rfnt>> ReadFromDisc(const std::string& disc,
                                              std::span<const char* const> files,
                                              std::string_view game_id, std::string* error)
{
  constexpr u64 MAX_FILE = 8u << 20;
  std::vector<std::optional<Rfnt>> fonts(files.size());
  const auto say = [error](std::string why) {
    if (error)
      *error = std::move(why);
  };
  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(disc);
  if (!volume)
  {
    say(fmt::format("can't open {}", disc));
    return fonts;
  }
  const DiscIO::Partition partition = volume->GetGamePartition();
  const DiscIO::FileSystem* fs = volume->GetFileSystem(partition);
  if (!fs || volume->GetGameID(partition) != game_id)
  {
    say(fmt::format("{} is not {}", disc, game_id));
    return fonts;
  }
  for (std::size_t i = 0; i < files.size(); ++i)
  {
    const std::unique_ptr<DiscIO::FileInfo> info = fs->FindFileInfo(files[i]);
    if (!info || info->GetSize() == 0 || info->GetSize() > MAX_FILE)
    {
      say(fmt::format("{}: not on the disc", files[i]));
      continue;
    }
    std::vector<u8> bytes(info->GetSize());
    if (DiscIO::ReadFile(*volume, partition, info.get(), bytes.data(), bytes.size()) != bytes.size())
    {
      say(fmt::format("{}: unreadable", files[i]));
      continue;
    }
    if (bytes[0] == 0x10)
    {
      std::optional<std::vector<u8>> unpacked = DecompressLz10(bytes);
      if (!unpacked)
      {
        say(fmt::format("{}: bad LZ77", files[i]));
        continue;
      }
      bytes = std::move(*unpacked);
    }
    std::string why;
    fonts[i] = Rfnt::Parse(std::move(bytes), &why);
    if (!fonts[i])
      say(fmt::format("{}: {}", files[i], why));
  }
  return fonts;
}

std::vector<char32_t> OverlayCodePoints()
{
  std::vector<char32_t> codes;
  for (char32_t c = 0x20; c < 0x7F; ++c)
    codes.push_back(c);
  for (char32_t c = 0xA0; c <= 0xFF; ++c)
    codes.push_back(c);
  for (const char32_t c : {0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2025,
                           0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x30FB})
  {
    codes.push_back(c);
  }
  return codes;
}

std::u32string Substitute(char32_t code)
{
  switch (code)
  {
  case 0xB7:  // middle dot: try the katakana one, then a bullet
    return U"・";
  case 0x30FB:
    return U"•";
  case 0x2022:
    return U"-";
  case 0x2013:
  case 0x2014:
    return U"‐";
  case 0x2010:
  case 0x2212:
    return U"-";
  case 0x2018:
  case 0x2019:
    return U"'";
  case 0x201C:
  case 0x201D:
    return U"\"";
  case 0x2026:
    return U"...";
  case 0xA0:
    return U" ";
  default:
    return {};
  }
}

Atlas BuildAtlas(const Rfnt& font, std::span<const char32_t> codes, int pad, int levels)
{
  Atlas atlas;
  levels = std::max(1, levels);
  const int align = 1 << (levels - 1);
  const auto round_up = [align](int v) { return (v + align - 1) / align * align; };
  const int cw = round_up(font.CellWidth() + 2 * pad);
  const int ch = round_up(font.CellHeight() + 2 * pad);
  std::vector<std::pair<char32_t, u16>> present;
  for (const char32_t c : codes)
  {
    if (const std::optional<u16> index = font.IndexOf(c))
      present.emplace_back(c, *index);
  }
  atlas.cell_height = static_cast<float>(font.CellHeight());
  atlas.ascent = static_cast<float>(font.Ascent());
  if (present.empty())
    return atlas;
  // Roughly square: the smallest power-of-two width whose rows fit within that height.
  const int count = static_cast<int>(present.size());
  int width = 256;
  while (width < 2048 && (count + width / cw - 1) / std::max(1, width / cw) * ch > width)
    width *= 2;
  const int per_row = std::max(1, width / cw);
  const int rows = (count + per_row - 1) / per_row;
  int height = align;
  while (height < rows * ch)
    height *= 2;
  atlas.width = width;
  atlas.height = height;
  std::vector<u8> rgba(static_cast<std::size_t>(width) * height * 4, 0);
  for (std::size_t i = 0; i < rgba.size(); i += 4)
    rgba[i] = rgba[i + 1] = rgba[i + 2] = 255;
  std::vector<u8> alpha;
  for (std::size_t n = 0; n < present.size(); ++n)
  {
    const auto [code, index] = present[n];
    const int x0 = static_cast<int>(n) % per_row * cw + pad;
    const int y0 = static_cast<int>(n) / per_row * ch + pad;
    if (!font.GlyphAlpha(index, &alpha))
      continue;
    for (int y = 0; y < font.CellHeight(); ++y)
    {
      for (int x = 0; x < font.CellWidth(); ++x)
      {
        rgba[(static_cast<std::size_t>(y0 + y) * width + x0 + x) * 4 + 3] =
            alpha[static_cast<std::size_t>(y) * font.CellWidth() + x];
      }
    }
    const Widths w = font.WidthsOf(index);
    Atlas::Glyph g;
    g.u0 = static_cast<float>(x0) / width;
    g.v0 = static_cast<float>(y0) / height;
    g.u1 = static_cast<float>(x0 + std::clamp(w.glyph, 0, font.CellWidth())) / width;
    g.v1 = static_cast<float>(y0 + font.CellHeight()) / height;
    g.left = static_cast<float>(w.left);
    g.width = static_cast<float>(std::clamp(w.glyph, 0, font.CellWidth()));
    g.advance = static_cast<float>(w.advance);
    atlas.glyphs.emplace(code, g);
  }
  if (const auto space = atlas.glyphs.find(U' '); space != atlas.glyphs.end())
    atlas.space = space->second.advance;
  else
    atlas.space = static_cast<float>(font.CellWidth()) / 3;
  atlas.levels.push_back(std::move(rgba));
  // Each mip texel averages the four below it.
  for (int level = 1; level < levels; ++level)
  {
    const int pw = width >> (level - 1), ph = height >> (level - 1);
    const int w2 = std::max(1, pw / 2), h2 = std::max(1, ph / 2);
    const std::vector<u8>& prev = atlas.levels.back();
    std::vector<u8> next(static_cast<std::size_t>(w2) * h2 * 4, 255);
    for (int y = 0; y < h2; ++y)
    {
      for (int x = 0; x < w2; ++x)
      {
        int sum = 0;
        for (int dy = 0; dy < 2; ++dy)
        {
          for (int dx = 0; dx < 2; ++dx)
          {
            const int sx = std::min(pw - 1, x * 2 + dx), sy = std::min(ph - 1, y * 2 + dy);
            sum += prev[(static_cast<std::size_t>(sy) * pw + sx) * 4 + 3];
          }
        }
        next[(static_cast<std::size_t>(y) * w2 + x) * 4 + 3] = static_cast<u8>((sum + 2) / 4);
      }
    }
    atlas.levels.push_back(std::move(next));
  }
  return atlas;
}
}  // namespace Orca::UX::GameFont
