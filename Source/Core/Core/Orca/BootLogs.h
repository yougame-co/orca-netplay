// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/Logging/Log.h"

// The boot logs. After a listing's boots die before their first frame, the YouGame app runs the
// next one with Dolphin's boot logs on (YouGame desktop/src/orca-crash.ts, BOOT_DIAG_ARGS: `-C
// Logger.Logs.<name>=True` at INFO): the guest's prints, IOS and its SD card, the memory map. Up to
// a healthy first frame they are under 200 lines (Project+ 174, Brawl 65); after it they say
// nothing about the boot, and Project+ reads its SD card all game: 0.7 MB a minute of play in the
// run's log (IOS_SD's reads, OSREPORT's file lines; Brawl 50 KB), which the app keeps and sends
// with a crash report.
namespace Orca::BootLogs
{
inline constexpr std::array TYPES = {Common::Log::LogType::OSREPORT, Common::Log::LogType::IOS,
                                     Common::Log::LogType::IOS_SD, Common::Log::LogType::MEMMAP};

// At the game's first frame: turns off each of TYPES that is on while Logger.ini leaves it off
// (the command line turned it on), and returns their names ("OSREPORT, IOS_SD"), empty when none.
// A category Logger.ini turns on stays on.
std::string EndAtFirstFrame();
}  // namespace Orca::BootLogs
