// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/DiscVerify.h"

#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>

#include <fmt/format.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "Common/StringUtil.h"
#include "DiscIO/Volume.h"

namespace Orca::DiscVerify
{
namespace
{
using Severity = DiscIO::VolumeVerifier::Severity;

// Problems only in Brawl's Masterpiece partitions, matched on VolumeVerifier.cpp's English strings
// (nogui never loads translations): GetPartitionName's "{0} (Masterpiece)", the partition's name in
// CheckPartition's and Finish's problems, and CheckPartitions' "The Masterpiece partitions are
// missing."
bool OnlyMasterpieces(std::string_view text)
{
  return text.find("(Masterpiece)") != std::string_view::npos ||
         text.find("Masterpiece partitions") != std::string_view::npos;
}

std::string_view SeverityWord(Severity severity)
{
  switch (severity)
  {
  case Severity::High:
    return "high";
  case Severity::Medium:
    return "medium";
  default:
    return "low";
  }
}

bool IsControl(char c)
{
  const auto u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7f;
}

std::string OneLine(std::string_view text)
{
  std::string line;
  for (const char c : text)
  {
    if (c != ' ' && !IsControl(c))
      line += c;
    else if (!line.empty() && line.back() != ' ')
      line += ' ';
  }
  if (!line.empty() && line.back() == ' ')
    line.pop_back();
  return line;
}
}  // namespace

std::optional<std::string> TakeArgument(std::vector<char*>* argv, std::string* error)
{
  std::string path;
  std::vector<char*> kept;
  for (size_t i = 0; i < argv->size(); ++i)
  {
    char* const arg = (*argv)[i];
    if (i == 0 || !arg || std::strcmp(arg, "--verify") != 0)
    {
      kept.push_back(arg);
      continue;
    }
    const char* const value = i + 1 < argv->size() ? (*argv)[i + 1] : nullptr;
    if (!value || !*value)
    {
      *error = "--verify needs the path of a disc image";
      return std::nullopt;
    }
    path = value;
    ++i;
  }
  *argv = std::move(kept);
  return path;
}

bool Counts(const Problem& problem)
{
  return (problem.severity == Severity::Medium || problem.severity == Severity::High) &&
         !OnlyMasterpieces(problem.text);
}

std::size_t CountDamage(std::span<const Problem> problems)
{
  std::size_t damage = 0;
  for (const Problem& problem : problems)
  {
    if (Counts(problem))
      ++damage;
  }
  return damage;
}

int ExitCode(std::size_t damage)
{
  return damage ? EXIT_DAMAGED : EXIT_OK;
}

std::string ProgressLine(u64 done, u64 total)
{
  return fmt::format("orca verify progress {} {}", done, total);
}

std::string ProblemLine(const Problem& problem)
{
  return fmt::format("orca verify problem {} {}", SeverityWord(problem.severity),
                     OneLine(problem.text));
}

std::string DamageLine(const Problem& problem)
{
  return fmt::format("orca verify damage {}", OneLine(problem.text));
}

std::string VerdictLine(std::size_t damage)
{
  return damage ? fmt::format("orca verify damaged {}", damage) : "orca verify ok";
}

std::string UnreadableLine(std::string_view path)
{
  // The path as given, on one line.
  std::string line = fmt::format("orca verify unreadable {}", path);
  for (char& c : line)
  {
    if (IsControl(c))
      c = ' ';
  }
  return line;
}

std::vector<std::string> ResultLines(std::span<const Problem> problems)
{
  std::vector<std::string> lines;
  for (const Problem& problem : problems)
  {
    lines.push_back(ProblemLine(problem));
    if (Counts(problem))
      lines.push_back(DamageLine(problem));
  }
  lines.push_back(VerdictLine(CountDamage(problems)));
  return lines;
}

bool ProgressPacer::Due(std::chrono::steady_clock::time_point now)
{
  if (m_last && now - *m_last < std::chrono::seconds(1))
    return false;
  m_last = now;
  return true;
}

int Run(const std::string& path, std::FILE* out)
{
  const auto print = [out](const std::string& line) {
    fmt::print(out, "{}\n", line);
    std::fflush(out);
  };

  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(path);
  if (!volume)
  {
    print(UnreadableLine(path));
    return EXIT_UNREADABLE;
  }

  // No Redump lookup and no whole-disc hashes: the Wii hash tree is the integrity check.
  DiscIO::VolumeVerifier verifier(*volume, false, {});
  verifier.Start();
  ProgressPacer pacer;
  while (verifier.GetBytesProcessed() != verifier.GetTotalBytes())
  {
    if (pacer.Due(std::chrono::steady_clock::now()))
      print(ProgressLine(verifier.GetBytesProcessed(), verifier.GetTotalBytes()));
    verifier.Process();
  }
  verifier.Finish();

  const std::vector<Problem>& problems = verifier.GetResult().problems;
  for (const std::string& line : ResultLines(problems))
    print(line);
  return ExitCode(CountDamage(problems));
}

bool ProcessAlive(u64 pid)
{
#ifdef _WIN32
  if (pid == 0 || pid > MAXDWORD)
    return false;
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process)
    return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  if (pid == 0 || pid > static_cast<u64>(INT_MAX))
    return false;
  return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::size_t ClearStaleNands(const std::string& dir, const std::function<bool(u64 pid)>& alive)
{
  // Error codes throughout: Mac and Linux builds have no exceptions, and a throw would abort.
  std::vector<std::filesystem::path> stale;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(StringToPath(dir), ec), end; !ec && it != end;
       it.increment(ec))
  {
    const std::string name = PathToString(it->path().filename());
    if (!name.starts_with(NAND_PREFIX))
      continue;
    // "<pid>-<n>": the pid, then a dash.
    const char* const first = name.data() + NAND_PREFIX.size();
    const char* const last = name.data() + name.size();
    u64 pid = 0;
    const auto [stop, error] = std::from_chars(first, last, pid);
    if (error != std::errc{} || stop == first || stop == last || *stop != '-')
      continue;
    std::error_code type_ec;
    if (it->is_symlink(type_ec) || !it->is_directory(type_ec) || alive(pid))
      continue;
    stale.push_back(it->path());
  }
  std::size_t removed = 0;
  for (const std::filesystem::path& path : stale)
  {
    std::error_code remove_ec;
    std::filesystem::remove_all(path, remove_ec);
    if (!remove_ec)
      ++removed;
  }
  return removed;
}

std::string MakeNand()
{
  std::error_code ec;
  const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
  if (ec)
    return {};
  ClearStaleNands(PathToString(temp), ProcessAlive);
#ifdef _WIN32
  const u64 pid = GetCurrentProcessId();
#else
  const u64 pid = static_cast<u64>(getpid());
#endif
  const auto stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
  for (u64 n = 0; n < 8; ++n)
  {
    const std::filesystem::path nand = temp / fmt::format("{}{}-{:x}", NAND_PREFIX, pid, stamp + n);
    if (std::filesystem::create_directory(nand, ec))
    {
#ifdef _WIN32
      // Dolphin's paths use forward slashes (File::SetUserPath, File::CreateTempDir).
      return ReplaceAll(PathToString(nand), "\\", "/");
#else
      return PathToString(nand);
#endif
    }
    if (ec)
      return {};
  }
  return {};
}
}  // namespace Orca::DiscVerify
