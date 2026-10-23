// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Music.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/UX.h"

namespace Orca::Music
{
namespace
{
std::atomic<bool>& OnFlag()
{
  static std::atomic<bool> on{Orca::GetEnv("ORCA_MUSIC") != "off"};
  return on;
}

// The mixer is given the game's (cached) address and the AI DMA the physical one.
constexpr u32 Physical(u32 address)
{
  return address & 0x1FFFFFFF;
}

void PutBE16(u8* out, s32 value)
{
  const s16 sample = static_cast<s16>(std::clamp<s32>(value, -32768, 32767));
  out[0] = static_cast<u8>(static_cast<u16>(sample) >> 8);
  out[1] = static_cast<u8>(static_cast<u16>(sample) & 0xFF);
}
}  // namespace

bool Supported()
{
  // Keep the Brawl-disc music switch for builtin-less packs, as before module selection.
  const auto* profile = ActiveProfile();
  const bool brawl_disc = profile &&
      (profile->IsLauncher() ? profile->disc : profile->game_id) == "RSBE01";
  return brawl_disc || Orca::UX::Game().Describe().music;
}

bool On()
{
  return OnFlag().load(std::memory_order_relaxed);
}

void SetOn(bool on)
{
  if (OnFlag().exchange(on, std::memory_order_relaxed) != on)
    NOTICE_LOG_FMT(ROLLBACK, "Music: {} on this machine", on ? "on" : "off");
}

Frame MixFrame(std::span<const int, FRAME_SAMPLES> main_left,
               std::span<const int, FRAME_SAMPLES> main_right,
               std::span<const int, FRAME_SAMPLES> music_left,
               std::span<const int, FRAME_SAMPLES> music_right,
               std::span<const u16, FRAME_SAMPLES> ramp)
{
  // As AXWiiUCode::OutputSamples: the mix times the volume ramp, clamped to 16 bits.
  Frame out{};
  for (std::size_t i = 0; i < FRAME_SAMPLES; ++i)
  {
    const s64 left = (s64(main_left[i]) - music_left[i]) * ramp[i] >> 15;
    const s64 right = (s64(main_right[i]) - music_right[i]) * ramp[i] >> 15;
    PutBE16(&out[4 * i], static_cast<s32>(std::clamp<s64>(right, -32768, 32767)));
    PutBE16(&out[4 * i + 2], static_cast<s32>(std::clamp<s64>(left, -32768, 32767)));
  }
  return out;
}

bool StreamVoices::IsMusic(u32 pb, u16 is_stream, u32 loop, u32 end, u32 current)
{
  Ring* ring = nullptr;
  for (Ring& r : m_rings)
  {
    if (r.used && r.pb == pb)
      ring = &r;
  }
  if (IsMusicVoice(is_stream))
  {
    if (!ring)
    {
      ring = &m_rings[m_next];
      m_next = (m_next + 1) % KEPT;
    }
    // As a stream the voice loops over its whole ring buffer.
    *ring = Ring{pb, std::min(loop, end), std::max(loop, end), true};
    return true;
  }
  if (!ring)
    return false;
  if (current >= ring->low && current <= ring->high)
    return true;
  // Played past its ring buffer: an effect now, or the end of a song that doesn't loop.
  *ring = Ring{};
  return false;
}

void StreamVoices::Clear()
{
  m_rings = {};
  m_next = 0;
}

void Shadow::Put(u32 address, const Frame& written, const Frame& quiet)
{
  Entry& e = m_entries[m_next];
  m_next = (m_next + 1) % KEPT;
  e.address = Physical(address);
  e.used = true;
  e.written = written;
  e.quiet = quiet;
}

const u8* Shadow::Find(u32 address, const u8* ram)
{
  // About every 30 s of play, how much of it went out without the music.
  if (++m_reads % (1u << 17) == 0)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Music: off, {} of {} AI blocks played without it", m_quiet_reads,
                   m_reads);
  }
  address = Physical(address);
  // Newest first: after a rollback re-run the same buffer holds the re-run's frame.
  for (std::size_t n = 1; n <= KEPT; ++n)
  {
    const Entry& e = m_entries[(m_next + KEPT - n) % KEPT];
    if (!e.used || address < e.address || address - e.address > FRAME_BYTES - DMA_BYTES)
      continue;
    const std::size_t offset = address - e.address;
    if (std::memcmp(ram, &e.written[offset], DMA_BYTES) == 0)
    {
      ++m_quiet_reads;
      return &e.quiet[offset];
    }
  }
  return nullptr;
}

void Shadow::Clear()
{
  m_entries = {};
  m_next = 0;
  m_reads = 0;
  m_quiet_reads = 0;
}

Shadow& GetShadow()
{
  static Shadow shadow;
  return shadow;
}
}  // namespace Orca::Music
