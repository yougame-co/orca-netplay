// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/RangeSet.h"
#include "Core/Orca/PanicDiag.h"

using Orca::PanicDiag::DescribeWord;
using Orca::PanicDiag::View;

namespace
{
// Low MEM1 as two host views see it, big-endian like the guest's.
std::array<u8, 0x1000> Ram(u32 word_at_500)
{
  std::array<u8, 0x1000> ram{};
  ram[0x500] = static_cast<u8>(word_at_500 >> 24);
  ram[0x501] = static_cast<u8>(word_at_500 >> 16);
  ram[0x502] = static_cast<u8>(word_at_500 >> 8);
  ram[0x503] = static_cast<u8>(word_at_500);
  return ram;
}
}  // namespace

TEST(OrcaPanicDiag, ViewsThatAgree)
{
  const auto ram = Ram(0x7c7b43a6);
  const std::vector<View> views{{"ram", ram.data(), 0, 0x1000},
                                {"logical:80000000", ram.data(), 0, 0x1000}};
  EXPECT_EQ(DescribeWord(0x500, views),
            "orca diag word 00000500: ram=7c7b43a6 logical:80000000=7c7b43a6");
}

TEST(OrcaPanicDiag, ViewsThatDisagreeSaySo)
{
  const auto ram = Ram(0);
  const auto logical = Ram(0x7c7b43a6);
  const std::vector<View> views{{"ram", ram.data(), 0, 0x1000},
                                {"logical:80000000", logical.data(), 0, 0x1000}};
  EXPECT_EQ(DescribeWord(0x500, views),
            "orca diag word 00000500: ram=00000000 logical:80000000=7c7b43a6 differ");
}

TEST(OrcaPanicDiag, OnlyViewsThatCoverTheWord)
{
  const auto mem1 = Ram(0x12345678);
  const std::vector<View> views{{"exram", mem1.data(), 0x10000000, 0x1000},
                                {"ram", mem1.data(), 0, 0x1000},
                                {"short", mem1.data(), 0, 0x502},
                                {"null", nullptr, 0, 0x1000}};
  EXPECT_EQ(DescribeWord(0x500, views), "orca diag word 00000500: ram=12345678");
  EXPECT_EQ(DescribeWord(0x20000000, views), "orca diag word 20000000: no view");
}

TEST(OrcaPanicDiag, ReadsTheVectorsAndThreadGlobals)
{
  // The external-interrupt vector and the OS's current-thread pointer are what the Mac crash read
  // as zero.
  const auto& words = Orca::PanicDiag::WORDS;
  EXPECT_NE(std::find(words.begin(), words.end(), 0x500u), words.end());
  EXPECT_NE(std::find(words.begin(), words.end(), 0xE4u), words.end());
}

namespace
{
using Orca::PanicDiag::GuestRam;

// A small MEM1 (64 KiB) and MEM2 (4 KiB) image, big-endian words like the guest's.
struct Image
{
  std::vector<u8> mem1 = std::vector<u8>(0x10000);
  std::vector<u8> mem2 = std::vector<u8>(0x1000);

  GuestRam Ram() const { return GuestRam{mem1, mem2}; }
  void Put(u32 address, u32 value)
  {
    const u32 physical = address & 0x3FFFFFFF;
    u8* p = physical >= GuestRam::MEM2_PHYSICAL ? &mem2[physical - GuestRam::MEM2_PHYSICAL] :
                                                  &mem1[physical];
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
  }
};
}  // namespace

TEST(OrcaPanicDiag, GuestRamMapsTheDefaultBats)
{
  Image image;
  image.Put(0x80001000, 0x11223344);
  image.Put(0x90000010, 0x55667788);
  const GuestRam ram = image.Ram();
  EXPECT_EQ(ram.Word(0x80001000), 0x11223344u);
  EXPECT_EQ(ram.Word(0xC0001000), 0x11223344u);
  EXPECT_EQ(ram.Word(0x90000010), 0x55667788u);
  EXPECT_EQ(ram.Word(0xD0000010), 0x55667788u);
  EXPECT_EQ(ram.Word(0x80001002), std::nullopt);  // unaligned
  EXPECT_EQ(ram.Word(0x80010000), std::nullopt);  // past MEM1
  EXPECT_EQ(ram.Word(0x90001000), std::nullopt);  // past MEM2
  EXPECT_EQ(ram.Word(0x00001000), std::nullopt);  // physical, not a segment the OS maps
  EXPECT_EQ(ram.Word(0x00000000), std::nullopt);
  EXPECT_EQ(ram.Word(0xFFFFFFFC), std::nullopt);
}

TEST(OrcaPanicDiag, BackChainWalksToItsEnd)
{
  Image image;
  // r1 -> 0x8000F020 (saved LR 80012340) -> 0x8000F080 (saved LR 80023450) -> 0.
  image.Put(0x8000F000, 0x8000F020);
  image.Put(0x8000F020, 0x8000F080);
  image.Put(0x8000F024, 0x80012340);
  image.Put(0x8000F080, 0);
  image.Put(0x8000F084, 0x80023450);
  const auto chain = Orca::PanicDiag::WalkBackChain(image.Ram(), 0x8000F000);
  ASSERT_EQ(chain.frames.size(), 2u);
  EXPECT_EQ(chain.frames[0].back_chain, 0x8000F020u);
  EXPECT_EQ(chain.frames[0].saved_lr, 0x80012340u);
  EXPECT_EQ(chain.frames[1].saved_lr, 0x80023450u);
  EXPECT_EQ(chain.stop, "end");
  EXPECT_EQ(Orca::PanicDiag::DescribeBackChain(chain, 0x8000F000),
            std::vector<std::string>{
                "orca diag chain 8000f000: 8000f020>80012340 8000f080>80023450 end"});
}

TEST(OrcaPanicDiag, BackChainStopsOnAWildStackPointer)
{
  Image image;
  // r1 inside code, as in the 0x80558678 reports: its "back-chain" is an instruction.
  image.Put(0x80003000, 0x7c0802a6);
  auto chain = Orca::PanicDiag::WalkBackChain(image.Ram(), 0x80003000);
  EXPECT_TRUE(chain.frames.empty());
  EXPECT_EQ(chain.stop, "bad 7c0802a6");

  // A back-chain pointing down (a loop or garbage) ends the walk too.
  image.Put(0x8000F000, 0x8000E000);
  chain = Orca::PanicDiag::WalkBackChain(image.Ram(), 0x8000F000);
  EXPECT_EQ(chain.stop, "not above 8000e000");

  chain = Orca::PanicDiag::WalkBackChain(image.Ram(), 0x81000000);
  EXPECT_EQ(chain.stop, "sp not ram");
  chain = Orca::PanicDiag::WalkBackChain(image.Ram(), 0x8000F002);
  EXPECT_EQ(chain.stop, "sp not ram");
}

TEST(OrcaPanicDiag, BackChainDepthIsCapped)
{
  Image image;
  for (u32 sp = 0x80008000; sp < 0x80009000; sp += 0x10)
  {
    image.Put(sp, sp + 0x10);
    image.Put(sp + 4, 0x80001000 + sp % 0x100);
  }
  const auto chain = Orca::PanicDiag::WalkBackChain(image.Ram(), 0x80008000);
  EXPECT_EQ(chain.frames.size(), static_cast<size_t>(Orca::PanicDiag::MAX_CHAIN_DEPTH));
  EXPECT_EQ(chain.stop, "cap");
  const auto lines = Orca::PanicDiag::DescribeBackChain(chain, 0x80008000);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_TRUE(lines[0].starts_with("orca diag chain 80008000: 80008010>"));
  EXPECT_TRUE(lines[1].starts_with("orca diag chain+ 80008060>"));
  EXPECT_TRUE(lines[1].ends_with(" cap"));
}

TEST(OrcaPanicDiag, WordsAroundAnAddress)
{
  Image image;
  image.Put(0x8000FFF8, 0xAAAAAAAA);
  image.Put(0x8000FFFC, 0xBBBBBBBB);
  const auto lines = Orca::PanicDiag::DescribeWords(image.Ram(), "sp", 0x8000FFF8, 9);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "orca diag sp 8000fff8: aaaaaaaa bbbbbbbb -------- -------- -------- "
                      "-------- -------- --------");
  EXPECT_EQ(lines[1], "orca diag sp 80010018: --------");
}

TEST(OrcaPanicDiag, ModulesAndTheSectionHoldingAnAddress)
{
  Image image;
  // Two modules: id 27 at 80002000 (section 1 = 80003000+0x100, executable) and id 1 at 80004000.
  image.Put(Orca::PanicDiag::OS_MODULE_LIST, 0x80002000);
  image.Put(0x80002000, 27);
  image.Put(0x80002004, 0x80004000);
  image.Put(0x8000200C, 2);
  image.Put(0x80002010, 0x80002100);
  image.Put(0x80002108, 0x80003001);
  image.Put(0x8000210C, 0x100);
  image.Put(0x80004000, 1);
  image.Put(0x80004004, 0);
  const GuestRam ram = image.Ram();
  EXPECT_EQ(Orca::PanicDiag::ModuleOf(ram, 0x80003010), "m27s1+10");
  EXPECT_EQ(Orca::PanicDiag::ModuleOf(ram, 0x80003100), "-");
  EXPECT_EQ(Orca::PanicDiag::DescribeModules(ram), "orca diag mods: 27@80002000 1@80004000");
  EXPECT_EQ(Orca::PanicDiag::DescribeModules(Image{}.Ram()), "orca diag mods: none");
}

TEST(OrcaPanicDiag, CurrentContextAndThread)
{
  Image image;
  constexpr u32 THREAD = 0x80005000;
  image.Put(Orca::PanicDiag::OS_CURRENT_CONTEXT, THREAD);
  image.Put(Orca::PanicDiag::OS_CURRENT_THREAD, THREAD);
  image.Put(THREAD + 0x4, 0x8000F000);    // context r1
  image.Put(THREAD + 0x84, 0x80012340);   // lr
  image.Put(THREAD + 0x88, 0);            // ctr
  image.Put(THREAD + 0x198, 0x813365cc);  // srr0
  image.Put(THREAD + 0x19C, 0x00009032);  // srr1
  image.Put(THREAD + 0x1A0, 0x00000001);  // state: FPU
  image.Put(THREAD + 0x2C8, 0x00020000);  // running
  image.Put(THREAD + 0x2D0, 16);          // priority
  image.Put(THREAD + 0x304, 0x8000FF00);  // stack top
  image.Put(THREAD + 0x308, 0x8000C000);  // stack end
  image.Put(0x8000C000, 0xDEADBABE);
  auto lines = Orca::PanicDiag::DescribeOS(image.Ram(), 0x8000F000);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "orca diag os ctx 80005000: srr0 813365cc srr1 00009032 lr 80012340 r1 "
                      "8000f000 ctr 00000000 state 0001");
  EXPECT_EQ(lines[1], "orca diag os thread 80005000: state 0002 prio 16 stack 8000c000-8000ff00 "
                      "magic ok r1 in");
  // r1 in code and an overwritten stack magic.
  image.Put(0x8000C000, 0);
  lines = Orca::PanicDiag::DescribeOS(image.Ram(), 0x80003000);
  EXPECT_TRUE(lines.back().ends_with("magic bad r1 out"));
  // A context that isn't the thread's gets its own line.
  image.Put(Orca::PanicDiag::OS_CURRENT_CONTEXT, 0x80006000);
  lines = Orca::PanicDiag::DescribeOS(image.Ram(), 0x8000F000);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_TRUE(lines[0].starts_with("orca diag os ctx 80006000: srr0 00000000"));
  EXPECT_TRUE(lines[1].starts_with("orca diag os thread ctx 80005000: srr0 813365cc"));
  // No thread.
  image.Put(Orca::PanicDiag::OS_CURRENT_THREAD, 0);
  lines = Orca::PanicDiag::DescribeOS(image.Ram(), 0x8000F000);
  EXPECT_EQ(lines.back(), "orca diag os thread 00000000: not ram");
}

TEST(OrcaPanicDiag, CodeHashSeesRewrittenCode)
{
  Image image;
  image.Put(0x80003000, 0x7c0802a6);
  image.Put(0x90000100, 0x4e800020);
  Common::RangeSet<u32> ranges;
  ranges.insert(0x3000, 0x3010);
  ranges.insert(0x10000100, 0x10000104);
  const u64 compiled = Orca::PanicDiag::CodeHash(image.Ram(), ranges);
  EXPECT_NE(compiled, 0u);
  EXPECT_EQ(Orca::PanicDiag::CodeHash(image.Ram(), ranges), compiled);
  image.Put(0x90000100, 0x4e800021);  // a plain store over the code, no invalidation
  EXPECT_NE(Orca::PanicDiag::CodeHash(image.Ram(), ranges), compiled);

  Common::RangeSet<u32> outside;
  outside.insert(0x20000, 0x20004);  // past this MEM1
  EXPECT_EQ(Orca::PanicDiag::CodeHash(image.Ram(), outside), 0u);
  EXPECT_EQ(Orca::PanicDiag::CodeHash(image.Ram(), Common::RangeSet<u32>{}), 0u);
}

TEST(OrcaPanicDiag, ExceptionRingOldestFirst)
{
  std::array<Orca::PanicDiag::ExceptionNote, 4> ring{};
  // Six notes into a ring of four: the last four remain, oldest first.
  for (u32 i = 0; i < 6; ++i)
    ring[i % 4] = {0x900, 0x80000000 + i, 0x805b4000, 0x80010000};
  const auto lines = Orca::PanicDiag::DescribeExceptions(ring, 6);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "orca diag exc 6: 900:80000002/805b4000/80010000 "
                      "900:80000003/805b4000/80010000 900:80000004/805b4000/80010000 "
                      "900:80000005/805b4000/80010000");
  EXPECT_EQ(Orca::PanicDiag::DescribeExceptions(ring, 0),
            std::vector<std::string>{"orca diag exc 0:"});
  const auto two = Orca::PanicDiag::DescribeExceptions(ring, 2);
  EXPECT_EQ(two[0], "orca diag exc 2: 900:80000004/805b4000/80010000 "
                    "900:80000005/805b4000/80010000");
}
