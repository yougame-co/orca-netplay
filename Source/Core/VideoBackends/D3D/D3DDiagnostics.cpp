// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DDiagnostics.h"

#ifdef _WIN32
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "Common/StringUtil.h"
#include "Core/Orca/CrashNote.h"
#include "Core/Orca/GpuDiagnostics.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11::Diagnostics
{
namespace
{
namespace Diag = Orca::GpuDiagnostics;
std::mutex s_mutex;
ComPtr<IDXGIAdapter3> s_adapter;
std::optional<Diag::Clock::time_point> s_last_budget;
Diag::Render s_render;
std::atomic<u64> s_presented{0};

const char* Mode(ShaderCompilationMode mode)
{
  switch (mode)
  {
  case ShaderCompilationMode::Synchronous:
    return "synchronous";
  case ShaderCompilationMode::SynchronousUberShaders:
    return "synchronous-uber";
  case ShaderCompilationMode::AsynchronousUberShaders:
    return "asynchronous-uber";
  case ShaderCompilationMode::AsynchronousSkipRendering:
    return "asynchronous-skip";
  default:
    return "unknown";
  }
}

void Budget()
{
  if (!s_adapter)
    return;
  const auto query = [](DXGI_MEMORY_SEGMENT_GROUP group) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    const HRESULT hr = s_adapter->QueryVideoMemoryInfo(0, group, &info);
    return Diag::Segment{static_cast<u32>(hr), info.Budget, info.CurrentUsage,
                         info.AvailableForReservation};
  };
  char text[384];
  Diag::FormatGpuMemory(text, sizeof(text), query(DXGI_MEMORY_SEGMENT_GROUP_LOCAL),
                        query(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL));
  Diag::Log(text);
}
}  // namespace

void Initialize(IDXGIAdapter* adapter)
{
  std::lock_guard lock(s_mutex);
  s_adapter.Reset();
  s_last_budget.reset();
  s_presented.store(0, std::memory_order_relaxed);
  s_render = {};
  s_render.backend = "D3D11";
  s_render.mode = Mode(g_ActiveConfig.iShaderCompilationMode);
  s_render.feature_level = static_cast<u32>(D3D::feature_level);
  // The actual scale is refreshed once the framebuffer manager exists.
  s_render.scale = static_cast<u32>(g_ActiveConfig.iEFBScale);
  if (!adapter)
  {
    Diag::Log("orca gpu adapter unavailable");
    return;
  }
  DXGI_ADAPTER_DESC desc{};
  if (SUCCEEDED(adapter->GetDesc(&desc)))
  {
    const std::string description = WStringToUTF8(desc.Description);
    LARGE_INTEGER umd{};
    std::string driver = "unknown";
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
    {
      const auto high = static_cast<u32>(umd.HighPart);
      const auto low = static_cast<u32>(umd.LowPart);
      driver = fmt::format("{}.{}.{}.{}", high >> 16, high & 0xffff, low >> 16, low & 0xffff);
    }
    char text[768];
    Diag::FormatAdapter(text, sizeof(text),
                         {description, driver, desc.VendorId, desc.DeviceId,
                          desc.DedicatedVideoMemory, desc.DedicatedSystemMemory,
                          desc.SharedSystemMemory});
    Diag::Log(text);
  }
  else
  {
    Diag::Log("orca gpu adapter description unavailable");
  }
  const HRESULT hr = adapter->QueryInterface(IID_PPV_ARGS(&s_adapter));
  if (FAILED(hr))
  {
    char text[128];
    Orca::CrashNote::FixedLine line(text, sizeof(text));
    line << "orca gpu mem IDXGIAdapter3 unavailable ";
    line.Hex(static_cast<u32>(hr), 8);
    Diag::Log(text);
  }
}

void Shutdown()
{
  std::lock_guard lock(s_mutex);
  s_adapter.Reset();
}

void Poll()
{
  std::lock_guard lock(s_mutex);
  s_render.mode = Mode(g_ActiveConfig.iShaderCompilationMode);
  s_render.scale = g_framebuffer_manager ? g_framebuffer_manager->GetEFBScale() :
                                         static_cast<u32>(g_ActiveConfig.iEFBScale);
  const auto now = Diag::Clock::now();
  if (Diag::SampleDue(now, s_last_budget))
  {
    s_last_budget = now;
    Budget();
  }
}

void Presented()
{
  s_presented.fetch_add(1, std::memory_order_relaxed);
}

void Lost(u32 removed_reason)
{
  std::lock_guard lock(s_mutex);
  Budget();
  auto render = s_render;
  render.presented = s_presented.load(std::memory_order_relaxed);
  render.removed_reason = removed_reason;
  Diag::LogRender(render);
}
}  // namespace DX11::Diagnostics
#endif
