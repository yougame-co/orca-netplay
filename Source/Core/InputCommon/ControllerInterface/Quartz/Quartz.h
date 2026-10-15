// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "InputCommon/ControllerInterface/InputBackend.h"

namespace ciface::Quartz
{
std::string GetSourceName();

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);

// Orca: without the keyboard-and-mouse device, nothing reads the global key state
// (CGEventSourceKeyState). Call before the controller interface starts (Orca/Sandbox.h).
void SetKeyboardEnabled(bool enabled);
}  // namespace ciface::Quartz
