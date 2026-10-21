// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

// orca-launch.ini tells Orca.app which launcher profile to start when the YouGame app runs it with
// no arguments. It sits in the build's folder, next to the mod's files:
//
//   [Launch]
//   Profile = PPLUS32
//   Executable = pplus/Project+ Netplay Launcher.dol
//
// Profile names a launcher profile in Sys/Orca; Executable is the loader to boot, relative to the
// folder. The profile pins the loader, disc and SD card by hash, so this file only says where they
// are. Parsing is strict: one [Launch] section, each key exactly once, and the loader must resolve
// (symlinks included) inside the folder.
namespace Orca
{
inline constexpr std::string_view LAUNCH_INI = "orca-launch.ini";

struct LaunchSpec
{
  std::string profile;
  // Relative to the folder holding the ini, as written.
  std::string executable;
};

// Parses the file's text; returns nullopt and sets the reason when it's malformed.
std::optional<LaunchSpec> ParseLaunchIni(std::string_view text, std::string* error);

// Reads `folder`/orca-launch.ini and resolves its loader to an absolute path inside `folder`.
// Nullopt with an empty error when there is no such file, or with the reason when it's invalid.
std::optional<LaunchSpec> ReadLaunchIni(const std::string& folder, std::string* error);
}  // namespace Orca
