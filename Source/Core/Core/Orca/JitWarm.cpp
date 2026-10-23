// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/JitWarm.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <fmt/ranges.h>
#include <xxh3.h>

#include "Common/CommonPaths.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/UX.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Orca::JitWarm
{
namespace
{
constexpr u32 FILE_MAGIC = 0x4D574A4F;  // "OJWM"
constexpr u32 FILE_VERSION = 1;
constexpr u32 MEM2_PHYSICAL = 0x10000000;
constexpr std::size_t MAX_RANGES = 32;
constexpr u32 MAX_RANGE_BYTES = 0x10000;
// Wii RAM, physical: MEM1 (24 MiB at 0) and MEM2 (64 MiB at 0x10000000).
constexpr u32 MEM1_END = 0x01800000;
constexpr u32 MEM2_END = MEM2_PHYSICAL + 0x04000000;
// The most instructions the JIT puts in one block (JitBase::code_buffer_size), as bytes.
constexpr u32 MAX_BLOCK_BYTES = 32000 * 4;
// The smallest block in the file: its fixed fields and one range.
constexpr std::size_t MIN_BLOCK_BYTES = 4 + 4 + 4 + 8 + 2 + 1 + 1 + 8;

bool InRam(u32 start, u32 end)
{
  return start < end && (end <= MEM1_END || (start >= MEM2_PHYSICAL && end <= MEM2_END));
}

// Brawl (NTSC-U, rev 2), Project+ included: gfSceneManager* at 0x805A0060, its current scene at
// +0x4, and the scene's name (char*) at +0x0. A match is the scMelee scene, starting after the load
// that follows character and stage select.
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr std::string_view MATCH_SCENE = "scMelee";

// Warm-up runs for at most WARM_FRAMES from a match's first frame. The first sweep gets BUDGET_US
// per frame boundary (loading frames are cheap to emulate). Blocks whose words weren't in RAM yet
// are retried every SWEEP_EVERY frames with LATE_BUDGET_US, since that happens during play.
constexpr int WARM_FRAMES = 600;
constexpr int SWEEP_EVERY = 30;
constexpr int BUDGET_US = 8000;
constexpr int LATE_BUDGET_US = 1000;
// Each block is due LEAD frames before the frame it first ran in the recorded match, since a match
// can load a little faster than the recorded one. The sweep may exceed BUDGET_US to meet deadlines,
// spreading the work evenly over the frames left. This matters for Project+, which runs its match
// code ~22 frames after scMelee starts: on a slow JIT, compiling it all at once would cost one very
// long frame, while paced it lengthens loading frames that show a still image.
constexpr int LEAD = 3;
// Hard cap on one boundary's sweep: a sweep that fell behind catches up over several frames instead
// of in one long frame during play. Leaves room for a host ~2.5x slower than a typical PC.
constexpr int MAX_SWEEP_US = 100'000;
// JIT space reserved at a match's first frame (JitBase::ReserveForBlocks): a process's first match
// compiles its whole start (~30,000 blocks for Brawl, ~28,000 for Project+); later ones compile
// little.
constexpr std::size_t FIRST_MATCH_BLOCKS = 32'000;
constexpr std::size_t LATER_MATCH_BLOCKS = 8'000;
// Recording: blocks compiled in a match's first RECORD_FRAMES frames. A block counts only if its
// words are unchanged STABLE_AFTER frames after compiling, so code rewritten without an
// invalidation is never compiled ahead.
constexpr int RECORD_FRAMES = 600;
static_assert(WARM_FRAMES <= RECORD_FRAMES);
constexpr int STABLE_AFTER = 60;

template <typename T>
void Put(std::vector<u8>* out, T value)
{
  const std::size_t at = out->size();
  out->resize(at + sizeof(T));
  std::memcpy(out->data() + at, &value, sizeof(T));
}

class Reader
{
public:
  explicit Reader(std::span<const u8> bytes) : m_bytes(bytes) {}
  template <typename T>
  bool Get(T* value)
  {
    if (m_bytes.size() - m_at < sizeof(T))
      return false;
    std::memcpy(value, m_bytes.data() + m_at, sizeof(T));
    m_at += sizeof(T);
    return true;
  }
  bool AtEnd() const { return m_at == m_bytes.size(); }

private:
  std::span<const u8> m_bytes;
  std::size_t m_at = 0;
};

struct Key
{
  u32 effective, physical, flags;
  u64 hash;
  bool operator==(const Key&) const = default;
};
struct KeyHash
{
  std::size_t operator()(const Key& k) const
  {
    return static_cast<std::size_t>(
        XXH3_64bits_withSeed(&k.hash, sizeof(k.hash),
                             (u64{k.effective} << 32) ^ (u64{k.physical} << 3) ^ k.flags));
  }
};
Key KeyOf(const Block& b)
{
  return {b.effective, b.physical, b.flags, b.hash};
}

std::string FilePath()
{
  const Profile* profile = ActiveProfile();
  return File::GetUserPath(D_CACHE_IDX) + (profile ? profile->game_id : "unknown") + ".jitwarm";
}

// Orca's shipped list (Data/Sys/Orca/<profile>.jitwarm) for a player who has none yet, so their
// first match doesn't compile its whole start live. Recorded from reference matches and
// hash-checked like the player's own; the player's list takes over after the first match.
// ORCA_JITWARM_SEED=0 disables it.
std::string SeedPath()
{
  const Profile* profile = ActiveProfile();
  if (!profile || GetEnv("ORCA_JITWARM_SEED") == "0")
    return {};
  // Mod mode: the bundle's JIT seed, when it has one (host-only: not in the tree hash).
  if (profile->mod)
    return profile->mod->JitWarmPath();
  return File::GetSysDirectory() + "Orca/" + profile->game_id + ".jitwarm";
}

// The pack format's cap on orca/jitwarm is the same as Decode's.
static_assert(Mod::MAX_JITWARM_BYTES == JitWarm::MAX_FILE_BYTES);

// Empty when missing, unreadable or over Mod::MAX_JITWARM_BYTES (a mod's orca/jitwarm may be up to
// 4 GiB in its tree.json; Orca's seeds and a player's list are about 1 MB).
std::vector<u8> ReadWhole(const std::string& path)
{
  std::vector<u8> bytes;
  if (File::IOFile in(path, "rb"); in && in.GetSize() <= Mod::MAX_JITWARM_BYTES)
  {
    bytes.resize(in.GetSize());
    if (!in.ReadBytes(bytes.data(), bytes.size()))
      bytes.clear();
  }
  return bytes;
}

// Whether the current scene is a match; nullopt while the scene can't be read (boot, transitions).
std::optional<bool> InMatch(const Core::CPUThreadGuard& guard)
{
  const auto mapped = [&guard](u32 address) {
    return PowerPC::MMU::HostIsRAMAddress(guard, address);
  };
  if (!mapped(SCENE_MANAGER))
    return std::nullopt;
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, SCENE_MANAGER);
  if (!mapped(manager + 0x4))
    return std::nullopt;
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (!mapped(scene))
    return std::nullopt;
  const u32 name = PowerPC::MMU::HostRead<u32>(guard, scene);
  for (u32 i = 0; i <= MATCH_SCENE.size(); ++i)
  {
    if (!mapped(name + i))
      return std::nullopt;
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name + i));
    if (i == MATCH_SCENE.size())
      return c == '\0';
    if (c != MATCH_SCENE[i])
      return false;
  }
  return false;
}

enum class Status : u8
{
  Waiting,   // words not in RAM yet; retried next sweep
  Present,   // the JIT already had it
  Compiled,  // compiled here
  GivenUp,   // no block came out, or one with other ranges (erased unused)
};

struct Fresh
{
  Block block;
  int compiled_at = 0;
};

class State
{
public:
  ~State()
  {
    if (m_io.joinable())
      m_io.join();
  }

  void OnBlockFinalized(const JitBlock& jit_block);
  void OnBoundary(const Core::CPUThreadGuard& guard);

private:
  bool Enabled();
  void StartMatch();
  void EndMatch();
  void StopWarm();
  void Warm(const Core::CPUThreadGuard& guard);
  void CheckStable(int up_to_frame);
  void FinishRecording();
  void LogWarm();
  void LogGame(int frame);
  void LogPresence(const Core::CPUThreadGuard& guard);
  std::span<const u8> Mem1() const;
  std::span<const u8> Mem2() const;

  bool m_checked = false;
  bool m_enabled = false;
  int m_budget_us = BUDGET_US;
  int m_test_slow_us = 0;     // ORCA_TEST_JITWARM_SLOW_US: adds this much to each compile
  bool m_log_frames = false;  // ORCA_JITWARM_LOG=1: a line per boundary that compiled
  bool m_log_presence = false;  // ORCA_JITWARM_LOG=2: also which listed blocks are in RAM
  std::string m_path;
  std::string m_seed_path;  // Orca's shipped list, used when the player has none (SeedPath)
  // A flat bundle's seed as it was verified (Mod.h ModInfo::flat_jitwarm): read from here, never
  // from m_seed_path again.
  std::optional<std::vector<u8>> m_seed_bytes;

  bool m_seen_other_scene = false;  // a match starts only from another scene, not on a join
  bool m_in_match = false;
  int m_match_frame = 0;
  int m_matches = 0;

  // Blocks known before this match, and what this match did with them.
  std::vector<Block> m_known;
  std::vector<Status> m_status;
  // Blocks this sweep looks at, earliest `when` first, and those left for the next sweep.
  std::vector<u32> m_queue, m_waiting;
  std::size_t m_cursor = 0;
  // First-sweep deadlines: each `when` in the queue and the queue index past its last block.
  std::vector<std::pair<int, std::size_t>> m_due;
  int m_next_sweep = 0;
  bool m_warming = false;
  bool m_compiling = false;  // inside a warm-up compile, not the game's own

  // This match's own compiles.
  bool m_recording = false;
  std::deque<Fresh> m_fresh;
  std::vector<Block> m_stable;
  int m_unstable = 0;

  // ORCA_JITWARM_LOG: the game's own compiles since the last boundary, grouped by what the warm-up
  // did with them.
  std::unordered_map<u64, u32> m_index;
  int m_game_total = 0, m_game_warmed = 0, m_game_unreached = 0, m_game_present = 0,
      m_game_unlisted = 0;
  std::vector<u32> m_game_gone;  // a few warmed blocks the game compiled again
  int m_boundaries = 0;
  bool m_logged_frame = false;  // this boundary's warm-up line is logged

  // Warm-up counts for the log.
  int m_compiled = 0, m_present = 0, m_discarded = 0, m_no_room = 0, m_sweeps = 0;
  int m_first_frame = -1, m_last_frame = -1;
  double m_ms = 0, m_max_frame_ms = 0;

  // The file is read and written on m_io; StartMatch picks up the result from m_ready.
  std::thread m_io;
  std::mutex m_mutex;
  std::optional<std::vector<Block>> m_ready;
  bool m_load_started = false;
};

State& GetState()
{
  static State s_state;
  return s_state;
}

bool State::Enabled()
{
  if (m_checked)
    return m_enabled;
  m_checked = true;
  m_enabled = SessionActive() && ScenesReadable() && GetEnv("ORCA_JITWARM") != "0";
  if (const std::string budget = GetEnv("ORCA_JITWARM_BUDGET_US"); !budget.empty())
    m_budget_us = std::clamp(std::atoi(budget.c_str()), 0, 100'000);
  if (const std::string slow = GetEnv("ORCA_TEST_JITWARM_SLOW_US"); !slow.empty())
    m_test_slow_us = std::clamp(std::atoi(slow.c_str()), 0, 10'000);
  const std::string log = GetEnv("ORCA_JITWARM_LOG");
  m_log_frames = log == "1" || log == "2";
  m_log_presence = log == "2";
  m_path = FilePath();
  m_seed_path = SeedPath();
  if (const Profile* profile = ActiveProfile();
      profile && profile->mod && profile->mod->flat && !m_seed_path.empty())
  {
    const std::string& seed = profile->mod->flat_jitwarm;
    m_seed_bytes.emplace(seed.begin(), seed.end());
  }
  if (m_enabled)
    NOTICE_LOG_FMT(ROLLBACK, "JIT warm-up: on ({}, {} us per frame)", m_path, m_budget_us);
  return m_enabled;
}

std::span<const u8> State::Mem1() const
{
  auto& memory = Core::System::GetInstance().GetMemory();
  return {memory.GetRAM(), memory.GetRAM() ? memory.GetRamSizeReal() : 0};
}

std::span<const u8> State::Mem2() const
{
  auto& memory = Core::System::GetInstance().GetMemory();
  return {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSizeReal() : 0};
}

void State::OnBlockFinalized(const JitBlock& jit_block)
{
  if (m_log_frames && m_in_match && !m_compiling)
  {
    ++m_game_total;
    const auto it = m_index.find((u64{jit_block.effectiveAddress} << 32) | jit_block.feature_flags);
    if (it == m_index.end() || it->second >= m_status.size())
      ++m_game_unlisted;
    else if (m_status[it->second] == Status::Compiled)
    {
      ++m_game_warmed;
      if (m_game_gone.size() < 6)
        m_game_gone.push_back(jit_block.effectiveAddress);
    }
    else if (m_status[it->second] == Status::Present)
      ++m_game_present;
    else
      ++m_game_unreached;
  }
  if (!m_recording || m_compiling)
    return;
  Block b;
  b.effective = jit_block.effectiveAddress;
  b.physical = jit_block.physicalAddress;
  b.flags = jit_block.feature_flags;
  b.when = static_cast<u16>(std::clamp(m_match_frame, 0, 0xFFFF));
  for (const auto& [start, end] : jit_block.physical_addresses)
  {
    if (b.ranges.size() == MAX_RANGES)
      return;
    b.ranges.emplace_back(start, end);
  }
  // Instruction-BAT-mapped code only (InstructionBATTranslate).
  auto& mmu = Core::System::GetInstance().GetMMU();
  if (mmu.InstructionBATTranslate(b.effective) != b.physical)
    return;
  const std::optional<u64> hash = HashRanges(Mem1(), Mem2(), b.ranges);
  if (!hash)
    return;
  b.hash = *hash;
  m_fresh.push_back({std::move(b), m_match_frame});
}

void State::StartMatch()
{
  if (m_io.joinable())
    m_io.join();
  {
    std::lock_guard lock(m_mutex);
    if (m_ready)
    {
      m_known = std::move(*m_ready);
      m_ready.reset();
    }
  }
  m_in_match = true;
  m_match_frame = 0;
  ++m_matches;
  m_queue.resize(m_known.size());
  std::iota(m_queue.begin(), m_queue.end(), 0u);
  std::stable_sort(m_queue.begin(), m_queue.end(),
                   [this](u32 a, u32 b) { return m_known[a].when < m_known[b].when; });
  m_due.clear();
  for (std::size_t i = 0; i < m_queue.size(); ++i)
  {
    const int when = m_known[m_queue[i]].when;
    if (m_due.empty() || m_due.back().first != when)
      m_due.emplace_back(when, i + 1);
    else
      m_due.back().second = i + 1;
  }
  m_waiting.clear();
  m_status.assign(m_known.size(), Status::Waiting);
  m_cursor = 0;
  m_next_sweep = 0;
  m_warming = !m_known.empty() && m_budget_us > 0;
  m_compiled = m_present = m_discarded = m_no_room = m_sweeps = 0;
  m_first_frame = m_last_frame = -1;
  m_ms = m_max_frame_ms = 0;
  m_recording = true;
  m_fresh.clear();
  m_stable.clear();
  m_unstable = 0;
  m_index.clear();
  if (m_log_frames)
  {
    for (u32 i = 0; i < m_known.size(); ++i)
      m_index.emplace((u64{m_known[i].effective} << 32) | m_known[i].flags, i);
  }
  m_game_total = m_game_warmed = m_game_unreached = m_game_present = m_game_unlisted = 0;
  NOTICE_LOG_FMT(ROLLBACK, "JIT warm-up: match {} starts, {} blocks known", m_matches,
                 m_known.size());
}

void State::EndMatch()
{
  m_in_match = false;
  StopWarm();
  if (m_recording)
    FinishRecording();
}

void State::StopWarm()
{
  if (!m_warming)
    return;
  m_warming = false;
  LogWarm();
}

void State::LogWarm()
{
  int waiting = 0, given_up = 0;
  for (const Status status : m_status)
  {
    waiting += status == Status::Waiting;
    given_up += status == Status::GivenUp;
  }
  NOTICE_LOG_FMT(ROLLBACK,
                 "JIT warm-up: match {}: {} of {} blocks compiled ahead ({:.1f} ms, frames {} to "
                 "{}, at most {:.1f} ms in one), {} already compiled, {} not in RAM, {} given up "
                 "({} discarded), {} sweeps, {} boundaries without room",
                 m_matches, m_compiled, m_known.size(), m_ms, m_first_frame, m_last_frame,
                 m_max_frame_ms, m_present, waiting, given_up, m_discarded, m_sweeps, m_no_room);
}

void State::LogGame(int frame)
{
  if (m_game_total > 0)
  {
    NOTICE_LOG_FMT(
        ROLLBACK,
        "JIT warm-up: frame {}: the game compiled {} ({} warmed earlier and gone since, "
        "{} not reached yet, {} found compiled earlier and gone since, {} not listed){}{:08x}",
        frame, m_game_total, m_game_warmed, m_game_unreached, m_game_present, m_game_unlisted,
        m_game_gone.empty() ? "" : ": ", fmt::join(m_game_gone, " "));
  }
  m_game_gone.clear();
  m_game_total = m_game_warmed = m_game_unreached = m_game_present = m_game_unlisted = 0;
}

// ORCA_JITWARM_LOG=2, every 30 boundaries outside a match: how many listed blocks could be compiled
// now and how many the JIT already has.
void State::LogPresence(const Core::CPUThreadGuard& guard)
{
  std::lock_guard lock(m_mutex);
  const std::vector<Block>* known = m_ready ? &*m_ready : nullptr;
  if (!known || known->empty())
    return;
  auto& system = guard.GetSystem();
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit)
    return;
  JitBaseBlockCache& cache = *jit->GetBlockCache();
  auto& mmu = system.GetMMU();
  const u32 flags = system.GetPPCState().feature_flags;
  const auto mem1 = Mem1(), mem2 = Mem2();
  int in_ram = 0, compiled = 0;
  for (const Block& b : *known)
  {
    if (b.flags != flags || mmu.InstructionBATTranslate(b.effective) != b.physical)
      continue;
    if (cache.GetBlockFromStartAddress(b.effective, static_cast<CPUEmuFeatureFlags>(flags)))
      ++compiled;
    else if (HashRanges(mem1, mem2, b.ranges) == b.hash)
      ++in_ram;
  }
  NOTICE_LOG_FMT(ROLLBACK,
                 "JIT warm-up: boundary {} (no match): of {} listed, {} compiled, {} more in RAM",
                 m_boundaries, known->size(), compiled, in_ram);
}

void State::CheckStable(int up_to_frame)
{
  const auto mem1 = Mem1(), mem2 = Mem2();
  while (!m_fresh.empty() && m_fresh.front().compiled_at <= up_to_frame)
  {
    Fresh& f = m_fresh.front();
    if (HashRanges(mem1, mem2, f.block.ranges) == f.block.hash)
      m_stable.push_back(std::move(f.block));
    else
      ++m_unstable;
    m_fresh.pop_front();
  }
}

void State::FinishRecording()
{
  // Warming is over by now (WARM_FRAMES <= RECORD_FRAMES), so the known blocks go to the merge.
  StopWarm();
  m_recording = false;
  // Check the youngest blocks now, while their code is still loaded.
  CheckStable(m_match_frame);
  NOTICE_LOG_FMT(ROLLBACK,
                 "JIT warm-up: match {} compiled {} blocks itself in its first {} frames ({} "
                 "rewritten since, not kept)",
                 m_matches, m_stable.size(), m_match_frame, m_unstable);
  if (m_io.joinable())
    m_io.join();
  // Merged and written off the CPU thread; the next match takes the result.
  // Blocks found or compiled this match have their age reset.
  std::vector<bool> seen(m_status.size());
  for (std::size_t i = 0; i < m_status.size(); ++i)
    seen[i] = m_status[i] == Status::Present || m_status[i] == Status::Compiled;
  m_io = std::thread([this, known = std::move(m_known), seen = std::move(seen),
                      learned = std::move(m_stable)]() mutable {
    std::vector<Block> merged = Merge(std::move(known), seen, std::move(learned));
    const std::vector<u8> bytes = Encode(merged);
    const std::string temp = m_path + ".tmp";
    bool written = false;
    if (File::CreateFullPath(m_path))
    {
      File::IOFile out(temp, "wb");
      written = out.WriteBytes(bytes.data(), bytes.size()) && out.Close();
    }
    if (!written || !File::Rename(temp, m_path))
      WARN_LOG_FMT(ROLLBACK, "JIT warm-up: could not write {}", m_path);
    const std::size_t count = merged.size();
    {
      std::lock_guard lock(m_mutex);
      m_ready = std::move(merged);
    }
    INFO_LOG_FMT(ROLLBACK, "JIT warm-up: {} blocks kept ({} bytes)", count, bytes.size());
  });
  m_known.clear();
  m_stable.clear();
  m_status.clear();
  m_queue.clear();
  m_waiting.clear();
  m_due.clear();
  m_index.clear();
}

void State::Warm(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit)
    return;
  if (m_cursor == 0 && m_match_frame < m_next_sweep)
    return;
  auto& ppc_state = system.GetPPCState();
  auto& mmu = system.GetMMU();
  JitBaseBlockCache& cache = *jit->GetBlockCache();
  const CPUEmuFeatureFlags flags = ppc_state.feature_flags;
  const auto mem1 = Mem1(), mem2 = Mem2();
  const auto start = std::chrono::steady_clock::now();
  // The first sweep compiles nearly everything during loading; later sweeps mostly find the
  // remaining blocks still not in RAM, during play.
  const auto deadline =
      start + std::chrono::microseconds(m_sweeps == 0 ? m_budget_us : LATE_BUDGET_US);
  // The first sweep gets at least this far, past the budget if it must (see LEAD), but never past
  // MAX_SWEEP_US.
  const std::size_t must_reach =
      m_sweeps == 0 ? PacedTarget(m_due, m_cursor, m_match_frame, LEAD) : m_cursor;
  const auto hard_deadline = start + std::chrono::microseconds(std::max(m_budget_us, MAX_SWEEP_US));
  bool compiled_here = false;
  const int compiled_before = m_compiled, discarded_before = m_discarded;
  const std::size_t cursor_before = m_cursor;
  int not_in_ram = 0;
  while (true)
  {
    if (m_cursor == m_queue.size())
    {
      // Whatever is still waiting is retried in SWEEP_EVERY frames.
      m_queue.swap(m_waiting);
      m_waiting.clear();
      m_cursor = 0;
      m_next_sweep = m_match_frame + SWEEP_EVERY;
      ++m_sweeps;
      break;
    }
    if (const auto now = std::chrono::steady_clock::now();
        now >= hard_deadline || (m_cursor >= must_reach && now >= deadline))
    {
      break;
    }
    const u32 index = m_queue[m_cursor];
    const Block& b = m_known[index];
    Status& status = m_status[index];
    if (b.flags != flags || mmu.InstructionBATTranslate(b.effective) != b.physical)
    {
      m_waiting.push_back(index);
      ++m_cursor;
      continue;
    }
    if (cache.GetBlockFromStartAddress(b.effective, flags))
    {
      status = Status::Present;
      ++m_present;
      ++m_cursor;
      continue;
    }
    if (HashRanges(mem1, mem2, b.ranges) != b.hash)
    {
      // Not loaded yet, or different code at that address this match.
      ++not_in_ram;
      m_waiting.push_back(index);
      ++m_cursor;
      continue;
    }
    if (!jit->CanCompileFromHook())
    {
      ++m_no_room;
      break;  // retry this block at the next boundary
    }
    ++m_cursor;
    // Compile in the guest's float mode, as the dispatcher does.
    m_compiling = true;
    const auto compile_start = std::chrono::steady_clock::now();
    PowerPC::RoundingModeUpdated(ppc_state);
    const bool fit = jit->CompileFromHook(b.effective);
    Common::FPU::LoadDefaultSIMDState();
    m_compiling = false;
    if (!fit)
    {
      // Bigger than the free code space (no real game's block comes close): the JIT dropped it.
      // Stop warming this match rather than try more of the list.
      WARN_LOG_FMT(ROLLBACK, "JIT warm-up: {:08x} did not fit in the free code space; stopping",
                   b.effective);
      status = Status::GivenUp;
      ++m_discarded;
      m_queue.clear();
      m_waiting.clear();
      m_cursor = 0;
      break;
    }
    if (m_log_frames)
    {
      const double compile_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - compile_start)
                                    .count();
      if (compile_ms >= 4)
      {
        const JitBlock* slow = cache.GetBlockFromStartAddress(b.effective, flags);
        NOTICE_LOG_FMT(ROLLBACK, "JIT warm-up: frame {}: {:08x} took {:.2f} ms ({} instructions)",
                       m_match_frame, b.effective, compile_ms, slow ? slow->originalSize : 0);
      }
    }
    if (m_test_slow_us > 0)
    {
      // Simulates a slower host's compile (tests the schedule, not the JIT).
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::microseconds(m_test_slow_us);
      while (std::chrono::steady_clock::now() < until)
      {
      }
    }
    compiled_here = true;
    JitBlock* block = cache.GetBlockFromStartAddress(b.effective, flags);
    bool same = block != nullptr;
    if (same)
    {
      auto it = b.ranges.begin();
      for (const auto& [range_start, range_end] : block->physical_addresses)
      {
        if (it == b.ranges.end() || it->first != range_start || it->second != range_end)
        {
          same = false;
          break;
        }
        ++it;
      }
      same = same && it == b.ranges.end();
    }
    if (!same)
    {
      // Not the recorded block (its words differ from RAM's): discard it as if never compiled.
      if (block)
        cache.EraseSingleBlock(*block);
      ++m_discarded;
      status = Status::GivenUp;
      continue;
    }
    ++m_compiled;
    status = Status::Compiled;
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  m_ms += ms;
  m_max_frame_ms = std::max(m_max_frame_ms, ms);
  if (compiled_here)
  {
    if (m_first_frame < 0)
      m_first_frame = m_match_frame;
    m_last_frame = m_match_frame;
  }
  if (m_log_frames)
  {
    m_logged_frame = true;
    NOTICE_LOG_FMT(ROLLBACK,
                   "JIT warm-up: frame {}: {} compiled, {} discarded, {} not in RAM, {:.2f} ms ({} "
                   "due)",
                   m_match_frame, m_compiled - compiled_before, m_discarded - discarded_before,
                   not_in_ram, ms, must_reach - cursor_before);
  }
  if (m_queue.empty())
    StopWarm();  // nothing left waiting
}

void State::OnBoundary(const Core::CPUThreadGuard& guard)
{
  if (!ScenesReadable() || !Enabled())
    return;
  if (!m_load_started)
  {
    m_load_started = true;
    m_io = std::thread([this] {
      const std::vector<u8> bytes = ReadWhole(m_path);
      std::optional<std::vector<Block>> blocks = bytes.empty() ? std::nullopt : Decode(bytes);
      if (!bytes.empty() && !blocks)
        WARN_LOG_FMT(ROLLBACK, "JIT warm-up: {} is not readable, starting over", m_path);
      if (!blocks && !m_seed_path.empty())
      {
        blocks = Decode(m_seed_bytes ? *m_seed_bytes : ReadWhole(m_seed_path));
        if (blocks)
        {
          NOTICE_LOG_FMT(ROLLBACK,
                         "JIT warm-up: no list of this player's yet; Orca's {} ({} blocks)",
                         m_seed_path, blocks->size());
        }
      }
      std::lock_guard lock(m_mutex);
      if (!m_ready)
        m_ready = blocks ? std::move(*blocks) : std::vector<Block>{};
    });
  }

  ++m_boundaries;
  if (m_log_frames && m_in_match && m_match_frame < RECORD_FRAMES)
    LogGame(m_match_frame);
  const std::optional<bool> in_match = InMatch(guard);
  if (!in_match)
    return;
  if (!m_in_match)
  {
    if (!*in_match)
    {
      m_seen_other_scene = true;
      if (m_log_presence && m_boundaries % 30 == 0)
        LogPresence(guard);
      return;
    }
    if (!m_seen_other_scene)
      return;  // joined a match already running: its code compiles as it runs
    StartMatch();
    // Grow the JIT's tables now, behind the loading image, not inside a compile during play.
    if (auto* jit = static_cast<JitBase*>(guard.GetSystem().GetJitInterface().GetCore()))
    {
      const auto start = std::chrono::steady_clock::now();
      jit->ReserveForBlocks(m_matches == 1 ? std::max(m_known.size(), FIRST_MATCH_BLOCKS) :
                                             LATER_MATCH_BLOCKS);
      if (m_log_frames)
      {
        NOTICE_LOG_FMT(
            ROLLBACK, "JIT warm-up: frame 0: the JIT's tables made room in {:.2f} ms",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
      }
    }
  }
  else if (!*in_match)
  {
    EndMatch();
    m_seen_other_scene = true;
    return;
  }
  else
  {
    ++m_match_frame;
  }

  if (m_recording)
  {
    CheckStable(m_match_frame - STABLE_AFTER);
    if (m_match_frame >= RECORD_FRAMES)
      FinishRecording();
  }
  m_logged_frame = false;
  if (m_warming)
  {
    if (m_match_frame >= WARM_FRAMES)
      StopWarm();
    else
      Warm(guard);
  }
  // In this mode every boundary of a match's first frames gets a line, so each frame can be placed
  // in time (Tools/orca/match-start.mjs --from-frame).
  if (m_log_frames && m_in_match && m_match_frame < RECORD_FRAMES && !m_logged_frame)
    NOTICE_LOG_FMT(ROLLBACK, "JIT warm-up: frame {}: nothing to warm", m_match_frame);
}
}  // namespace

std::optional<u64> HashRanges(std::span<const u8> mem1, std::span<const u8> mem2,
                              const std::vector<std::pair<u32, u32>>& ranges)
{
  if (ranges.empty())
    return std::nullopt;
  u64 hash = 0x4F7263614A495457ull;
  for (const auto& [start, end] : ranges)
  {
    if (end <= start || end - start > MAX_RANGE_BYTES)
      return std::nullopt;
    const u8* p = nullptr;
    if (end <= mem1.size())
      p = mem1.data() + start;
    else if (start >= MEM2_PHYSICAL && end - MEM2_PHYSICAL <= mem2.size())
      p = mem2.data() + (start - MEM2_PHYSICAL);
    if (!p)
      return std::nullopt;
    hash = XXH3_64bits_withSeed(p, end - start, hash ^ ((u64{start} << 32) | end));
  }
  return hash;
}

std::size_t PacedTarget(std::span<const std::pair<int, std::size_t>> due, std::size_t cursor,
                        int frame, int lead)
{
  // For each deadline ahead, the share of its remaining blocks due at this boundary; the most
  // pressing one sets the pace.
  std::size_t target = cursor;
  for (const auto& [when, end] : due)
  {
    if (end <= cursor)
      continue;
    const auto boundaries = static_cast<std::size_t>(std::max(1, when - lead - frame + 1));
    target = std::max(target, cursor + (end - cursor + boundaries - 1) / boundaries);
  }
  return target;
}

std::vector<u8> Encode(const std::vector<Block>& blocks)
{
  std::vector<u8> out;
  out.reserve(16 + blocks.size() * 32);
  Put<u32>(&out, FILE_MAGIC);
  Put<u32>(&out, FILE_VERSION);
  Put<u32>(&out, static_cast<u32>(blocks.size()));
  for (const Block& b : blocks)
  {
    Put<u32>(&out, b.effective);
    Put<u32>(&out, b.physical);
    Put<u32>(&out, b.flags);
    Put<u64>(&out, b.hash);
    Put<u16>(&out, b.when);
    Put<u8>(&out, b.age);
    Put<u8>(&out, static_cast<u8>(b.ranges.size()));
    for (const auto& [start, end] : b.ranges)
    {
      Put<u32>(&out, start);
      Put<u32>(&out, end);
    }
  }
  Put<u64>(&out, XXH3_64bits(out.data(), out.size()));
  return out;
}

std::optional<std::vector<Block>> Decode(std::span<const u8> bytes)
{
  if (bytes.size() < 3 * sizeof(u32) + sizeof(u64) || bytes.size() > MAX_FILE_BYTES)
    return std::nullopt;
  const std::span<const u8> body = bytes.first(bytes.size() - sizeof(u64));
  u64 checksum;
  std::memcpy(&checksum, bytes.data() + body.size(), sizeof(checksum));
  if (XXH3_64bits(body.data(), body.size()) != checksum)
    return std::nullopt;
  Reader in(body);
  u32 magic, version, count;
  if (!in.Get(&magic) || !in.Get(&version) || !in.Get(&count) || magic != FILE_MAGIC ||
      version != FILE_VERSION || count > 2 * MAX_BLOCKS ||
      count > (bytes.size() - 3 * sizeof(u32) - sizeof(u64)) / MIN_BLOCK_BYTES)
  {
    return std::nullopt;
  }
  std::vector<Block> blocks(count);
  for (Block& b : blocks)
  {
    u8 ranges;
    if (!in.Get(&b.effective) || !in.Get(&b.physical) || !in.Get(&b.flags) || !in.Get(&b.hash) ||
        !in.Get(&b.when) || !in.Get(&b.age) || !in.Get(&ranges) || ranges == 0 ||
        ranges > MAX_RANGES || (b.effective & 3) != 0 || (b.physical & 3) != 0 ||
        !InRam(b.physical, b.physical + 4))
    {
      return std::nullopt;
    }
    b.ranges.resize(ranges);
    u32 block_bytes = 0;
    for (auto& [start, end] : b.ranges)
    {
      if (!in.Get(&start) || !in.Get(&end) || end <= start || end - start > MAX_RANGE_BYTES ||
          ((start | end) & 3) != 0 || !InRam(start, end))
      {
        return std::nullopt;
      }
      block_bytes += end - start;
    }
    if (block_bytes > MAX_BLOCK_BYTES)
      return std::nullopt;
  }
  if (!in.AtEnd())
    return std::nullopt;
  return blocks;
}

std::vector<Block> Merge(std::vector<Block> known, const std::vector<bool>& seen,
                         std::vector<Block> learned)
{
  for (std::size_t i = 0; i < known.size(); ++i)
  {
    if (i < seen.size() && seen[i])
      known[i].age = 0;
    else if (known[i].age < 255)
      ++known[i].age;
  }
  std::unordered_map<Key, std::size_t, KeyHash> index;
  index.reserve(known.size() + learned.size());
  for (std::size_t i = 0; i < known.size(); ++i)
    index.emplace(KeyOf(known[i]), i);
  for (Block& b : learned)
  {
    b.age = 0;
    const auto [it, inserted] = index.emplace(KeyOf(b), known.size());
    if (inserted)
    {
      known.push_back(std::move(b));
      continue;
    }
    Block& old = known[it->second];
    old.age = 0;
    old.when = std::min(old.when, b.when);
  }
  std::erase_if(known, [](const Block& b) { return b.age > MAX_AGE; });
  if (known.size() > MAX_BLOCKS)
  {
    // Youngest first, and among them, what a match runs first.
    std::stable_sort(known.begin(), known.end(), [](const Block& a, const Block& b) {
      return a.age != b.age ? a.age < b.age : a.when < b.when;
    });
    known.resize(MAX_BLOCKS);
  }
  return known;
}

void OnBlockFinalized(const JitBlock& block)
{
  // Recording can still be armed from a previous boot in this process.
  if (ScenesReadable())
    GetState().OnBlockFinalized(block);
}

void OnBoundary(const Core::CPUThreadGuard& guard)
{
  GetState().OnBoundary(guard);
}

// Only modules that can read these scenes use Brawl's warm-up timing and diagnostics.
bool ScenesReadable()
{
  const auto& running = SConfig::GetInstance();
  return Orca::UX::Game().ScenesReadable(running.GetGameID(), running.GetRevision());
}

std::optional<bool> SceneIsMatch(const Core::CPUThreadGuard& guard)
{
  if (!ScenesReadable())
    return std::nullopt;
  return InMatch(guard);
}

std::optional<Scene> CurrentScene(const Core::CPUThreadGuard& guard)
{
  if (!ScenesReadable())
    return std::nullopt;
  const auto mapped = [&guard](u32 address) {
    return PowerPC::MMU::HostIsRAMAddress(guard, address);
  };
  if (!mapped(SCENE_MANAGER))
    return std::nullopt;
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, SCENE_MANAGER);
  if (!mapped(manager + 0x4))
    return std::nullopt;
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (!mapped(scene))
    return std::nullopt;
  return Scene{scene, PowerPC::MMU::HostRead<u32>(guard, scene)};
}

std::string SceneName(const Core::CPUThreadGuard& guard, u32 name)
{
  if (!ScenesReadable())
    return {};
  std::string out;
  for (u32 i = 0; i < 31 && PowerPC::MMU::HostIsRAMAddress(guard, name + i); ++i)
  {
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name + i));
    if (c == '\0')
      break;
    out.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  }
  return out;
}
}  // namespace Orca::JitWarm
