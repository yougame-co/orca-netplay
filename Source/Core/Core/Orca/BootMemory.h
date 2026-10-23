// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>

namespace Orca::BootMemory
{
// Start of BootCore; resets the first-frame boundary and logs memory once.
void BeginBoot();
// Called by the presenter on its first frame, including boots without an Orca profile.
void FirstFrame();
// Machine memory, normal log only. No protocol output.
void LogMemory();
// Reports a boot allocation failure; false once the first frame has happened.
bool AllocationFailed();

bool Run(void (*step)(void*), void* context);

// The thread/boot-call boundary owns cleanup. The callable is also the unit-test injection seam;
// it adds no allocation and catches only bad_alloc, only before the first frame.
template <typename Step>
bool Run(Step&& step)
{
  return Run([](void* context) { (*static_cast<std::remove_reference_t<Step>*>(context))(); },
             &step);
}
}  // namespace Orca::BootMemory
