// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <dxgi.h>

// Orca: the D3D backends' one path for a lost device (Orca/GpuLost.h). After a call that failed
// (a present, a texture, a map, the swap chain's buffers), CheckDeviceLost asks the device why; a
// removed device is reported once and the run stops, and from then on the backend draws, presents
// and creates nothing: every call on a dead device only fails again, and a failure that slips
// through is how Orca used to crash.
namespace D3DCommon
{
// The backend's device says why it was removed (GetDeviceRemovedReason), S_OK while it works. Set
// when the device is made (it starts out not lost), cleared before it is destroyed. Without one the
// failed call's own result decides (DXGI_ERROR_DEVICE_REMOVED and the like).
using RemovedReasonQuery = HRESULT (*)();
using LossDiagnostics = void (*)(HRESULT removed_reason);
void SetDeviceRemovedQuery(const char* backend, RemovedReasonQuery query,
                           LossDiagnostics diagnostics = nullptr);

// After a call that returned `hr`: whether the device is lost. The first time, the loss is
// reported naming `what` failed ("present", "a 16x16x1 texture"). True from then on, whatever
// `hr`; false for a failure that leaves the device working (out of memory, a bad argument).
bool CheckDeviceLost(HRESULT hr, std::string_view what);

// Whether the device was lost: nothing more is drawn or presented. Cheap (one atomic load).
bool IsDeviceLost();
}  // namespace D3DCommon
