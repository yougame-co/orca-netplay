// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The character select's mode title (ORCA.md "On-screen UI", UX/CssTitle.h): what
// it says in each room, where the mark lives (the title texture's palette in the character
// select's archive), and that the word is written only as one of the values this knows.

#include <map>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Orca/Games/Brawl/CssTitle.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"

using namespace Orca::UX::CssTitle;
namespace Block = Orca::UX::MatchBlock;

namespace
{
class FakeMemory final : public Orca::UX::GuestMemory
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
constexpr u32 SEQUENCE = 0x90FF4540;
constexpr u32 NAMES = 0x80701000;
constexpr u32 ARCHIVE_OBJECT = 0x934CDD80;
constexpr u32 IMAGE = 0x92F67FA0;  // 32-byte aligned
constexpr u32 BRES = IMAGE + 0x60;
constexpr u32 ROOT = BRES + 0x10;
constexpr u32 SUB_GROUP = BRES + 0x100;
constexpr u32 PLT0 = BRES + 0x200;
constexpr u32 PALETTE = PLT0 + 0x40;

struct Game
{
  u16 count;
  u16 entry;
  u16 value;
};
constexpr Game kBrawl{96, 0, 0x0EEE};
constexpr Game kPPlus{256, 7, 0x0000};

// A bres index group at `group` with the named entries (data offsets from the group), the names
// written after it, each preceded by its length as the format has it.
void PutGroup(FakeMemory& m, u32 group, std::initializer_list<std::pair<std::string_view, u32>> e)
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

// The character select with its archive loaded and Versus running, for `game`'s title texture,
// no match block (dead code) and the menu's pick `pick` kept in sqVsMelee.
FakeMemory CharacterSelect(const Game& game, u32 pick = 0xCCCCCCCC)
{
  FakeMemory m;
  m.Put32(0x805A0060, MANAGER);
  m.Fill(MANAGER, 0x300);
  m.Put32(MANAGER + 0x4, SCENE);
  m.Put32(MANAGER + 0x10, SEQUENCE);
  m.PutString(NAMES, "scSelctCharacter");
  m.PutString(NAMES + 0x20, "sqVsMelee");
  m.Fill(SCENE, 0x420);
  m.Put32(SCENE, NAMES);
  m.Put32(SCENE + 0x410, ARCHIVE_OBJECT);
  m.Fill(SEQUENCE, 0x20, 0xCC);
  m.Put32(SEQUENCE, NAMES + 0x20);
  m.Put32(SEQUENCE + 0x18, pick);
  // The gfArchive: one of its words is the image.
  m.Fill(ARCHIVE_OBJECT, 0x80);
  m.Put32(ARCHIVE_OBJECT, 0x934CDCE0);
  m.Put32(ARCHIVE_OBJECT + 0x20, IMAGE);
  // The image: one MiscData entry, 30, the bres.
  m.Fill(IMAGE, 0x60);
  m.Put32(IMAGE, 0x41524300);
  m.Put16(IMAGE + 6, 1);
  m.PutString(IMAGE + 0x10, "sc_selcharacter_en");
  m.Put16(IMAGE + 0x40, 1);
  m.Put16(IMAGE + 0x42, 30);
  m.Put32(IMAGE + 0x44, 0x200 + 0x40 + 2 * game.count);
  m.Fill(BRES, 0x240 + 2 * game.count);
  m.Put32(BRES, 0x62726573);
  m.Put16(BRES + 0xC, 0x10);
  m.Put32(ROOT, 0x726F6F74);
  PutGroup(m, ROOT + 8, {{"3DModels(NW4R)", 0x40}, {"Palettes(NW4R)", SUB_GROUP - (ROOT + 8)}});
  PutGroup(m, SUB_GROUP,
           {{"MenSelchrPanelRule3.2", 0x60}, {"MenSelchrPanelRule3.1", PLT0 - SUB_GROUP}});
  m.Put32(PLT0, 0x504C5430);
  m.Put32(PLT0 + 0x10, 0x40);
  m.Put32(PLT0 + 0x18, 2);  // RGB5A3
  m.Put16(PLT0 + 0x1C, game.count);
  for (u32 i = 0; i < game.count; ++i)
    m.Put16(PALETTE + 2 * i, static_cast<u16>(0x8000 | i));  // opaque colours
  m.Put16(PALETTE + 2 * game.entry, game.value);
  // The match block's memory, the game's dead code.
  m.Fill(Block::BASE, Block::SIZE, 0x3A);
  return m;
}

void PutHeader(FakeMemory& m, Block::Mode mode)
{
  m.Put32(Block::MAGIC, Block::MAGIC_VALUE);
  m.bytes[Block::VERSION] = Block::VERSION_VALUE;
  m.bytes[Block::MODE] = static_cast<u8>(mode);
  m.bytes[Block::RULESET] = 1;
}

u16 Word(const FakeMemory& m, const Game& game)
{
  return m.Read16(PALETTE + 2 * game.entry);
}
}  // namespace

TEST(OrcaCssTitle, WhatTheTitleSays)
{
  // Group > Brawl (the heap's fill in sqVsMelee's word), alone: the game's own.
  EXPECT_EQ(Wanted(CharacterSelect(kBrawl), 1), Title::Game);
  // The With Anyone picks, alone: the character select a search waits on.
  EXPECT_EQ(Wanted(CharacterSelect(kBrawl, 30), 1), Title::Casual);
  EXPECT_EQ(Wanted(CharacterSelect(kBrawl, 31), 1), Title::Ranked);
  // Every With Friends button, alone or not.
  for (u32 code = 24; code <= 27; ++code)
  {
    EXPECT_EQ(Wanted(CharacterSelect(kBrawl, code), 1), Title::Friends) << code;
    EXPECT_EQ(Wanted(CharacterSelect(kBrawl, code), 2), Title::Friends) << code;
  }
  // A friend plugged in with no queue header: Group > Brawl, or a pick, is a friends room.
  EXPECT_EQ(Wanted(CharacterSelect(kBrawl), 2), Title::Friends);
  EXPECT_EQ(Wanted(CharacterSelect(kBrawl, 30), 3), Title::Friends);
  // A queue room's header says it, whoever is plugged in and whatever was picked.
  for (const u32 pick : {0xCCCCCCCCu, 25u, 30u, 31u})
  {
    for (const int players : {1, 2})
    {
      FakeMemory casual = CharacterSelect(kBrawl, pick);
      PutHeader(casual, Block::Mode::Casual);
      EXPECT_EQ(Wanted(casual, players), Title::Casual) << pick << " " << players;
      FakeMemory ranked = CharacterSelect(kBrawl, pick);
      PutHeader(ranked, Block::Mode::Ranked);
      EXPECT_EQ(Wanted(ranked, players), Title::Ranked) << pick << " " << players;
      // A header with no mode (written, then cleared to none) says nothing.
      FakeMemory none = CharacterSelect(kBrawl, pick);
      PutHeader(none, Block::Mode::None);
      EXPECT_EQ(Wanted(none, players), Wanted(CharacterSelect(kBrawl, pick), players));
    }
  }
}

TEST(OrcaCssTitle, ThePickIsReadOnlyFromARunningVersus)
{
  EXPECT_EQ(ReadPick(CharacterSelect(kBrawl, 25)), 25u);
  EXPECT_EQ(ReadPick(CharacterSelect(kBrawl, 31)), 31u);
  // The heap's fill, a code the back out already used (0), the main menu's other codes.
  for (const u32 word : {0xCCCCCCCCu, 0u, 1u, 23u, 32u})
    EXPECT_EQ(ReadPick(CharacterSelect(kBrawl, word)), 0u) << word;
  // Another sequence running (Classic, Special Brawl...): no pick.
  FakeMemory other = CharacterSelect(kBrawl, 25);
  other.PutString(NAMES + 0x20, "sqSpMelee");
  EXPECT_EQ(ReadPick(other), 0u);
  EXPECT_EQ(Wanted(other, 1), Title::Game);
}

TEST(OrcaCssTitle, TheSlotIsTheTitlePalettesBackgroundEntry)
{
  for (const Game& game : {kBrawl, kPPlus})
  {
    SCOPED_TRACE(game.count);
    const Slot slot = FindSlot(CharacterSelect(game));
    EXPECT_EQ(slot.address, PALETTE + 2 * game.entry);
    EXPECT_EQ(slot.game, game.value);
  }
  // Not on the character select.
  FakeMemory menu = CharacterSelect(kBrawl);
  menu.PutString(NAMES, "muMenuMain");
  EXPECT_EQ(FindSlot(menu).address, 0u);
  // Another archive.
  FakeMemory other = CharacterSelect(kBrawl);
  other.PutString(IMAGE + 0x10, "sc_selcharacter2_en");
  EXPECT_EQ(FindSlot(other).address, 0u);
  // The palette's entry holds something else (a mod's texture): not one this knows.
  FakeMemory modded = CharacterSelect(kBrawl);
  modded.Put16(PALETTE, 0x1234);
  EXPECT_EQ(FindSlot(modded).address, 0u);
  // Another palette size.
  FakeMemory sized = CharacterSelect(kBrawl);
  sized.Put16(PLT0 + 0x1C, 80);
  EXPECT_EQ(FindSlot(sized).address, 0u);
  // Another palette format.
  FakeMemory format = CharacterSelect(kPPlus);
  format.Put32(PLT0 + 0x18, 1);
  EXPECT_EQ(FindSlot(format).address, 0u);
  // The texture under another name only.
  FakeMemory renamed = CharacterSelect(kPPlus);
  PutGroup(renamed, SUB_GROUP,
           {{"MenSelchrPanelRule3.2", 0x60}, {"MenSelchrPanelRule3.9", PLT0 - SUB_GROUP}});
  EXPECT_EQ(FindSlot(renamed).address, 0u);
}

TEST(OrcaCssTitle, EveryMarkIsATransparentColourOfItsOwn)
{
  for (const Game& game : {kBrawl, kPPlus})
  {
    const Slot slot = FindSlot(CharacterSelect(game));
    ASSERT_NE(slot.address, 0u);
    std::map<u16, Title> seen;
    for (const Title t : {Title::Game, Title::Casual, Title::Ranked, Title::Friends})
    {
      const u16 v = slot.ValueFor(t);
      // RGB5A3 with the top bit clear is 0AAARRRRGGGGBBBB: alpha zero.
      EXPECT_EQ(v & 0xF000, 0) << std::hex << v;
      // Only the blue moves.
      EXPECT_EQ(v & 0x0FF0, game.value & 0x0FF0) << std::hex << v;
      EXPECT_TRUE(seen.emplace(v, t).second);
      EXPECT_EQ(slot.TitleOf(v), t);
    }
  }
  // The marks the texture pack's names were made with (Tools/orca/textures/make-labels.py).
  const Slot brawl = FindSlot(CharacterSelect(kBrawl));
  EXPECT_EQ(brawl.ValueFor(Title::Casual), 0x0EED);
  EXPECT_EQ(brawl.ValueFor(Title::Ranked), 0x0EEC);
  EXPECT_EQ(brawl.ValueFor(Title::Friends), 0x0EEB);
  const Slot pplus = FindSlot(CharacterSelect(kPPlus));
  EXPECT_EQ(pplus.ValueFor(Title::Casual), 0x0001);
  EXPECT_EQ(pplus.ValueFor(Title::Ranked), 0x0002);
  EXPECT_EQ(pplus.ValueFor(Title::Friends), 0x0003);
}

TEST(OrcaCssTitle, ApplyWritesTheMarkOnceAndGivesTheGamesTitleBack)
{
  for (const Game& game : {kBrawl, kPPlus})
  {
    SCOPED_TRACE(game.count);
    FakeMemory m = CharacterSelect(game, 30);
    const Slot slot = FindSlot(m);
    Title shown = Title::Game;
    EXPECT_EQ(Apply(m, 1, &shown), 1);
    EXPECT_EQ(shown, Title::Casual);
    EXPECT_EQ(Word(m, game), slot.ValueFor(Title::Casual));
    // Idempotent: the same boundary again (a re-run) writes nothing.
    m.writes = 0;
    EXPECT_EQ(Apply(m, 1), 0);
    EXPECT_EQ(m.writes, 0);
    // A friend plugs in, with no queue header: FRIENDS.
    EXPECT_EQ(Apply(m, 2, &shown), 1);
    EXPECT_EQ(shown, Title::Friends);
    EXPECT_EQ(Word(m, game), slot.ValueFor(Title::Friends));
    // A ranked header.
    PutHeader(m, Block::Mode::Ranked);
    EXPECT_EQ(Apply(m, 2, &shown), 1);
    EXPECT_EQ(Word(m, game), slot.ValueFor(Title::Ranked));
    // Nothing calls for a mark any more (the back out used the pick, the header cleared): the
    // game's own value again.
    m.Put32(Block::MAGIC, 0x3A200000);
    m.Put32(SEQUENCE + 0x18, 0);
    EXPECT_EQ(Apply(m, 1, &shown), 1);
    EXPECT_EQ(shown, Title::Game);
    EXPECT_EQ(Word(m, game), game.value);
    m.writes = 0;
    EXPECT_EQ(Apply(m, 1), 0);
    EXPECT_EQ(m.writes, 0);
    // Only that one word ever changes.
    FakeMemory before = CharacterSelect(game, 25);
    FakeMemory after = before;
    EXPECT_EQ(Apply(after, 1), 1);
    for (const auto& [a, v] : before.bytes)
    {
      if (a != slot.address && a != slot.address + 1)
        EXPECT_EQ(after.bytes.at(a), v) << std::hex << a;
    }
  }
  // A word that isn't one of the values: never touched.
  FakeMemory modded = CharacterSelect(kBrawl, 30);
  modded.Put16(PALETTE, 0x1234);
  EXPECT_EQ(Apply(modded, 1), 0);
  EXPECT_EQ(modded.writes, 0);
  EXPECT_EQ(Word(modded, kBrawl), 0x1234);
  // Off the character select: nothing.
  FakeMemory menu = CharacterSelect(kBrawl, 30);
  menu.PutString(NAMES, "muMenuMain");
  EXPECT_EQ(Apply(menu, 1), 0);
  EXPECT_EQ(menu.writes, 0);
}
