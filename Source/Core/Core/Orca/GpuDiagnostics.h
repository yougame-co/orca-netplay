// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Orca::GpuDiagnostics
{
using Clock = std::chrono::steady_clock;
// Callers serialize and update last only when taking a sample. Boot, error and loss samples
// force a snapshot; periodic samples share a sixty-second limit across successive boots.
bool SampleDue(Clock::time_point now, std::optional<Clock::time_point> last, bool force = false);
std::optional<u64> FrameNumber(int frame);

struct Memory
{
  std::optional<u64> commit, limit, available, working_set, private_bytes;
  std::string_view job = "none";  // none, yes, or unknown
  std::optional<u64> job_peak, job_limit;
  bool process_limit = false;
};
struct Segment
{
  u32 result = 0;
  u64 budget = 0, usage = 0, reservation = 0;
};
struct Adapter
{
  std::string_view description, driver;
  u32 vendor = 0, device = 0;
  u64 video = 0, system = 0, shared = 0;
};
struct Render
{
  std::string_view backend, mode, scene = "unknown";
  u32 feature_level = 0, removed_reason = 0;
  u32 scale = 0, pixel = 0, vertex = 0, uber_pixel = 0, uber_vertex = 0;
  u64 presented = 0;
  std::optional<u64> frame, scene_frame;
};
// Byte counts are displayed in whole MiB (labelled MB, as in existing Orca logs). Unknown
// values remain unknown rather than looking like zero. All output is bounded and terminated.
size_t FormatMemory(char* out, size_t size, const Memory& memory);
size_t FormatGpuMemory(char* out, size_t size, const Segment& local, const Segment& non_local);
size_t FormatAdapter(char* out, size_t size, const Adapter& adapter);
size_t FormatRender(char* out, size_t size, const Render& render);
size_t FormatTdr(char* out, size_t size, const std::string_view (&values)[3]);

#ifdef _WIN32
void Log(const char* line);
void LogMemory(bool force = false);
void LogTdrOnce();
void ResetScene();
void NoteFrame(int frame);
void NoteScene(std::string_view scene, int frame);
// Counts successful GX shaders, including disk cache loads, cumulatively for this boot.
// Recompiles after a graphics reload count again; failed and pending shaders do not.
void ShaderCreated(bool pixel, bool uber);
void LogRender(Render render);
#endif
}  // namespace Orca::GpuDiagnostics
