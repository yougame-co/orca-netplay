// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

// Broadcast file v1, little endian, fixed size SIZE. Main creates and initializes it before
// launch; Dolphin maps the existing file RW, never creates/resizes it. All offsets are bytes.
// Header (256): u32 magic/version/width/height/fps/rate/channels/video_slots/audio_slots/
// video_bytes/audio_frames/size at 0..44; video_latest/audio_latest/video_drops/audio_drops/state
// at 64/68/72/76/80. State: 0 waiting, 1 replay sink active, 2 stopped. Remaining words are zero.
// Video: 3 slots at 256, each 64 + 1280*720*4 bytes, RGBA8, top row first.
// Audio: 64 slots after video, each 64 + 480*2*2 bytes, interleaved LE s16 L/R, 48 kHz.
// Slot prefix: u32 guard/bytes/timestamp_us_low/timestamp_us_high/count at 0/4/8/12/16.
// count is 1 for video, 480 for audio. Timestamp is monotonic host time at capture (audio's first
// sample); both producers use the same clock. Each ring has exactly one producer, no reader ACK.
// Tickets are nonzero u31, wrap 0x7fffff80 -> 1; slot=(ticket-1)%slots. Writer publishes odd
// guard=ticket*2+1 BEFORE any metadata/payload, then even guard=ticket*2, then latest=ticket.
// Reader copies only a matching even guard and checks it again AFTER copying. Video reads newest
// only, audio reads retained tickets in order; overflow drops oldest. No waits or disk flushes.
// Payload words use relaxed atomics too, making the C++ reader safe even in same-process tests.
namespace Broadcast
{
constexpr u32 MAGIC = 0x31424759;  // "YGB1"
constexpr u32 VERSION = 1;
constexpr u32 WIDTH = 1280, HEIGHT = 720, FPS = 30, RATE = 48000, CHANNELS = 2;
constexpr u32 VIDEO_SLOTS = 3, AUDIO_SLOTS = 64, VIDEO_BYTES = WIDTH * HEIGHT * 4;
constexpr u32 AUDIO_FRAMES = 480, AUDIO_BYTES = AUDIO_FRAMES * CHANNELS * sizeof(s16);
constexpr size_t HEADER_BYTES = 256, SLOT_HEADER = 64;
constexpr size_t VIDEO_STRIDE = SLOT_HEADER + VIDEO_BYTES;
constexpr size_t AUDIO_STRIDE = SLOT_HEADER + AUDIO_BYTES;
constexpr size_t AUDIO_OFFSET = HEADER_BYTES + VIDEO_SLOTS * VIDEO_STRIDE;
constexpr size_t SIZE = AUDIO_OFFSET + AUDIO_SLOTS * AUDIO_STRIDE;
constexpr u32 MAX_TICKET = 0x7fffff80;  // Multiple of 3 and 64: slot order survives rollover.
static_assert(MAX_TICKET % VIDEO_SLOTS == 0 && MAX_TICKET % AUDIO_SLOTS == 0);
static_assert(std::atomic_ref<u32>::is_always_lock_free);

inline u32 Load(std::span<u32> memory, size_t offset)
{
  return std::atomic_ref<u32>(memory[offset / 4]).load(std::memory_order_acquire);
}
inline void Store(std::span<u32> memory, size_t offset, u32 value)
{
  std::atomic_ref<u32>(memory[offset / 4]).store(value, std::memory_order_release);
}
inline bool Valid(std::span<u32> memory)
{
  return std::endian::native == std::endian::little && memory.size_bytes() == SIZE &&
         memory[0] == MAGIC && memory[1] == VERSION && memory[2] == WIDTH &&
         memory[3] == HEIGHT && memory[4] == FPS && memory[5] == RATE &&
         memory[6] == CHANNELS && memory[7] == VIDEO_SLOTS && memory[8] == AUDIO_SLOTS &&
         memory[9] == VIDEO_BYTES && memory[10] == AUDIO_FRAMES && memory[11] == SIZE;
}
inline void Initialize(std::span<u32> memory)
{
  std::fill(memory.begin(), memory.end(), 0);
  const u32 fields[] = {MAGIC, VERSION, WIDTH, HEIGHT, FPS, RATE, CHANNELS, VIDEO_SLOTS,
                        AUDIO_SLOTS, VIDEO_BYTES, AUDIO_FRAMES, static_cast<u32>(SIZE)};
  std::copy(std::begin(fields), std::end(fields), memory.begin());
}
inline u32 Next(u32 ticket) { return ticket == MAX_TICKET ? 1 : ticket + 1; }
inline u32 Distance(u32 newer, u32 older)
{
  return newer >= older ? newer - older : MAX_TICKET - older + newer;
}
inline size_t Slot(bool audio, u32 ticket)
{
  return (audio ? AUDIO_OFFSET : HEADER_BYTES) +
         ((ticket - 1) % (audio ? AUDIO_SLOTS : VIDEO_SLOTS)) *
             (audio ? AUDIO_STRIDE : VIDEO_STRIDE);
}
inline u32 Write(std::span<u32> memory, bool audio, std::span<const u8> bytes, u64 timestamp,
                 size_t row_stride = 0)
{
  const size_t payload_size = audio ? AUDIO_BYTES : VIDEO_BYTES;
  if (row_stride ? (audio || row_stride < WIDTH * 4 || bytes.size() != row_stride * HEIGHT) :
                   bytes.size() != payload_size)
    return 0;
  const size_t latest = audio ? 68 : 64;
  const u32 ticket = Next(Load(memory, latest));
  const size_t slot = Slot(audio, ticket);
  std::atomic_ref<u32>(memory[slot / 4]).exchange(ticket * 2 + 1, std::memory_order_acq_rel);
  // If a reader observes any new relaxed payload word, its acquire fence must also observe
  // this invalidation. This fence supplies that synchronization on weakly ordered CPUs.
  std::atomic_thread_fence(std::memory_order_release);
  Store(memory, slot + 4, static_cast<u32>(payload_size));
  Store(memory, slot + 8, static_cast<u32>(timestamp));
  Store(memory, slot + 12, static_cast<u32>(timestamp >> 32));
  Store(memory, slot + 16, audio ? AUDIO_FRAMES : 1);
  // The worker reads the mapped GPU rows straight into the ring, including padded strides.
  const size_t row_bytes = audio ? AUDIO_BYTES : WIDTH * 4;
  const size_t rows = audio ? 1 : HEIGHT;
  const size_t source_stride = row_stride ? row_stride : row_bytes;
  for (size_t row = 0; row < rows; ++row)
  {
    const u8* source = bytes.data() + row * source_stride;
    const size_t destination = slot + SLOT_HEADER + row * row_bytes;
    for (size_t x = 0; x < row_bytes; x += 4)
    {
      u32 word;
      std::memcpy(&word, source + x, 4);
      std::atomic_ref<u32>(memory[(destination + x) / 4]).store(word, std::memory_order_relaxed);
    }
  }
  Store(memory, slot, ticket * 2);
  Store(memory, latest, ticket);
  return ticket;
}
struct Packet
{
  u64 timestamp = 0;
  std::vector<u8> bytes;
};
inline bool Read(std::span<u32> memory, bool audio, u32 ticket, Packet* packet)
{
  if (!ticket || ticket > MAX_TICKET)
    return false;
  const size_t slot = Slot(audio, ticket);
  if (Load(memory, slot) != ticket * 2)
    return false;
  const u32 size = Load(memory, slot + 4);
  if (size != (audio ? AUDIO_BYTES : VIDEO_BYTES) ||
      Load(memory, slot + 16) != (audio ? AUDIO_FRAMES : 1))
    return false;
  const u64 timestamp = Load(memory, slot + 8) | (static_cast<u64>(Load(memory, slot + 12)) << 32);
  std::vector<u8> bytes(size);
  for (size_t i = 0; i < size; i += 4)
  {
    const u32 word = std::atomic_ref<u32>(memory[(slot + SLOT_HEADER + i) / 4])
                         .load(std::memory_order_relaxed);
    std::memcpy(bytes.data() + i, &word, 4);
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  if (Load(memory, slot) != ticket * 2)
    return false;
  *packet = {timestamp, std::move(bytes)};
  return true;
}
}  // namespace Broadcast
