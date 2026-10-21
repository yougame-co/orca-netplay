// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "Common/CommonTypes.h"

// The player's Music switch (stdin "music on|off", cap "music"). Host only: with it off this
// machine plays the mix without the game's music, and nothing the game sees changes.
//
// Brawl and Project+ play their music on stream voices (AX type 1), their effects on normal ones.
// AX HLE writes the full mix to RAM as always and keeps a second mix without the stream voices;
// the AI DMA plays that one while RAM still holds what the mixer wrote. See ORCA.md "Music switch".
namespace Orca::Music
{
// AX Wii mixes 96 samples (3 ms at 32 kHz) per frame and writes them as big-endian s16 pairs,
// right then left.
inline constexpr std::size_t FRAME_SAMPLES = 96;
inline constexpr std::size_t FRAME_BYTES = FRAME_SAMPLES * 4;
// The AI DMA reads 32 bytes (8 pairs) at a time.
inline constexpr std::size_t DMA_BYTES = 32;

using Frame = std::array<u8, FRAME_BYTES>;

// Brawl (any revision) and Project+, whose music is on stream voices.
bool Supported();

// On unless the player turned it off, or ORCA_MUSIC=off for a harness run.
bool On();
void SetOn(bool on);

// Whether an AX voice plays music: a stream voice (AXPB type 1).
constexpr bool IsMusicVoice(u16 is_stream)
{
  return is_stream == 1;
}

// The stream voices, followed through their loops. At each loop of a song the game's stream player
// turns the pair into normal voices (type 0) with the loop's start and end inside the ring buffer,
// so the DSP takes the loop's ADPCM context at the jump, and makes them stream voices again a
// moment later: 0.5-5 s at every loop (Brawl's menu song loops every 91 s). A voice that played as
// a stream stays music while it plays inside the ring buffer it had then; a voice the game frees
// and reuses for an effect plays from elsewhere. Host only, never saved: like the split itself, it
// changes nothing the game sees.
class StreamVoices
{
public:
  static constexpr std::size_t KEPT = 8;

  // Whether the voice whose parameter block is at `pb` plays music this frame. `loop` and `end`
  // are its loop start and end, `current` where it plays now (DSP nibble or sample addresses).
  bool IsMusic(u32 pb, u16 is_stream, u32 loop, u32 end, u32 current);
  void Clear();

private:
  struct Ring
  {
    u32 pb = 0;
    u32 low = 0;
    u32 high = 0;
    bool used = false;
  };
  std::array<Ring, KEPT> m_rings{};
  std::size_t m_next = 0;
};

// One frame as the mixer writes it: (main * ramp) >> 15, clamped, big-endian right then left.
// `music_*` is subtracted first; zero for the frame written to RAM.
Frame MixFrame(std::span<const int, FRAME_SAMPLES> main_left,
               std::span<const int, FRAME_SAMPLES> main_right,
               std::span<const int, FRAME_SAMPLES> music_left,
               std::span<const int, FRAME_SAMPLES> music_right,
               std::span<const u16, FRAME_SAMPLES> ramp);

// The last frames the mixer wrote with the music on (as in RAM) and without it, by address.
class Shadow
{
public:
  static constexpr std::size_t KEPT = 32;

  void Put(u32 address, const Frame& written, const Frame& quiet);
  // The 32 quiet bytes to play for the AI DMA's read at `address`, which finds `ram` there: only if
  // a kept frame covers that read and RAM still holds what the mixer wrote (a rollback load or the
  // game could have changed it). Otherwise null, and the DMA plays RAM.
  const u8* Find(u32 address, const u8* ram);
  void Clear();

private:
  struct Entry
  {
    u32 address = 0;
    bool used = false;
    Frame written{};
    Frame quiet{};
  };
  std::array<Entry, KEPT> m_entries{};
  std::size_t m_next = 0;
  u64 m_reads = 0;
  u64 m_quiet_reads = 0;
};

// The emulator's one shadow, used on the CPU thread only (the AX HLE and the AI DMA both run
// there).
Shadow& GetShadow();
}  // namespace Orca::Music
