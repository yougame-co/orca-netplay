// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Orca/Sandbox.h"

// Direct links between the Orcas in a room: one UDP path per pair of players alongside the room's
// relay, so inputs go straight to the other machine instead of through Cloudflare and the room's
// Durable Object.
//
// Like Slippi, the data path is plain UDP with inputs repeated until acknowledged. Orca connects
// with ICE (libjuice): host candidates on every interface, the public address from STUN, and
// Cloudflare TURN from the room ticket as a last resort. Signalling rides the room as peer
// messages. Every datagram is sealed with ChaCha20-Poly1305 under keys both players contribute to.
// The relay keeps carrying every packet too and the first copy to arrive wins (the session handles
// duplicates), so a link that fails or never opens costs nothing beyond the relay's latency. See
// ORCA.md, "Direct links".
namespace Orca::Net
{
// ICE servers from a room ticket's `ice`: one STUN server and one TURN server over UDP. No TURN
// over TCP or TLS: the room's relay already covers networks that block UDP.
struct IceConfig
{
  std::string stun_host = "stun.cloudflare.com";
  u16 stun_port = 3478;
  // Empty: no TURN.
  std::string turn_host;
  u16 turn_port = 3478;
  std::string turn_user;
  std::string turn_pass;
  bool operator==(const IceConfig&) const = default;
};

// Parses the ticket's `ice` array: [{urls: string or [string], username?, credential?}]. Takes the
// first stun: URL and the first UDP turn: URL, preferring port 3478 to 53. False when `ice` isn't
// an array (the config keeps its defaults).
bool ParseIce(const picojson::value& ice, IceConfig* out);

// How a peer's inputs reach this machine.
enum class LinkKind
{
  Relay = 0,   // only through the room
  Direct = 1,  // a direct path (host, server- or peer-reflexive candidates)
  Turn = 2,    // a direct link through a TURN server
};

struct DirectStats
{
  LinkKind kind = LinkKind::Relay;
  // Median of recent round trips measured on the link's own datagrams (-1: none yet).
  int rtt_ms = -1;
  // Candidate types of the selected pair, local/remote ("host/host"; "" until selected).
  std::string pair;
  std::string test_local_address;
  int generation = 0;
  u64 sent = 0;
  u64 received = 0;
  // Datagrams that failed authentication or were replays, plus authentic packets dropped for
  // arriving faster than an honest peer sends.
  u64 auth_drops = 0;
  u64 replays = 0;
  u64 floods = 0;
  // The longest a send took on the caller's thread, in microseconds.
  int max_send_us = 0;
  // Links built for this peer, and the latest reason ("new", "failed", "silent", "upgrade", ...).
  int builds = 0;
  std::string reason;
};

struct DirectOptions
{
  // This Orca's connection id in the room and the room code; both are bound into the link keys.
  std::string me;
  std::string room;
  IceConfig ice;
  // When `ice` was minted, and how to mint fresh credentials (blocking; runs on the link's thread).
  // TURN credentials last 30 minutes, and a friend may arrive long after the room opened.
  std::chrono::steady_clock::time_point ice_minted = std::chrono::steady_clock::now();
  std::function<std::optional<IceConfig>()> fresh_ice;

  // Test knobs (ORCA_DIRECT_*): use only relay candidates ("turn") or no host candidates ("srflx");
  // also offer loopback candidates (two Orcas on one machine); drop a percentage of sent datagrams;
  // start with every datagram dropped (as `Test("off")`).
  std::string force;
  bool loopback = false;
  int loss_percent = 0;
  bool blocked = false;
  // The network the app's sandbox gives Orca (ORCA_NET, Orca/Sandbox.h): without the LAN, no LAN
  // host candidate is offered or tried and there is no upgrade to the LAN; never loopback.
  Orca::Sandbox::NetMode net = Orca::Sandbox::NetMode::All;
  // ORCA_TEST_NET_DELAY_MS / ORCA_TEST_NET_JITTER_MS, applied as on the relay: every datagram waits
  // this long (plus 0..jitter) on the way out and again on the way in.
  std::optional<int> net_delay_ms;
  int net_jitter_ms = 0;
  // Scheduled packet drops, sharing the room's clock; independent of delay and test-direct.
  std::optional<std::pair<std::chrono::steady_clock::time_point,
                          std::chrono::steady_clock::time_point>> blackout;
};

// Options from the environment; nullopt when ORCA_DIRECT=0 (relay only).
std::optional<DirectOptions> DirectOptionsFromEnv();

class DirectLink
{
public:
  // A received packet: the sender's connection id and seat, and the payload as sent.
  struct Received
  {
    std::string from;
    int slot = -1;
    std::string payload;
  };

  DirectLink();
  ~DirectLink();
  DirectLink(const DirectLink&) = delete;
  DirectLink& operator=(const DirectLink&) = delete;

  // ---- The room's I/O thread (none of these block) ----

  // Starts the link thread. It binds a throwaway UDP socket at once, so a firewall prompt (Windows
  // Defender Firewall) appears now, in the lobby, rather than when a friend arrives mid-game.
  // False when file playback disables networking, or the thread is already started.
  bool Start(DirectOptions options);
  // A player of this game (it said hello with our compatibility key and supports direct links). The
  // lower connection id of the pair offers; the other answers.
  void AddPeer(const std::string& id, int slot);
  // The player left or its connection was replaced: drop its link and unread packets.
  void RemovePeer(const std::string& id);
  // A signalling message (`dl`, `dlc`, `dlr`) from a peer already added.
  void OnSignal(const std::string& id, const picojson::object& message);
  // Messages to send to peers through the room, as (to, message).
  std::vector<std::pair<std::string, picojson::object>> TakeSignals();
  // The peer's packets still arrive through the relay, so a silent link is worth rebuilding.
  void OnRelayPacket(const std::string& id);
  // Fresh ICE servers (from a new ticket), for links built from now on.
  void SetIce(const IceConfig& ice);

  // ---- Any thread ----

  // Sends to every peer with an open link, or just `seat` when nonnegative, immediately on the
  // caller's thread (never takes the room's locks). At most MAX_PAYLOAD bytes.
  void Send(const std::string& payload, int seat = -1);
  // Packets received since the last call.
  std::vector<Received> Receive();
  // How each peer's link is doing (nullopt: not a peer).
  std::optional<DirectStats> Stats(const std::string& id) const;
  // Tests (`test-direct off|on|in|out|rebuild`): drop all, incoming or outgoing datagrams, or none;
  // or rebuild every link now.
  void Test(const std::string& command);
  // Closes every link and stops the thread (the destructor does too). Send and Receive do nothing
  // afterwards.
  void Stop();

  // At most 1200 bytes on the wire: IPv6's minimum MTU of 1280 less IP, UDP and TURN channel
  // headers. The payload limit subtracts the header and tag.
  static constexpr size_t MAX_DATAGRAM = 1200;
  static constexpr size_t HEADER_SIZE = 18;
  static constexpr size_t TAG_SIZE = 16;
  static constexpr size_t MAX_PAYLOAD = MAX_DATAGRAM - HEADER_SIZE - TAG_SIZE;

  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
};

// The ICE lines of a description libjuice wrote, without a candidate `net` can't reach (tests).
std::string DescriptionFor(std::string_view description, Orca::Sandbox::NetMode net);

// Exposed for tests: whether two machines are on one LAN, checked before the "upgrade" rule trades
// a working server-reflexive or TURN link for their LAN addresses. A shared host subnet (IPv4 /24
// or IPv6 /64) isn't enough, since many homes use the same router subnet; they must also share a
// public address seen by STUN (any port), which only machines behind one NAT do.
bool OneLan(const std::vector<std::string>& my_hosts, const std::vector<std::string>& their_hosts,
            const std::vector<std::string>& my_srflx, const std::vector<std::string>& their_srflx);

// Exposed for tests: link keys and datagram sealing.
namespace DirectCrypto
{
using Key = std::array<u8, 32>;
// From each side's contribution and connection id, the room code and the link's generation. The
// first key seals what the lower id sends, the second what the higher id sends.
std::pair<Key, Key> DeriveKeys(const Key& low_half, const Key& high_half, const std::string& room,
                               u16 generation, const std::string& low_id,
                               const std::string& high_id);

// A datagram's header (layout in DirectLink.cpp). `counter` is the nonce: starts at 1, never reused
// under one key. `echo` and `hold`: the newest counter received from the peer and how long ago, in
// 0.1 ms, which gives the peer its round trip.
struct Header
{
  u8 kind = 1;
  u16 generation = 0;
  u64 counter = 0;
  u32 echo = 0;
  u16 hold = 0;
};
// The sealed datagram; empty if the payload is too large or the counter is 0.
std::vector<u8> Seal(const Key& key, const Header& header, std::string_view payload);
// Opens a datagram sealed with `key` and fills `header`; nullopt if it isn't one.
std::optional<std::string> Open(const Key& key, const u8* data, size_t size, Header* header);
// Reads the header without authenticating (false: not an Orca datagram).
bool PeekHeader(const u8* data, size_t size, Header* header);
// Counters received: the newest and the 63 before it. Anything older or already seen is a replay.
struct ReplayWindow
{
  u64 top = 0;
  u64 seen = 0;
  bool Fresh(u64 counter) const;
  void Take(u64 counter);
};
}  // namespace DirectCrypto
}  // namespace Orca::Net
