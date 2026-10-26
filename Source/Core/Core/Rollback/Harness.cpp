// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Harness.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/UX/OrbCombo.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/UX.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/Loopback.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/Rollback.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"

namespace Rollback::Harness
{
namespace
{
int EnvInt(const char* name, int fallback)
{
  const std::string v = Orca::GetEnv(name);
  return v.empty() ? fallback : std::atoi(v.c_str());
}

const int s_scenes = EnvInt("YG_SCENES", 0);
// ORCA_UX_TEST_RESEAT_AT=<frame> (harness runs, not online): the player plays port 2 until that
// frame and port 1 from it, the other port reporting no controller, as a former joiner does when it
// comes home (OnlineMatch.cpp ComeHome). Port 1's script rules drive whichever port the player is
// on. Returns the player's port (0 or 1) for `frame`, or -1 without the knob.
int ReseatLocalPort(int frame)
{
  static const int s_at = EnvInt("ORCA_UX_TEST_RESEAT_AT", -1);
  if (s_at < 0 || Orca::Online::Enabled())
    return -1;
  return frame < s_at ? 1 : 0;
}
const int s_synctest_k = EnvInt("YG_SYNCTEST", 0);
const int s_synctest_from = EnvInt("YG_SYNCTEST_FROM", 300);
const int s_exit_after = EnvInt("YG_EXIT_AFTER", 0);
const int s_shot_every = EnvInt("YG_SHOT_EVERY", 0);
const int s_shot_from = EnvInt("YG_SHOT_FROM", 0);
const std::vector<int> s_shot_at = [] {
  std::vector<int> frames;
  std::istringstream list(Orca::GetEnv("YG_SHOT_AT"));
  for (std::string word; std::getline(list, word, ',');)
  {
    if (!word.empty())
      frames.push_back(std::atoi(word.c_str()));
  }
  return frames;
}();
const int s_diag = EnvInt("YG_SYNCTEST_DIAG", 0);
const int s_sections = EnvInt("YG_SYNCTEST_SECTIONS", 0);
const int s_bench_k = EnvInt("YG_BENCH", 0);
const int s_render_reruns = EnvInt("YG_SYNCTEST_RENDER", 0);
// Clearing the JIT after each load makes re-runs compile blocks with different boundaries, as a
// peer with another rollback history would.
const int s_clear_jit = EnvInt("YG_SYNCTEST_CLEARJIT", 0);
const std::string s_loopback_path = Orca::GetEnv("YG_LOOPBACK");
const char* const s_loopback = s_loopback_path.empty() ? nullptr : s_loopback_path.c_str();
const int s_lb_lag = std::max(0, EnvInt("YG_LOOPBACK_LAG", 6));
const int s_lb_jitter = std::max(0, EnvInt("YG_LOOPBACK_JITTER", 3));
const int s_lb_delay = std::max(0, EnvInt("YG_LOOPBACK_DELAY", 2));
const int s_lb_max_rollback = std::max(1, EnvInt("YG_LOOPBACK_MAX_ROLLBACK", 7));
const int s_lb_checksum_every = std::max(1, EnvInt("YG_LOOPBACK_CHECKSUM_EVERY", 10));
// 2 or 4 seats; seat 1 is local (4 needs ORCA_TEST_PADS=4).
const int s_lb_seats = EnvInt("YG_LOOPBACK_SEATS", 2) == 4 ? 4 : 2;
// Remotes go silent from this frame; the session must give up after _GIVE_UP_MS of stalling
// (default 10 s, as online; 0 is never).
const int s_lb_silent_from = EnvInt("YG_LOOPBACK_SILENT_FROM", -1);
const int s_lb_give_up_ms = std::max(0, EnvInt("YG_LOOPBACK_GIVE_UP_MS", 10'000));
// Link spikes: "<period ms>,<length ms>[,<from frame>]" (see VirtualRemote::StartSpikes).
struct LoopbackSpikes
{
  int period_ms = 0;
  int length_ms = 0;
  int from = 0;
};
const LoopbackSpikes s_lb_spikes = [] {
  LoopbackSpikes spikes;
  const std::string value = Orca::GetEnv("YG_LOOPBACK_SPIKE");
  if (value.empty() ||
      std::sscanf(value.c_str(), "%d,%d,%d", &spikes.period_ms, &spikes.length_ms, &spikes.from) <
          2 ||
      spikes.period_ms <= 0 || spikes.length_ms <= 0 || spikes.length_ms >= spikes.period_ms)
  {
    return LoopbackSpikes{};
  }
  return spikes;
}();
// Diagnosis: save a snapshot every frame from here on but never load, to check that saving alone
// does not change the run.
const int s_save_only_from = EnvInt("YG_SAVE_ONLY_FROM", -1);
// Diagnosis: at this first-pass frame, write MEM1 and MEM2 to <user dir>/ram-<frame>-mem{1,2}.bin.
const int s_dump_ram_at = EnvInt("YG_DUMP_RAM_AT", -1);
// A comma-separated list of numbers from an environment variable.
std::vector<u32> EnvList(const char* name, int base)
{
  std::vector<u32> values;
  std::istringstream list(Orca::GetEnv(name));
  for (std::string word; std::getline(list, word, ',');)
  {
    if (!word.empty())
      values.push_back(static_cast<u32>(std::strtoul(word.c_str(), nullptr, base)));
  }
  return values;
}
// Simulates another JIT history: Dolphin clears its JIT when code space fills, at different times
// on different machines. YG_CLEARJIT_AT=<frame>[,...] clears the JIT at the end of those first-pass
// frames; YG_CLEARJIT_EVERY=N clears every Nth frame from YG_CLEARJIT_FROM (default 0).
const std::vector<u32> s_clear_jit_at = EnvList("YG_CLEARJIT_AT", 10);
const int s_clear_jit_every = EnvInt("YG_CLEARJIT_EVERY", 0);
const int s_clear_jit_from = EnvInt("YG_CLEARJIT_FROM", 0);
// YG_JIT_INVALIDATE=<frame>:<hex address>[,...]: at the end of that first-pass frame, drop only
// the JIT blocks containing that instruction.
const std::vector<std::pair<int, u32>> s_jit_invalidate = [] {
  std::vector<std::pair<int, u32>> items;
  std::istringstream list(Orca::GetEnv("YG_JIT_INVALIDATE"));
  for (std::string word; std::getline(list, word, ',');)
  {
    const std::size_t colon = word.find(':');
    if (colon != std::string::npos)
    {
      items.emplace_back(std::atoi(word.c_str()),
                         static_cast<u32>(std::strtoul(word.c_str() + colon + 1, nullptr, 16)));
    }
  }
  return items;
}();
// YG_JITCODE_CENSUS=<frame>[,...] (with YG_JITCODE_LOG): at the end of those first-pass frames,
// list every instruction a live JIT block runs that no longer matches RAM.
const std::vector<u32> s_jit_census_at = EnvList("YG_JITCODE_CENSUS", 10);
// YG_WATCH=<hex address>[,...]: log each word's value once, then on every first-pass frame where
// it changes.
const std::vector<u32> s_watch = EnvList("YG_WATCH", 16);
// Diagnosis: at these first-pass frames, log every NAND file's size and hash, to compare what
// different inputs leave in the NAND.
const std::vector<int> s_nand_hashes_at = [] {
  std::vector<int> frames;
  std::istringstream list(Orca::GetEnv("YG_NAND_HASHES_AT"));
  for (std::string word; std::getline(list, word, ',');)
  {
    if (!word.empty())
      frames.push_back(std::atoi(word.c_str()));
  }
  return frames;
}();
// Keyframe test: capture a keyframe at this frame, keep saving ring snapshots for
// KEYFRAME_TEST_FRAMES more, then load the keyframe and re-run. RAM must match the capture, and
// every re-run frame must hash the same as its first pass.
const int s_keyframe_test = EnvInt("YG_KEYFRAME_TEST", -1);
constexpr int KEYFRAME_TEST_FRAMES = 30;
// Dolphin frame dump between these first-pass frames (see Harness.h).
const int s_dump_frames_from = EnvInt("YG_DUMP_FRAMES_FROM", -1);
const int s_dump_frames_to = EnvInt("YG_DUMP_FRAMES_TO", -1);

// ---- Scene names (scene-relative input and logs) ----

// The game module's scene names (Brawl rev 2's, Games/Brawl/BrawlModule.cpp), where it can read
// them: Dolphin runs Brawl rev 2, a mod declares a builtin, or a launcher profile boots it.
using SceneReader = std::string (*)(const Core::CPUThreadGuard&);

SceneReader SceneReaderForRunningGame()
{
  const SConfig& config = SConfig::GetInstance();
  if (!Orca::UX::Game().ScenesReadable(config.GetGameID(), config.GetRevision()))
    return nullptr;
  return [](const Core::CPUThreadGuard& guard) { return Orca::UX::Game().SceneName(guard); };
}

// Opens the file named by an environment variable for writing; a null handle when unset.
File::IOFile OpenEnvFile(const char* name)
{
  const std::string path = Orca::GetEnv(name);
  return path.empty() ? File::IOFile() : File::IOFile(path, "w");
}
File::IOFile s_hashlog_file = OpenEnvFile("YG_HASHLOG");
FILE* const s_hashlog = s_hashlog_file.GetHandle();

// Modes that save or load snapshots or run their own session (YG_SYNCTEST, YG_BENCH, YG_LOOPBACK,
// YG_SAVE_ONLY_FROM) are off during an online match, which owns the ring and NAND journal.
// Scripted input, recording, hashing and exit still work.
bool SnapshotModesAllowed();

File::IOFile s_padrec_file = OpenEnvFile("YG_PADREC");
FILE* const s_padrec = s_padrec_file.GetHandle();

// ---- Scripted input ----

struct InputRule
{
  int from = 0, to = 0, port = 0, period = 0;
  u32 fuzz_seed = 0;  // nonzero: pseudo-random input
  std::string scene;  // nonempty: from/to count from entering this scene
  int visit = 0;      // nonzero: only on that entry into the scene (1 is the first)
  u16 buttons = 0;
  int sx = -1, sy = -1, cx = -1, cy = -1;
};

std::vector<InputRule> LoadInputScript()
{
  std::vector<InputRule> rules;
  const std::string path = Orca::GetEnv("YG_INPUT");
  if (path.empty())
    return rules;
  std::ifstream in;
  File::OpenFStream(in, path, std::ios_base::in);
  std::string line;
  while (std::getline(in, line))
  {
    if (const auto hash = line.find('#'); hash != std::string::npos)
      line.resize(hash);
    std::istringstream words(line);
    std::string first;
    if (!(words >> first))
      continue;
    InputRule r;
    if (first.size() > 1 && first[0] == '@')
    {
      r.scene = first.substr(1);
      if (const auto colon = r.scene.find(':'); colon != std::string::npos)
      {
        r.visit = std::atoi(r.scene.c_str() + colon + 1);
        r.scene.resize(colon);
      }
      if (!(words >> first))
        continue;
    }
    if (first == "mash")
      words >> r.from >> r.to >> r.port >> r.period;
    else if (first == "fuzz")
      words >> r.from >> r.to >> r.port >> r.fuzz_seed;
    else
    {
      r.from = std::atoi(first.c_str());
      words >> r.to >> r.port;
    }
    r.port -= 1;
    std::string w;
    while (words >> w)
    {
      static const std::map<std::string, u16> names = {
          {"A", 0x0100},    {"B", 0x0200},     {"X", 0x0400},    {"Y", 0x0800},
          {"Z", 0x0010},    {"R", 0x0020},     {"L", 0x0040},    {"START", 0x1000},
          {"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"DOWN", 0x0004}, {"UP", 0x0008}};
      if (auto it = names.find(w); it != names.end())
        r.buttons |= it->second;
      else if (w.rfind("SX=", 0) == 0)
        r.sx = std::atoi(w.c_str() + 3);
      else if (w.rfind("SY=", 0) == 0)
        r.sy = std::atoi(w.c_str() + 3);
      else if (w.rfind("CX=", 0) == 0)
        r.cx = std::atoi(w.c_str() + 3);
      else if (w.rfind("CY=", 0) == 0)
        r.cy = std::atoi(w.c_str() + 3);
    }
    if (r.port >= 0 && r.port < 4)
      rules.push_back(r);
  }
  NOTICE_LOG_FMT(ROLLBACK, "Harness input: {} rules from {}{}", rules.size(), path,
                 in.is_open() ? "" : " (could not open the file)");
  return rules;
}

// Loaded on first use, after logging is up, so a bad path shows in the log.
const std::vector<InputRule>& Input()
{
  static const std::vector<InputRule> s_rules = LoadInputScript();
  return s_rules;
}

// ---- Frame and scene bookkeeping ----

int s_frame = 0;       // the frame being run, or whose end we are at; rewinds on a load
std::atomic<int> s_shown_frame{-1};  // for ShownFrame()
int s_rewind_to = -1;  // set by RewindTo for this boundary
bool s_restart = false;  // set by Restart for this boundary
int s_online_stop_at = -1;  // online: the first-pass frame at which emulation stops
int s_max_frame = -1;  // highest frame reached on a first pass
bool s_stop_queued = false;
std::string s_scene;
// The scene YG_SCENES last logged. Only first passes log, so this can differ from s_scene after a
// re-run.
std::string s_logged_scene;
int s_scene_start = 0;
int s_scene_visit = 0;  // which entry into s_scene this is (1 for the first)
// Every scene entry so far (frame -> scene), for visit numbers; a load drops later ones.
std::map<int, std::string> s_scene_entries;
struct SceneAt
{
  std::string scene;
  int start = 0;
  int visit = 0;
};
// Scene state at the end of each recent frame, so a load can restore it.
std::map<int, SceneAt> s_scene_history;

// After a load to the end of `frame`, restores the scene state from then.
void RestoreScene(int frame)
{
  if (auto history = s_scene_history.find(frame); history != s_scene_history.end())
  {
    s_scene = history->second.scene;
    s_scene_start = history->second.start;
    s_scene_visit = history->second.visit;
  }
  s_scene_entries.erase(s_scene_entries.upper_bound(frame), s_scene_entries.end());
}

// ---- Hashing ----

constexpr u32 REGION_SIZE = 64 * 1024;
constexpr u32 MEM2_PHYSICAL = 0x10000000u;

std::vector<u64> HashRegions(Core::System& system)
{
  auto& memory = system.GetMemory();
  std::vector<u64> hashes;
  auto hash_area = [&](const u8* base, std::size_t size) {
    for (std::size_t offset = 0; offset < size; offset += REGION_SIZE)
    {
      const std::size_t length = std::min<std::size_t>(REGION_SIZE, size - offset);
      hashes.push_back(XXH3_64bits(base + offset, length));
    }
  };
  hash_area(memory.GetRAM(), memory.GetRamSize());
  if (memory.GetEXRAM())
    hash_area(memory.GetEXRAM(), memory.GetExRamSize());
  return hashes;
}

u32 RegionPhysical(std::size_t index, std::size_t mem1_regions)
{
  return index < mem1_regions ?
             static_cast<u32>(index) * REGION_SIZE :
             MEM2_PHYSICAL + static_cast<u32>(index - mem1_regions) * REGION_SIZE;
}

// ---- Sync test ----

struct FrameRecord
{
  s64 ticks = 0;  // CoreTiming ticks just after the save
  u64 ram = 0;
  u64 dev = 0;
  std::vector<u64> regions;
  // YG_SYNCTEST_DIAG only: full copies for byte-level diffs, DoState section ends, events run.
  std::vector<u8> mem1, mem2, dev_buf;
  std::vector<std::pair<std::string, std::size_t>> markers;
  std::vector<std::string> events;
};

std::map<int, FrameRecord> s_records;
std::vector<std::string> s_frame_events;  // CoreTiming events since the last frame end
u64 s_checked = 0, s_mismatches = 0, s_ram_mismatches = 0, s_loads = 0;
std::map<std::string, u64> s_section_frames;  // per section: re-run frames where it differed

// Counts which DoState sections of a re-run frame differ from its first pass. Sections are matched
// by name and occurrence, so one section changing size does not misalign the rest.
void CountSectionDiffs(const FrameRecord& want, const FrameRecord& got)
{
  using Ranges = std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>>;
  const auto sections = [](const FrameRecord& r) {
    Ranges out;
    std::size_t start = 0;
    for (const auto& [name, end] : r.markers)
    {
      out[name].emplace_back(start, end);
      start = end;
    }
    if (start < r.dev_buf.size())
      out["(after last marker)"].emplace_back(start, r.dev_buf.size());
    return out;
  };
  const Ranges a = sections(want), b = sections(got);
  std::set<std::string> differ;
  for (const auto& [name, ranges] : a)
  {
    const auto it = b.find(name);
    if (it == b.end() || it->second.size() != ranges.size())
    {
      differ.insert(name);
      continue;
    }
    for (std::size_t k = 0; k < ranges.size(); ++k)
    {
      const auto [a0, a1] = ranges[k];
      const auto [b0, b1] = it->second[k];
      if (a1 - a0 != b1 - b0 ||
          std::memcmp(want.dev_buf.data() + a0, got.dev_buf.data() + b0, a1 - a0) != 0)
      {
        differ.insert(name);
        break;
      }
    }
  }
  for (const auto& [name, ranges] : b)
  {
    if (!a.contains(name))
      differ.insert(name);
  }
  for (const std::string& name : differ)
    ++s_section_frames[name];
}

std::string DescribeSectionDiffs()
{
  if (s_section_frames.empty())
    return "none";
  std::string out;
  for (const auto& [name, frames] : s_section_frames)
    out += fmt::format("{}{}: {}", out.empty() ? "" : ", ", name, frames);
  return out;
}

// With ORCA_TEST_SNAPSHOT_EVERY=K, the sync test loads only multiples of K, so each rewind re-runs
// k to k + K - 1 frames, like a session rolling back from a sparse snapshot. Every frame is still
// saved, so frames saved in one pass but not another are left to YG_LOOPBACK.
int SyncTestLoadEvery()
{
  return std::max(Orca::TestSnapshotEvery(), 1);
}

SnapshotRing& Ring()
{
  static SnapshotRing ring(static_cast<std::size_t>(std::max(s_synctest_k, s_bench_k)) +
                           static_cast<std::size_t>(SyncTestLoadEvery()) + 1);
  return ring;
}

// ---- Keyframe test (YG_KEYFRAME_TEST) ----

struct KeyframeTestState
{
  std::optional<MachineImage> image;
  u64 ram = 0;
  std::map<int, u64> hashes;
  bool loaded = false;
  int checked = 0;
  int mismatches = 0;
};
KeyframeTestState s_keyframe;

u64 LiveRamChecksum(Core::System& system)
{
  auto& memory = system.GetMemory();
  return RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                     {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
}

void KeyframeTest(Core::System& system, int f, bool first_pass)
{
  const int from = s_keyframe_test, to = s_keyframe_test + KEYFRAME_TEST_FRAMES;
  if (!first_pass)
  {
    if (const auto it = s_keyframe.hashes.find(f); it != s_keyframe.hashes.end())
    {
      ++s_keyframe.checked;
      if (it->second != LiveRamChecksum(system))
        ++s_keyframe.mismatches;
    }
    if (f == to)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Keyframe test re-run: {} frames checked, {} mismatches ({})",
                     s_keyframe.checked, s_keyframe.mismatches,
                     s_keyframe.checked == KEYFRAME_TEST_FRAMES && s_keyframe.mismatches == 0 ?
                         "PASS" :
                         "FAIL");
    }
    return;
  }
  if (f < from || f > to)
    return;
  if (f == from)
  {
    MachineImage image;
    if (SnapshotRing::Capture(system, &image, false))
    {
      s_keyframe.image = std::move(image);
      s_keyframe.ram = LiveRamChecksum(system);
    }
  }
  if (f > from)
    s_keyframe.hashes[f] = LiveRamChecksum(system);
  Ring().Save(system, f);
  if (f == to && s_keyframe.image && !s_keyframe.loaded)
  {
    s_keyframe.loaded = true;
    const bool ok = Ring().LoadMachineImage(system, std::move(*s_keyframe.image), from);
    const u64 ram = LiveRamChecksum(system);
    NOTICE_LOG_FMT(ROLLBACK, "Keyframe test: loaded frame {} at frame {}: {}, RAM {:016x} (captured {:016x}) {}",
                   from, f, ok ? "ok" : "failed", ram, s_keyframe.ram,
                   ok && ram == s_keyframe.ram ? "PASS" : "FAIL");
    if (ok)
    {
      s_frame = from;
      // Restore the scene so scene-relative input does not shift across a scene change.
      RestoreScene(from);
    }
  }
}

void LogByteDiffs(const FrameRecord& want, const FrameRecord& got)
{
  auto ram_diffs = [](const std::vector<u8>& a, const std::vector<u8>& b, u32 virtual_base,
                      const char* name) {
    const std::size_t n = std::min(a.size(), b.size());
    int ranges = 0;
    std::size_t i = 0;
    while (i < n)
    {
      if (a[i] == b[i])
      {
        ++i;
        continue;
      }
      std::size_t start = i, last = i, count = 0;
      while (i < n && i - last < 16)
      {
        if (a[i] != b[i])
        {
          last = i;
          ++count;
        }
        ++i;
      }
      if (ranges++ < 40)
      {
        std::string bytes;
        for (std::size_t k = start; k <= std::min(last, start + 7); ++k)
          bytes += fmt::format(" {:02x}>{:02x}", a[k], b[k]);
        NOTICE_LOG_FMT(ROLLBACK, "  {} diff {:08x}..{:08x} ({} bytes differ):{}", name,
                       virtual_base + start, virtual_base + last + 1, count, bytes);
      }
    }
    NOTICE_LOG_FMT(ROLLBACK, "  {}: {} differing ranges", name, ranges);
  };
  ram_diffs(want.mem1, got.mem1, 0x80000000u, "MEM1");
  ram_diffs(want.mem2, got.mem2, 0x90000000u, "MEM2");

  // Attribute each differing byte to the DoState section whose end marker follows it.
  std::map<std::string, std::size_t> per_section;
  std::map<std::string, std::string> detail;
  const std::size_t n = std::min(want.dev_buf.size(), got.dev_buf.size());
  for (std::size_t i = 0; i < n; ++i)
  {
    if (want.dev_buf[i] == got.dev_buf[i])
      continue;
    std::string section = "(after last marker)";
    std::size_t section_start = 0;
    for (const auto& [name, end] : want.markers)
    {
      if (i < end)
      {
        section = name;
        break;
      }
      section_start = end;
    }
    if (per_section[section]++ < 8)
    {
      detail[section] += fmt::format(" +{}:{:02x}>{:02x}", i - section_start, want.dev_buf[i],
                                     got.dev_buf[i]);
    }
  }
  if (want.dev_buf.size() != got.dev_buf.size())
    NOTICE_LOG_FMT(ROLLBACK, "  DoState size {} vs {}", want.dev_buf.size(), got.dev_buf.size());
  for (const auto& [name, count] : per_section)
  {
    NOTICE_LOG_FMT(ROLLBACK, "  DoState section {}: {} bytes differ:{}", name, count,
                   detail[name]);
  }

  std::size_t first = 0;
  while (first < want.events.size() && first < got.events.size() &&
         want.events[first] == got.events[first])
  {
    ++first;
  }
  NOTICE_LOG_FMT(ROLLBACK, "  events: {} first pass, {} re-run, first difference at #{}",
                 want.events.size(), got.events.size(), first);
  for (std::size_t k = (first > 3 ? first - 3 : 0); k < first + 6; ++k)
  {
    NOTICE_LOG_FMT(ROLLBACK, "    #{} first: {}  | re-run: {}", k,
                   k < want.events.size() ? want.events[k] : "-",
                   k < got.events.size() ? got.events[k] : "-");
  }
}

void SyncTest(Core::System& system)
{
  const int f = s_frame;
  const bool first_pass = f > s_max_frame;
  SnapshotRing& ring = Ring();

  std::vector<std::pair<std::string, const u8*>> marker_ptrs;
  if (s_diag || s_sections)
    PointerWrap::s_marker_log = &marker_ptrs;
  ring.Save(system, f);
  PointerWrap::s_marker_log = nullptr;

  FrameRecord rec;
  rec.ticks = static_cast<s64>(system.GetCoreTiming().GetTicks());
  const auto state = ring.LastState();
  rec.dev = XXH3_64bits(state.data(), state.size());
  rec.regions = HashRegions(system);
  rec.ram = XXH3_64bits(rec.regions.data(), rec.regions.size() * sizeof(u64));
  if (s_diag || s_sections)
  {
    rec.dev_buf.assign(state.begin(), state.end());
    for (const auto& [name, ptr] : marker_ptrs)
    {
      if (ptr >= state.data() && ptr <= state.data() + state.size())
        rec.markers.emplace_back(name, static_cast<std::size_t>(ptr - state.data()));
    }
  }
  if (s_diag)
  {
    auto& memory = system.GetMemory();
    rec.mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
    if (memory.GetEXRAM())
      rec.mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
    rec.events = std::move(s_frame_events);
    s_frame_events.clear();

    static bool s_sizes_logged = false;
    if (!s_sizes_logged)
    {
      s_sizes_logged = true;
      std::size_t previous = 0;
      for (const auto& [name, end] : rec.markers)
      {
        if (end - previous >= 16 * 1024)
          NOTICE_LOG_FMT(ROLLBACK, "  snapshot section {}: {} KB", name, (end - previous) / 1024);
        previous = end;
      }
      NOTICE_LOG_FMT(ROLLBACK, "  snapshot non-RAM total: {} KB", state.size() / 1024);
    }
  }

  if (first_pass)
  {
    s_records[f] = std::move(rec);
    s_records.erase(s_records.begin(),
                    s_records.lower_bound(
                        f - (s_diag ? std::max(8, s_synctest_k + SyncTestLoadEvery()) : 64)));
  }
  else if (auto it = s_records.find(f); it != s_records.end())
  {
    ++s_checked;
    const FrameRecord& want = it->second;
    if (s_sections && want.dev != rec.dev)
      CountSectionDiffs(want, rec);
    if (want.ram != rec.ram || want.dev != rec.dev)
    {
      // Only RAM decides pass or fail. Device state also holds the emulated icache (filled only
      // when the JIT compiles) and GPU dirty flags, which differ on re-runs without reaching RAM.
      ++s_mismatches;
      const bool ram_bad = want.ram != rec.ram;
      if (ram_bad)
        ++s_ram_mismatches;
      if ((ram_bad && s_ram_mismatches <= 20) || (!ram_bad && s_ram_mismatches == 0 && s_mismatches <= 5))
      {
        const std::size_t mem1_regions =
            (system.GetMemory().GetRamSize() + REGION_SIZE - 1) / REGION_SIZE;
        std::string diff;
        int shown = 0, total = 0;
        for (std::size_t i = 0; i < rec.regions.size() && i < want.regions.size(); ++i)
        {
          if (rec.regions[i] == want.regions[i])
            continue;
          ++total;
          if (shown++ < 12)
            diff += fmt::format(" {:08x}", RegionPhysical(i, mem1_regions));
        }
        NOTICE_LOG_FMT(ROLLBACK,
                       "Synctest MISMATCH frame {} (scene {}): ram {} dev {}; ticks {:+}; {} "
                       "regions differ:{}",
                       f, s_scene, ram_bad ? "DIFF" : "same", want.dev == rec.dev ? "same" : "DIFF",
                       rec.ticks - want.ticks, total, diff);
        if (s_diag && !want.mem1.empty() &&
            ((ram_bad && s_ram_mismatches <= 3) || (!ram_bad && s_mismatches <= 3)))
        {
          LogByteDiffs(want, rec);
        }
      }
    }
  }

  // On each new frame, rewind at least k frames (to a multiple of ORCA_TEST_SNAPSHOT_EVERY) so
  // they are re-run and compared.
  const int every = SyncTestLoadEvery();
  const int target = f - s_synctest_k - ((f - s_synctest_k) % every + every) % every;
  if (first_pass && target >= 0 && ring.Has(target))
  {
    if (ring.Load(system, target))
    {
      ++s_loads;
      if (s_clear_jit)
      {
        if (Diag::g_jit_code_log)
          Diag::JitCodeNote("YG_SYNCTEST_CLEARJIT");
        system.GetJitInterface().ClearSafe();
      }
      s_frame = target;
      s_frame_events.clear();
      RestoreScene(s_frame);
    }
  }

  if (first_pass && f % 600 == 0)
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Synctest frame {} (scene {}): {} re-run frames checked, {} RAM mismatches, {} "
                   "device-state-only mismatches, {} loads (rewind {}, to multiples of {})",
                   f, s_scene, s_checked, s_ram_mismatches, s_mismatches - s_ram_mismatches,
                   s_loads, s_synctest_k, every);
    if (s_sections)
      NOTICE_LOG_FMT(ROLLBACK, "Synctest device-state sections differing (re-run frames): {}",
                     DescribeSectionDiffs());
  }
}

// ---- Bench ----

void Bench(Core::System& system, bool first_pass)
{
  using clock = std::chrono::steady_clock;
  static clock::time_point s_last = clock::now();
  static double s_first_ms = 0, s_rerun_ms = 0, s_save_ms = 0, s_load_ms = 0, s_first_max = 0,
                s_rerun_max = 0;
  static u64 s_first = 0, s_reruns = 0, s_saves = 0, s_bench_loads = 0;
  auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

  // Time for the frame that just ended, excluding save and load.
  const double frame = ms(clock::now() - s_last);
  if (s_frame >= 300)  // skip boot
  {
    double& total = first_pass ? s_first_ms : s_rerun_ms;
    double& max = first_pass ? s_first_max : s_rerun_max;
    total += frame;
    max = std::max(max, frame);
    ++(first_pass ? s_first : s_reruns);

    SnapshotRing& ring = Ring();
    auto t0 = clock::now();
    ring.Save(system, s_frame);
    s_save_ms += ms(clock::now() - t0);
    ++s_saves;
    // Rewind once per 60 new frames; the re-run reaches this frame again without rewinding.
    if (first_pass && s_frame % 60 == 0 && ring.Has(s_frame - s_bench_k))
    {
      t0 = clock::now();
      if (ring.Load(system, s_frame - s_bench_k))
      {
        s_load_ms += ms(clock::now() - t0);
        ++s_bench_loads;
        s_frame -= s_bench_k;
      }
    }
    if (first_pass && s_first > 0 && s_first % 600 == 0)
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Bench at frame {}: frame {:.2f} ms avg ({:.2f} max); re-run frame {:.2f} ms "
                     "avg ({:.2f} max) over {}; save {:.2f} ms avg; load({}) {:.2f} ms avg over {}",
                     s_frame, s_first_ms / s_first, s_first_max,
                     s_rerun_ms / std::max<u64>(s_reruns, 1), s_rerun_max, s_reruns,
                     s_save_ms / std::max<u64>(s_saves, 1), s_bench_k,
                     s_load_ms / std::max<u64>(s_bench_loads, 1), s_bench_loads);
      s_first_ms = s_rerun_ms = s_save_ms = s_load_ms = s_first_max = s_rerun_max = 0;
      s_first = s_reruns = s_saves = s_bench_loads = 0;
    }
  }
  s_last = clock::now();
}

bool SnapshotModesAllowed()
{
  static const bool allowed = [] {
    if (!Orca::Online::Enabled())
      return true;
    if (s_synctest_k > 0 || s_bench_k > 0 || s_loopback || s_save_only_from >= 0)
    {
      ERROR_LOG_FMT(ROLLBACK, "Harness: YG_SYNCTEST, YG_BENCH, YG_LOOPBACK and YG_SAVE_ONLY_FROM are "
                              "off in an online match (they save and load snapshots or play their "
                              "own session)");
    }
    return false;
  }();
  return allowed;
}

// ---- Loopback session (YG_LOOPBACK) ----

struct LoopbackRun
{
  std::vector<Loopback::RecordedFrame> recording;
  std::unique_ptr<Loopback::VirtualRemote> remote;
  std::unique_ptr<RingPort> port;
  std::unique_ptr<Orca::Net::Session> session;
  Orca::Net::Stats stats;
  double save_ms = 0;
  std::string error;
  bool started = false;
  // Frame at which the session gave up on a silent remote, or -1.
  int gave_up_at = -1;
  int remote_spikes = 0;
  bool spikes_started = false;
};

LoopbackRun& Lb()
{
  static LoopbackRun run;
  return run;
}

// Ends the session while emulation still runs, since RingPort's destructor resets the ring and
// NAND journal.
void EndLoopback()
{
  LoopbackRun& run = Lb();
  if (run.session)
    run.stats = run.session->GetStats();
  if (run.remote)
    run.remote_spikes = run.remote->Spikes();
  if (run.port)
    run.save_ms = run.port->SaveMs();
  run.session.reset();
  run.port.reset();
  run.remote.reset();
}

void LogLoopbackResult(int frame)
{
  const LoopbackRun& run = Lb();
  // Checksums must have kept flowing until the end, not just started.
  const int expected = run.stats.confirmed_frame / s_lb_checksum_every - 2;
  // With a silent remote, the session must give up after the silence starts, and every checksum
  // before that must still match.
  const bool pass = s_lb_silent_from >= 0 ? run.gave_up_at >= s_lb_silent_from &&
                                                run.stats.checksums_matched >= expected :
                                            run.error.empty() && run.stats.checksums_matched > 0 &&
                                                run.stats.checksums_matched >= expected;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Loopback done at frame {} ({}): {} rollbacks, {} re-run frames, deepest {}, {} "
                 "stalls, {} checksums matched, confirmed frame {}, {} snapshots ({:.2f} per "
                 "frame, every {}, {:.2f} ms each){}{}{}",
                 frame, pass ? "PASS" : "FAIL", run.stats.rollbacks, run.stats.resimulated_frames,
                 run.stats.deepest_rollback, run.stats.stalls, run.stats.checksums_matched,
                 run.stats.confirmed_frame, run.stats.saves,
                 frame > 0 ? static_cast<double>(run.stats.saves) / frame : 0.0,
                 run.stats.snapshot_every, run.save_ms,
                 run.remote_spikes > 0 ? fmt::format("; {} link spikes", run.remote_spikes) : "",
                 run.error.empty() ? "" : "; error: ", run.error);
}

void LoopbackBoundary(Core::System& system, int frame_here, bool first_pass)
{
  LoopbackRun& run = Lb();
  if (!run.started)
  {
    run.started = true;
    run.recording = Loopback::LoadRecording(s_loopback);
    if (run.recording.empty())
    {
      run.error = fmt::format("no usable recording in {}", s_loopback);
      ERROR_LOG_FMT(ROLLBACK, "Loopback: {}", run.error);
      return;
    }
    if (s_lb_seats == 4 && Orca::TestPads() != 4)
    {
      run.error = "YG_LOOPBACK_SEATS=4 needs ORCA_TEST_PADS=4";
      ERROR_LOG_FMT(ROLLBACK, "Loopback: {}", run.error);
      run.recording.clear();
      return;
    }
    // With two seats, ports 3 and 4 would replay as neutral and cause a false desync.
    for (const Loopback::RecordedFrame& recorded : run.recording)
    {
      if (s_lb_seats == 2 &&
          (recorded.pads[2] != Orca::Net::Pad{} || recorded.pads[3] != Orca::Net::Pad{}))
      {
        run.error = "the recording has input on ports 3-4: set YG_LOOPBACK_SEATS=4";
        ERROR_LOG_FMT(ROLLBACK, "Loopback: {}", run.error);
        run.recording.clear();
        return;
      }
    }
    Orca::Net::Config config;
    config.seats = s_lb_seats;
    config.local_seat = 0;
    config.input_delay = s_lb_delay;
    // The recording fixes which frame each local input lands on, so the delay cannot adapt. Time
    // sync stays on, since it only changes timing.
    config.max_delay = s_lb_delay;
    config.max_rollback = s_lb_max_rollback;
    config.checksum_every = s_lb_checksum_every;
    config.snapshot_every = Orca::TestSnapshotEvery();
    std::vector<int> remote_seats;
    for (int seat = 1; seat < s_lb_seats; ++seat)
      remote_seats.push_back(seat);
    run.remote = std::make_unique<Loopback::VirtualRemote>(run.recording, std::move(remote_seats),
                                                           0, s_lb_lag, s_lb_jitter, s_lb_delay,
                                                           s_lb_checksum_every);
    run.remote->SilenceFrom(s_lb_silent_from);
    // YG_THROTTLE=1 runs the session in real time, as in a match, to show what re-runs cost.
    run.port = std::make_unique<RingPort>(system, config.max_rollback, !Diag::KeepThrottle());
    run.session = std::make_unique<Orca::Net::Session>(config, *run.port, *run.remote);
    NOTICE_LOG_FMT(ROLLBACK,
                   "Loopback{}: {} recorded frames, {} seats, remote lag {}+0..{}, input delay {}, "
                   "max rollback {}, checksum every {}, snapshot every {}",
                   Diag::KeepThrottle() ? " (real time)" : "",
                   run.recording.size(), s_lb_seats, s_lb_lag, s_lb_jitter, s_lb_delay,
                   s_lb_max_rollback, s_lb_checksum_every,
                   config.snapshot_every > 0 ? fmt::to_string(config.snapshot_every) : "auto");
  }
  if (!run.session)
    return;

  if (first_pass)
  {
    if (s_lb_spikes.period_ms > 0 && !run.spikes_started && frame_here >= s_lb_spikes.from)
    {
      run.spikes_started = true;
      run.remote->StartSpikes(std::chrono::milliseconds{s_lb_spikes.period_ms},
                              std::chrono::milliseconds{s_lb_spikes.length_ms}, s_lb_max_rollback);
      NOTICE_LOG_FMT(ROLLBACK, "Loopback: link spikes from frame {}: {} ms every {} ms",
                     frame_here, s_lb_spikes.length_ms, s_lb_spikes.period_ms);
    }
    run.remote->Tick();
  }
  // frame_here runs next; the local pad sampled now applies input_delay frames later.
  const std::size_t target = static_cast<std::size_t>(frame_here + s_lb_delay);
  const Orca::Net::Pad local =
      target < run.recording.size() ? run.recording[target].pads[0] : Orca::Net::Pad{};
  bool ran_past_recording = false;
  bool gave_up = false;
  const Orca::Net::Step step = StepSession(
      *run.session, *run.port, frame_here - 1, [&local] { return local; },
      [&] {
        // The remote keeps playing while we wait, until its recording runs out.
        ran_past_recording = run.remote->Exhausted();
        run.remote->TickWhileWaiting();
        return ran_past_recording;
      },
      // With link spikes, poll like a match does instead of spinning.
      std::chrono::microseconds{s_lb_spikes.period_ms > 0 ? 1000 : 0},
      std::chrono::milliseconds{s_lb_give_up_ms}, &gave_up);

  if (step.kind == Orca::Net::StepKind::Ended)
  {
    if (gave_up)
      run.gave_up_at = frame_here;
    run.error = gave_up ? fmt::format("the remote stopped answering ({} ms)", s_lb_give_up_ms) :
                ran_past_recording           ? "ran past the end of the recording" :
                run.session->Error().empty() ? "stopped while stalled" :
                                               run.session->Error();
    ERROR_LOG_FMT(ROLLBACK, "Loopback session ended at frame {}: {}", frame_here, run.error);
    EndLoopback();
    LogLoopbackResult(frame_here);
    if (!s_stop_queued)
    {
      s_stop_queued = true;
      Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
    }
    return;
  }
  if (step.kind == Orca::Net::StepKind::Rollback)
    RewindTo(step.frame);
}
}  // namespace

void RewindTo(int frame)
{
  s_rewind_to = frame;
}

void Restart()
{
  s_restart = true;
}

bool Active()
{
  return s_scenes || s_synctest_k > 0 || s_exit_after > 0 || !Input().empty() ||
         s_shot_every > 0 || !s_shot_at.empty() || s_hashlog != nullptr || s_bench_k > 0 ||
         s_padrec != nullptr || s_loopback != nullptr || s_save_only_from >= 0 ||
         s_dump_ram_at >= 0 || s_dump_frames_from >= 0 || !s_nand_hashes_at.empty() ||
         !s_clear_jit_at.empty() ||
         s_clear_jit_every > 0 || !s_watch.empty() || !s_jit_invalidate.empty() ||
         !s_jit_census_at.empty();
}

int ShownFrame()
{
  return s_shown_frame.load(std::memory_order_relaxed);
}

namespace
{
// The YouGame shortcut applied to port 1's script. Each frame's result is kept so re-runs get
// exactly what the first run got.
struct ComboFrame
{
  int frame = -1;
  u16 buttons = 0;
};
std::array<ComboFrame, 256> s_combo_frames;
int s_combo_last = -1;  // the last frame stepped
Orca::UX::OrbCombo s_combo;

u16 ComboButtons(int frame, u16 buttons)
{
  ComboFrame& slot = s_combo_frames[static_cast<std::size_t>(frame) % s_combo_frames.size()];
  if (frame <= s_combo_last)
    return slot.frame == frame ? slot.buttons : buttons;
  const Orca::UX::OrbCombo::Step step = s_combo.Next(buttons);
  slot = ComboFrame{frame, step.buttons};
  s_combo_last = frame;
  if (step.fired)
    NOTICE_LOG_FMT(ROLLBACK, "Harness: the YouGame shortcut (Up + Start) at frame {}", frame);
  return step.buttons;
}

// ORCA_UX_TEST_UNPLUG_AT=<frame>:<port> (port 2-4, harness runs only): that test port leaves at
// that frame, as when a friend leaves: it drops out of the frame hook's ports and its pad reports
// no controller (UNPLUGGED_PAD).
struct TestUnplug
{
  int from = -1;
  int port = -1;  // 0-based
};

const TestUnplug& TestUnplugSpec()
{
  static const TestUnplug s_unplug = [] {
    TestUnplug out;
    const std::string spec = Orca::GetEnv("ORCA_UX_TEST_UNPLUG_AT");
    if (spec.empty())
      return out;
    int from = -1, port = -1;
    if (std::sscanf(spec.c_str(), "%d:%d", &from, &port) == 2 && from >= 0 && port >= 2 &&
        port <= 4)
    {
      out.from = from;
      out.port = port - 1;
      NOTICE_LOG_FMT(ROLLBACK, "ORCA_UX_TEST_UNPLUG_AT: port {} leaves at frame {}", port, from);
    }
    else
    {
      ERROR_LOG_FMT(ROLLBACK, "ORCA_UX_TEST_UNPLUG_AT={}: not <frame>:<port 2-4>", spec);
    }
    return out;
  }();
  return s_unplug;
}

bool TestUnplugged(int port, int frame)
{
  const TestUnplug& u = TestUnplugSpec();
  return u.from >= 0 && port == u.port && frame >= u.from && SnapshotModesAllowed();
}

// Whether test port `port` (0-based) reports no controller for `frame`: one that left
// (ORCA_UX_TEST_UNPLUG_AT), and with ORCA_UX_TEST_PADS_FOLLOW_PLUGS=1 any port outside the frame
// hook's ports for that frame (beyond ORCA_UX_TEST_NAMES, or before ORCA_UX_TEST_PLUG_AT), as in a
// session. Port 1 always has its pad. Not for ORCA_TEST_QUEUE_PICK.
bool TestPortUnplugged(int port, int frame)
{
  if (TestUnplugged(port, frame))
    return true;
  static const bool s_follow = Orca::GetEnv("ORCA_UX_TEST_PADS_FOLLOW_PLUGS") == "1";
  if (!s_follow || port <= 0 || !SnapshotModesAllowed())
    return false;
  static const int s_names = [] {
    const std::string names = Orca::GetEnv("ORCA_UX_TEST_NAMES");
    if (names.empty())
      return 1;
    return std::min<int>(4, 1 + static_cast<int>(std::count(names.begin(), names.end(), ',')));
  }();
  static const int s_plug_at = EnvInt("ORCA_UX_TEST_PLUG_AT", -1);
  if (port >= s_names)
    return true;
  return s_plug_at >= 0 && frame < s_plug_at;
}
}  // namespace

std::optional<GCPadStatus> InputOverride(int port)
{
  // A test port nobody plugged in reports no controller, scripted or not.
  if (TestPortUnplugged(port, s_frame))
    return Orca::Net::DecodePad(Orca::Net::UNPLUGGED_PAD);
  if (Input().empty())
    return std::nullopt;
  const int f = s_frame;
  // ORCA_UX_TEST_RESEAT_AT: port 1's rules play the player's port; the other reports no controller.
  int rules_port = port;
  if (const int local = ReseatLocalPort(f); local >= 0 && port < 2)
  {
    if (port != local)
      return Orca::Net::DecodePad(Orca::Net::UNPLUGGED_PAD);
    rules_port = 0;
  }
  u16 buttons = 0;
  int sx = 128, sy = 128, cx = 128, cy = 128;
  for (const InputRule& r : Input())
  {
    if (r.port != rules_port)
      continue;
    int rf = f;  // frame in the rule's from/to terms
    if (!r.scene.empty())
    {
      if (r.scene != s_scene || (r.visit && r.visit != s_scene_visit))
        continue;
      rf = f - s_scene_start;
    }
    if (rf < r.from || rf > r.to)
      continue;
    if (r.period > 0 && (rf - r.from) % r.period >= 3)
      continue;
    if (r.fuzz_seed)
    {
      // Deterministic per (seed, port, 8-frame segment): a stick direction plus usually one
      // attack, jump or shield button, never Start.
      const u64 h = XXH3_64bits_withSeed(
          &f, 0, (u64{r.fuzz_seed} << 32) ^ (static_cast<u64>(port) << 24) ^ (f / 8));
      static constexpr u16 BUTTONS[] = {0,      0x0100, 0x0200, 0x0400, 0x0800,
                                        0x0010, 0x0020, 0x0100, 0x0200, 0};
      buttons |= BUTTONS[(h >> 8) % 10];
      const int dir = static_cast<int>((h >> 16) % 9);  // 0 centre, 1-8 directions
      static constexpr int DX[] = {0, 1, 1, 0, -1, -1, -1, 0, 1};
      static constexpr int DY[] = {0, 0, 1, 1, 1, 0, -1, -1, -1};
      const int magnitude = 60 + static_cast<int>((h >> 24) % 68);
      sx = 128 + DX[dir] * magnitude;
      sy = 128 + DY[dir] * magnitude;
      if (((h >> 32) & 7) == 0)  // occasional C-stick smash
      {
        cx = 128 + DX[(h >> 36) % 9] * 127;
        cy = 128 + DY[(h >> 36) % 9] * 127;
      }
      continue;
    }
    buttons |= r.buttons;
    if (r.sx >= 0)
      sx = r.sx;
    if (r.sy >= 0)
      sy = r.sy;
    if (r.cx >= 0)
      cx = r.cx;
    if (r.cy >= 0)
      cy = r.cy;
  }
  GCPadStatus pad;
  pad.button = buttons;
  pad.stickX = static_cast<u8>(sx);
  pad.stickY = static_cast<u8>(sy);
  pad.substickX = static_cast<u8>(cx);
  pad.substickY = static_cast<u8>(cy);
  pad.triggerLeft = (buttons & 0x0040) ? 255 : 0;
  pad.triggerRight = (buttons & 0x0020) ? 255 : 0;
  pad.analogA = 0;
  pad.analogB = 0;
  pad.isConnected = true;
  // Round-trip through the wire form so a recording replays exactly what the ports reported.
  GCPadStatus out = Orca::Net::DecodePad(Orca::Net::EncodePad(pad));
  // Outside an online match, port 1's script is the local pad, so filter the YouGame shortcut
  // (Up + Start) here. An online match filters it in its own read.
  if (port == 0 && !Orca::Online::Enabled())
    out.button = ComboButtons(f, out.button);
  return out;
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  if (!Active())
    return;
  auto& system = guard.GetSystem();
  // Run unthrottled unless a session paces the run.
  if (!Orca::Online::Enabled() && !RingPort::Active() && !Diag::KeepThrottle())
    Core::SetIsThrottlerTempDisabled(true);
  if (s_diag && s_synctest_k > 0 && s_frame >= s_synctest_from - 1)
    CoreTiming::g_rollback_event_trace = &s_frame_events;

  static const SceneReader s_scene_reader = SceneReaderForRunningGame();
  if (s_scene_reader && (s_scenes || s_synctest_k > 0 || !Input().empty()))
  {
    std::string scene = s_scene_reader(guard);
    if (scene != s_scene)
    {
      s_scene = std::move(scene);
      s_scene_start = s_frame;
      s_scene_entries[s_frame] = s_scene;
      s_scene_visit = static_cast<int>(
          std::count_if(s_scene_entries.begin(), s_scene_entries.end(),
                        [](const auto& entry) { return entry.second == s_scene; }));
    }
    s_scene_history[s_frame] = {s_scene, s_scene_start, s_scene_visit};
    if (s_scene_history.size() > 128)
      s_scene_history.erase(s_scene_history.begin());
    // Log only on first passes, against the last logged scene, so a scene a re-run undid gets
    // corrected at the next first pass. Skip boundaries where an online session just rolled back:
    // memory there is already the rewound frame's start, not this frame's end.
    if (s_scenes && s_frame > s_max_frame && s_rewind_to < 0 && s_scene != s_logged_scene)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Scene frame {}: {} -> {}", s_scene_start, s_logged_scene, s_scene);
      s_logged_scene = s_scene;
    }
  }

  const int frame_here = s_frame;
  const bool first_pass_here = frame_here > s_max_frame;  // false on re-runs
  if (first_pass_here)
    s_shown_frame.store(frame_here, std::memory_order_relaxed);
  // Call the in-game UI's frame hook as an online match would, before any save and on re-runs too.
  // Ports come from ORCA_UX_TEST_NAMES (comma-separated, else port 1 alone), with controls from
  // ORCA_UX_TEST_CONTROLS (comma-separated hex profiles in UX/NameTags.h layout, "-" for none).
  if (SnapshotModesAllowed())
  {
    static const std::vector<Orca::Events::PortInfo> s_test_ports = [] {
      const auto split = [](const std::string& list) {
        std::vector<std::string> items;
        for (size_t start = 0; !list.empty() && start <= list.size() && items.size() < 4;)
        {
          const size_t comma = std::min(list.find(',', start), list.size());
          items.push_back(list.substr(start, comma - start));
          start = comma + 1;
        }
        return items;
      };
      const std::vector<std::string> names = split(Orca::GetEnv("ORCA_UX_TEST_NAMES"));
      const std::vector<std::string> controls = split(Orca::GetEnv("ORCA_UX_TEST_CONTROLS"));
      std::vector<Orca::Events::PortInfo> ports;
      for (int port = 0; port < static_cast<int>(names.size()); ++port)
      {
        std::vector<u8> profile;
        if (static_cast<size_t>(port) < controls.size() && controls[port] != "-")
        {
          const std::string& hex = controls[port];
          if (hex.size() % 2 == 0 && std::all_of(hex.begin(), hex.end(), [](char c) {
                return std::isxdigit(static_cast<unsigned char>(c)) != 0;
              }))
          {
            for (size_t i = 0; i < hex.size(); i += 2)
              profile.push_back(
                  static_cast<u8>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
          }
          else
          {
            ERROR_LOG_FMT(ROLLBACK, "ORCA_UX_TEST_CONTROLS: port {}'s profile isn't hex: none",
                          port + 1);
          }
        }
        ports.push_back({port, names[port], port > 0, std::move(profile)});
      }
      if (ports.empty())
        ports.push_back({0, "", false, {}});
      return ports;
    }();
    // ORCA_UX_TEST_PLUG_AT=<frame>: ports after the first plug in at that frame, as when a friend
    // drops in.
    static const int s_plug_at = EnvInt("ORCA_UX_TEST_PLUG_AT", -1);
    const bool early = s_plug_at >= 0 && frame_here + 1 < s_plug_at && s_test_ports.size() > 1;
    std::vector<Orca::Events::PortInfo> base_ports =
        early ? std::vector<Orca::Events::PortInfo>(s_test_ports.begin(), s_test_ports.begin() + 1) :
                s_test_ports;
    // ORCA_UX_TEST_RESEAT_AT: only the player's port, as in a former joiner's solo game.
    if (const int local = ReseatLocalPort(frame_here + 1); local >= 0)
    {
      Orca::Events::PortInfo own = s_test_ports.front();
      own.port = local;
      own.remote = false;
      base_ports = {own};
    }
    // ORCA_UX_TEST_UNPLUG_AT=<frame>:<port>: that port leaves from that frame (TestUnplugged).
    std::erase_if(base_ports, [&](const Orca::Events::PortInfo& p) {
      return TestUnplugged(p.port, frame_here + 1);
    });
    // ORCA_TEST_QUEUE_PICK=<frame>:<char>:<costume>:<x>:<y>[:<rating>]: port 2 plugs in at that
    // frame with that queue identity, as a matched player does.
    struct TestPick
    {
      int from = -1;
      std::vector<u8> queue;
    };
    static const TestPick s_test_pick = [] {
      TestPick out;
      const std::string spec = Orca::TestQueuePickSpec();
      if (spec.empty())
        return out;
      std::vector<std::string> f;
      for (size_t start = 0;;)
      {
        const size_t colon = spec.find(':', start);
        f.push_back(spec.substr(start, colon == std::string::npos ? colon : colon - start));
        if (colon == std::string::npos)
          break;
        start = colon + 1;
      }
      if (f.size() < 5 || f.size() > 6)
      {
        ERROR_LOG_FMT(ROLLBACK, "ORCA_TEST_QUEUE_PICK={}: not <frame>:<char>:<costume>:<x>:<y>"
                      "[:<rating>]",
                      spec);
        return out;
      }
      Orca::UX::QueueApp::Identity id;
      id.character = std::atoi(f[1].c_str());
      id.costume = std::atoi(f[2].c_str());
      id.x = std::strtof(f[3].c_str(), nullptr);
      id.y = std::strtof(f[4].c_str(), nullptr);
      id.rating = f.size() == 6 ? std::atoi(f[5].c_str()) : -1;
      out.from = std::atoi(f[0].c_str());
      out.queue = Orca::UX::QueueApp::EncodeIdentity(id);
      NOTICE_LOG_FMT(ROLLBACK, "ORCA_TEST_QUEUE_PICK: port 2 from frame {}: character {} costume "
                     "{} at ({}, {})",
                     out.from, id.character, id.costume, id.x, id.y);
      return out;
    }();
    if (s_test_pick.from >= 0)
    {
      std::vector<Orca::Events::PortInfo> ports;
      for (const Orca::Events::PortInfo& p : base_ports)
      {
        if (p.port != 1)
          ports.push_back(p);
        else if (frame_here + 1 >= s_test_pick.from)
          ports.emplace_back(p).queue = s_test_pick.queue;
      }
      if (std::none_of(ports.begin(), ports.end(), [](const auto& p) { return p.port == 1; }) &&
          frame_here + 1 >= s_test_pick.from)
      {
        ports.push_back({1, "bo", true, {}, s_test_pick.queue});
      }
      Orca::Events::NotifyFrame(guard, frame_here + 1, !first_pass_here, ports, first_pass_here);
    }
    else
    {
      Orca::Events::NotifyFrame(guard, frame_here + 1, !first_pass_here, base_ports,
                                first_pass_here);
    }
    // The queue's casual ready timer for port 1; it logs when it runs out.
    if (first_pass_here && !Orca::TestQueueSpec().empty())
      Orca::UX::QueueApp::ConfirmTimeout(frame_here + 1, 0);
    // YG_RESULTS=1: log the match results reader's output (the module's Verdicts,
    // Engine/Module.h). Frames are final at their first pass here.
    static const bool s_results = Orca::GetEnv("YG_RESULTS") == "1";
    if (s_results && first_pass_here)
    {
      for (const Orca::Engine::GameResult& r :
           Orca::UX::Game().Results().Confirm(frame_here + 1, -1))
      {
        NOTICE_LOG_FMT(ROLLBACK, "Results: frame {}: {}{}{}", r.frame,
                       r.kind == Orca::Engine::GameResult::Kind::Win ?
                           fmt::format("port {} won", r.winner_port + 1) :
                       r.kind == Orca::Engine::GameResult::Kind::Draw ? std::string("a draw") :
                                                                        "void " + r.why,
                       r.kind == Orca::Engine::GameResult::Kind::Void ? "" : " ",
                       r.kind == Orca::Engine::GameResult::Kind::Void ? "" : r.DetailJson());
      }
    }
  }
  if (first_pass_here)
    Diag::OnFirstPassBoundary(system, frame_here);
  if (s_hashlog && first_pass_here && s_synctest_k <= 0)
  {
    const auto regions = HashRegions(system);
    static int local_seats = 1;
    const int count = OnlineMatch::LocalSeatCount();
    if (count != local_seats)
    {
      std::fprintf(s_hashlog, "# localSeats %d\n", count);
      local_seats = count;
    }
    std::fprintf(s_hashlog, "%d %s %016llx\n", frame_here, s_scene.c_str(),
                 static_cast<unsigned long long>(
                     XXH3_64bits(regions.data(), regions.size() * sizeof(u64))));
    if (frame_here % 60 == 0)
      std::fflush(s_hashlog);
  }
  if (first_pass_here &&
      ((s_shot_every > 0 && frame_here >= s_shot_from && frame_here % s_shot_every == 0) ||
       std::find(s_shot_at.begin(), s_shot_at.end(), frame_here) != s_shot_at.end()))
  {
    Core::SaveScreenShot(fmt::format("f{:06d}", frame_here));
  }
  // For YG_PADREC: the RAM checksum at the start of the next frame. Its pads are written below.
  u64 recorded_checksum = 0;
  bool record_here = s_padrec && first_pass_here && s_rewind_to < 0;
  if (record_here)
  {
    auto& memory = system.GetMemory();
    recorded_checksum = RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                                    {memory.GetEXRAM(), memory.GetExRamSize()});
  }

  if (first_pass_here && s_dump_frames_from >= 0 && frame_here == s_dump_frames_from)
    Config::SetCurrent(Config::MAIN_MOVIE_DUMP_FRAMES, true);
  if (first_pass_here && s_dump_frames_to >= 0 && frame_here == s_dump_frames_to)
    Config::SetCurrent(Config::MAIN_MOVIE_DUMP_FRAMES, false);
  if (first_pass_here && frame_here == s_dump_ram_at)
  {
    auto& memory = system.GetMemory();
    const std::string base = File::GetUserPath(D_USER_IDX) + fmt::format("ram-{}", frame_here);
    File::IOFile(base + "-mem1.bin", "wb").WriteBytes(memory.GetRAM(), memory.GetRamSize());
    File::IOFile(base + "-mem2.bin", "wb").WriteBytes(memory.GetEXRAM(), memory.GetExRamSize());
    NOTICE_LOG_FMT(ROLLBACK, "RAM at frame {} written to {}-mem*.bin", frame_here, base);
  }
  const auto listed = [frame_here](const std::vector<u32>& frames) {
    return std::find(frames.begin(), frames.end(), static_cast<u32>(frame_here)) != frames.end();
  };
  if (first_pass_here && listed(s_jit_census_at))
    Diag::JitCodeCensus(guard, fmt::format("frame {}", frame_here).c_str());
  for (const auto& [at, address] : s_jit_invalidate)
  {
    if (first_pass_here && frame_here == at)
    {
      system.GetJitInterface().InvalidateICache(address, 4, true);
      NOTICE_LOG_FMT(ROLLBACK, "JIT blocks at {:08x} dropped at the end of frame {}", address, at);
    }
  }
  if (first_pass_here &&
      (listed(s_clear_jit_at) || (s_clear_jit_every > 0 && frame_here >= s_clear_jit_from &&
                                  frame_here % s_clear_jit_every == 0)))
  {
    if (Diag::g_jit_code_log)
      Diag::JitCodeNote("YG_CLEARJIT");
    system.GetJitInterface().ClearSafe();
    NOTICE_LOG_FMT(ROLLBACK, "JIT cleared at the end of frame {}", frame_here);
  }
  if (first_pass_here && !s_watch.empty())
  {
    static std::vector<u32> s_watched(s_watch.size(), 0);
    static bool s_watch_started = false;
    for (std::size_t i = 0; i < s_watch.size(); ++i)
    {
      const u32 value = PowerPC::MMU::HostRead<u32>(guard, s_watch[i]);
      if (!s_watch_started || value != s_watched[i])
      {
        NOTICE_LOG_FMT(ROLLBACK, "Watch frame {} ({}): {:08x} = {:08x}", frame_here, s_scene,
                       s_watch[i], value);
      }
      s_watched[i] = value;
    }
    s_watch_started = true;
  }
  if (Diag::g_jit_code_log && first_pass_here && frame_here % 600 == 0)
  {
    std::string stats;
    for (const auto& [name, free] : system.GetJitInterface().GetMemoryStats())
      stats += fmt::format(" {} {:.1f} MB free ({:.2f} fragmented)", name, free.first / 1048576.0,
                           free.second);
    Diag::JitCodeNote("JIT" + stats);
  }
  if (first_pass_here && std::find(s_nand_hashes_at.begin(), s_nand_hashes_at.end(), frame_here) !=
                             s_nand_hashes_at.end())
  {
    IOS::HLE::EmulationKernel* ios = system.GetIOS();
    const auto* nand = ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
    std::vector<Orca::Net::NandEntry> entries;
    if (nand && Orca::Net::ReadNandTree(nand->HostRoot(), &entries))
    {
      u64 total = 0;
      for (const Orca::Net::NandEntry& entry : entries)
      {
        if (entry.directory)
          continue;
        total += entry.data.size();
        NOTICE_LOG_FMT(ROLLBACK, "NAND at frame {}: {} {} {:016x}", frame_here, entry.path,
                       entry.data.size(), Orca::Net::NandHash(entry.data));
      }
      NOTICE_LOG_FMT(ROLLBACK, "NAND at frame {}: {} bytes in files", frame_here, total);
    }
  }
  // Not alongside a session: both rings would share the same NAND journal.
  if (s_save_only_from >= 0 && frame_here >= s_save_only_from && !s_loopback &&
      !RingPort::Active() && SnapshotModesAllowed())
  {
    Ring().Save(system, frame_here);
  }
  if (!SnapshotModesAllowed())
  {
    // An online match drives its own session.
  }
  else if (s_loopback)
  {
    LoopbackBoundary(system, frame_here, first_pass_here);
  }
  else if (s_synctest_k > 0 && frame_here >= s_synctest_from)
  {
    SyncTest(system);
  }
  else if (s_keyframe_test >= 0)
  {
    KeyframeTest(system, frame_here, first_pass_here);
  }
  else if (s_bench_k > 0)
  {
    Bench(system, first_pass_here);
  }

  // A session rolled back here: the state is now the start of s_rewind_to.
  const bool rewound = s_rewind_to >= 0;
  if (rewound)
  {
    s_frame = s_rewind_to;
    RestoreScene(s_rewind_to);
    s_rewind_to = -1;
  }
  ++s_frame;
  if (Diag::g_jit_code_log)
    Diag::JitCodeSetFrame(s_frame);
  s_max_frame = std::max(s_max_frame, frame_here);
  // A new timeline from the rewind on (Restart): its frames are first passes, so scenes, results
  // and the first-pass hooks follow it rather than the frame numbers the old one reached.
  if (std::exchange(s_restart, false) && rewound)
  {
    s_max_frame = s_frame - 1;
    s_scene_history.clear();
  }
  // The next frame is a re-run if it has run before. Sessions decide this themselves.
  if (!s_loopback && !RingPort::Active())
    SetResimulating(s_frame <= s_max_frame && !s_render_reruns);

  // Skip the line if a session rolled back here and replaced the state and pads.
  record_here = record_here && s_frame == frame_here + 1;
  if (record_here)
  {
    Orca::Net::Pads pads{};
    for (int port = 0; port < Orca::Net::MAX_SEATS; ++port)
    {
      // Record the raw pad for the next frame, before the input gate, as the session sends it.
      if (const auto pad = Rollback::RawInputOverride(port))
        pads[port] = Orca::Net::EncodePad(*pad);
    }
    std::fputs(Loopback::FormatRecordedFrame(frame_here, pads, recorded_checksum).c_str(),
               s_padrec);
    if (frame_here % 60 == 0)
      std::fflush(s_padrec);
  }

  // Online: stats were logged at the exit frame. Stop a bit later so the other player reaches its
  // own exit frame before this one leaves the room.
  if (s_online_stop_at >= 0 && first_pass_here && frame_here >= s_online_stop_at &&
      !s_stop_queued)
  {
    s_stop_queued = true;
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
  }

  // YG_EXIT_AFTER: stop at the end of this first-pass frame, in every mode.
  if (s_exit_after > 0 && first_pass_here && frame_here >= s_exit_after && !s_stop_queued &&
      s_online_stop_at < 0)
  {
    s_stop_queued = true;
    Diag::LogSummary();
    NOTICE_LOG_FMT(ROLLBACK,
                   "Harness done at frame {}: {} re-run frames checked, {} RAM mismatches ({}), {} "
                   "device-state-only mismatches, {} loads",
                   frame_here, s_checked, s_ram_mismatches,
                   s_synctest_k > 0 ? (s_ram_mismatches == 0 ? "PASS" : "FAIL") : "no sync test",
                   s_mismatches - s_ram_mismatches, s_loads);
    if (s_sections && s_synctest_k > 0)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Synctest device-state sections differing (re-run frames): {}",
                     DescribeSectionDiffs());
    }
    // Matches across runs with and without rollback if NAND writes inside rollback windows are
    // undone correctly.
    if (Orca::SessionActive())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Session save hash at exit: {:016x}",
                     Orca::HashSessionSave(SConfig::GetInstance().GetTitleID()));
    }
    if (s_loopback && SnapshotModesAllowed())
    {
      EndLoopback();
      LogLoopbackResult(frame_here);
    }
    if (s_padrec)
      std::fflush(s_padrec);
    if (OnlineMatch::Active())
    {
      OnlineMatch::LogStats(fmt::format("at exit frame {}", frame_here).c_str());
      s_stop_queued = false;
      s_online_stop_at = frame_here + 180;
      return;
    }
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
  }
}
}  // namespace Rollback::Harness
