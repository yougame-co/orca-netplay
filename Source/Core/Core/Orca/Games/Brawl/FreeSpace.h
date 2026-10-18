// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

// Emulated memory Orca can use for its own data and code caves in Brawl (RSBE01 rev 2) and
// Project+ v3.2. See ORCA.md, "Free space".
//
// It is all dead code in sora_scene.rel (module 1), at the same address in both games: the methods
// of Brawl's unused online sequences (sqNetAnyOkiraku, sqNetFriend*, sqNetAnyTeamMelee,
// sqNetAnyWatch). sora_scene stays loaded from boot, and Orca's menu patches redirect the only
// entries into these sequences, so nothing else ever runs or reads this code. Excluded:
//   - each sequence's create function, which runs at boot;
//   - words relocated against sora_melee (module 27), which the loader may rewrite;
//   - 0x806F3DA4, which Project+ nops, so caves stay identical in both games.
// Tools/orca/free-space.py verifies this by filling the ranges with illegal instructions and
// checking that matches still play out identically.
//
// Code caves are written once per boot as guarded patch groups (GamePatches.h). The match block is
// data only, written by the frame hook as a pure function of memory, and 32-byte aligned so it
// shares no cache line with a cave. Until written, it holds the original code, so readers check
// its magic. Rollback snapshots carry it like the rest of MEM1.
namespace Orca::UX::FreeSpace
{
struct Range
{
  u32 begin = 0;
  u32 end = 0;  // exclusive

  constexpr u32 Size() const { return end - begin; }
  constexpr bool Contains(u32 address) const { return address >= begin && address < end; }
  constexpr bool Overlaps(const Range& other) const
  {
    return begin < other.end && other.begin < end;
  }
};

// Where sora_scene.rel loads in both games; its text starts at +0xD4.
constexpr u32 kSoraSceneBase = 0x806BB480;
constexpr Range kSoraSceneText{0x806BB554, 0x806F8ADC};

// The match block (MatchBlock.h: header, steps, saved rules, cursors, queue, character order, the
// ranked set, friend drop-in), inside sqNetAnyOkiraku::setNext, all of it: up to the next relocated
// word, which its last field (FRIENDS_END) reaches. kMatchBlockHead is its first 512 bytes, the
// part the block's codecs count in (MatchBlock::SIZE); the character order starts where it ends.
// (Until S5 kMatchBlock was only the head, so free-space.py and CavesValid never covered the 152
// bytes after it.)
constexpr Range kMatchBlock{0x806F2380, 0x806F2618};
constexpr Range kMatchBlockHead{0x806F2380, 0x806F2580};
constexpr u32 kMatchBlockLimit = kMatchBlock.end;
constexpr u32 kMatchBlockOriginalWord = 0x3A200000;  // `li r17, 0`, at kMatchBlock.begin

// Code caves: runs of at least 500 bytes with no relocated words (16,908 bytes in total). All of
// MEM1 is within relative-branch range (+-32 MB).
constexpr std::array<Range, 12> kCodeCaves{{
    {0x806F2674, 0x806F2D24},  // sqNetAnyOkiraku::setNext, its states and exit (1,712 bytes)
    {0x806F2D74, 0x806F30E0},  // sqNetFriendList (876)
    {0x806F31F0, 0x806F34CC},  // sqNetFriendMelee (732)
    {0x806F352C, 0x806F3DA4},  // sqNetFriendMelee, up to Project+'s nop (2,168)
    {0x806F3DA8, 0x806F3FA0},  // sqNetFriendMelee, after it (504)
    {0x806F3FF0, 0x806F4468},  // sqNetFriendKumite (1,144)
    {0x806F45B0, 0x806F4C70},  // sqNetFriendKumite (1,728)
    {0x806F4CC0, 0x806F5180},  // sqNetFriendHomerun (1,216)
    {0x806F52E0, 0x806F58B4},  // sqNetFriendHomerun (1,492)
    {0x806F6C74, 0x806F7220},  // sqNetAnyTeamMelee (1,452)
    {0x806F7328, 0x806F76F8},  // sqNetAnyTeamMelee (976)
    {0x806F7748, 0x806F82A4},  // sqNetAnyWatch (2,908)
}};

// Each online sequence's create function (runs at boot, so not free).
constexpr std::array<Range, 7> kSequenceCreates{{
    {0x806F229C, 0x806F22EC},  // sqNetAnyOkiraku
    {0x806F2D24, 0x806F2D74},  // sqNetFriendList
    {0x806F30E0, 0x806F3130},  // sqNetFriendMelee
    {0x806F3FA0, 0x806F3FF0},  // sqNetFriendKumite
    {0x806F4C70, 0x806F4CC0},  // sqNetFriendHomerun
    {0x806F6C24, 0x806F6C74},  // sqNetAnyTeamMelee
    {0x806F76F8, 0x806F7748},  // sqNetAnyWatch
}};

// Words the loader relocates against sora_melee (module 27).
constexpr std::array<u32, 25> kSoraMeleeRelocations{
    0x806F2368, 0x806F2618, 0x806F2634, 0x806F2654, 0x806F2670, 0x806F31EC, 0x806F34CC,
    0x806F34E8, 0x806F350C, 0x806F3528, 0x806F4468, 0x806F4550, 0x806F456C, 0x806F4590,
    0x806F45AC, 0x806F5180, 0x806F5280, 0x806F529C, 0x806F52C0, 0x806F52DC, 0x806F7220,
    0x806F72CC, 0x806F72E8, 0x806F7308, 0x806F7324,
};

// Project+'s "Online Handicap Disable" (its codeset writes a nop here).
constexpr u32 kPPlusHandicapNop = 0x806F3DA4;

// Already used by Orca's online menu patch (sqMenuMain::setNext's old online entries).
constexpr Range kOnlineMenuBlock{0x806DC8C8, 0x806DC920};

namespace Detail
{
constexpr bool Free(const Range& range)
{
  if (range.begin % 4 != 0 || range.end % 4 != 0 || range.end <= range.begin ||
      !kSoraSceneText.Contains(range.begin) || !kSoraSceneText.Contains(range.end - 1) ||
      range.Overlaps(kOnlineMenuBlock) || range.Contains(kPPlusHandicapNop))
  {
    return false;
  }
  for (const Range& create : kSequenceCreates)
  {
    if (range.Overlaps(create))
      return false;
  }
  for (const u32 word : kSoraMeleeRelocations)
  {
    if (range.Contains(word))
      return false;
  }
  return true;
}

constexpr bool CavesValid()
{
  for (std::size_t i = 0; i < kCodeCaves.size(); ++i)
  {
    if (!Free(kCodeCaves[i]) || kCodeCaves[i].Overlaps(kMatchBlock))
      return false;
    if (i > 0 && kCodeCaves[i - 1].end > kCodeCaves[i].begin)
      return false;
  }
  return true;
}
}  // namespace Detail

// The block starts on a cache line and its last line holds no cave (only the relocated word, which
// is data the loader writes before Orca runs).
static_assert(kMatchBlockHead.Size() >= 96 && kMatchBlock.begin % 32 == 0 &&
              kMatchBlockHead.begin == kMatchBlock.begin && kMatchBlockHead.end % 32 == 0 &&
              kMatchBlockHead.end <= kMatchBlock.end);
static_assert(Detail::Free(kMatchBlock));
static_assert([] {
  for (const Range& cave : kCodeCaves)
  {
    if (cave.begin < ((kMatchBlock.end + 31) & ~31u) && cave.end > (kMatchBlock.begin & ~31u))
      return false;
  }
  return true;
}());
static_assert(Detail::CavesValid());
}  // namespace Orca::UX::FreeSpace
