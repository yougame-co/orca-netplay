// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Diag.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#if defined(_M_X86_64)
#include <emmintrin.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Profile.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/Rollback/Cow.h"
#include "Core/System.h"

namespace Rollback::Diag
{
namespace
{
using Clock = std::chrono::steady_clock;

int EnvInt(const char* name, int fallback)
{
  const char* value = std::getenv(name);
  return value && *value ? std::atoi(value) : fallback;
}

const int s_frametime_from = EnvInt("YG_FRAMETIME", -1);
const bool s_keep_throttle = EnvInt("YG_THROTTLE", 0) != 0;
const bool s_snaptime = EnvInt("YG_SNAPTIME", 0) != 0;
const bool s_ntcopy = EnvInt("YG_NTCOPY", 0) != 0;
const int s_pagediff_from = EnvInt("YG_PAGEDIFF", -1);

constexpr std::size_t PAGE = 4096;
constexpr std::size_t BUCKET = 1024 * 1024;
constexpr std::array<int, 3> GAPS{1, 2, 4};

double Ms(Clock::time_point from, Clock::time_point to)
{
  return std::chrono::duration<double, std::milli>(to - from).count();
}

// Mean, percentiles and max of a sample, as one log fragment.
template <typename T>
std::string Describe(std::vector<T> v)
{
  if (v.empty())
    return "no samples";
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (const T x : v)
    sum += static_cast<double>(x);
  const auto at = [&](std::size_t i) { return static_cast<double>(v[i]); };
  return fmt::format("mean {:.2f} p50 {:.1f} p90 {:.1f} p99 {:.1f} max {:.1f}", sum / v.size(),
                     at(v.size() / 2), at(v.size() * 9 / 10), at(v.size() * 99 / 100),
                     at(v.size() - 1));
}

struct FrameTimes
{
  std::vector<float> ms;
  Clock::time_point first{}, last{};
} s_frames;

struct SnapTimes
{
  double state = 0, mem1 = 0, mem2 = 0;
  int saves = 0;
  std::vector<float> port_saves, port_loads;
  // Copy-on-write saves and the faults between them.
  double cow_state = 0, cow_protect = 0;
  int cow_saves = 0;
  std::vector<float> cow_faults, cow_recorded, cow_copied;  // per first-pass frame
  bool cow_sampled = false;
  Rollback::Cow::Counters cow_last;
  std::vector<float> ring_loads, ring_load_blocks;
} s_snap;

struct PageDiff
{
  struct Copy
  {
    int frame = -1;
    std::vector<u8> mem1, mem2;
  };
  std::deque<Copy> history;  // newest first, frames f-1 .. f-4
  // [gap index][0 = MEM1, 1 = MEM2]: pages differing per frame
  std::array<std::array<std::vector<int>, 2>, GAPS.size()> counts;
  std::vector<u64> mem1_buckets, mem2_buckets;  // 1-frame gap, dirty pages per 1 MB
} s_pages;

int CountDirty(const u8* live, const std::vector<u8>& old, std::vector<u64>* buckets)
{
  int dirty = 0;
  for (std::size_t offset = 0; offset < old.size(); offset += PAGE)
  {
    const std::size_t length = std::min(PAGE, old.size() - offset);
    if (std::memcmp(live + offset, old.data() + offset, length) == 0)
      continue;
    ++dirty;
    if (buckets)
      ++(*buckets)[offset / BUCKET];
  }
  return dirty;
}

void SamplePages(Core::System& system, int frame)
{
  auto& memory = system.GetMemory();
  const u8* mem1 = memory.GetRAM();
  const u8* mem2 = memory.GetEXRAM();
  const std::size_t mem1_size = memory.GetRamSize();
  const std::size_t mem2_size = mem2 ? memory.GetExRamSize() : 0;
  if (s_pages.mem1_buckets.empty())
  {
    s_pages.mem1_buckets.resize((mem1_size + BUCKET - 1) / BUCKET);
    s_pages.mem2_buckets.resize((mem2_size + BUCKET - 1) / BUCKET);
  }
  for (std::size_t g = 0; g < GAPS.size() && frame >= s_pagediff_from; ++g)
  {
    const auto old = std::find_if(s_pages.history.begin(), s_pages.history.end(),
                                  [&](const auto& c) { return c.frame == frame - GAPS[g]; });
    if (old == s_pages.history.end())
      continue;
    s_pages.counts[g][0].push_back(
        CountDirty(mem1, old->mem1, g == 0 ? &s_pages.mem1_buckets : nullptr));
    if (mem2)
    {
      s_pages.counts[g][1].push_back(
          CountDirty(mem2, old->mem2, g == 0 ? &s_pages.mem2_buckets : nullptr));
    }
  }
  // Keep frames f .. f-3 for the next boundary (it needs f, f-1 and f-3).
  PageDiff::Copy copy;
  if (s_pages.history.size() >= GAPS.back())
  {
    copy = std::move(s_pages.history.back());
    s_pages.history.pop_back();
  }
  copy.frame = frame;
  copy.mem1.assign(mem1, mem1 + mem1_size);
  if (mem2)
    copy.mem2.assign(mem2, mem2 + mem2_size);
  s_pages.history.push_front(std::move(copy));
}

std::string TopBuckets(const std::vector<u64>& buckets, u32 base)
{
  u64 total = 0;
  for (const u64 b : buckets)
    total += b;
  if (total == 0)
    return "none";
  std::vector<std::size_t> order(buckets.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](auto a, auto b) { return buckets[a] > buckets[b]; });
  std::string out;
  for (std::size_t i = 0; i < std::min<std::size_t>(8, order.size()) && buckets[order[i]]; ++i)
  {
    out += fmt::format("{}{:08x} {:.1f}%", out.empty() ? "" : ", ",
                       base + static_cast<u32>(order[i] * BUCKET),
                       100.0 * static_cast<double>(buckets[order[i]]) / static_cast<double>(total));
  }
  return out;
}
}  // namespace

bool KeepThrottle()
{
  return s_keep_throttle;
}

bool SnapTime()
{
  return s_snaptime;
}

void OnFirstPassBoundary(Core::System& system, int frame)
{
  if (s_frametime_from >= 0 && frame >= s_frametime_from)
  {
    const auto now = Clock::now();
    if (frame == s_frametime_from)
      s_frames.first = now;
    else if (s_frames.last != Clock::time_point{})
      s_frames.ms.push_back(static_cast<float>(Ms(s_frames.last, now)));
    s_frames.last = now;
  }
  if (s_pagediff_from >= 0 && frame >= s_pagediff_from - GAPS.back())
    SamplePages(system, frame);
  if (s_snaptime && s_snap.cow_saves > 0)
  {
    const Rollback::Cow::Counters now = Rollback::Cow::GetCounters();
    if (s_snap.cow_sampled && (s_frametime_from < 0 || frame > s_frametime_from))
    {
      s_snap.cow_faults.push_back(static_cast<float>(now.faults - s_snap.cow_last.faults));
      s_snap.cow_recorded.push_back(
          static_cast<float>(now.pages_recorded - s_snap.cow_last.pages_recorded));
      s_snap.cow_copied.push_back(
          static_cast<float>(now.pages_copied - s_snap.cow_last.pages_copied));
    }
    s_snap.cow_last = now;
    s_snap.cow_sampled = true;
  }
}

void LogSummary()
{
  if (!s_frames.ms.empty())
  {
    const auto& v = s_frames.ms;
    const double wall = Ms(s_frames.first, s_frames.last) / 1000.0;
    const auto over = [&](float limit) { return std::count_if(v.begin(), v.end(), [&](float x) { return x > limit; }); };
    NOTICE_LOG_FMT(ROLLBACK,
                   "Diag frame times from frame {}: {} frames in {:.1f} s ({:.1f} fps), ms {}, "
                   ">16.7 ms {}, >33 ms {}, >50 ms {}",
                   s_frametime_from, v.size(), wall, v.size() / wall, Describe(v), over(16.7f),
                   over(33.334f), over(50.f));
  }
  if (s_snap.saves > 0)
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Diag snapshot saves ({} copy): {}, state {:.2f} ms, MEM1 {:.2f} ms, MEM2 {:.2f} ms",
                   s_ntcopy ? "non-temporal" : "plain", s_snap.saves, s_snap.state / s_snap.saves,
                   s_snap.mem1 / s_snap.saves, s_snap.mem2 / s_snap.saves);
  }
  if (s_snap.cow_saves > 0)
  {
    const Rollback::Cow::Counters c = Rollback::Cow::GetCounters();
    NOTICE_LOG_FMT(ROLLBACK,
                   "Diag copy-on-write snapshots ({} KB pages): {} saves, state {:.3f} ms, "
                   "save {:.3f} ms; per frame: faults {}; pages recorded by faults {}; hot pages "
                   "copied {}; {} mapping changes",
                   Rollback::Cow::PageSize() / 1024, s_snap.cow_saves,
                   s_snap.cow_state / s_snap.cow_saves, s_snap.cow_protect / s_snap.cow_saves,
                   Describe(s_snap.cow_faults), Describe(s_snap.cow_recorded),
                   Describe(s_snap.cow_copied), c.remaps);
  }
  if (!s_snap.ring_loads.empty())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Diag ring loads: {}, ms {}; 4 KB blocks restored {}",
                   s_snap.ring_loads.size(), Describe(s_snap.ring_loads),
                   Describe(s_snap.ring_load_blocks));
  }
  if (!s_snap.port_saves.empty() || !s_snap.port_loads.empty())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Diag session port: {} saves, ms {}; {} loads, ms {}",
                   s_snap.port_saves.size(), Describe(s_snap.port_saves), s_snap.port_loads.size(),
                   Describe(s_snap.port_loads));
  }
  for (std::size_t g = 0; g < GAPS.size(); ++g)
  {
    if (s_pages.counts[g][0].empty())
      continue;
    NOTICE_LOG_FMT(ROLLBACK, "Diag dirty 4 KB pages vs {} frame(s) earlier, {} frames: MEM1 {}; MEM2 {}",
                   GAPS[g], s_pages.counts[g][0].size(), Describe(s_pages.counts[g][0]),
                   Describe(s_pages.counts[g][1]));
  }
  if (!s_pages.counts[0][0].empty())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Diag dirty pages by 1 MB (1-frame gap): MEM1 {}; MEM2 {}",
                   TopBuckets(s_pages.mem1_buckets, 0x80000000u),
                   TopBuckets(s_pages.mem2_buckets, 0x90000000u));
  }
}

void CopyRam(std::vector<u8>* dst, const u8* src, std::size_t size)
{
#if defined(_M_X86_64)
  if (s_ntcopy)
  {
    dst->resize(size);
    u8* const out = dst->data();
    const std::size_t head =
        std::min(size, (16 - (reinterpret_cast<std::uintptr_t>(out) & 15)) & std::size_t{15});
    std::memcpy(out, src, head);
    std::size_t i = head;
    for (; i + 64 <= size; i += 64)
    {
      const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
      const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 16));
      const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 32));
      const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 48));
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i), a);
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i + 16), b);
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i + 32), c);
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i + 48), d);
    }
    std::memcpy(out + i, src + i, size - i);
    _mm_sfence();
    return;
  }
#endif
  dst->assign(src, src + size);
}

void AddSnapshotSave(double state_ms, double mem1_ms, double mem2_ms)
{
  s_snap.state += state_ms;
  s_snap.mem1 += mem1_ms;
  s_snap.mem2 += mem2_ms;
  ++s_snap.saves;
}

void AddCowSave(double state_ms, double protect_ms)
{
  s_snap.cow_state += state_ms;
  s_snap.cow_protect += protect_ms;
  ++s_snap.cow_saves;
}

void AddRingLoad(double ms, std::size_t changed_blocks)
{
  s_snap.ring_loads.push_back(static_cast<float>(ms));
  s_snap.ring_load_blocks.push_back(static_cast<float>(changed_blocks));
}

void AddPortSave(double ms)
{
  s_snap.port_saves.push_back(static_cast<float>(ms));
}

namespace
{
struct JitCodeWord
{
  u32 word = 0;
  u32 clears = 0;       // s_jit_clears when it was compiled
  bool guest = false;   // invalidated by the guest (icbi, dcbst/dcbf) since
  bool forced = false;  // invalidated by a rollback restore since
};
FILE* const s_jit_code_file = [] {
  const char* path = std::getenv("YG_JITCODE_LOG");
  return path && *path ? std::fopen(path, "w") : nullptr;
}();
std::unordered_map<u32, JitCodeWord> s_jit_words;
// The words each block's latest compile read, by block start.
std::unordered_map<u32, std::vector<std::pair<u32, u32>>> s_jit_block_words;
u32 s_jit_clears = 0;
int s_jit_frame = 0;

// The word at an address in MEM1 or MEM2, or nothing. Avoids Memory::Read_U32, which raises a
// panic alert for other addresses.
std::optional<u32> RamWord(Memory::MemoryManager& memory, u32 address)
{
  const u32 physical = address & 0x3FFFFFFF;
  if (physical <= memory.GetRamSizeReal() - 4)
    return Common::swap32(memory.GetRAM() + physical);
  const u32 offset = physical & 0x0FFFFFFF;
  if (memory.GetEXRAM() && (physical >> 28) == 1 && memory.GetExRamSizeReal() >= 4 &&
      offset <= memory.GetExRamSizeReal() - 4)
  {
    return Common::swap32(memory.GetEXRAM() + offset);
  }
  return std::nullopt;
}

// What an instruction fetch at `address` reads now, including the profile's kept game code
// (Orca::KeptInstruction).
std::optional<u32> FetchWord(Memory::MemoryManager& memory, u32 address)
{
  const std::optional<u32> ram = RamWord(memory, address);
  if (!ram)
    return std::nullopt;
  return Orca::KeptInstruction(address & 0x3FFFFFFF, *ram);
}
}  // namespace

const bool g_jit_code_log = s_jit_code_file != nullptr;

void JitCodeSetFrame(int frame)
{
  s_jit_frame = frame;
  if (s_jit_code_file && frame % 60 == 0)
    std::fflush(s_jit_code_file);
}

void JitCodeCompiled(u32 block_start, const std::vector<std::pair<u32, u32>>& words)
{
  if (!s_jit_code_file)
    return;
  auto& memory = Core::System::GetInstance().GetMemory();
  for (const auto& [address, word] : words)
  {
    const std::optional<u32> in_ram = FetchWord(memory, address);
    if (!in_ram)
      continue;
    const u32 ram = *in_ram;
    auto [it, fresh] = s_jit_words.try_emplace(address);
    JitCodeWord& w = it->second;
    if (!fresh && w.word != word)
    {
      const char* why = w.guest  ? "guest" :
                        w.forced ? "forced" :
                        w.clears != s_jit_clears ? "clear" :
                                                   "none";
      std::fprintf(s_jit_code_file, "%d chg %08x %08x %08x %08x %s %08x\n", s_jit_frame, address,
                   w.word, word, ram, why, block_start);
    }
    if (word != ram)
    {
      std::fprintf(s_jit_code_file, "%d stale %08x %08x %08x %08x - %08x\n", s_jit_frame, address,
                   w.word, word, ram, block_start);
    }
    w = {word, s_jit_clears, false, false};
  }
  s_jit_block_words[block_start] = words;
}

void JitCodeCensus(const Core::CPUThreadGuard& guard, const char* why)
{
  if (!s_jit_code_file)
    return;
  Core::System& system = guard.GetSystem();
  auto& memory = system.GetMemory();
  std::map<u32, std::pair<u32, u32>> stale;  // address -> (compiled, RAM)
  std::size_t blocks = 0;
  system.GetJitInterface().RunOnBlocks(guard, [&](const JitBlock& block) {
    ++blocks;
    const auto it = s_jit_block_words.find(block.effectiveAddress);
    if (it == s_jit_block_words.end())
      return;
    for (const auto& [address, word] : it->second)
    {
      if (const std::optional<u32> ram = FetchWord(memory, address); ram && *ram != word)
        stale[address] = {word, *ram};
    }
  });
  for (const auto& [address, words] : stale)
  {
    std::fprintf(s_jit_code_file, "%d stalecode %08x %08x %08x %s\n", s_jit_frame, address,
                 words.first, words.second, why);
  }
  std::fprintf(s_jit_code_file, "%d census %s: %zu blocks, %zu words compiled other than RAM\n",
               s_jit_frame, why, blocks, stale.size());
  std::fflush(s_jit_code_file);
}

void JitCodeInvalidated(u32 address, u32 length, bool forced)
{
  if (!s_jit_code_file || length == 0)
    return;
  const u64 end = static_cast<u64>(address) + length;
  if (length <= 4096)
  {
    for (u64 a = address & ~3u; a < end; a += 4)
    {
      if (auto it = s_jit_words.find(static_cast<u32>(a)); it != s_jit_words.end())
        (forced ? it->second.forced : it->second.guest) = true;
    }
    return;
  }
  for (auto& [a, w] : s_jit_words)
  {
    if (a >= address && a < end)
      (forced ? w.forced : w.guest) = true;
  }
}

void JitCodeCleared(const char* why)
{
  if (!s_jit_code_file)
    return;
  ++s_jit_clears;
  std::fprintf(s_jit_code_file, "%d clear %s\n", s_jit_frame, why);
}

void JitCodeNote(const std::string& line)
{
  if (s_jit_code_file)
    std::fprintf(s_jit_code_file, "%d note %s\n", s_jit_frame, line.c_str());
}

void AddPortLoad(double ms)
{
  s_snap.port_loads.push_back(static_cast<float>(ms));
}
}  // namespace Rollback::Diag
