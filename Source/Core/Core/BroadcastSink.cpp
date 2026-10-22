// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/BroadcastSink.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>

#include "Common/BroadcastRing.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Profile.h"
#include "Core/Rollback/ReplayStream.h"

#ifdef _WIN32
#include <windows.h>
#include "Common/StringUtil.h"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Broadcast
{
namespace
{
class Mapping
{
public:
  explicit Mapping(const std::string& path)
  {
#ifdef _WIN32
    m_file = CreateFileW(UTF8ToWString(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (m_file == INVALID_HANDLE_VALUE)
      return;
    LARGE_INTEGER size;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileSizeEx(m_file, &size) || size.QuadPart != SIZE ||
        !GetFileInformationByHandle(m_file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
      return;
    m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (m_mapping)
      m_data = static_cast<u32*>(MapViewOfFile(m_mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, SIZE));
#else
    m_file = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat statbuf;
    if (m_file < 0 || fstat(m_file, &statbuf) != 0 || !S_ISREG(statbuf.st_mode) ||
        statbuf.st_size != static_cast<off_t>(SIZE))
      return;
    void* data = mmap(nullptr, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_file, 0);
    if (data != MAP_FAILED)
      m_data = static_cast<u32*>(data);
#endif
  }
  ~Mapping()
  {
#ifdef _WIN32
    if (m_data)
      UnmapViewOfFile(m_data);
    if (m_mapping)
      CloseHandle(m_mapping);
    if (m_file != INVALID_HANDLE_VALUE)
      CloseHandle(m_file);
#else
    if (m_data)
      munmap(m_data, SIZE);
    if (m_file >= 0)
      close(m_file);
#endif
  }
  std::span<u32> Memory() { return m_data ? std::span<u32>(m_data, SIZE / 4) : std::span<u32>{}; }
private:
  u32* m_data = nullptr;
#ifdef _WIN32
  HANDLE m_file = INVALID_HANDLE_VALUE;
  HANDLE m_mapping = nullptr;
#else
  int m_file = -1;
#endif
};
// Intentionally leaked: static destruction can race the video/audio writer threads at exit.
// The OS releases the mapping and handles after the last thread has gone.
Mapping* s_mapping = nullptr;
bool s_attempted = false;
}  // namespace

u64 NowMicros()
{
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
void StartReplay()
{
  if (s_attempted || !Rollback::ReplayStream::ProductMode())
    return;
  s_attempted = true;
  const std::string path = Orca::GetEnv("ORCA_BROADCAST_SINK");
  if (path.empty())
    return;
  auto mapping = std::make_unique<Mapping>(path);
  if (!Valid(mapping->Memory()))
  {
    Orca::Status::Line("orca error broadcast_sink The broadcast file could not be opened");
    return;
  }
  s_mapping = mapping.release();
  Store(s_mapping->Memory(), 80, 1);
  s_enabled.store(true, std::memory_order_release);
}
void StopReplay()
{
  s_enabled.store(false, std::memory_order_release);
  if (s_mapping)
    Store(s_mapping->Memory(), 80, 2);
}
void Video(const u8* rgba, size_t stride, u64 timestamp)
{
  if (Active() && rgba && stride >= WIDTH * 4)
    Write(s_mapping->Memory(), false, {rgba, stride * HEIGHT}, timestamp, stride);
}
void VideoDrop()
{
  if (Active())
  {
    const auto memory = s_mapping->Memory();
    std::atomic_ref<u32>(memory[72 / 4]).fetch_add(1, std::memory_order_relaxed);
  }
}
void Audio(const s16* samples, size_t frames, u32 rate)
{
  if (!Active() || !rate)
    return;
  // Only the final mixer thread calls this. Fixed storage, no allocation, locks or file I/O.
  static std::array<s16, AUDIO_FRAMES * CHANNELS> chunk{};
  static size_t filled = 0;
  static u64 timestamp = 0;
  static u32 phase = 0;
  static std::array<s16, 2> previous{};
  static u64 last_call = 0;
  static u32 input_rate = 0;
  const u64 now = NowMicros();
  if (!last_call || now - last_call > 250000 || input_rate != rate)
  {
    filled = 0;
    phase = 0;
    previous = {};
    timestamp = now;
    input_rate = rate;
  }
  last_call = now;
  for (size_t i = 0; i < frames; ++i)
  {
    phase += RATE;
    while (phase >= rate)
    {
      phase -= rate;
      for (size_t channel = 0; channel < CHANNELS; ++channel)
      {
        const float fraction = 1.0f - static_cast<float>(phase) / RATE;
        chunk[filled * 2 + channel] = static_cast<s16>(previous[channel] +
            (samples[i * 2 + channel] - previous[channel]) * fraction);
      }
      if (++filled == AUDIO_FRAMES)
      {
        Write(s_mapping->Memory(), true,
              {reinterpret_cast<const u8*>(chunk.data()), AUDIO_BYTES}, timestamp);
        // Chunk timestamps follow the sample clock, never sound callback scheduling jitter.
        timestamp += AUDIO_FRAMES * 1000000ull / RATE;
        filled = 0;
      }
    }
    previous = {samples[i * 2], samples[i * 2 + 1]};
  }
}
}  // namespace Broadcast
