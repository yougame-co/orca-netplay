// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

// The stdio bridge to the YouGame desktop app (ORCA.md "The stdio bridge"). With YOUGAME_STDIO=1
// the app's bridge is Orca's own stdin and stdout instead of a loopback server (YOUGAME_BRIDGE,
// YOUGAME_TOKEN), which a sandboxed Orca can't reach. Each bridge line starts with "yg "; every
// other line keeps its embed meaning (DolphinNoGUI/Embed.h).
//   Orca -> app (stdout), a request:
//     yg {"id":<n>,"type":"hello"|"mpTicket"|"mpRoom",...}
//   app -> Orca (stdin), its reply:
//     yg {"re":<n>,"ok":true,"result":{...}}  or  yg {"re":<n>,"ok":false,"error":"..."}
//   app -> Orca (stdin), a controller snapshot:
//     yg {"push":{"type":"controllers","snapshot":{...}}}
//   app -> Orca (stdin), the page's lobby:
//     yg {"push":{"slug":"...","cmd":"leave"|"kick",...}}
// The stdin reader routes the app's lines on its own thread (Route): replies to the request waiting
// for them, controller snapshots straight into the controller cache (only the newest of one read:
// the older ones are stale already), the page's lobby commands to the room. Malformed, oversized
// and unknown messages are dropped and counted, never acted on.
namespace Orca::Bridge
{
inline constexpr std::string_view PREFIX = "yg ";
// The longest bridge line taken from the app (a controller snapshot is 1-3 KB; Controllers.h takes
// up to 64 KB of one).
inline constexpr size_t MAX_INBOUND = 64 * 1024;
// The longest line of any kind the stdin reader keeps; a longer one is dropped through its newline.
inline constexpr size_t MAX_LINE = 128 * 1024;
// The longest request Orca writes (the app refuses a longer one).
inline constexpr size_t MAX_OUTBOUND = 32 * 1024;

// YOUGAME_STDIO=1: the app speaks the bridge on stdin and stdout.
bool Enabled();
// Enabled() and stdout set up to carry requests (SetWriter): what online play needs. Without
// --embed (or ORCA_TEST_COMMANDS) nothing reads Orca's stdin, so the bridge is off.
bool Ready();
// The only requests Orca makes: "hello", "mpTicket" and "mpRoom".
bool AllowedRequest(std::string_view type);

// Splits a byte stream into lines, without the newline or a trailing CR. A line longer than `max`
// is dropped whole, through its newline.
class LineSplitter
{
public:
  explicit LineSplitter(size_t max) : m_max(max) {}
  std::vector<std::string> Feed(std::string_view bytes);
  u64 Dropped() const { return m_dropped; }
  size_t Pending() const { return m_pending.size(); }

private:
  size_t m_max;
  std::string m_pending;
  bool m_skipping = false;
  u64 m_dropped = 0;
};

// One bridge line from the app.
struct Inbound
{
  enum class Kind
  {
    Ignored,
    Reply,
    Controllers,
    Push,
  };
  Kind kind = Kind::Ignored;
  // Reply: the request's id.
  u64 re = 0;
  // Reply: the whole reply ({re, ok, result|error}); Controllers and Push: the push object.
  picojson::value body;
};
// Parses a whole line, "yg " included. Ignored: not a bridge line, over MAX_INBOUND, not JSON, too
// deep, or a shape Orca doesn't know.
Inbound Parse(std::string_view line);

// Routes the lines of one read from stdin and returns the rest (embed commands), in order. Bridge
// lines are dropped unless Enabled().
std::vector<std::string> Route(std::vector<std::string> lines);
// Stdin closed: waiting requests give up at once, and later ones fail.
void Close();

// Where requests are written (Embed::Out in embed mode). Without one, Request fails at once.
void SetWriter(void (*writer)(const std::string& line));

// Sends `request`, whose "type" must be AllowedRequest, and waits for the app's reply: {ok, result}
// or {ok: false, error} (and "re"). nullopt when no reply came within `timeout`, `cancelled`
// returned true (checked every 20 ms), the request was refused here, or stdin is closed. Any
// thread.
std::optional<picojson::object> Request(picojson::object request, std::chrono::milliseconds timeout,
                                        const std::function<bool()>& cancelled = {});

// The page's lobby commands ({slug, cmd, ...}), on the stdin thread. One handler at a time, the
// current room's; the returned token removes it (ClearPushHandler waits out a call in progress, so
// the handler may not call it).
u64 SetPushHandler(std::function<void(const picojson::object&)> handler);
void ClearPushHandler(u64 token);
// The controller snapshots ({type: "controllers", snapshot}), on the stdin thread
// (ControllerSource).
u64 SetControllersHandler(std::function<void(const picojson::value&)> handler);
void ClearControllersHandler(u64 token);

struct Stats
{
  u64 replies = 0;
  // Replies whose request had already given up, or never existed.
  u64 unmatched = 0;
  u64 controllers = 0;
  // Controller snapshots skipped for a newer one in the same read.
  u64 coalesced = 0;
  u64 pushes = 0;
  // Bridge lines dropped: malformed, oversized, unknown, or with no handler (or the bridge off).
  u64 ignored = 0;
  u64 requests = 0;
  u64 unanswered = 0;
};
Stats GetStats();
// Tests: counters to zero and stdin open again.
void ResetForTests();

// The heartbeat on the stdio bridge: HEARTBEAT_LINE about once a HEARTBEAT_PERIOD, from the thread
// that owns Orca's window (every main-loop pass asks Due). The app stops a running Orca that said
// it once and then goes quiet (its window is a child of the app's and shares its input queue, so a
// hung window thread would hang the app's window too). Said only to the app, on the stdio bridge.
inline constexpr std::string_view HEARTBEAT_LINE = "orca alive";
inline constexpr std::chrono::milliseconds HEARTBEAT_PERIOD{1000};
class Heartbeat
{
public:
  // Whether to say it now; then not again for HEARTBEAT_PERIOD. The first call is always due.
  bool Due(std::chrono::steady_clock::time_point now);

private:
  std::optional<std::chrono::steady_clock::time_point> m_next;
};
}  // namespace Orca::Bridge
