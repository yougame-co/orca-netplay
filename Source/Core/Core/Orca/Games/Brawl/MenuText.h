// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// Rewrites the main menu's descriptions of online play so none of them mention Nintendo WFC or
// promise modes Orca doesn't have (e.g. "Play different modes online."). See ORCA.md, "Online
// menu". Labels drawn as textures are handled by Orca's texture pack instead.
//
// The text lives in the main menu's archive (Brawl rev 2 and Project+): scene manager 0x805A0060
// +4 is muMenuMain, whose +0xAC0 is its gfArchive. MiscData 5 (menu descriptions) and 10 (record
// descriptions) are message files: a table of u32 offsets, then single-byte text with inline
// colour codes.
//
// A message is only rewritten while its bytes exactly match the game's, in place and never longer
// (zero-padded). It is a pure function of memory applied every frame on both machines, before
// snapshots, so rollback state always includes it. Covered by UX kCompatVersion.
//
// The description box draws from its own formatted copy. When the menu opens straight onto a page
// (backing out of an online character select), it formats the first description in the same frame
// the archive loads, before we can rewrite it, so the box's copy is patched too: muMenuMain +0x7BC
// is the panel, +0x40 the shown message, +0x48 its window; the window's +0x8 -> +0x9C is the text
// buffer (capacity +0x48, length +0x4C, data +0x50).
namespace Orca::UX
{
class GuestMemory;

struct MenuTextEdit
{
  u32 file;                   // MiscData index in the archive
  u32 message;                // message index in that file
  std::string_view original;  // the game's original bytes
  std::string_view text;      // replacement, no longer than the original (zero-padded)
};

// Edits for both games; each one only matches its own game's original.
std::span<const MenuTextEdit> MenuTextEdits();

// The main menu's archive image, or 0 if it isn't loaded.
u32 FindMenuArchive(const GuestMemory& memory);
// Address of MiscData `file` in that image, or 0.
u32 FindArchiveFile(const GuestMemory& memory, u32 archive, u32 file);
// The menu descriptions' message file, which the description box reads from.
constexpr u32 DESCRIPTION_FILE = 5;

// The description box's formatted copy of its current message. `buffer` is 0 if the box isn't
// up or doesn't print from `messages`.
struct MenuDescription
{
  u32 buffer = 0;
  u32 length = 0;
  u32 message = 0;  // index of the message shown
};
MenuDescription FindMenuDescription(const GuestMemory& memory, u32 messages);
// Rewrites the box's copy if it holds an edited original. Returns 1 if it did.
int ApplyMenuDescription(GuestMemory& memory, u32 messages, std::span<const MenuTextEdit> edits);
// Rewrites the messages, then the box's copy. Returns the number rewritten; `in_box` gets how many
// of those were the box's copy.
int ApplyMenuText(GuestMemory& memory, std::span<const MenuTextEdit> edits,
                  int* in_box = nullptr);

// Per-frame hook (Brawl rev 2 and Project+ only).
void WriteMenuText(const Core::CPUThreadGuard& guard);
}  // namespace Orca::UX
