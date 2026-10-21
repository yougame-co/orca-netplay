// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/CssTitle.h"

#include <array>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Core/Orca/Games/Brawl/MenuText.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"

namespace Orca::UX::CssTitle
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x4;
constexpr u32 MANAGER_SEQUENCE = 0x10;  // running sequence
constexpr u32 SCENE_ARCHIVE = 0x410;    // scSelctCharacter's gfArchive*
constexpr u32 ARCHIVE_WORDS = 0x80 / 4;
constexpr u32 SEQUENCE_PICK = 0x18;    // menu pick stored by Orca's patch (RSBE01.patches)
constexpr u32 ARC_MAGIC = 0x41524300;  // "ARC\0"
constexpr u32 ARC_NAME = 0x10;
constexpr u32 ARC_FIRST = 0x40;
constexpr u32 TITLE_FILE = 30;  // MiscData 30: character select's bres

constexpr u32 BRES_MAGIC = 0x62726573;  // "bres"
constexpr u32 BRES_ROOT = 0xC;          // u16 offset of the root section
constexpr u32 ROOT_GROUP = 0x8;         // root's index group
constexpr u32 GROUP_COUNT = 0x4;
constexpr u32 GROUP_ENTRIES = 0x8;  // 16-byte entries; the first is the group's own
constexpr u32 ENTRY_SIZE = 0x10;
constexpr u32 ENTRY_NAME = 0x8;  // offset from the group's start
constexpr u32 ENTRY_DATA = 0xC;
constexpr u32 MAX_ENTRIES = 1024;
constexpr u32 PLT0_MAGIC = 0x504C5430;  // "PLT0"
constexpr u32 PLT0_DATA = 0x10;
constexpr u32 PLT0_FORMAT = 0x18;
constexpr u32 PLT0_COUNT = 0x1C;
constexpr u32 FORMAT_RGB5A3 = 2;

constexpr std::string_view PALETTES = "Palettes(NW4R)";
constexpr std::string_view TITLE_TEXTURE = "MenSelchrPanelRule3.1";

// Known title textures, keyed by palette size: the background entry, its shipped value, and the
// direction a mark moves its blue (one step per title; alpha stays zero).
struct Variant
{
  u16 count;
  u16 entry;
  u16 game;
  int step;
};
constexpr std::array<Variant, 2> VARIANTS{{
    {96, 0, 0x0EEE, -1},   // Brawl's BRAWL (132x48)
    {256, 7, 0x0000, +1},  // Project+'s VERSUS (128x56)
}};

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

bool StringAt(const GuestMemory& m, u32 at, std::string_view want)
{
  for (size_t i = 0; i <= want.size(); ++i)
  {
    const u32 a = at + static_cast<u32>(i);
    if (!m.Valid(a) || m.Read8(a) != (i < want.size() ? static_cast<u8>(want[i]) : 0))
      return false;
  }
  return true;
}

// Scenes and sequences start with a pointer to their name.
bool Named(const GuestMemory& m, u32 object, std::string_view name)
{
  return Pointer(m, object) && StringAt(m, m.Read32(object), name);
}

// The scene manager's word at `offset`, or 0 if unreadable.
u32 ManagerWord(const GuestMemory& m, u32 offset)
{
  if (!Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  return Pointer(m, manager + offset) ? m.Read32(manager + offset) : 0;
}

// The character select's archive image, or 0 if it isn't loaded.
u32 FindArchive(const GuestMemory& m)
{
  const u32 scene = ManagerWord(m, MANAGER_SCENE);
  if (!Named(m, scene, "scSelctCharacter") || !Pointer(m, scene + SCENE_ARCHIVE))
    return 0;
  const u32 archive = m.Read32(scene + SCENE_ARCHIVE);
  if (!Pointer(m, archive) || !m.Valid(archive + ARCHIVE_WORDS * 4 - 1))
    return 0;
  for (u32 i = 0; i < ARCHIVE_WORDS; ++i)
  {
    const u32 image = m.Read32(archive + 4 * i);
    if (image % 32 == 0 && Pointer(m, image) && m.Valid(image + ARC_FIRST - 1) &&
        m.Read32(image) == ARC_MAGIC && StringAt(m, image + ARC_NAME, "sc_selcharacter_en"))
    {
      return image;
    }
  }
  return 0;
}

// Data of the entry named `name` in the bres index group at `group`, or 0.
u32 GroupEntry(const GuestMemory& m, u32 group, std::string_view name)
{
  if (!Pointer(m, group) || !m.Valid(group + GROUP_ENTRIES - 1))
    return 0;
  const u32 count = m.Read32(group + GROUP_COUNT);
  if (count > MAX_ENTRIES || !m.Valid(group + GROUP_ENTRIES + ENTRY_SIZE * (count + 1) - 1))
    return 0;
  for (u32 i = 1; i <= count; ++i)
  {
    const u32 entry = group + GROUP_ENTRIES + ENTRY_SIZE * i;
    const u32 at = group + m.Read32(entry + ENTRY_NAME);
    // A name is preceded by its length.
    if (!Pointer(m, at - 4) || m.Read32(at - 4) != name.size() || !StringAt(m, at, name))
      continue;
    return group + m.Read32(entry + ENTRY_DATA);
  }
  return 0;
}
}  // namespace

std::string_view TitleName(Title title)
{
  switch (title)
  {
  case Title::Casual:
    return "CASUAL";
  case Title::Ranked:
    return "RANKED";
  case Title::Friends:
    return "FRIENDS";
  case Title::Game:
    break;
  }
  return "the game's own";
}

u32 ReadPick(const GuestMemory& m)
{
  const u32 sequence = ManagerWord(m, MANAGER_SEQUENCE);
  if (!Named(m, sequence, "sqVsMelee") || !Pointer(m, sequence + SEQUENCE_PICK))
    return 0;
  const u32 code = m.Read32(sequence + SEQUENCE_PICK);
  return code >= 24 && code <= 31 ? code : 0;
}

Title Wanted(const GuestMemory& m, int players)
{
  const Rules::Header header = Rules::ReadHeader(m);
  if (header.present && header.mode == Rules::Mode::Casual)
    return Title::Casual;
  if (header.present && header.mode == Rules::Mode::Ranked)
    return Title::Ranked;
  const u32 pick = ReadPick(m);
  if (pick >= 24 && pick <= 27)
    return Title::Friends;
  if (players >= 2)
    return Title::Friends;
  if (pick == 30)
    return Title::Casual;
  if (pick == 31)
    return Title::Ranked;
  return Title::Game;
}

std::optional<Title> Slot::TitleOf(u16 value) const
{
  for (const Title t : {Title::Game, Title::Casual, Title::Ranked, Title::Friends})
  {
    if (ValueFor(t) == value)
      return t;
  }
  return std::nullopt;
}

Slot FindSlot(const GuestMemory& m)
{
  const u32 archive = FindArchive(m);
  if (!archive)
    return {};
  const u32 bres = FindArchiveFile(m, archive, TITLE_FILE);
  if (!Pointer(m, bres) || !m.Valid(bres + 0x10 - 1) || m.Read32(bres) != BRES_MAGIC)
    return {};
  const u32 root = bres + m.Read16(bres + BRES_ROOT);
  const u32 palette = GroupEntry(m, GroupEntry(m, root + ROOT_GROUP, PALETTES), TITLE_TEXTURE);
  if (!Pointer(m, palette) || !m.Valid(palette + 0x20 - 1) || m.Read32(palette) != PLT0_MAGIC ||
      m.Read32(palette + PLT0_FORMAT) != FORMAT_RGB5A3)
  {
    return {};
  }
  const u16 count = m.Read16(palette + PLT0_COUNT);
  const u32 data = palette + m.Read32(palette + PLT0_DATA);
  for (const Variant& v : VARIANTS)
  {
    if (count != v.count || !m.Valid(data + 2 * v.entry + 1) || (data + 2 * v.entry) % 2)
      continue;
    Slot slot{data + 2 * v.entry, v.game, v.step};
    if (slot.TitleOf(m.Read16(slot.address)))
      return slot;
  }
  return {};
}

int Apply(GuestMemory& m, int players, Title* shown)
{
  if (shown)
    *shown = Title::Game;
  const Slot slot = FindSlot(m);
  if (!slot.address)
    return 0;
  const Title want = Wanted(m, players);
  if (shown)
    *shown = want;
  const u16 value = slot.ValueFor(want);
  if (m.Read16(slot.address) == value)
    return 0;
  m.Write16(slot.address, value);
  return 1;
}

void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Orca::Events::PortInfo>& ports)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  int players = 0;
  for (const Orca::Events::PortInfo& p : ports)
  {
    if (p.port >= 0 && p.port < 4)
      ++players;
  }
  GuardMemory memory(guard);
  Title shown = Title::Game;
  // Log only on first runs; a resimulated frame repeats the write.
  if (Apply(memory, players, &shown) && !resimulating)
  {
    const Slot slot = FindSlot(memory);
    NOTICE_LOG_FMT(ROLLBACK, "Orca: the character select's title reads {} ({:08x} = {:04x})",
                   TitleName(shown), slot.address, memory.Read16(slot.address));
  }
}
}  // namespace Orca::UX::CssTitle
