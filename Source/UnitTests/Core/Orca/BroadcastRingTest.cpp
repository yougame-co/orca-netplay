// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <gtest/gtest.h>
#include "Common/BroadcastRing.h"

TEST(BroadcastRing, FormatAndBounds)
{
  std::vector<u32> memory(Broadcast::SIZE / 4);
  Broadcast::Initialize(memory);
  EXPECT_TRUE(Broadcast::Valid(memory));
  EXPECT_FALSE(Broadcast::Valid({memory.data(), memory.size() - 1}));
  memory[3] = 1;
  EXPECT_FALSE(Broadcast::Valid(memory));
  EXPECT_EQ(Broadcast::Write(memory, false, {}, 0), 0u);
}

TEST(BroadcastRing, WrapAndNewestDropsStale)
{
  std::vector<u32> memory(Broadcast::SIZE / 4);
  Broadcast::Initialize(memory);
  std::vector<u8> frame(Broadcast::VIDEO_BYTES, 7);
  for (u32 i = 1; i <= 8; ++i)
    EXPECT_EQ(Broadcast::Write(memory, false, frame, i * 1000), i);
  Broadcast::Packet packet;
  EXPECT_FALSE(Broadcast::Read(memory, false, 1, &packet));
  EXPECT_TRUE(Broadcast::Read(memory, false, Broadcast::Load(memory, 64), &packet));
  EXPECT_EQ(packet.timestamp, 8000u);
  EXPECT_EQ(packet.bytes, frame);
  Broadcast::Store(memory, 64, Broadcast::MAX_TICKET);
  EXPECT_EQ(Broadcast::Write(memory, false, frame, 9000), 1u);
  EXPECT_TRUE(Broadcast::Read(memory, false, 1, &packet));
  EXPECT_EQ(packet.timestamp, 9000u);
  EXPECT_EQ(Broadcast::Distance(2, Broadcast::MAX_TICKET), 2u);
}

TEST(BroadcastRing, AudioIndependentAndPartialWriteRejected)
{
  std::vector<u32> memory(Broadcast::SIZE / 4);
  Broadcast::Initialize(memory);
  std::vector<u8> audio(Broadcast::AUDIO_BYTES, 4);
  const auto ticket = Broadcast::Write(memory, true, audio, 0x100000123ull);
  EXPECT_EQ(Broadcast::Load(memory, 64), 0u);
  Broadcast::Packet packet;
  ASSERT_TRUE(Broadcast::Read(memory, true, ticket, &packet));
  EXPECT_EQ(packet.timestamp, 0x100000123ull);
  EXPECT_EQ(packet.bytes, audio);
  Broadcast::Store(memory, Broadcast::Slot(true, ticket), ticket * 2 + 1);
  EXPECT_FALSE(Broadcast::Read(memory, true, ticket, &packet));
  EXPECT_EQ(packet.bytes, audio);  // Failure never exposes a partial packet.
}

TEST(BroadcastRing, ConcurrentOverwriteNeverAcceptsMixedPayload)
{
  std::vector<u32> memory(Broadcast::SIZE / 4);
  Broadcast::Initialize(memory);
  std::atomic<bool> done{false};
  std::thread writer([&] {
    std::vector<u8> audio(Broadcast::AUDIO_BYTES);
    for (int i = 1; i <= 3000; ++i)
    {
      std::fill(audio.begin(), audio.end(), static_cast<u8>(i));
      Broadcast::Write(memory, true, audio, i);
    }
    done.store(true, std::memory_order_release);
  });
  bool consistent = true;
  while (!done.load(std::memory_order_acquire))
  {
    Broadcast::Packet packet;
    const u32 ticket = Broadcast::Load(memory, 68);
    if (Broadcast::Read(memory, true, ticket, &packet))
      consistent &= std::all_of(packet.bytes.begin(), packet.bytes.end(),
          [&](u8 byte) { return byte == static_cast<u8>(packet.timestamp); });
  }
  writer.join();
  EXPECT_TRUE(consistent);
}

TEST(BroadcastRing, PaddedMappedRowsAreWrittenWithoutAnIntermediateFrame)
{
  std::vector<u32> memory(Broadcast::SIZE / 4);
  Broadcast::Initialize(memory);
  constexpr size_t stride = Broadcast::WIDTH * 4 + 64;
  std::vector<u8> mapped(stride * Broadcast::HEIGHT, 0xff);
  for (size_t row = 0; row < Broadcast::HEIGHT; ++row)
    std::fill_n(mapped.begin() + row * stride, Broadcast::WIDTH * 4, static_cast<u8>(row));
  const u32 ticket = Broadcast::Write(memory, false, mapped, 123, stride);
  ASSERT_NE(ticket, 0u);
  Broadcast::Packet packet;
  ASSERT_TRUE(Broadcast::Read(memory, false, ticket, &packet));
  EXPECT_EQ(packet.timestamp, 123u);
  ASSERT_EQ(packet.bytes.size(), Broadcast::VIDEO_BYTES);
  for (size_t row = 0; row < Broadcast::HEIGHT; ++row)
  {
    EXPECT_EQ(packet.bytes[row * Broadcast::WIDTH * 4], static_cast<u8>(row));
    EXPECT_EQ(packet.bytes[(row + 1) * Broadcast::WIDTH * 4 - 1], static_cast<u8>(row));
  }
  EXPECT_EQ(Broadcast::Write(memory, false, mapped, 123, Broadcast::WIDTH * 4 - 4), 0u);
  EXPECT_EQ(Broadcast::Write(memory, true, mapped, 123, stride), 0u);
}
