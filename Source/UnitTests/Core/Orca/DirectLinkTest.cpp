// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/Timer.h"
#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/YouGameRoom.h"

using namespace Orca::Net;
using namespace std::chrono_literals;

namespace
{
DirectCrypto::Key KeyOf(u8 byte)
{
  DirectCrypto::Key key;
  key.fill(byte);
  return key;
}

picojson::value Json(const char* text)
{
  picojson::value value;
  EXPECT_TRUE(picojson::parse(value, text).empty()) << text;
  return value;
}

// Orcas in this process, each with its DirectLink, signalling through the test as the room carries
// it (peer messages, in order). No STUN, no TURN: nothing leaves this machine. Loopback candidates
// too, in case the host's LAN addresses refuse a send to themselves.
class Mesh
{
public:
  struct Node
  {
    std::string id;
    int slot;
    std::unique_ptr<DirectLink> link;
    std::vector<DirectLink::Received> received;
  };

  explicit Mesh(int count, const std::function<void(DirectOptions&, int)>& tweak = {})
  {
    for (int i = 0; i < count; ++i)
    {
      Node node{std::string(1, static_cast<char>('a' + i)) + "-connection", i,
                std::make_unique<DirectLink>(), {}};
      DirectOptions options;
      options.me = node.id;
      options.room = "testroom";
      options.ice.stun_host.clear();
      options.loopback = true;
      if (tweak)
        tweak(options, i);
      node.link->Start(std::move(options));
      m_nodes.push_back(std::move(node));
    }
    for (const Node& a : m_nodes)
    {
      for (const Node& b : m_nodes)
      {
        if (a.id != b.id)
          a.link->AddPeer(b.id, b.slot);
      }
    }
  }

  Node& At(int i) { return m_nodes[static_cast<size_t>(i)]; }

  // Signalling from each node to the others, and what each received.
  void Pump()
  {
    for (Node& from : m_nodes)
    {
      for (auto& [to, message] : from.link->TakeSignals())
      {
        m_signals.push_back(message);
        for (Node& node : m_nodes)
        {
          if (node.id == to)
            node.link->OnSignal(from.id, message);
        }
      }
      for (DirectLink::Received& r : from.link->Receive())
        from.received.push_back(std::move(r));
    }
  }

  bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s)
  {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until)
    {
      Pump();
      if (done())
        return true;
      std::this_thread::sleep_for(5ms);
    }
    Pump();
    return done();
  }

  // How node `i` sees node `j`'s link.
  DirectStats Stats(int i, int j) { return At(i).link->Stats(At(j).id).value_or(DirectStats{}); }
  bool Up(int i, int j) { return Stats(i, j).kind != LinkKind::Relay; }
  bool AllUp()
  {
    for (size_t i = 0; i < m_nodes.size(); ++i)
    {
      for (size_t j = 0; j < m_nodes.size(); ++j)
      {
        if (i != j && !Up(static_cast<int>(i), static_cast<int>(j)))
          return false;
      }
    }
    return true;
  }

  const std::vector<picojson::object>& Signals() const { return m_signals; }

private:
  std::vector<Node> m_nodes;
  std::vector<picojson::object> m_signals;
};

int Count(const std::vector<DirectLink::Received>& received, const std::string& payload)
{
  return static_cast<int>(std::count_if(received.begin(), received.end(),
                                        [&](const auto& r) { return r.payload == payload; }));
}
}  // namespace

TEST(OrcaDirectLink, ParsesTheTicketsIceServers)
{
  // As YouGame's ticket route hands out Cloudflare's (web/src/lib/turn.ts).
  IceConfig c;
  ASSERT_TRUE(ParseIce(Json(R"([{"urls":["stun:stun.cloudflare.com:3478",
      "turn:turn.cloudflare.com:3478?transport=udp","turn:turn.cloudflare.com:3478?transport=tcp",
      "turns:turn.cloudflare.com:5349?transport=tcp"],"username":"u1","credential":"p1"}])"),
                       &c));
  EXPECT_EQ(c.stun_host, "stun.cloudflare.com");
  EXPECT_EQ(c.stun_port, 3478);
  EXPECT_EQ(c.turn_host, "turn.cloudflare.com");
  EXPECT_EQ(c.turn_port, 3478);
  EXPECT_EQ(c.turn_user, "u1");
  EXPECT_EQ(c.turn_pass, "p1");

  // `urls` as one string; TURN on 53 when that is all there is.
  IceConfig d;
  ASSERT_TRUE(ParseIce(Json(R"([{"urls":"stun:s.example:19302"},
      {"urls":"turn:t.example:53?transport=udp","username":"x","credential":"y"}])"),
                       &d));
  EXPECT_EQ(d.stun_host, "s.example");
  EXPECT_EQ(d.stun_port, 19302);
  EXPECT_EQ(d.turn_host, "t.example");
  EXPECT_EQ(d.turn_port, 53);

  // TURN over TCP or TLS only, or without credentials: no TURN (the room's relay covers that).
  IceConfig e;
  ASSERT_TRUE(ParseIce(Json(R"([{"urls":["turn:t.example:3478?transport=tcp",
      "turns:t.example:5349"],"username":"x","credential":"y"},{"urls":"turn:t.example"}])"),
                       &e));
  EXPECT_TRUE(e.turn_host.empty());
  EXPECT_EQ(e.stun_host, "stun.cloudflare.com");

  IceConfig f;
  EXPECT_FALSE(ParseIce(picojson::value(1.0), &f));
  EXPECT_FALSE(ParseIce(picojson::value(), &f));
}

TEST(OrcaDirectLink, KeysDifferPerDirectionGenerationRoomAndPair)
{
  const auto base = DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 1, "a", "b");
  EXPECT_NE(base.first, base.second);
  EXPECT_EQ(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 1, "a", "b"), base);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 2, "a", "b").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room2", 1, "a", "b").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(2), KeyOf(1), "room", 1, "a", "b").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 1, "a", "c").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(3), "room", 1, "a", "b").first, base.first);
}

TEST(OrcaDirectLink, OneLanNeedsASharedPublicAddress)
{
  // Two homes on the same router default (192.168.1.x): the same /24, but STUN saw two public
  // addresses. Not one LAN: no "upgrade" teardown of their working cross-home link.
  const std::vector<std::string> mine{"192.168.1.20"}, theirs{"192.168.1.35"};
  EXPECT_FALSE(OneLan(mine, theirs, {"203.0.113.7"}, {"198.51.100.40"}));
  // Behind one NAT: one public address, each on a port of its own (and maybe more than one).
  EXPECT_TRUE(OneLan(mine, theirs, {"203.0.113.7"}, {"198.51.100.40", "203.0.113.7"}));
  EXPECT_TRUE(OneLan(mine, theirs, {"203.0.113.7"}, {"203.0.113.7"}));
  // A side STUN never answered: no proof, no upgrade.
  EXPECT_FALSE(OneLan(mine, theirs, {}, {"203.0.113.7"}));
  EXPECT_FALSE(OneLan(mine, theirs, {"203.0.113.7"}, {}));
  // One public address, but no subnet in common (a guest network behind the same NAT).
  EXPECT_FALSE(OneLan({"192.168.1.20"}, {"192.168.2.35"}, {"203.0.113.7"}, {"203.0.113.7"}));
  // IPv6: the same /64 counts as a subnet.
  EXPECT_TRUE(OneLan({"2001:db8:1:2::10"}, {"2001:db8:1:2::20"}, {"203.0.113.7"},
                     {"203.0.113.7"}));
  EXPECT_FALSE(OneLan({"2001:db8:1:2::10"}, {"2001:db8:1:3::20"}, {"203.0.113.7"},
                      {"203.0.113.7"}));
}

TEST(OrcaDirectLink, DatagramsAuthenticateAndRefuseReplays)
{
  const DirectCrypto::Key key = KeyOf(7);
  DirectCrypto::Header header;
  header.kind = 1;
  header.generation = 3;
  header.counter = 9;
  header.echo = 5;
  header.hold = 12;
  const std::vector<u8> datagram = DirectCrypto::Seal(key, header, "inputs for frame 120");
  ASSERT_EQ(datagram.size(), DirectLink::HEADER_SIZE + 20 + DirectLink::TAG_SIZE);
  DirectCrypto::Header got;
  const auto payload = DirectCrypto::Open(key, datagram.data(), datagram.size(), &got);
  ASSERT_TRUE(payload);
  EXPECT_EQ(*payload, "inputs for frame 120");
  EXPECT_EQ(got.kind, 1);
  EXPECT_EQ(got.generation, 3);
  EXPECT_EQ(got.counter, 9u);
  EXPECT_EQ(got.echo, 5u);
  EXPECT_EQ(got.hold, 12);
  // Unreadable on the wire.
  const std::string clear = "inputs";
  EXPECT_EQ(std::search(datagram.begin(), datagram.end(), clear.begin(), clear.end()),
            datagram.end());
  // Another key (another pair, direction or generation): refused.
  EXPECT_FALSE(DirectCrypto::Open(KeyOf(8), datagram.data(), datagram.size(), &got));
  // Any byte changed, the header included: refused.
  for (size_t i = 0; i < datagram.size(); ++i)
  {
    std::vector<u8> tampered = datagram;
    tampered[i] ^= 0x10;
    EXPECT_FALSE(DirectCrypto::Open(key, tampered.data(), tampered.size(), &got)) << "byte " << i;
  }
  // Cut short, or empty.
  EXPECT_FALSE(DirectCrypto::Open(key, datagram.data(), datagram.size() - 1, &got));
  EXPECT_FALSE(DirectCrypto::Open(key, datagram.data(), 0, &got));
  // Counter 0 and payloads past one datagram are never sealed.
  header.counter = 0;
  EXPECT_TRUE(DirectCrypto::Seal(key, header, "x").empty());
  header.counter = 10;
  EXPECT_TRUE(DirectCrypto::Seal(key, header, std::string(DirectLink::MAX_PAYLOAD + 1, 'x')).empty());
  EXPECT_EQ(DirectCrypto::Seal(key, header, std::string(DirectLink::MAX_PAYLOAD, 'x')).size(),
            DirectLink::MAX_DATAGRAM);

  // The replay window: each counter once, out of order within the last 64.
  DirectCrypto::ReplayWindow window;
  EXPECT_FALSE(window.Fresh(0));
  EXPECT_TRUE(window.Fresh(1));
  window.Take(1);
  EXPECT_FALSE(window.Fresh(1));
  window.Take(5);
  EXPECT_TRUE(window.Fresh(3));
  window.Take(3);
  EXPECT_FALSE(window.Fresh(3));
  EXPECT_TRUE(window.Fresh(4));
  window.Take(100);
  EXPECT_FALSE(window.Fresh(100));
  EXPECT_FALSE(window.Fresh(36));
  EXPECT_TRUE(window.Fresh(37));
  EXPECT_TRUE(window.Fresh(99));
}

TEST(OrcaDirectLink, DirectPacketsOnlyForTheSendersSeat)
{
  Packet packet;
  packet.seat = 1;
  packet.sequence = 4;
  packet.first_frame = 10;
  packet.pads = {Pad{1, 2, 3, 4, 5, 6, 7, 8}};
  packet.current_frame = 11;
  Packet got;
  ASSERT_TRUE(DecodeDirectPacket(EncodePacket(packet), 1, &got));
  EXPECT_EQ(got.sequence, 4);
  EXPECT_EQ(got.pads, packet.pads);
  // A friend's link speaks for its own seat only.
  EXPECT_FALSE(DecodeDirectPacket(EncodePacket(packet), 2, &got));
  // History travels through the relay alone.
  packet.history_seat = 2;
  packet.history_first = 0;
  packet.history.resize(1);
  EXPECT_FALSE(DecodeDirectPacket(EncodePacket(packet), 1, &got));
  EXPECT_FALSE(DecodeDirectPacket("{not json", 1, &got));
}

// Two Orcas link over this machine: the lower id offers, the link comes up on host candidates,
// packets go both ways, the round trip is measured from the datagrams, and three rebuilds each
// bring a new generation back up with nothing refused.
TEST(OrcaDirectLink, TwoOrcasLinkAndRebuild)
{
  Mesh mesh(2);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); })) << mesh.Stats(0, 1).reason;
  EXPECT_EQ(mesh.Stats(0, 1).kind, LinkKind::Direct);
  // A host or loopback path (libjuice may name it peer-reflexive), never a relayed one.
  EXPECT_EQ(mesh.Stats(0, 1).pair.find("relay"), std::string::npos) << mesh.Stats(0, 1).pair;
  std::printf("[ pair ] %s / %s\n", mesh.Stats(0, 1).pair.c_str(), mesh.Stats(1, 0).pair.c_str());
  EXPECT_EQ(mesh.Stats(0, 1).generation, 1);
  EXPECT_EQ(mesh.Stats(1, 0).generation, 1);
  // The offer came from the lower id, and carried no ICE server credentials.
  ASSERT_FALSE(mesh.Signals().empty());
  EXPECT_EQ(mesh.Signals().front().at("k").get<std::string>(), "dl");

  mesh.At(0).link->Send("from a");
  mesh.At(1).link->Send("from b");
  ASSERT_TRUE(mesh.WaitFor([&] {
    return Count(mesh.At(1).received, "from a") == 1 && Count(mesh.At(0).received, "from b") == 1;
  }));
  EXPECT_EQ(mesh.At(1).received.front().from, mesh.At(0).id);
  EXPECT_EQ(mesh.At(1).received.front().slot, 0);
  EXPECT_EQ(mesh.At(0).received.front().slot, 1);
  // Keepalives echo: a round trip of about nothing on one machine.
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.Stats(0, 1).rtt_ms >= 0; }, 3s));
  EXPECT_LE(mesh.Stats(0, 1).rtt_ms, 20);

  for (int i = 0; i < 3; ++i)
  {
    // A request from the answering side waits out the offering side's second between builds.
    std::this_thread::sleep_for(1100ms);
    const int before = mesh.Stats(0, 1).generation;
    mesh.At(i % 2).link->Test("rebuild");
    ASSERT_TRUE(mesh.WaitFor([&] {
      return mesh.Stats(0, 1).generation > before && mesh.Stats(1, 0).generation > before &&
             mesh.AllUp();
    })) << "rebuild " << i;
    const std::string payload = "after rebuild " + std::to_string(i);
    mesh.At(0).link->Send(payload);
    ASSERT_TRUE(mesh.WaitFor([&] { return Count(mesh.At(1).received, payload) == 1; }));
  }
  for (int i = 0; i < 2; ++i)
  {
    EXPECT_EQ(mesh.Stats(i, 1 - i).auth_drops, 0u);
    EXPECT_EQ(mesh.Stats(i, 1 - i).replays, 0u);
  }
}

// A networking fix for Windows.
TEST(OrcaDirectLink, AnOversizedDatagramLeavesTheLinkUp)
{
  Mesh mesh(2);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); })) << mesh.Stats(1, 0).reason;
  mesh.At(0).link->Send("before from a");
  mesh.At(1).link->Send("before from b");
  ASSERT_TRUE(mesh.WaitFor([&] {
    return Count(mesh.At(1).received, "before from a") == 1 &&
           Count(mesh.At(0).received, "before from b") == 1;
  }));
  const std::array<int, 2> generations{mesh.Stats(0, 1).generation,
                                      mesh.Stats(1, 0).generation};
  const std::string local = mesh.Stats(1, 0).test_local_address;
  const auto colon = local.rfind(':');
  ASSERT_NE(colon, std::string::npos) << local;
  unsigned short port = 0;
  const auto parsed = std::from_chars(local.data() + colon + 1, local.data() + local.size(), port);
  ASSERT_EQ(parsed.ec, std::errc{});
  ASSERT_EQ(parsed.ptr, local.data() + local.size());
  ASSERT_NE(port, 0);

#ifdef _WIN32
  WSADATA data{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
  struct UdpSocket
  {
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ~UdpSocket()
    {
      if (fd != INVALID_SOCKET)
        closesocket(fd);
      WSACleanup();
    }
  } udp;
  ASSERT_NE(udp.fd, INVALID_SOCKET);
#else
  struct UdpSocket
  {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ~UdpSocket()
    {
      if (fd >= 0)
        close(fd);
    }
  } udp;
  ASSERT_GE(udp.fd, 0);
#endif
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  destination.sin_port = htons(port);
  const std::array<char, 9000> junk{};
  for (const int size : {5000, 5000, 5000, 9000})
  {
    ASSERT_EQ(sendto(udp.fd, junk.data(), size, 0,
                     reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)),
              size);
  }

  const auto until = std::chrono::steady_clock::now() + 2s;
  do
  {
    mesh.Pump();
    ASSERT_EQ(mesh.Stats(0, 1).generation, generations[0]);
    ASSERT_EQ(mesh.Stats(1, 0).generation, generations[1]);
    ASSERT_EQ(mesh.Stats(1, 0).kind, LinkKind::Direct);
    ASSERT_TRUE(mesh.AllUp());
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < until);

  mesh.At(0).link->Send("after from a");
  mesh.At(1).link->Send("after from b");
  ASSERT_TRUE(mesh.WaitFor([&] {
    return Count(mesh.At(1).received, "after from a") == 1 &&
           Count(mesh.At(0).received, "after from b") == 1;
  }));
  EXPECT_EQ(mesh.Stats(0, 1).generation, generations[0]);
  EXPECT_EQ(mesh.Stats(1, 0).generation, generations[1]);
  EXPECT_EQ(mesh.Stats(0, 1).kind, LinkKind::Direct);
  EXPECT_EQ(mesh.Stats(1, 0).kind, LinkKind::Direct);
}

// Three Orcas: a link per pair, each packet to both others.
TEST(OrcaDirectLink, ThreeOrcasLinkEveryPair)
{
  Mesh mesh(3);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  mesh.At(1).link->Send("from b");
  ASSERT_TRUE(mesh.WaitFor([&] {
    return Count(mesh.At(0).received, "from b") == 1 && Count(mesh.At(2).received, "from b") == 1;
  }));
  EXPECT_EQ(Count(mesh.At(1).received, "from b"), 0);
  // The peer the link belongs to, never another.
  for (const auto& r : mesh.At(2).received)
    EXPECT_EQ(r.slot, r.from == mesh.At(0).id ? 0 : 1);
}

// Blocked datagrams (ORCA_DIRECT_BLOCK, `test-direct off`) leave the inputs on the relay until
// they flow again; relayed candidates only (ORCA_DIRECT_FORCE=turn) offer no host address at all,
// and with no TURN server here no link opens.
TEST(OrcaDirectLink, BlockedOrTurnOnlyStaysOnTheRelay)
{
  {
    Mesh mesh(2, [](DirectOptions& o, int i) { o.blocked = i == 0; });
    EXPECT_FALSE(mesh.WaitFor([&] { return mesh.Up(0, 1) || mesh.Up(1, 0); }, 1500ms));
    mesh.At(0).link->Test("on");
    ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
    mesh.At(0).link->Test("in");
    ASSERT_TRUE(mesh.WaitFor([&] { return !mesh.Up(0, 1) && mesh.Up(1, 0); }, 3s));
    mesh.At(0).link->Test("on");
    ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  }
  {
    Mesh mesh(2, [](DirectOptions& o, int) { o.force = "turn"; });
    EXPECT_FALSE(mesh.WaitFor([&] { return mesh.Up(0, 1) || mesh.Up(1, 0); }, 1500ms));
    for (const picojson::object& signal : mesh.Signals())
    {
      if (signal.count("desc"))
        EXPECT_EQ(signal.at("desc").get<std::string>().find("a=candidate"), std::string::npos);
    }
  }
}

// The test delay (ORCA_TEST_NET_DELAY_MS) holds each datagram on the way out and again on the way
// in, as the room does for relayed messages: 30 ms on both machines is 60 ms each way, a round
// trip of 120 ms on the link.
TEST(OrcaDirectLink, TestDelayCoversTheLinkBothWays)
{
  // The link thread releases held datagrams on its own wake-ups. Orca asks Windows for 1 ms timers
  // once emulation starts (SystemTimers); without that, each of the four holds can overshoot by a
  // 15.6 ms tick. A no-op elsewhere.
  Common::Timer::IncreaseResolution();
  struct Restore
  {
    ~Restore() { Common::Timer::RestoreResolution(); }
  } restore;
  Mesh mesh(2, [](DirectOptions& o, int) { o.net_delay_ms = 30; });
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  // The median of about ten keepalive round trips (one each 200 ms), not the first one alone: one
  // late wake-up on a busy machine is not the delay.
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.Stats(0, 1).received >= 12; }))
      << mesh.Stats(0, 1).received;
  const int rtt = mesh.Stats(0, 1).rtt_ms;
  // Under 110: the delay is missing on one way. Windows' ceiling leaves room for a test process
  // the PC runs at below-normal priority next to other work.
#ifdef _WIN32
  constexpr int CEILING_MS = 200;
#else
  constexpr int CEILING_MS = 160;
#endif
  EXPECT_GE(rtt, 110);
  EXPECT_LE(rtt, CEILING_MS);
}

// A peer sending far faster than an honest one (one a frame; while it stalls, one every 4 ms) gets
// a burst of 64 and 300 a second through to Receive, the rest dropped and counted (its packets
// still come through the relay); once it slows down, the link delivers everything again.
TEST(OrcaDirectLink, AFloodingPeerIsHeldToTheRate)
{
  Mesh mesh(2);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  const auto start = std::chrono::steady_clock::now();
  constexpr int SENT = 3000;
  for (int i = 0; i < SENT; ++i)
    mesh.At(0).link->Send("flood");
  mesh.WaitFor(
      [&] {
        return mesh.Stats(1, 0).floods > 0 && Count(mesh.At(1).received, "flood") >= 64;
      },
      2s);
  // Everything that made it through the sockets is in by now.
  std::this_thread::sleep_for(300ms);
  mesh.Pump();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const int got = Count(mesh.At(1).received, "flood");
  const u64 dropped = mesh.Stats(1, 0).floods;
  std::printf("[ flood ] %d sent, %d delivered, %llu dropped over the rate, in %.2f s\n", SENT, got,
              static_cast<unsigned long long>(dropped), seconds);
  EXPECT_GE(got, 64);
  EXPECT_LE(got, 64 + static_cast<int>(300 * seconds) + 1);
  EXPECT_GT(dropped, 0u);
  // An honest pace afterwards: every packet delivered.
  std::this_thread::sleep_for(300ms);
  for (int i = 0; i < 30; ++i)
  {
    mesh.At(0).link->Send("steady " + std::to_string(i));
    std::this_thread::sleep_for(16ms);
  }
  ASSERT_TRUE(mesh.WaitFor([&] {
    for (int i = 0; i < 30; ++i)
    {
      if (Count(mesh.At(1).received, "steady " + std::to_string(i)) != 1)
        return false;
    }
    return true;
  }, 3s));
  EXPECT_EQ(mesh.Stats(1, 0).floods, dropped);
}

// One side starts over while the other keeps its count (a seat change, or links turned off and on
// on one side only): the offering side's new link must go past the generation the answering side
// holds, and an answering side with none asks at once. Both come back up in a few seconds, not
// after the backoff climbs past the old count or libjuice's 30 s consent timeout.
TEST(OrcaDirectLink, OneSideStartingOverCatchesUp)
{
  Mesh mesh(2);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  // A few builds first: the answering side holds generation 4.
  for (int i = 0; i < 3; ++i)
  {
    std::this_thread::sleep_for(600ms);
    const int before = mesh.Stats(1, 0).generation;
    mesh.At(0).link->Test("rebuild");
    ASSERT_TRUE(mesh.WaitFor(
        [&] { return mesh.Stats(1, 0).generation > before && mesh.AllUp(); }));
  }
  const int held = mesh.Stats(1, 0).generation;
  ASSERT_EQ(held, 4);

  // The offering side starts over: its first offer (generation 1) is refused with the answering
  // side's generation, and its next one goes past it.
  auto start = std::chrono::steady_clock::now();
  mesh.At(0).link->RemovePeer(mesh.At(1).id);
  mesh.At(0).link->AddPeer(mesh.At(1).id, 1);
  ASSERT_TRUE(mesh.WaitFor([&] {
    return mesh.Stats(0, 1).generation > held && mesh.Stats(1, 0).generation > held &&
           mesh.AllUp();
  }, 4s))
      << mesh.Stats(0, 1).generation << " / " << mesh.Stats(1, 0).generation;
  std::printf("[ restart ] offering side: up again at link %d after %.1f s\n",
              mesh.Stats(0, 1).generation,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());

  // The answering side starts over: it has no link and no generation, asks (`dlr` 0) once its
  // wait for an offer runs out, and the offering side builds at once.
  std::this_thread::sleep_for(1100ms);
  const int before = mesh.Stats(0, 1).generation;
  start = std::chrono::steady_clock::now();
  mesh.At(1).link->RemovePeer(mesh.At(0).id);
  mesh.At(1).link->AddPeer(mesh.At(0).id, 0);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.Stats(0, 1).generation > before && mesh.AllUp(); },
                           9s))
      << mesh.Stats(0, 1).generation << " / " << mesh.Stats(1, 0).generation;
  std::printf("[ restart ] answering side: up again at link %d after %.1f s\n",
              mesh.Stats(0, 1).generation,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  mesh.At(0).link->Send("after both restarts");
  ASSERT_TRUE(mesh.WaitFor([&] { return Count(mesh.At(1).received, "after both restarts") == 1; }));
}

// Fresh ICE credentials that can't be minted (no ticket: the site is down) are not asked for again
// on every build, each of which would hold the link thread up to 10 s: one try, then a wait. The
// links meanwhile build with what they have.
TEST(OrcaDirectLink, AFailedIceMintBacksOff)
{
  std::array<std::atomic<int>, 2> mints{};
  Mesh mesh(2, [&mints](DirectOptions& o, int i) {
    o.ice_minted = std::chrono::steady_clock::now() - 11min;
    o.fresh_ice = [&mints, i]() -> std::optional<IceConfig> {
      ++mints[static_cast<size_t>(i)];
      return std::nullopt;
    };
  });
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  for (int i = 0; i < 3; ++i)
  {
    std::this_thread::sleep_for(1100ms);
    const int before = mesh.Stats(0, 1).generation;
    mesh.At(i % 2).link->Test("rebuild");
    ASSERT_TRUE(mesh.WaitFor([&] { return mesh.Stats(1, 0).generation > before && mesh.AllUp(); }))
        << "rebuild " << i;
  }
  // Four builds on each side, one mint each.
  EXPECT_EQ(mesh.Stats(0, 1).builds, 4);
  EXPECT_EQ(mints[0].load(), 1);
  EXPECT_EQ(mints[1].load(), 1);
}

TEST(OrcaDirectLink, ScheduledBlackoutDropsBothWaysWithoutDeliveringLater)
{
  const auto from = std::chrono::steady_clock::now() + 2s;
  const auto until = from + 2s;
  Mesh mesh(2, [&](DirectOptions& options, int node) {
    if (node == 0)
      options.blackout = std::pair(from, until);
  });
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }, 1500ms));
  mesh.At(0).link->Send("before");
  ASSERT_TRUE(mesh.WaitFor([&] { return Count(mesh.At(1).received, "before") == 1; }, 400ms));
  ASSERT_TRUE(mesh.WaitFor([&] { return std::chrono::steady_clock::now() >= from + 50ms; }, 2s));
  mesh.At(0).link->Send("dark-out");
  mesh.At(1).link->Send("dark-in");
  ASSERT_TRUE(mesh.WaitFor([&] { return std::chrono::steady_clock::now() >= until + 100ms; }, 3s));
  mesh.At(0).link->Send("after-out");
  mesh.At(1).link->Send("after-in");
  ASSERT_TRUE(mesh.WaitFor([&] {
    return Count(mesh.At(0).received, "after-in") == 1 &&
           Count(mesh.At(1).received, "after-out") == 1;
  }));
  EXPECT_EQ(Count(mesh.At(0).received, "dark-in"), 0);
  EXPECT_EQ(Count(mesh.At(1).received, "dark-out"), 0);
}
