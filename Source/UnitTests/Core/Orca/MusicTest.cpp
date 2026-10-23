// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "Core/Orca/Music.h"
#include "Core/Orca/Status.h"

namespace
{
using Orca::Music::DMA_BYTES;
using Orca::Music::Frame;
using Orca::Music::FRAME_BYTES;
using Orca::Music::FRAME_SAMPLES;
using Orca::Music::MixFrame;
using Orca::Music::Shadow;

using Bus = std::array<int, FRAME_SAMPLES>;
using Ramp = std::array<u16, FRAME_SAMPLES>;

// AXWiiUCode::OutputSamples as it was before the switch: the frame Brawl's AI DMA reads from RAM.
Frame ReferenceOutput(Bus left, Bus right, const Ramp& ramp)
{
  std::array<s16, FRAME_SAMPLES * 2> buffer{};
  for (std::size_t i = 0; i < FRAME_SAMPLES; ++i)
  {
    s64 l = (s64(left[i]) * ramp[i]) >> 15;
    s64 r = (s64(right[i]) * ramp[i]) >> 15;
    left[i] = static_cast<int>(std::clamp<s64>(l, -32768, 32767));
    right[i] = static_cast<int>(std::clamp<s64>(r, -32768, 32767));
  }
  for (std::size_t i = 0; i < FRAME_SAMPLES; ++i)
  {
    const u16 r = static_cast<u16>(right[i]);
    const u16 l = static_cast<u16>(left[i]);
    buffer[2 * i] = static_cast<s16>(static_cast<u16>((r >> 8) | (r << 8)));
    buffer[2 * i + 1] = static_cast<s16>(static_cast<u16>((l >> 8) | (l << 8)));
  }
  Frame out;
  std::memcpy(out.data(), buffer.data(), out.size());
  return out;
}

Bus RandomBus(std::mt19937& rng, int range)
{
  std::uniform_int_distribution<int> d(-range, range);
  Bus b;
  for (int& v : b)
    v = d(rng);
  return b;
}

Ramp RandomRamp(std::mt19937& rng)
{
  std::uniform_int_distribution<int> d(0, 0xFFFF);
  Ramp r;
  for (u16& v : r)
    v = static_cast<u16>(d(rng));
  return r;
}

Frame Filled(u8 value)
{
  Frame f;
  f.fill(value);
  return f;
}
}  // namespace

TEST(OrcaMusic, OnlyStreamVoicesAreMusic)
{
  EXPECT_TRUE(Orca::Music::IsMusicVoice(1));
  EXPECT_FALSE(Orca::Music::IsMusicVoice(0));
  EXPECT_FALSE(Orca::Music::IsMusicVoice(2));
  EXPECT_FALSE(Orca::Music::IsMusicVoice(0xFFFF));
}

// Brawl's menu song on its left stream voice, as a probe saw it in RAM (Orca 0.3.35, idle main
// menu, frames 7100-7400): a ring buffer at nibbles 0x204BC6C2-0x204F06BF, and at the loop 222
// frames as a normal voice with the loop's start and end inside the ring.
constexpr u32 kPb = 0x8049A5C0;
constexpr u32 kRingLow = 0x204BC6C2, kRingHigh = 0x204F06BF;

TEST(OrcaMusic, AStreamVoiceStaysMusicThroughItsSongsLoop)
{
  Orca::Music::StreamVoices voices;
  EXPECT_TRUE(voices.IsMusic(kPb, 1, kRingLow, kRingHigh, 0x204C0000));
  // The loop: type 0, loop start 0x204D06C2 and end 0x204CD81D inside the ring.
  EXPECT_TRUE(voices.IsMusic(kPb, 0, 0x204D06C2, 0x204CD81D, 0x204CC000));
  // Past the jump: the ring's own bounds again, still type 0.
  EXPECT_TRUE(voices.IsMusic(kPb, 0, kRingLow, kRingHigh, 0x204D0800));
  EXPECT_TRUE(voices.IsMusic(kPb, 0, kRingLow, kRingHigh, kRingHigh));
  EXPECT_TRUE(voices.IsMusic(kPb, 1, kRingLow, kRingHigh, 0x204D2000));
}

TEST(OrcaMusic, AVoiceThatNeverStreamedIsNoMusic)
{
  Orca::Music::StreamVoices voices;
  // Even inside another voice's ring buffer.
  EXPECT_TRUE(voices.IsMusic(kPb, 1, kRingLow, kRingHigh, 0x204C0000));
  EXPECT_FALSE(voices.IsMusic(kPb - 0x140, 0, kRingLow, kRingHigh, 0x204C0000));
  EXPECT_FALSE(voices.IsMusic(0x80498040, 0, 0x10000000, 0x10001000, 0x10000800));
}

TEST(OrcaMusic, AStreamVoiceReusedForAnEffectIsNoLongerMusic)
{
  Orca::Music::StreamVoices voices;
  EXPECT_TRUE(voices.IsMusic(kPb, 1, kRingLow, kRingHigh, 0x204C0000));
  // Freed and acquired for an effect: type 0, its own samples elsewhere.
  EXPECT_FALSE(voices.IsMusic(kPb, 0, 0x21000000, 0x21004000, 0x21000010));
  // Forgotten: an effect that later plays at an address inside the old ring is still an effect.
  EXPECT_FALSE(voices.IsMusic(kPb, 0, kRingLow, kRingHigh, 0x204C0000));
  // A new song on the same block is music again, with its own ring.
  EXPECT_TRUE(voices.IsMusic(kPb, 1, 0x204AEB02, 0x204E8301, 0x204B0000));
  EXPECT_TRUE(voices.IsMusic(kPb, 0, 0x204C0000, 0x204B8000, 0x204E8301));
  EXPECT_FALSE(voices.IsMusic(kPb, 0, 0x204C0000, 0x204B8000, 0x204E8302));
}

TEST(OrcaMusic, EachStreamVoiceHasItsOwnRing)
{
  Orca::Music::StreamVoices voices;
  constexpr u32 right = 0x8049A480;
  EXPECT_TRUE(voices.IsMusic(kPb, 1, kRingLow, kRingHigh, 0x204C0000));
  EXPECT_TRUE(voices.IsMusic(right, 1, 0x204F5EC2, 0x20529EBF, 0x204F8000));
  EXPECT_TRUE(voices.IsMusic(right, 0, 0x20509EC2, 0x2050701D, 0x20506000));
  EXPECT_TRUE(voices.IsMusic(kPb, 0, 0x204D06C2, 0x204CD81D, 0x204CC000));
  // The left voice in the right one's ring is not the left voice's song.
  EXPECT_FALSE(voices.IsMusic(kPb, 0, kRingLow, kRingHigh, 0x20506000));
  EXPECT_TRUE(voices.IsMusic(right, 0, 0x204F5EC2, 0x20529EBF, 0x20510000));
  voices.Clear();
  EXPECT_FALSE(voices.IsMusic(right, 0, 0x204F5EC2, 0x20529EBF, 0x20510000));
}

TEST(OrcaMusic, WithoutMusicTheFrameIsTheOneWrittenToRam)
{
  std::mt19937 rng(7);
  const Bus zero{};
  for (int round = 0; round < 200; ++round)
  {
    // Quiet, loud and clipping mixes, at every output volume.
    const Bus left = RandomBus(rng, round % 3 == 0 ? 3000 : 90000);
    const Bus right = RandomBus(rng, round % 3 == 0 ? 3000 : 90000);
    const Ramp ramp = RandomRamp(rng);
    EXPECT_EQ(MixFrame(left, right, zero, zero, ramp), ReferenceOutput(left, right, ramp));
  }
}

TEST(OrcaMusic, TheQuietFrameIsTheMixOfEverythingElse)
{
  std::mt19937 rng(11);
  for (int round = 0; round < 200; ++round)
  {
    const Bus sfx_left = RandomBus(rng, 20000), sfx_right = RandomBus(rng, 20000);
    const Bus music_left = RandomBus(rng, 20000), music_right = RandomBus(rng, 20000);
    Bus main_left, main_right;
    for (std::size_t i = 0; i < FRAME_SAMPLES; ++i)
    {
      main_left[i] = sfx_left[i] + music_left[i];
      main_right[i] = sfx_right[i] + music_right[i];
    }
    const Ramp ramp = RandomRamp(rng);
    EXPECT_EQ(MixFrame(main_left, main_right, music_left, music_right, ramp),
              ReferenceOutput(sfx_left, sfx_right, ramp));
  }
}

TEST(OrcaMusic, MusicAloneIsSilent)
{
  std::mt19937 rng(3);
  const Bus left = RandomBus(rng, 30000), right = RandomBus(rng, 30000);
  Ramp full;
  full.fill(0x8000);
  EXPECT_EQ(MixFrame(left, right, left, right, full), Frame{});
}

TEST(OrcaMusic, TheDmaPlaysTheQuietFrameOnlyWhileRamHoldsWhatTheMixerWrote)
{
  Shadow shadow;
  const u32 at = 0x90001000;
  Frame written = Filled(0x11);
  written[40] = 0x22;
  const Frame quiet = Filled(0x33);
  shadow.Put(at, written, quiet);

  // Every 32-byte read inside the frame, at its offset.
  for (std::size_t offset = 0; offset + DMA_BYTES <= FRAME_BYTES; offset += DMA_BYTES)
  {
    const u8* got = shadow.Find(at + static_cast<u32>(offset), &written[offset]);
    ASSERT_NE(got, nullptr) << offset;
    EXPECT_EQ(std::memcmp(got, &quiet[offset], DMA_BYTES), 0);
  }
  // RAM changed since (a rollback load, or the game wrote there): RAM plays.
  Frame changed = written;
  changed[33] ^= 1;
  EXPECT_EQ(shadow.Find(at + 32, &changed[32]), nullptr);
  EXPECT_NE(shadow.Find(at, &changed[0]), nullptr);
  // Reads outside the frame.
  EXPECT_EQ(shadow.Find(at - 32, &written[0]), nullptr);
  EXPECT_EQ(shadow.Find(at + FRAME_BYTES, &written[0]), nullptr);
  EXPECT_EQ(shadow.Find(at + FRAME_BYTES - DMA_BYTES + 1, &written[0]), nullptr);
  shadow.Clear();
  EXPECT_EQ(shadow.Find(at, &written[0]), nullptr);
}

TEST(OrcaMusic, TheMixersAddressAndTheDmasAreTheSameFrame)
{
  // Brawl: AX writes its output at 0x804E84C0 (cached), the AI DMA reads 0x004E84C0 (physical).
  Shadow shadow;
  const Frame written = Filled(7), quiet = Filled(8);
  shadow.Put(0x804E84C0, written, quiet);
  const u8* got = shadow.Find(0x004E84C0 + 64, &written[64]);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got[0], 8);
  // MEM2 and uncached forms too.
  shadow.Put(0x90001000, written, quiet);
  EXPECT_NE(shadow.Find(0x10001000, &written[0]), nullptr);
  shadow.Put(0xC0002000, written, quiet);
  EXPECT_NE(shadow.Find(0x00002000, &written[0]), nullptr);
}

TEST(OrcaMusic, ARerunFrameReplacesTheFirstAndAnOlderOneStillMatchesAfterALoad)
{
  Shadow shadow;
  const u32 at = 0x90002000;
  const Frame first = Filled(1), first_quiet = Filled(2);
  const Frame rerun = Filled(3), rerun_quiet = Filled(4);
  shadow.Put(at, first, first_quiet);
  shadow.Put(at, rerun, rerun_quiet);
  // RAM holds the re-run's frame: its quiet copy.
  const u8* got = shadow.Find(at, &rerun[0]);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got[0], 4);
  // A load put the first frame's bytes back: its own quiet copy, never the re-run's.
  got = shadow.Find(at, &first[0]);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got[0], 2);
}

TEST(OrcaMusic, OldFramesLeaveTheShadow)
{
  Shadow shadow;
  const Frame frame = Filled(9), quiet = Filled(0);
  shadow.Put(0x90003000, frame, quiet);
  for (std::size_t n = 0; n < Shadow::KEPT; ++n)
    shadow.Put(0x90004000 + static_cast<u32>(n * FRAME_BYTES), Filled(5), quiet);
  EXPECT_EQ(shadow.Find(0x90003000, &frame[0]), nullptr);
}

TEST(OrcaMusic, OnlyAProfileWithStreamedMusicOffersTheSwitch)
{
  // No profile here (a unit test): not offered, and "music off" is not taken without the cap.
  EXPECT_FALSE(Orca::Music::Supported());
  EXPECT_EQ(Orca::Status::OfferedCaps().find(Orca::Status::MUSIC_CAP), std::string::npos);
  EXPECT_STREQ(Orca::Status::MUSIC_CAP, "music");
}
