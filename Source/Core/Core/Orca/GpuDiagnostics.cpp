// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/GpuDiagnostics.h"

#include "Core/Orca/CrashNote.h"

#ifdef _WIN32
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <windows.h>
#include <psapi.h>

#include "Common/Logging/LogManager.h"
#endif

namespace Orca::GpuDiagnostics
{
namespace
{
using Line = CrashNote::FixedLine;
constexpr u64 MB = 1024 * 1024;

void Number(Line& line, std::optional<u64> value, u64 divisor = 1)
{
  if (value)
    line.Dec(*value / divisor);
  else
    line << "unknown";
}

void OneLine(Line& line, std::string_view value)
{
  for (const char c : value)
    line << (static_cast<unsigned char>(c) < 32 ? std::string_view(" ") : std::string_view(&c, 1));
}
}  // namespace

bool SampleDue(Clock::time_point now, std::optional<Clock::time_point> last, bool force)
{
  return force || !last || (now >= *last && now - *last >= std::chrono::seconds(60));
}

std::optional<u64> FrameNumber(int frame)
{
  if (frame < 0)
    return std::nullopt;
  return static_cast<u64>(frame);
}

size_t FormatMemory(char* out, size_t size, const Memory& m)
{
  Line line(out, size);
  line << "orca mem commit ";
  Number(line, m.commit, MB);
  line << "/";
  Number(line, m.limit, MB);
  line << " MB avail ";
  Number(line, m.available, MB);
  line << " MB ws ";
  Number(line, m.working_set, MB);
  line << " MB priv ";
  Number(line, m.private_bytes, MB);
  line << " MB " << (m.process_limit ? "proc " : "job ");
  if (m.job == "yes")
  {
    line << "peak ";
    Number(line, m.job_peak, MB);
    line << "/";
    if (m.job_limit == 0)
      line << "unlimited";
    else
      Number(line, m.job_limit, MB);
    line << " MB";
  }
  else
  {
    line << m.job;
  }
  return line.Length();
}

size_t FormatGpuMemory(char* out, size_t size, const Segment& local, const Segment& non_local)
{
  Line line(out, size);
  line << "orca gpu mem";
  const auto segment = [&line](std::string_view name, const Segment& s) {
    line << " " << name;
    if (s.result != 0)
    {
      line << " unavailable ";
      line.Hex(s.result, 8);
      return;
    }
    line << " budget ";
    line.Dec(s.budget / MB) << " MB usage ";
    line.Dec(s.usage / MB) << " MB reservation-available ";
    line.Dec(s.reservation / MB) << " MB";
  };
  segment("LOCAL", local);
  segment("NON_LOCAL", non_local);
  return line.Length();
}

size_t FormatAdapter(char* out, size_t size, const Adapter& a)
{
  Line line(out, size);
  line << "orca gpu adapter ";
  OneLine(line, a.description);
  line << " vendor ";
  line.Hex(a.vendor, 4) << " device ";
  line.Hex(a.device, 4) << " driver ";
  OneLine(line, a.driver);
  line << " dedicated-video ";
  line.Dec(a.video / MB) << " MB dedicated-system ";
  line.Dec(a.system / MB) << " MB shared-system ";
  line.Dec(a.shared / MB) << " MB";
  return line.Length();
}

size_t FormatRender(char* out, size_t size, const Render& r)
{
  Line line(out, size);
  line << "orca gpu loss backend " << r.backend << " feature ";
  line.Hex(r.feature_level, 4) << " shaders " << r.mode << " scale ";
  line.Dec(r.scale) << " pixel ";
  line.Dec(r.pixel) << " vertex ";
  line.Dec(r.vertex) << " uber-pixel ";
  line.Dec(r.uber_pixel) << " uber-vertex ";
  line.Dec(r.uber_vertex) << " presented ";
  line.Dec(r.presented) << " scene ";
  OneLine(line, r.scene);
  line << " scene-at ";
  Number(line, r.scene_frame);
  line << " frame ";
  Number(line, r.frame);
  line << " removed ";
  line.Hex(r.removed_reason, 8);
  return line.Length();
}

size_t FormatTdr(char* out, size_t size, const std::string_view (&values)[3])
{
  Line line(out, size);
  line << "orca gpu tdr TdrDelay " << values[0] << " TdrDdiDelay " << values[1] << " TdrLevel "
       << values[2];
  return line.Length();
}

#ifdef _WIN32
namespace
{
std::mutex s_memory_mutex;
std::optional<Clock::time_point> s_last_memory;
std::mutex s_scene_mutex;
std::array<char, 64> s_scene{'u', 'n', 'k', 'n', 'o', 'w', 'n'};
std::optional<u64> s_scene_frame;
std::atomic<int> s_frame{-1};
std::array<std::atomic<u32>, 4> s_shaders{};
}  // namespace

void Log(const char* line)
{
  if (auto* const logs = Common::Log::LogManager::GetInstance())
    logs->LogWithFullPath(Common::Log::LogLevel::LNOTICE, Common::Log::LogType::ROLLBACK,
                         "Orca/GpuDiagnostics.cpp", 0, line);
}

void LogMemory(bool force)
{
  std::lock_guard lock(s_memory_mutex);
  const auto now = Clock::now();
  if (!SampleDue(now, s_last_memory, force))
    return;
  Memory m;
  PERFORMANCE_INFORMATION perf{};
  perf.cb = sizeof(perf);
  if (GetPerformanceInfo(&perf, sizeof(perf)))
  {
    m.commit = static_cast<u64>(perf.CommitTotal) * perf.PageSize;
    m.limit = static_cast<u64>(perf.CommitLimit) * perf.PageSize;
    m.available = static_cast<u64>(perf.PhysicalAvailable) * perf.PageSize;
  }
  PROCESS_MEMORY_COUNTERS_EX process{};
  process.cb = sizeof(process);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
  {
    m.working_set = process.WorkingSetSize;
    m.private_bytes = process.PrivateUsage;
  }
  BOOL in_job = FALSE;
  if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in_job))
    m.job = "unknown";
  else if (in_job)
  {
    m.job = "yes";
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof(job),
                                  nullptr))
    {
      const DWORD flags = job.BasicLimitInformation.LimitFlags;
      m.process_limit = !(flags & JOB_OBJECT_LIMIT_JOB_MEMORY) &&
                        (flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY);
      m.job_peak = m.process_limit ? job.PeakProcessMemoryUsed : job.PeakJobMemoryUsed;
      m.job_limit = (flags & JOB_OBJECT_LIMIT_JOB_MEMORY) ? job.JobMemoryLimit :
                   m.process_limit                     ? job.ProcessMemoryLimit :
                                                         0;
    }
  }
  char text[384];
  FormatMemory(text, sizeof(text), m);
  Log(text);
  s_last_memory = Clock::now();
}

void ResetScene()
{
  std::lock_guard lock(s_scene_mutex);
  s_scene.fill(0);
  std::copy_n("unknown", 7, s_scene.begin());
  s_scene_frame.reset();
  s_frame.store(-1, std::memory_order_relaxed);
  for (auto& count : s_shaders)
    count.store(0, std::memory_order_relaxed);
}

void NoteFrame(int frame)
{
  s_frame.store(frame, std::memory_order_relaxed);
}

void NoteScene(std::string_view scene, int frame)
{
  std::lock_guard lock(s_scene_mutex);
  const size_t size = std::min(scene.size(), s_scene.size() - 1);
  std::copy_n(scene.data(), size, s_scene.begin());
  s_scene[size] = '\0';
  s_scene_frame = FrameNumber(frame);
}

void ShaderCreated(bool pixel, bool uber)
{
  s_shaders[(uber ? 2 : 0) + (pixel ? 0 : 1)].fetch_add(1, std::memory_order_relaxed);
}

void LogRender(Render render)
{
  std::lock_guard lock(s_scene_mutex);
  render.scene = s_scene.data();
  render.scene_frame = s_scene_frame;
  const int frame = s_frame.load(std::memory_order_relaxed);
  if (frame >= 0)
    render.frame = static_cast<u64>(frame);
  render.pixel = s_shaders[0].load(std::memory_order_relaxed);
  render.vertex = s_shaders[1].load(std::memory_order_relaxed);
  render.uber_pixel = s_shaders[2].load(std::memory_order_relaxed);
  render.uber_vertex = s_shaders[3].load(std::memory_order_relaxed);
  char text[512];
  FormatRender(text, sizeof(text), render);
  Log(text);
}
#endif
}  // namespace Orca::GpuDiagnostics
