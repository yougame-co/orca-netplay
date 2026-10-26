// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Orca::Sandbox
{
// Only a real directory (lstat, never a symlink) can supply this metadata.
struct PackJailDirectory
{
  unsigned long long device;
  long fs_type;
};

struct PackJailHomeEntries
{
  bool symlink = false;
  std::vector<std::string> names;
};

// OS probes are separate from the policy so tests can exercise Linux on any build host.
struct PackJailProbes
{
  std::function<std::optional<long>()> root_type;
  std::function<std::optional<std::string>()> passwd_home;
  // lstat the home root before listing it. Absent roots have no entries; symlinks are reported.
  std::function<std::optional<PackJailHomeEntries>(const std::string&)> home_entries;
  std::function<std::optional<unsigned long long>()> root_device;
  std::function<std::optional<PackJailDirectory>(const std::string&)> directory;
  // lstat: true for any type (including dangling symlinks), false for absence, nullopt for errors.
  std::function<std::optional<bool>(const std::string&)> exists;
  // true: connected, false: refused/unreachable, nullopt: probe could not run.
  std::function<std::optional<bool>(int, bool)> connect_x;
  std::optional<std::string> home;
  std::optional<std::string> display;
};

struct PackJailResult
{
  bool jailed = false;
  bool passwd_present = false;
  std::string failed;
  std::string pid1;
  std::vector<std::string> interfaces;
  std::string Status() const;
};

// root prefixes only the proc reads. No launcher marker authorizes the jail.
PackJailResult CheckLinuxPackJail(const std::string& root, const PackJailProbes& probes, bool online);
#ifdef __linux__
PackJailProbes LinuxPackJailProbes();
#endif
}  // namespace Orca::Sandbox
