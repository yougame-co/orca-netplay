// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Bridge.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <utility>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/Controllers.h"

namespace Orca::Bridge
{
namespace
{
// How a controller snapshot push starts as the app writes it (JSON.stringify keeps key order), so
// the older ones of one read can be skipped unparsed. Only a shortcut: a line that doesn't match is
// parsed like any other.
constexpr std::string_view CONTROLLERS_PREFIX = R"(yg {"push":{"type":"controllers")";
// JSON numbers are doubles: ids stay exact up to 2^53.
constexpr double MAX_ID = 9007199254740991.0;

struct Counters
{
  std::atomic<u64> replies{0}, unmatched{0}, controllers{0}, coalesced{0}, pushes{0}, ignored{0},
      requests{0}, unanswered{0};
};
Counters& Count()
{
  static Counters* const counters = new Counters;
  return *counters;
}

// Requests waiting for their reply. Never destroyed: the stdin thread may outlive main.
struct Waiting
{
  std::mutex mutex;
  std::condition_variable wake;
  std::map<u64, std::optional<picojson::object>> replies;
  u64 next_id = 0;
  bool closed = false;
  void (*writer)(const std::string&) = nullptr;
};
Waiting& Wait()
{
  static Waiting* const waiting = new Waiting;
  return *waiting;
}

template <typename T>
struct Handler
{
  std::mutex mutex;  // held through a call, so clearing waits it out
  std::function<void(const T&)> fn;
  u64 token = 0;
};
Handler<picojson::object>& Pushes()
{
  static auto* const handler = new Handler<picojson::object>;
  return *handler;
}
Handler<picojson::value>& Pads()
{
  static auto* const handler = new Handler<picojson::value>;
  return *handler;
}

std::atomic<u64> s_next_token{0};

template <typename T>
u64 SetHandler(Handler<T>& h, std::function<void(const T&)> fn)
{
  std::lock_guard lock(h.mutex);
  h.fn = std::move(fn);
  h.token = ++s_next_token;
  return h.token;
}

template <typename T>
void ClearHandler(Handler<T>& h, u64 token)
{
  std::lock_guard lock(h.mutex);
  if (h.token != token)
    return;
  h.fn = nullptr;
  h.token = 0;
}

template <typename T>
bool CallHandler(Handler<T>& h, const T& arg)
{
  std::lock_guard lock(h.mutex);
  if (!h.fn)
    return false;
  h.fn(arg);
  return true;
}

void Deliver(Inbound in)
{
  Counters& c = Count();
  switch (in.kind)
  {
  case Inbound::Kind::Reply:
  {
    Waiting& w = Wait();
    std::lock_guard lock(w.mutex);
    const auto it = w.replies.find(in.re);
    if (it == w.replies.end() || it->second)
    {
      ++c.unmatched;
      return;
    }
    it->second = std::move(in.body.get<picojson::object>());
    ++c.replies;
    w.wake.notify_all();
    return;
  }
  case Inbound::Kind::Controllers:
    if (CallHandler(Pads(), in.body))
      ++c.controllers;
    else
      ++c.ignored;
    return;
  case Inbound::Kind::Push:
    if (CallHandler(Pushes(), in.body.get<picojson::object>()))
      ++c.pushes;
    else
      ++c.ignored;
    return;
  case Inbound::Kind::Ignored:
    break;
  }
  ++c.ignored;
}
}  // namespace

bool Enabled()
{
  return GetEnv("YOUGAME_STDIO") == "1";
}

bool Ready()
{
  if (!Enabled())
    return false;
  Waiting& w = Wait();
  std::lock_guard lock(w.mutex);
  return w.writer != nullptr;
}

bool AllowedRequest(std::string_view type)
{
  return type == "hello" || type == "mpTicket" || type == "mpRoom";
}

std::vector<std::string> LineSplitter::Feed(std::string_view bytes)
{
  std::vector<std::string> lines;
  while (!bytes.empty())
  {
    const size_t newline = bytes.find('\n');
    const std::string_view piece = bytes.substr(0, newline);
    if (m_skipping)
    {
      // The rest of a line already dropped.
    }
    else if (m_pending.size() + piece.size() > m_max)
    {
      m_pending.clear();
      m_skipping = true;
      ++m_dropped;
    }
    else
    {
      m_pending.append(piece);
    }
    if (newline == std::string_view::npos)
      break;
    bytes.remove_prefix(newline + 1);
    if (m_skipping)
    {
      m_skipping = false;
      continue;
    }
    if (!m_pending.empty() && m_pending.back() == '\r')
      m_pending.pop_back();
    lines.push_back(std::exchange(m_pending, {}));
  }
  return lines;
}

Inbound Parse(std::string_view line)
{
  Inbound in;
  if (!line.starts_with(PREFIX))
    return in;
  line.remove_prefix(PREFIX.size());
  if (line.size() > MAX_INBOUND || !Orca::UX::SafeJson(line))
    return in;
  picojson::value root;
  if (!picojson::parse(root, std::string(line)).empty() || !root.is<picojson::object>())
    return in;
  const picojson::object& o = root.get<picojson::object>();
  if (const auto re = o.find("re"); re != o.end())
  {
    const auto ok = o.find("ok");
    if (!re->second.is<double>() || ok == o.end() || !ok->second.is<bool>())
      return in;
    const double id = re->second.get<double>();
    if (!(id >= 1 && id <= MAX_ID) || id != static_cast<double>(static_cast<u64>(id)))
      return in;
    in.kind = Inbound::Kind::Reply;
    in.re = static_cast<u64>(id);
    in.body = std::move(root);
    return in;
  }
  if (const auto push = o.find("push"); push != o.end() && o.size() == 1)
  {
    if (!push->second.is<picojson::object>())
      return in;
    const picojson::object& p = push->second.get<picojson::object>();
    const auto type = p.find("type");
    // The page's lobby commands name their game; a controller snapshot never does.
    const bool controllers = type != p.end() && type->second.is<std::string>() &&
                             type->second.get<std::string>() == "controllers" && !p.count("slug");
    in.kind = controllers ? Inbound::Kind::Controllers : Inbound::Kind::Push;
    picojson::value body = push->second;
    in.body = std::move(body);
    return in;
  }
  return in;
}

std::vector<std::string> Route(std::vector<std::string> lines)
{
  std::vector<std::string> rest;
  size_t newest_pad = lines.size();
  for (size_t i = 0; i < lines.size(); ++i)
  {
    if (lines[i].starts_with(CONTROLLERS_PREFIX))
      newest_pad = i;
  }
  const bool enabled = Enabled();
  for (size_t i = 0; i < lines.size(); ++i)
  {
    std::string& line = lines[i];
    if (!line.starts_with(PREFIX))
    {
      rest.push_back(std::move(line));
      continue;
    }
    if (!enabled)
    {
      ++Count().ignored;
      continue;
    }
    if (i < newest_pad && line.starts_with(CONTROLLERS_PREFIX))
    {
      ++Count().coalesced;
      continue;
    }
    Deliver(Parse(line));
  }
  return rest;
}

void Close()
{
  Waiting& w = Wait();
  std::lock_guard lock(w.mutex);
  w.closed = true;
  w.wake.notify_all();
}

void SetWriter(void (*writer)(const std::string& line))
{
  Waiting& w = Wait();
  std::lock_guard lock(w.mutex);
  w.writer = writer;
}

std::optional<picojson::object> Request(picojson::object request, std::chrono::milliseconds timeout,
                                        const std::function<bool()>& cancelled)
{
  const auto type = request.find("type");
  if (type == request.end() || !type->second.is<std::string>() ||
      !AllowedRequest(type->second.get<std::string>()) || request.count("id") ||
      request.count("re"))
  {
    ERROR_LOG_FMT(NETPLAY, "Orca bridge: refused to send a request it never makes");
    return std::nullopt;
  }
  Waiting& w = Wait();
  u64 id;
  void (*writer)(const std::string&);
  {
    std::lock_guard lock(w.mutex);
    if (w.closed || !w.writer)
    {
      WARN_LOG_FMT(NETPLAY, "Orca bridge: no {} to the app", w.closed ? "stdin" : "stdout");
      return std::nullopt;
    }
    id = ++w.next_id;
    writer = w.writer;
    w.replies.emplace(id, std::nullopt);
  }
  request["id"] = picojson::value(static_cast<double>(id));
  std::string line = std::string(PREFIX) + picojson::value(request).serialize();
  const auto forget = [&w, id] {
    std::lock_guard lock(w.mutex);
    w.replies.erase(id);
  };
  if (line.size() > MAX_OUTBOUND)
  {
    ERROR_LOG_FMT(NETPLAY, "Orca bridge: a {} request of {} bytes is too long to send",
                  type->second.get<std::string>(), line.size());
    forget();
    return std::nullopt;
  }
  ++Count().requests;
  writer(line);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock lock(w.mutex);
  while (true)
  {
    auto it = w.replies.find(id);
    if (it->second)
    {
      picojson::object reply = std::move(*it->second);
      w.replies.erase(it);
      return reply;
    }
    if (w.closed || std::chrono::steady_clock::now() >= deadline)
      break;
    if (cancelled)
    {
      lock.unlock();
      const bool stop = cancelled();
      lock.lock();
      if (stop)
        break;
      if (w.replies.find(id)->second)
        continue;
    }
    w.wake.wait_for(lock, std::chrono::milliseconds(20));
  }
  w.replies.erase(id);
  ++Count().unanswered;
  return std::nullopt;
}

u64 SetPushHandler(std::function<void(const picojson::object&)> handler)
{
  return SetHandler(Pushes(), std::move(handler));
}

void ClearPushHandler(u64 token)
{
  ClearHandler(Pushes(), token);
}

u64 SetControllersHandler(std::function<void(const picojson::value&)> handler)
{
  return SetHandler(Pads(), std::move(handler));
}

void ClearControllersHandler(u64 token)
{
  ClearHandler(Pads(), token);
}

Stats GetStats()
{
  const Counters& c = Count();
  Stats s;
  s.replies = c.replies;
  s.unmatched = c.unmatched;
  s.controllers = c.controllers;
  s.coalesced = c.coalesced;
  s.pushes = c.pushes;
  s.ignored = c.ignored;
  s.requests = c.requests;
  s.unanswered = c.unanswered;
  return s;
}

void ResetForTests()
{
  Counters& c = Count();
  c.replies = c.unmatched = c.controllers = c.coalesced = c.pushes = c.ignored = c.requests =
      c.unanswered = 0;
  Waiting& w = Wait();
  std::lock_guard lock(w.mutex);
  w.closed = false;
  w.replies.clear();
}

bool Heartbeat::Due(std::chrono::steady_clock::time_point now)
{
  if (m_next && now < *m_next)
    return false;
  m_next = now + HEARTBEAT_PERIOD;
  return true;
}
}  // namespace Orca::Bridge
