// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace EMM
{
void InstallExceptionHandler();
void UninstallExceptionHandler();
bool IsExceptionHandlerSupported();
// Copy-on-write rollback snapshots (Rollback/Cow.h) need write faults on guest RAM handled on every
// thread. On macOS, InstallExceptionHandler covers only the thread that called it (a Mach thread
// exception port); this adds a process-wide signal handler that takes those faults on any other
// thread and passes everything else on. Elsewhere the handler is process-wide already: nothing to do.
void InstallCowFallbackHandler();
}  // namespace EMM
