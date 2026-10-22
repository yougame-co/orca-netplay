// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <gtest/gtest.h>

#include "Common/BroadcastRing.h"
#include "VideoCommon/BroadcastPresentation.h"

TEST(BroadcastPresentation, EmptyOrHiddenSurfacesStillCapture)
{
  for (const bool visible : {false, true})
  {
    for (const auto size : {std::array{0, 0}, std::array{0, 720}, std::array{1280, 0},
                            std::array{1, 1}, std::array{1280, 720}})
    {
      const auto output = VideoCommon::ChooseBroadcastPresentation(true, true, visible,
                                                                  size[0], size[1]);
      EXPECT_TRUE(output.capture);
      EXPECT_EQ(output.skip_window, !visible || size[0] == 0 || size[1] == 0);
    }
  }
}

TEST(BroadcastPresentation, DuplicateWindowFramesStillCapture)
{
  const auto output =
      VideoCommon::ChooseBroadcastPresentation(true, true, true, 1280, 720, false);
  EXPECT_TRUE(output.capture);
  EXPECT_TRUE(output.skip_window);
}

TEST(BroadcastPresentation, MissingXfbNeverCaptures)
{
  for (const bool visible : {false, true})
    EXPECT_FALSE(VideoCommon::ChooseBroadcastPresentation(true, false, visible, 0, 0).capture);
}

TEST(BroadcastPresentation, DisabledSinkPreservesWindowPolicy)
{
  for (const bool visible : {false, true})
  {
    for (const bool requested : {false, true})
    {
      const auto output =
          VideoCommon::ChooseBroadcastPresentation(false, true, visible, 0, 0, requested);
      EXPECT_FALSE(output.capture);
      EXPECT_EQ(output.skip_window, !requested);
    }
  }
}

TEST(BroadcastContract, AppHeaderMatchesEveryByte)
{
  // Literal desktop/src/broadcast-ring.ts ringHeader() fields and zero-filled reserved bytes.
  // Independent of the engine constants, so an accidental layout change cannot update both sides.
  constexpr std::array<u32, 12> app_fields = {0x31424759, 1, 1280, 720, 30, 48000,
                                             2, 3, 64, 3686400, 480, 11186624};
  std::array<u8, 256> app_header{};
  for (size_t i = 0; i < app_fields.size(); ++i)
    for (size_t byte = 0; byte < 4; ++byte)
      app_header[i * 4 + byte] = static_cast<u8>(app_fields[i] >> (byte * 8));
  std::vector<u32> memory(Broadcast::SIZE / 4);
  Broadcast::Initialize(memory);
  EXPECT_EQ(Broadcast::SIZE, 11186624u);
  EXPECT_EQ(std::memcmp(memory.data(), app_header.data(), app_header.size()), 0);
}

TEST(BroadcastContract, AppSlotOffsetsGuardsAndMetadata)
{
  std::vector<u32> memory(Broadcast::SIZE / 4);
  Broadcast::Initialize(memory);
  const std::vector<u8> audio(1920, 0x12);
  const std::vector<u8> video(3686400, 0x34);
  EXPECT_EQ(Broadcast::Write(memory, false, video, 0x100000002ull), 1u);
  EXPECT_EQ(Broadcast::Write(memory, true, audio, 0x300000004ull), 1u);
  EXPECT_EQ(memory[64 / 4], 1u);
  EXPECT_EQ(memory[68 / 4], 1u);
  EXPECT_EQ(Broadcast::Slot(false, 1), 256u);
  EXPECT_EQ(Broadcast::Slot(false, 3), 7373184u);
  EXPECT_EQ(Broadcast::Slot(true, 1), 11059648u);
  EXPECT_EQ(Broadcast::Slot(true, 64), 11184640u);
  for (const size_t offset : {size_t{256}, size_t{11059648}})
  {
    const bool is_audio = offset != 256;
    EXPECT_EQ(memory[offset / 4], 2u);  // matching even ticket * 2
    EXPECT_EQ(memory[(offset + 4) / 4], is_audio ? 1920u : 3686400u);
    EXPECT_EQ(memory[(offset + 8) / 4], is_audio ? 4u : 2u);
    EXPECT_EQ(memory[(offset + 12) / 4], is_audio ? 3u : 1u);
    EXPECT_EQ(memory[(offset + 16) / 4], is_audio ? 480u : 1u);
    EXPECT_EQ(memory[(offset + 64) / 4], is_audio ? 0x12121212u : 0x34343434u);
  }
  EXPECT_EQ(Broadcast::MAX_TICKET, 0x7fffff80u);
  EXPECT_EQ(Broadcast::Next(0x7fffff80), 1u);
}
