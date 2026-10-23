// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

#include "Common/CommonTypes.h"

// A lost graphics device ends the run. Windows can remove a D3D device (a GPU hang the driver reset,
// a driver update, the GPU itself gone); nothing draws on it again. Orca used to keep going on it:
// a failed texture became a null one, a failed map a write through a null pointer, and Orca died a
// moment later with an access violation (0xC0000005), or an assert ended it first (0x80000003).
// Now the video backend that first sees the device gone reports it here, once:
//   - a "[VIDEO]" line on stderr, which the app keeps as the run's log (FormatLine),
//   - "orca error gpu_lost <sentence>" for the app (Status.h), so the page says what happened and
//     what to try instead of "Orca crashed",
//   - and a stop, the same one a desync or a failed restore asks for: the core stops, the room is
//     left, and an online opponent sees this player leave.
// The backend draws and presents nothing more meanwhile (D3DCommon/DeviceLost.h).
namespace Orca::GpuLost
{
// The code of the app's error line ("orca error gpu_lost <SENTENCE>").
inline constexpr std::string_view CODE = "gpu_lost";
inline constexpr std::string_view SENTENCE =
    "Your graphics driver stopped responding. Try the Performance preset or a lower resolution in "
    "Graphics, and update your GPU driver.";

// DXGI's name for a device-removed reason or a failed call's result ("device hung"), or "error".
const char* ReasonName(u32 hr);

// The run log's line, e.g.
//   Orca: [VIDEO] the graphics device is lost (D3D11: device hung, 0x887A0006) at a 16x16x1
//   texture; Orca stops
// into `out` (NUL-terminated, cut to fit); returns its length. No allocation: it runs where a throw
// ends Orca at once (CrashNote.h).
size_t FormatLine(char* out, size_t size, std::string_view backend, u32 reason,
                  std::string_view where);

// Reports the loss, once per run, from any thread: the line, the device state crash notes carry,
// the app's error and the stop. `reason` is what the device says (GetDeviceRemovedReason), `where`
// what failed ("present", "a 16x16x1 texture"). Returns true the first time. A loss seen while the
// core already stops is only logged.
bool Report(std::string_view backend, u32 reason, std::string_view where);

// Whether this run's device was reported lost.
bool Lost();

// A backend made a new device: not lost, and the device state says so ("D3D11 running").
void DeviceCreated(std::string_view backend);
// It let its device go ("D3D11 closed"), unless it was lost: the loss stays the state.
void DeviceClosed(std::string_view backend);

// The last device state, for a crash note (CrashNote.h): "D3D11 running", "D3D11 closed", or the
// loss ("lost (D3D11: device hung, 0x887A0006) at present").
// Copies it into `out` (NUL-terminated, cut to fit) and returns its length; 0 when no backend set
// one. Lock-free and allocation-free, so an exception handler can call it.
size_t DeviceState(char* out, size_t size);
void SetDeviceState(std::string_view state);
}  // namespace Orca::GpuLost
