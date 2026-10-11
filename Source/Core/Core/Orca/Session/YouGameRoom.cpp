// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/YouGameRoom.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <fmt/format.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/WebSocket.h"
#include "Core/Orca/Bridge.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Queue.h"
#include "Core/Orca/UX/RankedSet.h"
#include "Core/Orca/UX/SetEnd.h"

namespace Orca::Net
{
namespace
{
// The rooms Worker's lobby protocol (rooms/src/index.ts and rooms/src/game-lobby.ts in the YouGame
// repo). Orca rooms are private game lobbies of up to four. The host plays from boot; a friend who
// joins plugs into the next free port (drop-in). A friends room starts no room match and keeps no
// results.
constexpr const char* CODE_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789";
// The room allows 120 messages a second per socket, so packets go out at most every 10 ms, the
// newest replacing any not yet sent.
constexpr auto SEND_GAP = std::chrono::milliseconds(10);
constexpr auto PING_EVERY = std::chrono::seconds(1);
constexpr auto HELLO_EVERY = std::chrono::seconds(1);
constexpr size_t MAX_INBOX = 1024;
// The server answers every ping (one a second), so a connection silent this long is dead even if
// sends still succeed into the socket buffer (the network vanished, the laptop slept). Not much
// shorter: a keyframe upload can fill a home uplink and delay pings by a second or two.
constexpr auto SERVER_SILENCE_LIMIT = std::chrono::seconds(8);
// A joiner whose room has no host (a stale invite, a host that left or restarted) gives up this
// long after its welcome; a present host says hello within about a second.
constexpr auto HOST_WAIT = std::chrono::seconds(8);
// The room's ticket: the page mints it, and a busy site took 12-60 s. A request that gets no reply
// is sent once more.
constexpr auto TICKET_TIMEOUT = std::chrono::seconds(30);
constexpr int TICKET_TRIES = 2;
// Matchmade rooms: a game's report waits this long for the room's match to start; the host's begin
// gets this many tries (one per roster change after a refusal) and is resent if unanswered this
// long.
constexpr auto REPORT_HOLD = std::chrono::seconds(60);
// How often a void `finish` the room refused (its games still being decided) goes again.
constexpr int FINISH_RETRIES = 30;
constexpr int BEGIN_TRIES = 5;
constexpr auto BEGIN_ANSWER = std::chrono::seconds(5);
// The server closed a matchmade room after its result (rooms/src/index.ts: 4004 "match-over").
constexpr int CLOSE_MATCH_OVER = 4004;
// How long a teardown waits for the app to take the emptied room (loopback, normally milliseconds).
constexpr auto MIRROR_END_TIMEOUT = std::chrono::seconds(1);
// Direct links: a stalled session asks to send every millisecond, but a direct copy goes out only
// for new inputs, a checksum or a leave, else at most this often. The stats summary refreshes this
// often.
constexpr auto DIRECT_STALL_GAP = std::chrono::milliseconds(4);
constexpr auto DIRECT_SUMMARY_EVERY = std::chrono::milliseconds(250);

using Clock = std::chrono::steady_clock;

// Rooms come and go within one process (reopened, joined, left), each mirroring itself to the page
// on its own threads. The page must only get the newest room's updates, never a late one from an
// older room. Each room has a generation; posts from a room older than the newest poster are
// dropped.
std::atomic<u64> s_room_generation{0};
std::mutex s_mirror_mutex;
u64 s_mirror_newest = 0;  // under s_mirror_mutex

u64 NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Env(const char* name)
{
  return Orca::GetEnv(name);
}

bool ValidCode(const std::string& code)
{
  return code.size() >= 6 && code.size() <= 12 &&
         std::all_of(code.begin(), code.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

// A player name safe for game memory and line-based output: printable ASCII and whole UTF-8
// sequences only, at most 64 bytes, never a split character.
std::string CleanName(const std::string& raw)
{
  std::string out;
  for (size_t i = 0; i < raw.size();)
  {
    const auto c = static_cast<unsigned char>(raw[i]);
    size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool ok = length != 0 && i + length <= raw.size() && (length > 1 || (c >= 0x20 && c != 0x7F));
    for (size_t k = 1; ok && k < length; ++k)
      ok = (static_cast<unsigned char>(raw[i + k]) & 0xC0) == 0x80;
    if (!ok)
    {
      ++i;
      continue;
    }
    if (out.size() + length > 64)
      break;
    out.append(raw, i, length);
    i += length;
  }
  return out;
}

// Hex encoding and decoding (defined below, with the packets').
std::string ToHex(const u8* data, size_t size);
bool FromHex(const std::string& hex, std::vector<u8>* out);

// A port's controls from the wire: hex, at most MAX_CONTROLS bytes; anything else is none. Both
// sides apply the same bound, so the host keeps exactly what a joiner reads back.
std::vector<u8> ControlsFromHex(const picojson::value& v, size_t max = MAX_CONTROLS)
{
  std::vector<u8> bytes;
  if (!v.is<std::string>() || v.get<std::string>().size() > max * 2 ||
      !FromHex(v.get<std::string>(), &bytes))
  {
    return {};
  }
  return bytes;
}

}  // namespace

// Each entry [seat, from, name], plus the controls' hex when the port carries controls.
picojson::value NamesToJson(const std::vector<KeyframeInfo::Name>& names)
{
  picojson::array out;
  for (const KeyframeInfo::Name& n : names)
  {
    picojson::array entry{picojson::value(static_cast<double>(n.seat)),
                          picojson::value(static_cast<double>(n.from)), picojson::value(n.name)};
    if (!n.controls.empty() || !n.queue.empty())
      entry.push_back(picojson::value(ToHex(n.controls.data(), n.controls.size())));
    if (!n.queue.empty())
      entry.push_back(picojson::value(ToHex(n.queue.data(), n.queue.size())));
    out.push_back(picojson::value(std::move(entry)));
  }
  return picojson::value(out);
}

// At most 16 entries (a few per seat: who held it, from when).
std::vector<KeyframeInfo::Name> NamesFromJson(const picojson::object& d)
{
  std::vector<KeyframeInfo::Name> names;
  const auto nm = d.find("nm");
  if (nm == d.end() || !nm->second.is<picojson::array>())
    return names;
  for (const auto& entry : nm->second.get<picojson::array>())
  {
    if (!entry.is<picojson::array>() || names.size() >= 16)
      continue;
    const auto& a = entry.get<picojson::array>();
    if (a.size() < 3 || a.size() > 5 || !a[0].is<double>() || !a[1].is<double>() ||
        !a[2].is<std::string>())
    {
      continue;
    }
    const double seat = a[0].get<double>(), from = a[1].get<double>();
    if (seat < 0 || seat >= MAX_SEATS || from < 0 || from > (1 << 30))
      continue;
    names.push_back({static_cast<int>(seat), static_cast<int>(from),
                     CleanName(a[2].get<std::string>()),
                     a.size() >= 4 ? ControlsFromHex(a[3]) : std::vector<u8>{},
                     a.size() == 5 ? ControlsFromHex(a[4], MAX_QUEUE) : std::vector<u8>{}});
  }
  return names;
}

namespace
{

std::string RandomString(const char* alphabet, size_t length)
{
  std::random_device random;
  const size_t n = std::char_traits<char>::length(alphabet);
  std::string out;
  for (size_t i = 0; i < length; ++i)
    out += alphabet[random() % n];
  return out;
}

std::string ToHex(const u8* data, size_t size)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i)
  {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

int HexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

bool FromHex(const std::string& hex, std::vector<u8>* out)
{
  if (hex.size() % 2)
    return false;
  out->clear();
  for (size_t i = 0; i < hex.size(); i += 2)
  {
    const int high = HexDigit(hex[i]), low = HexDigit(hex[i + 1]);
    if (high < 0 || low < 0)
      return false;
    out->push_back(static_cast<u8>(high << 4 | low));
  }
  return true;
}

// A whole number in [lo, hi], or nullopt.
std::optional<int> Int(const picojson::object& o, const std::string& key, int lo, int hi)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<double>())
    return std::nullopt;
  const double v = it->second.get<double>();
  if (!(v >= lo && v <= hi) || v != static_cast<double>(static_cast<int>(v)))
    return std::nullopt;
  return static_cast<int>(v);
}

// A telemetry number, clamped into [lo, hi] rather than refused, since a bad one should not end the
// match; nullopt when absent or not a number.
std::optional<int> ClampedInt(const picojson::object& o, const std::string& key, int lo, int hi)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<double>())
    return std::nullopt;
  const double v = it->second.get<double>();
  if (std::isnan(v))
    return std::nullopt;
  return static_cast<int>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
}

std::string Str(const picojson::object& o, const std::string& key)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "";
}

constexpr int MAX_FRAME = 1 << 30;
// Telemetry bounds: re-run frames per second, lead in tenths of a frame, round trip in ms, stalls
// and hitches per match. Far beyond anything honest (the session clamps further), yet small enough
// that no arithmetic on them overflows.
constexpr int MAX_RESIMULATED = 1 << 20;
constexpr int MAX_STALLS = 1 << 20;
constexpr int MAX_ADVANTAGE = 1 << 20;
constexpr int MAX_RTT = 1 << 20;
// Drop-in history runs per packet (the session sends far fewer; CoalescePacket may join two).
constexpr size_t MAX_HISTORY_RUNS_IN_PACKET = 128;
constexpr size_t MAX_HISTORY_FRAMES_IN_PACKET = MAX_HISTORY_IN_PACKET;

// A JSON string literal.
std::string JsonString(const std::string& text)
{
  return picojson::value(text).serialize();
}

// A label for a line the app shows (e.g. a rank): printable ASCII, no quotes or backslashes, at
// most `max` characters.
std::string CleanLabel(const std::string& raw, size_t max)
{
  std::string out;
  for (const char c : raw)
  {
    if (out.size() >= max)
      break;
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
      out += c;
  }
  return out;
}

// picojson has no nesting limit (deep arrays overflow the I/O thread's stack) and aborts on numbers
// that overflow a double. Nothing YouGame sends nests deeply, uses an exponent or has over 17
// digits in a row, so such values become null before parsing: a message carrying one (a value
// another player put in the room, echoed by the server) is still read, without that value.
constexpr int MAX_JSON_DEPTH = 16;
constexpr int MAX_JSON_DIGITS = 17;

// The index just past the string starting at `at` (a quote), or npos if it never ends.
size_t SkipJsonString(std::string_view text, size_t at)
{
  for (size_t i = at + 1; i < text.size(); ++i)
  {
    if (text[i] == '\\')
      ++i;
    else if (text[i] == '"')
      return i + 1;
  }
  return std::string_view::npos;
}

// The index of the bracket closing the array or object opened at `at`, or npos.
size_t MatchingClose(std::string_view text, size_t at)
{
  int depth = 0;
  for (size_t i = at; i < text.size();)
  {
    const char c = text[i];
    if (c == '"')
    {
      i = SkipJsonString(text, i);
      if (i == std::string_view::npos)
        return i;
      continue;
    }
    if (c == '[' || c == '{')
      ++depth;
    else if ((c == ']' || c == '}') && --depth == 0)
      return i;
    ++i;
  }
  return std::string_view::npos;
}

bool SafeNumber(std::string_view number)
{
  int digits = 0;
  for (const char c : number)
  {
    if (c == 'e' || c == 'E')
      return false;
    digits = c >= '0' && c <= '9' ? digits + 1 : 0;
    if (digits > MAX_JSON_DIGITS)
      return false;
  }
  return true;
}

// `text` with every value picojson can't take safely (nested past MAX_JSON_DEPTH, a number with an
// exponent or over MAX_JSON_DIGITS digits in a row) replaced by null. Nullopt if a string or
// bracket never closes (picojson would refuse it anyway).
std::optional<std::string> SanitizeJson(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  int depth = 0;
  for (size_t i = 0; i < text.size();)
  {
    const char c = text[i];
    if (c == '"')
    {
      const size_t end = SkipJsonString(text, i);
      if (end == std::string_view::npos)
        return std::nullopt;
      out.append(text.substr(i, end - i));
      i = end;
      continue;
    }
    if (c == '[' || c == '{')
    {
      if (depth >= MAX_JSON_DEPTH)
      {
        const size_t end = MatchingClose(text, i);
        if (end == std::string_view::npos)
          return std::nullopt;
        out += "null";
        i = end + 1;
        continue;
      }
      ++depth;
    }
    else if (c == ']' || c == '}')
    {
      --depth;
    }
    else if (c == '-' || (c >= '0' && c <= '9'))
    {
      size_t end = i + 1;
      while (end < text.size() &&
             ((text[end] >= '0' && text[end] <= '9') || text[end] == '.' || text[end] == 'e' ||
              text[end] == 'E' || text[end] == '+' || text[end] == '-'))
      {
        ++end;
      }
      const std::string_view number = text.substr(i, end - i);
      if (SafeNumber(number))
        out.append(number);
      else
        out += "null";
      i = end;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

bool ParseJson(const std::string& text, picojson::value* value)
{
  const std::optional<std::string> safe = SanitizeJson(text);
  return safe && picojson::parse(*value, *safe).empty();
}

picojson::object PacketToJson(const Packet& packet)
{
  picojson::object d;
  d["k"] = picojson::value("p");
  d["s"] = picojson::value(static_cast<double>(packet.seat));
  d["f"] = picojson::value(static_cast<double>(packet.first_frame));
  std::string pads;
  for (const Pad& pad : packet.pads)
    pads += ToHex(pad.data(), pad.size());
  d["p"] = picojson::value(pads);
  picojson::array ack;
  for (int a : packet.ack)
    ack.emplace_back(static_cast<double>(a));
  d["a"] = picojson::value(ack);
  d["c"] = picojson::value(static_cast<double>(packet.current_frame));
  if (packet.sequence > 0)
    d["q"] = picojson::value(static_cast<double>(packet.sequence));
  // Optional fields, so older peers' packets still parse.
  if (packet.resimulated >= 0)
    d["r"] = picojson::value(static_cast<double>(packet.resimulated));
  if (packet.advantage)
    d["v"] = picojson::value(static_cast<double>(*packet.advantage));
  if (packet.rtt)
    d["l"] = picojson::value(static_cast<double>(*packet.rtt));
  if (packet.stalls >= 0)
    d["n"] = picojson::value(static_cast<double>(packet.stalls));
  if (packet.hitches >= 0)
    d["j"] = picojson::value(static_cast<double>(packet.hitches));
  if (packet.spared)
  {
    picojson::array spared;
    for (const int n : *packet.spared)
      spared.emplace_back(static_cast<double>(n));
    d["sp"] = picojson::value(spared);
  }
  if (packet.checksum_frame)
  {
    d["x"] = picojson::value(static_cast<double>(*packet.checksum_frame));
    // A u64 doesn't survive a JSON number.
    u8 bytes[8];
    for (int i = 0; i < 8; ++i)
      bytes[i] = static_cast<u8>(packet.checksum >> (56 - 8 * i));
    d["h"] = picojson::value(ToHex(bytes, 8));
  }
  // Drop-in (all optional): the roster as [plug, unplug, live] per seat (-1: never), the history
  // ack, history as [count, pads hex] runs, and a leave request.
  if (packet.roster)
  {
    picojson::array roster;
    for (const SeatPlan& plan : *packet.roster)
    {
      picojson::array entry;
      for (int v : {plan.plug_from, plan.unplug_from, plan.live_from})
        entry.emplace_back(static_cast<double>(v == NEVER ? -1 : v));
      roster.emplace_back(entry);
    }
    d["ro"] = picojson::value(roster);
  }
  if (packet.history_ack >= 0)
    d["ha"] = picojson::value(static_cast<double>(packet.history_ack));
  if (packet.values_ack >= 0)
    d["va"] = picojson::value(static_cast<double>(packet.values_ack));
  if (packet.history_seat >= 0 && !packet.history.empty())
  {
    d["ht"] = picojson::value(static_cast<double>(packet.history_seat));
    d["hf"] = picojson::value(static_cast<double>(packet.history_first));
    picojson::array runs;
    for (size_t i = 0; i < packet.history.size();)
    {
      size_t n = 1;
      while (i + n < packet.history.size() && packet.history[i + n] == packet.history[i])
        ++n;
      std::string hex;
      for (const Pad& pad : packet.history[i])
        hex += ToHex(pad.data(), pad.size());
      picojson::array run;
      run.emplace_back(static_cast<double>(n));
      run.emplace_back(hex);
      runs.emplace_back(run);
      i += n;
    }
    d["hp"] = picojson::value(runs);
  }
  if (packet.leaving)
    d["lv"] = picojson::value(true);
  return d;
}

// Shape and range checks only; the session bounds frames against its own position.
bool PacketFromJson(const picojson::object& d, Packet* packet)
{
  if (Str(d, "k") != "p")
    return false;
  const auto seat = Int(d, "s", 0, MAX_SEATS - 1);
  const auto first = Int(d, "f", 0, MAX_FRAME);
  const auto current = Int(d, "c", 0, MAX_FRAME);
  if (!seat || !first || !current)
    return false;
  std::vector<u8> bytes;
  const std::string pads = Str(d, "p");
  if (pads.size() > MAX_PADS_IN_PACKET * sizeof(Pad) * 2 || !FromHex(pads, &bytes) ||
      bytes.size() % sizeof(Pad))
  {
    return false;
  }
  const auto ack = d.find("a");
  if (ack == d.end() || !ack->second.is<picojson::array>() ||
      ack->second.get<picojson::array>().size() != MAX_SEATS)
  {
    return false;
  }
  Packet out;
  out.seat = *seat;
  out.first_frame = *first;
  out.current_frame = *current;
  if (d.count("q"))
  {
    const auto sequence = Int(d, "q", 1, MAX_FRAME - 1);
    if (!sequence)
      return false;
    out.sequence = *sequence;
  }
  for (size_t i = 0; i < bytes.size(); i += sizeof(Pad))
  {
    Pad pad;
    std::copy_n(bytes.begin() + i, pad.size(), pad.begin());
    out.pads.push_back(pad);
  }
  const auto& acks = ack->second.get<picojson::array>();
  for (int s = 0; s < MAX_SEATS; ++s)
  {
    const picojson::value& v = acks[s];
    if (!v.is<double>() || v.get<double>() < -1 || v.get<double>() > MAX_FRAME ||
        v.get<double>() != static_cast<double>(static_cast<int>(v.get<double>())))
    {
      return false;
    }
    out.ack[s] = static_cast<int>(v.get<double>());
  }
  // Telemetry for adaptive delay and time sync: optional and clamped.
  if (const auto resimulated = ClampedInt(d, "r", 0, MAX_RESIMULATED))
    out.resimulated = *resimulated;
  out.advantage = ClampedInt(d, "v", -MAX_ADVANTAGE, MAX_ADVANTAGE);
  out.rtt = ClampedInt(d, "l", 0, MAX_RTT);
  if (const auto stalls = ClampedInt(d, "n", 0, MAX_STALLS))
    out.stalls = *stalls;
  if (const auto hitches = ClampedInt(d, "j", 0, MAX_STALLS))
    out.hitches = *hitches;
  // Stall lengths (Packet::spared): SPARED_FRAMES numbers, each clamped like the counts above, or
  // none at all if the length is wrong or anything isn't a number.
  if (const auto sp = d.find("sp"); sp != d.end() && sp->second.is<picojson::array>() &&
                                    sp->second.get<picojson::array>().size() == SPARED_FRAMES)
  {
    std::array<int, SPARED_FRAMES> spared{};
    bool numbers = true;
    for (int k = 0; k < SPARED_FRAMES && numbers; ++k)
    {
      const picojson::value& v = sp->second.get<picojson::array>()[k];
      numbers = v.is<double>() && !std::isnan(v.get<double>());
      if (numbers)
      {
        spared[k] =
            static_cast<int>(std::clamp(v.get<double>(), 0.0, static_cast<double>(MAX_STALLS)));
      }
    }
    if (numbers)
      out.spared = spared;
  }
  if (d.count("x"))
  {
    const auto frame = Int(d, "x", 0, MAX_FRAME);
    std::vector<u8> hash;
    if (!frame || !FromHex(Str(d, "h"), &hash) || hash.size() != 8)
      return false;
    out.checksum_frame = *frame;
    out.checksum = 0;
    for (u8 b : hash)
      out.checksum = out.checksum << 8 | b;
  }
  if (d.count("ro"))
  {
    const auto& ro = d.at("ro");
    if (!ro.is<picojson::array>() || ro.get<picojson::array>().size() != MAX_SEATS)
      return false;
    std::array<SeatPlan, MAX_SEATS> roster;
    for (int s = 0; s < MAX_SEATS; ++s)
    {
      const auto& entry = ro.get<picojson::array>()[s];
      if (!entry.is<picojson::array>() || entry.get<picojson::array>().size() != 3)
        return false;
      int values[3];
      for (int k = 0; k < 3; ++k)
      {
        const picojson::value& v = entry.get<picojson::array>()[k];
        if (!v.is<double>() || v.get<double>() < -1 || v.get<double>() > MAX_FRAME ||
            v.get<double>() != static_cast<double>(static_cast<int>(v.get<double>())))
        {
          return false;
        }
        values[k] = v.get<double>() < 0 ? NEVER : static_cast<int>(v.get<double>());
      }
      roster[s] = {values[0], values[1], values[2]};
    }
    out.roster = roster;
  }
  if (d.count("ha"))
  {
    const auto ha = Int(d, "ha", 0, MAX_FRAME);
    if (!ha)
      return false;
    out.history_ack = *ha;
  }
  if (d.count("va"))
  {
    const auto va = Int(d, "va", 0, MAX_FRAME);
    if (!va)
      return false;
    out.values_ack = *va;
  }
  if (d.count("hp"))
  {
    const auto seat_h = Int(d, "ht", 0, MAX_SEATS - 1);
    const auto first_h = Int(d, "hf", 0, MAX_FRAME);
    const auto& hp = d.at("hp");
    if (!seat_h || !first_h || !hp.is<picojson::array>() ||
        hp.get<picojson::array>().size() > MAX_HISTORY_RUNS_IN_PACKET)
    {
      return false;
    }
    out.history_seat = *seat_h;
    out.history_first = *first_h;
    for (const picojson::value& run : hp.get<picojson::array>())
    {
      if (!run.is<picojson::array>() || run.get<picojson::array>().size() != 2)
        return false;
      const auto& r = run.get<picojson::array>();
      std::vector<u8> run_bytes;
      if (!r[0].is<double>() || !r[1].is<std::string>() || r[0].get<double>() < 1 ||
          r[0].get<double>() > MAX_HISTORY_FRAMES_IN_PACKET ||
          !FromHex(r[1].get<std::string>(), &run_bytes) || run_bytes.size() != sizeof(Pads))
      {
        return false;
      }
      const int n = static_cast<int>(r[0].get<double>());
      if (out.history.size() + n > MAX_HISTORY_FRAMES_IN_PACKET)
        return false;
      Pads run_pads;
      for (int k = 0; k < MAX_SEATS; ++k)
        std::copy_n(run_bytes.begin() + k * sizeof(Pad), sizeof(Pad), run_pads[k].begin());
      out.history.insert(out.history.end(), n, run_pads);
    }
  }
  out.leaving = d.count("lv") && d.at("lv").evaluate_as_boolean();
  *packet = std::move(out);
  return true;
}

// The desktop app's bridge must be exactly http://127.0.0.1:<port>, so the token goes nowhere else.
bool LoopbackBridge(std::string bridge)
{
  if (bridge.ends_with("/"))
    bridge.pop_back();
  constexpr std::string_view prefix = "http://127.0.0.1:";
  if (!bridge.starts_with(prefix))
    return false;
  const std::string_view port = std::string_view(bridge).substr(prefix.size());
  return !port.empty() && port.size() <= 5 &&
         std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Room tickets travel only over TLS, except to a rooms Worker on this machine (tests).
bool AcceptableRoomUrl(const std::string& url)
{
  return url.starts_with("https://") || url.starts_with("wss://") ||
         url.starts_with("http://127.0.0.1:") || url.starts_with("http://localhost:") ||
         url.starts_with("ws://127.0.0.1:") || url.starts_with("ws://localhost:");
}

std::string WebSocketUrl(std::string url)
{
  if (url.starts_with("https://"))
    url.replace(0, 8, "wss://");
  else if (url.starts_with("http://"))
    url.replace(0, 7, "ws://");
  return url;
}
}  // namespace

// Why the rooms Worker refused the WebSocket upgrade, for the player. Only the status is readable:
// 401 an expired ticket; 403 a different lobby key (another Orca build, disc or save) or a host's
// kick; 409 a full room.
std::string RefusalCode(const std::string& connect_error)
{
  if (connect_error.find("(HTTP 403)") != std::string::npos)
    return "room_mismatch";
  if (connect_error.find("(HTTP 409)") != std::string::npos)
    return "room_full";
  return "network";
}

std::string RefusalReason(const std::string& connect_error)
{
  if (connect_error.find("(HTTP 403)") != std::string::npos)
  {
    return "The room didn't let you in: your friend's Orca, disc or save differs from yours (both "
           "of you: update YouGame and press Play again), or the host removed you";
  }
  if (connect_error.find("(HTTP 409)") != std::string::npos)
    return "That room is full";
  if (connect_error.find("(HTTP 401)") != std::string::npos)
    return "The invite expired; press Play again";
  if (connect_error.find("(HTTP 4") != std::string::npos)
    return "The room refused to let you in";
  return "Could not reach YouGame's rooms";
}

std::string EncodePacket(const Packet& packet)
{
  return picojson::value(PacketToJson(packet)).serialize();
}

bool DecodePacket(const std::string& json, Packet* packet)
{
  picojson::value value;
  if (!ParseJson(json, &value) || !value.is<picojson::object>())
    return false;
  return PacketFromJson(value.get<picojson::object>(), packet);
}

bool InlineKeyframe(u64 size)
{
  static const bool off = Orca::GetEnv("ORCA_TEST_INLINE") == "off" && Orca::UX::TestKnobsAllowed();
  return !off && size > 0 && size <= INLINE_KEYFRAME_MAX;
}

namespace
{
constexpr char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Base64Value(char c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}
}  // namespace

std::string EncodeInlineKeyframe(const std::vector<u8>& blob)
{
  std::string out;
  out.reserve((blob.size() + 2) / 3 * 4);
  for (size_t i = 0; i < blob.size(); i += 3)
  {
    const u32 a = blob[i];
    const u32 b = i + 1 < blob.size() ? blob[i + 1] : 0;
    const u32 c = i + 2 < blob.size() ? blob[i + 2] : 0;
    const u32 n = (a << 16) | (b << 8) | c;
    out.push_back(BASE64[(n >> 18) & 63]);
    out.push_back(BASE64[(n >> 12) & 63]);
    out.push_back(i + 1 < blob.size() ? BASE64[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < blob.size() ? BASE64[n & 63] : '=');
  }
  return out;
}

std::optional<std::vector<u8>> DecodeInlineKeyframe(std::string_view text, u64 size)
{
  // Bounded before anything is decoded: the size the offer states, and the one text length that
  // can hold exactly that many bytes.
  if (size == 0 || size > INLINE_KEYFRAME_MAX || text.size() > INLINE_KEYFRAME_MAX_TEXT ||
      text.size() != (size + 2) / 3 * 4)
  {
    return std::nullopt;
  }
  std::vector<u8> out;
  out.reserve(static_cast<size_t>(size));
  for (size_t i = 0; i < text.size(); i += 4)
  {
    const bool last = i + 4 == text.size();
    int v[4];
    for (int k = 0; k < 4; ++k)
    {
      // Padding only at the very end: "xx==" or "xxx=".
      if (text[i + k] == '=' && last && k >= 2 && (k == 3 || text[i + 3] == '='))
        v[k] = -2;
      else
        v[k] = Base64Value(text[i + k]);
      if (v[k] == -1)
        return std::nullopt;
    }
    if (v[0] < 0 || v[1] < 0)
      return std::nullopt;
    const u32 n = (static_cast<u32>(v[0]) << 18) | (static_cast<u32>(v[1]) << 12) |
                  (static_cast<u32>(std::max(v[2], 0)) << 6) | static_cast<u32>(std::max(v[3], 0));
    out.push_back(static_cast<u8>(n >> 16));
    if (v[2] >= 0)
      out.push_back(static_cast<u8>(n >> 8));
    else if ((n & 0xFFFF) != 0)
      return std::nullopt;
    if (v[3] >= 0)
      out.push_back(static_cast<u8>(n));
    else if ((n & 0xFF) != 0)
      return std::nullopt;
  }
  if (out.size() != size)
    return std::nullopt;
  return out;
}

picojson::object KeyframeOfferToJson(const KeyframeInfo& info)
{
  picojson::object d;
  d["k"] = picojson::value("kf");
  d["f"] = picojson::value(static_cast<double>(info.frame));
  d["id"] = picojson::value(info.id);
  d["n"] = picojson::value(static_cast<double>(info.size));
  d["x"] = picojson::value(info.hash);
  d["key"] = picojson::value(info.key);
  d["nm"] = NamesToJson(info.names);
  d["nv"] = picojson::value(static_cast<double>(info.names_version));
  if (!info.inline_blob.empty())
    d["b"] = picojson::value(EncodeInlineKeyframe(info.inline_blob));
  return d;
}

std::optional<KeyframeInfo> KeyframeOfferFromJson(const picojson::object& d)
{
  KeyframeInfo info;
  const auto frame = Int(d, "f", 0, MAX_FRAME);
  // YouGame's store accepts at most 128 MB.
  const auto size = Int(d, "n", 1, 128 << 20);
  info.id = Str(d, "id");
  info.hash = Str(d, "x");
  info.key = Str(d, "key");
  if (!frame || *frame > static_cast<int>(MAX_REPLAY_FRAMES) || !size || !ValidReplayId(info.id) ||
      info.hash.size() != 16 || info.key.size() != 64 ||
      info.id != fmt::format("replay-{}-{}", *frame, info.hash.substr(0, 8)))
  {
    return std::nullopt;
  }
  info.frame = *frame;
  info.size = static_cast<u64>(*size);
  // The replay inline: bounded by length before it is decoded, and only ever the stated size.
  if (const auto b = d.find("b"); b != d.end())
  {
    if (!b->second.is<std::string>() ||
        b->second.get<std::string>().size() > INLINE_KEYFRAME_MAX_TEXT)
      return std::nullopt;
    auto blob = DecodeInlineKeyframe(b->second.get<std::string>(), info.size);
    if (!blob)
      return std::nullopt;
    info.inline_blob = std::move(*blob);
  }
  info.names = NamesFromJson(d);
  if (const auto version = Int(d, "nv", 0, MAX_FRAME))
    info.names_version = *version;
  return info;
}

bool TicketAllowsDirect(const picojson::object& reply)
{
  const auto it = reply.find("direct");
  return it == reply.end() || !it->second.is<bool>() || it->second.get<bool>();
}

std::string GameStartMessage(const std::string& queue, const std::string& match, bool decided,
                             int start)
{
  if (queue != "ranked" || match.empty() || decided || start < 0)
    return "";
  return fmt::format("{{\"t\":\"game-start\",\"matchId\":{},\"id\":\"g{}\"}}", JsonString(match),
                     start);
}

bool ParseRoomJson(const std::string& text, picojson::value* value)
{
  return ParseJson(text, value);
}

bool DropKeepsRoom(const std::string& queue, bool match_live, bool set_over)
{
  return queue == "ranked" && match_live && !set_over;
}

bool KeepAliveDue(Clock::time_point now, std::optional<Clock::time_point> wanted,
                  std::optional<Clock::time_point> sent)
{
  if (!wanted || (sent && *wanted <= *sent))
    return false;
  return !sent || now - *sent >= KEEP_ALIVE_EVERY;
}

namespace
{
std::mutex s_known_mutex;
// RememberKnownSetGames: match id -> games known, the newest last (a ranked set is one match, so
// a few are plenty).
std::vector<std::pair<std::string, int>> s_known_by_match;
constexpr size_t KNOWN_MATCHES = 8;
}  // namespace

int RememberKnownSetGames(const std::string& match, int known)
{
  if (match.empty())
    return known;
  std::lock_guard lock(s_known_mutex);
  auto it = std::ranges::find(s_known_by_match, match, &std::pair<std::string, int>::first);
  if (it != s_known_by_match.end())
  {
    known = std::max(known, it->second);
    s_known_by_match.erase(it);
  }
  s_known_by_match.emplace_back(match, known);
  if (s_known_by_match.size() > KNOWN_MATCHES)
    s_known_by_match.erase(s_known_by_match.begin());
  return known;
}

std::string FinishMessage(const std::string& match, const std::string& winner)
{
  return winner.empty() ?
             fmt::format("{{\"t\":\"finish\",\"matchId\":{},\"void\":true}}", JsonString(match)) :
             fmt::format("{{\"t\":\"finish\",\"matchId\":{},\"winner\":{}}}", JsonString(match),
                         JsonString(winner));
}

bool DecodeDirectPacket(const std::string& payload, int slot, Packet* packet)
{
  Packet decoded;
  if (!DecodePacket(payload, &decoded) || decoded.seat != slot || !decoded.history.empty())
    return false;
  *packet = std::move(decoded);
  return true;
}

struct YouGameRoom::Impl
{
  RoomOptions options;
  std::thread thread;
  std::atomic<bool> stop{false};

  // Shared with the emulator thread.
  mutable std::mutex mutex;
  RoomState state = RoomState::Connecting;
  std::string status = "Connecting to YouGame...";
  std::string error_code;
  // Why this player leaves, told to the friend before going (an Orca::Status code, e.g. "desync").
  std::string leave_reason;
  std::string code;
  int seat = -1;
  // Each seat's player name from the room's roster (the same bytes on every machine; under mutex).
  std::map<int, std::string> seat_names;
  int rtt = -1;
  // Counts pongs, so a caller can tell a new measurement from a repeat read.
  u32 rtt_sequence = 0;
  // The latest RTT_SAMPLES measurements, oldest first.
  std::deque<int> recent_rtts;
  std::vector<Packet> inbox;
  // Which connection sent each inbox packet, so a departing player's unread packets go with it and
  // the next player in that seat doesn't read them as its own.
  std::vector<std::string> inbox_from;
  std::optional<Packet> outgoing;

  // Owned by the I/O thread.
  Common::WebSocket ws;
  std::string me, host;
  int revision = -1;
  // Every other participant, by connection id.
  struct Peer
  {
    int slot = -1;
    std::string name;
    // It said hello with our compatibility key; it heard ours; it was reported as a player.
    bool heard = false;
    bool heard_us = false;
    bool announced = false;
    bool drop_in_host = false;
    bool sent_packet = false;
    // A joiner's own controls from its hello ("ct"; empty: none).
    std::vector<u8> controls;
    // A joiner's queue identity from its hello ("qi"; empty: none).
    std::vector<u8> queue;
    // Its hello offered direct links (`dl`), and one is being built with it.
    bool direct = false;
    bool linked = false;
    std::optional<KeyframeInfo> keyframe;
    // The reason it gave for leaving (its "bye"), for this side's own report.
    std::string bye;
  };
  std::map<std::string, Peer> peers;
  Clock::time_point last_send{}, last_ping{}, last_hello{};
  // Shared with the emulator thread (under mutex): what happened to other players, and the keyframe
  // the host offers each joining seat.
  std::vector<PeerEvent> events;
  std::map<int, KeyframeInfo> keyframe_offers;
  std::vector<std::pair<int, std::string>> pending_drops;
  std::optional<std::pair<std::vector<KeyframeInfo::Name>, int>> pending_names;
  // Host: joins held (HoldJoins), and whether a change is still to be announced.
  bool holding = false;
  bool hold_told = true;
  // Whether this Orca joins someone else's game (RoomOptions::joining, or the app's invite), known
  // once the ticket is in (role_known, under mutex).
  bool joining = false;
  bool role_known = false;
  std::atomic<bool> offer_now{false};
  // The room as the page draws it (from welcome, join, leave, lobby and start).
  picojson::array room_participants, room_players;
  // This room's place among the process's rooms (s_room_generation), so its page mirror never
  // overwrites a newer room's.
  const u64 generation = ++s_room_generation;
  // I/O thread: when the welcome came (a joiner waits HOST_WAIT for its host from then), whether
  // the game's host ever said hello, and when the server last sent anything.
  std::optional<Clock::time_point> welcomed_at;
  bool host_seen = false;
  Clock::time_point last_heard{};

  // Under the desktop app, the room is mirrored to the YouGame page through the app's bridge
  // (mpRoom, latest only), and the page's lobby commands come back on its event stream. `stdio`:
  // that bridge is stdin and stdout (Orca/Bridge.h), and the lobby commands come with
  // `push_token`'s handler instead of the event stream.
  bool bridged = false;
  bool stdio = false;
  u64 push_token = 0;
  std::string bridge_url, bridge_token;
  // How tickets were minted (set once, before role_known): the dev site and player, and the
  // keyframe store's URL from the ticket reply.
  std::string site, dev_player, keyframes_url;
  std::string current_ticket;  // under mutex
  std::thread bridge_thread, events_thread;
  std::atomic<bool> bridge_done{false};
  std::mutex bridge_mutex;
  std::condition_variable bridge_wake;
  std::optional<std::string> bridge_pending;
  std::string last_published;  // I/O thread only
  bool mirror_closed = false;  // under s_mirror_mutex: the emptied room went out
  std::atomic<bool> leave_requested{false};
  std::atomic<bool> test_drop{false};
  std::vector<picojson::object> page_commands;  // guarded by bridge_mutex

  // A matchmade room: its queue from the welcome, the running room match (its id) and whether a
  // ranked set finished here (under mutex); and, from the emulator thread (under mutex), the host's
  // opponent plugged in, game reports, a desync.
  std::string queue = "private";
  bool stay = false;
  int room_size = MAX_SEATS;
  std::string match_id;
  bool set_done = false;
  // The room's `set-over` arrived: it stays open after the verdict until the players leave.
  bool set_over = false;
  bool opponent_plugged = false;
  struct HeldReport
  {
    GameReport report;
    Clock::time_point at;
    // A ranked game's start (report.start), not a result.
    bool game_start = false;
  };
  std::deque<HeldReport> reports;
  bool desync_pending = false;
  // KnownSetGames, kept by the I/O thread.
  int known_games = 0;
  // The session ended for the opponent's doing (ReportFault): the match in progress is void.
  bool fault_pending = false;
  // The opponent stopped sending inputs mid ranked set (ReportStall): this player claims the set.
  bool stall_pending = false;
  // KeepAlive: when the emulation thread last asked for one, and (I/O thread) when one last went.
  std::optional<Clock::time_point> alive_wanted;
  std::optional<Clock::time_point> alive_sent;
  // I/O thread: the last `finish` sent, and when to send it again after the room refused it while
  // its games were still being decided (a void must reach the match before this player goes).
  std::string last_finish;
  std::optional<Clock::time_point> finish_retry_at;
  int finish_retries = 0;
  // I/O thread: the room's participants (id, connection, slot), whether anyone is away, the host's
  // begin state (sent and unanswered, tries, waiting for a roster change after a refusal), and what
  // was reported and decided in this match.
  struct LobbyParticipant
  {
    std::string id, connection;
    int slot = -1;
  };
  std::vector<LobbyParticipant> lobby_participants;
  // The room match's participants as its `start` named them: ports map to these slots for the whole
  // match, whoever leaves later.
  std::vector<LobbyParticipant> match_participants;
  bool someone_away = false;
  std::optional<Clock::time_point> begin_sent;
  int begin_tries = 0;
  bool begin_wait_roster = false;
  int ranked_sent = 0;
  int casual_sent = 0;
  int casual_results = 0;
  // The casual sitting with this opponent so far: this player's wins and theirs.
  int casual_mine = 0, casual_theirs = 0;
  SetTally tally;
  bool finish_sent = false;
  std::set<std::string> verdicts_seen;
  // Why this side reported a game or the set void, and the detail it sent, by report id ("g<n>",
  // "c<n>" for a casual game, "$final").
  std::map<std::string, std::string> void_why;
  std::map<std::string, std::string> sent_detail;
  // A ranked set's reports from the match block, by report id: the game's number in the set and,
  // for the game that ended the set, the end (UX/RankedSet.h) as the finish names it.
  std::map<std::string, int> game_numbers;
  struct SetEnd
  {
    int done = 0;
    std::string winner;
  };
  std::map<std::string, SetEnd> set_ends;
  // Reports that came from the match block; only the block ends their set.
  std::set<std::string> block_ids;
  // Ranked games whose game-start went to this match (their fights' first frames).
  std::set<int> starts_sent;

  // Direct links to the other Orcas, unless ORCA_DIRECT=0. The pointer never changes after
  // construction; the link thread starts with the welcome.
  std::optional<DirectOptions> direct_options = DirectOptionsFromEnv();
  const std::unique_ptr<DirectLink> direct =
      direct_options ? std::make_unique<DirectLink>() : nullptr;
  bool direct_started = false;  // I/O thread only
  // Whether links are on, as the I/O thread last saw it (DirectAllowed; logged on change).
  std::optional<bool> direct_on;
  // The ticket reply's `direct` (TicketAllowsDirect): YouGame's own switch, re-read from every
  // ticket (under mutex).
  bool ticket_direct = true;
  Clock::time_point direct_summary_at{};
  DirectSummary direct_summary;  // under mutex
  // The ticket's ICE servers and when they were minted (under mutex).
  IceConfig ice;
  Clock::time_point ice_minted = Clock::now();
  // Emulator thread: the last direct copy, and the newest input it carried.
  Clock::time_point direct_sent_at{};
  int direct_sent_newest = -1;
  bool direct_malformed_logged = false;

  // Test-only network delay (Orca::TestNetDelayMs): messages wait in these queues, in order.
  struct Held
  {
    Clock::time_point due;
    std::string text;
  };
  std::optional<int> net_delay = TestNetDelayMs();
  int net_jitter = TestNetJitterMs();
  u32 net_count = 0;
  std::deque<Held> held_out, held_in;
  // Test-only spikes (Orca::TestNetSpikesConfig): the current or next one, from the room's start.
  std::optional<TestNetSpikes> net_spikes = TestNetSpikesConfig();
  const Clock::time_point net_opened = Clock::now();
  Clock::time_point spike_from = net_opened;
  Clock::time_point spike_to = net_opened;
  u32 spike_count = 0;
  // Test-only slow uplink (Orca::TestNetUplinkConfig): outgoing messages wait longer.
  std::optional<TestNetUplink> net_uplink = TestNetUplinkConfig();
  u32 uplink_count = 0;

  // The end of the spike `now` falls in, or `now`.
  Clock::time_point RadioFree(Clock::time_point now)
  {
    if (!net_spikes)
      return now;
    const auto draw = [this](int lo, int hi) {
      const u32 x = ++spike_count * 2246822519u ^ 0x9e3779b9u;
      return lo + static_cast<int>((x ^ (x >> 15)) % static_cast<u32>(hi - lo + 1));
    };
    // An exponential gap around the mean, from the same deterministic draws.
    const auto gap = [&] {
      if (!net_spikes->poisson)
        return draw(net_spikes->gap_min_ms, net_spikes->gap_max_ms);
      const double u = (draw(0, (1 << 24) - 1) + 1) / static_cast<double>(1 << 24);
      return static_cast<int>(std::lround(-std::log(u) * net_spikes->gap_min_ms));
    };
    while (spike_to <= now)
    {
      spike_from = spike_to + std::chrono::milliseconds(gap());
      spike_to = spike_from + std::chrono::milliseconds(draw(net_spikes->length_min_ms,
                                                             net_spikes->length_max_ms));
    }
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(spike_from - net_opened);
    if (now < spike_from || since.count() < net_spikes->from_s || since.count() >= net_spikes->to_s)
      return now;
    return spike_to;
  }

  Clock::time_point HoldUntil(const std::deque<Held>& line)
  {
    // A deterministic 0..jitter extra per message, after any spike (and, outgoing, a slow uplink's
    // hold); never earlier than the previous message.
    const auto now = Clock::now();
    const u32 extra = net_jitter ? (++net_count * 2654435761u) % static_cast<u32>(net_jitter + 1) : 0;
    auto due = RadioFree(now) + std::chrono::milliseconds(*net_delay + static_cast<int>(extra));
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(now - net_opened).count();
    if (&line == &held_out && net_uplink && since >= net_uplink->from_s && since < net_uplink->to_s)
    {
      const u32 x = ++uplink_count * 2246822519u ^ 0x85ebca6bu;
      const u32 more = (x ^ (x >> 13)) % static_cast<u32>(net_uplink->jitter_ms + 1);
      due += std::chrono::milliseconds(net_uplink->delay_ms + static_cast<int>(more));
    }
    return line.empty() ? due : std::max(due, line.back().due);
  }

  explicit Impl(RoomOptions room_options) : options(std::move(room_options))
  {
    joining = options.joining;
    site = Env("ORCA_SITE");
    if (site.empty())
      site = "https://yougame.co";
    while (site.ends_with("/"))
      site.pop_back();
    thread = std::thread([this] { Run(); });
  }

  ~Impl()
  {
    stop = true;
    if (thread.joinable())
      thread.join();
    Orca::Bridge::ClearPushHandler(std::exchange(push_token, 0));
    if (direct)
      direct->Stop();
  }

  RoomState State() const
  {
    std::lock_guard lock(mutex);
    return state;
  }

  void SetStatus(std::string line)
  {
    INFO_LOG_FMT(NETPLAY, "Orca room: {}", line);
    std::lock_guard lock(mutex);
    status = std::move(line);
  }

  // `app_code`: the app's error code for this end (Orca::Status); empty for a normal leave.
  void End(std::string why, std::string app_code = "network")
  {
    WARN_LOG_FMT(NETPLAY, "Orca room ended: {}", why);
    std::lock_guard lock(mutex);
    if (state == RoomState::Ended)
      return;
    state = RoomState::Ended;
    status = std::move(why);
    error_code = std::move(app_code);
  }

  // `held`: go through the test delay queue when configured (teardown messages skip it).
  bool SendJson(const picojson::object& message,
                std::chrono::milliseconds timeout = std::chrono::seconds(2), bool held = true)
  {
    if (held && net_delay)
    {
      held_out.push_back({HoldUntil(held_out), picojson::value(message).serialize()});
      return true;
    }
    if (ws.SendText(picojson::value(message).serialize(), timeout))
      return true;
    WARN_LOG_FMT(NETPLAY, "Orca room: send failed: {}", ws.Error());
    return false;
  }

  // The room's ticket and code, from the desktop app's bridge or (tests) the site.
  bool Ticket(std::string* url, std::string* ticket)
  {
    picojson::object lobby;
    lobby["version"] = picojson::value(1.0);
    lobby["minPlayers"] = picojson::value(2.0);
    lobby["maxLocalPlayers"] = picojson::value(1.0);
    lobby["compatibility"] = picojson::value(options.compatibility);

    const std::string bridge = Env("YOUGAME_BRIDGE"), token = Env("YOUGAME_TOKEN");
    const std::string dev = Env("ORCA_TEST_DEV_GAME");
    // The app's bridge on stdin and stdout (a sandboxed Orca), else on loopback.
    stdio = Orca::Bridge::Enabled();
    const bool use_bridge = stdio || (!bridge.empty() && !token.empty());
    if (!use_bridge && dev.empty())
    {
      End("Start Orca from the YouGame app to play online");
      return false;
    }
    // The bridge is the desktop app on loopback; nothing else may receive the token.
    if (use_bridge && !stdio && !LoopbackBridge(bridge))
    {
      End("Invalid YouGame app bridge address");
      return false;
    }
    if (use_bridge)
    {
      bridged = true;
      if (!stdio)
      {
        bridge_url = bridge.ends_with("/") ? bridge.substr(0, bridge.size() - 1) : bridge;
        bridge_token = token;
      }
    }

    // Leave() must not wait out a slow request: the progress callback aborts it.
    Common::HttpRequest http(TICKET_TIMEOUT, [this](s64, s64, s64, s64) { return !stop.load(); });
    const auto post = [&](const std::string& target, const picojson::object& body,
                          const Common::HttpRequest::Headers& headers,
                          picojson::object* result) -> bool {
      Common::HttpRequest::Response response;
      // On the stdio bridge, the app's reply itself.
      std::optional<picojson::object> answer;
      for (int attempt = 1; attempt <= TICKET_TRIES && !stop; ++attempt)
      {
        const auto start = Clock::now();
        if (stdio)
        {
          answer = Orca::Bridge::Request(body, TICKET_TIMEOUT, [this] { return stop.load(); });
          if (answer)
            break;
        }
        else
        {
          response = http.Post(target, picojson::value(body).serialize(), headers,
                               Common::HttpRequest::AllowedReturnCodes::All);
          if (response)
            break;
        }
        WARN_LOG_FMT(
            NETPLAY, "Orca room: no reply from {} in {} ms (HTTP {}, try {} of {})",
            stdio ? "the app" : target,
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count(),
            http.GetLastResponseCode(), attempt, TICKET_TRIES);
        const auto until = Clock::now() + std::chrono::seconds(1);
        while (attempt < TICKET_TRIES && !stop && Clock::now() < until)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (!response && !answer)
      {
        End(stop ? "You left the room" : "YouGame did not respond", stop ? "" : "network");
        return false;
      }
      picojson::value json;
      if (answer)
      {
        json = picojson::value(std::move(*answer));
      }
      else if (!ParseJson(std::string(response->begin(), response->end()), &json) ||
               !json.is<picojson::object>())
      {
        End(fmt::format("YouGame sent an unreadable reply (HTTP {})", http.GetLastResponseCode()));
        return false;
      }
      picojson::object o = json.get<picojson::object>();
      if (use_bridge)
      {
        // The bridge wraps SDK replies: {ok, result} or {ok: false, error}.
        if (!o.count("ok") || !o["ok"].evaluate_as_boolean() ||
            !o["result"].is<picojson::object>())
        {
          End(Str(o, "error").empty() ? "The YouGame app refused the request" : Str(o, "error"));
          return false;
        }
        // Copied out first: assigning a map from one of its own elements frees it mid-copy.
        picojson::object unwrapped = o["result"].get<picojson::object>();
        o = std::move(unwrapped);
      }
      else if (http.GetLastResponseCode() != 200)
      {
        End(Str(o, "error").empty() ? "YouGame refused the ticket" : Str(o, "error"));
        return false;
      }
      *result = std::move(o);
      return true;
    };

    std::string room = options.room_code;
    if (room.empty())
      room = Env("ORCA_TEST_ROOM");
    picojson::object reply;
    if (use_bridge)
    {
      const Common::HttpRequest::Headers headers{{"Authorization", "Bearer " + token},
                                                 {"Content-Type", "application/json"}};
      if (room.empty() && options.ask_app_for_room)
      {
        // An invite the player opened in the app names the room.
        picojson::object hello;
        hello["type"] = picojson::value("hello");
        if (!post(bridge + "/sdk", hello, headers, &reply))
          return false;
        room = Str(reply, "room");
        // The app opened an invite: this Orca joins the friend's game.
        if (!room.empty())
          joining = true;
      }
      if (room.empty())
        room = RandomString(CODE_ALPHABET, 8);
      if (!ValidCode(room))
      {
        End("Room codes are 6 to 12 lowercase letters and digits");
        return false;
      }
      picojson::object request;
      request["type"] = picojson::value("mpTicket");
      request["players"] = picojson::value(static_cast<double>(MAX_SEATS));
      request["mode"] = picojson::value(options.mode);
      request["ranked"] = picojson::value(false);
      request["room"] = picojson::value(room);
      request["lobby"] = picojson::value(lobby);
      if (!post(bridge + "/sdk", request, headers, &reply))
        return false;
    }
    else
    {
      if (room.empty())
        room = RandomString(CODE_ALPHABET, 8);
      if (!ValidCode(room))
      {
        End("Room codes are 6 to 12 lowercase letters and digits");
        return false;
      }
      std::string player = Env("ORCA_TEST_PLAYER");
      if (player.empty())
        player = "orca-" + RandomString("abcdefghijklmnopqrstuvwxyz0123456789", 15);
      dev_player = player;
      picojson::object request;
      request["dev"] = picojson::value(dev);
      request["player_id"] = picojson::value(player);
      request["name"] = picojson::value(options.player_name);
      request["players"] = picojson::value(static_cast<double>(MAX_SEATS));
      request["mode"] = picojson::value(options.mode);
      request["room"] = picojson::value(room);
      request["lobby"] = picojson::value(lobby);
      if (!post(site + "/api/multiplayer/ticket", request, {{"Content-Type", "application/json"}},
                &reply))
      {
        return false;
      }
    }
    *url = Str(reply, "url");
    *ticket = Str(reply, "ticket");
    if (url->empty() || ticket->empty() || !AcceptableRoomUrl(*url))
    {
      End("YouGame sent no usable room ticket");
      return false;
    }
    keyframes_url = KeyframesUrl(Str(reply, "keyframes"));
    std::lock_guard lock(mutex);
    code = room;
    current_ticket = *ticket;
    role_known = true;
    if (const auto it = reply.find("ice"); it != reply.end())
      ParseIce(it->second, &ice);
    ice_minted = Clock::now();
    ticket_direct = TicketAllowsDirect(reply);
    return true;
  }

  // The keyframe store's base URL: what the ticket reply names, if it's on the site the tickets
  // come from (ORCA_SITE, else yougame.co); otherwise the site's default.
  std::string KeyframesUrl(const std::string& named) const
  {
    if (!named.empty() && named.starts_with(site + "/"))
      return named.ends_with("/") ? named.substr(0, named.size() - 1) : named;
    return site + "/api/orca/keyframes";
  }

  // A fresh ticket for the keyframe store (tickets last five minutes; a keyframe goes out much
  // later). Any thread, once the room has its first ticket; no side effects.
  bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error)
  {
    std::string room;
    {
      std::lock_guard lock(mutex);
      if (!role_known)
      {
        *error = "the room has no ticket yet";
        return false;
      }
      room = code;
      // YouGame's keyframe store accepts a room ticket for hours past expiry, so only mint another
      // when it refused this one.
      if (!fresh && !current_ticket.empty())
      {
        *ticket = current_ticket;
        *store_url = keyframes_url + "/" + room;
        return true;
      }
    }
    picojson::object lobby;
    lobby["version"] = picojson::value(1.0);
    lobby["minPlayers"] = picojson::value(2.0);
    lobby["maxLocalPlayers"] = picojson::value(1.0);
    lobby["compatibility"] = picojson::value(options.compatibility);
    picojson::object request;
    request["players"] = picojson::value(static_cast<double>(MAX_SEATS));
    request["mode"] = picojson::value(options.mode);
    request["room"] = picojson::value(room);
    request["lobby"] = picojson::value(lobby);
    Common::HttpRequest http(std::chrono::seconds(10));
    Common::HttpRequest::Response response;
    std::optional<picojson::object> answer;  // the stdio bridge's reply
    if (bridged && stdio)
    {
      request["type"] = picojson::value("mpTicket");
      request["ranked"] = picojson::value(false);
      answer = Orca::Bridge::Request(std::move(request), std::chrono::seconds(10));
    }
    else if (bridged)
    {
      request["type"] = picojson::value("mpTicket");
      request["ranked"] = picojson::value(false);
      response = http.Post(bridge_url + "/sdk", picojson::value(request).serialize(),
                           {{"Authorization", "Bearer " + bridge_token},
                            {"Content-Type", "application/json"}},
                           Common::HttpRequest::AllowedReturnCodes::All);
    }
    else
    {
      request["dev"] = picojson::value(Env("ORCA_TEST_DEV_GAME"));
      request["player_id"] = picojson::value(dev_player);
      request["name"] = picojson::value(options.player_name);
      response = http.Post(site + "/api/multiplayer/ticket", picojson::value(request).serialize(),
                           {{"Content-Type", "application/json"}},
                           Common::HttpRequest::AllowedReturnCodes::All);
    }
    picojson::value json;
    if (answer)
    {
      json = picojson::value(std::move(*answer));
    }
    else if (stdio)
    {
      *error = "no ticket from the app";
      return false;
    }
    else if (!response || !ParseJson(std::string(response->begin(), response->end()), &json) ||
             !json.is<picojson::object>())
    {
      *error = fmt::format("no ticket from YouGame (HTTP {})", http.GetLastResponseCode());
      return false;
    }
    picojson::object reply = json.get<picojson::object>();
    if (bridged)
    {
      if (!reply.count("ok") || !reply["ok"].evaluate_as_boolean() ||
          !reply["result"].is<picojson::object>())
      {
        *error = Str(reply, "error").empty() ? "the app refused a ticket" : Str(reply, "error");
        return false;
      }
      picojson::object unwrapped = reply["result"].get<picojson::object>();
      reply = std::move(unwrapped);
    }
    else if (http.GetLastResponseCode() != 200)
    {
      *error = Str(reply, "error").empty() ? "YouGame refused a ticket" : Str(reply, "error");
      return false;
    }
    *ticket = Str(reply, "ticket");
    if (ticket->empty())
    {
      *error = "YouGame sent no ticket";
      return false;
    }
    *store_url = KeyframesUrl(Str(reply, "keyframes")) + "/" + room;
    std::lock_guard lock(mutex);
    current_ticket = *ticket;
    if (const auto it = reply.find("ice"); it != reply.end() && ParseIce(it->second, &ice))
      ice_minted = Clock::now();
    ticket_direct = TicketAllowsDirect(reply);
    return true;
  }

  // Fresh TURN credentials (a new ticket) for a direct link built long after the room opened. Runs
  // on the direct link's thread.
  std::optional<IceConfig> FreshIce()
  {
    std::string ticket, store, error;
    if (!FreshTicket(true, &ticket, &store, &error))
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: no fresh ICE servers for a direct link: {}", error);
      return std::nullopt;
    }
    std::lock_guard lock(mutex);
    return ice;
  }

  // Whether direct links may run now, and if not, why: YouGame's ticket can turn them off for
  // everyone (`direct: false`), and the player can too (the app's "direct off"). ORCA_DIRECT=0
  // builds no DirectLink at all.
  bool DirectAllowed(std::string* why)
  {
    if (!direct)
    {
      *why = "ORCA_DIRECT=0";
      return false;
    }
    {
      std::lock_guard lock(mutex);
      if (!ticket_direct)
      {
        *why = "YouGame's ticket turned them off";
        return false;
      }
    }
    if (Orca::Status::Cap("direct") && !Orca::Online::DirectWanted())
    {
      *why = "the player turned them off";
      return false;
    }
    return true;
  }

  // Direct links start with the welcome (this Orca's connection id is the link's identity), or
  // later when turned on.
  void StartDirect()
  {
    if (!direct || direct_started || me.empty())
      return;
    direct_started = true;
    DirectOptions o = *direct_options;
    o.me = me;
    {
      std::lock_guard lock(mutex);
      o.room = code;
      o.ice = ice;
      o.ice_minted = ice_minted;
    }
    o.fresh_ice = [this] { return FreshIce(); };
    NOTICE_LOG_FMT(NETPLAY, "Orca room: direct links on{}{}{}", o.force.empty() ? "" : ", forced ",
                 o.force, o.ice.turn_host.empty() ? ", no TURN in the ticket" : "");
    direct->Start(std::move(o));
  }

  // Link each player whose hello offered direct links, once both sides heard each other. While
  // links are off, drop them all and inputs take the relay.
  void LinkPeers()
  {
    if (!direct || me.empty())
      return;
    std::string why;
    const bool on = DirectAllowed(&why);
    if (direct_on != on)
    {
      if (!on)
      {
        NOTICE_LOG_FMT(NETPLAY, "Orca room: direct links off ({}): inputs go through the relay",
                       why);
      }
      else if (direct_on)
      {
        NOTICE_LOG_FMT(NETPLAY, "Orca room: direct links on again");
      }
      direct_on = on;
    }
    if (!on)
    {
      for (auto& [id, peer] : peers)
        UnlinkPeer(id, peer);
      if (direct_started)
        direct->TakeSignals();
      std::lock_guard lock(mutex);
      direct_summary = {};
      return;
    }
    StartDirect();
    for (auto& [id, peer] : peers)
    {
      if (peer.announced && peer.heard && peer.heard_us && peer.direct && !peer.linked)
      {
        direct->AddPeer(id, peer.slot);
        peer.linked = true;
      }
    }
    for (auto& [to, d] : direct->TakeSignals())
    {
      if (const auto p = peers.find(to); p != peers.end() && p->second.linked)
        SendTo(to, d);
    }
    const auto now = Clock::now();
    if (now - direct_summary_at < DIRECT_SUMMARY_EVERY)
      return;
    direct_summary_at = now;
    // The worst link among the game's players: relay, then TURN, then direct.
    const auto rank = [](LinkKind kind) {
      return kind == LinkKind::Relay ? 0 : kind == LinkKind::Turn ? 1 : 2;
    };
    std::optional<DirectStats> worst;
    for (const auto& [id, peer] : peers)
    {
      if (!peer.announced)
        continue;
      DirectStats stats;
      if (peer.linked)
        stats = direct->Stats(id).value_or(DirectStats{});
      if (!worst || rank(stats.kind) < rank(worst->kind))
        worst = stats;
    }
    DirectSummary summary;
    if (worst && worst->kind != LinkKind::Relay)
    {
      summary.transport = static_cast<int>(worst->kind);
      summary.rtt_ms = worst->rtt_ms;
      summary.line = fmt::format("{} {}{}", worst->kind == LinkKind::Turn ? "TURN" : "direct",
                                 worst->pair.empty() ? "?" : worst->pair,
                                 worst->rtt_ms >= 0 ? fmt::format(" {} ms", worst->rtt_ms) : "");
    }
    std::lock_guard lock(mutex);
    direct_summary = summary;
  }

  void UnlinkPeer(const std::string& id, Peer& peer)
  {
    if (peer.linked)
      direct->RemovePeer(id);
    peer.linked = false;
  }

  // The emulator thread's half of Send: a copy of the packet straight to every linked player, on
  // this thread, without the room's lock. Never drop-in history (the relay carries it, in order),
  // and only as many inputs as a datagram holds (the oldest first, as catch-up needs).
  void SendDirect(const Packet& packet)
  {
    if (!direct)
      return;
    const auto now = Clock::now();
    const int newest =
        packet.pads.empty() ? -1 : packet.first_frame + static_cast<int>(packet.pads.size()) - 1;
    if (newest < direct_sent_newest - 600)
      direct_sent_newest = -1;  // another session, from an earlier frame
    if (newest <= direct_sent_newest && !packet.checksum_frame && !packet.leaving &&
        now - direct_sent_at < DIRECT_STALL_GAP)
    {
      return;
    }
    direct_sent_at = now;
    direct_sent_newest = std::max(direct_sent_newest, newest);
    Packet copy = packet;
    copy.history_seat = -1;
    copy.history_first = 0;
    copy.history.clear();
    std::string payload = EncodePacket(copy);
    while (payload.size() > DirectLink::MAX_PAYLOAD && copy.pads.size() > 1)
    {
      // Each pad is 16 hex digits.
      const size_t over = (payload.size() - DirectLink::MAX_PAYLOAD + 15) / 16;
      copy.pads.resize(copy.pads.size() > over ? copy.pads.size() - over : 1);
      payload = EncodePacket(copy);
    }
    if (payload.size() <= DirectLink::MAX_PAYLOAD)
      direct->Send(payload);
  }

  void Run()
  {
    // Every input to and from friends passes through this thread (see PrioritizeThread).
    Orca::PrioritizeThread(Orca::LatencyThread::Room);
    std::string url, ticket;
    if (!Ticket(&url, &ticket))
      return;
    if (stop)
    {
      End("You left the room", "");
      return;
    }
    std::string room;
    {
      std::lock_guard lock(mutex);
      room = code;
    }
    Common::HttpRequest escape;
    std::string error;
    if (!ws.Connect(WebSocketUrl(url) + "/room/" + room + "?ticket=" +
                        escape.EscapeComponent(ticket),
                    std::chrono::seconds(8), &error))
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: {}", error);
      End(RefusalReason(error), RefusalCode(error));
      return;
    }
    SetStatus(fmt::format("Room {}: waiting for your friend", room));
    last_heard = Clock::now();
    if (bridged)
    {
      bridge_thread = std::thread([this] { BridgeLoop(); });
      if (stdio)
      {
        push_token = Orca::Bridge::SetPushHandler(
            [this](const picojson::object& event) { OnPageEvent(event); });
      }
      else
      {
        events_thread = std::thread([this] { EventsLoop(); });
      }
    }

    if (net_delay)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: TEST network delay {} ms + 0..{} ms each way", *net_delay,
                   net_jitter);
    }
    if (net_spikes)
    {
      WARN_LOG_FMT(NETPLAY,
                   "Orca room: TEST network spikes: dark both ways for {}-{} ms after every "
                   "{}{}-{} ms, from {} s to {} s",
                   net_spikes->length_min_ms, net_spikes->length_max_ms,
                   net_spikes->poisson ? "(on average) " : "", net_spikes->gap_min_ms,
                   net_spikes->gap_max_ms, net_spikes->from_s, net_spikes->to_s);
    }
    if (net_uplink)
    {
      WARN_LOG_FMT(NETPLAY,
                   "Orca room: TEST slow uplink: what it sends waits {} ms + 0..{} ms, from {} s "
                   "to {} s",
                   net_uplink->delay_ms, net_uplink->jitter_ms, net_uplink->from_s,
                   net_uplink->to_s);
    }
    // The connection failed under the player (went quiet, or a test dropped it): see below.
    bool dropped = false;
    while (!stop && State() != RoomState::Ended)
    {
      if (leave_requested)
      {
        End("You left the room", "");
        break;
      }
      if (test_drop)
      {
        WARN_LOG_FMT(NETPLAY, "Orca room: TEST dropping the connection");
        dropped = true;
        End("Lost the connection to the room (test drop)");
        break;
      }
      if (bridged)
        SendPageCommands();
      if (net_delay)
        FlushHeld();
      if (auto text = ws.ReceiveText(std::chrono::milliseconds(net_delay ? 1 : 2)))
      {
        last_heard = Clock::now();
        if (net_delay)
          held_in.push_back({HoldUntil(held_in), std::move(*text)});
        else
          OnMessage(*text);
        continue;
      }
      // Under the test delay, what the server sent before closing is still in flight and comes
      // first, as on a real link (e.g. a ranked verdict just ahead of the room's 4004).
      if (!ws.IsOpen() && net_delay && !held_in.empty())
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (!ws.IsOpen())
      {
        // 4000: the server replaced this socket with a newer one for the same player (another run
        // of this game on this account in the same room). A host leaves the room to it rather than
        // take it back, which would just replace that one in turn. 4004: a matchmade room closed
        // after its result: a clean end.
        if (ws.CloseCode() == CLOSE_MATCH_OVER)
        {
          End("The match is over", "match-over");
          break;
        }
        const bool replaced = ws.CloseCode() == 4000;
        End(fmt::format("{} ({})",
                        replaced ? "Another game of yours took over this room" :
                                   "Lost the connection to the room",
                        ws.Error().empty() ? "no reason given" : ws.Error()),
            replaced && !joining ? "taken" : "network");
        break;
      }
      if (Clock::now() - last_heard > SERVER_SILENCE_LIMIT)
      {
        dropped = true;
        End(fmt::format("Lost the connection to the room (no answer for {} s)",
                        SERVER_SILENCE_LIMIT.count()));
        break;
      }
      Tick();
    }

    // Direct links close with the room (the session ends with it).
    if (direct)
      direct->Stop();
    // The bridge threads wind down while the goodbyes go out. First tell the page the room is gone,
    // ahead of the next room's mirror if any (otherwise that one stands and this is dropped:
    // PostMirror).
    bridge_done = true;
    bridge_wake.notify_all();
    if (bridged)
      PublishEnded();
    std::string reason;
    {
      std::lock_guard lock(mutex);
      reason = leave_reason;
    }
    // A connection that failed under the player leaves without a goodbye and with a close the
    // server reads as a drop, so it holds the seat (and host role) for the player's next
    // connection.
    if (dropped)
    {
      // Tests only (ORCA_TEST_DROP_SILENT=1): a drop the server never sees, as when the network
      // goes away under the player: no close, and the connection stays open, unread, until the
      // player's next connection replaces it (Online::TestDropRoom keeps this room).
      if (Env("ORCA_TEST_DROP_SILENT") != "1")
        ws.Close(4001);
    }
    else
    {
      // Reports and a void the emulator thread left for this room go out before the goodbye.
      if (ws.IsOpen())
        FlushLobby();
      if (!reason.empty() && !peers.empty() && ws.IsOpen())
      {
        picojson::object d;
        d["k"] = picojson::value("bye");
        d["r"] = picojson::value(reason);
        picojson::object message;
        message["t"] = picojson::value("msg");
        message["d"] = picojson::value(d);
        SendJson(message, std::chrono::milliseconds(500), false);
      }
      if (ws.IsOpen())
      {
        picojson::object leave;
        leave["t"] = picojson::value("leave");
        SendJson(leave, std::chrono::milliseconds(500), false);
      }
      ws.Close();
    }
    StopBridge();
  }

  // The lobby the page shows: the same mpRoom a web game's SDK publishes (Player.tsx parses it).
  // `ended`: the room's code with nobody in it, which the page reads as Orca having left, so a
  // later invite into the same room isn't mistaken for one Orca is already in.
  picojson::object RoomMirror(bool ended)
  {
    picojson::object lobby;
    lobby["version"] = picojson::value(1.0);
    lobby["minPlayers"] = picojson::value(2.0);
    lobby["maxLocalPlayers"] = picojson::value(1.0);
    lobby["compatibility"] = picojson::value(options.compatibility);
    picojson::object room;
    room["type"] = picojson::value("mpRoom");
    room["lifecycle"] = picojson::value("game");
    room["participants"] = picojson::value(ended ? picojson::array{} : room_participants);
    room["revision"] = picojson::value(static_cast<double>(revision));
    {
      std::lock_guard lock(mutex);
      room["matchId"] = match_id.empty() || ended ? picojson::value() : picojson::value(match_id);
    }
    room["lobby"] = picojson::value(lobby);
    room["me"] = picojson::value(me);
    {
      std::lock_guard lock(mutex);
      room["code"] = picojson::value(code);
      room["ready"] =
          picojson::value(!ended && std::any_of(peers.begin(), peers.end(),
                                                [](const auto& p) { return p.second.announced; }));
    }
    {
      std::lock_guard lock(mutex);
      room["queue"] = picojson::value(queue);
      room["size"] = picojson::value(static_cast<double>(room_size));
    }
    room["host"] = ended ? picojson::value() : picojson::value(host);
    room["readyIds"] = picojson::value(picojson::array{});
    room["players"] = picojson::value(ended ? picojson::array{} : room_players);
    return room;
  }

  void Publish()
  {
    if (!bridged)
      return;
    std::string snapshot = picojson::value(RoomMirror(false)).serialize();
    if (snapshot == last_published)
      return;
    last_published = snapshot;
    {
      std::lock_guard lock(bridge_mutex);
      bridge_pending = std::move(snapshot);
    }
    bridge_wake.notify_all();
  }

  // At teardown, on the I/O thread: post the emptied room at once (the bridge thread is winding
  // down), if the page was shown this room.
  void PublishEnded()
  {
    if (last_published.empty())
      return;
    Common::HttpRequest http(MIRROR_END_TIMEOUT);
    if (!PostMirror(http, picojson::value(RoomMirror(true)).serialize(), true, MIRROR_END_TIMEOUT))
      WARN_LOG_FMT(NETPLAY, "Orca room: the app did not take the room's end");
  }

  // One mirror update to the app, unless a newer room already posted (this one is stale) or this
  // room already posted its end (`last` marks the end). True once the app took it, or if there was
  // nothing to send. On the stdio bridge it waits up to `timeout` for the app, or until
  // `cancelled`.
  bool PostMirror(Common::HttpRequest& http, const std::string& body, bool last = false,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5),
                  const std::function<bool()>& cancelled = {})
  {
    std::lock_guard lock(s_mirror_mutex);
    if (generation < s_mirror_newest || mirror_closed)
      return true;
    s_mirror_newest = generation;
    mirror_closed = last;
    if (stdio)
    {
      picojson::value request;
      if (!ParseJson(body, &request) || !request.is<picojson::object>())
        return false;
      const auto reply =
          Orca::Bridge::Request(std::move(request.get<picojson::object>()), timeout, cancelled);
      return reply && reply->count("ok") && reply->at("ok").evaluate_as_boolean();
    }
    const auto response = http.Post(
        bridge_url + "/sdk", body,
        {{"Authorization", "Bearer " + bridge_token}, {"Content-Type", "application/json"}},
        Common::HttpRequest::AllowedReturnCodes::All);
    picojson::value reply;
    return response && http.GetLastResponseCode() == 200 &&
           ParseJson(std::string(response->begin(), response->end()), &reply) &&
           reply.is<picojson::object>() && reply.get("ok").evaluate_as_boolean();
  }

  void SendPageCommands()
  {
    std::vector<picojson::object> commands;
    {
      std::lock_guard lock(bridge_mutex);
      commands.swap(page_commands);
    }
    for (const auto& command : commands)
      SendJson(command);
  }

  void BridgeLoop()
  {
    Common::HttpRequest http(std::chrono::seconds(5),
                             [this](s64, s64, s64, s64) { return !bridge_done.load(); });
    while (!bridge_done)
    {
      std::optional<std::string> body;
      {
        std::unique_lock lock(bridge_mutex);
        bridge_wake.wait_for(lock, std::chrono::milliseconds(200),
                             [this] { return bridge_done.load() || bridge_pending.has_value(); });
        body.swap(bridge_pending);
      }
      if (!body || bridge_done)
        continue;
      if (!PostMirror(http, *body, false, std::chrono::seconds(5),
                      [this] { return bridge_done.load(); }))
      {
        WARN_LOG_FMT(NETPLAY, "Orca room: the app did not take the room update (HTTP {})",
                     http.GetLastResponseCode());
      }
    }
  }

  // One event from the app's stream: a lobby command the page sent this game.
  void OnPageEvent(const std::string& line)
  {
    picojson::value value;
    if (!ParseJson(line, &value) || !value.is<picojson::object>())
      return;
    OnPageEvent(value.get<picojson::object>());
  }

  // The same command, already parsed (the stream's line, or a push on the stdio bridge).
  void OnPageEvent(const picojson::object& e)
  {
    // Only this game's own page may command it.
    const std::string game = Env("YOUGAME_GAME");
    if (game.empty() || Str(e, "slug") != game)
      return;
    const std::string cmd = Str(e, "cmd");
    if (cmd == "leave")
    {
      // The lobby's Leave is the app's "leave": a joiner unplugs at an agreed frame and plays on; a
      // host gives up its friends and opens a new room (at a frame boundary). An app without the
      // "leave" cap gets the old behaviour: the room just ends.
      if (Orca::Status::Cap("leave"))
        Orca::Online::RequestLeave();
      else
        leave_requested = true;
    }
    else if (cmd == "kick" && !Str(e, "id").empty())
    {
      picojson::object kick;
      kick["t"] = picojson::value("kick");
      kick["id"] = picojson::value(Str(e, "id"));
      std::lock_guard lock(bridge_mutex);
      if (page_commands.size() < 16)
        page_commands.push_back(std::move(kick));
    }
    // ready, setting, local-add and local-remove don't apply: one controller per Orca, and the
    // host's Orca starts the match itself.
  }

  struct EventStream
  {
    Impl* impl;
    std::string pending;
  };

  static size_t OnEventData(char* data, size_t size, size_t count, void* user)
  {
    auto* stream = static_cast<EventStream*>(user);
    stream->pending.append(data, size * count);
    if (stream->pending.size() > 65536)
      return 0;  // abort the transfer
    size_t newline;
    while ((newline = stream->pending.find('\n')) != std::string::npos)
    {
      std::string line = stream->pending.substr(0, newline);
      stream->pending.erase(0, newline + 1);
      if (line.ends_with("\r"))
        line.pop_back();
      // SSE: "data:" and an optional space.
      if (line.starts_with("data:"))
        stream->impl->OnPageEvent(line.substr(line.starts_with("data: ") ? 6 : 5));
    }
    return size * count;
  }

  static int OnEventProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
  {
    return static_cast<EventStream*>(user)->impl->bridge_done ? 1 : 0;
  }

  void EventsLoop()
  {
    // The stream stays open for the whole session; reconnect if the app restarts it.
    int backoff = 500;
    while (!bridge_done)
    {
      CURL* curl = curl_easy_init();
      if (!curl)
      {
        ERROR_LOG_FMT(NETPLAY, "Orca room: couldn't open the app's event stream");
        return;
      }
      EventStream stream{this, {}};
      curl_slist* headers =
          curl_slist_append(nullptr, ("Authorization: Bearer " + bridge_token).c_str());
      curl_easy_setopt(curl, CURLOPT_URL, (bridge_url + "/events").c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnEventData);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &OnEventProgress);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stream);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      const CURLcode result = curl_easy_perform(curl);
      long http_status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      if (bridge_done)
        return;
      // An app without the stream, or one that refuses the token, won't change its mind.
      if (http_status == 401 || http_status == 403 || http_status == 404)
      {
        ERROR_LOG_FMT(NETPLAY, "Orca room: the app refused its event stream (HTTP {}); the page's "
                               "lobby commands won't reach this game",
                      http_status);
        return;
      }
      WARN_LOG_FMT(NETPLAY, "Orca room: the app's event stream ended ({}, HTTP {})",
                   curl_easy_strerror(result), http_status);
      backoff = std::min(backoff * 2, 10000);
      for (int waited = 0; waited < backoff && !bridge_done; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  void StopBridge()
  {
    bridge_done = true;
    bridge_wake.notify_all();
    Orca::Bridge::ClearPushHandler(std::exchange(push_token, 0));
    if (bridge_thread.joinable())
      bridge_thread.join();
    if (events_thread.joinable())
      events_thread.join();
  }

  void FlushHeld()
  {
    const auto now = Clock::now();
    while (!held_out.empty() && held_out.front().due <= now)
    {
      if (ws.IsOpen() && !ws.SendText(held_out.front().text, std::chrono::seconds(2)))
        WARN_LOG_FMT(NETPLAY, "Orca room: send failed: {}", ws.Error());
      held_out.pop_front();
    }
    while (!held_in.empty() && held_in.front().due <= now && State() != RoomState::Ended)
    {
      const std::string text = std::move(held_in.front().text);
      held_in.pop_front();
      OnMessage(text);
    }
  }

  // ---- Matchmade rooms (ORCA.md "Matchmaking") ----

  // The lobby fields carried on welcome, join, leave, back, away, lobby and start: the participants
  // (the ids results name) and the running room match.
  void LobbyFields(const picojson::object& m)
  {
    if (const auto it = m.find("matchId"); it != m.end())
    {
      std::string id = it->second.is<std::string>() ? it->second.get<std::string>() : "";
      if (id.size() > 64)
        id.clear();
      std::lock_guard lock(mutex);
      if (id != match_id)
      {
        NOTICE_LOG_FMT(NETPLAY, "Orca room: room match {}", id.empty() ? "over" : id);
        // A match this process played before its room link dropped: the games it knew of come back.
        if (!id.empty())
          known_games = RememberKnownSetGames(id, 0);
        if (!id.empty() && known_games > 0)
        {
          NOTICE_LOG_FMT(NETPLAY, "Orca room: back in match {}, which this player knew {} game(s) of",
                         id, known_games);
        }
      }
      match_id = std::move(id);
    }
    if (const auto it = m.find("participants"); it != m.end() && it->second.is<picojson::array>())
    {
      lobby_participants.clear();
      for (const auto& p : it->second.get<picojson::array>())
      {
        if (!p.is<picojson::object>() || lobby_participants.size() >= MAX_SEATS * 2)
          continue;
        const auto& o = p.get<picojson::object>();
        const auto slot = Int(o, "slot", 0, MAX_SEATS - 1);
        const std::string id = Str(o, "id"), connection = Str(o, "connectionId");
        if (!slot || id.empty() || id.size() > 128)
          continue;
        lobby_participants.push_back({id, connection, *slot});
      }
    }
    if (const auto it = m.find("players"); it != m.end() && it->second.is<picojson::array>())
    {
      someone_away = false;
      for (const auto& p : it->second.get<picojson::array>())
      {
        if (p.is<picojson::object>() && p.get<picojson::object>().count("away") &&
            p.get<picojson::object>().at("away").evaluate_as_boolean())
        {
          someone_away = true;
        }
      }
    }
    // A refused begin waits for the roster to change.
    begin_wait_roster = false;
  }

  // The participants results name: the room match's once it started, else the room's.
  const std::vector<LobbyParticipant>& Participants() const
  {
    return match_participants.empty() ? lobby_participants : match_participants;
  }

  std::string ParticipantAt(int slot) const
  {
    for (const auto& p : Participants())
    {
      if (p.slot == slot)
        return p.id;
    }
    return "";
  }

  // This player's participant id and slot, and the opponent's id.
  std::string MyId() const
  {
    for (const auto& p : Participants())
    {
      if (p.connection == me)
        return p.id;
    }
    return "";
  }
  int MySlot() const
  {
    for (const auto& p : Participants())
    {
      if (p.connection == me)
        return p.slot;
    }
    return -1;
  }
  std::string TheirId() const
  {
    for (const auto& p : Participants())
    {
      if (p.connection != me)
        return p.id;
    }
    return "";
  }

  void SendRaw(const std::string& text)
  {
    if (net_delay)
    {
      held_out.push_back({HoldUntil(held_out), text});
      return;
    }
    if (!ws.SendText(text, std::chrono::seconds(2)))
      WARN_LOG_FMT(NETPLAY, "Orca room: send failed: {}", ws.Error());
  }

  // The host's Orca starts the room's match once its opponent plays in its game.
  void TryBegin()
  {
    std::string current_match, room_queue;
    bool plugged, done;
    int my_seat;
    {
      std::lock_guard lock(mutex);
      current_match = match_id;
      room_queue = queue;
      plugged = opponent_plugged;
      done = set_done;
      my_seat = seat;
    }
    if (room_queue == "private" || !current_match.empty() || (room_queue == "ranked" && done))
      return;
    if (begin_sent && Clock::now() - *begin_sent < BEGIN_ANSWER)
      return;
    if (begin_wait_roster || begin_tries >= BEGIN_TRIES || joining || host != me || my_seat != 0 ||
        lobby_participants.size() != 2 || someone_away || !plugged)
    {
      return;
    }
    begin_sent = Clock::now();
    ++begin_tries;
    NOTICE_LOG_FMT(NETPLAY, "Orca room: beginning the room's {} match (revision {}, try {})",
                   room_queue, revision, begin_tries);
    SendRaw(fmt::format("{{\"t\":\"begin\",\"revision\":{}}}", revision));
  }

  // A game's report to the room's match, identical from both Orcas.
  void SendReport(const GameReport& report, const std::string& match, const std::string& room_queue)
  {
    const bool ranked = room_queue == "ranked";
    const int n = ranked ? ++ranked_sent : ++casual_sent;
    // A ranked game from the match block is named by its fight's first frame, the same on both
    // machines whatever either missed.
    const std::string id = ranked && report.start >= 0 ? fmt::format("g{}", report.start) :
                                                         fmt::format("{}{}", ranked ? "g" : "c", n);
    if (ranked)
    {
      game_numbers[id] = report.number > 0 ? report.number : n;
      if (report.start >= 0)
        block_ids.insert(id);
      if (report.set_done)
      {
        SetEnd end;
        end.done = report.set_done;
        if (report.set_done == 1)
          end.winner = ParticipantAt(report.set_winner_seat);
        set_ends[id] = end;
      }
    }
    std::string winner;
    GameReport r = report;
    if (r.kind == GameReport::Kind::Win)
    {
      winner = ParticipantAt(r.winner_seat);
      if (winner.empty())
      {
        r.kind = GameReport::Kind::Void;
        r.why = "ports";
      }
    }
    std::string outcome;
    switch (r.kind)
    {
    case GameReport::Kind::Win:
      outcome = fmt::format("\"winner\":{},\"detail\":{}", JsonString(winner), r.detail);
      break;
    case GameReport::Kind::Draw:
      outcome = fmt::format("\"draw\":true,\"detail\":{}", r.detail);
      break;
    case GameReport::Kind::Void:
      outcome = "\"void\":true";
      void_why[id] = r.why;
      break;
    }
    if (r.kind != GameReport::Kind::Void)
      sent_detail[id] = r.detail;
    const std::string text =
        ranked ? fmt::format("{{\"t\":\"game-report\",\"matchId\":{},\"id\":\"{}\",{}}}",
                             JsonString(match), id, outcome) :
                 fmt::format("{{\"t\":\"finish\",\"matchId\":{},{}}}", JsonString(match), outcome);
    NOTICE_LOG_FMT(NETPLAY, "Orca room: report {}", text);
    SendRaw(text);
  }

  // A ranked game began: the room's match hears it once (GameStartMessage), so a leave from here on
  // loses the set. A refusal comes back as a command-error, which changes nothing.
  void SendGameStart(int start, const std::string& match, const std::string& room_queue,
                     bool decided)
  {
    const std::string text = GameStartMessage(room_queue, match, decided, start);
    if (text.empty() || !starts_sent.insert(start).second)
      return;
    NoteKnownGames();
    NOTICE_LOG_FMT(NETPLAY, "Orca room: {}", text);
    SendRaw(text);
  }

  // KnownSetGames for the room's match: what this room saw, or what this process saw of the same
  // match before its link dropped (RememberKnownSetGames), whichever is more.
  void NoteKnownGames()
  {
    const int known = std::max(tally.Games(), static_cast<int>(starts_sent.size()));
    std::lock_guard lock(mutex);
    known_games = RememberKnownSetGames(match_id, known);
  }

  // The set's (or a casual game's) end, once.
  void SendFinish(const std::string& match, const std::string& winner, const std::string& why)
  {
    finish_sent = true;
    if (!why.empty())
      void_why["$final"] = why;
    const std::string text = FinishMessage(match, winner);
    NOTICE_LOG_FMT(NETPLAY, "Orca room: {}", text);
    last_finish = text;
    finish_retry_at.reset();
    SendRaw(text);
  }

  // Reports wait for the room's match; a desync voids the match in progress.
  void FlushLobby()
  {
    std::string current_match, room_queue;
    bool desync, fault, done, stall;
    std::vector<HeldReport> ready;
    {
      std::lock_guard lock(mutex);
      current_match = match_id;
      room_queue = queue;
      done = set_done;
      desync = std::exchange(desync_pending, false);
      fault = std::exchange(fault_pending, false);
      stall = std::exchange(stall_pending, false);
      const auto now = Clock::now();
      while (!reports.empty())
      {
        if (room_queue == "private")
        {
          reports.pop_front();
          continue;
        }
        if (current_match.empty())
        {
          if (now - reports.front().at < REPORT_HOLD)
            break;
          WARN_LOG_FMT(NETPLAY, "Orca room: a game's {} found no room match in {} s: dropped",
                       reports.front().game_start ? "start" : "report", REPORT_HOLD.count());
          reports.pop_front();
          continue;
        }
        ready.push_back(std::move(reports.front()));
        reports.pop_front();
      }
    }
    if (room_queue == "private" || current_match.empty())
      return;
    if (desync && !finish_sent && !done)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca room: the games went apart: the room's match is void");
      SendFinish(current_match, "", "desync");
      return;
    }
    // The session ended for the opponent's doing (or the verdict never came): this player's own
    // word goes in first, so its leave or silence can't settle the match for the opponent.
    if (fault && !finish_sent && !done)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca room: the session ended for the opponent's doing: the room's "
                              "match is void");
      SendFinish(current_match, "", "fault");
      return;
    }
    // The opponent's inputs stopped mid ranked set: this player names itself the winner. The room
    // never awards the set to the player who stalled, and awards it here if they also left the
    // room.
    if (stall && room_queue == "ranked" && !finish_sent && !done)
    {
      if (const std::string mine = MyId(); !mine.empty())
      {
        NOTICE_LOG_FMT(NETPLAY, "Orca room: the opponent stalled: claiming the set");
        SendFinish(current_match, mine, "");
        return;
      }
    }
    // A ranked set already decided takes no more games.
    if (room_queue == "ranked" && (finish_sent || done))
      return;
    for (const HeldReport& held : ready)
    {
      if (held.game_start)
        SendGameStart(held.report.start, current_match, room_queue, finish_sent || done);
      else
        SendReport(held.report, current_match, room_queue);
    }
  }

  // The local player's view of a detail the room echoed (port order): their own port first.
  std::string LocalDetail(const picojson::object& event, const std::string& id) const
  {
    std::vector<int> c, st;
    int stage = -1, to = 0;
    bool have = false;
    const auto from = [&](const picojson::object& d) {
      const auto ints = [&](const char* key, std::vector<int>* out) {
        const auto it = d.find(key);
        if (it == d.end() || !it->second.is<picojson::array>())
          return false;
        for (const auto& v : it->second.get<picojson::array>())
        {
          if (!v.is<double>() || out->size() >= 2)
            return false;
          out->push_back(static_cast<int>(v.get<double>()));
        }
        return out->size() == 2;
      };
      const auto s = Int(d, "st", -1, 1 << 30), t = Int(d, "to", 0, 1);
      if (!s || !t || !ints("c", &c) || !ints("s", &st))
        return false;
      stage = *s;
      to = *t;
      return true;
    };
    if (const auto it = event.find("detail");
        it != event.end() && it->second.is<picojson::object>())
      have = from(it->second.get<picojson::object>());
    if (!have)
    {
      c.clear();
      st.clear();
      picojson::value sent;
      if (const auto it = sent_detail.find(id);
          it != sent_detail.end() && ParseJson(it->second, &sent) && sent.is<picojson::object>())
      {
        have = from(sent.get<picojson::object>());
      }
    }
    if (!have)
      return "";
    if (MySlot() == 1)
    {
      std::swap(c[0], c[1]);
      std::swap(st[0], st[1]);
    }
    return fmt::format("{{\"st\":{},\"c\":[{},{}],\"s\":[{},{}],\"to\":{}}}", stage, c[0], c[1],
                       st[0], st[1], to);
  }

  // A verdict's "out", as the screen shows it (UX/SetEnd.h).
  static Orca::UX::SetEnd::Outcome OutcomeOf(const std::string& out)
  {
    using Orca::UX::SetEnd::Outcome;
    return out == "won"  ? Outcome::Won :
           out == "lost" ? Outcome::Lost :
           out == "draw" ? Outcome::Draw :
                           Outcome::Void;
  }

  // How a verdict came out for this player: won, lost, draw or void (and why it doesn't count).
  struct Verdict
  {
    std::string out, why;
    std::string winner;
  };
  Verdict ReadVerdict(const picojson::object& event, const std::string& id) const
  {
    Verdict v;
    const bool is_void = event.count("void") && event.at("void").evaluate_as_boolean();
    const bool draw = event.count("draw") && event.at("draw").evaluate_as_boolean();
    if (const auto it = event.find("ranking");
        it != event.end() && it->second.is<picojson::array>() &&
        !it->second.get<picojson::array>().empty() &&
        it->second.get<picojson::array>()[0].is<std::string>())
    {
      v.winner = it->second.get<picojson::array>()[0].get<std::string>();
    }
    if (is_void)
    {
      v.out = "void";
      v.winner.clear();
      if (const auto it = void_why.find(id); it != void_why.end())
        v.why = it->second;
      else if (Str(event, "reason").find("could not be recorded") != std::string::npos)
        v.why = "unrecorded";
      else
        v.why = id == "$final" ? "unrecorded" : "mismatch";
      return v;
    }
    if (draw)
    {
      v.out = "draw";
      v.winner.clear();
      return v;
    }
    v.out = !v.winner.empty() && v.winner == MyId() ? "won" : "lost";
    return v;
  }

  std::string Score() const
  {
    return fmt::format("[{},{}]", tally.Wins(MyId()), tally.Wins(TheirId()));
  }

  // "game-result": a ranked game's verdict.
  void OnGameResult(const picojson::object& m)
  {
    std::string current_match;
    {
      std::lock_guard lock(mutex);
      current_match = match_id;
    }
    const std::string id = Str(m, "id"), match = Str(m, "matchId");
    if (current_match.empty() || match != current_match || id.size() > 64 ||
        !verdicts_seen.insert(match + "/" + id).second)
    {
      return;
    }
    const Verdict v = ReadVerdict(m, id);
    std::optional<SetTally::End> end = tally.Game(v.winner);
    NoteKnownGames();
    int n = 0;
    if (const auto it = game_numbers.find(id); it != game_numbers.end())
      n = it->second;
    else if (id.size() > 1 && id[0] == 'g')
      n = std::atoi(id.c_str() + 1);
    // A game from the match block: the block ends the set (both games decided it the same way),
    // whatever the room's tally of agreed games says.
    if (block_ids.count(id))
    {
      end.reset();
      if (const auto it = set_ends.find(id); it != set_ends.end())
      {
        end = SetTally::End{it->second.done == 1 ? it->second.winner : "",
                            it->second.done == 1 && !it->second.winner.empty() ? "" : "limit"};
      }
    }
    const std::string d = LocalDetail(m, id);
    Orca::Status::Result(
        fmt::format("{{\"q\":\"ranked\",\"k\":\"game\",\"n\":{},\"out\":\"{}\"{},\"score\":{}{}}}",
                    n, v.out, v.why.empty() ? "" : fmt::format(",\"why\":\"{}\"", v.why), Score(),
                    d.empty() ? "" : ",\"d\":" + d));
    const std::string score = Score();
    const int mine = tally.Wins(MyId()), theirs = tally.Wins(TheirId());
    // No "Game N": the set's dots show which game it was.
    Orca::UX::ShowToast(Orca::UX::RankedSet::ResultToast(v.out, mine, theirs));
    NOTICE_LOG_FMT(NETPLAY, "Orca room: game {} {} for this player, set {}", id, v.out, score);
    if (end && !finish_sent)
      SendFinish(current_match, end->winner, end->why);
  }

  // "result": a ranked set's verdict, or a casual game's.
  void OnResult(const picojson::object& m)
  {
    std::string room_queue;
    {
      std::lock_guard lock(mutex);
      room_queue = queue;
    }
    const std::string match = Str(m, "matchId");
    if (room_queue == "private" || match.empty() || !verdicts_seen.insert(match + "/$final").second)
      return;
    if (room_queue == "ranked")
    {
      const Verdict v = ReadVerdict(m, "$final");
      const bool forfeit = Str(m, "disposition") == "forfeit";
      std::string extra;
      const std::string my_id = MyId();
      int rating_before = -1, rating_after = -1;
      if (const auto r = m.find("ratings"); r != m.end() && r->second.is<picojson::object>() &&
                                            r->second.get<picojson::object>().count(my_id))
      {
        const picojson::value& mine = r->second.get<picojson::object>().at(my_id);
        if (mine.is<picojson::object>())
        {
          const auto& o = mine.get<picojson::object>();
          const auto before = ClampedInt(o, "before", 0, 1 << 20);
          const auto after = ClampedInt(o, "after", 0, 1 << 20);
          rating_before = before.value_or(-1);
          rating_after = after.value_or(-1);
          if (before && after)
            extra += fmt::format(",\"r\":[{},{}]", *before, *after);
          if (const auto rank = o.find("rank");
              rank != o.end() && rank->second.is<picojson::object>())
          {
            const std::string label =
                CleanLabel(Str(rank->second.get<picojson::object>(), "label"), 24);
            if (!label.empty())
              extra += fmt::format(",\"rank\":\"{}\"", label);
          }
        }
      }
      Orca::Status::Result(fmt::format(
          "{{\"q\":\"ranked\",\"k\":\"set\",\"out\":\"{}\",\"by\":\"{}\"{},\"score\":{}{}}}", v.out,
          forfeit ? "forfeit" : "games", v.why.empty() ? "" : fmt::format(",\"why\":\"{}\"", v.why),
          Score(), extra));
      const int mine = tally.Wins(my_id), theirs = tally.Wins(TheirId());
      // On screen: VICTORY or DEFEAT with the rating change, or "<name> forfeited" first
      // (UX/SetEnd.h).
      Orca::UX::SetEnd::Current().SetVerdict(
          OutcomeOf(v.out), forfeit, mine, theirs, rating_before, rating_after,
          Orca::UX::Queue::OwnRating(), Orca::UX::SetEnd::NowMs());
      NOTICE_LOG_FMT(NETPLAY, "Orca room: the set: {} {} ({})", v.out, Score(),
                     Str(m, "disposition"));
      std::lock_guard lock(mutex);
      set_done = true;
      match_id.clear();
      return;
    }
    // Casual: each game is its own room match.
    const int n = ++casual_results;
    const std::string id = fmt::format("c{}", n);
    Verdict v = ReadVerdict(m, id);
    // A desync voided this game's match (SendFinish's "$final").
    if (const auto it = void_why.find("$final"); it != void_why.end())
    {
      if (v.out == "void")
        v.why = it->second;
      void_why.erase(it);
    }
    const std::string d = LocalDetail(m, id);
    Orca::Status::Result(
        fmt::format("{{\"q\":\"casual\",\"k\":\"game\",\"n\":{},\"out\":\"{}\"{}{}}}", n, v.out,
                    v.why.empty() ? "" : fmt::format(",\"why\":\"{}\"", v.why),
                    d.empty() ? "" : ",\"d\":" + d));
    // On screen: YOU WON or YOU LOST with the sitting's score (UX/SetEnd.h).
    if (v.out == "won")
      ++casual_mine;
    else if (v.out == "lost")
      ++casual_theirs;
    Orca::UX::SetEnd::Current().CasualGame(OutcomeOf(v.out), casual_mine, casual_theirs,
                                           Orca::UX::SetEnd::NowMs());
    NOTICE_LOG_FMT(NETPLAY, "Orca room: casual game {} {} for this player", n, v.out);
    // The room stays: the host begins the next match (TryBegin).
    finish_sent = false;
    std::lock_guard lock(mutex);
    match_id.clear();
  }

  // "start": the room's match began.
  void OnStart(const picojson::object& m)
  {
    last_finish.clear();
    finish_retry_at.reset();
    finish_retries = 0;
    begin_sent.reset();
    begin_tries = 0;
    ranked_sent = 0;
    tally.Reset();
    finish_sent = false;
    game_numbers.clear();
    set_ends.clear();
    block_ids.clear();
    starts_sent.clear();
    match_participants.clear();
    LobbyFields(m);
    // After LobbyFields: the new match's id, so its count starts from what this process knew of it.
    NoteKnownGames();
    match_participants = lobby_participants;
    // On screen (UX/SetEnd.h): a queue room's set or casual game against this opponent.
    std::string room_queue, opponent;
    {
      std::lock_guard lock(mutex);
      room_queue = queue;
      for (const auto& p : match_participants)
      {
        if (p.connection == me)
          continue;
        if (const auto name = seat_names.find(p.slot); name != seat_names.end())
          opponent = name->second;
      }
    }
    if (room_queue != "private")
    {
      Orca::UX::SetEnd::Current().MatchBegan(room_queue == "ranked", opponent,
                                             Orca::UX::SetEnd::NowMs());
    }
  }

  void Tick()
  {
    const auto now = Clock::now();
    // A joiner in a room without its friend's game (a stale invite, or a host that left, restarted
    // or arrived second) would wait forever for a keyframe, so it gives up soon after its welcome
    // unless the host has said hello. After that, the host's leave covers it.
    if (joining && !host_seen && welcomed_at && now - *welcomed_at >= HOST_WAIT)
    {
      End("Your friend isn't in that game any more", "peer_left");
      return;
    }
    if (now - last_ping >= PING_EVERY)
    {
      last_ping = now;
      picojson::object ping;
      ping["t"] = picojson::value("ping");
      ping["at"] = picojson::value(static_cast<double>(NowMs()));
      {
        std::lock_guard lock(mutex);
        if (rtt >= 0)
          ping["rtt"] = picojson::value(static_cast<double>(rtt));
      }
      SendJson(ping);
    }

    // Players the host unplugged hear why first.
    std::vector<std::pair<int, std::string>> drops;
    {
      std::lock_guard lock(mutex);
      drops.swap(pending_drops);
    }
    std::optional<std::pair<std::vector<KeyframeInfo::Name>, int>> names;
    {
      std::lock_guard lock(mutex);
      names.swap(pending_names);
    }
    if (names)
    {
      picojson::object d;
      d["k"] = picojson::value("nm");
      d["nm"] = NamesToJson(names->first);
      d["v"] = picojson::value(static_cast<double>(names->second));
      for (const auto& [id, peer] : peers)
      {
        if (peer.heard)
          SendTo(id, d);
      }
    }
    for (const auto& [slot, reason] : drops)
    {
      for (const auto& [id, peer] : peers)
      {
        if (peer.slot != slot)
          continue;
        picojson::object d;
        d["k"] = picojson::value("drop");
        d["r"] = picojson::value(reason);
        SendTo(id, d);
      }
    }
    // A held join (HoldJoins): every joiner without a keyframe hears it on change and every second
    // while held (for late arrivals and reconnects).
    {
      bool hold_now, hold_changed;
      {
        std::lock_guard lock(mutex);
        hold_now = holding;
        hold_changed = !std::exchange(hold_told, true);
      }
      if (!joining && (hold_changed || (hold_now && now - last_hello >= HELLO_EVERY)))
      {
        picojson::object d;
        d["k"] = picojson::value("hold");
        d["on"] = picojson::value(hold_now);
        for (const auto& [id, peer] : peers)
        {
          if (peer.heard && !peer.drop_in_host && !peer.sent_packet)
            SendTo(id, d);
        }
      }
    }

    // Say hello to every other Orca until it answers, so neither side treats the other as a player
    // before both are up and agree on the build. Then the host offers each newcomer its keyframe
    // every second until the newcomer's first packet arrives.
    if (now - last_hello >= HELLO_EVERY || offer_now.exchange(false))
    {
      last_hello = now;
      std::map<int, KeyframeInfo> offers;
      {
        std::lock_guard lock(mutex);
        offers = keyframe_offers;
      }
      for (auto& [id, peer] : peers)
      {
        if (!peer.heard || !peer.heard_us)
        {
          picojson::object d;
          d["k"] = picojson::value("hello");
          d["c"] = picojson::value(options.compatibility);
          d["r"] = picojson::value(peer.heard);
          d["h"] = picojson::value(!joining);
          if (direct)
            d["dl"] = picojson::value(1.0);
          // A joiner sends the host its own controls, for its port in the host's game.
          if (joining && !options.controls.empty() && options.controls.size() <= MAX_CONTROLS)
            d["ct"] = picojson::value(ToHex(options.controls.data(), options.controls.size()));
          // And its queue identity (rating, pick), for the same port.
          if (joining && !options.queue.empty() && options.queue.size() <= MAX_QUEUE)
            d["qi"] = picojson::value(ToHex(options.queue.data(), options.queue.size()));
          SendTo(id, d);
        }
        if (const auto offer = offers.find(peer.slot);
            offer != offers.end() && peer.heard && !peer.sent_packet)
        {
          // With a small replay inline (`b`), the offer is all a joiner needs
          // (KeyframeOfferToJson).
          SendTo(id, KeyframeOfferToJson(offer->second));
        }
      }
    }

    LinkPeers();

    TryBegin();
    FlushLobby();
    KeepAliveAndRetry(now);

    if (now - last_send < SEND_GAP)
      return;
    std::optional<Packet> packet;
    {
      std::lock_guard lock(mutex);
      packet.swap(outgoing);
    }
    if (!packet)
      return;
    last_send = now;
    // One message to the whole room (the Worker relays it to everyone else): every player gets the
    // same inputs, and the room's message limit counts it once.
    picojson::object message;
    message["t"] = picojson::value("msg");
    message["d"] = picojson::value(PacketToJson(*packet));
    SendJson(message);
  }

  // While the game waits for a ranked verdict without a session: a sign of life the room counts
  // (anything but a ping), and a void it refused while the match's games were being decided.
  void KeepAliveAndRetry(Clock::time_point now)
  {
    bool live;
    std::optional<Clock::time_point> wanted;
    {
      std::lock_guard lock(mutex);
      live = queue == "ranked" && !match_id.empty() && !set_done && !set_over;
      wanted = alive_wanted;
    }
    if (!live)
    {
      finish_retry_at.reset();
      return;
    }
    if (finish_retry_at && now >= *finish_retry_at && !last_finish.empty())
    {
      finish_retry_at.reset();
      NOTICE_LOG_FMT(NETPLAY, "Orca room: {} (again)", last_finish);
      SendRaw(last_finish);
    }
    if (KeepAliveDue(now, wanted, alive_sent))
    {
      alive_sent = now;
      picojson::object alive;
      alive["t"] = picojson::value("alive");
      SendJson(alive);
    }
  }

  void SendTo(const std::string& to, const picojson::object& d)
  {
    picojson::object message;
    message["t"] = picojson::value("msg");
    message["to"] = picojson::value(to);
    message["d"] = picojson::value(d);
    SendJson(message);
  }

  // A player left: drop its unread packets too.
  void ForgetPackets(const std::string& id)
  {
    std::lock_guard lock(mutex);
    for (size_t i = inbox.size(); i-- > 0;)
    {
      if (inbox_from[i] == id)
      {
        inbox.erase(inbox.begin() + static_cast<std::ptrdiff_t>(i));
        inbox_from.erase(inbox_from.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }

  void Emit(PeerEvent event)
  {
    std::lock_guard lock(mutex);
    if (events.size() < 256)
      events.push_back(std::move(event));
  }

  // A connection out of the game while the room still lists it (away, or replaced by a new socket):
  // a game player is unplugged, its keyframe offer and unread packets go, and anything it sends is
  // a stranger's until it says hello again.
  void ResetPeer(std::map<std::string, Peer>::iterator p)
  {
    if (p->second.announced)
    {
      Emit({PeerEvent::Kind::Left,
            p->second.slot,
            p->second.drop_in_host,
            p->second.name,
            {},
            {},
            p->second.bye});
    }
    {
      std::lock_guard lock(mutex);
      keyframe_offers.erase(p->second.slot);
    }
    ForgetPackets(p->first);
    UnlinkPeer(p->first, p->second);
    p->second = {};
    p->second.slot = -1;
  }

  // The lobby roster rides on welcome, join, leave, back, away and lobby messages.
  // Whether connection `id` plays in the room's match (its `start` named it).
  bool InMatch(const std::string& id) const
  {
    return std::ranges::any_of(match_participants,
                               [&](const LobbyParticipant& p) { return p.connection == id; });
  }

  // A game player left the room: mid ranked set the opponent's forfeit counts down on screen; in
  // casual, the page searches again (UX/SetEnd.h).
  void OpponentGone(const std::string& id)
  {
    std::string room_queue;
    bool live = false, over = false;
    {
      std::lock_guard lock(mutex);
      room_queue = queue;
      live = !match_id.empty() && !set_done;
      over = set_over;
    }
    // After the set, a leave is just "<name> left", never a forfeit.
    if (room_queue == "ranked" && over && InMatch(id))
      Orca::UX::SetEnd::Current().OpponentLeftAfterSet(Orca::UX::SetEnd::NowMs());
    else if (room_queue == "ranked" && live && InMatch(id))
      Orca::UX::SetEnd::Current().OpponentLeft(Orca::UX::SetEnd::NowMs());
    else if (room_queue == "casual")
      Orca::UX::SetEnd::Current().CasualOpponentLeft(Orca::UX::SetEnd::NowMs());
  }

  void Roster(const picojson::object& m)
  {
    LobbyFields(m);
    if (m.count("host"))
      host = Str(m, "host");
    if (const auto players = m.find("players");
        players != m.end() && players->second.is<picojson::array>())
    {
      room_players = players->second.get<picojson::array>();
    }
    const auto it = m.find("participants");
    if (it == m.end() || !it->second.is<picojson::array>())
    {
      Publish();
      return;
    }
    room_participants = it->second.get<picojson::array>();
    if (const auto rev = Int(m, "revision", 0, MAX_FRAME))
      revision = *rev;
    std::map<std::string, std::pair<int, std::string>> present;
    std::map<int, std::string> names;
    for (const auto& p : room_participants)
    {
      if (!p.is<picojson::object>())
        continue;
      const auto& o = p.get<picojson::object>();
      const std::string id = Str(o, "connectionId");
      const auto slot = Int(o, "slot", 0, MAX_SEATS - 1);
      if (id.empty() || !slot)
        continue;
      names[*slot] = CleanName(Str(o, "name"));
      if (id == me)
      {
        std::lock_guard lock(mutex);
        seat = *slot;
        continue;
      }
      present[id] = {*slot, Str(o, "name")};
    }
    int my_seat;
    {
      std::lock_guard lock(mutex);
      seat_names = std::move(names);
      my_seat = seat;
    }
    // A hosting Orca must hold the room's first seat and be the room's host: its port 1 is seat 0,
    // and joiners trust only the room's host. If a friend got here first (an invite sent before
    // this Orca reached its own room), the room can't work for either, so it ends and the game
    // opens a fresh one. "taken" never reaches the app.
    if (!joining && !me.empty() && (my_seat != 0 || host != me))
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: this game's host got seat {} of a room hosted by {}",
                   my_seat + 1, host == me ? "it" : "someone else");
      End("Someone else was already hosting this room, so your game opened a new one", "taken");
      return;
    }
    // Who left: the host unplugs a game player; the host leaving ends a joiner's session.
    for (auto p = peers.begin(); p != peers.end();)
    {
      if (present.contains(p->first))
      {
        ++p;
        continue;
      }
      if (p->second.announced)
      {
        Emit({PeerEvent::Kind::Left,
              p->second.slot,
              p->second.drop_in_host,
              p->second.name,
              {},
              {},
              p->second.bye});
        OpponentGone(p->first);
      }
      {
        // An offer was for that player only: the next one in the seat gets its own keyframe.
        std::lock_guard lock(mutex);
        keyframe_offers.erase(p->second.slot);
      }
      ForgetPackets(p->first);
      UnlinkPeer(p->first, p->second);
      p = peers.erase(p);
    }
    for (const auto& [id, info] : present)
    {
      auto [p, inserted] = peers.try_emplace(id);
      // A link speaks for one seat: a different seat needs a different link.
      if (p->second.linked && p->second.slot != info.first)
        UnlinkPeer(p->first, p->second);
      p->second.slot = info.first;
      p->second.name = info.second;
      if (inserted)
        last_hello = {};
    }
    std::string room;
    {
      std::lock_guard lock(mutex);
      room = code;
    }
    if (peers.empty())
      SetStatus(fmt::format("Room {}: waiting for your friend", room));
    Publish();
  }

  void OnMessage(const std::string& text)
  {
    picojson::value value;
    if (!ParseJson(text, &value) || !value.is<picojson::object>())
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: ignored an unreadable message ({} bytes)", text.size());
      return;
    }
    const auto& m = value.get<picojson::object>();
    const std::string t = Str(m, "t");

    if (t == "welcome")
    {
      me = Str(m, "me");
      if (Str(m, "lifecycle") != "game")
      {
        End("This room is not an Orca lobby", "room_mismatch");
        return;
      }
      // A room whose game is running is what a drop-in friend joins: no refusal.
      welcomed_at = Clock::now();
      if (const auto r = m.find("room"); r != m.end() && r->second.is<picojson::object>())
      {
        const auto& room = r->second.get<picojson::object>();
        const std::string q = Str(room, "queue");
        std::lock_guard lock(mutex);
        queue = q == "casual" || q == "ranked" ? q : "private";
        stay = room.count("stay") && room.at("stay").evaluate_as_boolean();
        room_size = Int(room, "size", 2, MAX_SEATS).value_or(MAX_SEATS);
        if (queue != "private")
        {
          NOTICE_LOG_FMT(NETPLAY, "Orca room: a matchmade {} room{}", queue,
                         stay ? " (the players stay together)" : "");
        }
      }
      LinkPeers();
      Roster(m);
      std::lock_guard lock(mutex);
      NOTICE_LOG_FMT(NETPLAY, "Orca room: in room {} on seat {}, {}", code, seat + 1,
                     joining ? "joining" : "hosting");
      if (state == RoomState::Connecting)
        state = RoomState::Waiting;
    }
    else if (t == "join" || t == "back" || t == "leave" || t == "away" || t == "lobby")
    {
      // A dropped connection ("away") is out of the game at once and its controller unplugs. So is
      // the connection a player comes back on ("back", or "join" for a listed id): the room
      // replaced a dead socket, and behind the new one is another Orca run that greets anew and
      // needs its own keyframe.
      if (t == "away" || t == "back" || t == "join")
      {
        if (const auto player = m.find("player");
            player != m.end() && player->second.is<picojson::object>())
        {
          const std::string id = Str(player->second.get<picojson::object>(), "id");
          if (const auto p = peers.find(id); p != peers.end())
          {
            ResetPeer(p);
            if (t != "away")
              last_hello = {};  // greet it at once
          }
          // The opponent of a set whose forfeit was counting down is back.
          if (t != "away" && InMatch(id))
            Orca::UX::SetEnd::Current().OpponentBack(Orca::UX::SetEnd::NowMs());
        }
      }
      Roster(m);
    }
    else if (t == "msg")
    {
      const auto p = peers.find(Str(m, "from"));
      if (p == peers.end())
        return;
      const auto d = m.find("d");
      if (d == m.end() || !d->second.is<picojson::object>())
        return;
      OnPeerMessage(p->first, p->second, d->second.get<picojson::object>());
    }
    else if (t == "pong")
    {
      if (const auto at = m.find("at"); at != m.end() && at->second.is<double>())
      {
        const double sent = at->second.get<double>();
        const double now = static_cast<double>(NowMs());
        if (sent <= now && now - sent < 60000)
        {
          std::lock_guard lock(mutex);
          rtt = static_cast<int>(now - sent);
          ++rtt_sequence;
          recent_rtts.push_back(rtt);
          if (recent_rtts.size() > static_cast<size_t>(RTT_SAMPLES))
            recent_rtts.pop_front();
        }
      }
    }
    else if (t == "start")
    {
      OnStart(m);
      Roster(m);
    }
    else if (t == "game-result")
    {
      OnGameResult(m);
    }
    else if (t == "result")
    {
      OnResult(m);
    }
    else if (t == "set-over")
    {
      // A ranked set's room after its verdict stays open (relay, chat) until the players leave,
      // each when their results screen ends or they hold Z, or until `ms` passes and it closes with
      // 4004. Nothing more is rated or forfeited, and every lobby command is refused ("The set is
      // over").
      const int ms = Int(m, "ms", 0, 3600000).value_or(0);
      {
        std::lock_guard lock(mutex);
        set_over = true;
      }
      NOTICE_LOG_FMT(NETPLAY, "Orca room: set over: the room stays open until the players leave "
                              "({} s at most)",
                     ms / 1000);
      Orca::UX::SetEnd::Current().SetOver(Orca::UX::SetEnd::NowMs());
    }
    else if (t == "command-error")
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: {} refused: {}", Str(m, "action"), Str(m, "error"));
      // A void refused while the match's games were still being decided goes again shortly.
      if (Str(m, "action") == "finish" && last_finish.find("\"void\":true") != std::string::npos &&
          finish_retries < FINISH_RETRIES)
      {
        ++finish_retries;
        finish_retry_at = Clock::now() + KEEP_ALIVE_EVERY;
      }
      // The host's begin goes again once the roster changes (BEGIN_TRIES at most).
      if (Str(m, "action") == "begin")
      {
        begin_sent.reset();
        begin_wait_roster = true;
      }
    }
    else if (t == "error")
    {
      End(Str(m, "error").empty() ? "The room reported an error" : Str(m, "error"));
    }
    else if (t == "kicked")
    {
      End("You were removed from the room", "kicked");
    }
  }

  void OnPeerMessage(const std::string& id, Peer& peer, const picojson::object& d)
  {
    const std::string kind = Str(d, "k");
    if (kind == "bye")
    {
      // Only known reasons, so a peer can't inject text into this player's report. "mismatch" only
      // from the host this player is joining: its game never carried the queue room's header, so
      // the match can't start.
      if (const std::string reason = Str(d, "r");
          reason == "desync" || reason == "stalled" ||
          (reason == "mismatch" && peer.drop_in_host && joining))
      {
        peer.bye = reason;
      }
      return;
    }
    if (kind == "drop")
    {
      // The host unplugged this player: its report gives the host's reason.
      if (!peer.drop_in_host || !joining)
        return;
      const std::string reason = Str(d, "r");
      bool keep_room;
      {
        std::lock_guard lock(mutex);
        keep_room = DropKeepsRoom(queue, !match_id.empty() && !set_done, set_over);
        // In a ranked set still being decided this player's own void goes first, and it stays
        // for the verdict: the host leaving the session is all this is (HostGone).
        if (keep_room)
        {
          if (reason == "desync")
            desync_pending = true;
          else
            fault_pending = true;
        }
      }
      if (keep_room)
      {
        WARN_LOG_FMT(NETPLAY,
                     "Orca room: the host unplugged this player mid set ({}): the match "
                     "is void, and this game waits for the verdict",
                     reason);
        FlushLobby();
        Emit({PeerEvent::Kind::Left,
              peer.slot,
              true,
              peer.name,
              {},
              {},
              reason == "desync" ? std::string("desync") : std::string("dropped")});
        return;
      }
      // A matchmade room hears that the match in progress is void before this player goes.
      if (reason == "desync")
      {
        {
          std::lock_guard lock(mutex);
          desync_pending = true;
        }
        FlushLobby();
      }
      if (reason == "desync")
        End("Your games went out of sync, so you left your friend's game", "desync");
      else if (reason == "stalled")
        End("The connection with your friend stalled, so you left the game", "peer_left");
      else if (reason == "signed_out")
        End("Your friend needs to sign in to YouGame to share their game", "signed_out");
      else if (reason == "refused")
        End("YouGame couldn't pass your friend's game to you", "network");
      else if (reason == "room_full")
        End("Your friend's game already has another friend in it", "room_full");
      else
        End("Your friend's game disconnected you", "network");
      return;
    }
    if (kind == "hello")
    {
      // A legacy hello field "pf" (a float class) is ignored: every Mac and PC computes the same
      // game now, and a build that differs has another compatibility key.
      if (Str(d, "c") != options.compatibility)
      {
        // Not a player of this game; a joiner can't play it at all.
        WARN_LOG_FMT(NETPLAY, "Orca room: {} runs a different Orca build or setup", id);
        if (joining && id == host)
          End("Your friend is running a different Orca build or setup", "room_mismatch");
        return;
      }
      const bool was_heard = peer.heard;
      peer.heard = true;
      // It offers direct links (an Orca with ORCA_DIRECT=0 doesn't).
      peer.direct = d.count("dl") && d.at("dl").is<double>() && d.at("dl").get<double>() == 1;
      peer.heard_us = peer.heard_us || (d.count("r") && d.at("r").evaluate_as_boolean());
      // Only the game's host that is also the room's host (first in) counts, so no other player can
      // take over a joiner's game by claiming it.
      peer.drop_in_host = d.count("h") && d.at("h").evaluate_as_boolean() && id == host;
      // A joiner's own controls (the game's host sends none: its own are already in its port).
      if (!peer.drop_in_host && d.count("ct"))
        peer.controls = ControlsFromHex(d.at("ct"));
      if (!peer.drop_in_host && d.count("qi"))
        peer.queue = ControlsFromHex(d.at("qi"), MAX_QUEUE);
      host_seen = host_seen || peer.drop_in_host;
      if (!was_heard)
        last_hello = {};  // answer at once rather than on the next hello tick
      // A game player: a joiner at a seat other than the host's, or the host itself.
      if (!peer.announced && peer.slot >= 0)
      {
        peer.announced = true;
        std::string room;
        {
          std::lock_guard lock(mutex);
          room = code;
        }
        SetStatus(fmt::format("Room {}: {} is here", room, peer.name.empty() ? "a friend" : peer.name));
        Emit(
            {PeerEvent::Kind::Arrived, peer.slot, peer.drop_in_host, peer.name, peer.controls,
             {}, {}, peer.queue});
        // Refresh the page's lobby now, not at the next roster change.
        Publish();
      }
      return;
    }
    if (kind == "hold")
    {
      // Only from the host, to a joiner.
      if (!peer.drop_in_host || !joining)
        return;
      PeerEvent event{PeerEvent::Kind::Hold, peer.slot, true, peer.name, {}, {}};
      event.holding = d.count("on") && d.at("on").evaluate_as_boolean();
      Emit(std::move(event));
      return;
    }
    if (kind == "nm")
    {
      // Only from the host, to a player of its game.
      if (!peer.drop_in_host || !joining)
        return;
      KeyframeInfo info;
      info.names = NamesFromJson(d);
      if (const auto version = Int(d, "v", 0, MAX_FRAME))
        info.names_version = *version;
      Emit({PeerEvent::Kind::Names, peer.slot, true, peer.name, {}, info});
      return;
    }
    if (kind == "kf")
    {
      // Only from the host, to a joiner.
      if (!peer.drop_in_host || !joining)
        return;
      // The same offer comes every second until this player's first packet: once is enough.
      if (peer.keyframe && peer.keyframe->id == Str(d, "id"))
        return;
      // A malformed offer, or a replay inline that's too long or not what it says, is ignored.
      std::optional<KeyframeInfo> info = KeyframeOfferFromJson(d);
      if (!info)
      {
        WARN_LOG_FMT(NETPLAY, "Orca room: ignored a malformed keyframe offer from the host");
        return;
      }
      peer.keyframe = *info;
      Emit({PeerEvent::Kind::Keyframe, peer.slot, true, peer.name, {}, std::move(*info)});
      return;
    }
    if (kind == "dl" || kind == "dlc" || kind == "dlr")
    {
      // Direct-link signalling, only from a game player (its hello had our compatibility key) that
      // offered links, while links are on here. Its offer may arrive before this side linked it:
      // link it now.
      if (!direct_started || !direct_on.value_or(false) || !peer.announced || !peer.direct)
        return;
      if (!peer.linked)
      {
        direct->AddPeer(id, peer.slot);
        peer.linked = true;
      }
      direct->OnSignal(id, d);
      return;
    }
    if (kind != "p" || !peer.announced)
      return;
    Packet packet;
    if (!PacketFromJson(d, &packet) || packet.seat != peer.slot)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: a malformed packet from seat {}", peer.slot + 1);
      return;
    }
    peer.sent_packet = true;
    if (peer.linked)
      direct->OnRelayPacket(id);
    std::lock_guard lock(mutex);
    // Nobody read the inbox for a while (a long stall here). Each packet repeats every input the
    // sender hasn't seen acknowledged, so the newer half covers everything the older half did.
    if (inbox.size() >= MAX_INBOX)
    {
      WARN_LOG_FMT(NETPLAY, "Orca room: inbox full, dropping the {} oldest packets", MAX_INBOX / 2);
      inbox.erase(inbox.begin(), inbox.begin() + MAX_INBOX / 2);
      inbox_from.erase(inbox_from.begin(), inbox_from.begin() + MAX_INBOX / 2);
    }
    inbox.push_back(std::move(packet));
    inbox_from.push_back(id);
  }
};

YouGameRoom::YouGameRoom(RoomOptions options) : m_impl(std::make_unique<Impl>(std::move(options)))
{
}

YouGameRoom::~YouGameRoom() = default;

RoomState YouGameRoom::GetState() const
{
  return m_impl->State();
}

int YouGameRoom::Seat() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->seat;
}

std::optional<bool> YouGameRoom::Joining() const
{
  std::lock_guard lock(m_impl->mutex);
  if (!m_impl->role_known)
    return std::nullopt;
  return m_impl->joining;
}

std::vector<PeerEvent> YouGameRoom::TakePeerEvents()
{
  std::lock_guard lock(m_impl->mutex);
  return std::exchange(m_impl->events, {});
}

bool YouGameRoom::ArrivalPending() const
{
  std::lock_guard lock(m_impl->mutex);
  // As the host takes them (OnlineMatch's HostEvents): a friend on a seat after the host's.
  return std::ranges::any_of(m_impl->events, [](const PeerEvent& event) {
    return event.kind == PeerEvent::Kind::Arrived && event.seat > 0 && !event.host;
  });
}

std::vector<int> YouGameRoom::LeftSeatsPending() const
{
  std::lock_guard lock(m_impl->mutex);
  std::vector<int> seats;
  for (const PeerEvent& event : m_impl->events)
  {
    if (event.kind == PeerEvent::Kind::Left && !event.host && event.seat > 0)
      seats.push_back(event.seat);
  }
  return seats;
}

bool YouGameRoom::HostLeftPending() const
{
  std::lock_guard lock(m_impl->mutex);
  return std::ranges::any_of(m_impl->events, [](const PeerEvent& event) {
    return event.kind == PeerEvent::Kind::Left && event.host;
  });
}

bool YouGameRoom::FreshTicket(bool fresh, std::string* ticket, std::string* store_url,
                              std::string* error)
{
  return m_impl->FreshTicket(fresh, ticket, store_url, error);
}

void YouGameRoom::DropPeer(int seat, const std::string& reason)
{
  std::lock_guard lock(m_impl->mutex);
  if (m_impl->pending_drops.size() < 16)
    m_impl->pending_drops.emplace_back(seat, reason);
}

void YouGameRoom::HoldJoins(bool holding)
{
  std::lock_guard lock(m_impl->mutex);
  if (m_impl->holding == holding)
    return;
  m_impl->holding = holding;
  m_impl->hold_told = false;
}

void YouGameRoom::BroadcastNames(const std::vector<KeyframeInfo::Name>& names, int version)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->pending_names = std::pair(names, version);
  m_impl->offer_now = true;
}

void YouGameRoom::OfferKeyframe(int seat, const KeyframeInfo& info)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->keyframe_offers[seat] = info;
  // Sent on the I/O thread's next turn, not its next hello tick: a friend is waiting on it.
  m_impl->offer_now = true;
}

std::string YouGameRoom::SeatName(int seat) const
{
  std::lock_guard lock(m_impl->mutex);
  const auto it = m_impl->seat_names.find(seat);
  return it == m_impl->seat_names.end() ? std::string() : it->second;
}

std::vector<u8> YouGameRoom::HelloControls() const
{
  const auto& controls = m_impl->options.controls;
  return m_impl->options.joining && controls.size() <= MAX_CONTROLS ? controls : std::vector<u8>{};
}

std::vector<u8> YouGameRoom::HelloQueue() const
{
  const auto& queue = m_impl->options.queue;
  return m_impl->options.joining && queue.size() <= MAX_QUEUE ? queue : std::vector<u8>{};
}

std::string YouGameRoom::Code() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->code;
}

std::string YouGameRoom::Status() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->status;
}

void YouGameRoom::SetLeaveReason(std::string reason)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->leave_reason = std::move(reason);
}

std::string YouGameRoom::ErrorCode() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->state == RoomState::Ended ? m_impl->error_code : "";
}

int YouGameRoom::RoundTripMs(u32* sequence) const
{
  std::lock_guard lock(m_impl->mutex);
  if (sequence)
    *sequence = m_impl->rtt_sequence;
  return m_impl->rtt;
}

std::vector<int> YouGameRoom::RecentRoundTrips(u32* sequence) const
{
  std::lock_guard lock(m_impl->mutex);
  *sequence = m_impl->rtt_sequence;
  return {m_impl->recent_rtts.begin(), m_impl->recent_rtts.end()};
}

std::optional<SetTally::End> SetTally::Game(const std::string& winner)
{
  if (m_ended)
    return std::nullopt;
  ++m_games;
  if (!winner.empty() && ++m_wins[winner] >= SET_WINS)
  {
    m_ended = true;
    return End{winner, ""};
  }
  if (m_games >= SET_GAMES)
  {
    m_ended = true;
    return End{"", "limit"};
  }
  return std::nullopt;
}

int SetTally::Wins(const std::string& id) const
{
  const auto it = m_wins.find(id);
  return it == m_wins.end() ? 0 : it->second;
}

void SetTally::Reset()
{
  m_wins.clear();
  m_games = 0;
  m_ended = false;
}

void CoalescePacket(std::optional<Packet>* outgoing, const Packet& packet)
{
  std::optional<Packet>& pending = *outgoing;
  if (!pending)
  {
    pending = packet;
    return;
  }
  // The newer packet replaces one not yet sent. Keep what only the older one carried: inputs before
  // the newer one's first frame (both come from the same local history), and its checksum.
  Packet merged = packet;
  const int old_last = pending->first_frame + static_cast<int>(pending->pads.size()) - 1;
  if (pending->first_frame < merged.first_frame && old_last >= merged.first_frame - 1)
  {
    const int extra = std::min(merged.first_frame - pending->first_frame,
                               MAX_PADS_IN_PACKET - static_cast<int>(merged.pads.size()));
    if (extra > 0)
    {
      const auto from = pending->pads.begin() + (merged.first_frame - extra - pending->first_frame);
      merged.pads.insert(merged.pads.begin(), from, from + extra);
      merged.first_frame -= extra;
    }
  }
  if (pending->checksum_frame && !merged.checksum_frame)
  {
    merged.checksum_frame = pending->checksum_frame;
    merged.checksum = pending->checksum;
  }
  // Drop-in history: the older packet's goes first when the newer one continues it (or has none); a
  // gap is resent once the joiner's ack stalls.
  if (!pending->history.empty())
  {
    if (merged.history.empty())
    {
      merged.history_seat = pending->history_seat;
      merged.history_first = pending->history_first;
      merged.history = pending->history;
    }
    else if (merged.history_seat == pending->history_seat &&
             pending->history_first + static_cast<int>(pending->history.size()) ==
                 merged.history_first &&
             pending->history.size() + merged.history.size() <= MAX_HISTORY_FRAMES_IN_PACKET)
    {
      std::vector<Pads> joined = pending->history;
      joined.insert(joined.end(), merged.history.begin(), merged.history.end());
      merged.history = std::move(joined);
      merged.history_first = pending->history_first;
    }
  }
  merged.leaving = merged.leaving || pending->leaving;
  pending = std::move(merged);
}

std::string YouGameRoom::Queue() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->queue;
}

bool YouGameRoom::MatchLive() const
{
  std::lock_guard lock(m_impl->mutex);
  return !m_impl->match_id.empty() && !m_impl->set_done;
}

bool YouGameRoom::SetOver() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->set_over;
}

void YouGameRoom::SetOpponentPlugged(bool plugged)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->opponent_plugged = plugged;
}

void YouGameRoom::ReportGame(GameReport report)
{
  std::lock_guard lock(m_impl->mutex);
  if (m_impl->reports.size() < 16)
    m_impl->reports.push_back({std::move(report), Clock::now()});
}

void YouGameRoom::ReportGameStart(int start)
{
  GameReport report;
  report.start = start;
  std::lock_guard lock(m_impl->mutex);
  if (m_impl->reports.size() < 16)
    m_impl->reports.push_back({std::move(report), Clock::now(), true});
}

void YouGameRoom::ReportDesync()
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->desync_pending = true;
}

void YouGameRoom::ReportStall()
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->stall_pending = true;
}

int YouGameRoom::KnownSetGames() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->known_games;
}

void YouGameRoom::ReportFault()
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->fault_pending = true;
}

void YouGameRoom::KeepAlive()
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->alive_wanted = Clock::now();
}

void YouGameRoom::TestDropConnection()
{
  m_impl->test_drop = true;
}

void YouGameRoom::Send(const Packet& packet)
{
  {
    std::lock_guard lock(m_impl->mutex);
    CoalescePacket(&m_impl->outgoing, packet);
  }
  // Every packet also goes straight to each linked player; the first copy to arrive wins
  // (Session::ReceivePackets handles both).
  m_impl->SendDirect(packet);
}

std::vector<Packet> YouGameRoom::Receive()
{
  std::vector<Packet> packets;
  {
    std::lock_guard lock(m_impl->mutex);
    m_impl->inbox_from.clear();
    packets = std::exchange(m_impl->inbox, {});
  }
  if (!m_impl->direct)
    return packets;
  for (DirectLink::Received& received : m_impl->direct->Receive())
  {
    Packet packet;
    if (!DecodeDirectPacket(received.payload, received.slot, &packet))
    {
      if (!std::exchange(m_impl->direct_malformed_logged, true))
        WARN_LOG_FMT(NETPLAY, "Orca room: a malformed direct packet from seat {}", received.slot + 1);
      continue;
    }
    packets.push_back(std::move(packet));
  }
  return packets;
}

DirectSummary YouGameRoom::Direct() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->direct_summary;
}

void YouGameRoom::TestDirect(const std::string& command)
{
  if (m_impl->direct)
    m_impl->direct->Test(command);
}

bool YouGameRoom::Connected() const
{
  return GetState() == RoomState::Waiting;
}

void YouGameRoom::Leave()
{
  m_impl->stop = true;
  if (m_impl->thread.joinable())
    m_impl->thread.join();
  m_impl->End("You left the room", "");
}
}  // namespace Orca::Net
