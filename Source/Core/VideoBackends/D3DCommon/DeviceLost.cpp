// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3DCommon/DeviceLost.h"

#include <atomic>

#include "Common/CommonTypes.h"

#include "Core/Orca/GpuLost.h"

namespace D3DCommon
{
namespace
{
std::atomic<RemovedReasonQuery> s_query{nullptr};
std::atomic<const char*> s_backend{"D3D"};

// A failed call's own word that the device is gone.
bool LostResult(HRESULT hr)
{
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
         hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
}  // namespace

void SetDeviceRemovedQuery(const char* backend, RemovedReasonQuery query)
{
  s_backend.store(backend ? backend : "D3D");
  s_query.store(query);
  if (query)
    Orca::GpuLost::DeviceCreated(s_backend.load());
  else
    Orca::GpuLost::DeviceClosed(s_backend.load());
}

bool CheckDeviceLost(HRESULT hr, std::string_view what)
{
  if (Orca::GpuLost::Lost())
    return true;
  if (SUCCEEDED(hr))
    return false;
  const RemovedReasonQuery query = s_query.load();
  HRESULT reason = query ? query() : S_OK;
  // A call that says the device is gone is believed even if the device doesn't say why.
  if (reason == S_OK && LostResult(hr))
    reason = hr;
  if (reason == S_OK)
    return false;
  Orca::GpuLost::Report(s_backend.load(), static_cast<u32>(reason), what);
  return true;
}

bool IsDeviceLost()
{
  return Orca::GpuLost::Lost();
}
}  // namespace D3DCommon
