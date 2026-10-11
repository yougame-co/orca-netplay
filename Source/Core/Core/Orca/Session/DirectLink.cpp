// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/DirectLink.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <juice/juice.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/SocketContext.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Sandbox.h"

namespace Orca::Net
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Datagram layout (numbers big-endian):
//   0      'o'
//   1      version << 4 | kind
//   2-3    the link's generation
//   4-11   the sender's counter (the nonce: never reused under one key)
//   12-15  low 32 bits of the newest counter received from the peer (0: none yet)
//   16-17  how long ago that one arrived, in 0.1 ms
//   18..   the payload, sealed with ChaCha20-Poly1305 (the header is associated data)
//   last   the 16-byte tag
// The echo measures the round trip on the inputs' own path without a shared clock, like Slippi's
// pad acknowledgement ping.
constexpr u8 MAGIC = 'o';
constexpr u8 VERSION = 1;
constexpr u8 KIND_PACKET = 1;
constexpr u8 KIND_KEEPALIVE = 2;
// DSCP EF (46) in the traffic-class byte, as Slippi marks its socket (IP_TOS 0xB8), so Wi-Fi puts
// it in the voice queue.
constexpr int TOS_EXPEDITED = 0xB8;

// A link is up while an authenticated datagram arrived this recently. Idle links send keepalives,
// so an up link never looks down between inputs.
constexpr auto UP_WINDOW = 500ms;
constexpr auto KEEPALIVE_EVERY = 200ms;
// Rebuild rules. libjuice never re-nominates once it picks a pair and only fails one after 30 s
// without consent, so Orca rebuilds when: an offer gets no answer in time; there is no path after
// both descriptions; no datagram arrives while the relay still brings the peer's packets (a changed
// interface, an expired NAT mapping); or, once, a relayed or reflexive pair connects two machines
// on one LAN (OneLan). Backoff between failure rebuilds: 2, 4, 8 ... 30 s, reset after 30 s up.
constexpr auto ANSWER_TIMEOUT = 5s;
constexpr auto CONNECT_TIMEOUT = 10s;
constexpr auto SILENT_LIMIT = 3s;
constexpr auto RELAY_FRESH = 1s;
constexpr auto UPGRADE_AFTER = 10s;
constexpr auto STABLE_AFTER = 30s;
constexpr int FIRST_BACKOFF_S = 2;
constexpr int MAX_BACKOFF_S = 30;
// TURN credentials last 30 minutes and tickets hand them out with at least 15 left; older than
// this, a new link asks for fresh ones first.
constexpr auto ICE_FRESH_FOR = 10min;
// After a failed mint of fresh credentials (a ticket request, up to 10 s on the link thread), wait
// at least this long before retrying, doubling up to the max. Links keep their current ones.
constexpr auto FRESH_ICE_RETRY_FIRST = 30s;
constexpr auto FRESH_ICE_RETRY_MAX = 5min;
// Token bucket on delivered packets. An honest peer sends one per frame, or one every 4 ms while
// stalled, well under this. Excess datagrams are dropped (the relay still carries them), so a
// modified peer holding the keys can't flood Receive on the emulator thread.
constexpr double FLOOD_RATE = 300;
constexpr double FLOOD_BURST = 64;
// How often at most to answer an offer refused for an old generation with this side's (`dlr`).
constexpr auto REFUSAL_ANSWER_GAP = 1s;
// Candidates go out in batches at most this often (the room allows 120 messages a second).
constexpr auto TRICKLE_EVERY = 100ms;
constexpr size_t MAX_CANDIDATES_IN_MESSAGE = 16;
constexpr size_t MAX_REMOTE_CANDIDATES = 32;
constexpr size_t MAX_CANDIDATE_LENGTH = 255;
constexpr size_t MAX_DESCRIPTION = 4096;
constexpr size_t MAX_INBOX = 1024;
constexpr size_t RTT_SAMPLES_KEPT = 16;
constexpr size_t SENT_RING = 64;

std::atomic<u64> s_link_serial{0};

// ---- Logging: libjuice's lines go to Dolphin's log, never stdout (the app parses Orca's stdout),
// with addresses and credentials removed, since logs get uploaded. ----

// With `labels`, each distinct address becomes "<addrN>" (stable within the process) instead of
// "<address>", so a verbose log tells paths apart without revealing them.
std::string Scrub(std::string_view message, std::map<std::string, int>* labels = nullptr)
{
  std::string out;
  for (size_t i = 0; i < message.size();)
  {
    const char c = message[i];
    if (c == '"')
    {
      const size_t end = message.find('"', i + 1);
      out += "\"...\"";
      i = end == std::string_view::npos ? message.size() : end + 1;
      continue;
    }
    if (message.substr(i).starts_with("a=candidate:") || message.substr(i).starts_with("candidate:"))
    {
      out += "<candidate>";
      break;
    }
    // A run of hex digits, dots and colons containing two dots or two colons: an address.
    size_t j = i;
    int dots = 0, colons = 0;
    while (j < message.size() && (std::isxdigit(static_cast<unsigned char>(message[j])) ||
                                  message[j] == '.' || message[j] == ':'))
    {
      dots += message[j] == '.';
      colons += message[j] == ':';
      ++j;
    }
    if (j > i && (dots >= 2 || colons >= 2))
    {
      if (labels)
      {
        const auto [it, added] = labels->try_emplace(std::string(message.substr(i, j - i)),
                                                     static_cast<int>(labels->size()) + 1);
        out += fmt::format("<addr{}>", it->second);
      }
      else
      {
        out += "<address>";
      }
      i = j;
      continue;
    }
    if (j > i)
    {
      out.append(message.substr(i, j - i));
      i = j;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

// Tests only, ORCA_DIRECT_ICE_LOG=1: every libjuice line (debug level, no rate cap), logged at
// notice level so timestamps show what the ICE agent sent when.
bool IceLogVerbose()
{
  static const bool verbose = GetEnv("ORCA_DIRECT_ICE_LOG") == "1";
  return verbose;
}

void JuiceLog(juice_log_level_t level, const char* message)
{
  if (IceLogVerbose())
  {
    // libjuice logs under its own lock, one thread at a time.
    static std::map<std::string, int> labels;
    NOTICE_LOG_FMT(NETPLAY, "Orca direct link (ICE {}): {}", static_cast<int>(level),
                   Scrub(message ? message : "", &labels));
    return;
  }
  // A dead path makes every send fail: at most 10 lines a second.
  static Clock::time_point second{};
  static int lines = 0, dropped = 0;
  const auto now = Clock::now();
  if (now - second >= 1s)
  {
    if (dropped > 0)
      WARN_LOG_FMT(NETPLAY, "Orca direct link (ICE): {} more lines in that second", dropped);
    second = now;
    lines = dropped = 0;
  }
  if (++lines > 10)
  {
    ++dropped;
    return;
  }
  const std::string text = Scrub(message ? message : "");
  if (level >= JUICE_LOG_LEVEL_ERROR)
    ERROR_LOG_FMT(NETPLAY, "Orca direct link (ICE): {}", text);
  else if (level == JUICE_LOG_LEVEL_WARN)
    WARN_LOG_FMT(NETPLAY, "Orca direct link (ICE): {}", text);
  else
    DEBUG_LOG_FMT(NETPLAY, "Orca direct link (ICE): {}", text);
}

void SetUpJuiceLogging()
{
  static std::once_flag once;
  std::call_once(once, [] {
    juice_set_log_handler(&JuiceLog);
    juice_set_log_level(IceLogVerbose() ? JUICE_LOG_LEVEL_DEBUG : JUICE_LOG_LEVEL_WARN);
  });
}

// ---- Small helpers ----

std::string Hex(const u8* data, size_t size)
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

bool FromHex(std::string_view hex, u8* out, size_t size)
{
  if (hex.size() != size * 2)
    return false;
  const auto digit = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  for (size_t i = 0; i < size; ++i)
  {
    const int high = digit(hex[2 * i]), low = digit(hex[2 * i + 1]);
    if (high < 0 || low < 0)
      return false;
    out[i] = static_cast<u8>(high << 4 | low);
  }
  return true;
}

void PutU16(u8* p, u16 v)
{
  p[0] = static_cast<u8>(v >> 8);
  p[1] = static_cast<u8>(v);
}
void PutU32(u8* p, u32 v)
{
  for (int i = 0; i < 4; ++i)
    p[i] = static_cast<u8>(v >> (24 - 8 * i));
}
void PutU64(u8* p, u64 v)
{
  for (int i = 0; i < 8; ++i)
    p[i] = static_cast<u8>(v >> (56 - 8 * i));
}
u16 GetU16(const u8* p)
{
  return static_cast<u16>(p[0] << 8 | p[1]);
}
u32 GetU32(const u8* p)
{
  u32 v = 0;
  for (int i = 0; i < 4; ++i)
    v = v << 8 | p[i];
  return v;
}
u64 GetU64(const u8* p)
{
  u64 v = 0;
  for (int i = 0; i < 8; ++i)
    v = v << 8 | p[i];
  return v;
}

std::string Str(const picojson::object& o, const std::string& key)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "";
}

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

// ---- Candidates: "a=candidate:<foundation> <component> <transport> <priority> <address> <port>
// typ <type> ..." ----

struct Candidate
{
  std::string address;
  std::string port;
  std::string type;
};

std::optional<Candidate> ParseCandidate(std::string_view line)
{
  if (line.starts_with("a="))
    line.remove_prefix(2);
  if (!line.starts_with("candidate:"))
    return std::nullopt;
  std::istringstream in{std::string(line)};
  std::string foundation, component, transport, priority, typ;
  Candidate c;
  if (!(in >> foundation >> component >> transport >> priority >> c.address >> c.port >> typ >>
        c.type) ||
      typ != "typ")
  {
    return std::nullopt;
  }
  return c;
}

// Which candidate types a forced test mode allows ("turn": relayed only; "srflx": no host
// candidates).
bool Allowed(const std::string& force, const std::string& type)
{
  if (force == "turn")
    return type == "relay";
  if (force == "srflx")
    return type != "host";
  return true;
}

// A description as libjuice writes it, keeping only ICE lines and the candidates `force` and the
// sandbox's network (`net`) allow. Host candidate addresses go to `hosts` and server-reflexive ones
// to `srflx`, for the one-LAN check; never logged.
std::string FilterDescription(std::string_view description, const std::string& force,
                              std::vector<std::string>* hosts, std::vector<std::string>* srflx,
                              Orca::Sandbox::NetMode net)
{
  std::string out;
  size_t start = 0;
  while (start < description.size())
  {
    size_t end = description.find('\n', start);
    if (end == std::string_view::npos)
      end = description.size();
    std::string_view line = description.substr(start, end - start);
    start = end + 1;
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    if (line.starts_with("a=candidate:"))
    {
      const auto c = ParseCandidate(line);
      if (!c || line.size() > MAX_CANDIDATE_LENGTH ||
          !Orca::Sandbox::AddressAllowed(net, c->address))
        continue;
      if (c->type == "host" && hosts && c->address != "127.0.0.1" && c->address != "::1")
        hosts->push_back(c->address);
      if (c->type == "srflx" && srflx)
        srflx->push_back(c->address);
      if (!Allowed(force, c->type))
        continue;
    }
    else if (!line.starts_with("a=ice-ufrag:") && !line.starts_with("a=ice-pwd:") &&
             !line.starts_with("a=ice-options:") && !line.starts_with("a=end-of-candidates"))
    {
      continue;
    }
    out.append(line);
    out += "\r\n";
  }
  return out;
}

// Two Orcas on one machine: loopback candidates for the host candidates' socket. libjuice leaves
// loopback out, and macOS Local Network privacy may refuse sends to the machine's own LAN address.
std::string LoopbackCandidates(std::string_view description)
{
  size_t start = 0;
  while (start < description.size())
  {
    size_t end = description.find('\n', start);
    if (end == std::string_view::npos)
      end = description.size();
    const std::string_view line = description.substr(start, end - start);
    start = end + 1;
    const auto c = ParseCandidate(line);
    if (c && c->type == "host")
    {
      return fmt::format("a=candidate:9 1 UDP 2122317823 127.0.0.1 {} typ host\r\n"
                         "a=candidate:10 1 UDP 2122317822 ::1 {} typ host\r\n",
                         c->port, c->port);
    }
  }
  return "";
}

// Whether two machines share an IPv4 /24 or IPv6 /64 among their host candidates.
bool SameSubnet(const std::vector<std::string>& mine, const std::vector<std::string>& theirs)
{
  const auto prefix = [](const std::string& address) -> std::string {
    in_addr v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, address.c_str(), &v4) == 1)
    {
      const auto* b = reinterpret_cast<const u8*>(&v4);
      return std::string("4") + std::string(reinterpret_cast<const char*>(b), 3);
    }
    if (inet_pton(AF_INET6, address.c_str(), &v6) == 1)
    {
      const auto* b = reinterpret_cast<const u8*>(&v6);
      return std::string("6") + std::string(reinterpret_cast<const char*>(b), 8);
    }
    return "";
  };
  for (const std::string& a : mine)
  {
    const std::string p = prefix(a);
    if (p.empty())
      continue;
    for (const std::string& b : theirs)
    {
      if (prefix(b) == p)
        return true;
    }
  }
  return false;
}

std::string TypeOf(const char* candidate)
{
  const auto c = ParseCandidate(candidate ? candidate : "");
  return c ? c->type : "?";
}

// Resolves a server name to one IPv4 address, so libjuice needs no resolver thread whose join could
// delay an agent's teardown. Cached for the life of the link thread.
std::optional<std::string> Resolve(const std::string& host)
{
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
    return std::nullopt;
  char text[INET6_ADDRSTRLEN] = {};
  std::optional<std::string> out;
  for (const addrinfo* ai = result; ai && !out; ai = ai->ai_next)
  {
    if (ai->ai_family == AF_INET &&
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(ai->ai_addr)->sin_addr, text,
                  sizeof(text)))
    {
      out = text;
    }
  }
  freeaddrinfo(result);
  return out;
}

// Binds and closes a UDP socket on every interface, IPv4 and IPv6: the first such bind is what
// makes Windows ask whether Orca may use the network.
void PrimeFirewall()
{
  Common::SocketContext context;
  for (const int family : {AF_INET, AF_INET6})
  {
#ifdef _WIN32
    const SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
      continue;
#else
    const int s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
      continue;
#endif
    sockaddr_storage address{};
    socklen_t length;
    if (family == AF_INET)
    {
      auto* a = reinterpret_cast<sockaddr_in*>(&address);
      a->sin_family = AF_INET;
      a->sin_addr.s_addr = htonl(INADDR_ANY);
      length = sizeof(sockaddr_in);
    }
    else
    {
      auto* a = reinterpret_cast<sockaddr_in6*>(&address);
      a->sin6_family = AF_INET6;
      a->sin6_addr = in6addr_any;
      length = sizeof(sockaddr_in6);
    }
    const bool bound = bind(s, reinterpret_cast<const sockaddr*>(&address), length) == 0;
    if (bound)
    {
      // Also a receive (nothing waits for it), as a listening application would.
#ifdef _WIN32
      u_long nonblocking = 1;
      ioctlsocket(s, FIONBIO, &nonblocking);
#else
      fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
      char byte;
      recv(s, &byte, 1, 0);
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
  }
}
}  // namespace

std::string DescriptionFor(std::string_view description, Orca::Sandbox::NetMode net)
{
  return FilterDescription(description, "", nullptr, nullptr, net);
}

bool OneLan(const std::vector<std::string>& my_hosts, const std::vector<std::string>& their_hosts,
            const std::vector<std::string>& my_srflx, const std::vector<std::string>& their_srflx)
{
  if (!SameSubnet(my_hosts, their_hosts))
    return false;
  for (const std::string& mine : my_srflx)
  {
    if (std::find(their_srflx.begin(), their_srflx.end(), mine) != their_srflx.end())
      return true;
  }
  return false;
}

// ---- Keys ----

namespace DirectCrypto
{
std::pair<Key, Key> DeriveKeys(const Key& low_half, const Key& high_half, const std::string& room,
                               u16 generation, const std::string& low_id,
                               const std::string& high_id)
{
  std::array<u8, 64> ikm;
  std::copy(low_half.begin(), low_half.end(), ikm.begin());
  std::copy(high_half.begin(), high_half.end(), ikm.begin() + 32);
  std::string info = "orca-dl1";
  info += static_cast<char>(generation >> 8);
  info += static_cast<char>(generation & 0xff);
  info += low_id;
  info += '\0';
  info += high_id;
  std::array<u8, 64> okm{};
  const int result = mbedtls_hkdf(
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), reinterpret_cast<const u8*>(room.data()),
      room.size(), ikm.data(), ikm.size(), reinterpret_cast<const u8*>(info.data()), info.size(),
      okm.data(), okm.size());
  std::pair<Key, Key> keys{};
  if (result == 0)
  {
    std::copy_n(okm.begin(), 32, keys.first.begin());
    std::copy_n(okm.begin() + 32, 32, keys.second.begin());
  }
  else
  {
    // Never a usable key: both sides' datagrams fail authentication and the relay carries on.
    Common::Random::Generate(keys.first.data(), keys.first.size());
    Common::Random::Generate(keys.second.data(), keys.second.size());
  }
  return keys;
}
bool PeekHeader(const u8* data, size_t size, Header* header)
{
  if (size < DirectLink::HEADER_SIZE + DirectLink::TAG_SIZE || size > DirectLink::MAX_DATAGRAM ||
      data[0] != MAGIC || data[1] >> 4 != VERSION)
  {
    return false;
  }
  header->kind = static_cast<u8>(data[1] & 15);
  header->generation = GetU16(&data[2]);
  header->counter = GetU64(&data[4]);
  header->echo = GetU32(&data[12]);
  header->hold = GetU16(&data[16]);
  return header->counter != 0;
}

std::vector<u8> Seal(const Key& key, const Header& header, std::string_view payload)
{
  if (header.counter == 0 || payload.size() > DirectLink::MAX_PAYLOAD)
    return {};
  std::vector<u8> out(DirectLink::HEADER_SIZE + payload.size() + DirectLink::TAG_SIZE);
  out[0] = MAGIC;
  out[1] = static_cast<u8>(VERSION << 4 | (header.kind & 15));
  PutU16(&out[2], header.generation);
  PutU64(&out[4], header.counter);
  PutU32(&out[12], header.echo);
  PutU16(&out[16], header.hold);
  // Each direction of each link has its own key and an ever-growing counter, so a nonce is never
  // reused under one key.
  std::array<u8, 12> nonce{};
  PutU64(&nonce[4], header.counter);
  mbedtls_chachapoly_context context;
  mbedtls_chachapoly_init(&context);
  const bool ok =
      mbedtls_chachapoly_setkey(&context, key.data()) == 0 &&
      mbedtls_chachapoly_encrypt_and_tag(
          &context, payload.size(), nonce.data(), out.data(), DirectLink::HEADER_SIZE,
          reinterpret_cast<const u8*>(payload.data()), out.data() + DirectLink::HEADER_SIZE,
          out.data() + DirectLink::HEADER_SIZE + payload.size()) == 0;
  mbedtls_chachapoly_free(&context);
  if (!ok)
    return {};
  return out;
}

std::optional<std::string> Open(const Key& key, const u8* data, size_t size, Header* header)
{
  if (!PeekHeader(data, size, header))
    return std::nullopt;
  const size_t length = size - DirectLink::HEADER_SIZE - DirectLink::TAG_SIZE;
  std::string payload(length, '\0');
  std::array<u8, 12> nonce{};
  PutU64(&nonce[4], header->counter);
  mbedtls_chachapoly_context context;
  mbedtls_chachapoly_init(&context);
  const bool ok =
      mbedtls_chachapoly_setkey(&context, key.data()) == 0 &&
      mbedtls_chachapoly_auth_decrypt(&context, length, nonce.data(), data, DirectLink::HEADER_SIZE,
                                      data + DirectLink::HEADER_SIZE + length,
                                      data + DirectLink::HEADER_SIZE,
                                      reinterpret_cast<u8*>(payload.data())) == 0;
  mbedtls_chachapoly_free(&context);
  if (!ok)
    return std::nullopt;
  return payload;
}

bool ReplayWindow::Fresh(u64 counter) const
{
  if (counter == 0 || counter + 64 <= top)
    return false;
  return counter > top || !((seen >> (top - counter)) & 1);
}

void ReplayWindow::Take(u64 counter)
{
  if (counter > top)
  {
    seen = counter - top >= 64 ? 0 : seen << (counter - top);
    seen |= 1;
    top = counter;
  }
  else
  {
    seen |= u64{1} << (top - counter);
  }
}
}  // namespace DirectCrypto

// ---- ICE servers from a ticket ----

bool ParseIce(const picojson::value& ice, IceConfig* out)
{
  if (!ice.is<picojson::array>())
    return false;
  std::optional<std::pair<std::string, u16>> stun;
  struct Turn
  {
    std::string host;
    u16 port;
    std::string user, pass;
  };
  std::vector<Turn> turns;
  for (const picojson::value& server : ice.get<picojson::array>())
  {
    if (!server.is<picojson::object>())
      continue;
    const auto& o = server.get<picojson::object>();
    std::vector<std::string> urls;
    if (const auto it = o.find("urls"); it != o.end())
    {
      if (it->second.is<std::string>())
        urls.push_back(it->second.get<std::string>());
      else if (it->second.is<picojson::array>())
      {
        for (const auto& u : it->second.get<picojson::array>())
        {
          if (u.is<std::string>())
            urls.push_back(u.get<std::string>());
        }
      }
    }
    for (const std::string& url : urls)
    {
      // stun:host[:port], turn:host[:port][?transport=udp|tcp]; never turns: (TLS) here.
      const bool is_stun = url.starts_with("stun:");
      const bool is_turn = url.starts_with("turn:");
      if (!is_stun && !is_turn)
        continue;
      std::string rest = url.substr(5);
      std::string query;
      if (const size_t q = rest.find('?'); q != std::string::npos)
      {
        query = rest.substr(q + 1);
        rest.resize(q);
      }
      if (is_turn && !query.empty() && query != "transport=udp")
        continue;
      std::string host = rest;
      u16 port = 3478;
      if (const size_t colon = rest.rfind(':');
          colon != std::string::npos && rest.find(']') == std::string::npos)
      {
        host = rest.substr(0, colon);
        const int p = std::atoi(rest.c_str() + colon + 1);
        if (p <= 0 || p > 65535)
          continue;
        port = static_cast<u16>(p);
      }
      if (host.empty() || host.size() > 253)
        continue;
      if (is_stun && !stun)
        stun = {host, port};
      if (is_turn && o.count("username") && o.count("credential"))
        turns.push_back({host, port, Str(o, "username"), Str(o, "credential")});
    }
  }
  if (stun)
  {
    out->stun_host = stun->first;
    out->stun_port = stun->second;
  }
  // One TURN allocation per link: port 3478, else 53 (some networks only let DNS out), else any.
  const auto pick = [&]() -> const Turn* {
    for (const u16 want : {u16{3478}, u16{53}})
    {
      for (const Turn& t : turns)
      {
        if (t.port == want)
          return &t;
      }
    }
    return turns.empty() ? nullptr : &turns.front();
  };
  if (const Turn* t = pick(); t && !t->user.empty() && !t->pass.empty())
  {
    out->turn_host = t->host;
    out->turn_port = t->port;
    out->turn_user = t->user;
    out->turn_pass = t->pass;
  }
  else
  {
    out->turn_host.clear();
  }
  return true;
}

std::optional<DirectOptions> DirectOptionsFromEnv()
{
  if (GetEnv("ORCA_DIRECT") == "0")
    return std::nullopt;
  DirectOptions options;
  options.force = GetEnv("ORCA_DIRECT_FORCE");
  if (options.force != "turn" && options.force != "srflx")
    options.force.clear();
  // In the app's sandbox, the network it gives Orca (Orca/Sandbox.h): never loopback there.
  options.net = Orca::Sandbox::NetModeFromEnv();
  options.loopback =
      GetEnv("ORCA_DIRECT_LOOPBACK") == "1" && Orca::Sandbox::LoopbackAllowed(options.net);
  options.loss_percent = std::clamp(std::atoi(GetEnv("ORCA_DIRECT_LOSS").c_str()), 0, 100);
  options.blocked = GetEnv("ORCA_DIRECT_BLOCK") == "1";
  options.net_delay_ms = TestNetDelayMs();
  options.net_jitter_ms = TestNetJitterMs();
  return options;
}

// ---- The links ----

struct Link : std::enable_shared_from_this<Link>
{
  DirectLink::Impl* owner = nullptr;
  const u64 serial = ++s_link_serial;
  std::string peer;
  int slot = -1;
  u16 generation = 0;
  // The peer's removal count when this link was built (DirectLink::Impl::epochs).
  u64 epoch = 0;
  // Set before the agent can call back or be sent on; cleared under the send mutex before it is
  // destroyed.
  juice_agent_t* agent = nullptr;
  std::atomic<int> state{JUICE_STATE_DISCONNECTED};

  std::mutex m;  // guards everything below
  bool keyed = false;
  DirectCrypto::Key tx_key{}, rx_key{};
  u64 tx_counter = 0;
  DirectCrypto::ReplayWindow replay;
  // The newest datagram received (echoed back for the peer's round trip), and when.
  u64 echo_counter = 0;
  Clock::time_point echo_at{};
  u32 last_echo_used = 0;
  // When each recent counter went out (indexed by counter % SENT_RING).
  std::array<std::pair<u64, Clock::time_point>, SENT_RING> sent_at{};
  std::deque<int> rtt_us;
  std::optional<Clock::time_point> last_rx;
  Clock::time_point last_tx{};
  u64 sent = 0, received = 0, auth_drops = 0, replays = 0;
  // Sends on the caller's thread, sealing included: the longest, the total, and how many took over
  // a millisecond.
  int max_send_us = 0;
  u64 send_us_total = 0, sends_timed = 0, slow_sends = 0;

  // Deliver's token bucket (FLOOD_RATE), under DirectLink::Impl::inbox_mutex, and the authentic
  // packets it dropped (readable from anywhere).
  double tokens = FLOOD_BURST;
  std::optional<Clock::time_point> tokens_at;
  std::atomic<u64> flood_drops{0};

  // Under inbox_mutex: whether a packet arriving now may be delivered.
  bool TakeToken(Clock::time_point now)
  {
    if (tokens_at)
    {
      const double seconds = std::chrono::duration<double>(now - *tokens_at).count();
      tokens = std::min(FLOOD_BURST, tokens + std::max(0.0, seconds) * FLOOD_RATE);
    }
    tokens_at = now;
    if (tokens < 1)
      return false;
    tokens -= 1;
    return true;
  }

  bool Selected() const
  {
    const int s = state.load();
    return s == JUICE_STATE_CONNECTED || s == JUICE_STATE_COMPLETED;
  }

  // A datagram of `kind` carrying `payload`; nothing before the keys are in.
  std::optional<std::vector<u8>> Seal(u8 kind, std::string_view payload)
  {
    std::lock_guard lock(m);
    if (!keyed)
      return std::nullopt;
    const Clock::time_point now = Clock::now();
    DirectCrypto::Header header;
    header.kind = kind;
    header.generation = generation;
    header.counter = ++tx_counter;
    if (echo_counter > 0)
    {
      const auto held = std::chrono::duration_cast<std::chrono::microseconds>(now - echo_at);
      header.echo = static_cast<u32>(echo_counter);
      header.hold = static_cast<u16>(std::clamp<s64>(held.count() / 100, 0, 0xFFFF));
    }
    std::vector<u8> out = DirectCrypto::Seal(tx_key, header, payload);
    if (out.empty())
      return std::nullopt;
    sent_at[header.counter % SENT_RING] = {header.counter, now};
    last_tx = now;
    ++sent;
    return out;
  }

  // The payload of an authentic, not-yet-seen datagram on this link (`kind` says which); else
  // nothing.
  std::optional<std::string> Open(const u8* data, size_t size, u8* kind)
  {
    DirectCrypto::Header header;
    if (!DirectCrypto::PeekHeader(data, size, &header))
      return std::nullopt;
    std::lock_guard lock(m);
    // Keys not in yet (the answer is still travelling through the room while the peer's checks
    // already succeeded): nothing to open, and nothing wrong.
    if (!keyed)
      return std::nullopt;
    if (header.generation != generation)
    {
      ++auth_drops;
      return std::nullopt;
    }
    if (!replay.Fresh(header.counter))
    {
      ++replays;
      return std::nullopt;
    }
    auto payload = DirectCrypto::Open(rx_key, data, size, &header);
    if (!payload)
    {
      ++auth_drops;
      return std::nullopt;
    }
    replay.Take(header.counter);
    *kind = header.kind;
    const Clock::time_point now = Clock::now();
    if (header.counter > echo_counter)
    {
      echo_counter = header.counter;
      echo_at = now;
    }
    // Our datagram the peer last received, and how long it held it, give the round trip.
    if (header.echo != 0 && header.echo != last_echo_used)
    {
      last_echo_used = header.echo;
      const auto& [sent_counter, sent_time] = sent_at[header.echo % SENT_RING];
      if (static_cast<u32>(sent_counter) == header.echo && sent_counter != 0)
      {
        const s64 rtt =
            std::chrono::duration_cast<std::chrono::microseconds>(now - sent_time).count() -
            s64{header.hold} * 100;
        if (rtt >= 0 && rtt < 10'000'000)
        {
          rtt_us.push_back(static_cast<int>(rtt));
          if (rtt_us.size() > RTT_SAMPLES_KEPT)
            rtt_us.pop_front();
        }
      }
    }
    last_rx = now;
    ++received;
    return payload;
  }

  // Under m.
  int MedianRttMs() const
  {
    if (rtt_us.empty())
      return -1;
    std::vector<int> sorted(rtt_us.begin(), rtt_us.end());
    std::nth_element(sorted.begin(), sorted.begin() + (sorted.size() - 1) / 2, sorted.end());
    return (sorted[(sorted.size() - 1) / 2] + 500) / 1000;
  }
};

struct DirectLink::Impl
{
  // A peer and its signalling; link thread only.
  struct Peer
  {
    std::string id;
    int slot = -1;
    // This side has the lower connection id, so it offers.
    bool low = false;
    u16 generation = 0;
    std::shared_ptr<Link> link;
    DirectCrypto::Key my_half{};
    bool answered = false;  // both descriptions are in
    std::vector<std::string> early_candidates;
    bool early_done = false;
    size_t remote_candidates = 0;
    std::vector<std::string> my_hosts, their_hosts;
    // Public addresses seen by STUN (server-reflexive candidates): machines behind one NAT share
    // one.
    std::vector<std::string> my_srflx, their_srflx;
    // Candidates to trickle, and whether gathering is done (sent with the last batch).
    std::vector<std::string> trickle;
    bool trickle_done = false;
    Clock::time_point last_trickle{};
    Clock::time_point added_at{}, built_at{}, answered_at{};
    std::optional<Clock::time_point> connected_at, up_since;
    std::string pair;
    bool turn = false;
    // Whether this pair's links use the ticket's TURN server. Set once a link to this peer never
    // came up; the answering side follows the offer, the offering side follows `dlr` "t" requests,
    // and an upgrade clears it.
    bool with_turn = false;
    bool was_up = false;
    // This link was up at some point (a link that never came up isn't "silent").
    bool ever_up = false;
    bool upgrade_tried = false;
    Clock::time_point next_rebuild{};
    int backoff_s = FIRST_BACKOFF_S;
    int builds = 0;
    std::string reason;
    // Offering side: a build to start once the last is half a second old (the answering side said
    // it holds a newer generation).
    std::string pending_build;
    // Answering side: when it last answered a refused offer with its generation.
    Clock::time_point refusal_answered_at{};
  };

  struct Command
  {
    enum class Kind
    {
      Add,
      Remove,
      Signal,
      Ice,
      Test,
    } kind;
    std::string id;
    int slot = -1;
    picojson::object message;
    IceConfig ice;
    std::string test;
  };

  struct Event
  {
    enum class Kind
    {
      State,
      Candidate,
      GatheringDone,
    } kind;
    u64 serial;
    int state = 0;
    std::string candidate;
  };

  struct Held
  {
    Clock::time_point due;
    std::shared_ptr<Link> link;
    std::vector<u8> datagram;
  };

  // A datagram held by a test delay before it is opened, so the measured round trip includes the
  // delay both ways.
  struct HeldIn
  {
    Clock::time_point due;
    std::shared_ptr<Link> link;
    std::vector<u8> datagram;
  };

  // Keeps Winsock up for the link thread's name lookups (no-op elsewhere).
  Common::SocketContext socket_context;
  DirectOptions options;
  std::thread thread;
  std::atomic<bool> started{false};
  std::atomic<bool> stopping{false};

  // Lock order (never hold one while taking another except as listed): send_mutex, then a link's m;
  // libjuice's own lock (held around its callbacks, taken by a send through TURN), then a link's m,
  // inbox_mutex or cmd_mutex. Nothing waits on send_mutex while holding cmd_mutex.
  //
  // Commands in, signals out, and the link thread's wake-up (under cmd_mutex).
  std::mutex cmd_mutex;
  std::condition_variable wake;
  std::deque<Command> commands;
  std::deque<Event> events;
  std::vector<std::pair<std::string, picojson::object>> signals;
  std::map<std::string, DirectStats> stats;
  // When each peer's packets last came through the relay.
  std::map<std::string, Clock::time_point> relay_heard;

  // The links datagrams go out on (under send_mutex), and datagrams held by a test delay.
  std::mutex send_mutex;
  std::vector<std::shared_ptr<Link>> sending;
  std::deque<Held> held_out;
  bool stopped = false;
  u32 jitter_count = 0;

  // Received packets (under inbox_mutex), and each peer's removal count: a link built before its
  // peer's latest removal delivers nothing.
  std::mutex inbox_mutex;
  std::deque<Received> inbox;
  std::deque<HeldIn> held_in;
  std::map<std::string, u64> epochs;
  u32 inbound_jitter_count = 0;

  std::atomic<bool> block_in{false}, block_out{false};
  std::atomic<u32> loss_state{0x9e3779b9};

  // Owned by the link thread.
  std::map<std::string, Peer> peers;
  std::map<std::string, std::pair<std::string, Clock::time_point>> resolved;
  // After a failed mint of fresh ICE credentials: the earliest next try, and the wait after the
  // next failure.
  Clock::time_point fresh_ice_retry_at{};
  std::chrono::seconds fresh_ice_backoff = FRESH_ICE_RETRY_FIRST;

  ~Impl() { Stop(); }

  // ---- Any thread ----

  void Post(Command command)
  {
    {
      std::lock_guard lock(cmd_mutex);
      if (commands.size() < 4096)
        commands.push_back(std::move(command));
    }
    wake.notify_all();
  }

  void Stop()
  {
    if (stopping.exchange(true))
    {
      if (thread.joinable() && thread.get_id() != std::this_thread::get_id())
        thread.join();
      return;
    }
    wake.notify_all();
    if (thread.joinable())
      thread.join();
    // The thread destroyed its links on the way out (or never started).
    std::lock_guard lock(send_mutex);
    stopped = true;
    sending.clear();
    held_out.clear();
  }

  Clock::time_point Hold(u32* count)
  {
    const int jitter = options.net_jitter_ms;
    const u32 extra = jitter > 0 ? (++*count * 2654435761u) % static_cast<u32>(jitter + 1) : 0;
    return Clock::now() + std::chrono::milliseconds(*options.net_delay_ms + static_cast<int>(extra));
  }

  bool Lose()
  {
    if (options.loss_percent <= 0)
      return false;
    // xorshift: cheap, lock-free, good enough to drop a share of datagrams.
    u32 x = loss_state.load();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    loss_state.store(x);
    return static_cast<int>(x % 100) < options.loss_percent;
  }

  void SendOn(Link& link, const std::vector<u8>& datagram)
  {
    // Under send_mutex, so the agent is alive.
    if (!link.agent)
      return;
#ifdef _WIN32
    // libjuice can't mark traffic on Windows (that would need qWAVE, as Slippi uses).
    juice_send(link.agent, reinterpret_cast<const char*>(datagram.data()), datagram.size());
#else
    juice_send_diffserv(link.agent, reinterpret_cast<const char*>(datagram.data()),
                        datagram.size(), TOS_EXPEDITED);
#endif
  }

  void Send(std::string_view payload, u8 kind = KIND_PACKET, Link* only = nullptr)
  {
    if (payload.size() > MAX_PAYLOAD)
      return;
    std::lock_guard lock(send_mutex);
    if (stopped)
      return;
    for (const std::shared_ptr<Link>& link : sending)
    {
      if ((only && link.get() != only) || !link->Selected())
        continue;
      const auto start = Clock::now();
      const auto datagram = link->Seal(kind, payload);
      if (!datagram || block_out || Lose())
        continue;
      if (options.net_delay_ms)
      {
        held_out.push_back({Hold(&jitter_count), link, *datagram});
        wake.notify_all();
        continue;
      }
      SendOn(*link, *datagram);
      const int us = static_cast<int>(
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
      std::lock_guard link_lock(link->m);
      link->max_send_us = std::max(link->max_send_us, us);
      link->send_us_total += static_cast<u64>(us);
      ++link->sends_timed;
      link->slow_sends += us > 1000;
    }
  }

  std::vector<Received> Receive()
  {
    std::lock_guard lock(inbox_mutex);
    std::vector<Received> out(std::make_move_iterator(inbox.begin()),
                              std::make_move_iterator(inbox.end()));
    inbox.clear();
    return out;
  }

  // ---- libjuice's thread (callbacks: never call into juice or touch the link thread's state) ----

  static void OnState(juice_agent_t*, juice_state_t state, void* user)
  {
    auto* link = static_cast<Link*>(user);
    link->state = static_cast<int>(state);
    link->owner->PushEvent({Event::Kind::State, link->serial, static_cast<int>(state), {}});
  }

  static void OnCandidate(juice_agent_t*, const char* sdp, void* user)
  {
    auto* link = static_cast<Link*>(user);
    link->owner->PushEvent({Event::Kind::Candidate, link->serial, 0, sdp ? sdp : ""});
  }

  static void OnGatheringDone(juice_agent_t*, void* user)
  {
    auto* link = static_cast<Link*>(user);
    link->owner->PushEvent({Event::Kind::GatheringDone, link->serial, 0, {}});
  }

  static void OnRecv(juice_agent_t*, const char* data, size_t size, void* user)
  {
    auto* link = static_cast<Link*>(user);
    link->owner->OnDatagram(*link, reinterpret_cast<const u8*>(data), size);
  }

  void PushEvent(Event event)
  {
    {
      std::lock_guard lock(cmd_mutex);
      if (events.size() < 4096)
        events.push_back(std::move(event));
    }
    wake.notify_all();
  }

  void OnDatagram(Link& link, const u8* data, size_t size)
  {
    if (block_in)
      return;
    if (options.net_delay_ms)
    {
      {
        std::lock_guard lock(inbox_mutex);
        if (held_in.size() >= MAX_INBOX)
          held_in.pop_front();
        Clock::time_point due = Hold(&inbound_jitter_count);
        if (!held_in.empty())
          due = std::max(due, held_in.back().due);
        // The shared pointer keeps the link alive past its agent's callbacks.
        held_in.push_back({due, link.shared_from_this(), std::vector<u8>(data, data + size)});
      }
      wake.notify_all();
      return;
    }
    Deliver(link, data, size);
  }

  void Deliver(Link& link, const u8* data, size_t size)
  {
    u8 kind = 0;
    auto payload = link.Open(data, size, &kind);
    if (!payload || kind != KIND_PACKET)
      return;
    std::lock_guard lock(inbox_mutex);
    if (epochs[link.peer] != link.epoch)
      return;
    if (!link.TakeToken(Clock::now()))
    {
      if (link.flood_drops++ == 0)
      {
        WARN_LOG_FMT(NETPLAY,
                     "Orca direct link: seat {} sends over {} packets a second; the rest of them "
                     "come through the relay only (link {})",
                     link.slot + 1, FLOOD_RATE, link.generation);
      }
      return;
    }
    if (inbox.size() >= MAX_INBOX)
      inbox.pop_front();
    inbox.push_back({link.peer, link.slot, std::move(*payload)});
  }

  void DeliverHeld()
  {
    std::vector<HeldIn> due;
    {
      const auto now = Clock::now();
      std::lock_guard lock(inbox_mutex);
      while (!held_in.empty() && held_in.front().due <= now)
      {
        due.push_back(std::move(held_in.front()));
        held_in.pop_front();
      }
    }
    for (HeldIn& in : due)
      Deliver(*in.link, in.datagram.data(), in.datagram.size());
  }

  // ---- The link thread ----

  void Run()
  {
    SetUpJuiceLogging();
    PrimeFirewall();
    while (!stopping)
    {
      std::deque<Command> cmds;
      std::deque<Event> evs;
      // Before taking cmd_mutex: NextWait takes the send and inbox locks, and libjuice's thread
      // takes cmd_mutex from a callback while holding its own lock, which a send through TURN waits
      // for under the send lock.
      const auto wait = NextWait();
      {
        std::unique_lock lock(cmd_mutex);
        wake.wait_for(lock, wait, [this] {
          return stopping.load() || !commands.empty() || !events.empty();
        });
        cmds.swap(commands);
        evs.swap(events);
      }
      if (stopping)
        break;
      for (Command& command : cmds)
        Handle(command);
      for (Event& event : evs)
        Handle(event);
      Timers();
      SendHeld();
      DeliverHeld();
      PublishStats();
    }
    for (auto& [id, peer] : peers)
      DestroyLink(peer);
    peers.clear();
  }

  std::chrono::milliseconds NextWait()
  {
    if (peers.empty())
      return 100ms;
    std::optional<Clock::time_point> next;
    {
      std::lock_guard lock(send_mutex);
      if (!held_out.empty())
        next = held_out.front().due;
    }
    {
      std::lock_guard lock(inbox_mutex);
      if (!held_in.empty())
        next = std::min(next.value_or(held_in.front().due), held_in.front().due);
    }
    if (!next)
      return 10ms;
    return std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(*next - Clock::now()),
                      0ms, 10ms);
  }

  void SendHeld()
  {
    const auto now = Clock::now();
    std::lock_guard lock(send_mutex);
    while (!held_out.empty() && held_out.front().due <= now)
    {
      SendOn(*held_out.front().link, held_out.front().datagram);
      held_out.pop_front();
    }
  }

  void Signal(const Peer& peer, picojson::object message)
  {
    std::lock_guard lock(cmd_mutex);
    if (signals.size() < 256)
      signals.emplace_back(peer.id, std::move(message));
  }

  void Handle(Command& command)
  {
    switch (command.kind)
    {
    case Command::Kind::Add:
    {
      if (peers.contains(command.id))
        break;
      Peer& peer = peers[command.id];
      peer.id = command.id;
      peer.slot = command.slot;
      peer.low = options.me < command.id;
      peer.added_at = Clock::now();
      if (peer.low)
        Build(peer, "new", nullptr);
      break;
    }
    case Command::Kind::Remove:
    {
      const auto it = peers.find(command.id);
      if (it == peers.end())
        break;
      DestroyLink(it->second);
      peers.erase(it);
      std::lock_guard lock(cmd_mutex);
      stats.erase(command.id);
      relay_heard.erase(command.id);
      break;
    }
    case Command::Kind::Signal:
    {
      const auto it = peers.find(command.id);
      if (it != peers.end())
        OnSignal(it->second, command.message);
      break;
    }
    case Command::Kind::Ice:
      options.ice = command.ice;
      options.ice_minted = Clock::now();
      break;
    case Command::Kind::Test:
      if (command.test == "rebuild")
      {
        for (auto& [id, peer] : peers)
          Rebuild(peer, "test", true);
      }
      break;
    }
  }

  Peer* PeerOf(u64 serial)
  {
    for (auto& [id, peer] : peers)
    {
      if (peer.link && peer.link->serial == serial)
        return &peer;
    }
    return nullptr;
  }

  void Handle(Event& event)
  {
    Peer* peer = PeerOf(event.serial);
    if (!peer)
      return;  // the link was destroyed since
    switch (event.kind)
    {
    case Event::Kind::State:
      if (event.state == JUICE_STATE_CONNECTED || event.state == JUICE_STATE_COMPLETED)
      {
        if (!peer->connected_at)
          peer->connected_at = Clock::now();
        ReadPair(*peer);
      }
      else if (event.state == JUICE_STATE_FAILED)
      {
        INFO_LOG_FMT(NETPLAY, "Orca direct link: seat {} found no path (link {})", peer->slot + 1,
                     peer->generation);
      }
      break;
    case Event::Kind::Candidate:
    {
      const auto c = ParseCandidate(event.candidate);
      if (c && c->type == "srflx")
        peer->my_srflx.push_back(c->address);
      // Host candidates already went out with the description.
      if (!c || c->type == "host" || !Allowed(options.force, c->type) ||
          !Orca::Sandbox::AddressAllowed(options.net, c->address) ||
          event.candidate.size() > MAX_CANDIDATE_LENGTH)
      {
        break;
      }
      peer->trickle.push_back(event.candidate);
      break;
    }
    case Event::Kind::GatheringDone:
      peer->trickle_done = true;
      break;
    }
  }

  // The selected pair's candidate types. libjuice reports a pair sent from the agent's socket with
  // its first local candidate, normally "host", so the remote half is what tells a LAN path from a
  // NAT's.
  void ReadPair(Peer& peer)
  {
    if (!peer.link || !peer.link->Selected())
      return;
    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
    char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
    if (juice_get_selected_candidates(peer.link->agent, local, sizeof(local), remote,
                                      sizeof(remote)) != 0)
    {
      return;
    }
    const std::string pair = TypeOf(local) + "/" + TypeOf(remote);
    if (pair == peer.pair)
      return;
    peer.pair = pair;
    peer.turn = TypeOf(local) == "relay" || TypeOf(remote) == "relay";
    INFO_LOG_FMT(NETPLAY, "Orca direct link: seat {} selected {} (link {})", peer.slot + 1, pair,
                 peer.generation);
  }

  void OnSignal(Peer& peer, const picojson::object& message)
  {
    const std::string kind = Str(message, "k");
    const auto generation = Int(message, "g", 0, 0xFFFF);
    if (!generation)
      return;
    if (kind == "dl")
    {
      DirectCrypto::Key half{};
      const std::string description = Str(message, "desc");
      if (Int(message, "v", 1, 1) != 1 || *generation == 0 ||
          !FromHex(Str(message, "key"), half.data(), half.size()) || description.empty() ||
          description.size() > MAX_DESCRIPTION)
      {
        return;
      }
      if (!peer.low)
      {
        // An offer: build a new link, unless this side already answered it or one came less than
        // half a second ago (an honest peer offers at most once a second and re-offers if
        // unanswered).
        if (*generation < peer.generation)
        {
          // Older than this side's newest link: the offering side started over (unlinked and
          // relinked) and counts from 1 again. Tell it where this side is so its next offer comes
          // after (`dlr` below).
          const auto now = Clock::now();
          if (now - peer.refusal_answered_at >= REFUSAL_ANSWER_GAP)
          {
            peer.refusal_answered_at = now;
            INFO_LOG_FMT(NETPLAY,
                         "Orca direct link: seat {} offered link {}, older than link {}: asking "
                         "for a newer one",
                         peer.slot + 1, *generation, peer.generation);
            picojson::object d;
            d["k"] = picojson::value("dlr");
            d["g"] = picojson::value(static_cast<double>(peer.generation));
            d["t"] = picojson::value(peer.with_turn ? 1.0 : 0.0);
            Signal(peer, std::move(d));
          }
          return;
        }
        if ((*generation == peer.generation && peer.link) ||
            (peer.link && Clock::now() - peer.built_at < 500ms))
        {
          return;
        }
        peer.generation = static_cast<u16>(*generation);
        Build(peer, "offer", &message);
        return;
      }
      // The answer to this side's offer.
      if (*generation != peer.generation || !peer.link || peer.answered)
        return;
      Answered(peer, half, description);
      return;
    }
    if (kind == "dlc")
    {
      if (*generation != peer.generation || !peer.link)
        return;
      const auto c = message.find("c");
      if (c != message.end() && c->second.is<picojson::array>())
      {
        size_t taken = 0;
        for (const picojson::value& v : c->second.get<picojson::array>())
        {
          if (++taken > MAX_CANDIDATES_IN_MESSAGE)
            break;
          if (!v.is<std::string>() || v.get<std::string>().size() > MAX_CANDIDATE_LENGTH)
            continue;
          AddRemoteCandidate(peer, v.get<std::string>());
        }
      }
      if (message.count("done") && message.at("done").evaluate_as_boolean())
      {
        if (peer.answered)
          juice_set_remote_gathering_done(peer.link->agent);
        else
          peer.early_done = true;
      }
      return;
    }
    if (kind == "dlr" && peer.low)
    {
      // Whether the next offer includes TURN, as the answering side asks: it found no path without
      // (1), or its upgrade wants a link without (0).
      if (const auto t = Int(message, "t", 0, 1); t && (*t == 1) != peer.with_turn)
      {
        peer.with_turn = *t == 1;
        NOTICE_LOG_FMT(NETPLAY, "Orca direct link: seat {} asks for a link {} TURN", peer.slot + 1,
                       peer.with_turn ? "with" : "without");
      }
      if (*generation > peer.generation)
      {
        // The answering side holds a newer generation than this side's newest: this side started
        // over and the other refuses anything older. The next offer skips past it once the last
        // build is half a second old.
        if (*generation == 0xFFFF)
          return;
        INFO_LOG_FMT(NETPLAY, "Orca direct link: seat {} is at link {}, past this side's {}",
                     peer.slot + 1, *generation, peer.generation);
        DestroyLink(peer);
        peer.generation = static_cast<u16>(*generation);
        peer.pending_build = "behind";
        return;
      }
      // The answering side wants a new link: its rules found this one dead, or it has none
      // (generation 0). Not twice in a second, since both sides' rules may fire together. An older
      // generation is a stale request from before this side's newest build.
      if ((*generation == peer.generation || *generation == 0) &&
          Clock::now() - peer.built_at >= 1s)
      {
        Build(peer, "asked", nullptr);
      }
    }
  }

  void AddRemoteCandidate(Peer& peer, const std::string& line)
  {
    const auto c = ParseCandidate(line);
    if (!c || !Orca::Sandbox::AddressAllowed(options.net, c->address))
      return;
    if (c->type == "host" && c->address != "127.0.0.1" && c->address != "::1")
      peer.their_hosts.push_back(c->address);
    if (c->type == "srflx")
      peer.their_srflx.push_back(c->address);
    if (!Allowed(options.force, c->type))
      return;
    if (!peer.answered)
    {
      if (peer.early_candidates.size() < MAX_REMOTE_CANDIDATES)
        peer.early_candidates.push_back(line);
      return;
    }
    if (peer.remote_candidates >= MAX_REMOTE_CANDIDATES)
      return;
    ++peer.remote_candidates;
    juice_add_remote_candidate(peer.link->agent, line.c_str());
  }

  void Answered(Peer& peer, const DirectCrypto::Key& their_half, const std::string& description)
  {
    // Keys from both halves, the lower id's first; each direction has its own.
    const bool low = peer.low;
    const auto [low_key, high_key] = DirectCrypto::DeriveKeys(
        low ? peer.my_half : their_half, low ? their_half : peer.my_half, options.room,
        peer.generation, low ? options.me : peer.id, low ? peer.id : options.me);
    {
      std::lock_guard lock(peer.link->m);
      peer.link->tx_key = low ? low_key : high_key;
      peer.link->rx_key = low ? high_key : low_key;
      peer.link->keyed = true;
    }
    peer.their_hosts.clear();
    peer.their_srflx.clear();
    const std::string remote = FilterDescription(description, options.force, &peer.their_hosts,
                                                 &peer.their_srflx, options.net);
    if (juice_set_remote_description(peer.link->agent, remote.c_str()) != JUICE_ERR_SUCCESS)
    {
      WARN_LOG_FMT(NETPLAY, "Orca direct link: seat {} sent a description libjuice refused",
                   peer.slot + 1);
      return;
    }
    peer.answered = true;
    peer.answered_at = Clock::now();
    for (const std::string& line : std::exchange(peer.early_candidates, {}))
      AddRemoteCandidate(peer, line);
    if (std::exchange(peer.early_done, false))
      juice_set_remote_gathering_done(peer.link->agent);
  }

  void DestroyLink(Peer& peer)
  {
    std::shared_ptr<Link> link = std::move(peer.link);
    if (!link)
      return;
    {
      std::lock_guard lock(link->m);
      NOTICE_LOG_FMT(NETPLAY,
                     "Orca direct link: seat {} link {} closed: {} sent, {} received, {} refused, "
                     "{} replayed, {} over the rate; sends {:.1f} us on average, longest {} us, "
                     "{} over 1 ms",
                     peer.slot + 1, link->generation, link->sent, link->received, link->auth_drops,
                     link->replays, link->flood_drops.load(),
                     link->sends_timed ? static_cast<double>(link->send_us_total) /
                                             static_cast<double>(link->sends_timed) :
                                         0.0,
                     link->max_send_us, link->slow_sends);
    }
    juice_agent_t* agent;
    {
      std::lock_guard lock(send_mutex);
      std::erase(sending, link);
      agent = std::exchange(link->agent, nullptr);
    }
    // Once this returns, libjuice runs no more callbacks for it (it holds the registry lock while
    // calling back).
    if (agent)
      juice_destroy(agent);
    peer.answered = false;
    peer.early_candidates.clear();
    peer.early_done = false;
    peer.remote_candidates = 0;
    peer.trickle.clear();
    peer.trickle_done = false;
    peer.connected_at.reset();
    peer.pair.clear();
    peer.turn = false;
    peer.ever_up = false;
  }

  std::optional<std::string> ResolveCached(const std::string& host)
  {
    if (host.empty())
      return std::nullopt;
    const auto now = Clock::now();
    if (const auto it = resolved.find(host); it != resolved.end() && now - it->second.second < 10min)
      return it->second.first;
    const auto address = Resolve(host);
    if (!address)
    {
      WARN_LOG_FMT(NETPLAY, "Orca direct link: couldn't resolve {}", host);
      return std::nullopt;
    }
    resolved[host] = {*address, now};
    return address;
  }

  // A new link to `peer`: as the offering side (`offer` null), or answering `offer`.
  void Build(Peer& peer, const std::string& reason, const picojson::object* offer)
  {
    DestroyLink(peer);
    peer.pending_build.clear();
    if (peer.low)
    {
      if (peer.generation == 0xFFFF)
        return;
      ++peer.generation;
    }
    peer.built_at = Clock::now();
    ++peer.builds;
    peer.reason = reason;
    if (Clock::now() - options.ice_minted > ICE_FRESH_FOR && options.fresh_ice &&
        Clock::now() >= fresh_ice_retry_at)
    {
      if (const auto fresh = options.fresh_ice())
      {
        options.ice = *fresh;
        options.ice_minted = Clock::now();
        fresh_ice_backoff = FRESH_ICE_RETRY_FIRST;
      }
      else
      {
        // Don't retry on every build (each try can block this thread 10 s); links keep their
        // current credentials meanwhile, and TURN only fails once those expire.
        fresh_ice_retry_at = Clock::now() + fresh_ice_backoff;
        WARN_LOG_FMT(NETPLAY,
                     "Orca direct link: no fresh ICE servers; the next try in {} s, links keep "
                     "the ticket's until then",
                     fresh_ice_backoff.count());
        fresh_ice_backoff = std::min<std::chrono::seconds>(fresh_ice_backoff * 2,
                                                           FRESH_ICE_RETRY_MAX);
      }
    }
    const IceConfig& ice = options.ice;
    const auto stun = ResolveCached(ice.stun_host);
    const auto turn = ResolveCached(ice.turn_host);

    auto link = std::make_shared<Link>();
    link->owner = this;
    link->peer = peer.id;
    link->slot = peer.slot;
    link->generation = peer.generation;
    {
      std::lock_guard lock(inbox_mutex);
      link->epoch = epochs[peer.id];
    }
    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    // Tests only: ORCA_DIRECT_STUN=0 leaves the ticket's STUN server out of the agent.
    const bool use_stun = GetEnv("ORCA_DIRECT_STUN") != "0";
    // TURN only after a failure: keeping a TURN allocation alongside a working host or STUN path
    // can make some Wi-Fi chipsets drop out periodically, and most pairs never need it. A link
    // includes TURN once one to this peer never came up, or when the other side's offer or request
    // says so. Tests only: ORCA_DIRECT_TURN=0 never, =1 from the first link; a forced TURN path
    // always. See ORCA.md, "TURN only when needed".
    if (offer)
      peer.with_turn = Int(*offer, "t", 0, 1) == 1;
    const std::string turn_env = GetEnv("ORCA_DIRECT_TURN");
    const bool use_turn =
        turn_env != "0" && (turn_env == "1" || options.force == "turn" || peer.with_turn);
    config.stun_server_host = stun && use_stun ? stun->c_str() : nullptr;
    config.stun_server_port = ice.stun_port;
    juice_turn_server_t turn_server{};
    if (turn && use_turn && !ice.turn_user.empty())
    {
      turn_server.host = turn->c_str();
      turn_server.port = ice.turn_port;
      turn_server.username = ice.turn_user.c_str();
      turn_server.password = ice.turn_pass.c_str();
      config.turn_servers = &turn_server;
      config.turn_servers_count = 1;
    }
    config.cb_state_changed = &OnState;
    config.cb_candidate = &OnCandidate;
    config.cb_gathering_done = &OnGatheringDone;
    config.cb_recv = &OnRecv;
    config.user_ptr = link.get();
    link->agent = juice_create(&config);
    if (!link->agent)
    {
      WARN_LOG_FMT(NETPLAY, "Orca direct link: couldn't create a link to seat {}", peer.slot + 1);
      return;
    }
    peer.link = link;
    Common::Random::Generate(peer.my_half.data(), peer.my_half.size());

    if (offer)
    {
      // Controlled: the offer's description first, then this side's own candidates.
      DirectCrypto::Key half{};
      FromHex(Str(*offer, "key"), half.data(), half.size());
      Answered(peer, half, Str(*offer, "desc"));
      if (!peer.answered)
      {
        DestroyLink(peer);
        return;
      }
    }
    if (juice_gather_candidates(link->agent) != JUICE_ERR_SUCCESS)
    {
      WARN_LOG_FMT(NETPLAY, "Orca direct link: couldn't open a socket for seat {}", peer.slot + 1);
      DestroyLink(peer);
      return;
    }
    char description[JUICE_MAX_SDP_STRING_LEN] = {};
    if (juice_get_local_description(link->agent, description, sizeof(description)) !=
        JUICE_ERR_SUCCESS)
    {
      DestroyLink(peer);
      return;
    }
    peer.my_hosts.clear();
    peer.my_srflx.clear();
    std::string mine =
        FilterDescription(description, options.force, &peer.my_hosts, &peer.my_srflx, options.net);
    if (options.loopback && options.force.empty())
      mine += LoopbackCandidates(description);
    {
      std::lock_guard lock(send_mutex);
      sending.push_back(link);
    }
    NOTICE_LOG_FMT(NETPLAY, "Orca direct link: seat {}: {} link {} ({}{}{}{})", peer.slot + 1,
                   peer.low ? "offering" : "answering", peer.generation, reason,
                   config.turn_servers_count > 0 ? ", TURN" : "",
                   options.force.empty() ? "" : ", forced " + options.force,
                   options.net == Orca::Sandbox::NetMode::All ?
                       std::string() :
                       fmt::format(", sandbox {}", Orca::Sandbox::NetModeName(options.net)));
    picojson::object d;
    d["k"] = picojson::value("dl");
    if (config.turn_servers_count > 0)
      d["t"] = picojson::value(1.0);
    d["v"] = picojson::value(1.0);
    d["g"] = picojson::value(static_cast<double>(peer.generation));
    d["key"] = picojson::value(Hex(peer.my_half.data(), peer.my_half.size()));
    d["desc"] = picojson::value(mine);
    Signal(peer, std::move(d));
    peer.last_trickle = Clock::now();
  }

  // A new link when the rules allow one now (`now` skips the backoff; tests).
  void Rebuild(Peer& peer, const std::string& reason, bool now)
  {
    const auto t = Clock::now();
    if (!now && t < peer.next_rebuild)
      return;
    if (reason != "upgrade")
    {
      peer.next_rebuild = t + std::chrono::seconds(peer.backoff_s);
      peer.backoff_s = std::min(peer.backoff_s * 2, MAX_BACKOFF_S);
    }
    if (peer.low)
    {
      Build(peer, reason, nullptr);
      return;
    }
    INFO_LOG_FMT(NETPLAY, "Orca direct link: seat {}: asking for a new link ({})", peer.slot + 1,
                 reason);
    peer.reason = reason;
    picojson::object d;
    d["k"] = picojson::value("dlr");
    d["g"] = picojson::value(static_cast<double>(peer.generation));
    d["t"] = picojson::value(peer.with_turn ? 1.0 : 0.0);
    Signal(peer, std::move(d));
  }

  bool RelayHeardSince(const std::string& id, Clock::time_point since)
  {
    std::lock_guard lock(cmd_mutex);
    const auto it = relay_heard.find(id);
    return it != relay_heard.end() && it->second >= since;
  }

  void Timers()
  {
    const auto now = Clock::now();
    for (auto& [id, peer] : peers)
    {
      // Send candidates in batches.
      if ((!peer.trickle.empty() || peer.trickle_done) && peer.link &&
          now - peer.last_trickle >= TRICKLE_EVERY)
      {
        picojson::object d;
        d["k"] = picojson::value("dlc");
        d["g"] = picojson::value(static_cast<double>(peer.generation));
        picojson::array c;
        while (!peer.trickle.empty() && c.size() < MAX_CANDIDATES_IN_MESSAGE)
        {
          c.emplace_back(peer.trickle.front());
          peer.trickle.erase(peer.trickle.begin());
        }
        if (!c.empty())
          d["c"] = picojson::value(c);
        if (peer.trickle.empty() && peer.trickle_done)
        {
          d["done"] = picojson::value(true);
          peer.trickle_done = false;
        }
        Signal(peer, std::move(d));
        peer.last_trickle = now;
      }

      // Up and down, judged from the datagrams themselves. Up: datagrams arrive, and this side has
      // a pair to send on.
      bool up = false;
      if (peer.link && peer.link->Selected())
      {
        std::lock_guard lock(peer.link->m);
        up = peer.link->last_rx && now - *peer.link->last_rx < UP_WINDOW;
      }
      if (up && peer.pair.empty())
        ReadPair(peer);
      if (up != peer.was_up)
      {
        peer.was_up = up;
        if (up)
        {
          peer.ever_up = true;
          peer.up_since = now;
          int rtt;
          {
            std::lock_guard lock(peer.link->m);
            rtt = peer.link->MedianRttMs();
          }
          NOTICE_LOG_FMT(NETPLAY, "Orca direct link: seat {} up, {} {}, round trip {} (link {})",
                         peer.slot + 1, peer.turn ? "TURN" : "direct",
                         peer.pair.empty() ? "?" : peer.pair,
                         rtt >= 0 ? fmt::format("{} ms", rtt) : "not measured yet",
                         peer.generation);
        }
        else
        {
          peer.up_since.reset();
          u64 refused = 0;
          if (peer.link)
          {
            std::lock_guard lock(peer.link->m);
            refused = peer.link->auth_drops + peer.link->replays;
          }
          NOTICE_LOG_FMT(NETPLAY,
                         "Orca direct link: seat {} down (nothing for {} ms; link {}, {} refused), "
                         "inputs go through the relay",
                         peer.slot + 1, UP_WINDOW.count(), peer.generation, refused);
        }
      }
      if (up && peer.up_since && now - *peer.up_since > STABLE_AFTER)
        peer.backoff_s = FIRST_BACKOFF_S;

      // Keep an idle link warm (and its round trip measured).
      if (peer.link && peer.link->Selected())
      {
        bool idle;
        {
          std::lock_guard lock(peer.link->m);
          idle = now - peer.link->last_tx >= KEEPALIVE_EVERY;
        }
        if (idle)
          Send("", KIND_KEEPALIVE, peer.link.get());
      }

      // When to build anew.
      if (!peer.pending_build.empty())
      {
        // A copy: Build clears pending_build before it uses the reason.
        if (now - peer.built_at >= 500ms)
          Build(peer, std::string(peer.pending_build), nullptr);
        continue;
      }
      std::string reason;
      const int state = peer.link ? peer.link->state.load() : -1;
      const bool connected = peer.link && peer.link->Selected();
      if (!peer.link)
      {
        if (now - std::max(peer.built_at, peer.added_at) > ANSWER_TIMEOUT)
          reason = peer.low ? "retry" : "no offer";
      }
      else if (state == JUICE_STATE_FAILED)
      {
        reason = "failed";
      }
      else if (peer.low && !peer.answered && now - peer.built_at > ANSWER_TIMEOUT)
      {
        reason = "no answer";
      }
      else if (peer.answered && !connected && now - peer.answered_at > CONNECT_TIMEOUT)
      {
        reason = "no path";
      }
      else if (peer.ever_up && !up && RelayHeardSince(id, now - RELAY_FRESH))
      {
        std::optional<Clock::time_point> last_rx;
        {
          std::lock_guard lock(peer.link->m);
          last_rx = peer.link->last_rx;
        }
        if (!last_rx || now - *last_rx > SILENT_LIMIT)
          reason = "silent";
      }
      else if (connected && !peer.upgrade_tried && peer.connected_at &&
               now - *peer.connected_at > UPGRADE_AFTER &&
               (peer.turn || peer.pair.ends_with("/srflx")) && options.force.empty() &&
               Orca::Sandbox::LanAllowed(options.net) &&
               OneLan(peer.my_hosts, peer.their_hosts, peer.my_srflx, peer.their_srflx))
      {
        reason = "upgrade";
        peer.upgrade_tried = true;
      }
      // A link that never came up retries with TURN (a strict NAT on either side). One that worked
      // and then failed or went quiet keeps its previous setup. An upgrade to the LAN goes without
      // TURN.
      if ((reason == "failed" || reason == "no path") && !peer.ever_up && !peer.with_turn &&
          GetEnv("ORCA_DIRECT_TURN") != "0" && !options.ice.turn_user.empty())
      {
        peer.with_turn = true;
        NOTICE_LOG_FMT(NETPLAY, "Orca direct link: seat {}: {} without TURN; the next link has it",
                       peer.slot + 1, reason);
      }
      if (reason == "upgrade")
        peer.with_turn = false;
      if (!reason.empty())
        Rebuild(peer, reason, reason == "upgrade");
    }
  }

  void PublishStats()
  {
    const auto now = Clock::now();
    std::map<std::string, DirectStats> out;
    for (auto& [id, peer] : peers)
    {
      DirectStats s;
      s.generation = peer.generation;
      s.pair = peer.pair;
      s.builds = peer.builds;
      s.reason = peer.reason;
      if (peer.link)
      {
        const bool selected = peer.link->Selected();
        std::lock_guard lock(peer.link->m);
        const bool up = selected && peer.link->last_rx && now - *peer.link->last_rx < UP_WINDOW;
        s.kind = !up ? LinkKind::Relay : peer.turn ? LinkKind::Turn : LinkKind::Direct;
        s.rtt_ms = peer.link->MedianRttMs();
        s.sent = peer.link->sent;
        s.received = peer.link->received;
        s.auth_drops = peer.link->auth_drops;
        s.replays = peer.link->replays;
        s.floods = peer.link->flood_drops.load();
        s.max_send_us = peer.link->max_send_us;
      }
      out[id] = std::move(s);
    }
    std::lock_guard lock(cmd_mutex);
    stats = std::move(out);
  }
};

DirectLink::DirectLink() : m_impl(std::make_unique<Impl>())
{
}

DirectLink::~DirectLink() = default;

void DirectLink::Start(DirectOptions options)
{
  if (m_impl->started.exchange(true))
    return;
  m_impl->options = std::move(options);
  if (m_impl->options.blocked)
  {
    m_impl->block_in = true;
    m_impl->block_out = true;
  }
  m_impl->thread = std::thread([impl = m_impl.get()] { impl->Run(); });
}

void DirectLink::AddPeer(const std::string& id, int slot)
{
  {
    std::lock_guard lock(m_impl->cmd_mutex);
    m_impl->stats.try_emplace(id);
  }
  m_impl->Post({Impl::Command::Kind::Add, id, slot, {}, {}, {}});
}

void DirectLink::RemovePeer(const std::string& id)
{
  {
    // Nothing from it reaches Receive from now on, whatever the link thread is doing.
    std::lock_guard lock(m_impl->inbox_mutex);
    ++m_impl->epochs[id];
    std::erase_if(m_impl->inbox, [&id](const Received& in) { return in.from == id; });
  }
  {
    std::lock_guard lock(m_impl->cmd_mutex);
    m_impl->stats.erase(id);
  }
  m_impl->Post({Impl::Command::Kind::Remove, id, -1, {}, {}, {}});
}

void DirectLink::OnSignal(const std::string& id, const picojson::object& message)
{
  m_impl->Post({Impl::Command::Kind::Signal, id, -1, message, {}, {}});
}

std::vector<std::pair<std::string, picojson::object>> DirectLink::TakeSignals()
{
  std::lock_guard lock(m_impl->cmd_mutex);
  return std::exchange(m_impl->signals, {});
}

void DirectLink::OnRelayPacket(const std::string& id)
{
  std::lock_guard lock(m_impl->cmd_mutex);
  m_impl->relay_heard[id] = Clock::now();
}

void DirectLink::SetIce(const IceConfig& ice)
{
  m_impl->Post({Impl::Command::Kind::Ice, {}, -1, {}, ice, {}});
}

void DirectLink::Send(const std::string& payload)
{
  m_impl->Send(payload);
}

std::vector<DirectLink::Received> DirectLink::Receive()
{
  return m_impl->Receive();
}

std::optional<DirectStats> DirectLink::Stats(const std::string& id) const
{
  std::lock_guard lock(m_impl->cmd_mutex);
  const auto it = m_impl->stats.find(id);
  if (it == m_impl->stats.end())
    return std::nullopt;
  return it->second;
}

void DirectLink::Test(const std::string& command)
{
  if (command == "off" || command == "on" || command == "in" || command == "out")
  {
    m_impl->block_in = command == "off" || command == "in";
    m_impl->block_out = command == "off" || command == "out";
    WARN_LOG_FMT(NETPLAY, "Orca direct link: TEST {}", command == "on" ? "datagrams flow" :
                                                           command == "off" ?
                                                                              "dropping every datagram" :
                                                           command == "in" ?
                                                                              "dropping incoming datagrams" :
                                                                              "dropping outgoing datagrams");
  }
  else if (command == "rebuild")
  {
    m_impl->Post({Impl::Command::Kind::Test, {}, -1, {}, {}, command});
  }
}

void DirectLink::Stop()
{
  m_impl->Stop();
}
}  // namespace Orca::Net
