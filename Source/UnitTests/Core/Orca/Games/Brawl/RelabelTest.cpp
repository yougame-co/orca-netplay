// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Texture relabels (UX/Relabel.h): the marks' values, which label a stage turn names, where the
// marks live (READY TO FIGHT!'s palette in the character select's archive, the stage select's
// textures in its own), and that a mark is written only over the game's value or another mark.

#include <initializer_list>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/Games/Brawl/Relabel.h"

using namespace Orca::UX;
using namespace Orca::UX::Relabel;
namespace Block = Orca::UX::MatchBlock;

namespace
{
class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    ++writes;
  }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void Fill(u32 a, u32 n, u8 v = 0)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = v;
  }
  void Put32(u32 a, u32 v)
  {
    for (int i = 0; i < 4; ++i)
      bytes[a + i] = static_cast<u8>(v >> (24 - 8 * i));
  }
  void Put16(u32 a, u16 v)
  {
    bytes[a] = static_cast<u8>(v >> 8);
    bytes[a + 1] = static_cast<u8>(v);
  }
  void PutString(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  std::map<u32, u8> bytes;
  int writes = 0;
};

// Where the fake game keeps things (the shapes are the game's, the addresses made up).
constexpr u32 MANAGER = 0x805B8BA0;
constexpr u32 SCENE = 0x90FF6340;
constexpr u32 NAMES = 0x80701000;
constexpr u32 ARCHIVE_OBJECT = 0x934CDD80;
constexpr u32 IMAGE = 0x92F67FA0;  // 32-byte aligned
constexpr u32 BRES = IMAGE + 0x60;
constexpr u32 ROOT = BRES + 0x10;
constexpr u32 TEXTURES = BRES + 0x100;
constexpr u32 PALETTES = BRES + 0x200;
constexpr u32 RESOURCES = BRES + 0x300;  // each a 0x40 header and its data
constexpr u32 RESOURCE_SIZE = 0x80;

// A bres index group at `group` with the named entries (data offsets from the group), the names
// written after it, each preceded by its length as the format has it.
void PutGroup(FakeMemory& m, u32 group, const std::vector<std::pair<std::string_view, u32>>& e)
{
  const u32 count = static_cast<u32>(e.size());
  m.Fill(group, 8 + 16 * (count + 1));
  m.Put32(group, 8 + 16 * (count + 1));
  m.Put32(group + 4, count);
  u32 names = group + 8 + 16 * (count + 1);
  u32 i = 1;
  for (const auto& [name, data] : e)
  {
    m.Put32(names, static_cast<u32>(name.size()));
    m.PutString(names + 4, name);
    const u32 entry = group + 8 + 16 * i++;
    m.Put32(entry + 8, names + 4 - group);
    m.Put32(entry + 12, data);
    names = (names + 4 + static_cast<u32>(name.size()) + 1 + 3) & ~3u;
  }
}

struct Texture
{
  std::string_view name;
  u16 width, height;
  u32 format;
  std::vector<u8> first;  // its data's first bytes
};
struct Palette
{
  std::string_view name;
  std::vector<u16> entries;  // RGB5A3
};

// A scene named `scene` whose archive (at `slot` in the scene object) holds the image `image`
// with MiscData `file`: a bres with these textures and palettes.
FakeMemory Screen(std::string_view scene, u32 slot, std::string_view image, u16 file,
                  const std::vector<Texture>& textures, const std::vector<Palette>& palettes)
{
  FakeMemory m;
  m.Put32(0x805A0060, MANAGER);
  m.Fill(MANAGER, 0x300);
  m.Put32(MANAGER + 0x4, SCENE);
  m.PutString(NAMES, scene);
  m.Fill(SCENE, 0x800);
  m.Put32(SCENE, NAMES);
  m.Put32(SCENE + slot, ARCHIVE_OBJECT);
  // The gfArchive: one of its words is the image.
  m.Fill(ARCHIVE_OBJECT, 0x80);
  m.Put32(ARCHIVE_OBJECT, 0x934CDCE0);
  m.Put32(ARCHIVE_OBJECT + 0x20, IMAGE);
  // The image: one MiscData entry, the bres.
  const u32 count = static_cast<u32>(textures.size() + palettes.size());
  const u32 size = 0x300 + RESOURCE_SIZE * count;
  m.Fill(IMAGE, 0x60);
  m.Put32(IMAGE, 0x41524300);
  m.Put16(IMAGE + 6, 1);
  m.PutString(IMAGE + 0x10, image);
  m.Put16(IMAGE + 0x40, 1);
  m.Put16(IMAGE + 0x42, file);
  m.Put32(IMAGE + 0x44, size);
  m.Fill(BRES, size);
  m.Put32(BRES, 0x62726573);
  m.Put16(BRES + 0xC, 0x10);
  m.Put32(ROOT, 0x726F6F74);
  PutGroup(m, ROOT + 8,
           {{"Textures(NW4R)", TEXTURES - (ROOT + 8)}, {"Palettes(NW4R)", PALETTES - (ROOT + 8)}});
  u32 at = RESOURCES;
  std::vector<std::pair<std::string_view, u32>> tex_entries, plt_entries;
  for (const Texture& t : textures)
  {
    m.Put32(at, 0x54455830);
    m.Put32(at + 0x10, 0x40);
    m.Put16(at + 0x1C, t.width);
    m.Put16(at + 0x1E, t.height);
    m.Put32(at + 0x20, t.format);
    for (size_t i = 0; i < t.first.size(); ++i)
      m.bytes[at + 0x40 + static_cast<u32>(i)] = t.first[i];
    tex_entries.emplace_back(t.name, at - TEXTURES);
    at += RESOURCE_SIZE;
  }
  for (const Palette& p : palettes)
  {
    m.Put32(at, 0x504C5430);
    m.Put32(at + 0x10, 0x40);
    m.Put32(at + 0x18, 2);  // RGB5A3
    m.Put16(at + 0x1C, static_cast<u16>(p.entries.size()));
    for (size_t i = 0; i < p.entries.size(); ++i)
      m.Put16(at + 0x40 + 2 * static_cast<u32>(i), p.entries[i]);
    plt_entries.emplace_back(p.name, at - PALETTES);
    at += RESOURCE_SIZE;
  }
  PutGroup(m, TEXTURES, tex_entries);
  PutGroup(m, PALETTES, plt_entries);
  // The match block's memory, the game's dead code.
  m.Fill(Block::BASE, Block::SIZE, 0x3A);
  return m;
}

// READY TO FIGHT!'s palettes: Brawl's transparent 0x0222 at 0 (white 0x0FFF at 1, 0xFFFF at 14),
// Project+'s 0x0000 at 15 (0x9084 at 0).
std::vector<u16> BrawlBand()
{
  std::vector<u16> e(16, 0x8421);
  e[0] = 0x0222;
  e[1] = 0x0FFF;
  e[14] = 0xFFFF;
  return e;
}
std::vector<u16> PPlusBand()
{
  std::vector<u16> e(16, 0x8421);
  e[0] = 0x9084;
  e[15] = 0x0000;
  return e;
}

FakeMemory CharacterSelect(std::vector<u16> band)
{
  return Screen("scSelctCharacter", 0x410, "sc_selcharacter_en", 30,
                {{"MenSelchrReady01_2", 352, 36, 8, {0x11}}},
                {{"MenSelchrReady01_2", std::move(band)}});
}

// The stage select (Brawl's sizes, or Project+'s STAGE SELECT and its legend); STAGE SELECT's
// first byte the game's (Brawl's texel 1, Project+'s 0) unless given.
// `slot`: the scene word holding the archive (the game's +0x450, or another the scan finds).
FakeMemory StageSelect(bool pplus, int line_first = -1, u32 slot = 0x450)
{
  if (line_first < 0)
    line_first = pplus ? 0x0A : 0x1A;
  std::vector<Texture> textures{{"MenSelchrRtitle.30", 120, 24, 0, {0x05}}};
  if (pplus)
  {
    textures.push_back({"MenSelmapBack07", 189, 30, 0, {static_cast<u8>(line_first)}});
    textures.push_back({"CornerInfo", 245, 53, 0, {0x07}});
    textures.push_back({"CornerButtons", 150, 50, 3, {0x00, 0x00}});
  }
  else
  {
    textures.push_back({"MenSelmapBack07", 128, 16, 0, {static_cast<u8>(line_first)}});
  }
  return Screen("scSelStage", slot, "sc_selmap_en", 20, textures, {});
}

void PutHeader(FakeMemory& m, Block::Mode mode)
{
  m.Put32(Block::MAGIC, Block::MAGIC_VALUE);
  m.bytes[Block::VERSION] = Block::VERSION_VALUE;
  m.bytes[Block::MODE] = static_cast<u8>(mode);
  m.bytes[Block::RULESET] = 1;
}
}  // namespace

TEST(OrcaRelabel, TheMarksValues)
{
  // A palette entry steps by the label; a nibble wraps within its byte's high half and keeps the
  // low one; a byte steps.
  const Slot entry{Slot::Kind::PaletteEntry, 0, 0x0222, 3};
  EXPECT_EQ(entry.ValueFor(0, 0x0222), 0x0222);
  EXPECT_EQ(entry.ValueFor(2, 0x0222), 0x0224);
  EXPECT_EQ(entry.LabelOf(0x0223), 1);
  EXPECT_EQ(entry.LabelOf(0x0226), std::nullopt);
  const Slot nibble{Slot::Kind::HighNibble, 0, 1, LINE_PICK_A_STAGE};
  EXPECT_EQ(nibble.ValueFor(0, 0x1A), 0x1A);
  EXPECT_EQ(nibble.ValueFor(4, 0x1A), 0x5A);
  EXPECT_EQ(nibble.ValueFor(13, 0x5A), 0xEA);
  EXPECT_EQ(nibble.LabelOf(0xEA), 13);
  EXPECT_EQ(nibble.LabelOf(0xFA), std::nullopt);  // 15: no label's
  EXPECT_EQ(nibble.LabelOf(0x0A), std::nullopt);
  const Slot byte{Slot::Kind::Byte, 0, 0, 1};
  EXPECT_EQ(byte.ValueFor(1, 0), 1);
  EXPECT_EQ(byte.LabelOf(2), std::nullopt);
}

TEST(OrcaRelabel, StageTurnsAsTheLabelsNameThem)
{
  using Ranked::StepKind;
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Strike), 0, 1), 1);  // P1 STRIKES 1
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Strike), 0, 3), 3);
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Strike), 1, 2), 5);  // P2 STRIKES 2
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Strike), 1, 9), 6);  // never past 3
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Ban), 0, 1), 7);     // P1 BANS 1
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Ban), 1, 2), 10);    // P2 BANS 2
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Pick), 0, 1), 11);   // P1 PICKS
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Pick), 1, 1), 12);   // P2 PICKS
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Pick), Ranked::BOTH, 1), LINE_PICK_A_STAGE);
  EXPECT_EQ(LineFor(static_cast<u8>(StepKind::Prefer), Ranked::BOTH, 1), LINE_PICK_A_STAGE);
  EXPECT_EQ(LineFor(0, 0, 1), LINE_GAME);
}

TEST(OrcaRelabel, ReadyToFightsMarkIsItsPalettesTransparentEntry)
{
  const FakeMemory brawl = CharacterSelect(BrawlBand());
  const Slot b = FindBand(brawl);
  ASSERT_EQ(b.kind, Slot::Kind::PaletteEntry);
  EXPECT_EQ(brawl.Read16(b.address), 0x0222);
  const FakeMemory pplus = CharacterSelect(PPlusBand());
  const Slot p = FindBand(pplus);
  ASSERT_EQ(p.kind, Slot::Kind::PaletteEntry);
  EXPECT_EQ(p.address, b.address + 30);  // entry 15
  // A palette this doesn't know, or the entry holding anything but the game's value or a mark:
  // not the texture this knows.
  std::vector<u16> other = BrawlBand();
  other[0] = 0x1234;
  EXPECT_EQ(FindBand(CharacterSelect(other)).kind, Slot::Kind::None);
  other = BrawlBand();
  other[1] = 0x0EEE;
  EXPECT_EQ(FindBand(CharacterSelect(other)).kind, Slot::Kind::None);
  // Off the character select: none.
  FakeMemory off = CharacterSelect(BrawlBand());
  off.PutString(NAMES, "scSelStage");
  EXPECT_EQ(FindBand(off).kind, Slot::Kind::None);
}

TEST(OrcaRelabel, NothingCalledForPutsTheGamesWordsBack)
{
  // No queue room: the band is the game's; a mark left from before (label 2) goes, and only it.
  FakeMemory m = CharacterSelect(BrawlBand());
  const Slot band = FindBand(m);
  Shown shown;
  EXPECT_EQ(Apply(m, {}, &shown), 0);
  EXPECT_EQ(shown.band, Band::Game);
  EXPECT_FALSE(shown.band_up);
  m.Write16(band.address, 0x0224);
  EXPECT_EQ(Apply(m, {}, &shown), 1);
  EXPECT_EQ(m.Read16(band.address), 0x0222);
  const int writes = m.writes;
  EXPECT_EQ(Apply(m, {}, &shown), 0);
  EXPECT_EQ(m.writes, writes);
  EXPECT_FALSE(ClockInBand(m, {}));
}

TEST(OrcaRelabel, TheStageSelectsTitleSaysTheMode)
{
  for (const bool pplus : {false, true})
  {
    SCOPED_TRACE(pplus);
    FakeMemory m = StageSelect(pplus);
    const Slot title = FindTitle(m);
    ASSERT_EQ(title.kind, Slot::Kind::HighNibble);
    ASSERT_EQ(FindLine(m).kind, Slot::Kind::HighNibble);
    // No header: the game's.
    Shown shown;
    EXPECT_EQ(Apply(m, {}, &shown), 0);
    EXPECT_EQ(shown.title, Title::Game);
    // A ranked room's, then a casual one's: the first texel's high nibble, the low one kept.
    PutHeader(m, Block::Mode::Ranked);
    EXPECT_EQ(Apply(m, {}, &shown), 1);
    EXPECT_EQ(shown.title, Title::Ranked);
    EXPECT_EQ(m.Read8(title.address), 0x15);
    PutHeader(m, Block::Mode::Casual);
    Apply(m, {}, &shown);
    EXPECT_EQ(m.Read8(title.address), 0x25);
    EXPECT_EQ(shown.title, Title::Casual);
    // No turn under way (no stage flow): STAGE SELECT stays the game's; the legend too.
    EXPECT_EQ(shown.line, LINE_GAME);
    EXPECT_FALSE(shown.legend);
    EXPECT_EQ(Apply(m, {}, &shown), 0);
  }
}

TEST(OrcaRelabel, StageSelectsStaleMarkGoesAndTheLegendIsProjectPlusOwn)
{
  // Brawl's STAGE SELECT starts at 1 (its S's edge): a mark left from a turn (P2 STRIKES 1, 4)
  // goes back to 1 when no turn is under way.
  FakeMemory brawl = StageSelect(false, 0x5A);
  const Slot line = FindLine(brawl);
  ASSERT_EQ(line.kind, Slot::Kind::HighNibble);
  EXPECT_EQ(Apply(brawl, {}), 1);
  EXPECT_EQ(brawl.Read8(line.address), 0x1A);
  EXPECT_EQ(FindLegend(brawl)[0].kind, Slot::Kind::None);
  // Project+'s starts at 0, and has the legend: its words (I4) and its buttons (IA8's intensity
  // after a 0 alpha).
  const FakeMemory pplus = StageSelect(true, 0x0A);
  EXPECT_EQ(FindLine(pplus).kind, Slot::Kind::HighNibble);
  const std::array<Slot, 2> legend = FindLegend(pplus);
  EXPECT_EQ(legend[0].kind, Slot::Kind::HighNibble);
  EXPECT_EQ(legend[1].kind, Slot::Kind::Byte);
  EXPECT_EQ(legend[1].address, legend[0].address + RESOURCE_SIZE + 1);
  // A first texel the game never has: not the texture this knows.
  EXPECT_EQ(FindLine(StageSelect(false, 0x0A)).kind, Slot::Kind::None);
  // The archive in another word of the scene than the game's +0x450: the scan finds it.
  EXPECT_EQ(FindLine(StageSelect(false, -1, 0x1F0)).kind, Slot::Kind::HighNibble);
}
