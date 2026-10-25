// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Orca::Sandbox
{
// OS probes are separate from the policy so tests can exercise Linux on any build host.
struct PackJailProbes
{
  std::function<std::optional<long>()> root_type;
  std::function<std::optional<std::string>()> passwd_home;
  // true: exists, false: absent, nullopt: stat failed for a reason other than absence.
  std::function<std::optional<bool>(const std::string&)> exists;
  // true: connected, false: refused/unreachable, nullopt: probe could not run.
  std::function<std::optional<bool>(int, bool)> connect_x;
  std::optional<std::string> home;
  std::optional<std::string> display;
};

struct PackJailResult
{
  bool jailed = false;
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
