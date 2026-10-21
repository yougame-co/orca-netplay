// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/MenuText.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"

namespace Orca::UX
{
namespace
{
using namespace std::string_view_literals;

// Colour codes: 0x12 0x02 0x0C r g b a, ended by 0x13. The literal is split before "333" because a
// hex escape would swallow the following hex digits.
// clang-format off
#define BRAWL "\x12\x02\x0c" "333\xff"
#define PPLUS "\x12\x02\x0c\xbf\xc7\xc4\xff"
#define RECORD "\x12\x02\x0c\xc0\xc0\xc0\xff"
#define END "\x13"
// clang-format on

// Originals verified byte for byte against each game's mu_menumain archive. Grouped by file so
// each file is looked up once per frame.
constexpr std::array<MenuTextEdit, 19> EDITS{{
    // Footer under the Online button.
    {5, 2, BRAWL "Play different modes while connected to " BRAWL "Nintendo WFC." END END ""sv,
     BRAWL "Play different modes online." END ""sv},
    {5, 2, PPLUS "Play different modes online. (Discontinued)" END ""sv,
     PPLUS "Play different modes online." END ""sv},
    // With Friends: there is no friend roster; it opens the Versus character select, where
    // invited friends drop in.
    {5, 18, BRAWL "Play with people you've registered as Friends!" END ""sv,
     BRAWL "Play with friends you invite! (2-4 players)" END ""sv},
    {5, 18, PPLUS "Play with people you've registered as Friends!" END ""sv,
     PPLUS "Play with friends you invite! (2-4 players)" END ""sv},
    // With Anyone (Project+ only).
    {5, 19, PPLUS "Play against random people! (Project M not compatible!)" END ""sv,
     PPLUS "Play against random people!" END ""sv},
    // With Anyone's first button, now Casual.
    {5, 51, BRAWL "Go online for a quick brawl with someone, somewhere. (No names)" END ""sv,
     BRAWL "Go online for a quick brawl with someone, somewhere." END ""sv},
    {5, 51, PPLUS "Go online for a quick fight with someone, somewhere. (No names)" END ""sv,
     PPLUS "Go online for a quick fight with someone, somewhere." END ""sv},
    // With Anyone's second button, now Ranked.
    {5, 52, BRAWL "Form a team with someone, somewhere for a 2-on-2 battle!" END ""sv,
     BRAWL "Brawl someone at your level for a higher rank!" END ""sv},
    {5, 52, PPLUS "Form a team with someone, somewhere for a 2-on-2 battle!" END ""sv,
     PPLUS "Fight someone at your level for a higher rank!" END ""sv},
    // The With Friends page's buttons. The page is no longer reachable, but every button would
    // lead to the Versus character select, so none may promise another mode or a roster.
    // Friend Roster.
    {5, 45, BRAWL "Register friends here to simplify playing with them." END ""sv,
     BRAWL "Friends you invite join your brawl! (2-4 players)" END ""sv},
    {5, 45, PPLUS "Register friends here to simplify playing with them." END ""sv,
     PPLUS "Friends you invite join your fight! (2-4 players)" END ""sv},
    // Team Multi-Man.
    {5, 47, BRAWL "Join forces with a friend to defeat enemies! (2 players)" END ""sv,
     BRAWL "Brawl with the friends you invite! (2-4 players)" END ""sv},
    {5, 47, PPLUS "Join forces with a friend to defeat enemies! (2 players)" END ""sv,
     PPLUS "Fight the friends you invite! (2-4 players)" END ""sv},
    // Home-Run Contest.
    {5, 48, BRAWL "Have an online Home-Run Contest with a friend! (2 players)" END ""sv,
     BRAWL "Take on your friends in a brawl! (2-4 players)" END ""sv},
    {5, 48, PPLUS "Have an online Home-Run Contest with a friend! (2 players)" END ""sv,
     PPLUS "Take on your friends in a fight! (2-4 players)" END ""sv},
    // Boss Battles (no button shows it, but rewrite it anyway).
    {5, 49, BRAWL "Take on 10 bosses with the help of a friend! (2 players)" END ""sv,
     BRAWL "Take on your friends in a brawl! (2-4 players)" END ""sv},
    {5, 49, PPLUS "Take on 10 bosses with the help of a friend! (2 players)" END ""sv,
     PPLUS "Take on your friends in a fight! (2-4 players)" END ""sv},
    // Data > Records (Brawl).
    {5, 78,
     BRAWL "View" BRAWL " " END BRAWL "g" END BRAWL "roup " END BRAWL "r" END BRAWL
           "ecords (" END BRAWL "e" END BRAWL "xcludes " END BRAWL "Nintendo WFC" END BRAWL
           " " END BRAWL "b" END BRAWL "rawls)" END BRAWL "." END END ""sv,
     BRAWL "View group records (excludes online brawls)." END ""sv},
    // A record's description (both games).
    {10, 48, RECORD "Total vs. matches, including Nintendo WFC matches." END ""sv,
     RECORD "Total vs. matches, including online matches." END ""sv},
}};

#undef BRAWL
#undef PPLUS
#undef RECORD
#undef END

constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x4;
constexpr u32 MENU_ARCHIVE = 0xAC0;      // muMenuMain's gfArchive*
constexpr u32 ARCHIVE_WORDS = 0x80 / 4;  // sizeof(gfArchive); one word points at the image
constexpr u32 ARC_MAGIC = 0x41524300;    // "ARC\0"
constexpr u32 ARC_COUNT = 0x6;           // u16 entry count
constexpr u32 ARC_NAME = 0x10;
constexpr u32 ARC_FIRST = 0x40;
constexpr u32 ENTRY_HEADER = 0x20;  // u16 type, u16 index, u32 size, ...; data follows
constexpr u16 TYPE_MISC = 1;
constexpr u32 MAX_ENTRIES = 64;
constexpr u32 MAX_MESSAGES = 1024;
// Description box offsets (see MenuText.h).
constexpr u32 MENU_PANEL = 0x7BC;
constexpr u32 PANEL_MESSAGE = 0x40;
constexpr u32 PANEL_WINDOW = 0x48;
constexpr u32 WINDOW_FILE = 0x4;
constexpr u32 WINDOW_TEXT = 0x8;
constexpr u32 TEXT_MSG = 0x9C;
constexpr u32 MSG_CAPACITY = 0x48;
constexpr u32 MSG_LENGTH = 0x4C;
constexpr u32 MSG_BUFFER = 0x50;
constexpr u32 MAX_CAPACITY = 0x1000;

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
}  // namespace

std::span<const MenuTextEdit> MenuTextEdits()
{
  return EDITS;
}

u32 FindMenuArchive(const GuestMemory& m)
{
  if (!Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return 0;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene) || !Pointer(m, scene + MENU_ARCHIVE) ||
      !StringAt(m, m.Read32(scene), "muMenuMain"))
  {
    return 0;
  }
  const u32 archive = m.Read32(scene + MENU_ARCHIVE);
  if (!Pointer(m, archive) || !m.Valid(archive + ARCHIVE_WORDS * 4 - 1))
    return 0;
  // Find the gfArchive word that points at the menu's ARC image.
  for (u32 i = 0; i < ARCHIVE_WORDS; ++i)
  {
    const u32 image = m.Read32(archive + 4 * i);
    if (image % 32 == 0 && Pointer(m, image) && m.Valid(image + ARC_FIRST - 1) &&
        m.Read32(image) == ARC_MAGIC && StringAt(m, image + ARC_NAME, "mu_menumain_en"))
    {
      return image;
    }
  }
  return 0;
}

u32 FindArchiveFile(const GuestMemory& m, u32 archive, u32 file)
{
  if (!Pointer(m, archive) || !m.Valid(archive + ARC_FIRST - 1))
    return 0;
  const u32 count = m.Read16(archive + ARC_COUNT);
  u32 entry = archive + ARC_FIRST;
  for (u32 i = 0; i < count && i < MAX_ENTRIES; ++i)
  {
    if (!Pointer(m, entry) || !m.Valid(entry + ENTRY_HEADER - 1))
      return 0;
    const u16 type = m.Read16(entry);
    const u16 index = m.Read16(entry + 2);
    const u32 size = m.Read32(entry + 4);
    const u32 data = entry + ENTRY_HEADER;
    if (type == TYPE_MISC && index == file)
      return m.Valid(data) && (size == 0 || m.Valid(data + size - 1)) ? data : 0;
    // Entries are 32-byte aligned.
    const u64 next = (static_cast<u64>(data) + size + 31) & ~u64{31};
    if (next > 0xFFFFFFFFull)
      return 0;
    entry = static_cast<u32>(next);
  }
  return 0;
}

MenuDescription FindMenuDescription(const GuestMemory& m, u32 messages)
{
  if (!messages || !Pointer(m, SCENE_MANAGER))
    return {};
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return {};
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene) || !Pointer(m, scene + MENU_PANEL) ||
      !StringAt(m, m.Read32(scene), "muMenuMain"))
  {
    return {};
  }
  const u32 panel = m.Read32(scene + MENU_PANEL);
  if (!Pointer(m, panel) || !Pointer(m, panel + PANEL_WINDOW))
    return {};
  const u32 window = m.Read32(panel + PANEL_WINDOW);
  if (!Pointer(m, window) || !Pointer(m, window + WINDOW_TEXT) ||
      m.Read32(window + WINDOW_FILE) != messages)
  {
    return {};
  }
  const u32 text = m.Read32(window + WINDOW_TEXT);
  if (!Pointer(m, text) || !Pointer(m, text + TEXT_MSG))
    return {};
  const u32 msg = m.Read32(text + TEXT_MSG);
  if (!Pointer(m, msg) || !Pointer(m, msg + MSG_BUFFER))
    return {};
  const u32 capacity = m.Read32(msg + MSG_CAPACITY);
  const u32 length = m.Read32(msg + MSG_LENGTH);
  const u32 buffer = m.Read32(msg + MSG_BUFFER);
  if (capacity > MAX_CAPACITY || length == 0 || length > capacity || !m.Valid(buffer) ||
      !m.Valid(buffer + length - 1))
  {
    return {};
  }
  return {buffer, length, m.Read32(panel + PANEL_MESSAGE)};
}

int ApplyMenuDescription(GuestMemory& m, u32 messages, std::span<const MenuTextEdit> edits)
{
  const MenuDescription d = FindMenuDescription(m, messages);
  if (!d.buffer)
    return 0;
  std::vector<u8> now(d.length);
  for (u32 i = 0; i < d.length; ++i)
    now[i] = m.Read8(d.buffer + i);
  for (const MenuTextEdit& e : edits)
  {
    if (e.file != DESCRIPTION_FILE || e.message != d.message || e.original.size() > now.size())
      continue;
    const auto at = std::search(now.begin(), now.end(), e.original.begin(), e.original.end(),
                                [](u8 a, char b) { return a == static_cast<u8>(b); });
    if (at == now.end())
      continue;
    // Write what the game would have formatted from the rewritten message; the length is
    // unchanged.
    const u32 from = d.buffer + static_cast<u32>(at - now.begin());
    for (size_t i = 0; i < e.original.size(); ++i)
      m.Write8(from + static_cast<u32>(i), i < e.text.size() ? static_cast<u8>(e.text[i]) : 0);
    return 1;
  }
  return 0;
}

int ApplyMenuText(GuestMemory& m, std::span<const MenuTextEdit> edits, int* in_box)
{
  if (in_box)
    *in_box = 0;
  const u32 archive = FindMenuArchive(m);
  if (!archive)
    return 0;
  int written = 0;
  u32 file_for = ~0u, messages = 0;
  for (const MenuTextEdit& e : edits)
  {
    if (e.file != file_for)
    {
      file_for = e.file;
      messages = FindArchiveFile(m, archive, e.file);
    }
    if (!messages || !Pointer(m, messages))
      continue;
    // The first offset is where the text starts, so it also gives the table's length.
    const u32 count = m.Read32(messages) / 4;
    if (count < 2 || count > MAX_MESSAGES || e.message + 1 >= count ||
        !m.Valid(messages + 4 * (e.message + 1) + 3))
    {
      continue;
    }
    const u32 from = m.Read32(messages + 4 * e.message);
    const u32 to = m.Read32(messages + 4 * (e.message + 1));
    if (to < from || to - from != e.original.size() || e.text.size() > e.original.size() ||
        !m.Valid(messages + to - 1))
    {
      continue;
    }
    const u32 at = messages + from;
    bool same = true;
    for (size_t i = 0; i < e.original.size() && same; ++i)
      same = m.Read8(at + static_cast<u32>(i)) == static_cast<u8>(e.original[i]);
    if (!same)
      continue;
    for (size_t i = 0; i < e.original.size(); ++i)
      m.Write8(at + static_cast<u32>(i), i < e.text.size() ? static_cast<u8>(e.text[i]) : 0);
    ++written;
  }
  // Then fix a box that formatted a message before it was rewritten.
  const int box = ApplyMenuDescription(m, FindArchiveFile(m, archive, DESCRIPTION_FILE), edits);
  if (in_box)
    *in_box = box;
  return written + box;
}

void WriteMenuText(const Core::CPUThreadGuard& guard)
{
  // Brawl rev 2, directly or via a launcher profile (Project+); in mod mode, the mod's builtin while
  // its guard words hold: the module's answer (BrawlGame.h).
  if (!Builtin().Active())
    return;
  GuardMemory memory(guard);
  int in_box = 0;
  if (const int n = ApplyMenuText(memory, MenuTextEdits(), &in_box))
  {
    INFO_LOG_FMT(ROLLBACK, "Orca: {} main menu text(s) rewritten, {} of them the description box's",
                 n, in_box);
  }
}
}  // namespace Orca::UX
