// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Common/SocketContext.h"
#include "juice.h"
#include "socket.h"

extern "C"
{
int OrcaTestTurnStart(const char* address);
void OrcaTestTurnStop();
int OrcaTestTurnStaleNonceReply(void* data, int size, void* reply, int capacity);
}

using namespace std::chrono_literals;

namespace
{
// A localhost proxy in front of the real fixture. STUN servers may cache a response by
// transaction ID; changing a request must therefore start a new transaction. No credentials
// from the environment, DNS, or external network are used here.
class TurnResponseCache
{
public:
  explicit TurnResponseCache(int upstream, bool stale_nonce) : m_stale_nonce(stale_nonce)
  {
    m_server.sin_family = AF_INET;
    m_server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    m_server.sin_port = htons(static_cast<uint16_t>(upstream));
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET)
      return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
      return;
    socklen_t length = sizeof(address);
    if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &length) == 0)
      port = ntohs(address.sin_port);
    ctl_t nonblocking = 1;
    EXPECT_EQ(ioctlsocket(m_socket, FIONBIO, &nonblocking), 0);
  }

  ~TurnResponseCache() { closesocket(m_socket); }

  void Pump()
  {
    char data[4096];
    sockaddr_in source{};
    socklen_t length = sizeof(source);
    const int size = recvfrom(m_socket, data, sizeof(data), 0,
                              reinterpret_cast<sockaddr*>(&source), &length);
    if (size < 20)
      return;
    const std::string id(data + 8, 12);
    if (source.sin_port == m_server.sin_port)
    {
      m_responses[id] = std::string(data, size);
      Send(m_client, m_responses.at(id));
      return;
    }
    m_client = source;
    if (const auto it = m_responses.find(id); it != m_responses.end())
    {
      ++cache_hits;
      Send(m_client, it->second);
      return;
    }
    char reply[4096];
    const int written = m_stale_nonce ?
                            OrcaTestTurnStaleNonceReply(data, size, reply, sizeof(reply)) : 0;
    ASSERT_GE(written, 0);
    if (written > 0)
    {
      m_stale_nonce = false;
      m_responses[id] = std::string(reply, written);
      ++stale_challenges;
      Send(m_client, m_responses.at(id));
      return;
    }
    Send(m_server, std::string(data, size));
  }

  int port = 0;
  int cache_hits = 0;
  int stale_challenges = 0;

private:
  void Send(const sockaddr_in& to, const std::string& data)
  {
    EXPECT_EQ(sendto(m_socket, data.data(), static_cast<int>(data.size()), 0,
                     reinterpret_cast<const sockaddr*>(&to), sizeof(to)),
              static_cast<int>(data.size()));
  }

  Common::SocketContext m_context;
  socket_t m_socket = INVALID_SOCKET;
  sockaddr_in m_server{}, m_client{};
  bool m_stale_nonce;
  std::map<std::string, std::string> m_responses;
};

void GatherThroughCache(bool stale_nonce)
{
  const int upstream = OrcaTestTurnStart("127.0.0.1");
  ASSERT_GT(upstream, 0);
  struct Cleanup
  {
    ~Cleanup() { OrcaTestTurnStop(); }
  } cleanup;
  TurnResponseCache cache(upstream, stale_nonce);
  ASSERT_GT(cache.port, 0);
  std::atomic<bool> relay{false}, done{false};
  struct Callbacks
  {
    std::atomic<bool>& relay;
    std::atomic<bool>& done;
  } callbacks{relay, done};
  juice_turn_server_t server{};
  server.host = "127.0.0.1";
  server.port = static_cast<uint16_t>(cache.port);
  server.username = "orca-fixture";
  server.password = "orca-fixture-only";
  juice_config_t config{};
  config.bind_address = "127.0.0.1";
  config.turn_servers = &server;
  config.turn_servers_count = 1;
  config.user_ptr = &callbacks;
  config.cb_candidate = [](juice_agent_t*, const char* candidate, void* user) {
    if (std::strstr(candidate, "typ relay"))
      static_cast<Callbacks*>(user)->relay = true;
  };
  config.cb_gathering_done = [](juice_agent_t*, void* user) {
    static_cast<Callbacks*>(user)->done = true;
  };
  std::unique_ptr<juice_agent_t, decltype(&juice_destroy)> agent(juice_create(&config), juice_destroy);
  ASSERT_TRUE(agent);
  ASSERT_EQ(juice_gather_candidates(agent.get()), 0);
  const auto until = std::chrono::steady_clock::now() + 3s;
  while (!done && std::chrono::steady_clock::now() < until)
  {
    cache.Pump();
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(done);
  EXPECT_TRUE(relay);
  EXPECT_EQ(cache.cache_hits, 0);
  EXPECT_EQ(cache.stale_challenges, stale_nonce ? 1 : 0);
}
}  // namespace

TEST(OrcaTurnRetry, AllocateAfterChallengeUsesANewTransaction)
{
  GatherThroughCache(false);
}

TEST(OrcaTurnRetry, AllocateAfterStaleNonceUsesANewTransaction)
{
  GatherThroughCache(true);
}
