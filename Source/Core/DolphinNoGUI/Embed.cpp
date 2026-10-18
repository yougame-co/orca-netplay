// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Embed.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "Core/Orca/Bridge.h"

namespace Embed
{
namespace
{
// Never destroyed: the reader thread may still be blocked on stdin while the process exits.
struct Inbox
{
  std::mutex mutex;
  std::vector<std::string> lines;
};
Inbox& GetInbox()
{
  static Inbox* const inbox = new Inbox;
  return *inbox;
}

std::FILE* s_protocol = nullptr;

std::mutex& OutMutex()
{
  static std::mutex* const mutex = new std::mutex;
  return *mutex;
}

bool ParseInt(const char* text, int* out)
{
  if (!text || !*text)
    return false;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (*end != '\0' || value < -100000 || value > 100000)
    return false;
  *out = static_cast<int>(value);
  return true;
}
}  // namespace

std::optional<Rect> ParseRect(const std::string& text, bool clamp_size)
{
  std::string spaced = text;
  for (char& c : spaced)
  {
    if (c == ',')
      c = ' ';
  }
  std::istringstream in(spaced);
  Rect r;
  if (!(in >> r.x >> r.y >> r.w >> r.h))
    return std::nullopt;
  std::string rest;
  if (in >> rest)
    return std::nullopt;
  if (clamp_size)
  {
    r.w = std::max(r.w, 1);
    r.h = std::max(r.h, 1);
  }
  return r;
}

bool TraceEnabled()
{
  const char* const value = std::getenv("ORCA_EMBED_TRACE");
  return value && std::strcmp(value, "1") == 0;
}

void Trace(const Rect& requested, const WindowBounds& actual)
{
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
  NOTICE_LOG_FMT(ROLLBACK, "embed-trace t={} req={},{},{},{} real={},{},{},{} scale={}", ms,
                 requested.x, requested.y, requested.w, requested.h, actual.x, actual.y, actual.w,
                 actual.h, actual.scale);
}

bool TooSmall(const Rect& rect)
{
  return rect.w < MIN_SIDE || rect.h < MIN_SIDE;
}

std::optional<Options> TakeArguments(std::vector<char*>* argv, std::string* error)
{
  Options options;
  bool have_parent = false;
  std::vector<char*> kept;
  for (size_t i = 0; i < argv->size(); ++i)
  {
    char* const arg = (*argv)[i];
    if (i == 0 || !arg)
    {
      kept.push_back(arg);
      continue;
    }
    const auto next = [&](size_t n) -> const char* {
      return i + n < argv->size() ? (*argv)[i + n] : nullptr;
    };
    if (std::strcmp(arg, "--embed") == 0)
    {
      options.enabled = true;
    }
    else if (std::strcmp(arg, "--embed-adopted") == 0)
    {
      options.adopted = true;
    }
    else if (std::strcmp(arg, "--parent") == 0)
    {
      const char* value = next(1);
      char* end = nullptr;
      options.parent = value ? std::strtoull(value, &end, 10) : 0;
      if (!value || !*value || *end != '\0' || options.parent == 0)
      {
        *error = "--parent needs a window handle (a decimal number)";
        return std::nullopt;
      }
      have_parent = true;
      ++i;
    }
    else if (std::strcmp(arg, "--rect") == 0)
    {
      // Either four arguments or one "X,Y,W,H".
      Rect r;
      if (ParseInt(next(1), &r.x) && ParseInt(next(2), &r.y) && ParseInt(next(3), &r.w) &&
          ParseInt(next(4), &r.h))
      {
        r.w = std::max(r.w, 1);
        r.h = std::max(r.h, 1);
        options.rect = r;
        i += 4;
      }
      else if (const char* one = next(1); one && ParseRect(one))
      {
        options.rect = *ParseRect(one);
        ++i;
      }
      else
      {
        *error = "--rect needs X Y W H (or X,Y,W,H)";
        return std::nullopt;
      }
    }
    else
    {
      kept.push_back(arg);
    }
  }
  options.too_small = options.enabled && TooSmall(options.rect);
  options.rect.w = std::max(options.rect.w, MIN_SIDE);
  options.rect.h = std::max(options.rect.h, MIN_SIDE);
  if (options.enabled && !have_parent)
  {
    *error = "--embed needs --parent";
    return std::nullopt;
  }
  if (options.adopted && !options.enabled)
  {
    *error = "--embed-adopted needs --embed";
    return std::nullopt;
  }
  *argv = std::move(kept);
  return options;
}

void Result::Decide(std::string_view reason)
{
  if (!m_enabled || m_decided)
    return;
  m_decided = true;
  m_line = reason.empty() ? "orca embed ok" : "orca embed failed " + std::string(reason);
}

std::optional<std::string> Result::TakeLine(bool ready)
{
  if (!ready)
    return std::nullopt;
  return std::exchange(m_line, std::nullopt);
}

std::FILE* ClaimStdout()
{
  std::fflush(stdout);
#ifdef _WIN32
  const int fd = _dup(_fileno(stdout));
  std::FILE* const protocol = fd >= 0 ? _fdopen(fd, "w") : nullptr;
  _dup2(_fileno(stderr), _fileno(stdout));
  SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
#else
  const int fd = dup(STDOUT_FILENO);
  std::FILE* const protocol = fd >= 0 ? fdopen(fd, "w") : nullptr;
  dup2(STDERR_FILENO, STDOUT_FILENO);
#endif
  s_protocol = protocol;
  return protocol ? protocol : stdout;
}

void Out(const std::string& line)
{
  std::string one = line;
  for (char& c : one)
  {
    if (c == '\n' || c == '\r')
      c = ' ';
  }
  one += '\n';
  std::lock_guard lock(OutMutex());
  std::FILE* const out = s_protocol ? s_protocol : stdout;
  std::fputs(one.c_str(), out);
  std::fflush(out);
}

void StartReading()
{
  // Read the raw file descriptor, not stdin: a thread blocked in a FILE read holds that FILE's
  // lock, and fflush(nullptr) at shutdown or exit would then hang forever.
  std::thread([] {
    // 64 KB reads: a controller snapshot on the stdio bridge is a few KB, and many can queue up.
    Orca::Bridge::LineSplitter splitter(Orca::Bridge::MAX_LINE);
    // Never freed, like the inbox: this thread may still be blocked on stdin when the process
    // exits.
    auto* const buffer = new char[64 * 1024];
    u64 dropped = 0;
    while (true)
    {
#ifdef _WIN32
      const int got = _read(0, buffer, 64 * 1024);
#else
      const ssize_t got = read(STDIN_FILENO, buffer, 64 * 1024);
      if (got < 0 && errno == EINTR)
        continue;
#endif
      if (got <= 0)
        break;
      // Bridge lines are handled here, on this thread; the commands wait for the frame loop.
      std::vector<std::string> lines =
          Orca::Bridge::Route(splitter.Feed(std::string_view(buffer, static_cast<size_t>(got))));
      if (splitter.Dropped() != dropped)
      {
        dropped = splitter.Dropped();
        std::fprintf(stderr, "Orca: dropped a line over %zu bytes from the app (%llu so far)\n",
                     Orca::Bridge::MAX_LINE, static_cast<unsigned long long>(dropped));
      }
      if (lines.empty())
        continue;
      bool quit = false;
      Inbox& inbox = GetInbox();
      std::lock_guard lock(inbox.mutex);
      for (std::string& line : lines)
      {
        quit = quit || line == "quit";
        inbox.lines.push_back(std::move(line));
      }
      if (quit)
      {
        // Nothing more is read, so no reply can come.
        Orca::Bridge::Close();
        return;
      }
    }
    // The app closed the pipe or died, so there is nobody left to show the game to.
    Orca::Bridge::Close();
    Inbox& inbox = GetInbox();
    std::lock_guard lock(inbox.mutex);
    inbox.lines.emplace_back("quit");
  }).detach();
}

std::vector<std::string> TakeCommands()
{
  Inbox& inbox = GetInbox();
  std::vector<std::string> lines;
  std::lock_guard lock(inbox.mutex);
  lines.swap(inbox.lines);
  return lines;
}
}  // namespace Embed
