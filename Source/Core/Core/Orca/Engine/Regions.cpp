// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Engine/Regions.h"

#include <array>
#include <atomic>
#include <mutex>

#include "Common/Logging/Log.h"

namespace Orca::Engine
{
namespace
{
// Distinct 256-byte blocks logged per module, so a write repeated every frame logs once.
constexpr size_t LOGGED_BLOCKS = 16;

// Set by SetWriteRegions while no hook runs (UX::Init before installing them, Shutdown after
// removing them); read on the CPU thread.
std::atomic<const Region*> s_regions{nullptr};
std::atomic<size_t> s_region_count{0};
std::mutex s_lock;  // the module's name and the logged blocks
std::string_view s_module;
std::array<u32, LOGGED_BLOCKS> s_logged{};
size_t s_logged_count = 0;
std::atomic<u64> s_writes{0};
std::atomic<u64> s_outside{0};
}  // namespace

void SetWriteRegions(std::string_view module, std::span<const Region> regions)
{
  std::lock_guard lock(s_lock);
  s_module = module;
  s_logged_count = 0;
  s_writes = 0;
  s_outside = 0;
  // Count first, pointer after: a NoteWrite racing a clear sees no regions or the old ones, never
  // the old count over a null pointer (a module's regions are static, so the old ones stay valid).
  s_region_count.store(0, std::memory_order_release);
  if (regions.empty())
    return;
  s_regions.store(regions.data(), std::memory_order_release);
  s_region_count.store(regions.size(), std::memory_order_release);
}

void NoteWrite(u32 address, u32 bytes)
{
  const size_t count = s_region_count.load(std::memory_order_acquire);
  if (count == 0)
    return;
  s_writes.fetch_add(1, std::memory_order_relaxed);
  for (const Region& region : std::span(s_regions.load(std::memory_order_acquire), count))
  {
    if (region.Contains(address, bytes))
      return;
  }
  s_outside.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(s_lock);
  const u32 block = address & ~0xFFu;
  for (size_t i = 0; i < s_logged_count; ++i)
  {
    if (s_logged[i] == block)
      return;
  }
  if (s_logged_count == LOGGED_BLOCKS)
    return;
  s_logged[s_logged_count++] = block;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Orca: module {} wrote {} byte(s) at {:08x}, outside the regions it declares "
                 "(recorded, not refused)",
                 s_module, bytes, address);
}

WriteCounts Writes()
{
  return {s_writes.load(std::memory_order_relaxed), s_outside.load(std::memory_order_relaxed)};
}
}  // namespace Orca::Engine
