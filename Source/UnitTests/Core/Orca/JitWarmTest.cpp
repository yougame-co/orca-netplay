// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <xxh3.h>

#include "Common/FileUtil.h"

#include "Core/Orca/JitWarm.h"

namespace
{
using Orca::JitWarm::Block;

Block MakeBlock(u32 effective, u64 hash, u16 when = 0, u8 age = 0)
{
  Block b;
  b.effective = effective;
  b.physical = effective & 0x1FFFFFFF;
  b.flags = 3;
  b.hash = hash;
  b.when = when;
  b.age = age;
  b.ranges = {{b.physical, b.physical + 0x20}, {b.physical + 0x40, b.physical + 0x48}};
  return b;
}

bool Same(const Block& a, const Block& b)
{
  return a.effective == b.effective && a.physical == b.physical && a.flags == b.flags &&
         a.hash == b.hash && a.when == b.when && a.age == b.age && a.ranges == b.ranges;
}
}  // namespace

TEST(OrcaJitWarm, FileRoundTrips)
{
  const std::vector<Block> blocks = {MakeBlock(0x80001000, 1, 5, 2), MakeBlock(0x80700000, 2, 0, 0),
                                     MakeBlock(0x81000000, 3, 600, 24)};
  const std::vector<u8> bytes = Orca::JitWarm::Encode(blocks);
  const auto decoded = Orca::JitWarm::Decode(bytes);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), blocks.size());
  for (std::size_t i = 0; i < blocks.size(); ++i)
    EXPECT_TRUE(Same((*decoded)[i], blocks[i])) << i;

  const auto empty = Orca::JitWarm::Decode(Orca::JitWarm::Encode({}));
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST(OrcaJitWarm, DamagedFileIsRefused)
{
  const std::vector<u8> bytes = Orca::JitWarm::Encode({MakeBlock(0x80001000, 1)});
  // Any byte changed, the file cut short or grown: refused (the checksum, then the layout).
  for (std::size_t i = 0; i < bytes.size(); ++i)
  {
    std::vector<u8> damaged = bytes;
    damaged[i] ^= 0x40;
    EXPECT_FALSE(Orca::JitWarm::Decode(damaged).has_value()) << i;
  }
  EXPECT_FALSE(Orca::JitWarm::Decode(std::span(bytes).first(bytes.size() - 1)).has_value());
  std::vector<u8> longer = bytes;
  longer.push_back(0);
  EXPECT_FALSE(Orca::JitWarm::Decode(longer).has_value());
  EXPECT_FALSE(Orca::JitWarm::Decode({}).has_value());
}

// A list is data anyone could have written (a player's cache file, a seed): Decode keeps only
// what the JIT could have recorded.
TEST(OrcaJitWarm, ImplausibleBlocksAreRefused)
{
  const auto decodes = [](const Block& b) {
    return Orca::JitWarm::Decode(Orca::JitWarm::Encode({MakeBlock(0x80001000, 1), b})).has_value();
  };
  Block b = MakeBlock(0x80002000, 2);
  EXPECT_TRUE(decodes(b));
  // MEM2 too.
  b = MakeBlock(0x90001000, 2);
  EXPECT_TRUE(decodes(b));

  b = MakeBlock(0x80002002, 2);  // a half-word address
  EXPECT_FALSE(decodes(b));
  b = MakeBlock(0x80002000, 2);
  b.ranges[1] = {0x2041, 0x2048};  // a range not on words
  EXPECT_FALSE(decodes(b));
  b = MakeBlock(0x80002000, 2);
  b.ranges[1] = {0x017FFFF0, 0x01800010};  // past the end of MEM1
  EXPECT_FALSE(decodes(b));
  b.ranges[1] = {0x02000000, 0x02000010};  // between MEM1 and MEM2
  EXPECT_FALSE(decodes(b));
  b.ranges[1] = {0x13FFFFF0, 0x14000010};  // past the end of MEM2
  EXPECT_FALSE(decodes(b));
  b = MakeBlock(0x80002000, 2);
  b.physical = 0xFFFFFFFC;  // nowhere, and no wrap-around
  EXPECT_FALSE(decodes(b));

  // More instructions than one JIT block holds (32,000), even in ranges of allowed sizes.
  b = MakeBlock(0x80002000, 2);
  b.ranges.clear();
  for (u32 i = 0; i < 2; ++i)
    b.ranges.emplace_back(0x10000 + i * 0x10000, 0x10000 + i * 0x10000 + 0xFA00);
  EXPECT_TRUE(decodes(b));  // exactly 32,000
  b.ranges.back().second += 4;
  EXPECT_FALSE(decodes(b));
}

TEST(OrcaJitWarm, OversizedFilesAreRefused)
{
  // A count the bytes can't hold is refused before anything is allocated for it.
  std::vector<u8> bytes = Orca::JitWarm::Encode({MakeBlock(0x80001000, 1)});
  bytes[8] = 0x40;  // count = 64, with a valid checksum
  const u64 checksum = XXH3_64bits(bytes.data(), bytes.size() - sizeof(u64));
  std::memcpy(bytes.data() + bytes.size() - sizeof(u64), &checksum, sizeof(checksum));
  EXPECT_FALSE(Orca::JitWarm::Decode(bytes).has_value());
  // Past MAX_FILE_BYTES: refused whatever it holds.
  std::vector<u8> huge(Orca::JitWarm::MAX_FILE_BYTES + 1);
  EXPECT_FALSE(Orca::JitWarm::Decode(huge).has_value());
}

TEST(OrcaJitWarm, HashFollowsWordsAndRanges)
{
  std::vector<u8> mem1(0x100000, 0), mem2(0x1000, 0);
  for (std::size_t i = 0; i < mem1.size(); ++i)
    mem1[i] = static_cast<u8>(i * 7);
  const std::vector<std::pair<u32, u32>> ranges = {{0x1000, 0x1020}, {0x1040, 0x1048}};
  const auto hash = Orca::JitWarm::HashRanges(mem1, mem2, ranges);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(Orca::JitWarm::HashRanges(mem1, mem2, ranges), hash);

  // A word inside a range changes the hash; one between the ranges doesn't.
  mem1[0x1030] ^= 1;
  EXPECT_EQ(Orca::JitWarm::HashRanges(mem1, mem2, ranges), hash);
  mem1[0x1044] ^= 1;
  EXPECT_NE(Orca::JitWarm::HashRanges(mem1, mem2, ranges), hash);
  mem1[0x1044] ^= 1;
  // The same bytes at other ranges are another block.
  EXPECT_NE(Orca::JitWarm::HashRanges(mem1, mem2, {{0x1000, 0x1028}, {0x1040, 0x1048}}), hash);

  // MEM2 is at physical 0x10000000; anything outside both is no block.
  EXPECT_TRUE(Orca::JitWarm::HashRanges(mem1, mem2, {{0x10000000, 0x10000010}}).has_value());
  EXPECT_FALSE(Orca::JitWarm::HashRanges(mem1, mem2, {{0x10000ff8, 0x10001008}}).has_value());
  EXPECT_FALSE(Orca::JitWarm::HashRanges(mem1, mem2, {{0xffff0, 0x100010}}).has_value());
  EXPECT_FALSE(Orca::JitWarm::HashRanges(mem1, {}, {{0x10000000, 0x10000010}}).has_value());
  EXPECT_FALSE(Orca::JitWarm::HashRanges(mem1, mem2, {}).has_value());
  EXPECT_FALSE(Orca::JitWarm::HashRanges(mem1, mem2, {{0x2000, 0x2000}}).has_value());
}

TEST(OrcaJitWarm, MergeAgesRefreshesAndAdds)
{
  std::vector<Block> known = {MakeBlock(0x80001000, 1, 50, 3), MakeBlock(0x80002000, 2, 10, 0),
                              MakeBlock(0x80003000, 3, 20, Orca::JitWarm::MAX_AGE)};
  // The first was found compiled this match; the others weren't.
  const std::vector<bool> seen = {true, false, false};
  // The match compiled the second itself earlier than recorded, and a new one (twice).
  std::vector<Block> learned = {MakeBlock(0x80002000, 2, 4), MakeBlock(0x80004000, 4, 7),
                                MakeBlock(0x80004000, 4, 9)};
  const std::vector<Block> merged =
      Orca::JitWarm::Merge(std::move(known), seen, std::move(learned));
  ASSERT_EQ(merged.size(), 3u);  // the third aged past MAX_AGE
  EXPECT_EQ(merged[0].effective, 0x80001000u);
  EXPECT_EQ(merged[0].age, 0);
  EXPECT_EQ(merged[0].when, 50);
  EXPECT_EQ(merged[1].effective, 0x80002000u);
  EXPECT_EQ(merged[1].age, 0);
  EXPECT_EQ(merged[1].when, 4);
  EXPECT_EQ(merged[2].effective, 0x80004000u);
  EXPECT_EQ(merged[2].age, 0);
  EXPECT_EQ(merged[2].when, 7);

  // Not seen and not learned: one match older.
  const std::vector<Block> aged = Orca::JitWarm::Merge({MakeBlock(0x80005000, 5, 0, 1)}, {}, {});
  ASSERT_EQ(aged.size(), 1u);
  EXPECT_EQ(aged[0].age, 2);

  // Other words at the same address are another block, kept beside the first.
  const std::vector<Block> both =
      Orca::JitWarm::Merge({MakeBlock(0x80006000, 6)}, {false}, {MakeBlock(0x80006000, 7)});
  EXPECT_EQ(both.size(), 2u);
}

TEST(OrcaJitWarm, MergeKeepsTheYoungestPastTheCap)
{
  std::vector<Block> known;
  known.reserve(Orca::JitWarm::MAX_BLOCKS + 10);
  for (u32 i = 0; i < Orca::JitWarm::MAX_BLOCKS + 10; ++i)
  {
    known.push_back(MakeBlock(0x80000000 + i * 4, i, static_cast<u16>(i % 100),
                              static_cast<u8>(i < 10 ? 20 : 1)));
  }
  const std::vector<Block> merged = Orca::JitWarm::Merge(std::move(known), {}, {});
  ASSERT_EQ(merged.size(), Orca::JitWarm::MAX_BLOCKS);
  for (const Block& b : merged)
    EXPECT_EQ(b.age, 2);  // the ten oldest went
}

TEST(OrcaJitWarm, PacedTargetMeetsEveryDeadline)
{
  using Orca::JitWarm::PacedTarget;
  // Project+'s shape: 600 blocks first run in frame 0, 17,000 more by frame 24, 1,200 in frame 34.
  const std::vector<std::pair<int, std::size_t>> due = {{0, 600}, {24, 17'600}, {34, 18'800}};
  constexpr int LEAD = 3;
  // At frame 0 the 600 frame-0 blocks are already late, and 17,600 by boundary 21 (24 - LEAD) is
  // 800 per boundary, which covers them.
  EXPECT_EQ(PacedTarget(due, 0, 0, LEAD), 800u);
  // Walked boundary by boundary at exactly the pace, every deadline holds.
  std::size_t cursor = 0;
  for (int frame = 0; frame <= 40; ++frame)
  {
    cursor = PacedTarget(due, cursor, frame, LEAD);
    for (const auto& [when, end] : due)
    {
      if (frame >= when - LEAD)
        EXPECT_GE(cursor, end) << "frame " << frame << ", when " << when;
    }
  }
  EXPECT_EQ(cursor, 18'800u);
  // Far behind: what is late is due at once (the next deadline then has one boundary left).
  EXPECT_EQ(PacedTarget(due, 100, 30, LEAD), 17'600u);
  EXPECT_EQ(PacedTarget(due, 17'600, 31, LEAD), 18'800u);
  EXPECT_EQ(PacedTarget(due, 100, 40, LEAD), 18'800u);
  // Nothing left, or nothing listed: no work.
  EXPECT_EQ(PacedTarget(due, 18'800, 5, LEAD), 18'800u);
  EXPECT_EQ(PacedTarget({}, 0, 5, LEAD), 0u);
  // Brawl's shape (30,000 by frame 165) asks little of each boundary: the budget does the rest.
  EXPECT_EQ(PacedTarget(std::vector<std::pair<int, std::size_t>>{{165, 30'000}}, 0, 0, LEAD), 185u);
}

// The seed lists Orca ships for players without their own (Data/Sys/Orca/<profile>.jitwarm): they
// decode, are large enough to cover a match start, and include blocks first run while loading.
TEST(OrcaJitWarm, ShippedSeedListsRead)
{
  // The source tree's Data/Sys/Orca, found from this file (tests run from the build directory).
  const auto dir = std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/Orca";
  for (const char* name : {"RSBE01.jitwarm", "PPLUS32.jitwarm"})
  {
    std::string bytes;
    ASSERT_TRUE(File::ReadFileToString((dir / name).string(), bytes)) << name;
    const auto blocks =
        Orca::JitWarm::Decode(std::span(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
    ASSERT_TRUE(blocks.has_value()) << name;
    EXPECT_GT(blocks->size(), 20'000u) << name;
    EXPECT_LE(blocks->size(), Orca::JitWarm::MAX_BLOCKS) << name;
    bool early = false;
    for (const Block& b : *blocks)
      early = early || b.when < 30;
    EXPECT_TRUE(early) << name;
  }
}
