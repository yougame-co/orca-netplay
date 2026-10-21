// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

struct IDXGIAdapter;

namespace DX11::Diagnostics
{
void Initialize(IDXGIAdapter* adapter);
void Shutdown();
// GPU thread: refresh the render snapshot and sample budgets when due.
void Poll();
void Presented();
// Device-loss checks may run on a shader compiler thread. Uses only synchronized snapshots
// and read-only DXGI queries, never guest memory or the live shader cache.
void Lost(u32 removed_reason);
}  // namespace DX11::Diagnostics
