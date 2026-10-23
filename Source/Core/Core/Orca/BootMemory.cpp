// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/BootMemory.h"

#include <atomic>
#include <cstdio>
#include <new>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/Logging/LogManager.h"
#include "Core/Orca/GpuDiagnostics.h"
#include "Core/Orca/Status.h"

namespace Orca::BootMemory
{
namespace
{
std::atomic<bool> s_first_frame{false};
}

void BeginBoot()
{
  s_first_frame.store(false, std::memory_order_relaxed);
  LogMemory();
#ifdef _WIN32
  GpuDiagnostics::ResetScene();
#endif
}

void FirstFrame()
{
  s_first_frame.store(true, std::memory_order_relaxed);
}

void LogMemory()
{
  constexpr u64 MB = 1024 * 1024;
  char line[160];
#ifdef _WIN32
  GpuDiagnostics::LogMemory(true);
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (!GlobalMemoryStatusEx(&memory))
    return;
  std::snprintf(line, sizeof(line), "memory: total %llu MB, available %llu MB, commit %llu/%llu MB",
                static_cast<unsigned long long>(memory.ullTotalPhys / MB),
                static_cast<unsigned long long>(memory.ullAvailPhys / MB),
                static_cast<unsigned long long>((memory.ullTotalPageFile - memory.ullAvailPageFile) /
                                                MB),
                static_cast<unsigned long long>(memory.ullTotalPageFile / MB));
#else
  u64 total = 0, available = 0;
#if defined(__APPLE__)
  size_t size = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) != 0)
    return;
  const mach_port_t host = mach_host_self();
  vm_size_t page_size = 0;
  vm_statistics64_data_t statistics{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  const bool ok = host_page_size(host, &page_size) == KERN_SUCCESS &&
                  host_statistics64(host, HOST_VM_INFO64,
                                    reinterpret_cast<host_info64_t>(&statistics), &count) ==
                      KERN_SUCCESS;
  mach_port_deallocate(mach_task_self(), host);
  if (!ok)
    return;
  available = (static_cast<u64>(statistics.free_count) + statistics.inactive_count) * page_size;
#elif defined(__linux__)
  std::FILE* const file = std::fopen("/proc/meminfo", "r");
  if (!file)
    return;
  char meminfo_line[256];
  while (std::fgets(meminfo_line, sizeof(meminfo_line), file))
  {
    unsigned long long kb = 0;
    if (std::sscanf(meminfo_line, "MemTotal: %llu kB", &kb) == 1)
      total = kb * 1024;
    else if (std::sscanf(meminfo_line, "MemAvailable: %llu kB", &kb) == 1)
      available = kb * 1024;
  }
  std::fclose(file);
#else
  return;
#endif
  std::snprintf(line, sizeof(line), "memory: total %llu MB, available %llu MB",
                static_cast<unsigned long long>(total / MB),
                static_cast<unsigned long long>(available / MB));
#endif
  // Boot/error diagnostics go through the normal listeners even with category filtering off.
  if (auto* const logs = Common::Log::LogManager::GetInstance())
  {
    logs->LogWithFullPath(Common::Log::LogLevel::LNOTICE, Common::Log::LogType::ROLLBACK,
                          "Orca/BootMemory.cpp", 0, line);
  }
}

bool Run(void (*step)(void*), void* context)
{
  try
  {
    step(context);
    return true;
  }
  catch (const std::bad_alloc&)
  {
    if (!AllocationFailed())
      throw;
    return false;
  }
}

bool AllocationFailed()
{
  if (s_first_frame.load(std::memory_order_relaxed))
    return false;
  Status::Error("low_memory", "Your PC ran out of memory while starting the game. Close other apps "
                              "first.");
  return true;
}
}  // namespace Orca::BootMemory
