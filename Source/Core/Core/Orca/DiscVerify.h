// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/VolumeVerifier.h"

// `Orca --verify <disc>`: Dolphin's integrity check of a disc image (the Wii hash tree; no Redump,
// no whole-disc hashes), without booting it. One line each on stdout:
//   orca verify progress <bytes done> <bytes total>   at most once a second
//   orca verify problem <low|medium|high> <text>      one per problem the verifier reports
//   orca verify damage <text>                         after a problem's line, when it counts
//   orca verify ok | orca verify damaged <n> | orca verify unreadable <path>
// A problem counts when it is Medium or High and not only in Brawl's Masterpiece partitions, which
// Orca and Project+ never read. Damaged means n of them. Exit codes: 0 ok, 2 damaged, 1 unreadable.
namespace Orca::DiscVerify
{
using Problem = DiscIO::VolumeVerifier::Problem;

inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_UNREADABLE = 1;
inline constexpr int EXIT_DAMAGED = 2;

// Removes "--verify <path>" from argv. Returns the path, "" without the flag, or nullopt and sets
// *error when the path is missing.
std::optional<std::string> TakeArgument(std::vector<char*>* argv, std::string* error);

// Whether a problem makes the disc damaged (above).
bool Counts(const Problem& problem);
std::size_t CountDamage(std::span<const Problem> problems);
int ExitCode(std::size_t damage);

std::string ProgressLine(u64 done, u64 total);
// The text goes on one line: each run of spaces, line breaks and other control characters becomes
// one space.
std::string ProblemLine(const Problem& problem);
std::string DamageLine(const Problem& problem);
std::string VerdictLine(std::size_t damage);
std::string UnreadableLine(std::string_view path);
// Every problem's line, each followed by its damage line if it counts, then the verdict.
std::vector<std::string> ResultLines(std::span<const Problem> problems);

// Lets a progress line through at most once a second.
class ProgressPacer
{
public:
  bool Due(std::chrono::steady_clock::time_point now);

private:
  std::optional<std::chrono::steady_clock::time_point> m_last;
};

// Opens the disc and verifies it, printing the lines above to `out`. Returns the exit code.
int Run(const std::string& path, std::FILE* out);

// The verifier's IOS checks signatures in a NAND of its own, so the player's is never written: a
// folder in the system's temp folder, named for the verifying process
// ("orca-verify-nand-<pid>-<n>"). A verify that is killed (the app's SIGTERM, TerminateProcess on
// Windows) never removes its folder, so each verify first removes those whose process is gone.
inline constexpr std::string_view NAND_PREFIX = "orca-verify-nand-";
// Makes this process's NAND folder after clearing the stale ones. "" when it can't.
std::string MakeNand();
// Removes each NAND_PREFIX folder directly in `dir` whose process `alive` says is gone. A name
// without a pid, a file and a symlink stay. Returns how many it removed.
std::size_t ClearStaleNands(const std::string& dir, const std::function<bool(u64 pid)>& alive);
// Whether a process with this id is running. One this user may not look at counts as running.
bool ProcessAlive(u64 pid);
}  // namespace Orca::DiscVerify
