// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Core/Orca/Bridge.h"
#include "Core/Orca/UX/ControllerSource.h"

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/LogManager.h"
#include "Common/ScopeGuard.h"
#include "Common/WebSocket.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/Games/Generic/GenericModule.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/RoomResilience.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/Search.h"
#include "Core/Orca/UX/UX.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"

namespace Rollback
{
Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad()>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr,
                            std::chrono::nanoseconds* waited = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence = {},
                            OnlineMatch::StallBudget* stall_budget = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering = {});
}

using namespace Orca::Net;

namespace
{
void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

// Sets a variable for one scope, then puts back what was there.
class ScopedEnv
{
public:
  ScopedEnv(const char* name, const char* value) : m_name(name)
  {
    if (const char* old = std::getenv(name))
      m_old = old;
    SetEnv(name, value);
  }
  ~ScopedEnv() { SetEnv(m_name, m_old ? m_old->c_str() : nullptr); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  const char* m_name;
  std::optional<std::string> m_old;
};

Packet Sample()
{
  Packet packet;
  packet.seat = 1;
  packet.first_frame = 120;
  for (u8 i = 0; i < 3; ++i)
    packet.pads.push_back(Pad{i, 0xff, 0x80, 0x7f, 0, 1, static_cast<u8>(0xa0 + i), 0x0f});
  packet.ack = {130, -1, -1, -1};
  packet.current_frame = 121;
  packet.checksum_frame = 60;
  packet.checksum = 0xfedcba9876543210ull;
  packet.resimulated = 42;
  packet.advantage = -17;
  packet.rtt = 64;
  packet.stalls = 3;
  packet.hitches = 5;
  packet.spared = std::array<int, SPARED_FRAMES>{1, 2, 2, 3};
  return packet;
}

void ExpectSame(const Packet& a, const Packet& b)
{
  EXPECT_EQ(a.seat, b.seat);
  EXPECT_EQ(a.first_frame, b.first_frame);
  EXPECT_EQ(a.pads, b.pads);
  EXPECT_EQ(a.ack, b.ack);
  EXPECT_EQ(a.current_frame, b.current_frame);
  EXPECT_EQ(a.checksum_frame, b.checksum_frame);
  EXPECT_EQ(a.checksum, b.checksum);
  EXPECT_EQ(a.resimulated, b.resimulated);
  EXPECT_EQ(a.advantage, b.advantage);
  EXPECT_EQ(a.rtt, b.rtt);
  EXPECT_EQ(a.stalls, b.stalls);
  EXPECT_EQ(a.hitches, b.hitches);
  EXPECT_EQ(a.spared, b.spared);
}

Packet Frames(int seat, int first, int count)
{
  Packet packet;
  packet.seat = seat;
  packet.first_frame = first;
  for (int f = first; f < first + count; ++f)
    packet.pads.push_back(Pad{static_cast<u8>(f), 0, 0, 0, 0, 0, 0, 0});
  packet.current_frame = first + count;
  return packet;
}
}  // namespace

TEST(OrcaRoom, PacketRoundTrips)
{
  const Packet sent = Sample();
  Packet got;
  ASSERT_TRUE(DecodePacket(EncodePacket(sent), &got));
  ExpectSame(sent, got);

  // Without a checksum, re-simulation count or advantage, and with no pads.
  Packet bare;
  bare.current_frame = 5;
  ASSERT_TRUE(DecodePacket(EncodePacket(bare), &got));
  ExpectSame(bare, got);
  EXPECT_EQ(got.resimulated, -1);
  EXPECT_FALSE(got.advantage);
  EXPECT_FALSE(got.rtt);
  EXPECT_EQ(got.stalls, -1);
  EXPECT_EQ(got.hitches, -1);
  EXPECT_FALSE(got.spared);
  const std::string text = EncodePacket(bare);
  EXPECT_EQ(text.find("\"r\""), std::string::npos);
  EXPECT_EQ(text.find("\"v\""), std::string::npos);
  EXPECT_EQ(text.find("\"l\""), std::string::npos);
  EXPECT_EQ(text.find("\"n\""), std::string::npos);
  EXPECT_EQ(text.find("\"j\""), std::string::npos);
  EXPECT_EQ(text.find("\"sp\""), std::string::npos);

  // A lead of zero, a quiet window and an instant relay still travel.
  bare.resimulated = 0;
  bare.advantage = 0;
  bare.rtt = 0;
  bare.stalls = 0;
  bare.hitches = 0;
  bare.spared = std::array<int, SPARED_FRAMES>{};
  ASSERT_TRUE(DecodePacket(EncodePacket(bare), &got));
  ExpectSame(bare, got);
}

TEST(OrcaRoom, LegacyPacketGoldens)
{
  // Captured with the unmodified 0.3.77 encoder (55238506e8).
  EXPECT_EQ(EncodePacket(Packet{}), R"golden({"a":[-1,-1,-1,-1],"c":0,"f":0,"k":"p","p":"","s":0})golden");
  EXPECT_EQ(EncodePacket(Sample()), R"golden({"a":[130,-1,-1,-1],"c":121,"f":120,"h":"fedcba9876543210","j":5,"k":"p","l":64,"n":3,"p":"00ff807f0001a00f01ff807f0001a10f02ff807f0001a20f","r":42,"s":1,"sp":[1,2,2,3],"v":-17,"x":60})golden");
  Packet host = Frames(0, 20, 2);
  host.sequence = 7;
  host.roster = std::array<SeatPlan, MAX_SEATS>{SeatPlan{0, NEVER, 0},
      SeatPlan{30, NEVER, 20}, SeatPlan{}, SeatPlan{10, 19, 0}};
  host.start_gate = {20, 30};
  host.departed[3] = {10, 19, 17, Frames(3, 17, 2).pads};
  host.history_ack = 12;
  host.values_ack = 3;
  host.history_seat = 1;
  host.history_first = 0;
  host.history = {Pads{}};
  host.leaving = true;
  EXPECT_EQ(EncodePacket(host), R"golden({"a":[-1,-1,-1,-1],"c":22,"ds":[[3,10,19,17,"11000000000000001200000000000000"]],"f":20,"ha":12,"hf":0,"hp":[[1,"0000000000000000000000000000000000000000000000000000000000000000"]],"ht":1,"k":"p","lv":true,"p":"14000000000000001500000000000000","q":7,"ro":[[0,-1,0],[30,-1,20],[-1,-1,-1],[10,19,0]],"s":0,"sg":[20,30],"va":3})golden");
}

// The host's port values travel in the room's "nm" and "kf" messages: names, the frame each
// applies from, and a player's own controls; a joiner reads back exactly what the host holds.
namespace
{
using RK = RoomTestStep::Kind;
using namespace std::chrono_literals;

std::string RosterMessage(const char* type, bool joining = true, bool include_peer = true)
{
  const std::string host = joining ? "peer" : "me";
  const int local_slot = joining ? 1 : 0;
  const int peer_slot = joining ? 0 : 1;
  return fmt::format(R"({{"t":"{}","me":"me","host":"{}","lifecycle":"game",
      "player":{{"id":"peer"}},"players":[{{"id":"me","name":"Me"}}{}],"participants":[{{"connectionId":"me","slot":{},"name":"Me"}}{}]}})",
                     type, host,
                     include_peer ? std::string(R"(,{"id":"peer","name":"Friend"})") : "",
                     local_slot,
                     include_peer ? fmt::format(R"(,{{"connectionId":"peer","slot":{},"name":"Friend"}})", peer_slot) : "");
}

std::string PeerHello(bool host = true, bool resume = false)
{
  return fmt::format(R"({{"t":"msg","from":"peer","d":{{"k":"hello","c":"test",
      "r":true,"h":{},"resume":{}}}}})", host, resume);
}

std::string PeerPacket(int seat = 0)
{
  return fmt::format(R"({{"t":"msg","from":"peer","d":{}}})", EncodePacket(Frames(seat, 20, 3)));
}

RoomOptions TraceOptions(bool joining = true)
{
  RoomOptions options;
  options.room_code = "roomtest";
  options.compatibility = "test";
  options.joining = joining;
  return options;
}
}  // namespace

namespace
{
// Public opt-in is room metadata on welcome only; other messages keep the normal roster shape.
std::string QueueRoster(const char* type, const char* queue, const std::string& recovery = "true",
                        bool joining = true)
{
  picojson::value value;
  picojson::parse(value, RosterMessage(type, joining));
  auto& m = value.get<picojson::object>();
  for (auto& participant : m["participants"].get<picojson::array>())
  {
    auto& p = participant.get<picojson::object>();
    p["id"] = p.at("connectionId");
  }
  m["matchId"] = picojson::value("queue-recovery-match");
  picojson::object room{{"queue", picojson::value(queue)}};
  if (!recovery.empty())
  {
    picojson::value flag;
    picojson::parse(flag, recovery);
    room["recovery"] = flag;
  }
  m["room"] = picojson::value(room);
  return value.serialize();
}
RoomOptions QueueTraceOptions(bool joining = true)
{
  auto options = TraceOptions(joining);
  options.friends_room = false;
  return options;
}
}  // namespace

TEST(OrcaRoomTraceDeathTest, ReportStepsFailLoudlyWhenTheQueueIsFull)
{
  for (const auto kind : {RK::ReportGame, RK::GameStart})
  {
    SCOPED_TRACE(kind == RK::ReportGame ? "result" : "start");
    std::vector<RoomTestStep> steps(16, {kind, 0, "1", 120});
    EXPECT_EQ(TraceRoomForTest(QueueTraceOptions(), steps).size(), 16u);
    steps.push_back({kind, 1, "1", 121});
    EXPECT_DEATH((void)TraceRoomForTest(QueueTraceOptions(), steps),
                 "TraceRoomForTest: report queue full");
  }
}

TEST(OrcaRoomPingRange, QueueWelcomesAdvertiseProbeOnJoin)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const bool joining : {false, true})
    {
      SCOPED_TRACE(fmt::format("{} joining={}", queue, joining));
      const auto welcome = QueueRoster("welcome", queue, "true", joining);
      const auto trace = TraceRoomForTest(QueueTraceOptions(joining), {
          {RK::Message, 0, welcome}, {RK::SocketClose, 1000}, {RK::Message, 2000, welcome},
      });
      for (const size_t step : {0u, 2u})
        EXPECT_EQ(trace[step].sent,
                  (std::vector<std::string>{R"({"probe":1,"t":"join"})"}));
    }
  }
}

TEST(OrcaRoomPingRange, FriendsAndTournamentJoinsKeepTheirBytes)
{
  ScopedEnv direct("ORCA_DIRECT", "1");
  for (const bool joining : {false, true})
  {
    for (const char* queue : {"private", "casual", "ranked"})
    {
      SCOPED_TRACE(fmt::format("{} joining={}", queue, joining));
      const auto welcome = QueueRoster("welcome", queue, "true", joining);
      const auto trace = TraceRoomForTest(TraceOptions(joining), {
          {RK::Tournament, 0}, {RK::Message, 0, welcome},
      });
      EXPECT_TRUE(trace.back().sent.empty());
    }
    auto options = TraceOptions(joining);
    options.recovery_enabled = false;  // No random private recovery identity in this wire golden.
    const auto friends = TraceRoomForTest(options, {
        {RK::Message, 0, RosterMessage("welcome", joining)},
        {RK::Tick, 1000},
    });
    EXPECT_TRUE(friends[0].sent.empty());
    // Captured from 0.3.83 with one seat: no capability fields in the peer hello.
    ASSERT_EQ(friends[1].sent.size(), 2u);  // Normal timestamped ping, then hello.
    EXPECT_EQ(friends[1].sent[1], fmt::format(
        R"({{"d":{{"c":"test","dl":1,"h":{},"k":"hello","r":false}},"t":"msg","to":"peer"}})",
        !joining));
  }
}

TEST(OrcaRoomPingRange, ProbeEchoIsImmediateAndPreservesTheReceivedNonce)
{
  ScopedEnv delay("ORCA_TEST_NET_DELAY_MS", nullptr);
  for (const char* queue : {"casual", "ranked"})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)},
        // A room match's start is still character select, before game 1's fight.
        {RK::Message, 0, QueueRoster("start", queue)},
        {RK::SocketMessage, 0, R"({"t":"probe","n":"b3fdc99d-64e1-4c92-9967-d81e6e873073"})"},
        {RK::SocketMessage, 1000, R"({"t":"probe","n":"quote\"slash\\"})"},
    });
    EXPECT_EQ(trace[2].sent, (std::vector<std::string>{
        R"({"n":"b3fdc99d-64e1-4c92-9967-d81e6e873073","t":"probe-ack"})"}));
    EXPECT_EQ(trace[3].sent, (std::vector<std::string>{
        R"({"n":"quote\"slash\\","t":"probe-ack"})"}));
  }
}

TEST(OrcaRoomPingRange, ProbeDiagnosticsMeasureFromSocketReceiptWithTheInjectedClock)
{
  ScopedEnv session("ORCA_SESSION", "1");
  ScopedEnv delay("ORCA_TEST_NET_DELAY_MS", "1000");
  ScopedEnv jitter("ORCA_TEST_NET_JITTER_MS", "0");
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")},
      {RK::FlushHeld, 1000},
      {RK::SocketMessage, 1100, R"({"t":"probe","n":"12345678-private-suffix"})"},
      {RK::FlushHeld, 2100}, {RK::FlushHeld, 3099}, {RK::FlushHeld, 3100},
  });
  for (size_t i = 0; i < 5; ++i)
    EXPECT_TRUE(trace[i].probe_lines.empty()) << i;
  EXPECT_EQ(trace[5].probe_lines, (std::vector<std::string>{
      "orca probe n=12345678 recv→ack 2000000 us write-flush 0 us read-gap 0 us read 0 us result=sent"}));
}

TEST(OrcaRoomPingRange, ProbeDiagnosticsBoundAndSanitizeTheNoncePrefix)
{
  ScopedEnv delay("ORCA_TEST_NET_DELAY_MS", nullptr);
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")},
      {RK::SocketMessage, 0, R"({"t":"probe","n":"quote\"slash\\-private"})"},
      {RK::SocketMessage, 1000, R"({"t":"probe","n":"short"})"},
  });
  EXPECT_EQ(trace[1].probe_lines, (std::vector<std::string>{
      "orca probe n=quote_sl recv→ack 0 us write-flush 0 us read-gap 0 us read 0 us result=sent"}));
  EXPECT_EQ(trace[2].probe_lines, (std::vector<std::string>{
      "orca probe n=- recv→ack 0 us write-flush 0 us read-gap 0 us read 0 us result=sent"}));
}

TEST(OrcaRoomPingRange, AgentDelayHoldsBothProbeAndAckForTheRoundTrip)
{
  ScopedEnv session("ORCA_SESSION", "1");
  ScopedEnv delay("ORCA_TEST_NET_DELAY_MS", "1000");
  ScopedEnv jitter("ORCA_TEST_NET_JITTER_MS", "0");
  for (const char* queue : {"casual", "ranked"})
  {
    SCOPED_TRACE(queue);
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)},
        {RK::FlushHeld, 1000},  // Clear the delayed join before measuring the probe.
        {RK::SocketMessage, 1100, R"({"t":"probe","n":"delayed"})"},
        {RK::FlushHeld, 2099}, {RK::FlushHeld, 2100},
        {RK::FlushHeld, 3099}, {RK::FlushHeld, 3100}, {RK::FlushHeld, 4100},
    });
    EXPECT_EQ(trace[1].sent, (std::vector<std::string>{R"({"probe":1,"t":"join"})"}));
    for (size_t i = 2; i < 6; ++i)
      EXPECT_TRUE(trace[i].sent.empty()) << i;
    EXPECT_EQ(trace[6].sent, (std::vector<std::string>{
        R"({"n":"delayed","t":"probe-ack"})"}));
    EXPECT_TRUE(trace[7].sent.empty());
  }
}

TEST(OrcaRoomPingRange, MalformedProbesDoNotConsumeTheEchoBudget)
{
  std::vector<RoomTestStep> steps{{RK::Message, 0, QueueRoster("welcome", "ranked")}};
  for (const std::string bad : {R"({"t":"probe"})", R"({"t":"probe","n":null})",
                               R"({"t":"probe","n":1})", R"({"t":"probe","n":true})",
                               R"({"t":"probe","n":[]})", R"({"t":"probe","n":{}})",
                               R"({"t":"probe","n":""})", R"({"t":"probe","n":"\n"})",
                               R"({"t":"probe",)", R"(["probe","n"])"})
    steps.push_back({RK::Message, 0, bad});
  steps.push_back({RK::Message, 0, fmt::format(R"({{"t":"probe","n":"{}"}})", std::string(65, 'a'))});
  steps.push_back({RK::Message, 0, fmt::format(R"({{"t":"probe","n":"{}"}})", std::string(64, 'a'))});
  const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
  for (size_t i = 1; i + 1 < trace.size(); ++i)
    EXPECT_TRUE(trace[i].sent.empty()) << i;
  EXPECT_EQ(trace.back().sent, (std::vector<std::string>{fmt::format(
      R"({{"n":"{}","t":"probe-ack"}})", std::string(64, 'a'))}));
}

TEST(OrcaRoomPingRange, JitteredProbeSpacingStillCollectsBothSamples)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "casual")},
      {RK::SocketMessage, 0, R"({"t":"probe","n":"late"})"},
      {RK::SocketMessage, 850, R"({"t":"probe","n":"on-time"})"},
  });
  EXPECT_EQ(trace[1].sent, (std::vector<std::string>{R"({"n":"late","t":"probe-ack"})"}));
  EXPECT_EQ(trace[2].sent, (std::vector<std::string>{R"({"n":"on-time","t":"probe-ack"})"}));
}

TEST(OrcaRoomPingRange, BurstDeliveredUniqueProbesAreAcknowledgedUpToSix)
{
  std::vector<RoomTestStep> steps{{RK::Message, 0, QueueRoster("welcome", "ranked")}};
  for (int ms = 0; ms < 10; ++ms)
    steps.push_back({RK::SocketMessage, ms, fmt::format(R"({{"t":"probe","n":"n{}"}})", ms)});
  const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
  std::vector<int> acknowledged;
  for (size_t i = 1; i < trace.size(); ++i)
    if (!trace[i].sent.empty())
      acknowledged.push_back(steps[i].milliseconds);
  EXPECT_EQ(acknowledged, (std::vector<int>{0, 1, 2, 3, 4, 5}));
}

TEST(OrcaRoomPingRange, FloodAndRepeatedNoncesAreBounded)
{
  std::vector<RoomTestStep> steps{{RK::Message, 0, QueueRoster("welcome", "casual")}};
  for (int ms = 0; ms <= 10000; ms += 10)
    steps.push_back({RK::Message, ms, fmt::format(R"({{"t":"probe","n":"n{}"}})", ms)});
  const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
  std::vector<int> acknowledged;
  for (size_t i = 1; i < trace.size(); ++i)
  {
    EXPECT_LE(trace[i].sent.size(), 1u);
    if (!trace[i].sent.empty())
      acknowledged.push_back(steps[i].milliseconds);
  }
  ASSERT_EQ(acknowledged.size(), 6u);
  for (size_t i = 1; i < acknowledged.size(); ++i)
    EXPECT_EQ(acknowledged[i] - acknowledged[i - 1], 10);
  const auto repeated = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "casual")},
      {RK::Message, 0, R"({"t":"probe","n":"same"})"},
      {RK::Message, 1000, R"({"t":"probe","n":"same"})"},
      {RK::Message, 1000, R"({"t":"probe","n":"next"})"},
      {RK::Message, 7000, R"({"t":"probe","n":"expired"})"},
  });
  EXPECT_EQ(repeated[1].sent.size(), 1u);
  EXPECT_TRUE(repeated[2].sent.empty());
  EXPECT_EQ(repeated[3].sent.size(), 1u);
  EXPECT_TRUE(repeated[4].sent.empty());
}

TEST(OrcaRoomPingRange, EchoStopsAtTheFirstFightAndStaysOffOnRejoin)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const bool result : {false, true})
    {
      const auto welcome = QueueRoster("welcome", queue);
      const auto trace = TraceRoomForTest(QueueTraceOptions(), {
          {RK::Message, 0, welcome},
          {RK::Message, 0, R"({"t":"probe","n":"before"})"},
          {result ? RK::ReportGame : RK::GameStart, 1, "1", 120},
          // No Tick: the frame thread's report must close the echo path immediately.
          {RK::Message, 1000, R"({"t":"probe","n":"late"})"},
          {RK::SocketClose, 2000}, {RK::Message, 3000, welcome},
          {RK::Message, 4000, R"({"t":"probe","n":"rejoin"})"},
      });
      EXPECT_EQ(trace[1].sent.size(), 1u);
      EXPECT_TRUE(trace[3].sent.empty());
      EXPECT_TRUE(trace[6].sent.empty());
    }
  }
}

TEST(OrcaRoomPingRange, ProbesOutsideQueueRoomsProduceNoTraffic)
{
  for (const bool tournament : {false, true})
  {
    std::vector<RoomTestStep> steps;
    if (tournament)
      steps.push_back({RK::Tournament, 0});
    steps.push_back({RK::Message, 0, R"({"t":"probe","n":"before-welcome"})"});
    steps.push_back({RK::Message, 0, QueueRoster("welcome", tournament ? "ranked" : "private")});
    steps.push_back({RK::Message, 1000, R"({"t":"probe","n":"outside"})"});
    const auto trace = TraceRoomForTest(TraceOptions(), steps);
    for (const auto& step : trace)
      EXPECT_TRUE(step.sent.empty());
  }
}

TEST(OrcaRoomPingRange, TooFarVoidReachesThePageVerbatimOnce)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const std::string recovery : {"true", "false"})
    {
      for (const bool started : {false, true})
      {
        SCOPED_TRACE(fmt::format("{} recovery={} match-start={}", queue, recovery, started));
        picojson::value welcome;
        ASSERT_TRUE(picojson::parse(welcome, QueueRoster("welcome", queue, recovery)).empty());
        if (!started)
          welcome.get<picojson::object>().erase("matchId");
        std::vector<RoomTestStep> steps{{RK::Message, 0, welcome.serialize()}};
        if (started)
          steps.push_back({RK::Message, 0, QueueRoster("start", queue, recovery)});
        const std::string verdict = R"({"t":"result","matchId":"queue-recovery-match",
            "id":"ping-check","round":1,"void":true,"draw":false,"ranking":["me","peer"],
            "disposition":"void","reason":"too_far","ratings":null})";
        const size_t result = steps.size();
        steps.push_back({RK::Message, 1, verdict});
        steps.push_back({RK::Message, 2, verdict});
        steps.push_back({RK::Message, 3, R"({"t":"probe","n":"after-result"})"});
        const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
        const std::string expected = std::string(queue) == "ranked" ?
            R"({"q":"ranked","k":"set","out":"void","by":"games","why":"too_far","score":[0,0]})" :
            R"({"q":"casual","k":"game","n":1,"out":"void","why":"too_far"})";
        EXPECT_EQ(trace[result].results, (std::vector<std::string>{expected}));
        EXPECT_TRUE(trace[result + 1].results.empty());
        EXPECT_TRUE(trace.back().sent.empty());
      }
    }
  }
}

TEST(OrcaRoomRecovery, PublicRoomsNeedAnExactWelcomeOptIn)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const std::string flag : {"", "false", "1", "\"true\"", "null", "{}"})
    {
      const auto trace = TraceRoomForTest(QueueTraceOptions(), {
          {RK::Message, 0, QueueRoster("welcome", queue, flag)},
          {RK::Message, 0, PeerHello()}, {RK::SocketClose, 1000},
      });
      EXPECT_FALSE(trace[0].recovery) << queue << flag;
      EXPECT_FALSE(trace.back().reconnecting);
      EXPECT_EQ(trace.back().state, RoomState::Ended);
      EXPECT_TRUE(trace.back().notices.empty());
    }
  }
  const auto pre = TraceRoomForTest(QueueTraceOptions(), {{RK::SocketClose, 0}});
  EXPECT_FALSE(pre.back().recovery);
  EXPECT_EQ(pre.back().state, RoomState::Ended);
  auto disabled = QueueTraceOptions();
  disabled.recovery_enabled = false;
  const auto off = TraceRoomForTest(disabled, {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::SocketClose, 1000}});
  EXPECT_FALSE(off.back().recovery);
  EXPECT_EQ(off.back().state, RoomState::Ended);
}

TEST(OrcaRoomRecovery, PublicHoldRejoinsSameSeatAndContinuesAfterFifteenSeconds)
{
  for (const char* queue : {"casual", "ranked"})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::Message, 0, PeerHello()},
        {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000}, {RK::Tick, 16000},
        {RK::Message, 17000, QueueRoster("welcome", queue)}, {RK::Message, 17001, PeerPacket()},
    });
    EXPECT_TRUE(trace[0].recovery);
    EXPECT_TRUE(trace[3].reconnect_started);
    EXPECT_TRUE(trace[4].events.empty());
    EXPECT_EQ(trace[4].state, RoomState::Waiting);
    EXPECT_FALSE(trace[5].reconnecting);
    EXPECT_TRUE(trace[5].events.empty());
    EXPECT_EQ(trace[6].packets.size(), 1u);
    ASSERT_FALSE(trace[3].notices.empty());
    EXPECT_NE(trace[3].notices[0].find("room-away self"), std::string::npos);
    ASSERT_FALSE(trace[5].notices.empty());
    EXPECT_NE(trace[5].notices.back().find("room-back self"), std::string::npos);
  }
}

TEST(OrcaRoomRecovery, PublicPeerAwayKeepsItsSessionAndBackDeliversInputs)
{
  for (const char* queue : {"casual", "ranked"})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::Message, 0, PeerHello()},
        {RK::Message, 1000, QueueRoster("away", queue)}, {RK::Tick, 16000},
        {RK::Message, 17000, QueueRoster("back", queue)}, {RK::Message, 17001, PeerPacket()},
    });
    EXPECT_TRUE(trace[2].events.empty());
    EXPECT_TRUE(trace[3].events.empty());
    EXPECT_TRUE(trace[4].events.empty());
    EXPECT_EQ(trace[5].packets.size(), 1u);
  }
}

TEST(OrcaRoomRecovery, PublicExpiryEndsWithNetworkSentenceAndClearsTheHold)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const bool own_socket : {false, true})
    {
      const auto trace = TraceRoomForTest(QueueTraceOptions(), {
          {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::Message, 0, PeerHello()},
          {own_socket ? RK::SocketClose : RK::Message, 1000, own_socket ? "" : QueueRoster("away", queue)},
          {RK::PollReconnect, own_socket ? 90999 : 100999},
          {RK::PollReconnect, own_socket ? 91000 : 101000},
      });
      EXPECT_EQ(trace[3].state, RoomState::Waiting);
      EXPECT_EQ(trace[4].state, RoomState::Ended);
      EXPECT_FALSE(trace[4].reconnecting);
      EXPECT_FALSE(trace[4].host_away_until);
      EXPECT_EQ(trace[4].error_code, "network");
      EXPECT_EQ(trace[4].status, "Lost the connection to the match. Press Play to look for a new one.");
      EXPECT_TRUE(trace[4].results.empty());
    }
  }
}

TEST(OrcaRoomRecovery, RefusedPublicRejoinEndsWithNetworkSentence)
{
  for (const char* queue : {"casual", "ranked"})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::SocketClose, 1000},
        {RK::ConnectFailure, 1500, "WebSocket upgrade failed (HTTP 403) Room is full"},
    });
    EXPECT_EQ(trace.back().state, RoomState::Ended);
    EXPECT_EQ(trace.back().error_code, "network");
    EXPECT_EQ(trace.back().status, "Lost the connection to the match. Press Play to look for a new one.");
    EXPECT_FALSE(trace.back().reconnecting);
  }
}

TEST(OrcaRoomRecovery, MissingPublicRoomAfterOwnHoldUsesRejoinRefusalEnding)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const int http : {404, 410})
    {
      for (const bool reachable : {false, true})
      {
        for (const bool expired : {false, true})
        {
          SCOPED_TRACE(fmt::format("{} HTTP {} reachable={} expired={}", queue, http, reachable, expired));
          const int deadline = reachable ? 101000 : 91000;
          std::vector<RoomTestStep> steps{
              {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::SocketClose, 1000}};
          if (reachable)
            steps.push_back({RK::SocketConnected, 2000});
          steps.push_back({RK::ConnectFailure, deadline - (expired ? 0 : 1),
                           fmt::format("WebSocket connect failed: upgrade refused (HTTP {})", http)});
          steps.push_back({RK::PollReconnect, deadline + 1000});
          const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
          for (size_t i = trace.size() - 2; i < trace.size(); ++i)
          {
            EXPECT_EQ(trace[i].state, RoomState::Ended);
            EXPECT_EQ(trace[i].error_code, "network");
            EXPECT_EQ(trace[i].status, expired ?
                "You couldn't rejoin the match (the time to reconnect ran out)." :
                "Lost the connection to the match. Press Play to look for a new one.");
            EXPECT_FALSE(trace[i].reconnecting);
            EXPECT_FALSE(trace[i].reconnect_at);
            EXPECT_EQ(trace[i].reconnect_attempts, 0u);
            EXPECT_FALSE(trace[i].public_deadline);
            EXPECT_TRUE(trace[i].results.empty());
          }
        }
      }
    }
  }
}

TEST(OrcaRoomRecovery, MissingRoomPreservesLegacyAndFriendsEndings)
{
  for (const char* queue : {"casual", "ranked", "private"})
  {
    for (const int http : {404, 410})
    {
      auto options = QueueTraceOptions();
      options.friends_room = std::string(queue) == "private";
      const auto trace = TraceRoomForTest(options, {
          {RK::Message, 0, QueueRoster("welcome", queue, "false")},
          {RK::SocketClose, 1000},
          {RK::ConnectFailure, 92000,
           fmt::format("WebSocket connect failed: upgrade refused (HTTP {})", http)},
      });
      EXPECT_EQ(trace.back().state, RoomState::Ended);
      EXPECT_EQ(trace.back().error_code, "network");
      EXPECT_EQ(trace.back().status, options.friends_room ?
          "Lost the connection to the room (hold-expired)" : trace[1].status);
      EXPECT_NE(trace.back().status,
                "You couldn't rejoin the match (the time to reconnect ran out).");
    }
  }
}

TEST(OrcaRoomRecovery, AuthoritativeVerdictWinsOverMissingPublicRoom)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const int http : {404, 410})
    {
      const auto trace = TraceRoomForTest(QueueTraceOptions(), {
          {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::SocketClose, 1000},
          {RK::Message, 2000, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"reason":"The server ended this match."})"},
          {RK::ConnectFailure, 2001,
           fmt::format("WebSocket connect failed: upgrade refused (HTTP {})", http)},
      });
      ASSERT_EQ(trace[2].results.size(), 1u);
      EXPECT_NE(trace[2].results[0].find("won"), std::string::npos);
      EXPECT_EQ(trace.back().state, RoomState::Ended);
      EXPECT_EQ(trace.back().error_code, "match-over");
      EXPECT_EQ(trace.back().status, "The server ended this match.");
      EXPECT_FALSE(trace.back().reconnecting);
      EXPECT_TRUE(trace.back().results.empty());
    }
  }
}

TEST(OrcaRoomRecovery, ServerVerdictsEndPublicHoldsWithoutLocalForfeits)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const std::string outcome : {R"("ranking":["peer","me"],"disposition":"forfeit")",
                                      R"("void":true,"disposition":"no-contest")"})
    {
      for (const char* type : {"result", "finish"})
      {
        const auto verdict = fmt::format(R"({{"t":"{}","matchId":"queue-recovery-match",{},"reason":"The server ended this match."}})", type, outcome);
        const auto trace = TraceRoomForTest(QueueTraceOptions(), {
            {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::Message, 0, PeerHello()},
            {RK::SocketClose, 1000}, {RK::Message, 2000, verdict}, {RK::Message, 2001, verdict},
            {RK::PollReconnect, 91000},
        });
        EXPECT_EQ(trace[3].state, RoomState::Ended);
        EXPECT_EQ(trace[3].error_code, "match-over");
        EXPECT_EQ(trace[3].status, "The server ended this match.");
        EXPECT_FALSE(trace[3].reconnecting);
        EXPECT_FALSE(trace[3].host_away_until);
        ASSERT_EQ(trace[3].results.size(), 1u);
        EXPECT_NE(trace[3].results[0].find(outcome.find("void") == std::string::npos ? "lost" : "void"), std::string::npos);
        if (std::string(queue) == "ranked" && outcome.find("forfeit") != std::string::npos)
          EXPECT_NE(trace[3].results[0].find(R"("by":"forfeit")"), std::string::npos);
        EXPECT_TRUE(trace[4].results.empty());
        EXPECT_EQ(trace[5].status, trace[3].status);
      }
    }
  }
}

TEST(OrcaRoomRecovery, RefusedRejoinCloseEndsPublicRoomImmediatelyWithoutRetry)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const char* reason : {"rejoin-refused", "rejoin-refused:allowance",
                               "rejoin-refused:not-your-seat", "WebSocket closed by the server (4004)"})
    {
      for (const bool held : {false, true})
      {
        SCOPED_TRACE(fmt::format("{} {} held={}", queue, reason, held));
        std::vector<RoomTestStep> steps{{RK::Message, 0, QueueRoster("welcome", queue)}};
        if (held)
          steps.push_back({RK::SocketClose, 1000});
        steps.push_back({RK::SocketClose, 2000, reason, 4004});
        steps.push_back({RK::PollReconnect, 3000});
        const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
        for (size_t i = trace.size() - 2; i < trace.size(); ++i)
        {
          EXPECT_EQ(trace[i].state, RoomState::Ended);
          EXPECT_EQ(trace[i].error_code, "network");
          EXPECT_EQ(trace[i].status, "You couldn't rejoin the match (the time to reconnect ran out).");
          EXPECT_FALSE(trace[i].reconnect_started);
          EXPECT_FALSE(trace[i].reconnecting);
          EXPECT_FALSE(trace[i].reconnect_at);
          EXPECT_EQ(trace[i].reconnect_attempts, 0u);
          EXPECT_FALSE(trace[i].public_deadline);
          EXPECT_TRUE(trace[i].results.empty());
        }
      }
    }
  }
}

TEST(OrcaRoomRecovery, RefusedRejoinClosePreservesLegacyAndFriendsBehavior)
{
  for (const char* queue : {"casual", "ranked", "private"})
  {
    for (const std::string flag : {"", "false", "true"})
    {
      for (const bool enabled : {false, true})
      {
        if (std::string(queue) != "private" && flag == "true" && enabled)
          continue;
        SCOPED_TRACE(fmt::format("{} recovery={} enabled={}", queue, flag, enabled));
        auto options = QueueTraceOptions();
        options.recovery_enabled = enabled;
        const auto trace = TraceRoomForTest(options, {
            {RK::Message, 0, QueueRoster("welcome", queue, flag)},
            {RK::SocketClose, 1000, "rejoin-refused:allowance", 4004},
        });
        EXPECT_EQ(trace.back().state, RoomState::Ended);
        EXPECT_EQ(trace.back().error_code, "match-over");
        EXPECT_EQ(trace.back().status, "The match is over");
        EXPECT_FALSE(trace.back().reconnect_started);
      }
    }
  }
}

TEST(OrcaRoomRecovery, AuthoritativeVerdictWinsOverRefusedRejoinClose)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const bool held : {false, true})
    {
      SCOPED_TRACE(fmt::format("{} held={}", queue, held));
      std::vector<RoomTestStep> steps{{RK::Message, 0, QueueRoster("welcome", queue)}};
      if (held)
        steps.push_back({RK::SocketClose, 1000});
      steps.push_back({RK::Message, 2000, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"reason":"The server ended this match."})"});
      steps.push_back({RK::SocketClose, 2001, "rejoin-refused:not-your-seat", 4004});
      const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
      ASSERT_EQ(trace[trace.size() - 2].results.size(), 1u);
      EXPECT_NE(trace[trace.size() - 2].results[0].find("won"), std::string::npos);
      EXPECT_EQ(trace.back().state, RoomState::Ended);
      EXPECT_EQ(trace.back().error_code, "match-over");
      EXPECT_EQ(trace.back().status, held ? "The server ended this match." : "The match is over");
      EXPECT_FALSE(trace.back().reconnecting);
      EXPECT_TRUE(trace.back().results.empty());
    }
  }
}

TEST(OrcaRoomRecovery, RankedRejoinPreservesGameCountAndDoesNotRepeatReports)
{
  const auto start = QueueRoster("start", "ranked");
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, start},
      {RK::Message, 0, PeerHello()}, {RK::GameStart, 1, "", 120},
      {RK::ReportGame, 2, "1", 120}, {RK::Tick, 3},
      {RK::Message, 4, R"({"t":"game-result","matchId":"queue-recovery-match","id":"g120","ranking":["me","peer"]})"},
      {RK::SocketClose, 1000}, {RK::GameStart, 1001, "", 360}, {RK::ReportGame, 1002, "2", 360},
      {RK::Tick, 16000}, {RK::Message, 17000, QueueRoster("welcome", "ranked")},
      {RK::Message, 17001, start}, {RK::GameStart, 17002, "", 120},
      {RK::ReportGame, 17003, "1", 120}, {RK::Tick, 17004},
      {RK::Tick, 18000},
  });
  EXPECT_EQ(trace[6].known_games, 1);
  EXPECT_TRUE(trace[10].sent.empty());
  EXPECT_EQ(trace[12].known_games, 1);
  int starts = 0, reports = 0;
  for (const auto& step : trace)
    for (const auto& message : step.sent)
    {
      starts += message.find("game-start") != std::string::npos;
      reports += message.find("game-report") != std::string::npos;
    }
  EXPECT_EQ(starts, 3);
  EXPECT_EQ(reports, 2);
  EXPECT_EQ(trace.back().known_games, 2);
  EXPECT_EQ(trace.back().state, RoomState::Waiting);
}

TEST(OrcaRoomRecovery, ReportsRetryOriginalBytesAndRejoinWithoutGameplay)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const bool failed : {false, true})
    {
      SCOPED_TRACE(std::string(queue) + (failed ? " failed send" : " lost processing/receipt"));
      const auto trace = TraceRoomForTest(QueueTraceOptions(), {
          {RK::Message, 0, QueueRoster("welcome", queue)},
          {RK::Message, 0, QueueRoster("start", queue)}, {RK::Message, 0, PeerHello()},
          {RK::GameStart, 1, "", 120}, {RK::ReportGame, 2, "1", 120},
          {RK::SendFailure, 3, "", failed ? (std::string(queue) == "ranked" ? 2u : 1u) : 0u}, {RK::Tick, 4},
          {RK::Tick, 10004}, {RK::SocketClose, 11000},
          {RK::Message, 12000, QueueRoster("welcome", queue)},
          {RK::Message, 12001, QueueRoster("start", queue)},
          {RK::Message, 12002, std::string(queue) == "ranked" ?
              R"({"t":"game-result","matchId":"queue-recovery-match","id":"g120","ranking":["me","peer"]})" :
              R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"]})"},
          {RK::Message, 12003, std::string(queue) == "ranked" ?
              R"({"t":"game-result","matchId":"queue-recovery-match","id":"g120","ranking":["me","peer"]})" :
              R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"]})"},
          {RK::Tick, 25000},
      });
      std::map<std::string, std::vector<std::string>> payloads;
      size_t results = 0;
      for (const auto& step : trace)
      {
        results += step.results.size();
        for (const auto& wire : step.sent)
        {
          picojson::value message;
          ASSERT_TRUE(picojson::parse(message, wire).empty());
          const auto kind = message.get("t").to_str();
          if (kind == "game-start" || kind == "game-report" || kind == "finish")
            payloads[kind].push_back(wire);
        }
      }
      EXPECT_EQ(results, 1u);
      const auto& reports = payloads[std::string(queue) == "ranked" ? "game-report" : "finish"];
      ASSERT_GE(reports.size(), 2u);
      for (const auto& wire : reports)
        EXPECT_EQ(wire, reports.front());
      if (std::string(queue) == "ranked")
      {
        ASSERT_EQ(payloads["game-start"].size(), 2u);
        EXPECT_EQ(payloads["game-start"][0], payloads["game-start"][1]);
      }
      for (const auto& wire : trace.back().sent)
      {
        EXPECT_EQ(wire.find("game-report"), std::string::npos);
        EXPECT_EQ(wire.find("finish"), std::string::npos);
      }
    }
  }
}

namespace
{
std::string WithHold(const std::string& wire, const std::string& milliseconds)
{
  picojson::value message, ms;
  picojson::parse(message, wire);
  picojson::parse(ms, milliseconds);
  message.get<picojson::object>()["hold_ms"] = ms;
  return message.serialize();
}
}

TEST(OrcaRoomRecovery, ServerDeadlineReconcilesEarlierInputSilence)
{
  for (const char* queue : {"casual", "ranked"})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::Message, 0, PeerHello()},
        {RK::InputSilence, 1000, "", 0},
        {RK::Message, 8000, WithHold(QueueRoster("away", queue), "90000")},
        {RK::Tick, 90000}, {RK::InputSilence, 90001, "", 0}, {RK::Tick, 98000},
        {RK::Message, 98000, R"({"t":"result","matchId":"queue-recovery-match","void":true})"},
    });
    ASSERT_TRUE(trace[2].public_deadline);
    EXPECT_EQ(*trace[2].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(90));
    ASSERT_TRUE(trace[3].public_deadline);
    EXPECT_EQ(*trace[3].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(98));
    for (size_t i = 3; i < 7; ++i)
    {
      EXPECT_EQ(trace[i].state, RoomState::Waiting);
      EXPECT_TRUE(trace[i].session_recovering);
    }
    ASSERT_EQ(trace.back().results.size(), 1u);
    EXPECT_EQ(trace.back().error_code, "match-over");
  }
}

TEST(OrcaRoomRecovery, ShortServerHoldAndLateVerdictUseOneBoundedWindow)
{
  for (const bool self : {false, true})
  {
    std::vector<RoomTestStep> steps{
        {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
        {self ? RK::SocketClose : RK::InputSilence, 1000, "", 0},
        {RK::Message, 2000, self ? R"({"t":"recovering","hold_ms":1500})" :
            WithHold(QueueRoster("away", "ranked"), "1500")},
        {RK::Tick, 3500}, {RK::InputSilence, 3501, "", 0}, {RK::Tick, 5500},
    };
    if (self)
      steps.insert(steps.begin() + 3, {RK::SocketConnected, 2000});
    const size_t deadline_step = self ? 4 : 3;
    const auto pending = TraceRoomForTest(QueueTraceOptions(), steps);
    EXPECT_EQ(*pending[deadline_step].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(3500));
    EXPECT_EQ(pending.back().state, RoomState::Waiting);
    EXPECT_TRUE(pending.back().session_recovering);
    auto no_verdict = steps;
    no_verdict.push_back({RK::Tick, 13499});
    no_verdict.push_back({RK::Tick, 13500});
    const auto expired = TraceRoomForTest(QueueTraceOptions(), no_verdict);
    EXPECT_EQ(expired[expired.size() - 2].state, RoomState::Waiting);
    EXPECT_EQ(expired.back().state, RoomState::Ended);
    steps.push_back({RK::Message, 5500, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"disposition":"forfeit"})"});
    const auto verdict = TraceRoomForTest(QueueTraceOptions(), steps);
    ASSERT_EQ(verdict.back().results.size(), 1u);
    EXPECT_EQ(verdict.back().error_code, "match-over");
  }
}

TEST(OrcaRoomRecovery, ReconciledSelfCutoffStopsRetriesButReadsReachableVerdict)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 1000}, {RK::SocketConnected, 2000},
      {RK::Message, 2000, R"({"t":"recovering","hold_ms":1000})"},
      {RK::PollReconnect, 3000}, {RK::PollReconnect, 5000},
      {RK::Message, 5000, R"({"t":"result","matchId":"queue-recovery-match","void":true})"},
  });
  EXPECT_EQ(trace[5].reconnect_attempts, 0u);
  EXPECT_EQ(trace[6].state, RoomState::Waiting);
  ASSERT_EQ(trace.back().results.size(), 1u);
  EXPECT_EQ(trace.back().error_code, "match-over");
}

TEST(OrcaRoomRecovery, MissingOrInvalidHoldKeepsNinetySecondsAndLegacyIgnoresField)
{
  for (const std::string ms : {"null", "-1", "1.5", "\"2000\"", "false"})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", "casual")}, {RK::Message, 0, PeerHello()},
        {RK::Message, 1000, WithHold(QueueRoster("away", "casual"), ms)},
        {RK::Tick, 91000}, {RK::Tick, 101000},
    });
    EXPECT_EQ(*trace[2].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(91));
    EXPECT_EQ(trace[3].state, RoomState::Waiting);
    EXPECT_EQ(trace.back().state, RoomState::Ended);
  }
  for (const char* queue : {"casual", "ranked"})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue, "false")}, {RK::Message, 0, PeerHello()},
        {RK::Message, 1000, WithHold(QueueRoster("away", queue, "false"), "0")},
        {RK::InputSilence, 2000, "", 0}, {RK::SocketClose, 3000},
    });
    EXPECT_FALSE(trace[2].public_deadline);
    EXPECT_FALSE(trace[3].session_recovering);
    EXPECT_EQ(trace.back().state, RoomState::Ended);
    EXPECT_EQ(trace.back().status, "Lost the connection to the room (no reason given)");
  }
}

TEST(OrcaRoomRecovery, CrossedHoldsKeepEachPlayersDeadline)
{
  // Both orderings: the peer's earlier hold, and the review's returning player whose
  // almost-expired hold crosses a peer that dropped much later.
  for (const bool peer_first : {true, false})
  {
    const int peer_drop = peer_first ? 0 : 85000;
    const int self_drop = peer_first ? 85000 : 0;
    const int peer_end = peer_drop + 90000;
    auto welcome = QueueRoster("welcome", "ranked");
    picojson::value roster;
    picojson::parse(roster, welcome);
    for (auto& player : roster.get<picojson::object>()["players"].get<picojson::array>())
      if (player.get("id").to_str() == "peer")
      {
        player.get<picojson::object>()["away"] = picojson::value(true);
        player.get<picojson::object>()["hold_ms"] = picojson::value(double(peer_end - 89000));
      }
    std::vector<RoomTestStep> steps{
        {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
    };
    const RoomTestStep away{RK::Message, peer_drop, WithHold(QueueRoster("away", "ranked"), "90000")};
    const RoomTestStep self{RK::SocketClose, self_drop};
    steps.push_back(peer_first ? away : self);
    steps.push_back(peer_first ? self : away);
    steps.push_back({RK::Message, 89000, WithHold(roster.serialize(), "1000")});
    steps.push_back({RK::Tick, 100001});
    if (peer_first)
      steps.back().milliseconds = 90000;
    steps.push_back({RK::Tick, peer_end + 9999});
    auto no_verdict = steps;
    no_verdict.push_back({RK::Tick, peer_end + 10000});
    const auto expired = TraceRoomForTest(QueueTraceOptions(), no_verdict);
    EXPECT_EQ(expired[expired.size() - 2].state, RoomState::Waiting);
    EXPECT_EQ(expired.back().state, RoomState::Ended);
    steps.push_back({RK::Message, peer_end + 2000,
        R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"disposition":"forfeit"})"});
    steps[steps.size() - 2].milliseconds = peer_end + 1000;
    const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
    ASSERT_TRUE(trace[4].public_deadline);
    EXPECT_EQ(*trace[4].public_deadline, std::chrono::steady_clock::time_point{} +
              std::chrono::milliseconds(peer_end));
    EXPECT_EQ(trace[5].state, RoomState::Waiting);
    ASSERT_EQ(trace.back().results.size(), 1u);
    EXPECT_EQ(trace.back().error_code, "match-over");
  }
}

TEST(OrcaRoomRecovery, SingleSelfRejoinReleasesItsHoldAndMissingFieldsUseNinetySeconds)
{
  for (const bool self : {false, true})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
        {self ? RK::SocketClose : RK::Message, 1000, self ? "" : QueueRoster("away", "ranked")},
        {RK::Tick, 90999}, {self ? RK::PollReconnect : RK::Tick, self ? 91000 : 101000},
    });
    ASSERT_TRUE(trace[2].public_deadline);
    EXPECT_EQ(*trace[2].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(91));
    EXPECT_EQ(trace[3].state, RoomState::Waiting);
    EXPECT_EQ(trace.back().state, RoomState::Ended);
  }
  const auto returned = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 85000},
      {RK::Message, 89000, WithHold(QueueRoster("welcome", "ranked"), "1000")},
      {RK::Tick, 101000},
  });
  EXPECT_FALSE(returned[3].reconnecting);
  EXPECT_EQ(returned.back().state, RoomState::Waiting);
}

TEST(OrcaRoomRecovery, PeerAwayMissedDuringSelfDropGetsAnIndependentFallback)
{
  picojson::value welcome;
  picojson::parse(welcome, QueueRoster("welcome", "ranked"));
  for (auto& player : welcome.get<picojson::object>()["players"].get<picojson::array>())
    if (player.get("id").to_str() == "peer")
      player.get<picojson::object>()["away"] = picojson::value(true);
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 0}, {RK::Message, 89000, WithHold(welcome.serialize(), "1000")},
      {RK::Tick, 100001},
      {RK::Message, 101000, WithHold(QueueRoster("away", "ranked"), "74000")},
      {RK::Tick, 175000},
      {RK::Message, 177000, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"disposition":"forfeit"})"},
  });
  ASSERT_TRUE(trace[3].public_deadline);
  EXPECT_EQ(*trace[3].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(179));
  EXPECT_EQ(trace[4].state, RoomState::Waiting);
  EXPECT_EQ(*trace[5].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(175));
  EXPECT_EQ(trace[6].state, RoomState::Waiting);
  ASSERT_EQ(trace.back().results.size(), 1u);
  EXPECT_EQ(trace.back().error_code, "match-over");
}

TEST(OrcaRoomRecovery, SelfReconnectUsesOwnHoldAndRosterClearsOnlyPeerPresence)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 0, WithHold(QueueRoster("away", "ranked"), "90000")},
      {RK::SocketClose, 85000}, {RK::Tick, 100001},
      {RK::Message, 110000, WithHold(QueueRoster("welcome", "ranked"), "65000")},
      {RK::Tick, 185001},
  });
  EXPECT_EQ(trace[4].state, RoomState::Waiting);
  ASSERT_TRUE(trace[4].public_deadline);
  EXPECT_EQ(*trace[4].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(175));
  EXPECT_FALSE(trace[5].reconnecting);
  EXPECT_EQ(trace[5].public_deadline,
            std::chrono::steady_clock::time_point{} + std::chrono::seconds(200));
  EXPECT_FALSE(trace[5].host_away_until);
  EXPECT_TRUE(trace[5].session_recovering);
  EXPECT_EQ(trace.back().state, RoomState::Waiting);
  EXPECT_TRUE(trace.back().results.empty());
}

TEST(OrcaRoomRecovery, PresentPeerRejoinKeepsTheOriginalInputStall)
{
  for (const bool self_remaining : {false, true})
  {
    for (const int rejoin : {89000, 111000})
    {
      SCOPED_TRACE(fmt::format("self remaining: {}, rejoin: {}", self_remaining, rejoin));
      const auto welcome = QueueRoster("welcome", "ranked");
      std::vector<RoomTestStep> steps{
          {RK::Message, 0, welcome}, {RK::Message, 0, PeerHello()},
          {RK::Message, 0, WithHold(QueueRoster("away", "ranked"), "90000")},
          {RK::InputSilence, 0, "", 0}, {RK::SocketClose, 85000},
          // Peer returns at t=86 while self is offline; WELCOME sees it present before inputs.
          {RK::Message, rejoin, self_remaining ? WithHold(welcome, "64000") : welcome},
          {RK::InputSilence, rejoin + 1, "", 0}, {RK::Tick, rejoin + 3},
      };
      const auto pending = TraceRoomForTest(QueueTraceOptions(), steps);
      const auto deadline = std::chrono::steady_clock::time_point{} +
          std::chrono::milliseconds(rejoin < 90000 ? 90000 : rejoin + 90000);
      for (size_t i = 5; i < pending.size(); ++i)
      {
        EXPECT_EQ(pending[i].state, RoomState::Waiting);
        EXPECT_EQ(pending[i].public_deadline, deadline);
        EXPECT_FALSE(pending[i].host_away_until);
        EXPECT_TRUE(pending[i].session_recovering);
        EXPECT_TRUE(pending[i].events.empty());
      }
      ASSERT_TRUE(pending[6].input_held.has_value());
      EXPECT_TRUE(*pending[6].input_held);  // Original silence timestamp, not rejoin time.
      for (const bool verdict : {false, true})
      {
        auto ended = steps;
        ended.push_back({RK::Message, rejoin + 5000, verdict ?
            R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"disposition":"forfeit"})" :
            RosterMessage("leave", true, false)});
        ended.push_back({RK::InputSilence, rejoin + 5001, "", 0});
        const auto trace = TraceRoomForTest(QueueTraceOptions(), ended);
        ASSERT_TRUE(trace.back().input_held.has_value());
        EXPECT_FALSE(*trace.back().input_held);
        EXPECT_FALSE(trace.back().session_recovering);
        if (verdict)
        {
          EXPECT_EQ(trace.back().state, RoomState::Ended);
          EXPECT_EQ(trace.back().error_code, "match-over");
          ASSERT_EQ(trace[8].results.size(), 1u);
        }
        else
        {
          ASSERT_EQ(trace[8].events.size(), 1u);
          EXPECT_EQ(trace[8].events[0].kind, PeerEvent::Kind::Left);
        }
      }
    }
  }
}

TEST(OrcaRoomRecovery, RejoinRestoresAnInputHoldFirstObservedOffline)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 85000}, {RK::InputSilence, 110000, "", 0},
      {RK::Message, 111000, QueueRoster("welcome", "ranked")},
      {RK::InputSilence, 111001, "", 0}, {RK::Tick, 111003},
  });
  EXPECT_EQ(trace[4].public_deadline,
            std::chrono::steady_clock::time_point{} + std::chrono::seconds(201));
  ASSERT_TRUE(trace[5].input_held.has_value());
  EXPECT_TRUE(*trace[5].input_held);
  EXPECT_TRUE(trace.back().session_recovering);
  EXPECT_EQ(trace.back().state, RoomState::Waiting);
}

TEST(OrcaRoomRecovery, PresentPeerRejoinWithoutOptInKeepsLegacyEnding)
{
  for (const bool self_remaining : {false, true})
  {
    const auto welcome = QueueRoster("welcome", "ranked", "false");
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, welcome}, {RK::Message, 0, PeerHello()},
        {RK::Message, 0, QueueRoster("away", "ranked", "false")},
        {RK::InputSilence, 0, "", 0}, {RK::SocketClose, 85000},
        {RK::Message, 111000, self_remaining ? WithHold(welcome, "64000") : welcome},
        {RK::InputSilence, 111001, "", 0},
    });
    EXPECT_EQ(trace[4].state, RoomState::Ended);
    EXPECT_FALSE(trace[4].reconnect_started);
    EXPECT_EQ(trace.back().state, RoomState::Ended);
    ASSERT_TRUE(trace.back().input_held.has_value());
    EXPECT_FALSE(*trace.back().input_held);
    EXPECT_FALSE(trace.back().public_deadline);
    EXPECT_FALSE(trace.back().session_recovering);
  }
}

TEST(OrcaRoomRecovery, PeerAwayAfterRejoinUpdatesOnlyThatPeersHold)
{
  picojson::value welcome;
  picojson::parse(welcome, QueueRoster("welcome", "ranked"));
  for (auto& player : welcome.get<picojson::object>()["players"].get<picojson::array>())
    if (player.get("id").to_str() == "peer")
      player.get<picojson::object>()["away"] = picojson::value(true);
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 0, WithHold(QueueRoster("away", "ranked"), "90000")},
      {RK::SocketClose, 85000}, {RK::Message, 89000, WithHold(welcome.serialize(), "1000")},
      {RK::Message, 89001, WithHold(QueueRoster("away", "ranked"), "5000")},
      {RK::Tick, 104001},
  });
  EXPECT_EQ(*trace[4].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::seconds(179));
  EXPECT_EQ(*trace[5].public_deadline, std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(94001));
  EXPECT_EQ(trace.back().state, RoomState::Ended);
}

TEST(OrcaRoomRecovery, RejoinRefreshesAPeerThatReturnedAndDroppedWhileSelfWasOffline)
{
  for (const bool self_remaining : {false, true})
  {
    for (const std::string peer_remaining : {"", "3000", "0", "null", "-1", "1.5", "\"3000\""})
    {
      SCOPED_TRACE(fmt::format("self remaining: {}, peer remaining: {}", self_remaining, peer_remaining));
      picojson::value welcome;
      picojson::parse(welcome, QueueRoster("welcome", "ranked"));
      auto& peer = welcome.get<picojson::object>()["players"].get<picojson::array>()[1].get<picojson::object>();
      peer["away"] = picojson::value(true);
      if (!peer_remaining.empty())
        picojson::parse(peer["hold_ms"], peer_remaining);
      const auto trace = TraceRoomForTest(QueueTraceOptions(), {
          {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
          {RK::Message, 0, WithHold(QueueRoster("away", "ranked"), "90000")},
          {RK::SocketClose, 85000},
          // Self misses the peer's back at t=86 and second away at t=110 (deadline t=114).
          {RK::Message, 111000, self_remaining ? WithHold(welcome.serialize(), "64000") : welcome.serialize()},
          {RK::Tick, 111003}, {RK::InputSilence, 111004, "", 0},
          {RK::Message, 111010, WithHold(QueueRoster("away", "ranked"), "2990")},
          {RK::Tick, 114000},
          {RK::Message, 116000, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"disposition":"forfeit"})"},
      });
      const int remaining = peer_remaining == "3000" ? 3000 : peer_remaining == "0" ? 0 : 90000;
      const auto deadline = std::chrono::steady_clock::time_point{} +
                            std::chrono::milliseconds(111000 + remaining);
      EXPECT_FALSE(trace[4].reconnecting);
      EXPECT_EQ(trace[4].public_deadline, deadline);
      EXPECT_EQ(trace[4].host_away_until, deadline);
      for (size_t i = 4; i < 9; ++i)
      {
        EXPECT_EQ(trace[i].state, RoomState::Waiting);
        EXPECT_TRUE(trace[i].session_recovering);
        EXPECT_TRUE(trace[i].events.empty());
      }
      EXPECT_EQ(trace[6].public_deadline, deadline);  // Input silence uses the refreshed peer hold.
      const auto current = std::chrono::steady_clock::time_point{} + std::chrono::seconds(114);
      EXPECT_EQ(trace[7].public_deadline, current);
      EXPECT_EQ(trace[7].host_away_until, current);
      ASSERT_EQ(trace.back().results.size(), 1u);
      EXPECT_EQ(trace.back().error_code, "match-over");
    }
  }
}

TEST(OrcaRoomRecovery, RankedFinalRetriesAndReplaysUntilItsReceipt)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 0, QueueRoster("start", "ranked")},
      {RK::Message, 1, R"({"t":"game-result","matchId":"queue-recovery-match","id":"g1","ranking":["me","peer"]})"},
      {RK::SendFailure, 2, "", 1},
      {RK::Message, 3, R"({"t":"game-result","matchId":"queue-recovery-match","id":"g2","ranking":["me","peer"]})"},
      {RK::Tick, 10000}, {RK::SocketClose, 11000},
      {RK::Message, 12000, QueueRoster("welcome", "ranked")},
      {RK::Message, 12001, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"]})"},
      {RK::Message, 12002, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"]})"},
  });
  std::vector<std::string> finals;
  size_t results = 0;
  for (const auto& step : trace)
  {
    results += step.results.size();
    for (const auto& wire : step.sent)
      if (wire.find("finish") != std::string::npos)
        finals.push_back(wire);
  }
  ASSERT_EQ(finals.size(), 2u);
  EXPECT_EQ(finals[0], finals[1]);
  EXPECT_EQ(results, 3u);  // Two games, one final.
}

TEST(OrcaRoom, SceneRankedWinsWithStartFramesFinishThroughTheTally)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")},
      {RK::Message, 0, QueueRoster("start", "ranked")},
      {RK::GameStart, 1, "", 120}, {RK::ReportGame, 2, "1", 120}, {RK::Tick, 3},
      {RK::Message, 4, R"({"t":"game-result","matchId":"queue-recovery-match","id":"g120","ranking":["me","peer"]})"},
      {RK::GameStart, 5, "", 360}, {RK::ReportGame, 6, "2", 360}, {RK::Tick, 7},
      {RK::Message, 8, R"({"t":"game-result","matchId":"queue-recovery-match","id":"g360","ranking":["me","peer"]})"},
  });
  std::vector<std::string> reports, finishes;
  for (size_t i = 0; i < trace.size(); ++i)
  {
    for (const auto& wire : trace[i].sent)
    {
      picojson::value message;
      ASSERT_TRUE(picojson::parse(message, wire).empty());
      if (message.get("t").to_str() == "game-report")
        reports.push_back(message.get("id").to_str());
      if (message.get("t").to_str() == "finish")
      {
        EXPECT_EQ(i, trace.size() - 1);
        EXPECT_EQ(message.get("winner").to_str(), "me");
        EXPECT_EQ(message.get("matchId").to_str(), "queue-recovery-match");
        finishes.push_back(wire);
      }
    }
  }
  EXPECT_EQ(reports, (std::vector<std::string>{"g120", "g360"}));
  ASSERT_EQ(finishes.size(), 1u);
  ASSERT_EQ(trace.back().results.size(), 1u);
  EXPECT_NE(trace.back().results[0].find(R"("score":[2,0])"), std::string::npos);
}

TEST(OrcaRoom, SceneRankedGamesWithStartFramesVoidAtTheGameLimit)
{
  std::vector<RoomTestStep> steps{
      {RK::Message, 0, QueueRoster("welcome", "ranked")},
      {RK::Message, 0, QueueRoster("start", "ranked")}};
  for (int n = 1; n <= SetTally::SET_GAMES; ++n)
  {
    steps.push_back({RK::ReportGame, n * 3, std::to_string(n), unsigned(n * 120)});
    steps.push_back({RK::Tick, n * 3 + 1});
    steps.push_back({RK::Message, n * 3 + 2,
        fmt::format(R"({{"t":"game-result","matchId":"queue-recovery-match","id":"g{}","draw":true}})", n * 120)});
  }
  const auto trace = TraceRoomForTest(QueueTraceOptions(), steps);
  size_t finishes = 0;
  for (size_t i = 0; i < trace.size(); ++i)
    for (const auto& wire : trace[i].sent)
    {
      picojson::value message;
      ASSERT_TRUE(picojson::parse(message, wire).empty());
      if (message.get("t").to_str() == "finish")
      {
        ++finishes;
        EXPECT_EQ(i, trace.size() - 1);
        EXPECT_TRUE(message.get("void").evaluate_as_boolean());
      }
    }
  EXPECT_EQ(finishes, 1u);
}

TEST(OrcaRoomRecovery, RankedMissingEarlierReceiptReconcilesBeforeFinalScore)
{
  const auto first = R"({"t":"game-result","matchId":"queue-recovery-match","id":"g120","ranking":["me","peer"]})";
  const auto final = R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"]})";
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 0, QueueRoster("start", "ranked")},
      {RK::GameStart, 1, "", 120}, {RK::ReportGame, 2, "1", 120}, {RK::Tick, 3},
      {RK::GameStart, 4, "", 360}, {RK::ReportGame, 5, "2", 360}, {RK::Tick, 6},
      {RK::Message, 7, R"({"t":"game-result","matchId":"queue-recovery-match","id":"g360","ranking":["me","peer"]})"},
      {RK::Message, 8, final}, {RK::Message, 9, QueueRoster("start", "ranked")},
      {RK::Message, 10, final}, {RK::Tick, 3000},
      {RK::Message, 3001, R"({"t":"game-result","matchId":"another-match","id":"g120","ranking":["me","peer"]})"},
      {RK::Message, 3002, first}, {RK::Message, 3003, first}, {RK::Message, 3004, final},
      {RK::Tick, 6000},
  });
  ASSERT_EQ(trace[9].results.size(), 1u);
  EXPECT_TRUE(trace[10].results.empty());
  EXPECT_TRUE(trace[12].results.empty());
  std::vector<std::string> retried;
  for (const auto& wire : trace[13].sent)
    if (wire.find("game-report") != std::string::npos)
      retried.push_back(wire);
  ASSERT_EQ(retried.size(), 1u);
  EXPECT_NE(retried[0].find("g120"), std::string::npos);
  EXPECT_NE(retried[0].find("queue-recovery-match"), std::string::npos);
  EXPECT_TRUE(trace[14].results.empty());
  ASSERT_EQ(trace[15].results.size(), 2u);
  EXPECT_NE(trace[15].results[0].find("\"k\":\"game\""), std::string::npos);
  EXPECT_NE(trace[15].results[1].find("\"k\":\"set\""), std::string::npos);
  EXPECT_NE(trace[15].results[1].find("\"score\":[2,0]"), std::string::npos);
  size_t results = 0;
  for (const auto& step : trace)
    results += step.results.size();
  EXPECT_EQ(results, 3u);
  for (const auto& wire : trace.back().sent)
  {
    EXPECT_EQ(wire.find("game-report"), std::string::npos);
    EXPECT_EQ(wire.find("finish"), std::string::npos);
  }
}

TEST(OrcaRoomRecovery, HeldRankedFinalKeepsMissingReceiptsUntilReconciled)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 0, QueueRoster("start", "ranked")},
      {RK::GameStart, 1, "", 120}, {RK::ReportGame, 2, "1", 120}, {RK::Tick, 3},
      {RK::SocketClose, 4}, {RK::SocketConnected, 5},
      {RK::Message, 6, R"({"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"disposition":"forfeit"})"},
      {RK::Message, 7, R"({"t":"set-over","ms":300000})"},
      {RK::InputSilence, 100000, "", 100000}, {RK::Tick, 100001},
      {RK::Message, 100002, R"({"t":"game-result","matchId":"queue-recovery-match","id":"g120","ranking":["me","peer"]})"},
  });
  EXPECT_TRUE(trace[8].results.empty());
  EXPECT_FALSE(trace[10].public_deadline);
  EXPECT_FALSE(trace[10].session_recovering);
  EXPECT_EQ(trace[11].state, RoomState::Waiting);
  ASSERT_EQ(trace[12].results.size(), 2u);
  EXPECT_EQ(trace.back().state, RoomState::Ended);
  EXPECT_EQ(trace.back().error_code, "match-over");
}

TEST(OrcaRoomRecovery, FalseRecoveringWelcomeDoesNotCreateAHold)
{
  picojson::value welcome;
  picojson::parse(welcome, QueueRoster("welcome", "ranked"));
  welcome.get<picojson::object>()["recovering"] = picojson::value(false);
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {{RK::Message, 0, welcome.serialize()}});
  EXPECT_FALSE(trace.back().public_deadline);
  EXPECT_FALSE(trace.back().session_recovering);
}

TEST(OrcaRoomRecovery, RankedRecoveryNeverClaimsAStall)
{
  const auto trace = TraceRoomForTest(QueueTraceOptions(), {
      {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::Stall, 1000}, {RK::Tick, 1001},
  });
  for (const auto& message : trace.back().sent)
    EXPECT_EQ(message.find("finish"), std::string::npos);
}

TEST(OrcaRoomRecovery, PrivateManualRetryDoesNotDependOnTransportRecovery)
{
  auto options = TraceOptions(false);
  options.recovery_enabled = false;
  const auto trace = TraceRoomForTest(options, {
      {RK::Message, 0, RosterMessage("welcome", false)}, {RK::Message, 0, PeerHello(false)},
      {RK::RetryPeer, 1000, "", 1}, {RK::Tick, 1001},
  });
  ASSERT_EQ(trace.back().events.size(), 1u);
  EXPECT_EQ(trace.back().events[0].kind, PeerEvent::Kind::Left);
  EXPECT_TRUE(std::ranges::any_of(trace.back().sent, [](const std::string& message) {
    return message.find(R"("resume":true)") != std::string::npos;
  }));
}

TEST(OrcaRoomRecovery, PublicManualRetryWithoutOptInKeepsLegacyResetAndResume)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const std::string flag : {"", "false"})
    {
      SCOPED_TRACE(std::string(queue) + ":" + flag);
      const auto trace = TraceRoomForTest(QueueTraceOptions(), {
          {RK::Message, 0, QueueRoster("welcome", queue, flag)},
          {RK::Message, 0, PeerHello()}, {RK::Message, 1, PeerPacket()},
          {RK::RetryPeer, 1000, "", 0}, {RK::Tick, 1001},
          {RK::Message, 1002, PeerPacket()},
      });
      ASSERT_EQ(trace[1].events.size(), 1u);
      EXPECT_EQ(trace[1].events[0].kind, PeerEvent::Kind::Arrived);
      ASSERT_EQ(trace[2].packets.size(), 1u);
      EXPECT_FALSE(trace[0].recovery);
      // Base 343a655173 resets the announced peer and starts a fresh resume handshake.
      ASSERT_EQ(trace[4].events.size(), 1u);
      const auto& left = trace[4].events[0];
      EXPECT_EQ(left.kind, PeerEvent::Kind::Left);
      EXPECT_EQ(left.seat, 0);
      EXPECT_TRUE(left.host);
      EXPECT_EQ(left.name, "Friend");
      EXPECT_EQ(left.reason, "room-resume");
      ASSERT_EQ(trace[4].sent.size(), 2u);
      picojson::value message;
      ASSERT_TRUE(picojson::parse(message, trace[4].sent[0]).empty());
      EXPECT_EQ(message.get("t").to_str(), "ping");
      ASSERT_TRUE(picojson::parse(message, trace[4].sent[1]).empty());
      EXPECT_EQ(message.get("t").to_str(), "msg");
      EXPECT_EQ(message.get("to").to_str(), "peer");
      const auto& hello = message.get("d");
      EXPECT_EQ(hello.get("k").to_str(), "hello");
      EXPECT_EQ(hello.get("c").to_str(), "test");
      EXPECT_FALSE(hello.get("r").evaluate_as_boolean());
      EXPECT_FALSE(hello.get("h").evaluate_as_boolean());
      EXPECT_TRUE(hello.get("resume").evaluate_as_boolean());
      EXPECT_FALSE(hello.get("resume_id").to_str().empty());
      EXPECT_TRUE(trace[5].packets.empty());
      EXPECT_EQ(trace.back().state, RoomState::Waiting);
    }
  }
}

TEST(OrcaRoomRecovery, PublicManualRetryWithOptInKeepsTheSession)
{
  for (const char* queue : {"casual", "ranked"})
  {
    SCOPED_TRACE(queue);
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::Message, 0, PeerHello()},
        {RK::Message, 1, PeerPacket()}, {RK::RetryPeer, 1000, "", 0},
        {RK::Tick, 1001}, {RK::Tick, 2002}, {RK::Message, 2003, PeerPacket()},
    });
    EXPECT_TRUE(trace[0].recovery);
    ASSERT_EQ(trace[1].events.size(), 1u);
    EXPECT_EQ(trace[1].events[0].kind, PeerEvent::Kind::Arrived);
    ASSERT_EQ(trace[2].packets.size(), 1u);
    for (size_t i = 3; i < trace.size(); ++i)
    {
      EXPECT_TRUE(trace[i].events.empty()) << i;
      for (const auto& sent : trace[i].sent)
      {
        picojson::value message;
        ASSERT_TRUE(picojson::parse(message, sent).empty());
        EXPECT_EQ(message.get("t").to_str(), "ping") << i;
      }
    }
    EXPECT_EQ(trace.back().packets.size(), 1u);
    EXPECT_EQ(trace.back().state, RoomState::Waiting);
  }
}

TEST(OrcaRoomRecovery, CompletedWelcomeCanDeliverItsFirstFinalVerdict)
{
  for (const char* queue : {"casual", "ranked"})
  {
    picojson::value welcome;
    ASSERT_TRUE(picojson::parse(welcome, QueueRoster("welcome", queue)).empty());
    welcome.get<picojson::object>()["matchId"] = picojson::value();
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, welcome.serialize()},
        {RK::Message, 1, R"({"t":"result","matchId":"queue-recovery-match","void":true})"},
        {RK::SocketClose, 2, "room ended", 4004},
    });
    ASSERT_EQ(trace[1].results.size(), 1u);
    EXPECT_NE(trace[1].results[0].find("void"), std::string::npos);
    EXPECT_EQ(trace.back().state, RoomState::Ended);
    EXPECT_EQ(trace.back().error_code, "match-over");
    EXPECT_FALSE(trace.back().reconnecting);
  }
}

TEST(OrcaRoomRecovery, RecoveryWelcomeCanClearTheMatchBeforeItsFinalVerdict)
{
  for (const char* queue : {"casual", "ranked"})
  {
    picojson::value welcome;
    ASSERT_TRUE(picojson::parse(welcome, QueueRoster("welcome", queue)).empty());
    welcome.get<picojson::object>()["matchId"] = picojson::value();
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", queue)}, {RK::Message, 0, PeerHello()},
        {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000},
        {RK::Message, 17000, welcome.serialize()}, {RK::Tick, 17001},
        {RK::Message, 17002, R"({"t":"result","matchId":"queue-recovery-match","ranking":["peer","me"],"disposition":"forfeit"})"},
    });
    for (const auto& message : trace[5].sent)
      EXPECT_EQ(message.find("begin"), std::string::npos);
    ASSERT_EQ(trace.back().results.size(), 1u);
    EXPECT_NE(trace.back().results[0].find("lost"), std::string::npos);
    EXPECT_EQ(trace.back().state, RoomState::Ended);
    EXPECT_EQ(trace.back().error_code, "match-over");
    EXPECT_FALSE(trace.back().reconnecting);
  }
}

TEST(OrcaRoomRecovery, BothDropAndRejoinAClosingMatchAcceptsCompletedVerdict)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const bool joining : {false, true})
    {
      for (const bool closing_on_welcome : {false, true})
      {
        SCOPED_TRACE(fmt::format("{} joining={} closing-on-welcome={}", queue, joining,
                                closing_on_welcome));
        const std::string initial = QueueRoster("welcome", queue, "true", joining);
        picojson::value welcome;
        ASSERT_TRUE(picojson::parse(welcome, initial).empty());
        auto& closing = welcome.get<picojson::object>();
        closing["matchId"] = picojson::value();
        // Settling has cleared the gameplay roster; the original match's verdict follows it.
        closing["host"] = picojson::value();
        closing["players"] = picojson::value(picojson::array{});
        closing["participants"] = picojson::value(picojson::array{});
        const std::string verdict = fmt::format(
            R"({{"t":"result","matchId":"queue-recovery-match","ranking":["{}","{}"],"disposition":"completed"}})",
            joining ? "peer" : "me", joining ? "me" : "peer");
        std::vector<RoomTestStep> steps{
            {RK::Message, -3, initial},
            {RK::Message, -2, QueueRoster("start", queue, "true", joining)},
            {RK::Message, -1, PeerHello(joining)},
            {RK::SocketClose, 0, "both sockets dropped", 1006},
            {RK::InputSilence, 1, "", 0},
            {RK::Message, 1700, closing_on_welcome ? welcome.serialize() : initial},
        };
        if (!closing_on_welcome)
        {
          closing["t"] = picojson::value("lobby");
          steps.push_back({RK::Message, 1900, welcome.serialize()});
        }
        steps.push_back({RK::InputSilence, 2399, "", 0});
        steps.push_back({RK::Message, 2400, verdict});
        steps.push_back({RK::Message, 2401, verdict});  // A replay cannot publish it twice.
        steps.push_back({RK::SocketClose, 2500, "match closed", 4004});
        const auto trace = TraceRoomForTest(QueueTraceOptions(joining), steps);
        EXPECT_TRUE(trace[3].reconnect_started);
        ASSERT_TRUE(trace[trace.size() - 4].input_held.has_value());
        EXPECT_TRUE(*trace[trace.size() - 4].input_held);
        for (size_t i = 5; i + 3 < trace.size(); ++i)
        {
          EXPECT_EQ(trace[i].state, RoomState::Waiting) << i;
          EXPECT_TRUE(trace[i].events.empty()) << i;
        }
        size_t results = 0;
        for (const auto& step : trace)
        {
          EXPECT_NE(step.error_code, "network");
          for (const auto& result : step.results)
          {
            ++results;
            EXPECT_NE(result.find(joining ? "lost" : "won"), std::string::npos);
          }
          for (const auto& sent : step.sent)
            EXPECT_EQ(sent.find("\"t\":\"begin\""), std::string::npos);
        }
        EXPECT_EQ(results, 1u);
        EXPECT_EQ(trace.back().state, RoomState::Ended);
        EXPECT_EQ(trace.back().error_code, "match-over");
        EXPECT_FALSE(trace.back().reconnecting);
      }
    }
  }
}

TEST(OrcaRoomRecovery, ClearedRejoinWithoutAFinishEndsNormallyAtHoldOrVerdictDeadline)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const bool joining : {false, true})
    {
      for (const bool hold_expires_first : {false, true})
      {
        SCOPED_TRACE(fmt::format("{} joining={} hold-first={}", queue, joining, hold_expires_first));
        picojson::value welcome;
        ASSERT_TRUE(picojson::parse(welcome, QueueRoster("welcome", queue, "true", joining)).empty());
        auto& closing = welcome.get<picojson::object>();
        closing["room"].get<picojson::object>()["stay"] = picojson::value(true);
        const std::string initial = welcome.serialize();
        closing["matchId"] = picojson::value();
        closing["host"] = picojson::value();
        closing["players"] = picojson::value(picojson::array{});
        closing["participants"] = picojson::value(picojson::array{});
        const int returned = hold_expires_first ? 95000 : 1700;
        const int deadline = hold_expires_first ? 100000 : 11700;
        std::vector<RoomTestStep> steps{
            {RK::Message, -3, initial},
            {RK::Message, -2, QueueRoster("start", queue, "true", joining)},
            {RK::Message, -1, PeerHello(joining)},
            {RK::SocketClose, 0, "both sockets dropped", 1006},
            {RK::InputSilence, 1, "", 0},
            {RK::Message, returned, welcome.serialize()},
        };
        // A roster refresh cannot restart the verdict wait or begin another match.
        closing["t"] = picojson::value("lobby");
        steps.push_back({RK::Message, deadline - 2, welcome.serialize()});
        steps.push_back({RK::Tick, deadline - 1});
        steps.push_back({RK::Tick, deadline});
        const auto trace = TraceRoomForTest(QueueTraceOptions(joining), steps);
        EXPECT_EQ(trace[trace.size() - 2].state, RoomState::Waiting);
        EXPECT_EQ(trace.back().state, RoomState::Ended);
        EXPECT_EQ(trace.back().error_code, "match-over");
        EXPECT_FALSE(trace.back().session_recovering);
        for (const auto& step : trace)
        {
          EXPECT_NE(step.error_code, "network");
          EXPECT_TRUE(step.results.empty());
          for (const auto& sent : step.sent)
          {
            EXPECT_EQ(sent.find("\"t\":\"finish\""), std::string::npos);
            EXPECT_EQ(sent.find("\"t\":\"begin\""), std::string::npos);
          }
        }
      }
    }
  }
}

TEST(OrcaRoomRecovery, SetOverOrFinalCloseEndsAClearedRejoinWithoutItsVerdict)
{
  for (const bool joining : {false, true})
  {
    for (const bool set_over : {false, true})
    {
      SCOPED_TRACE(fmt::format("joining={} set-over={}", joining, set_over));
      picojson::value welcome;
      ASSERT_TRUE(picojson::parse(welcome, QueueRoster("welcome", "ranked", "true", joining)).empty());
      welcome.get<picojson::object>()["matchId"] = picojson::value();
      const auto trace = TraceRoomForTest(QueueTraceOptions(joining), {
          {RK::Message, -3, QueueRoster("welcome", "ranked", "true", joining)},
          {RK::Message, -2, QueueRoster("start", "ranked", "true", joining)},
          {RK::Message, -1, PeerHello(joining)},
          {RK::SocketClose, 0, "both sockets dropped", 1006},
          {RK::Message, 1700, welcome.serialize()},
          set_over ? RoomTestStep{RK::Message, 1800, R"({"t":"set-over","ms":300000})"} :
                     RoomTestStep{RK::SocketClose, 1800, "match closed", 4004},
          {RK::InputSilence, 1801, "", 0},
          {RK::Tick, 100001},
      });
      EXPECT_EQ(trace[4].state, RoomState::Waiting);
      EXPECT_EQ(trace[5].state, RoomState::Ended);
      ASSERT_TRUE(trace[6].input_held.has_value());
      EXPECT_FALSE(*trace[6].input_held);
      EXPECT_EQ(trace.back().error_code, "match-over");
      EXPECT_FALSE(trace.back().session_recovering);
      EXPECT_FALSE(trace.back().reconnecting);
    }
  }
}

TEST(OrcaRoomRecovery, RejoinCannotRemoveTheOptInOrMoveToAnotherQueue)
{
  for (const std::string& welcome : {QueueRoster("welcome", "ranked", ""),
                                   QueueRoster("welcome", "casual")})
  {
    const auto trace = TraceRoomForTest(QueueTraceOptions(), {
        {RK::Message, 0, QueueRoster("welcome", "ranked")}, {RK::SocketClose, 1000},
        {RK::Message, 2000, welcome},
    });
    EXPECT_EQ(trace.back().state, RoomState::Ended);
    EXPECT_FALSE(trace.back().reconnecting);
    EXPECT_EQ(trace.back().error_code, "network");
    EXPECT_EQ(trace.back().status, "Lost the connection to the match. Press Play to look for a new one.");
  }
}

TEST(OrcaRoomRecovery, PublicOverlayCountsTheHoldAndTheServerVerdictWins)
{
  using namespace Orca::UX::SetEnd;
  for (const bool ranked : {false, true})
  {
    Model overlay;
    overlay.MatchBegan(ranked, "Peer", 0);
    overlay.GameBegan(0);
    overlay.RoomHold(91000);
    overlay.Stall(15000, 16000);
    const auto held = overlay.At(16000);
    ASSERT_TRUE(held.notice);
    EXPECT_EQ(held.notice->title, "Reconnecting…");
    EXPECT_EQ(held.notice->seconds, 75);
    EXPECT_TRUE(held.hides_waiting);
    ASSERT_TRUE(overlay.At(91000).notice);
    EXPECT_EQ(overlay.At(91000).notice->seconds, 0);
    overlay.RoomHold(std::nullopt);
    EXPECT_FALSE(overlay.At(17000).notice);
    overlay.RoomHold(91000);
    if (ranked)
      overlay.SetVerdict(Outcome::Lost, true, 0, 0, -1, -1, -1, 18000);
    else
      overlay.CasualGame(Outcome::Lost, 0, 1, 18000);
    const auto ended = overlay.At(18000);
    EXPECT_FALSE(ended.notice && ended.notice->title == "Reconnecting…");
    ASSERT_TRUE(ended.panel);
    EXPECT_EQ(ended.panel->out, Outcome::Lost);
  }
}

TEST(OrcaRoomResilience, AwayWithDirectInputsKeepsPeerAndBackDoesNotAnnounceAgain)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::DirectInput, 15000, "peer"}, {RK::Tick, 15000},
      {RK::Message, 15001, RosterMessage("back")}, {RK::Message, 15002, PeerPacket()},
      {RK::Message, 15003, RosterMessage("join")}, {RK::Message, 15004, PeerPacket()},
  });
  ASSERT_EQ(trace[1].events.size(), 1u);
  EXPECT_EQ(trace[1].events[0].kind, PeerEvent::Kind::Arrived);
  EXPECT_TRUE(trace[3].host_away_until);
  for (size_t i = 2; i < trace.size(); ++i)
  {
    EXPECT_TRUE(trace[i].events.empty()) << i;
    EXPECT_EQ(trace[i].state, RoomState::Waiting) << i;
  }
  EXPECT_FALSE(trace[6].host_away_until);
  ASSERT_EQ(trace[7].packets.size(), 1u);
  ASSERT_EQ(trace[9].packets.size(), 1u);
}

TEST(OrcaRoomResilience, SilentDirectInputsKeepTheSessionUntilTheRoomVerdict)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::Tick, 10999}, {RK::Tick, 11000}, {RK::Tick, 12000},
      {RK::Message, 12000, PeerPacket()},
      {RK::Message, 15000, RosterMessage("back")}, {RK::Message, 15001, PeerHello()},
  });
  EXPECT_TRUE(trace[4].events.empty());
  EXPECT_TRUE(trace[5].events.empty());
  EXPECT_TRUE(trace[5].host_away_until);
  EXPECT_EQ(trace[5].state, RoomState::Waiting);
  EXPECT_TRUE(trace[6].events.empty());
  EXPECT_EQ(trace[7].packets.size(), 1u);
  EXPECT_FALSE(trace[8].host_away_until);
  EXPECT_TRUE(trace[9].events.empty());
}

TEST(OrcaRoomResilience, RealLeaveEndsEvenWhenDirectInputIsLive)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::Message, 1001, RosterMessage("leave", true, false)},
      {RK::Message, 1002, PeerPacket()},
  });
  ASSERT_EQ(trace[4].events.size(), 1u);
  EXPECT_EQ(trace[4].events[0].kind, PeerEvent::Kind::Left);
  EXPECT_FALSE(trace[4].host_away_until);
  EXPECT_TRUE(trace[5].packets.empty());
}

TEST(OrcaRoomResilience, RealLeaveAlsoEndsTheSuspendedHostWait)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 1000, RosterMessage("away")}, {RK::Tick, 11000},
      {RK::Message, 12000, RosterMessage("leave", true, false)},
  });
  ASSERT_EQ(trace[4].events.size(), 1u);
  EXPECT_TRUE(trace[4].events[0].host);
  EXPECT_EQ(trace[4].events[0].kind, PeerEvent::Kind::Left);
  EXPECT_TRUE(trace[4].events[0].reason.empty());
  EXPECT_FALSE(trace[4].host_away_until);
}

TEST(OrcaRoomResilience, HoldExpiresEvenWithDirectInputsAndDuplicateAwayDoesNotExtendIt)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 1000, RosterMessage("away")},
      {RK::DirectInput, 89999, "peer"}, {RK::Message, 89999, RosterMessage("away")},
      {RK::DirectInput, 90999, "peer"}, {RK::Tick, 90999},
      {RK::DirectInput, 91000, "peer"}, {RK::Tick, 91000},
  });
  EXPECT_EQ(trace[2].host_away_until, trace[4].host_away_until);
  EXPECT_TRUE(trace[6].events.empty());
  ASSERT_EQ(trace[8].events.size(), 1u);
  EXPECT_EQ(trace[8].events[0].reason, "room-hold-expired");
}

TEST(OrcaRoomResilience, AwayPeerMissingFromARosterStillHasItsHeldSeat)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away", true, false)},
      {RK::Message, 1001, RosterMessage("back")}, {RK::Message, 1002, PeerPacket()},
  });
  EXPECT_TRUE(trace[3].events.empty());
  EXPECT_TRUE(trace[4].events.empty());
  ASSERT_EQ(trace[5].packets.size(), 1u);
}

TEST(OrcaRoomResilience, OwnSocketReattachesWithoutResettingALivePeer)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000},
      {RK::Message, 3000, RosterMessage("welcome")}, {RK::Message, 3001, PeerPacket()},
  });
  EXPECT_TRUE(trace[3].reconnect_started);
  EXPECT_TRUE(trace[3].reconnecting);
  EXPECT_EQ(trace[3].state, RoomState::Waiting);
  EXPECT_FALSE(trace[4].reconnecting);
  EXPECT_TRUE(trace[4].events.empty());
  ASSERT_EQ(trace[5].packets.size(), 1u);
}

TEST(OrcaRoomResilience, FinalSocketClosesEndButRelayOnlyRoomsCanRecoverTheirHeldSeat)
{
  for (unsigned code : {1008u, 4000u, 4001u, 4003u, 4004u})
  {
    const auto trace = TraceRoomForTest(TraceOptions(), {
        {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
        {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000, "", code},
    });
    EXPECT_FALSE(trace.back().reconnect_started);
  }
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 1000},
  });
  EXPECT_TRUE(trace.back().reconnect_started);
}

TEST(OrcaRoomResilience, RecoveryRefusesAChangedIdentityOrSeat)
{
  for (const std::string& welcome : {
       std::string(R"({"t":"welcome","me":"other","lifecycle":"game"})"),
       RosterMessage("welcome", false)})
  {
    const auto trace = TraceRoomForTest(TraceOptions(), {
        {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
        {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000},
        {RK::Message, 3000, welcome},
    });
    EXPECT_EQ(trace.back().state, RoomState::Ended);
  }
}

TEST(OrcaRoomResilience, ResumeHelloRequestsDropInOnce)
{
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, RosterMessage("welcome", false)}, {RK::Message, 0, PeerHello(false)},
      {RK::Message, 1, PeerPacket(1)},
      {RK::Message, 1000, PeerHello(false, true)}, {RK::Message, 1001, PeerPacket(1)},
      {RK::Message, 1002, PeerHello(false, true)},
  });
  ASSERT_EQ(trace[3].events.size(), 2u);
  EXPECT_EQ(trace[3].events[0].kind, PeerEvent::Kind::Left);
  EXPECT_EQ(trace[3].events[1].kind, PeerEvent::Kind::Arrived);
  EXPECT_TRUE(trace[5].events.empty());
}

TEST(OrcaRoomResilience, NewBootAfterBackRequiresFreshDropIn)
{
  const auto hello = [](const char* run) {
    return fmt::format(R"({{"t":"msg","from":"peer","d":{{"k":"hello","c":"test",
        "r":true,"h":true,"run":"{}"}}}})", run);
  };
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, hello("first")},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::Message, 1001, RosterMessage("back")}, {RK::Message, 1002, hello("second")},
  });
  EXPECT_TRUE(trace[4].events.empty());
  ASSERT_EQ(trace[5].events.size(), 2u);
  EXPECT_EQ(trace[5].events[0].kind, PeerEvent::Kind::Left);
  EXPECT_EQ(trace[5].events[0].reason, "room-resume");
  EXPECT_TRUE(trace[5].events[0].host);
  EXPECT_EQ(trace[5].events[1].kind, PeerEvent::Kind::Arrived);
}

TEST(OrcaRoomResilience, WaitingJoinerRequestsResumeOnBackAndReceivesAKeyframe)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 1000, RosterMessage("away")}, {RK::Tick, 11000},
      {RK::RetryPeer, 11001}, {RK::Tick, 11001},
      {RK::Message, 14000, RosterMessage("back")}, {RK::Tick, 14001},
      {RK::Message, 14002, PeerHello()}, {RK::Tick, 14003},
      {RK::Message, 14004, fmt::format(R"({{"t":"msg","from":"peer","d":{{"k":"kf",
          "f":300,"n":1,"id":"replay-300-01234567","x":"0123456789abcdef","key":"{}"}}}})", std::string(64, '0'))},
  });
  ASSERT_EQ(trace[5].events.size(), 1u);
  EXPECT_EQ(trace[5].events[0].reason, "room-resume");
  const auto hello = [](const RoomTestSnapshot& snapshot) {
    for (const std::string& text : snapshot.sent)
    {
      picojson::value message;
      EXPECT_TRUE(picojson::parse(message, text).empty());
      if (message.get("d").is<picojson::object>() && message.get("d").get("k").to_str() == "hello")
        return message.get("d");
    }
    return picojson::value();
  };
  EXPECT_TRUE(hello(trace[5]).is<picojson::null>()); // no rejoin before back
  EXPECT_TRUE(hello(trace[7]).get("resume").evaluate_as_boolean());
  EXPECT_FALSE(hello(trace[7]).get("r").evaluate_as_boolean());
  ASSERT_EQ(trace[8].events.size(), 1u);
  EXPECT_EQ(trace[8].events[0].kind, PeerEvent::Kind::Arrived);
  EXPECT_TRUE(hello(trace[9]).is<picojson::null>()); // next resume respects its backoff
  ASSERT_EQ(trace[10].events.size(), 1u);
  EXPECT_EQ(trace[10].events[0].kind, PeerEvent::Kind::Keyframe);
  EXPECT_EQ(trace[10].events[0].keyframe.id, "replay-300-01234567");
}

TEST(OrcaRoomResilience, ReconnectDeadlineAndInputTimeoutDoNotRestartOnRetry)
{
  const auto start = RoomClock::time_point{};
  RoomReconnect recovery;
  EXPECT_TRUE(recovery.Start(start, 0));
  EXPECT_TRUE(recovery.Start(start + 8s, 0));
  EXPECT_EQ(recovery.since, start);
  PeerRoomPresence peer;
  peer.Away(start);
  EXPECT_EQ(peer.Check(start + 9999ms, start), RoomLoss::None);
  EXPECT_EQ(peer.Check(start + 10s, start), RoomLoss::None);
  EXPECT_EQ(recovery.Check(start + 89999ms), RoomLoss::None);
  EXPECT_EQ(recovery.Check(start + 90s), RoomLoss::HoldExpired);
}

TEST(OrcaRoomResilience, JoinerWaitsForBackThenUsesFreshDropInBeforeTheHoldExpires)
{
  const auto start = RoomClock::time_point{};
  PeerRoomPresence host;
  host.Away(start);
  EXPECT_EQ(host.Check(start + 10s, start), RoomLoss::None);
  host.suspended = true;
  HostRoomWait wait;
  ASSERT_TRUE(wait.Start(start + 10s, host.Deadline()));
  EXPECT_FALSE(wait.Expired(start + 89s));
  // A suspended session must greet again and take a keyframe, rather than reuse its old state.
  EXPECT_FALSE(host.Back(start + 89s, true));
  wait.Back();
  EXPECT_FALSE(wait.until);
  EXPECT_FALSE(host.Deadline());

  host.Away(start);
  ASSERT_TRUE(wait.Start(start + 10s, host.Deadline()));
  EXPECT_TRUE(wait.Expired(start + 90s));
  EXPECT_FALSE(wait.Start(start + 90s, host.Deadline()));
}

TEST(OrcaRoom, PortValuesRoundTrip)
{
  std::vector<u8> controls(46);
  for (size_t i = 0; i < controls.size(); ++i)
    controls[i] = static_cast<u8>(i * 7);
  const std::vector<KeyframeInfo::Name> sent{
      {0, 12, "cy", {}}, {1, 3600, "sandbox-ada", controls}, {0, 3601, "cy", controls}};
  picojson::object message;
  message["nm"] = NamesToJson(sent);
  const std::string text = picojson::value(message).serialize();
  picojson::value parsed;
  ASSERT_TRUE(picojson::parse(parsed, text).empty());
  EXPECT_EQ(NamesFromJson(parsed.get<picojson::object>()), sent);
  // A port with no controls has no fourth element, matching the older message format.
  EXPECT_NE(text.find("[0,12,\"cy\"]"), std::string::npos) << text;

  // Controls past MAX_CONTROLS bytes, or not hex, are none; the name stays.
  const auto read = [](const std::string& json) {
    picojson::value v;
    EXPECT_TRUE(picojson::parse(v, json).empty()) << json;
    return NamesFromJson(v.get<picojson::object>());
  };
  const std::string long_hex(2 * (MAX_CONTROLS + 1), 'a');
  std::vector<KeyframeInfo::Name> got = read(R"({"nm":[[1,5,"ada",")" + long_hex + R"("]]})");
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "ada");
  EXPECT_TRUE(got[0].controls.empty());
  got = read(R"({"nm":[[1,5,"ada","zz"],[2,6,"bo",7],[2,6,"bo","0a0b"]]})");
  ASSERT_EQ(got.size(), 3u);
  EXPECT_TRUE(got[0].controls.empty());
  EXPECT_TRUE(got[1].controls.empty());
  EXPECT_EQ(got[2].controls, (std::vector<u8>{0x0a, 0x0b}));
  // Six elements, a bad seat: skipped.
  EXPECT_TRUE(read(R"({"nm":[[1,5,"ada","00","01",1],[9,5,"x"]]})").empty());
  // A queue identity (UX/Queue.h) is the fifth, after the controls (empty when none); one past
  // MAX_QUEUE bytes, or not hex, is none.
  const std::vector<u8> queue{1, 0x05, 0xFC, 0x10, 2, 0, 0, 0, 0, 0x41, 0x20, 0, 0};
  const std::vector<KeyframeInfo::Name> with_queue{{1, 40, "bo", {}, queue},
                                                   {1, 41, "bo", controls, queue}};
  picojson::object qm;
  qm["nm"] = NamesToJson(with_queue);
  const std::string qtext = picojson::value(qm).serialize();
  EXPECT_NE(qtext.find("[1,40,\"bo\",\"\",\"0105fc"), std::string::npos) << qtext;
  EXPECT_EQ(read(qtext), with_queue);
  const std::string long_queue(2 * (MAX_QUEUE + 1), 'b');
  got = read(R"({"nm":[[1,5,"ada","",")" + long_queue + R"("]]})");
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].queue.empty());

  // The players' acknowledgement of the newest values (Packet::values_ack).
  Packet packet = Sample();
  packet.values_ack = 7;
  Packet back;
  ASSERT_TRUE(DecodePacket(EncodePacket(packet), &back));
  EXPECT_EQ(back.values_ack, 7);
  EXPECT_FALSE(
      DecodePacket(R"({"k":"p","s":1,"f":0,"c":0,"p":"","a":[-1,-1,-1,-1],"va":-3})", &back));
}

TEST(OrcaRoom, RejectsMalformedPackets)
{
  Packet got;
  const std::string good = EncodePacket(Sample());
  ASSERT_TRUE(DecodePacket(good, &got));
  const auto with = [&](const std::string& from, const std::string& to) {
    std::string text = good;
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return at == std::string::npos ? text : text.replace(at, from.size(), to);
  };
  EXPECT_FALSE(DecodePacket("not json", &got));
  EXPECT_FALSE(DecodePacket("[]", &got));
  EXPECT_FALSE(DecodePacket(with("\"k\":\"p\"", "\"k\":\"q\""), &got));
  EXPECT_FALSE(DecodePacket(with("\"s\":1", "\"s\":4"), &got));
  EXPECT_FALSE(DecodePacket(with("\"s\":1", "\"s\":0.5"), &got));
  EXPECT_FALSE(DecodePacket(with("\"f\":120", "\"f\":-1"), &got));
  EXPECT_FALSE(DecodePacket(with("\"f\":120", "\"f\":1e300"), &got));
  EXPECT_FALSE(DecodePacket(with("\"c\":121", "\"c\":\"121\""), &got));
  // Pads: odd length, not hex, not whole pads.
  EXPECT_FALSE(DecodePacket(with("\"p\":\"00", "\"p\":\"0"), &got));
  EXPECT_FALSE(DecodePacket(with("\"p\":\"00", "\"p\":\"zz"), &got));
  EXPECT_FALSE(DecodePacket(with("\"p\":\"00", "\"p\":\"0000"), &got));
  EXPECT_FALSE(DecodePacket(with("[130,-1,-1,-1]", "[130,-1,-1]"), &got));
  EXPECT_FALSE(DecodePacket(with("[130,-1,-1,-1]", "[130,-2,-1,-1]"), &got));
  EXPECT_FALSE(DecodePacket(with("\"h\":\"fedcba9876543210\"", "\"h\":\"fedcba98\""), &got));
  // Bad telemetry must never refuse a packet, since that would end the match: out-of-range values
  // are clamped, fractions truncated, non-numbers ignored (an exponent reads as null).
  const auto telemetry = [&](const std::string& from, const std::string& to) {
    Packet out;
    EXPECT_TRUE(DecodePacket(with(from, to), &out)) << to;
    return out;
  };
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":-1").resimulated, 0);
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":4.5").resimulated, 4);
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":999999999999").resimulated, 1 << 20);
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":\"42\"").resimulated, -1);
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":-1.5").advantage, -1);
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":-999999999999").advantage, -(1 << 20));
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":null").advantage, std::nullopt);
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":1000").advantage, 1000);
  EXPECT_EQ(telemetry("\"l\":64", "\"l\":-5").rtt, 0);
  EXPECT_EQ(telemetry("\"l\":64", "\"l\":999999999999").rtt, 1 << 20);
  EXPECT_EQ(telemetry("\"l\":64", "\"l\":[]").rtt, std::nullopt);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":-1").stalls, 0);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":2.5").stalls, 2);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":999999999999").stalls, 1 << 20);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":\"3\"").stalls, -1);
  EXPECT_EQ(telemetry("\"j\":5", "\"j\":-2").hitches, 0);
  EXPECT_EQ(telemetry("\"j\":5", "\"j\":999999999999").hitches, 1 << 20);
  EXPECT_EQ(telemetry("\"j\":5", "\"j\":true").hitches, -1);
  using Spared = std::array<int, SPARED_FRAMES>;
  EXPECT_EQ(telemetry("[1,2,2,3]", "[-1,2.5,2,999999999999]").spared, (Spared{0, 2, 2, 1 << 20}));
  EXPECT_EQ(telemetry("[1,2,2,3]", "[1,2,2]").spared, std::nullopt);
  EXPECT_EQ(telemetry("[1,2,2,3]", "[1,2,2,3,4]").spared, std::nullopt);
  EXPECT_EQ(telemetry("[1,2,2,3]", "[1,\"2\",2,3]").spared, std::nullopt);
  EXPECT_EQ(telemetry("\"sp\":[1,2,2,3]", "\"sp\":7").spared, std::nullopt);

  // What would crash the JSON parser: deep nesting, and numbers that overflow a double.
  EXPECT_FALSE(DecodePacket(std::string(5000, '[') + std::string(5000, ']'), &got));
  EXPECT_FALSE(DecodePacket(with("\"c\":121", "\"c\":1e999"), &got));
  EXPECT_FALSE(DecodePacket(with("\"c\":121", "\"c\":" + std::string(400, '9')), &got));
  // Words with an e inside strings, and true/false, are fine.
  EXPECT_TRUE(DecodePacket(with("\"k\":\"p\"", "\"e\":\"eEe\",\"t\":true,\"u\":false,\"k\":\"p\""), &got));

  // More pads than a packet may carry.
  Packet big = Frames(0, 0, MAX_PADS_IN_PACKET + 1);
  EXPECT_FALSE(DecodePacket(EncodePacket(big), &got));
  big.pads.pop_back();
  EXPECT_TRUE(DecodePacket(EncodePacket(big), &got));
}

// A ranked game's start for the room (rooms/src/game-lobby.ts gameStart, ORCA.md "Matchmaking and
// results"): exactly the room's message, only in a ranked queue room with its match current and the
// set still open.
TEST(OrcaRoom, GameStartIsTheRoomsMessage)
{
  const std::string match = "6f1c2b9e-0d3a-4c55-9a7e-1b2c3d4e5f60";
  const std::string text = GameStartMessage("ranked", match, false, 4210);
  EXPECT_EQ(text, "{\"t\":\"game-start\",\"matchId\":\"6f1c2b9e-0d3a-4c55-9a7e-1b2c3d4e5f60\","
                  "\"id\":\"g4210\"}");
  picojson::value v;
  ASSERT_TRUE(picojson::parse(v, text).empty());
  ASSERT_TRUE(v.is<picojson::object>());
  const picojson::object& o = v.get<picojson::object>();
  EXPECT_EQ(o.size(), 3u);
  EXPECT_EQ(o.at("t").get<std::string>(), "game-start");
  EXPECT_EQ(o.at("matchId").get<std::string>(), match);
  // The room's game id rule: 1-64 letters, digits, _ or -.
  EXPECT_EQ(o.at("id").get<std::string>(), "g4210");
  // A match id is a JSON string whatever it holds.
  ASSERT_TRUE(picojson::parse(v, GameStartMessage("ranked", "a\"b", false, 1)).empty());
  EXPECT_EQ(v.get<picojson::object>().at("matchId").get<std::string>(), "a\"b");
  // None outside a ranked queue room, without a room match, once the set is decided, or without a
  // fight frame.
  EXPECT_EQ(GameStartMessage("casual", match, false, 4210), "");
  EXPECT_EQ(GameStartMessage("private", match, false, 4210), "");
  EXPECT_EQ(GameStartMessage("ranked", "", false, 4210), "");
  EXPECT_EQ(GameStartMessage("ranked", match, true, 4210), "");
  EXPECT_EQ(GameStartMessage("ranked", match, false, -1), "");
}

// A host's `drop` in a ranked set still being decided keeps the joiner in the room to report the
// match void and wait for the verdict; anywhere else the joiner's room ends as before.
TEST(OrcaRoom, ADropMidSetKeepsTheRoom)
{
  EXPECT_TRUE(DropKeepsRoom("ranked", true, false));
  EXPECT_FALSE(DropKeepsRoom("ranked", false, false));
  EXPECT_FALSE(DropKeepsRoom("ranked", true, true));
  EXPECT_FALSE(DropKeepsRoom("casual", true, false));
  EXPECT_FALSE(DropKeepsRoom("private", true, false));
}

// A player whose room link dropped mid set comes back on a fresh room in the same match: the games
// it knew of come back with it (a joiner's keyframe may then hold that many, OnlineRules
// SetFitsJoin), while a match it never played starts from none. Only the most it knew counts: a
// fresh room's lower count never lowers it.
TEST(OrcaRoom, AReconnectKeepsTheGamesItKnew)
{
  const std::string match = "6f1c2b9e-0d3a-4c55-9a7e-reconnect01";
  EXPECT_EQ(RememberKnownSetGames(match, 0), 0);
  EXPECT_EQ(RememberKnownSetGames(match, 1), 1);
  EXPECT_EQ(RememberKnownSetGames(match, 2), 2);
  // The fresh room after the drop knows nothing yet.
  EXPECT_EQ(RememberKnownSetGames(match, 0), 2);
  EXPECT_EQ(RememberKnownSetGames(match, 1), 2);
  // Another match starts from what it saw.
  EXPECT_EQ(RememberKnownSetGames("another-match-0002", 0), 0);
  // No match: nothing kept.
  EXPECT_EQ(RememberKnownSetGames("", 3), 3);
  EXPECT_EQ(RememberKnownSetGames("", 0), 0);
  // A few matches are kept, the oldest go first.
  for (int i = 0; i < 20; ++i)
    RememberKnownSetGames(fmt::format("filler-{}", i), 1);
  EXPECT_EQ(RememberKnownSetGames(match, 0), 0);
}

// The void a room's match hears is exactly the room's `finish` (rooms/src/game-lobby.ts).
TEST(OrcaRoom, FinishIsTheRoomsMessage)
{
  EXPECT_EQ(FinishMessage("m1", ""), "{\"t\":\"finish\",\"matchId\":\"m1\",\"void\":true}");
  EXPECT_EQ(FinishMessage("m1", "u2"), "{\"t\":\"finish\",\"matchId\":\"m1\",\"winner\":\"u2\"}");
  picojson::value v;
  ASSERT_TRUE(picojson::parse(v, FinishMessage("a\"b", "")).empty());
  EXPECT_EQ(v.get<picojson::object>().at("matchId").get<std::string>(), "a\"b");
}

// A keep-alive goes only when the emulation thread asked since the last one, and at most every
// KEEP_ALIVE_EVERY: a game whose emulation froze stops sending them.
TEST(OrcaRoom, KeepAliveOnlyWhileTheGameRuns)
{
  const RoomClock::time_point t0 = RoomClock::now();
  EXPECT_FALSE(KeepAliveDue(t0, std::nullopt, std::nullopt));
  EXPECT_TRUE(KeepAliveDue(t0, t0, std::nullopt));
  // Asked again right after one went: wait for the interval.
  EXPECT_FALSE(
      KeepAliveDue(t0 + std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(400), t0));
  EXPECT_TRUE(KeepAliveDue(t0 + KEEP_ALIVE_EVERY, t0 + std::chrono::milliseconds(1900), t0));
  // Nothing asked since the last one (the emulation thread stopped): none, however long.
  EXPECT_FALSE(KeepAliveDue(t0 + std::chrono::seconds(60), t0, t0));
}

// A value the parser can't take (another player's, echoed in a server message such as the welcome)
// costs only that value: the rest of the message is still read, so a welcome still names this
// player and its room. Nothing deep or huge reaches picojson.
TEST(OrcaRoom, AnUnreadableValueCostsOnlyThatValue)
{
  const std::string head =
      R"({"t":"welcome","me":"c1","lifecycle":"game","room":{"code":"abc123"},)";
  std::string deep_object = "1";
  for (int i = 0; i < 3000; ++i)
    deep_object = R"({"a":)" + deep_object + "}";
  const std::string deep_array = std::string(5000, '[') + std::string(5000, ']');
  for (const std::string& bad :
       {std::string("1e999"), std::string("-2E+308"), std::string(400, '9'),
        std::string("0.") + std::string(40, '1'), deep_array, deep_object})
  {
    SCOPED_TRACE(bad.substr(0, 16));
    picojson::value v;
    ASSERT_TRUE(ParseRoomJson(head + R"("state":{"x":)" + bad + R"(,"y":2},"n":3})", &v));
    ASSERT_TRUE(v.is<picojson::object>());
    const picojson::object& o = v.get<picojson::object>();
    EXPECT_EQ(o.at("t").get<std::string>(), "welcome");
    EXPECT_EQ(o.at("me").get<std::string>(), "c1");
    EXPECT_EQ(o.at("room").get<picojson::object>().at("code").get<std::string>(), "abc123");
    EXPECT_EQ(o.at("n").get<double>(), 3);
    const picojson::object& state = o.at("state").get<picojson::object>();
    // A number goes whole; a deep value keeps its first levels, and what is past them goes.
    if (bad != deep_array && bad != deep_object)
      EXPECT_TRUE(state.at("x").is<picojson::null>());
    EXPECT_EQ(state.at("y").get<double>(), 2);
  }
  // Strings are left alone, escapes and brackets inside them included.
  picojson::value v;
  ASSERT_TRUE(ParseRoomJson(R"({"s":"1e999 [[[ \" }","d":-12.5,"t":true,"f":false,"z":null})", &v));
  EXPECT_EQ(v.get<picojson::object>().at("s").get<std::string>(), "1e999 [[[ \" }");
  EXPECT_EQ(v.get<picojson::object>().at("d").get<double>(), -12.5);
  // What never closes is still refused.
  EXPECT_FALSE(ParseRoomJson(R"({"s":"open)", &v));
  EXPECT_FALSE(ParseRoomJson(std::string(40, '[') + "1", &v));
}

TEST(OrcaRoom, CoalescingKeepsEarlierInputsAndChecksum)
{
  std::optional<Packet> pending;
  Packet first = Frames(0, 10, 3);  // frames 10-12
  first.checksum_frame = 0;
  first.checksum = 42;
  CoalescePacket(&pending, first);
  // The next one starts later (frames 12-14; the peer acknowledged up to 11 meanwhile, as far as
  // the sender knows) and carries no checksum.
  CoalescePacket(&pending, Frames(0, 12, 3));
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->first_frame, 10);
  ASSERT_EQ(pending->pads.size(), 5u);
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(pending->pads[i][0], 10 + i);
  EXPECT_EQ(pending->checksum_frame, 0);
  EXPECT_EQ(pending->checksum, 42u);

  // A packet that starts at or before the pending one simply replaces it.
  CoalescePacket(&pending, Frames(0, 8, 9));
  EXPECT_EQ(pending->first_frame, 8);
  EXPECT_EQ(pending->pads.size(), 9u);
  EXPECT_EQ(pending->checksum_frame, 0);

  // Never more than a packet may carry: the newest inputs win.
  pending = Frames(0, 0, 40);
  CoalescePacket(&pending, Frames(0, 30, 40));  // frames 30-69
  EXPECT_EQ(pending->pads.size(), static_cast<size_t>(MAX_PADS_IN_PACKET));
  EXPECT_EQ(pending->first_frame, 70 - MAX_PADS_IN_PACKET);
  EXPECT_EQ(pending->pads.front()[0], 70 - MAX_PADS_IN_PACKET);
}

// YouGame's remote switch for direct links: the ticket reply's top-level `direct`, off only when
// it is false; a site that sends none (or something else) leaves them on.
TEST(OrcaRoom, TicketSwitchesDirectLinks)
{
  const auto reply = [](const char* text) {
    picojson::value value;
    EXPECT_TRUE(picojson::parse(value, text).empty()) << text;
    return value.get<picojson::object>();
  };
  EXPECT_TRUE(TicketAllowsDirect(reply(R"({"url":"wss://rooms.yougame.co","ticket":"t"})")));
  EXPECT_TRUE(TicketAllowsDirect(reply(R"({"ticket":"t","direct":true})")));
  EXPECT_FALSE(TicketAllowsDirect(reply(R"({"ticket":"t","direct":false})")));
  EXPECT_TRUE(TicketAllowsDirect(reply(R"({"ticket":"t","direct":0})")));
  EXPECT_TRUE(TicketAllowsDirect(reply(R"({"ticket":"t","direct":"false"})")));
  EXPECT_TRUE(TicketAllowsDirect(reply(R"({"ticket":"t","ice":{"direct":false}})")));
}

TEST(OrcaRoom, RefusesToStartOutsideTheApp)
{
  // No bridge and no dev game: the room ends at once with a reason, without touching the network.
  if (std::getenv("YOUGAME_BRIDGE") || std::getenv("ORCA_TEST_DEV_GAME"))
    GTEST_SKIP() << "a bridge or dev game is configured";
  YouGameRoom room({});
  for (int i = 0; i < 100 && room.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(room.GetState(), RoomState::Ended);
  EXPECT_FALSE(room.Connected());
  EXPECT_FALSE(room.Status().empty());
}

// Waits until `room` reports another player arrived; returns its event.
std::optional<PeerEvent> WaitArrived(YouGameRoom& room)
{
  for (int i = 0; i < 1500 && room.GetState() != RoomState::Ended; ++i)
  {
    for (const PeerEvent& event : room.TakePeerEvents())
    {
      if (event.kind == PeerEvent::Kind::Arrived)
        return event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

// A host and a joiner in one dev room on the real rooms Worker: each sees the other arrive on its
// seat, a packet and a keyframe offer cross, a forged packet is dropped, and the host sees the
// joiner leave while its own room stays. ORCA_LIVE_TEST=1 (ORCA_SITE picks another site).
TEST(OrcaLive, TwoRoomsPlayEachOther)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  const std::string code =
      "orca" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
  RoomOptions options;
  options.room_code = code;
  options.compatibility = "orca-live-test";
  options.player_name = "Orca A";
  YouGameRoom a(options);
  // The first in is the host; give it a head start so the seats are predictable.
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  EXPECT_EQ(a.Joining(), false);
  options.player_name = "Orca B";
  options.joining = true;
  YouGameRoom b(options);
  const auto b_seen = WaitArrived(a);
  const auto a_seen = WaitArrived(b);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  ASSERT_TRUE(b_seen) << a.Status();
  ASSERT_TRUE(a_seen) << b.Status();
  EXPECT_EQ(b.Joining(), true);
  EXPECT_EQ(a.Seat(), 0);
  EXPECT_EQ(b.Seat(), 1);
  EXPECT_EQ(b_seen->seat, 1);
  EXPECT_FALSE(b_seen->host);
  EXPECT_EQ(a_seen->seat, 0);
  EXPECT_TRUE(a_seen->host);
  EXPECT_EQ(a.Code(), code);

  KeyframeInfo offer{120, "replay-120-00112233", 1234, "00112233aabbccdd",
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
  a.OfferKeyframe(1, offer);
  std::optional<PeerEvent> offered;
  for (int i = 0; i < 500 && !offered; ++i)
  {
    for (const PeerEvent& event : b.TakePeerEvents())
    {
      if (event.kind == PeerEvent::Kind::Keyframe)
        offered = event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(offered);
  EXPECT_EQ(offered->keyframe.id, offer.id);
  EXPECT_EQ(offered->keyframe.frame, offer.frame);
  EXPECT_EQ(offered->keyframe.size, offer.size);
  EXPECT_EQ(offered->keyframe.hash, offer.hash);
  EXPECT_EQ(offered->keyframe.key, offer.key);

  Packet sent = Sample();
  sent.seat = a.Seat();
  a.Send(sent);
  // Through the relay, and over a direct link too once one is up (DirectLink): the same packet.
  std::vector<Packet> got;
  for (int i = 0; i < 500 && got.size() < 2 && (got.empty() || i < 100); ++i)
  {
    for (Packet& packet : b.Receive())
      got.push_back(std::move(packet));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_GE(got.size(), 1u);
  ASSERT_LE(got.size(), 2u);
  for (const Packet& packet : got)
    ExpectSame(sent, packet);
  // A's seat speaking as B's is dropped, on either path.
  Packet forged = Sample();
  forged.seat = b.Seat();
  a.Send(forged);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  for (const Packet& packet : b.Receive())
    EXPECT_NE(packet.seat, b.Seat());
  EXPECT_TRUE(b.Connected());

  // The joiner leaves; the host's room stays, and says who left.
  b.Leave();
  std::optional<PeerEvent> left;
  for (int i = 0; i < 500 && !left; ++i)
  {
    for (const PeerEvent& event : a.TakePeerEvents())
    {
      if (event.kind == PeerEvent::Kind::Left)
        left = event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(left);
  EXPECT_EQ(left->seat, 1);
  EXPECT_TRUE(a.Connected()) << a.Status();
  EXPECT_GE(a.RoundTripMs(), 0);
}

// Two Orcas in one dev room open a direct link that beats the relay; blocking it falls back to the
// relay and unblocking restores it. A third Orca with ORCA_DIRECT=0 never links (its room reports
// the relay). ORCA_LIVE_TEST=1.
TEST(OrcaLive, DirectLinkBetweenTwoRooms)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  const ScopedEnv dev_game("ORCA_TEST_DEV_GAME", "orca-live-test");
  const ScopedEnv loopback("ORCA_DIRECT_LOOPBACK", "1");
  const auto wait_for = [](const std::function<bool()>& done, int ms) {
    for (int waited = 0; waited < ms && !done(); waited += 10)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return done();
  };
  const std::string code =
      "orca" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
  RoomOptions options;
  options.room_code = code;
  options.compatibility = "orca-live-test";
  options.player_name = "Orca A";
  YouGameRoom a(options);
  ASSERT_TRUE(wait_for([&] { return a.GetState() != RoomState::Connecting; }, 5000));
  options.player_name = "Orca B";
  options.joining = true;
  YouGameRoom b(options);
  ASSERT_TRUE(WaitArrived(a));
  ASSERT_TRUE(WaitArrived(b));
  ASSERT_TRUE(wait_for([&] { return a.Direct().transport == 1 && b.Direct().transport == 1; },
                       10000))
      << a.Direct().line << " / " << b.Direct().line;
  ASSERT_TRUE(wait_for([&] { return a.Direct().rtt_ms >= 0; }, 3000));
  std::printf("[ direct ] A: %s, B: %s, relay round trip %d ms\n", a.Direct().line.c_str(),
              b.Direct().line.c_str(), a.RoundTripMs());
  EXPECT_LE(a.Direct().rtt_ms, 20);

  // The direct copy first, the relay's after.
  Packet sent = Frames(a.Seat(), 100, 4);
  sent.sequence = 1;
  const auto start = std::chrono::steady_clock::now();
  a.Send(sent);
  std::vector<Packet> got;
  std::optional<double> first_ms;
  for (int i = 0; i < 3000 && got.size() < 2; ++i)
  {
    for (Packet& packet : b.Receive())
    {
      if (!first_ms)
      {
        first_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             start)
                       .count();
      }
      got.push_back(std::move(packet));
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  ASSERT_EQ(got.size(), 2u);
  for (const Packet& packet : got)
    EXPECT_EQ(packet.pads, sent.pads);
  const double second_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("[ direct ] first copy after %.1f ms, the relay's after %.1f ms\n", *first_ms,
              second_ms);
  EXPECT_LT(*first_ms, std::max(5.0, second_ms / 2));

  // Datagrams blocked: both see the relay again; lifted: the link is back.
  a.TestDirect("off");
  EXPECT_TRUE(wait_for([&] { return a.Direct().transport == 0 && b.Direct().transport == 0; },
                       3000));
  a.TestDirect("on");
  EXPECT_TRUE(wait_for([&] { return a.Direct().transport == 1 && b.Direct().transport == 1; },
                       10000))
      << a.Direct().line << " / " << b.Direct().line;

  // The player's switch ("direct off" from an app with the "direct" cap): both rooms of this
  // process go back to the relay at once, and come back when it is on again. Without the cap it
  // counts for nothing.
  Orca::Status::SetAppCaps("");
  Orca::Online::SetDirectWanted(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  EXPECT_EQ(a.Direct().transport, 1);
  Orca::Status::SetAppCaps("direct");
  EXPECT_TRUE(wait_for([&] { return a.Direct().transport == 0 && b.Direct().transport == 0; },
                       3000));
  Orca::Online::SetDirectWanted(true);
  EXPECT_TRUE(wait_for([&] { return a.Direct().transport == 1 && b.Direct().transport == 1; },
                       10000))
      << a.Direct().line << " / " << b.Direct().line;
  Orca::Status::SetAppCaps("");

  // A third Orca without direct links: the others link to each other, never to it.
  {
    const ScopedEnv off("ORCA_DIRECT", "0");
    options.player_name = "Orca C";
    YouGameRoom c(options);
    ASSERT_TRUE(WaitArrived(c));
    EXPECT_TRUE(wait_for([&] { return a.Direct().transport == 0; }, 5000)) << a.Direct().line;
    EXPECT_TRUE(wait_for([&] { return b.Direct().transport == 0; }, 5000)) << b.Direct().line;
    EXPECT_EQ(c.Direct().transport, 0);
    c.Leave();
  }
  EXPECT_TRUE(wait_for([&] { return a.Direct().transport == 1; }, 5000)) << a.Direct().line;
  b.Leave();
  a.Leave();
}

// A friend arriving is visible before the emulator thread takes it (what ends a solo pause, which
// takes no events while paused), and no longer once taken. ORCA_LIVE_TEST=1.
TEST(OrcaLive, ArrivalIsPendingUntilTaken)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  const ScopedEnv dev_game("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcap" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  YouGameRoom a(options);
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  EXPECT_FALSE(a.ArrivalPending());
  options.joining = true;
  YouGameRoom b(options);
  for (int i = 0; i < 1500 && !a.ArrivalPending() && a.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_TRUE(a.ArrivalPending()) << a.Status();
  bool arrived = false;
  for (const PeerEvent& event : a.TakePeerEvents())
    arrived = arrived || event.kind == PeerEvent::Kind::Arrived;
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(a.ArrivalPending());
}

// A joiner hears its host leave the room as a pending event (OnlineMatch ends a stall on its host's
// inputs on it, rather than wait out the silence limit), and not before.
TEST(OrcaLive, HostLeavingIsPendingUntilTaken)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  const ScopedEnv dev_game("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcah" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  auto a = std::make_unique<YouGameRoom>(options);
  for (int i = 0; i < 500 && a->GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a->GetState(), RoomState::Waiting) << a->Status();
  options.joining = true;
  YouGameRoom b(options);
  ASSERT_TRUE(WaitArrived(*a)) << a->Status();
  const auto host = WaitArrived(b);
  ASSERT_TRUE(host) << b.Status();
  EXPECT_TRUE(host->host);
  EXPECT_FALSE(b.HostLeftPending());
  // The host leaves: its room thread says goodbye on the way out.
  a.reset();
  for (int i = 0; i < 1000 && !b.HostLeftPending(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_TRUE(b.HostLeftPending()) << b.Status();
  bool left = false;
  for (const PeerEvent& event : b.TakePeerEvents())
    left = left || (event.kind == PeerEvent::Kind::Left && event.host);
  EXPECT_TRUE(left);
  EXPECT_FALSE(b.HostLeftPending());
}

// The test network delay: one side holds every message 100 ms each way, the other sets the variable
// to 0 (both are then test processes, so their keys match). The delayed side's round trip shows it.
TEST(OrcaLive, TestNetworkDelayHoldsMessages)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_SESSION", "1");
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  const std::string code =
      "orcad" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  RoomOptions options;
  options.room_code = code;
  options.compatibility = "orca-live-test";
  SetEnv("ORCA_TEST_NET_DELAY_MS", "100");
  YouGameRoom a(options);
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_NET_DELAY_MS", "0");
  options.joining = true;
  YouGameRoom b(options);
  WaitArrived(a);
  WaitArrived(b);
  SetEnv("ORCA_TEST_NET_DELAY_MS", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  SetEnv("ORCA_SESSION", nullptr);
  ASSERT_TRUE(a.Connected()) << a.Status();
  ASSERT_TRUE(b.Connected()) << b.Status();

  // A packet from b reaches a's inbox no sooner than a's inbound delay.
  const auto sent_at = std::chrono::steady_clock::now();
  Packet sent = Sample();
  sent.seat = b.Seat();
  b.Send(sent);
  std::vector<Packet> got;
  for (int i = 0; i < 300 && got.empty(); ++i)
  {
    got = a.Receive();
    if (got.empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ(got.size(), 1u);
  EXPECT_GE(std::chrono::steady_clock::now() - sent_at, std::chrono::milliseconds(100));
  // Pings go out held and pongs come back held: the round trip includes both delays.
  for (int i = 0; i < 300 && a.RoundTripMs() < 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_GE(a.RoundTripMs(), 200);
  EXPECT_LT(b.RoundTripMs(), 200);
}

// Under the desktop app: one room goes through the app's bridge (YOUGAME_BRIDGE, YOUGAME_TOKEN,
// YOUGAME_GAME, pointed at Tools/orca/fake_bridge.py), the other uses a dev ticket. The bridged
// side mirrors the room to the page, and the page's Leave ends it. ORCA_LIVE_TEST=1 and
// ORCA_TEST_BRIDGE_ROOM=<the fake bridge's room code>.
TEST(OrcaLive, BridgeMirrorsTheRoomAndTakesLeave)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  const char* room = std::getenv("ORCA_TEST_BRIDGE_ROOM");
  if (!live || std::string(live) != "1" || !room || !std::getenv("YOUGAME_BRIDGE"))
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1, ORCA_TEST_BRIDGE_ROOM and a bridge";
  RoomOptions options;
  options.compatibility = "orca-live-test";
  options.mode = "orca-test";
  // The app's invite names the room (hello.room).
  YouGameRoom a(options);
  for (int i = 0; i < 1000 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  EXPECT_EQ(a.Code(), room);

  // The friend joins without the app. (Not as a host: a hosting Orca that finds someone else
  // hosting its room gives it up.)
  const std::string bridge = std::getenv("YOUGAME_BRIDGE");
  SetEnv("YOUGAME_BRIDGE", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", "orca-bridge-test");
  options.room_code = room;
  options.joining = true;
  YouGameRoom b(options);
  // The room reads its environment on its own thread: restore it once b has its ticket.
  for (int i = 0; i < 1000 && b.Code().empty() && b.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  SetEnv("YOUGAME_BRIDGE", bridge.c_str());
  for (int i = 0; i < 1500 && !(a.Connected() && b.Connected()); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_TRUE(a.Connected()) << a.Status();
  ASSERT_TRUE(b.Connected()) << b.Status();

  // The fake bridge sends the page's Leave two seconds after the room reads "ready". Without the
  // app's "caps leave" (none here), it ends the room.
  for (int i = 0; i < 1000 && a.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(a.GetState(), RoomState::Ended);
  EXPECT_EQ(a.Status(), "You left the room");
  EXPECT_EQ(a.ErrorCode(), "");
  for (int i = 0; i < 500 && b.Connected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(b.Connected()) << b.Status();

  // The page's last word on the room is the room with nobody in it (Orca left it), so a later
  // invite into it isn't taken for one Orca is already in. FAKE_LOG: the bridge's log, if given.
  a.Leave();
  if (const char* log = std::getenv("FAKE_LOG"))
  {
    std::ifstream in(log);
    std::string line, last;
    while (std::getline(in, line))
    {
      if (line.find("\"mpRoom\"") != std::string::npos)
        last = line;
    }
    EXPECT_NE(last.find("\"players\": []"), std::string::npos) << last;
    EXPECT_NE(last.find("\"participants\": []"), std::string::npos) << last;
    EXPECT_NE(last.find(std::string("\"code\": \"") + room + "\""), std::string::npos) << last;
  }
}

TEST(OrcaRoom, RefusalsSayWhy)
{
  // WebSocket::Connect's error carries the upgrade's HTTP status.
  EXPECT_NE(RefusalReason("WebSocket connect failed: ... (HTTP 403)").find("differs"),
            std::string::npos);
  EXPECT_EQ(RefusalReason("WebSocket connect failed: ... (HTTP 403) lobby_mismatch"),
            "That room is set up for a different game or mode, so your game opened its own room.");
  EXPECT_EQ(RefusalReason("WebSocket connect failed: ... (HTTP 409)"), "That room is full");
  EXPECT_NE(RefusalReason("WebSocket connect failed: ... (HTTP 401)").find("expired"),
            std::string::npos);
  EXPECT_EQ(RefusalReason("WebSocket connect failed: Couldn't connect to server (HTTP 0)"),
            "Could not reach YouGame's rooms");
}

// Two Orcas whose compatibility keys differ never share a room: the second is told why.
TEST(OrcaLive, DifferentKeysAreRefusedWithAReason)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcak" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-key-a";
  YouGameRoom a(options);
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  options.compatibility = "orca-key-b";
  YouGameRoom b(options);
  for (int i = 0; i < 1000 && b.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  EXPECT_EQ(b.GetState(), RoomState::Ended);
  EXPECT_NE(b.Status().find("differs"), std::string::npos) << b.Status();
  EXPECT_EQ(b.ErrorCode(), "room_mismatch");
  EXPECT_EQ(a.GetState(), RoomState::Waiting);
  EXPECT_EQ(a.ErrorCode(), "");
  a.Leave();
  EXPECT_EQ(a.ErrorCode(), "");
}

TEST(OrcaRoom, RefusalsHaveAppCodes)
{
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 403)"), "room_mismatch");
  for (const std::string reason : {"host_older", "host_newer", "lobby_mismatch"})
    EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 403) " + reason), "room_mismatch");
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 409)"), "room_full");
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 0)"), "network");
  // Only the rooms Worker's host_gone code means the friend left; other 410s (a closed
  // tournament match, a bare 410) keep their old handling.
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 410) host_gone"), "host_gone");
  EXPECT_NE(RefusalCode("WebSocket connect failed: ... (HTTP 410)"), "host_gone");
}

// A desync is reported as one on both sides, never as a friend who left. Under drop-in play:
// a joiner that leaves on a desync says so in its bye, and the host's room reports the joiner's
// departure with that reason; a host that unplugs a joiner for a desync tells it, and the joiner's
// room ends with the desync code; a host that leaves on a desync says so to whoever is left.
TEST(OrcaLive, DesyncIsReportedOnBothSides)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcab" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  YouGameRoom host(options);
  for (int i = 0; i < 500 && host.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(host.GetState(), RoomState::Waiting) << host.Status();
  options.joining = true;
  const auto left_reason = [&host]() -> std::optional<std::string> {
    for (int i = 0; i < 500; ++i)
    {
      for (const PeerEvent& event : host.TakePeerEvents())
      {
        if (event.kind == PeerEvent::Kind::Left)
          return event.reason;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
  };

  // 1. The joiner leaves on a desync.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    joiner.SetLeaveReason("desync");
    joiner.Leave();
    EXPECT_EQ(left_reason(), std::optional<std::string>("desync"));
    EXPECT_EQ(joiner.ErrorCode(), "");
  }
  // A plain leave carries no reason.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    joiner.Leave();
    EXPECT_EQ(left_reason(), std::optional<std::string>(""));
  }
  // 2. The host unplugs the joiner for a desync.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    host.DropPeer(joiner.Seat(), "desync");
    for (int i = 0; i < 500 && joiner.GetState() != RoomState::Ended; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(joiner.GetState(), RoomState::Ended);
    EXPECT_EQ(joiner.ErrorCode(), "desync") << joiner.Status();
    joiner.Leave();
    left_reason();
  }
  // 3. The host leaves on a desync: the joiner hears the reason with the host's departure.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    host.SetLeaveReason("desync");
    host.Leave();
    std::optional<PeerEvent> gone;
    for (int i = 0; i < 500 && !gone; ++i)
    {
      for (const PeerEvent& event : joiner.TakePeerEvents())
      {
        if (event.kind == PeerEvent::Kind::Left)
          gone = event;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(gone);
    EXPECT_TRUE(gone->host);
    EXPECT_EQ(gone->reason, "desync");
    EXPECT_EQ(host.ErrorCode(), "");
  }
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
}

// A queue-room host whose game never carried the room's header leaves with "mismatch"
// (OnlineMatch's FailHeaderWait), and the waiting joiner hears that reason. A joiner's own
// "mismatch" bye (LeaveOnMismatch) is just a plain leave to the host.
TEST(OrcaLive, AHostsMismatchReachesItsJoiner)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcam" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  YouGameRoom host(options);
  for (int i = 0; i < 500 && host.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(host.GetState(), RoomState::Waiting) << host.Status();
  options.joining = true;

  // 1. The joiner leaves on a mismatch: the host hears a plain leave.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    joiner.SetLeaveReason("mismatch");
    joiner.Leave();
    std::optional<std::string> reason;
    for (int i = 0; i < 500 && !reason; ++i)
    {
      for (const PeerEvent& event : host.TakePeerEvents())
      {
        if (event.kind == PeerEvent::Kind::Left)
          reason = event.reason;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(reason, std::optional<std::string>(""));
  }
  // 2. The host leaves on a mismatch: the joiner hears the reason with the host's departure.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    host.SetLeaveReason("mismatch");
    host.Leave();
    std::optional<PeerEvent> gone;
    for (int i = 0; i < 500 && !gone; ++i)
    {
      for (const PeerEvent& event : joiner.TakePeerEvents())
      {
        if (event.kind == PeerEvent::Kind::Left)
          gone = event;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(gone);
    EXPECT_TRUE(gone->host);
    EXPECT_EQ(gone->reason, "mismatch");
    EXPECT_EQ(host.ErrorCode(), "");
  }
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
}

// A player the room re-seats on a new socket ("back": its old one died without the room noticing)
// is another run of Orca: the host unplugs the old one and greets the new one, which can then get
// its own keyframe. A joiner whose host is there doesn't give up on it.
TEST(OrcaLive, ReturningPlayerIsGreetedAgain)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  const std::string stamp =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  RoomOptions options;
  options.room_code = "orcab" + stamp;
  options.compatibility = "orca-live-test";
  YouGameRoom host(options);
  for (int i = 0; i < 500 && host.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(host.GetState(), RoomState::Waiting) << host.Status();
  options.joining = true;
  const std::string player = "orca-back-" + stamp;
  SetEnv("ORCA_TEST_PLAYER", player.c_str());
  auto first = std::make_unique<YouGameRoom>(options);
  const auto arrived = WaitArrived(host);
  ASSERT_TRUE(arrived) << host.Status();
  EXPECT_EQ(arrived->seat, 1);

  // The same player from a new socket: the room replaces the old one and says "back".
  YouGameRoom second(options);
  for (int i = 0; i < 1000 && second.Code().empty() && second.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_PLAYER", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  std::vector<PeerEvent::Kind> seen;
  for (int i = 0; i < 1500 && (seen.empty() || seen.back() != PeerEvent::Kind::Arrived); ++i)
  {
    for (const PeerEvent& event : host.TakePeerEvents())
    {
      EXPECT_EQ(event.seat, 1);
      seen.push_back(event.kind);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(seen.size(), 2u) << host.Status();
  EXPECT_EQ(seen[0], PeerEvent::Kind::Left);
  EXPECT_EQ(seen[1], PeerEvent::Kind::Arrived);
  EXPECT_TRUE(second.Connected()) << second.Status();
  // Its host said hello: it waits as long as the keyframe takes.
  std::this_thread::sleep_for(std::chrono::seconds(9));
  EXPECT_TRUE(second.Connected()) << second.Status();
  EXPECT_TRUE(host.Connected()) << host.Status();
}

// A friend who reaches the room before its host (an invite sent before the host's game got there)
// holds the room's first seat. The host's room ends ("taken": its game opens another), and the
// friend, with nobody's game to join, gives up soon after its welcome instead of waiting minutes.
TEST(OrcaLive, HostSecondInItsRoomGivesItUp)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcat" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  options.joining = true;
  YouGameRoom joiner(options);
  for (int i = 0; i < 500 && joiner.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(joiner.GetState(), RoomState::Waiting) << joiner.Status();
  const auto welcomed = std::chrono::steady_clock::now();
  options.joining = false;
  YouGameRoom host(options);
  for (int i = 0; i < 1000 && host.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  EXPECT_EQ(host.GetState(), RoomState::Ended) << host.Status();
  EXPECT_EQ(host.ErrorCode(), "taken") << host.Status();
  for (int i = 0; i < 1500 && joiner.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(joiner.GetState(), RoomState::Ended);
  EXPECT_EQ(joiner.ErrorCode(), "peer_left") << joiner.Status();
  EXPECT_NE(joiner.Status().find("isn't in that game"), std::string::npos) << joiner.Status();
  EXPECT_LT(std::chrono::steady_clock::now() - welcomed, std::chrono::seconds(12));
}

// A host whose connection dropped comes back into its own room, and its seat: the same player id
// on a new socket inside the server's hold (what a reopened room does after a dropped connection).
TEST(OrcaLive, HostComesBackToItsRoom)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  const std::string stamp =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  const std::string player = "orca-host-" + stamp;
  SetEnv("ORCA_TEST_PLAYER", player.c_str());
  RoomOptions options;
  options.room_code = "orcah" + stamp;
  options.compatibility = "orca-live-test";
  auto first = std::make_unique<YouGameRoom>(options);
  for (int i = 0; i < 500 && first->GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(first->GetState(), RoomState::Waiting) << first->Status();
  // A friend was in the game, and leaves once its host is gone.
  SetEnv("ORCA_TEST_PLAYER", nullptr);
  options.joining = true;
  auto friend_room = std::make_unique<YouGameRoom>(options);
  ASSERT_TRUE(WaitArrived(*first)) << first->Status();
  options.joining = false;
  SetEnv("ORCA_TEST_PLAYER", player.c_str());
  ScopedEnv drop("ORCA_TEST_ROOM_DROP", "1");
  first->TestDropConnection();
  // Background recovery retains this room object and its seat, without a goodbye.
  std::this_thread::sleep_for(std::chrono::seconds(5));
  EXPECT_EQ(first->GetState(), RoomState::Waiting) << first->Status();
  EXPECT_TRUE(first->ErrorCode().empty());
  EXPECT_EQ(first->Seat(), 0);
  EXPECT_EQ(first->Code(), options.room_code);
  friend_room->Leave();
  SetEnv("ORCA_TEST_PLAYER", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
}

// A room waiting for a friend stays open as long as the friend takes: half a minute with only
// pings. Guards against an idle socket being dropped after a few seconds (seen on Windows).
TEST(OrcaLive, WaitingRoomStaysOpen)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcaw" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  YouGameRoom a(options);
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  for (int second = 0; second < 30; ++second)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_EQ(a.GetState(), RoomState::Waiting) << "after " << second + 1 << " s: " << a.Status();
  }
  EXPECT_GE(a.RoundTripMs(), 0);
}


TEST(OrcaRoomResilience, RuntimeRestartsAndReceiveFailuresAreReconnectable)
{
  for (const auto& [code, reason] : std::vector<std::pair<unsigned, std::string>>{
           {1013, "WebSocket closed by the server (1013)"},
           {0, "WebSocket closed by the server"},
           {0, "WebSocket receive failed: Failure when receiving data from the peer"},
           {0, "server-silent"},
           {0, "WebSocket send failed: connection reset"}})
  {
    const auto trace = TraceRoomForTest(TraceOptions(), {
        {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
        {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000, reason, code},
        {RK::PollReconnect, 1000}, {RK::PollReconnect, 1500},
        {RK::Message, 2000, RosterMessage("welcome")},
    });
    EXPECT_TRUE(trace[3].reconnect_started);
    const bool immediate = reason.starts_with("WebSocket receive") ||
                           reason.starts_with("WebSocket closed");
    EXPECT_EQ(trace[4].reconnect_attempts, immediate ? 1u : 0u);
    EXPECT_EQ(trace[5].reconnect_attempts, 1u);
    EXPECT_EQ(trace[3].reconnect_at,
              RoomClock::time_point{} + (immediate ? 1000ms : 1500ms));
    const std::string loss = reason == "server-silent" ? "silent" :
                             reason.starts_with("WebSocket receive") ? "recv" :
                             reason.starts_with("WebSocket send") ? "send" : "eof";
    EXPECT_NE(trace[3].notices.back().find("room-away self " + loss), std::string::npos);
    EXPECT_FALSE(trace[6].reconnecting);
    EXPECT_TRUE(trace[6].events.empty());
    const auto& connecting = trace[immediate ? 4 : 5];
    ASSERT_FALSE(connecting.notices.empty());
    EXPECT_NE(connecting.notices.back().find("close-code=" + std::to_string(code)), std::string::npos);
    EXPECT_NE(connecting.notices.back().find("close-reason=" + reason), std::string::npos);
    ASSERT_FALSE(trace[6].notices.empty());
    EXPECT_NE(trace[6].notices.back().find("same-seat"), std::string::npos);
  }
}

TEST(OrcaRoomResilience, SeatInUseRetriesWithBoundedExponentialBackoff)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 1000, "WebSocket closed by the server", 1013},
      {RK::PollReconnect, 1500}, {RK::ConnectFailure, 1500, "WebSocket upgrade failed (HTTP 409) seat in use"},
      {RK::PollReconnect, 1999}, {RK::PollReconnect, 2000},
      {RK::ConnectFailure, 2000, "WebSocket upgrade failed (HTTP 409) seat in use"},
      {RK::PollReconnect, 3000}, {RK::ConnectFailure, 3000, "WebSocket upgrade failed (HTTP 409) seat in use"},
      {RK::PollReconnect, 5000}, {RK::ConnectFailure, 5000, "WebSocket upgrade failed (HTTP 409) seat in use"},
      {RK::PollReconnect, 9000}, {RK::ConnectFailure, 9000, "WebSocket upgrade failed (HTTP 409) seat in use"},
      {RK::Message, 9500, RosterMessage("welcome")},
  });
  const auto at = [](int ms) { return RoomClock::time_point{} + std::chrono::milliseconds(ms); };
  EXPECT_EQ(trace[4].reconnect_at, at(2000));
  EXPECT_EQ(trace[5].reconnect_attempts, 1u);
  EXPECT_EQ(trace[7].reconnect_at, at(3000));
  EXPECT_EQ(trace[9].reconnect_at, at(5000));
  EXPECT_EQ(trace[11].reconnect_at, at(9000));
  // A held seat's retries run to 12 s after the drop (the server's live-seat window is 10 s).
  EXPECT_EQ(trace[13].reconnect_at, at(13000));
  EXPECT_EQ(trace[13].reconnect_attempts, 5u);
  ASSERT_FALSE(trace[4].notices.empty());
  EXPECT_NE(trace[4].notices.back().find("seat-in-use"), std::string::npos);
  EXPECT_FALSE(trace.back().reconnecting);
  EXPECT_EQ(trace.back().state, RoomState::Waiting);
}

TEST(OrcaRoomResilience, InitialSeatInUseRefusalRetriesButOtherRefusalsRemainFinal)
{
  const auto held = TraceRoomForTest(TraceOptions(), {
      {RK::InitialConnectFailure, 0, "WebSocket upgrade failed (HTTP 409) seat in use"},
      {RK::PollReconnect, 500}, {RK::Message, 1000, RosterMessage("welcome")},
  });
  EXPECT_TRUE(held[0].reconnect_started);
  EXPECT_EQ(held[1].reconnect_attempts, 1u);
  EXPECT_EQ(held.back().state, RoomState::Waiting);
  EXPECT_FALSE(held.back().reconnecting);
  const auto refused = TraceRoomForTest(TraceOptions(), {
      {RK::InitialConnectFailure, 0, "WebSocket upgrade failed (HTTP 403)"},
  });
  EXPECT_FALSE(refused.back().reconnect_started);
  EXPECT_EQ(refused.back().state, RoomState::Ended);
}

TEST(OrcaRoomResilience, InitialSeatRetriesCoverTheLiveSeatWindowBeforeSoftFailure)
{
  const std::string refusal = "WebSocket upgrade failed (HTTP 409) seat in use";
  std::vector<RoomTestStep> steps{{RK::InitialConnectFailure, 0, refusal}};
  for (const int ms : {500, 1500, 3500, 7500, 11500, 15500})
  {
    steps.push_back({RK::PollReconnect, ms});
    steps.push_back({RK::ConnectFailure, ms, refusal});
  }
  const auto trace = TraceRoomForTest(TraceOptions(), steps);
  for (size_t i = 0; i + 1 < trace.size(); ++i)
    EXPECT_NE(trace[i].state, RoomState::Ended) << i;
  EXPECT_EQ(trace.back().state, RoomState::Ended);
  EXPECT_EQ(trace.back().error_code, "network");
  EXPECT_EQ(trace.back().status, "Your seat in that room is still held by your last connection. "
                                 "Try again in a few seconds.");
  // A seat released after 10 s succeeds on the next attempt, before the refusal deadline.
  steps.resize(11);
  steps.push_back({RK::Message, 11500, RosterMessage("welcome")});
  const auto recovered = TraceRoomForTest(TraceOptions(), steps);
  EXPECT_EQ(recovered.back().state, RoomState::Waiting);
  EXPECT_FALSE(recovered.back().reconnecting);
}

TEST(OrcaRoomResilience, HostGoneAfterServerCloseEndsWithoutRetrying)
{
  for (const auto& refusal : {"WebSocket upgrade failed (HTTP 410) host_gone",
                              "WebSocket upgrade failed (HTTP 403) host_gone"})
  {
    SCOPED_TRACE(refusal);
    const auto trace = TraceRoomForTest(TraceOptions(), {
        {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
        {RK::SocketClose, 1000, "WebSocket closed by the server"},
        {RK::PollReconnect, 1000}, {RK::ConnectFailure, 1000, refusal},
    });
    EXPECT_EQ(trace.back().state, RoomState::Ended);
    EXPECT_EQ(trace.back().error_code, "host_gone");
    EXPECT_EQ(trace.back().status, "Your friend left that game. Ask them for a new invite.");
    EXPECT_EQ(trace.back().reconnect_attempts, 1u);
  }
}

TEST(OrcaRoomResilience, ServerCloseRefusalsKeepTheirExistingRetryWindows)
{
  // A held seat gets 12 s (the server's live-seat window is 10 s), then ends; a 503 (a rooms
  // isolate restarting until the site answers) is never final: it falls back to the room hold.
  for (const auto& refusal : {"WebSocket upgrade failed (HTTP 409) seat in use",
                              "WebSocket upgrade failed (HTTP 503)"})
  {
    SCOPED_TRACE(refusal);
    const bool seat = std::string_view(refusal).find("seat in use") != std::string_view::npos;
    std::vector<RoomTestStep> steps{
        {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
        {RK::SocketClose, 1000, "WebSocket closed by the server"},
    };
    for (const int ms : {1000, 1500, 2500, 4500, 8500, 11000})
    {
      steps.push_back({RK::PollReconnect, ms});
      steps.push_back({RK::ConnectFailure, ms, refusal});
    }
    const auto at_eleven = TraceRoomForTest(TraceOptions(), steps);
    for (const auto& row : at_eleven)
      EXPECT_NE(row.state, RoomState::Ended);
    EXPECT_TRUE(at_eleven.back().reconnecting);
    steps.push_back({RK::PollReconnect, 13000});
    steps.push_back({RK::ConnectFailure, 13000, refusal});
    const auto later = TraceRoomForTest(TraceOptions(), steps);
    if (seat)
    {
      EXPECT_EQ(later.back().state, RoomState::Ended);
      EXPECT_EQ(later.back().error_code, "network");
    }
    else
    {
      EXPECT_NE(later.back().state, RoomState::Ended);
      EXPECT_TRUE(later.back().reconnecting);
    }
  }
  // Other refusals remain final; no retry window for a genuinely full room or a 403.
  for (const auto& refusal : {"WebSocket upgrade failed (HTTP 409) room full",
                              "WebSocket upgrade failed (HTTP 403) host_older"})
  {
    const auto trace = TraceRoomForTest(TraceOptions(), {
        {RK::Message, 0, RosterMessage("welcome")},
        {RK::SocketClose, 1000, "WebSocket closed by the server"},
        {RK::ConnectFailure, 1000, refusal},
    });
    EXPECT_EQ(trace.back().state, RoomState::Ended);
  }
}

TEST(OrcaRoomResilience, ServerClose503RefusalsKeepFriendsUntilTheRoomHoldExpires)
{
  constexpr int closed_ms = 1000;
  constexpr int deadline_ms =
      closed_ms + std::chrono::duration_cast<std::chrono::milliseconds>(ROOM_HOLD).count();
  // Midpoint jitter: immediate, 500 ms, 1 s, 2 s, 4 s, the 10 s restart boundary,
  // then one attempt per 4 s backoff. Refused upgrades do not spend the rolling budget
  // of six *successful connections*, so they must keep retrying throughout the hold.
  std::vector<int> attempts{1000, 1500, 2500, 4500, 8500, 11000};
  for (int ms = 15000; ms < deadline_ms; ms += 4000)
    attempts.push_back(ms);
  std::vector<RoomTestStep> steps{
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, closed_ms, "WebSocket closed by the server"},
  };
  size_t next = 0;
  for (int ms = closed_ms; ms < deadline_ms; ms += 100)
  {
    steps.push_back({RK::PollReconnect, ms});
    if (next < attempts.size() && ms == attempts[next])
    {
      steps.push_back({RK::ConnectFailure, ms, "WebSocket upgrade failed (HTTP 503)"});
      steps.push_back({RK::PollReconnect, ms});  // Polling again cannot bypass the backoff.
      ++next;
    }
  }
  steps.push_back({RK::PollReconnect, deadline_ms - 1});
  steps.push_back({RK::PollReconnect, deadline_ms});
  steps.push_back({RK::PollReconnect, deadline_ms + 1});
  const auto trace = TraceRoomForTest(TraceOptions(), steps);
  ASSERT_EQ(trace.size(), steps.size());
  unsigned expected_attempts = 0;
  next = 0;
  for (size_t i = 3; i < trace.size(); ++i)
  {
    SCOPED_TRACE(steps[i].milliseconds);
    if (next < attempts.size() && steps[i].kind == RK::PollReconnect &&
        steps[i].milliseconds == attempts[next])
    {
      ++expected_attempts;
      ++next;
    }
    EXPECT_EQ(trace[i].reconnect_attempts, expected_attempts);
    if (steps[i].milliseconds < deadline_ms)
    {
      EXPECT_NE(trace[i].state, RoomState::Ended);
      EXPECT_TRUE(trace[i].reconnecting);
      if (steps[i].kind == RK::ConnectFailure)
        EXPECT_EQ(trace[i].reconnect_at, RoomClock::time_point{} +
                      std::chrono::milliseconds(next < attempts.size() ? attempts[next] : deadline_ms));
    }
    else
    {
      EXPECT_EQ(trace[i].state, RoomState::Ended);
      EXPECT_EQ(trace[i].error_code, "network");
      EXPECT_EQ(trace[i].status, "Lost the connection to the room (hold-expired)");
    }
  }
  EXPECT_EQ(expected_attempts, attempts.size());
  ASSERT_FALSE(trace[trace.size() - 2].notices.empty());
  EXPECT_NE(trace[trace.size() - 2].notices.back().find("hold-expired"), std::string::npos);
  EXPECT_TRUE(trace.back().notices.empty());
}

TEST(OrcaRoomResilience, ReconnectRetriesNeverExtendTheHoldAndLogTheFinalOutcomeOnce)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 1000, "WebSocket receive failed: connection reset", 1013},
      {RK::ConnectFailure, 89000, "WebSocket upgrade failed (HTTP 409) seat in use"},
      {RK::PollReconnect, 90999}, {RK::PollReconnect, 91000}, {RK::PollReconnect, 92000},
  });
  EXPECT_EQ(trace[4].state, RoomState::Waiting);
  EXPECT_EQ(trace[5].state, RoomState::Ended);
  ASSERT_FALSE(trace[5].notices.empty());
  EXPECT_NE(trace[5].notices.back().find("hold-expired"), std::string::npos);
  EXPECT_NE(trace[5].notices.back().find("close-code=1013"), std::string::npos);
  EXPECT_NE(trace[5].notices.back().find("HTTP 409"), std::string::npos);
  EXPECT_TRUE(trace[6].notices.empty());
  EXPECT_EQ(trace[6].state, RoomState::Ended);
}

TEST(OrcaRoomResilience, OwnRoomLossWithSilentInputsWaitsForFreshDropIn)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000},
      {RK::Tick, 10999}, {RK::Tick, 11000}, {RK::RetryPeer, 11001}, {RK::Tick, 11001},
      {RK::Message, 14000, RosterMessage("welcome")}, {RK::Tick, 14001},
      {RK::Message, 14002, PeerHello()},
  });
  EXPECT_TRUE(trace[4].events.empty());
  EXPECT_TRUE(trace[5].events.empty());
  EXPECT_TRUE(trace[5].host_away_until);
  EXPECT_FALSE(trace[8].reconnecting);
  ASSERT_EQ(trace[10].events.size(), 1u);
  EXPECT_EQ(trace[10].events[0].kind, PeerEvent::Kind::Arrived);
}

TEST(OrcaRoomResilience, SameRunHelloAfterBackKeepsTheLivePeer)
{
  const std::string hello = R"({"t":"msg","from":"peer","d":{"k":"hello","c":"test","r":true,"h":true,"run":"same-run"}})";
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, hello},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::Message, 1001, RosterMessage("back")}, {RK::Message, 1002, hello},
  });
  EXPECT_TRUE(trace[4].events.empty());
  EXPECT_TRUE(trace[5].events.empty());
}

TEST(OrcaRoomResilience, LegacyHelloStillInteroperatesAfterBackWithLiveInputs)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::Message, 1001, RosterMessage("back")}, {RK::Message, 1002, PeerHello()},
      {RK::Message, 1003, PeerPacket()},
  });
  EXPECT_TRUE(trace[5].events.empty());
  EXPECT_EQ(trace[6].packets.size(), 1u);
}


TEST(OrcaRoomResilience, RecoveryWelcomeClearsAMissedBackAndHonorsARealLeave)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::SocketClose, 1001}, {RK::Message, 2000, RosterMessage("welcome")},
      {RK::SocketClose, 3000}, {RK::Message, 4000, RosterMessage("welcome", true, false)},
  });
  EXPECT_FALSE(trace[5].host_away_until);
  EXPECT_TRUE(trace[5].events.empty());
  ASSERT_EQ(trace[7].events.size(), 1u);
  EXPECT_EQ(trace[7].events[0].kind, PeerEvent::Kind::Left);
  EXPECT_FALSE(trace[7].host_away_until);
}

TEST(OrcaRoomResilience, RecoveryWelcomeDoesNotClearAPeerStillAway)
{
  picojson::value server;
  ASSERT_TRUE(picojson::parse(server, RosterMessage("welcome")).empty());
  server.get<picojson::object>()["players"].get<picojson::array>()[1].get<picojson::object>()["away"] =
      picojson::value(true);
  const auto welcome = server.serialize();
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::Message, 1000, RosterMessage("away")}, {RK::SocketClose, 1001},
      {RK::Message, 2000, welcome}, {RK::Tick, 11000}, {RK::Tick, 91000},
  });
  EXPECT_TRUE(trace[4].host_away_until);
  EXPECT_TRUE(trace[5].events.empty());
  ASSERT_EQ(trace[6].events.size(), 1u);
  EXPECT_EQ(trace[6].events[0].reason, "room-hold-expired");
}


TEST(OrcaRoomResilience, OldSessionStalledNoticeCrossingBackKeepsTheRoom)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::DirectInput, 1000, "peer"}, {RK::Message, 1000, RosterMessage("away")},
      {RK::Message, 9000, RosterMessage("back")},
      {RK::Message, 9001, R"({"t":"msg","from":"peer","d":{"k":"drop","r":"stalled"}})"},
  });
  EXPECT_EQ(trace.back().state, RoomState::Waiting);
  ASSERT_EQ(trace.back().events.size(), 1u);
  EXPECT_EQ(trace.back().events[0].reason, "room-away-silent");
}


namespace
{
class OrcaRoomRuntime : public testing::Test
{
protected:
  void SetUp() override
  {
    const char* fixture = std::getenv("ORCA_TEST_ROOM_SERVER");
    if (!fixture)
      GTEST_SKIP() << "run through Tools/orca/room-fix-tests.py (loopback fixtures)";
    url = fixture;
    ASSERT_TRUE(url.starts_with("http://127.0.0.1:"));
    for (const auto& [name, value] : std::vector<std::pair<const char*, std::string>>{
             {"ORCA_SITE", url}, {"ORCA_TEST_DEV_GAME", "room-fixture"},
             {"ORCA_DIRECT", "0"}, {"ORCA_TEST_ROOM_DROP", "1"}})
      vars.push_back(std::make_unique<ScopedEnv>(name, value.c_str()));
  }
  picojson::value Request(const std::string& path, const std::string& body = "")
  {
    Common::HttpRequest http(2s);
    const auto reply = body.empty() ? http.Get(url + path) : http.Post(url + path, body);
    EXPECT_TRUE(reply);
    picojson::value value;
    if (reply)
      EXPECT_TRUE(picojson::parse(value, std::string(reply->begin(), reply->end())).empty());
    return value;
  }
  std::unique_ptr<YouGameRoom> Open(const std::string& code, const std::string& config = "{}",
                                    bool joining = true, bool friends = true,
                                    bool recovery_enabled = true)
  {
    Request("/configure/" + code, config);
    auto options = TraceOptions(joining);
    options.room_code = code;
    options.friends_room = friends;
    options.recovery_enabled = recovery_enabled;
    return std::make_unique<YouGameRoom>(options);
  }
  bool Wait(const std::function<bool()>& predicate, std::chrono::milliseconds limit = 6s)
  {
    const auto until = RoomClock::now() + limit;
    while (RoomClock::now() < until)
    {
      if (predicate())
        return true;
      std::this_thread::sleep_for(10ms);
    }
    return predicate();
  }
  int Connections(const std::string& code)
  {
    return static_cast<int>(Request("/state/" + code).get("connects").get<double>());
  }
  bool Sent(const std::string& code, const std::string& type, const std::string& kind = "")
  {
    const auto value = Request("/state/" + code);
    for (const auto& m : value.get("messages").get<picojson::array>())
    {
      if (m.get("t").to_str() == type && (kind.empty() || m.get("d").get("k").to_str() == kind))
        return true;
    }
    return false;
  }
  void Action(const std::string& code, const std::string& body) { Request("/action/" + code, body); }
  std::string JoinFailureStatus(const YouGameRoom& room)
  {
    const ScopedEnv session("ORCA_SESSION", "1");
    auto* output = std::tmpfile();
    EXPECT_NE(output, nullptr);
    if (!output)
      return {};
    const Common::ScopeGuard cleanup([&] {
      Orca::Status::SetOutput(nullptr);
      Orca::Status::SetAppCaps("");
      std::fclose(output);
    });
    Orca::Status::SetOutput(output);
    Orca::Status::SetAppCaps("join");
    Rollback::OnlineMatch::ReportJoinFailure(room.ErrorCode(), room.Status());
    std::rewind(output);
    char buffer[512];
    std::string lines;
    while (std::fgets(buffer, sizeof(buffer), output))
      lines += buffer;
    return lines;
  }
  std::string url;
  std::vector<std::unique_ptr<ScopedEnv>> vars;
};

}  // namespace

TEST(OrcaRoom, ReopenDiscardsQueueExpiredAndTakenCodesAcrossRetries)
{
  using Rollback::OnlineMatch::RoomReopenCode;
  for (const std::string queue : {"casual", "ranked"})
  {
    const std::string retained = RoomReopenCode("oldqueue", queue, true, false);
    EXPECT_TRUE(retained.empty());
    // A failed fresh-room upgrade has no queue welcome; it must not revive the old queue code.
    EXPECT_TRUE(RoomReopenCode(retained, "private", true, false).empty());
  }
  EXPECT_EQ(RoomReopenCode("friends1", "private", true, false), "friends1");
  EXPECT_TRUE(RoomReopenCode("friends1", "private", false, false).empty());
  EXPECT_TRUE(RoomReopenCode("friends1", "private", true, true).empty());
}

TEST_F(OrcaRoomRuntime, NetworkEndedRoomsReopenWithFreshQueueCodesAndRetainFriendsCodes)
{
  for (const std::string queue : {"casual", "ranked", "private"})
  {
    SCOPED_TRACE(queue);
    const std::string old_code = queue + "0358";
    const std::string fresh_code = "solo" + queue;
    const ScopedEnv fresh("ORCA_TEST_ROOM", fresh_code.c_str());
    Request("/configure/" + fresh_code, R"({"joining":false})");
    auto room = Open(old_code, fmt::format(R"({{"queue":"{}","joining":false}})", queue),
                     false, queue == "private", false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    ASSERT_EQ(room->Queue(), queue);
    room->TestDropConnection();
    ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
    ASSERT_EQ(room->ErrorCode(), "network");
    // Use the welcome's queue even after the socket and room ended, as KeepRoomOpen does.
    const std::string reopen = Rollback::OnlineMatch::RoomReopenCode(
        room->Code(), room->Queue(), true, false);
    if (queue == "private")
      EXPECT_EQ(reopen, old_code);
    else
    {
      EXPECT_TRUE(reopen.empty());
      // A private-mode retry of the queue code would be refused on every attempt.
      Action(old_code, R"({"refuse":{"code":"lobby_mismatch"},"refuse_status":403})");
    }
    room.reset();
    auto options = TraceOptions(false);
    options.room_code = reopen;
    options.ask_app_for_room = false;
    room = std::make_unique<YouGameRoom>(options);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    EXPECT_TRUE(room->ErrorCode().empty());
    EXPECT_EQ(room->Code(), queue == "private" ? old_code : fresh_code);
    EXPECT_EQ(Connections(old_code), queue == "private" ? 2 : 1);
    EXPECT_EQ(Connections(fresh_code), queue == "private" ? 0 : 1);
  }
}

TEST_F(OrcaRoomRuntime, RecoveredSocketUsesTheRealGoodbyeTeardown)
{
  const std::string code = "teardown";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  u32 before;
  room->RoundTripMs(&before);
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] {
    u32 sequence;
    room->RoundTripMs(&sequence);
    return sequence > before && Connections(code) == 2;
  }));
  room->SetLeaveReason("desync");
  Action(code, R"({"message":{"t":"msg","from":"peer","d":{"k":"drop","r":"desync"}}})");
  ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
  // Observe Run's teardown before Leave() could turn it into a user quit.
  ASSERT_TRUE(Wait([&] { return Sent(code, "leave"); }));
  EXPECT_TRUE(Sent(code, "msg", "bye"));
  EXPECT_EQ(room->ErrorCode(), "desync");
}

TEST_F(OrcaRoomRuntime, FinalCloseCodesReachRunAndNeverReconnect)
{
  for (const auto& [close, reason] : std::vector<std::pair<int, std::string>>{
           {1008, "network"}, {4001, "kicked"}, {4003, "network"}})
  {
    const std::string code = "final" + std::to_string(close);
    auto room = Open(code);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
    Action(code, fmt::format(R"({{"close":{}}})", close));
    ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
    EXPECT_EQ(room->ErrorCode(), reason);
    room->Leave();
    EXPECT_EQ(Connections(code), 1);
  }
}

TEST_F(OrcaRoomRuntime, ServerCloseEchoesTheSameCodeBeforeCleanup)
{
  for (const int close : {4004, 4000, 1012, 0})
  {
    const std::string code = "echo" + std::to_string(close);
    Request("/configure/" + code, "{}");
    Common::WebSocket socket;
    std::string error;
    ASSERT_TRUE(socket.Connect("ws" + url.substr(4) + "/room/" + code, 2s, &error)) << error;
    ASSERT_TRUE(socket.ReceiveText(1s));
    Action(code, close ? fmt::format(R"({{"close":{}}})", close) : R"({"close_empty":true})");
    ASSERT_TRUE(Wait([&] {
      socket.ReceiveText(20ms);
      return !socket.IsOpen();
    }));
    EXPECT_EQ(socket.CloseCode(), close);
    ASSERT_TRUE(Wait([&] { return Request("/state/" + code).get("closes").get<double>() == 1; }));
    const auto codes = Request("/state/" + code).get("close_codes").get<picojson::array>();
    ASSERT_EQ(codes.size(), 1u);
    EXPECT_EQ(codes[0].get<double>(), close);
    socket.Close();
    EXPECT_EQ(Request("/state/" + code).get("closes").get<double>(), 1);
  }
}

TEST_F(OrcaRoomRuntime, CloseWaitsForAcknowledgementButKeepsItsTwoHundredMillisecondBudget)
{
  for (const int delay_ms : {60, 1000})
  {
    const std::string code = "closewait" + std::to_string(delay_ms);
    Request("/configure/" + code, fmt::format(R"({{"close_delay":{}}})", delay_ms / 1000.0));
    Common::WebSocket socket;
    std::string error;
    ASSERT_TRUE(socket.Connect("ws" + url.substr(4) + "/room/" + code, 2s, &error)) << error;
    const auto since = RoomClock::now();
    socket.Close(1000, 200ms);
    const auto elapsed = RoomClock::now() - since;
    EXPECT_GE(elapsed, delay_ms == 60 ? 40ms : 180ms);
    EXPECT_LT(elapsed, 600ms);
    EXPECT_FALSE(socket.IsOpen());
    const auto frames = Request("/state/" + code).get("close_codes").get<picojson::array>();
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].get<double>(), 1000);
  }
}

TEST_F(OrcaRoomRuntime, CloudflareColoKeepsOnlyTheValidatedHeaderSuffix)
{
  const std::vector<std::pair<std::string, std::string>> cases{
      {R"({"cf_ray":"fixture-EWR"})", "EWR"},
      {R"({"cf_ray":"fixture-IAD"})", "IAD"},
      {R"({"cf_ray":"fixture-invalid"})", ""},
      {R"({"cf_ray":"fixture-ABC extra"})", ""},
      {R"({})", ""},
  };
  for (size_t i = 0; i < cases.size(); ++i)
  {
    const std::string code = "edgecolo" + std::to_string(i);
    Request("/configure/" + code, cases[i].first);
    Common::WebSocket socket;
    std::string error;
    ASSERT_TRUE(socket.Connect("ws" + url.substr(4) + "/room/" + code, 2s, &error)) << error;
    EXPECT_EQ(socket.EdgeColo(), cases[i].second);
    EXPECT_TRUE(socket.LossClass().empty());
    EXPECT_FALSE(socket.OsError());
    Action(code, R"({"close":1013})");
    ASSERT_TRUE(Wait([&] { socket.ReceiveText(10ms); return !socket.IsOpen(); }));
    EXPECT_EQ(socket.LossClass(), "eof");
    EXPECT_EQ(socket.EdgeColo(), cases[i].second);  // retained after cleanup
  }
}

TEST_F(OrcaRoomRuntime, ReconnectReusesTheSeatTicketAndClosesTheOpenSocketWith4001)
{
  const std::string code = "sameticket";
  auto room = Open(code, R"({"same_ticket":true})");
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  u32 before;
  room->RoundTripMs(&before);
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] {
    u32 sequence;
    room->RoundTripMs(&sequence);
    return sequence > before && Connections(code) == 2;
  }));
  const auto state = Request("/state/" + code);
  EXPECT_EQ(state.get("tickets").get<double>(), 1);
  const auto tickets = state.get("ticket_uses").get<picojson::array>();
  ASSERT_EQ(tickets.size(), 2u);
  EXPECT_EQ(tickets[0].get<double>(), 1);
  EXPECT_EQ(tickets[1].get<double>(), 1);
  const auto closes = state.get("close_codes").get<picojson::array>();
  ASSERT_EQ(closes.size(), 1u);
  EXPECT_EQ(closes[0].get<double>(), 4001);
}

TEST_F(OrcaRoomRuntime, ReconnectRefreshesOnlyAfterTheCachedTicketIsRefused)
{
  const std::string code = "expireticket";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  Action(code, R"({"reject_first_ticket":true})");
  u32 before;
  room->RoundTripMs(&before);
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] {
    u32 sequence;
    room->RoundTripMs(&sequence);
    return sequence > before && Connections(code) == 3;
  }));
  const auto state = Request("/state/" + code);
  EXPECT_EQ(state.get("tickets").get<double>(), 2);
  const auto tickets = state.get("ticket_uses").get<picojson::array>();
  ASSERT_EQ(tickets.size(), 3u);
  EXPECT_EQ(tickets[0].get<double>(), 1);
  EXPECT_EQ(tickets[1].get<double>(), 1);
  EXPECT_EQ(tickets[2].get<double>(), 2);
}

TEST_F(OrcaRoomRuntime, RealSeatInUseBodyRetriesWithoutMintingAnotherTicket)
{
  // origin/main:rooms/src/index.ts's seatInUse refusal, including its sentence and code.
  const std::string refusal =
      R"({"error":"This seat is in use. Join again in a moment.","code":"seat_in_use"})";
  for (const bool initial : {false, true})
  {
    const std::string code = initial ? "initialseat" : "recoverseat";
    auto room = Open(code, initial ?
        fmt::format(R"({{"refuse":{},"refuse_count":1}})", refusal) : "{}");
    if (!initial)
    {
      ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
      Action(code, fmt::format(R"({{"refuse":{},"refuse_count":2}})", refusal));
      room->TestDropConnection();
    }
    const int expected = initial ? 2 : 3;
    ASSERT_TRUE(Wait([&] {
      return room->Seat() == 1 && room->RoundTripMs() >= 0 && Connections(code) == expected;
    }));
    EXPECT_NE(room->GetState(), RoomState::Ended);
    const auto state = Request("/state/" + code);
    EXPECT_EQ(state.get("tickets").get<double>(), 1);
    for (const auto& ticket : state.get("ticket_uses").get<picojson::array>())
      EXPECT_EQ(ticket.get<double>(), 1);
  }
}

TEST_F(OrcaRoomRuntime, RealFullRoomBodyEndsRecoveryAfterOneFreshTicketFallback)
{
  const std::string code = "recoverfull";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  Action(code, R"({"refuse":{"error":"Room is full"}})");
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
  EXPECT_EQ(room->ErrorCode(), "room_full");
  EXPECT_EQ(room->Status(), "That room is full");
  EXPECT_EQ(Connections(code), 3);
  EXPECT_EQ(Request("/state/" + code).get("tickets").get<double>(), 2);
}

TEST_F(OrcaRoomRuntime, UnstableRefusalBodiesEndWithoutHeldSeatRetriesOrTicketRefresh)
{
  for (const bool recovery : {false, true})
  {
    for (const bool initial : {false, true})
    {
      if (!initial && !recovery)
        continue;
      // Only the recognized code determines this policy, even with unrelated body text.
      for (const char* body : {R"({"error":"room unstable","code":"room_unstable"})",
                               R"({"error":"This seat is in use.","code":"room_unstable"})"})
      {
        SCOPED_TRACE(fmt::format("recovery={} initial={} body={}", recovery, initial, body));
        const std::string code = "unstref370";
        const std::string refusal = fmt::format(R"({{"refuse":{},"refuse_status":409}})", body);
        auto room = Open(code, initial ? refusal : "{}", true, true, recovery);
        if (!initial)
        {
          ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
          Action(code, fmt::format(R"({{"refuse":{},"refuse_status":409,"close":1013}})", body));
        }
        ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
        EXPECT_EQ(room->ErrorCode(), "room-unstable");
        EXPECT_EQ(room->Status(), "The room keeps disconnecting.");
        room->Leave();
        EXPECT_EQ(Connections(code), initial ? 1 : 2);
        EXPECT_EQ(Request("/state/" + code).get("tickets").get<double>(), 1);
      }
    }
  }
}

TEST_F(OrcaRoomRuntime, RealRestartRefusalBodiesRecoverAfterAServerClose)
{
  const std::vector<std::pair<int, std::string>> refusals{
      {409, R"({"error":"This seat is in use. Join again in a moment.","code":"seat_in_use"})"},
      {503, R"({"error":"Service Unavailable"})"},
  };
  for (const auto& [status, body] : refusals)
  {
    const std::string code = "restart" + std::to_string(status);
    auto room = Open(code);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
    u32 before;
    room->RoundTripMs(&before);
    Action(code, fmt::format(R"({{"refuse":{},"refuse_status":{},"refuse_count":3,"close":1013}})",
                             body, status));
    ASSERT_TRUE(Wait([&] {
      u32 after;
      room->RoundTripMs(&after);
      return after > before && Connections(code) == 4;
    }));
    EXPECT_NE(room->GetState(), RoomState::Ended);
    EXPECT_TRUE(room->ErrorCode().empty());
  }
}

TEST_F(OrcaRoomRuntime, HostGoneRefusalsEndOnceWithTheExistingHostLeftState)
{
  const std::vector<std::pair<int, std::string>> refusals{
      {410, R"({"error":"Your friend isn't in that game any more","code":"host_gone"})"},
      {403, R"({"error":"Your friend isn't in that game any more","code":"host_gone"})"},
      {409, R"({"code":"host_gone"})"},
  };
  for (const bool recovery : {false, true})
  {
    for (size_t i = 0; i < refusals.size(); ++i)
    {
      const auto& [status, body] = refusals[i];
      SCOPED_TRACE(fmt::format("recovery={} status={} body={}", recovery, status, body));
      const std::string code = fmt::format("gone{}{}", i, recovery ? 'r' : 'l');
      const std::string refusal = fmt::format(R"({{"refuse":{},"refuse_status":{}}})", body, status);
      auto room = Open(code, refusal, true, true, recovery);
      ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
      EXPECT_EQ(room->ErrorCode(), "host_gone");
      EXPECT_EQ(room->Status(), "Your friend left that game. Ask them for a new invite.");
      EXPECT_EQ(JoinFailureStatus(*room), "orca state host-left host_gone\n");
      room->Leave();
      EXPECT_EQ(Connections(code), 1);
      EXPECT_EQ(Request("/state/" + code).get("tickets").get<double>(), 1);
      if (!recovery)
        continue;
      // A refusal during recovery is also final, including inside the old restart grace.
      room = Open(code);
      ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
      Action(code, fmt::format(R"({{"refuse":{},"refuse_status":{},"close":1013}})", body, status));
      ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
      EXPECT_EQ(room->ErrorCode(), "host_gone");
      EXPECT_EQ(room->Status(), "Your friend left that game. Ask them for a new invite.");
      room->Leave();
      EXPECT_EQ(Connections(code), 2);
      EXPECT_EQ(Request("/state/" + code).get("tickets").get<double>(), 1);
    }
  }
}

TEST_F(OrcaRoomRuntime, RealExpiredTicketBodyKeepsTheKnownNetworkCode)
{
  auto room = Open("expired401",
                   R"({"refuse":{"error":"Bad or expired ticket"},"refuse_status":401})");
  ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
  EXPECT_EQ(room->ErrorCode(), "network");
  EXPECT_EQ(room->Status(), "The invite expired; press Play again");
  EXPECT_EQ(Connections("expired401"), 1);
  EXPECT_EQ(JoinFailureStatus(*room),
            "orca error network The invite expired; press Play again\n");
}

TEST_F(OrcaRoomRuntime, RefusalBodiesKeepTheSiteCodeAndDistinguishVersionsFromGameOrMode)
{
  const std::string mismatch = RefusalReason("WebSocket connect failed: ... (HTTP 403)");
  const std::string older = "Your friend is on an older YG Dolphin. Ask them to press Play to update it and send you a new invite.";
  const std::string newer = "Your friend is on a newer YG Dolphin. Press Play to update, then join again.";
  const std::vector<std::pair<std::string, std::string>> cases{
      {R"({"error":"Your friend is on an older YG Dolphin. Ask them to press Play to update it and send you a new invite.","code":"host_older","host_version":"0.3.47","your_version":"0.3.48"})",
       "Your friend is on an older YG Dolphin (0.3.47). Ask them to press Play to update it and send you a new invite."},
      {R"({"error":"Your friend is on an older YG Dolphin. Ask them to press Play to update it and send you a new invite.","code":"host_older","your_version":"0.3.48"})", older},
      {R"({"error":"Your friend is on a newer YG Dolphin. Press Play to update yours.","code":"host_newer","host_version":"0.3.49","your_version":"0.3.48"})", newer},
      {R"({"error":"Your friend is on a newer YG Dolphin. Press Play to update yours.","code":"host_newer"})", newer},
      {R"({"error":"This room uses a different lobby protocol or configuration","code":"lobby_mismatch"})",
       "That room is set up for a different game or mode, so your game opened its own room."},
      {R"({"error":"This room uses a different lobby protocol or configuration"})", mismatch},
      {R"({"error":"host_older host_version=injected","code":"unknown"})", mismatch},
      {R"({"error":"ignored","code":"host_older","host_version":"0.3.44\ntext"})", older},
      {R"({"error":"ignored","code":"host_older","host_version":44})", older},
  };
  for (const bool friends : {true, false})
  {
    for (size_t i = 0; i < cases.size(); ++i)
    {
      const auto& [body, sentence] = cases[i];
      SCOPED_TRACE(body);
      const std::string code = std::string(friends ? "version" : "qversion") + std::to_string(i);
      auto room = Open(code, fmt::format(R"({{"refuse":{},"refuse_status":403,"queue":"{}"}})",
                                        body, friends ? "private" : "ranked"), true, friends);
      ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
      std::string expected = sentence;
      if (!friends)
      {
        if (const auto at = expected.find("your friend"); at != std::string::npos)
          expected.replace(at, 11, "the other player");
        if (const auto at = expected.find("Your friend"); at != std::string::npos)
          expected.replace(at, 11, "The other player");
      }
      EXPECT_EQ(room->Status(), expected);
      EXPECT_EQ(room->ErrorCode(), "room_mismatch");
      EXPECT_EQ(Connections(code), 1);
    }
  }
}

TEST_F(OrcaRoomRuntime, ReplayLimitExplainsTheJoinFailure)
{
  auto room = Open("replaylimit");
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
  Action("replaylimit", R"({"message":{"t":"msg","from":"peer","d":{"k":"drop","r":"replay_limit"}}})");
  ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
  EXPECT_EQ(room->ErrorCode(), "network");
  EXPECT_EQ(room->Status(), "Your friend's game couldn't send the match to you (too long to catch up). "
                            "Try again at the next character select.");
}

TEST_F(OrcaRoomRuntime, WelcomeDoesNotResetTheReconnectBudget)
{
  const std::string code = "budget44";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  for (int i = 0; i < 6; ++i)
  {
    u32 before;
    room->RoundTripMs(&before);
    room->TestDropConnection();
    ASSERT_TRUE(Wait([&] {
      u32 now;
      room->RoundTripMs(&now);
      return now > before && Connections(code) == i + 2;
    }));
  }
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
  EXPECT_EQ(room->ErrorCode(), "network");
  room->Leave();  // final teardown must not make a new connection for leave
  EXPECT_EQ(Connections(code), 7);
  EXPECT_FALSE(Sent(code, "leave"));
}

TEST_F(OrcaRoomRuntime, FirstUpgradeRefusalsMakeOnlyOneRequest)
{
  for (const auto& [code, body] : std::vector<std::pair<std::string, std::string>>{
           {"fullroom", "Room is full"}, {"complete", "This match is complete"},
           {"unknown4", "unclassified"}})
  {
    auto room = Open(code, fmt::format(R"({{"refuse":"{}"}})", body));
    ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }, 1s));
    if (body == "This match is complete")
      EXPECT_EQ(room->ErrorCode(), "match-over");
    else
      EXPECT_EQ(room->ErrorCode(), "room_full");
    EXPECT_EQ(Connections(code), 1);
    EXPECT_EQ(Request("/state/" + code).get("probes").get<double>(), 0);
  }
}

TEST_F(OrcaRoomRuntime, QueueRoomsRetainTheirOriginalDropAndHelloBehavior)
{
  for (const std::string queue : {"ranked", "casual"})
  {
    const std::string code = queue + "44";
    auto room = Open(code, fmt::format(R"({{"queue":"{}"}})", queue), true, false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
    const auto messages = Request("/state/" + code).get("messages");
    for (const auto& message : messages.get<picojson::array>())
    {
      if (message.get("d").is<picojson::object>() &&
          message.get("d").get("k").to_str() == "hello")
        EXPECT_TRUE(message.get("d").get("run").is<picojson::null>());
    }
    room->TestDropConnection();
    ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
    EXPECT_EQ(Connections(code), 1);
  }
}

TEST_F(OrcaRoomRuntime, DropHookIsInertWithoutItsOptIn)
{
  ScopedEnv disabled("ORCA_TEST_ROOM_DROP", nullptr);
  auto room = Open("inert444");
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
  room->TestDropConnection();
  std::this_thread::sleep_for(700ms);
  EXPECT_EQ(room->GetState(), RoomState::Waiting);
  EXPECT_EQ(Connections("inert444"), 1);
}

TEST_F(OrcaRoomRuntime, QuitCancelsAnUpgradeAndAttemptsLeaveWithTheCachedTicket)
{
  const std::string code = "quitmid4";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
  Action(code, R"({"upgrade_delay":6})");
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] { return Connections(code) >= 2; }));
  Action(code, R"({"upgrade_delay":0})");
  const auto start = RoomClock::now();
  room->Leave();
  EXPECT_LT(RoomClock::now() - start, 3s);
  EXPECT_TRUE(Sent(code, "leave"));
}

TEST_F(OrcaRoomRuntime, QuitCancelsATicketRefreshAndAttemptsLeave)
{
  const std::string code = "quittkt4";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
  Action(code, R"({"ticket_delay":6,"reject_first_ticket":true})");
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] { return Request("/state/" + code).get("tickets").get<double>() >= 2; }));
  Action(code, R"({"reject_first_ticket":false})");
  const auto start = RoomClock::now();
  room->Leave();
  EXPECT_LT(RoomClock::now() - start, 3s);
  EXPECT_TRUE(Sent(code, "leave"));
}

TEST_F(OrcaRoomRuntime, ResumeRetriesUntilAKeyframeArrives)
{
  const std::string code = "resume44";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return Sent(code, "msg", "hello"); }));
  room->TakePeerEvents();
  const auto now = RoomClock::now();
  ASSERT_TRUE(room->HoldInputSilence(now, now + 10s));
  room->RetryPeer(0);
  ASSERT_TRUE(Wait([&] {
    int resumes = 0;
    const auto messages = Request("/state/" + code).get("messages");
    for (const auto& m : messages.get<picojson::array>())
      if (m.get("d").is<picojson::object>())
        resumes += m.get("d").get("resume").evaluate_as_boolean();
    return resumes >= 3;
  }));
  // A host's hello does not acknowledge the resync; only the keyframe does.
  Action(code, R"({"message":{"t":"msg","from":"peer","d":{"k":"hello","c":"test","h":true,"r":true}}})");
  KeyframeInfo keyframe;
  keyframe.id = "replay-300-01234567";
  keyframe.frame = 300;
  keyframe.size = 1;
  keyframe.hash = "0123456789abcdef";
  keyframe.key = std::string(64, '0');
  Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer","d":{}}}}})",
                           picojson::value(KeyframeOfferToJson(keyframe)).serialize()));
  ASSERT_TRUE(Wait([&] {
    for (const auto& event : room->TakePeerEvents())
      if (event.kind == PeerEvent::Kind::Keyframe)
        return true;
    return false;
  }));
  EXPECT_EQ(room->Code(), code);
}

TEST(OrcaRoomResilience, ReconnectBudgetRollsAfterFiveMinutesAndJitterIsBounded)
{
  RoomReconnectBudget budget;
  const auto start = RoomClock::time_point{};
  for (int i = 0; i < 6; ++i)
    EXPECT_TRUE(budget.Take(start + std::chrono::seconds(i)));
  EXPECT_FALSE(budget.Take(start + 299s));
  EXPECT_TRUE(budget.Take(start + 300s));
  EXPECT_FALSE(budget.Take(start + 300s));
  for (unsigned sample = 0; sample < 61; ++sample)
  {
    const auto delay = RoomReconnect::Jitter(500ms, sample);
    EXPECT_GE(delay, 350ms);
    EXPECT_LE(delay, 650ms);
  }
  EXPECT_EQ(RoomReconnect::Jitter(500ms, 0), 350ms);
  EXPECT_EQ(RoomReconnect::Jitter(500ms, 60), 650ms);
}


namespace
{
class RoomHashGame : public Game
{
public:
  bool Save(int frame) override { snapshots[frame] = hash; return true; }
  bool Load(int frame) override
  {
    if (!snapshots.contains(frame))
      return false;
    hash = snapshots[frame];
    return true;
  }
  void SetPads(int frame, const Pads& input) override { next = frame; pads = input; }
  void SetResimulating(bool) override {}
  std::optional<u64> SnapshotChecksum(int frame) override
  {
    return snapshots.contains(frame) ? std::optional(snapshots[frame]) : std::nullopt;
  }
  void ResetPacing() override {}
  int Run()
  {
    history[next] = hash;
    hash = (hash ^ static_cast<u64>(next)) * 1099511628211ULL;
    for (const auto& pad : pads)
      for (const auto byte : pad)
        hash = (hash ^ byte) * 1099511628211ULL;
    return next;
  }
  int next = 0;
  u64 hash = 1469598103934665603ULL;
  Pads pads{};
  std::map<int, u64> snapshots, history;
};

Packet NeutralFrames(int first, int count)
{
  Packet packet;
  packet.seat = 0;
  packet.first_frame = first;
  packet.current_frame = first + count;
  packet.pads.resize(count);
  return packet;
}

std::string InjectPacket(const Packet& packet)
{
  return fmt::format(R"({{"message":{{"t":"msg","from":"peer","d":{}}}}})", EncodePacket(packet));
}

Orca::Net::Config RoomSessionConfig()
{
  Orca::Net::Config config;
  config.local_seat = 1;
  config.time_sync = false;
  config.fixed_delay = 2;
  config.max_delay = 2;
  config.checksum_every = 1000000;
  return config;
}
class OrcaRoomStallRuntime : public OrcaRoomRuntime, public testing::WithParamInterface<int>
{
protected:
  ~OrcaRoomStallRuntime() override
  {
    extra_done = true;
    if (extra_sender.joinable())
      extra_sender.join();
  }
  void OpenDirect(const std::string& code, bool joining, const std::string& queue = "private",
                  bool recovery_enabled = true)
  {
    vars.push_back(std::make_unique<ScopedEnv>("ORCA_DIRECT", "1"));
    vars.push_back(std::make_unique<ScopedEnv>("ORCA_DIRECT_LOOPBACK", "1"));
    vars.push_back(std::make_unique<ScopedEnv>("ORCA_DIRECT_STUN", "0"));
    // Queue rooms retain their ordinary two-player budget cases in every instantiation.
    room_players = queue == "private" ? GetParam() : 0;
    room_code = code;
    local_seat = joining ? 1 : 0;
    room = Open(code, fmt::format(R"({{"direct":true,"joining":{},"queue":"{}","doubles":{},"size":{},"peers":{}}})",
        joining, queue, room_players > 0, room_players == 4 ? 4 : 2, room_players == 4 ? 3 : 1),
                joining, queue == "private", recovery_enabled);
    ASSERT_TRUE(Wait([&] { return room->Seat() == local_seat && room->RoundTripMs() >= 0; }));
    DirectOptions options;
    options.me = "peer";
    options.room = code;
    options.ice.stun_host.clear();
    options.loopback = true;
    direct.Start(std::move(options));
    direct.AddPeer("me", local_seat);
    if (room_players == 4)
    {
      for (int slot : {2, 3})
      {
        extra_direct[slot] = std::make_unique<DirectLink>();
        DirectOptions extra_options;
        extra_options.me = fmt::format("peer{}", slot);
        extra_options.room = code;
        extra_options.ice.stun_host.clear();
        extra_options.loopback = true;
        extra_direct[slot]->Start(std::move(extra_options));
        extra_direct[slot]->AddPeer("me", local_seat);
      }
    }
    size_t seen = 0;
    ASSERT_TRUE(Wait([&] {
      for (auto& [to, message] : direct.TakeSignals())
        Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer","d":{}}}}})",
                                 picojson::value(message).serialize()));
      for (int slot : {2, 3})
        if (extra_direct[slot])
          for (auto& [to, message] : extra_direct[slot]->TakeSignals())
            Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer{}","d":{}}}}})",
                                    slot, picojson::value(message).serialize()));
      const auto state = Request("/state/" + code);
      const auto& messages = state.get("messages").get<picojson::array>();
      for (; seen < messages.size(); ++seen)
      {
        const auto& data = messages[seen].get("d");
        if (!data.is<picojson::object>() || !data.get("k").to_str().starts_with("dl"))
          continue;
        const auto& target = messages[seen].get("to").to_str();
        if (target == "peer")
          direct.OnSignal("me", data.get<picojson::object>());
        for (int slot : {2, 3})
          if (extra_direct[slot] && target == fmt::format("peer{}", slot))
            extra_direct[slot]->OnSignal("me", data.get<picojson::object>());
      }
      // Link setup must not leave game inputs in flight when the session is constructed.
      direct.Send(EncodePacket(NeutralPacket(0, 0)));
      room->Receive();
      return room->Direct().transport == 1;
    }, 10s));
    room->TakePeerEvents();
    auto config = RoomSessionConfigForSeat();
    session = std::make_unique<Session>(config, game, *room);
    if (room_players == 4)
      extra_sender = std::thread([this] {
        int tick = 0;
        std::array<int, MAX_SEATS> ack{-1, -1, -1, -1};
        while (!extra_done)
        {
          const int frame = extra_frame;
          for (int slot : {2, 3})
          {
            for (const auto& received : extra_direct[slot]->Receive())
            {
              Packet input;
              if (DecodeDirectPacket(received.payload, local_seat, &input))
                ack[slot] = std::max(ack[slot], input.first_frame + static_cast<int>(input.pads.size()) - 1);
            }
            if (slot == 3 && tick % 3 != 0)
              continue;
            Packet packet;
            packet.seat = slot;
            packet.first_frame = std::max(0, frame - 16);
            packet.current_frame = frame;
            packet.ack[local_seat] = ack[slot];
            packet.pads.resize(frame + 32 - packet.first_frame);
            extra_direct[slot]->Send(EncodePacket(packet));
          }
          ++tick;
          std::this_thread::sleep_for(2ms);
        }
      });
  }
  Packet NeutralPacket(int first, int count)
  {
    Packet packet;
    packet.seat = 1 - local_seat;
    packet.first_frame = first;
    packet.current_frame = first + count;
    packet.pads.resize(count);
    if (room_players == 4)
      for (int seat : {2, 3})
        packet.ack[seat] = first + count + 2;
    return packet;
  }
  Orca::Net::Config RoomSessionConfigForSeat()
  {
    Orca::Net::Config config;
    config.local_seat = local_seat;
    config.per_peer_inputs = room_players > 0;
    config.seats = room_players == 4 ? 4 : 2;
    config.authority_seat = room_players > 0 ? 0 : -1;
    config.time_sync = false;
    config.fixed_delay = 2;
    config.max_delay = 2;
    config.checksum_every = 1000000;
    return config;
  }

  std::string PresenceRoster(const char* kind)
  {
    const auto json = RosterMessage(kind, local_seat == 1);
    if (room_players != 4)
      return json;
    picojson::value value;
    EXPECT_TRUE(picojson::parse(value, json).empty());
    auto& roster = value.get<picojson::object>();
    for (int slot : {2, 3})
    {
      const auto id = fmt::format("peer{}", slot);
      picojson::object player{{"id", picojson::value(id)},
                              {"name", picojson::value(fmt::format("Friend{}", slot))}};
      roster["players"].get<picojson::array>().emplace_back(player);
      player["connectionId"] = picojson::value(id);
      player["slot"] = picojson::value(static_cast<double>(slot));
      roster["participants"].get<picojson::array>().emplace_back(player);
    }
    return value.serialize();
  }

  Step Boundary(RoomClock::time_point deadline, int* polls = nullptr)
  {
    extra_frame = session->CurrentFrame();
    const auto start = RoomClock::now();
    const int recoveries_before = budget.Recoveries();
    const auto result = Rollback::StepSession(*session, game, completed, [] { return Pad{}; }, [&] {
      if (polls)
        ++*polls;
      return room->HostLeftPending() || RoomClock::now() > deadline;
    }, 1ms, 10s, nullptr, nullptr,
    [&](RoomClock::time_point now) {
      ++hold_calls;
      return Rollback::OnlineMatch::WaitForFriendsRoster(*session, local_seat != 0, room->Doubles()) ||
             room->HoldInputSilence(start, now,
                 Rollback::OnlineMatch::FriendsMissingInputs(*session, room->Doubles()));
    }, &budget, [&](RoomClock::time_point now) { return room->RoomRecovering(now); });
    last_excused = budget.Recoveries() > recoveries_before;
    if (result.kind == StepKind::Run || result.kind == StepKind::Rollback)
      completed = game.Run();
    return result;
  }

  Step StallFor(std::chrono::milliseconds duration, bool drop_socket = false, bool peer_away = false)
  {
    if (peer_away)
    {
      Action(room_code, fmt::format(R"({{"message":{}}})", PresenceRoster("away")));
      EXPECT_TRUE(Wait([&] { return room->RoomRecovering(RoomClock::now()); }));
    }
    if (drop_socket)
    {
      Action(room_code, R"({"block_for":80})");
      room->TestDropConnection();
      direct.Test("off");
    }
    const auto begin = RoomClock::now();
    int polls = 0;
    std::thread restore([&] {
      std::this_thread::sleep_for(duration);
      direct.Test("on");
      const auto recovered = NeutralPacket(next_input, 16);
      const auto packet = EncodePacket(recovered);
      if (peer_away)
        Action(room_code, fmt::format(R"({{"message":{}}})", PresenceRoster("back")));
      // Repeated UDP packets, as the session does, until StepSession has received them.
      // Peer-away cases restore through the room, keeping back and its inputs in socket order.
      while (!restored.load())
      {
        if (peer_away)
          Action(room_code, InjectPacket(recovered));
        else
          direct.Send(packet);
        std::this_thread::sleep_for(2ms);
      }
    });
    Step result{};
    bool outage_excused = false;
    for (int boundary = 0; boundary < 100; ++boundary)
    {
      result = Boundary(begin + duration + 5s, &polls);
      outage_excused |= last_excused;
      // Another friend's shorter wait may finish first. Consume this returning peer's entire
      // prefix before advancing next_input, so the fixture cannot skip a range between outages.
      if (result.kind == StepKind::Ended ||
          (polls > 0 && (room_players != 4 || session->ConfirmedFrame() >= next_input + 15)))
        break;
    }
    restored = true;
    restore.join();
    restored = false;
    last_excused = outage_excused;
    if (peer_away)
      EXPECT_TRUE(Wait([&] { return !room->RoomRecovering(RoomClock::now()); }));
    next_input += 16;
    return result;
  }
  std::string room_code;
  int room_players = 0;
  int local_seat = 1, completed = -1, next_input = 0, hold_calls = 0;
  std::unique_ptr<YouGameRoom> room;
  DirectLink direct;
  RoomHashGame game;
  std::unique_ptr<Session> session;
  Rollback::OnlineMatch::StallBudget budget;
  std::atomic<bool> restored{false};
  bool last_excused = false;
  std::array<std::unique_ptr<DirectLink>, MAX_SEATS> extra_direct;
  std::atomic<bool> extra_done{false};
  std::atomic<int> extra_frame{0};
  std::thread extra_sender;

  void ExpectDarkRecovery(std::chrono::milliseconds duration)
  {
    ASSERT_TRUE(session);
    for (int small = 0; small < 3; ++small)
      ASSERT_EQ(StallFor(100ms).kind, StepKind::Run) << session->Error() << ": " << session->Describe();
    EXPECT_EQ(budget.Recoveries(), 0);
    EXPECT_EQ(hold_calls, 0);
    // The room socket stays down when the actual UDP link returns, just as in staging.
    ASSERT_EQ(StallFor(duration, true).kind, StepKind::Run);
    EXPECT_TRUE(last_excused);
    EXPECT_EQ(budget.Recoveries(), 1);
    EXPECT_LT(budget.Stalled(RoomClock::now()), 1s);
    EXPECT_TRUE(room->RoomRecovering(RoomClock::now()));
    EXPECT_GT(Connections(room_code), 1);
    EXPECT_EQ(room->GetState(), RoomState::Waiting);
    EXPECT_EQ(room->Code(), room_code);
    EXPECT_TRUE(session->Error().empty());
    EXPECT_FALSE(session->Idle());
    EXPECT_EQ(StallFor(200ms).kind, StepKind::Run);
    // A short stall while the room is still recovering spends no second excuse.
    EXPECT_EQ(budget.Recoveries(), 1);
    EXPECT_FALSE(Sent(room_code, "leave"));
    EXPECT_FALSE(Sent(room_code, "msg", "drop"));
  }
};
}  // namespace

TEST_F(OrcaRoomRuntime, HealthyRealSessionHashesMatchWithRecoveryDisabled)
{
  const auto run = [&](bool recovery) {
    const std::string code = recovery ? "healthy1" : "healthy0";
    auto room = Open(code, "{}", true, true, recovery);
    EXPECT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
    struct BufferedRoom : Transport
    {
      explicit BufferedRoom(YouGameRoom& r) : room(r) {}
      void Send(const Packet& p) override { room.Send(p); }
      std::vector<Packet> Receive() override
      {
        auto next = room.Receive();
        pending.insert(pending.end(), next.begin(), next.end());
        return std::exchange(pending, {});
      }
      bool Connected() const override { return room.Connected(); }
      YouGameRoom& room;
      std::vector<Packet> pending;
    } transport(*room);
    RoomHashGame game;
    Session session(RoomSessionConfig(), game, transport);
    int completed = -1;
    for (int frame = 0; frame < 200; ++frame)
    {
      Packet peer = NeutralFrames(frame, 1);
      if (frame >= 2)
        peer.pads[0][0] = static_cast<u8>((frame - 2) % 17);
      Action(code, InjectPacket(peer));
      EXPECT_TRUE(Wait([&] {
        auto packets = room->Receive();
        transport.pending.insert(transport.pending.end(), packets.begin(), packets.end());
        return !transport.pending.empty();
      }));
      const auto start = RoomClock::now();
      const auto step = Rollback::StepSession(session, game, completed,
          [&] { return Pad{static_cast<u8>(frame % 13)}; }, [] { return false; }, 1ms, 10s,
          nullptr, nullptr, [&](RoomClock::time_point now) {
            return room->HoldInputSilence(start, now);
          });
      EXPECT_EQ(step.kind, StepKind::Run) << session.Error();
      if (step.kind != StepKind::Run)
        break;
      completed = game.Run();
    }
    EXPECT_TRUE(session.Error().empty());
    EXPECT_EQ(game.history.size(), 200u);
    return game.history;
  };
  EXPECT_EQ(run(true), run(false));
}

TEST_F(OrcaRoomRuntime, BothSilentForFifteenSecondsKeepTheRealSessionAndRoom)
{
  const std::string code = "blip15s4";
  const auto recovery = NeutralFrames(0, 40);
  auto room = Open(code, fmt::format(R"({{"recovery_packet":{{"t":"msg","from":"peer","d":{}}}}})",
                                    EncodePacket(recovery)));
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  Action(code, InjectPacket(NeutralFrames(0, 16)));
  RoomHashGame game;
  Session session(RoomSessionConfig(), game, *room);
  int completed = -1;
  ASSERT_TRUE(Wait([&] {
    const auto step = session.OnBoundary(completed, Pad{});
    if (step.kind == StepKind::Run)
      completed = game.Run();
    return step.kind == StepKind::Stall;
  }));
  room->TakePeerEvents();
  Action(code, R"({"block_for":15})");
  room->TestDropConnection();
  const auto since = RoomClock::now();
  int holds = 0;
  bool gave_up = false;
  const auto step = Rollback::StepSession(session, game, completed, [] { return Pad{}; },
      [&] { return room->HostLeftPending(); }, 1ms, 10s, &gave_up, nullptr,
      [&](RoomClock::time_point now) {
        ++holds;
        return room->HoldInputSilence(since, now);
      });
  EXPECT_GE(RoomClock::now() - since, 14s);
  EXPECT_LT(RoomClock::now() - since, 22s);
  EXPECT_GT(holds, 0);
  EXPECT_FALSE(gave_up);
  EXPECT_EQ(step.kind, StepKind::Run) << session.Error();
  EXPECT_TRUE(session.Error().empty());
  EXPECT_EQ(room->Code(), code);
  EXPECT_EQ(room->GetState(), RoomState::Waiting);
  EXPECT_FALSE(room->HostLeftPending());
  EXPECT_FALSE(Sent(code, "leave"));
}

TEST_F(OrcaRoomRuntime, MissingInputBuffersRequestDropInInTheSameRoom)
{
  const std::string code = "gapres44";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  Action(code, InjectPacket(NeutralFrames(0, 16)));
  RoomHashGame game;
  Session session(RoomSessionConfig(), game, *room);
  int completed = -1;
  ASSERT_TRUE(Wait([&] {
    const auto step = session.OnBoundary(completed, Pad{});
    if (step.kind == StepKind::Run)
      completed = game.Run();
    return step.kind == StepKind::Stall;
  }));
  room->TakePeerEvents();
  const auto since = RoomClock::now();
  ASSERT_TRUE(room->HoldInputSilence(since, since + 10s));
  Action(code, InjectPacket(NeutralFrames(1000, 16)));
  const auto step = Rollback::StepSession(session, game, completed, [] { return Pad{}; },
      [&] { return room->HostLeftPending(); }, 1ms, 1s);
  EXPECT_EQ(step.kind, StepKind::Ended);
  EXPECT_TRUE(session.Error().empty());
  ASSERT_TRUE(room->HostAwayDeadline());
  const auto events = room->TakePeerEvents();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events[0].reason, "room-resume");
  EXPECT_TRUE(Wait([&] { return Sent(code, "msg", "hello"); }));
  Action(code, R"({"message":{"t":"msg","from":"peer","d":{"k":"hello","c":"test","h":true,"r":true}}})");
  const std::string keyframe = fmt::format(R"({{"message":{{"t":"msg","from":"peer","d":{{"k":"kf",
      "f":1000,"n":1,"id":"replay-1000-01234567","x":"0123456789abcdef","key":"{}"}}}}}})", std::string(64, '0'));
  Action(code, keyframe);
  EXPECT_TRUE(Wait([&] {
    for (const auto& event : room->TakePeerEvents())
      if (event.kind == PeerEvent::Kind::Keyframe && event.keyframe.frame == 1000)
        return true;
    return false;
  }));
  EXPECT_EQ(room->Code(), code);
  EXPECT_EQ(room->GetState(), RoomState::Waiting);
  EXPECT_FALSE(Sent(code, "leave"));
}


TEST_F(OrcaRoomRuntime, ANewResumeRequestCanResetAHostThatAlreadyResumedOnce)
{
  const std::string code = "hostres4";
  auto room = Open(code, R"({"joining":false})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && Sent(code, "msg", "hello"); }));
  room->TakePeerEvents();
  const auto resume = [&](const char* id) {
    Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer","d":{{"k":"hello",
        "c":"test","r":true,"h":false,"resume":true,"resume_id":"{}"}}}}}})", id));
  };
  resume("first");
  ASSERT_TRUE(Wait([&] {
    for (const auto& e : room->TakePeerEvents())
      if (e.kind == PeerEvent::Kind::Arrived)
        return true;
    return false;
  }));
  KeyframeInfo keyframe{300, "replay-300-01234567", 1, "0123456789abcdef", std::string(64, '0')};
  room->OfferKeyframe(1, keyframe);
  Packet peer = NeutralFrames(300, 1);
  peer.seat = 1;
  Action(code, InjectPacket(peer));
  ASSERT_TRUE(Wait([&] { return !room->Receive().empty(); }));
  // The old keyframe offer and a live-session packet must not suppress this new request.
  resume("second");
  ASSERT_TRUE(Wait([&] {
    for (const auto& e : room->TakePeerEvents())
      if (e.kind == PeerEvent::Kind::Arrived)
        return true;
    return false;
  }));
  EXPECT_EQ(room->Code(), code);
  EXPECT_EQ(room->GetState(), RoomState::Waiting);
}

TEST_F(OrcaRoomRuntime, UnreportedSilenceHasABoundedHoldAndRealLeaveInterruptsIt)
{
  const std::string code = "verdict4";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && Sent(code, "msg", "hello"); }));
  room->TakePeerEvents();
  const auto now = RoomClock::now();
  ASSERT_TRUE(room->HoldInputSilence(now - 89s, now));
  EXPECT_FALSE(room->HoldInputSilence(now, now + 2s)); // a retry cannot restart the hold
  Action(code, fmt::format(R"({{"message":{}}})", RosterMessage("leave", true, false)));
  ASSERT_TRUE(Wait([&] { return room->HostLeftPending(); }));
  EXPECT_FALSE(room->HoldInputSilence(now, now));
  EXPECT_FALSE(room->HostAwayDeadline());
}


TEST_F(OrcaRoomRuntime, PrivateWelcomeOverridesAnInitialQueueHint)
{
  const std::string code = "privhint";
  Request("/configure/" + code, "{}");
  auto options = TraceOptions();
  options.room_code = code;
  options.friends_room = false;
  YouGameRoom room(options);
  ASSERT_TRUE(Wait([&] { return room.Seat() == 1 && room.RoundTripMs() >= 0; }));
  u32 before;
  room.RoundTripMs(&before);
  room.TestDropConnection();
  ASSERT_TRUE(Wait([&] {
    u32 after;
    room.RoundTripMs(&after);
    return after > before && Connections(code) == 2;
  }));
  EXPECT_EQ(room.Code(), code);
  EXPECT_EQ(room.GetState(), RoomState::Waiting);
}

TEST(OrcaRoomResilience, QueueBackAndJoinGreetImmediatelyAndEmitNoRoomNotices)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const char* event : {"back", "join"})
    {
      picojson::value welcome;
      ASSERT_TRUE(picojson::parse(welcome, RosterMessage("welcome")).empty());
      picojson::object metadata;
      metadata["queue"] = picojson::value(queue);
      welcome.get<picojson::object>()["room"] = picojson::value(metadata);
      const auto trace = TraceRoomForTest(TraceOptions(), {
          {RK::Message, 0, welcome.serialize()}, {RK::Message, 0, PeerHello()},
          {RK::Tick, 1000}, {RK::Message, 1001, RosterMessage(event)}, {RK::Tick, 1002},
          {RK::Message, 1003, RosterMessage("leave", true, false)},
          {RK::SocketClose, 1004, "closed", 1008},
      });
      ASSERT_EQ(trace[4].sent.size(), 1u);
      EXPECT_NE(trace[4].sent[0].find("hello"), std::string::npos);
      EXPECT_EQ(trace[3].events.size(), 1u);
      for (const auto& step : trace)
        EXPECT_TRUE(step.notices.empty());
    }
  }
}

TEST_F(OrcaRoomRuntime, PublicWelcomeOptInSharesInputHoldsAndRejoinsTheSocket)
{
  for (const std::string queue : {"casual", "ranked"})
  {
    const std::string code = queue + "h354";
    auto room = Open(code, fmt::format(R"({{"queue":"{}","recovery":true}})", queue), true, false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; })) << room->Status();
    ASSERT_TRUE(room->RoomRecovery());
    room->TakePeerEvents();
    RoomHashGame game;
    Session session(RoomSessionConfig(), game, *room);
    int completed = -1;
    ASSERT_TRUE(Wait([&] {
      const auto step = session.OnBoundary(completed, Pad{});
      if (step.kind == StepKind::Run)
        completed = game.Run();
      return step.kind == StepKind::Stall;
    }));
    const auto now = RoomClock::now();
    const auto since = now - 111s;
    Action(code, fmt::format(R"({{"message":{}}})", QueueRoster("away", queue.c_str())));
    ASSERT_TRUE(Wait([&] { return room->RoomRecovering(RoomClock::now()); }));
    EXPECT_TRUE(room->HoldInputSilence(since, now));
    EXPECT_TRUE(room->RoomRecovering(now));
    u32 before;
    room->RoundTripMs(&before);
    room->TestDropConnection();
    ASSERT_TRUE(Wait([&] {
      u32 after;
      room->RoundTripMs(&after);
      return after > before && Connections(code) == 2;
    }));
    EXPECT_EQ(room->GetState(), RoomState::Waiting);
    EXPECT_EQ(room->Code(), code);
    EXPECT_TRUE(room->ErrorCode().empty());
    EXPECT_TRUE(room->RoomRecovering(RoomClock::now()));
    EXPECT_TRUE(room->HostAwayDeadline());
    const auto rejoined = RoomClock::now();
    EXPECT_TRUE(room->HoldInputSilence(since, rejoined));
    EXPECT_TRUE(room->RoomRecovering(rejoined));
    std::thread restore([&] {
      std::this_thread::sleep_for(50ms);
      Action(code, InjectPacket(NeutralFrames(0, 40)));
    });
    bool gave_up = false;
    int holds = 0;
    const auto step = Rollback::StepSession(session, game, completed, [] { return Pad{}; },
        [&] { return room->HostLeftPending() || RoomClock::now() > rejoined + 3s; },
        1ms, 1ms, &gave_up, nullptr, [&](RoomClock::time_point poll) {
          ++holds;
          return room->HoldInputSilence(since, poll);
        });
    restore.join();
    EXPECT_EQ(step.kind, StepKind::Run) << session.Error();
    EXPECT_FALSE(gave_up);
    EXPECT_GT(holds, 0);
    EXPECT_TRUE(session.Error().empty());
    EXPECT_TRUE(Wait([&] { return !room->RoomRecovering(RoomClock::now()); }));
    EXPECT_FALSE(room->HostAwayDeadline());
    EXPECT_FALSE(Sent(code, "leave"));
    EXPECT_FALSE(Sent(code, "msg", "bye"));
    const auto messages = Request("/state/" + code).get("messages");
    for (const auto& message : messages.get<picojson::array>())
      if (message.get("d").is<picojson::object>() && message.get("d").get("k").to_str() == "hello")
      {
        EXPECT_TRUE(message.get("d").get("run").is<picojson::null>());
        EXPECT_TRUE(message.get("d").get("resume").is<picojson::null>());
      }
    room->Leave();
    EXPECT_FALSE(room->RoomRecovering(RoomClock::now()));
    const auto overlay = Orca::UX::SetEnd::Current().At(Orca::UX::SetEnd::NowMs());
    EXPECT_FALSE(overlay.notice && overlay.notice->title == "Reconnecting…");
  }
}

TEST_F(OrcaRoomRuntime, PublicWelcomeWithoutOptInCannotHoldInputs)
{
  for (const std::string queue : {"casual", "ranked"})
  {
    auto room = Open(queue + "old354", fmt::format(R"({{"queue":"{}"}})", queue), true, false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
    const auto now = RoomClock::now();
    EXPECT_FALSE(room->RoomRecovery());
    EXPECT_FALSE(room->HoldInputSilence(now, now));
    EXPECT_FALSE(room->RoomRecovering(now));
  }
}

TEST_F(OrcaRoomRuntime, QueueSilentDropKeepsTheSocketUnreadThroughLeave)
{
  ScopedEnv silent("ORCA_TEST_DROP_SILENT", "1");
  for (const char* queue : {"casual", "ranked"})
  {
    const std::string code = std::string(queue) + "silent";
    auto room = Open(code, fmt::format(R"({{"queue":"{}"}})", queue), true, false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
    room->TestDropConnection();
    ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
    room->Leave();  // joins Run; Online's retained shared_ptr keeps this socket alive
    EXPECT_EQ(Connections(code), 1);
    EXPECT_FALSE(Sent(code, "leave"));
    EXPECT_FALSE(Sent(code, "msg", "bye"));
    EXPECT_EQ(Request("/state/" + code).get("closes").get<double>(), 0);
    const auto before = Request("/state/" + code).get("messages").get<picojson::array>().size();
    u32 sequence;
    room->RoundTripMs(&sequence);
    Action(code, R"({"message":{"t":"pong","at":1}})");
    std::this_thread::sleep_for(50ms);
    u32 after;
    room->RoundTripMs(&after);
    EXPECT_EQ(after, sequence);
    EXPECT_EQ(Request("/state/" + code).get("messages").get<picojson::array>().size(), before);
  }
}

TEST_F(OrcaRoomRuntime, ExpiredInputHoldDoesNotRequestResync)
{
  const std::string code = "stale444";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  room->TakePeerEvents();
  Packet ack;
  ack.seat = 1;
  ack.ack[0] = 10;
  room->Send(ack);
  const auto now = RoomClock::now();
  EXPECT_FALSE(room->HoldInputSilence(now - ROOM_HOLD - 1s, now));
  Action(code, InjectPacket(NeutralFrames(1000, 16)));
  EXPECT_TRUE(Wait([&] { return !room->Receive().empty(); }));
  EXPECT_FALSE(room->HostLeftPending());
  EXPECT_TRUE(room->TakePeerEvents().empty());
}

TEST_F(OrcaRoomRuntime, FinalRecoveryEndLeavesAnOpenSocketOnce)
{
  const std::string code = "endopen4";
  auto room = Open(code, R"({"recovery_identity":"changed"})");
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
  ASSERT_TRUE(Wait([&] { return Sent(code, "leave"); }));
  room->Leave();
  int leaves = 0;
  const auto state = Request("/state/" + code);
  for (const auto& message : state.get("messages").get<picojson::array>())
    leaves += message.get("t").to_str() == "leave";
  EXPECT_EQ(leaves, 1);
  EXPECT_EQ(Connections(code), 2);
}

TEST_F(OrcaRoomRuntime, UpgradeFailurePathsNeverLogTicketUrls)
{
  using namespace Common::Log;
  struct Capture final : LogListener
  {
    void Log(LogLevel, const char* line) override
    {
      std::lock_guard lock(mutex);
      lines.emplace_back(line);
    }
    std::mutex mutex;
    std::vector<std::string> lines;
  };
  struct Logs
  {
    struct BaseLayer final : ::Config::ConfigLayerLoader
    {
      BaseLayer() : ConfigLayerLoader(::Config::LayerType::Base) {}
      void Load(::Config::Layer*) override {}
      void Save(::Config::Layer*) override {}
    };
    Logs()
    {
      old = File::GetUserPath(D_USER_IDX);
      dir = File::CreateTempDir();
      File::SetUserPath(D_USER_IDX, dir + DIR_SEP);
      ::Config::Init();
      ::Config::AddLayer(std::make_unique<BaseLayer>());
      LogManager::Init();
    }
    ~Logs()
    {
      LogManager::Shutdown();
      ::Config::Shutdown();
      File::SetUserPath(D_USER_IDX, old);
      File::DeleteDirRecursively(dir);
    }
    std::string old, dir;
  } logs;
  auto capture = std::make_unique<Capture>();
  Capture* const observed = capture.get();
  auto* const manager = LogManager::GetInstance();
  manager->RegisterListener(LogListener::LOG_WINDOW_LISTENER, std::move(capture));
  manager->EnableListener(LogListener::FILE_LISTENER, false);
  manager->EnableListener(LogListener::CONSOLE_LISTENER, false);
  manager->EnableListener(LogListener::LOG_WINDOW_LISTENER, true);
  manager->SetEnable(LogType::NETPLAY, true);
  manager->SetEnable(LogType::COMMON, true);
  {
    auto refused = Open("logfull4", R"({"refuse":"Room is full","probe_delay":2})");
    ASSERT_TRUE(Wait([&] { return refused->GetState() == RoomState::Ended; }));
    auto cancelled = Open("logabort", R"({"upgrade_delay":5})");
    ASSERT_TRUE(Wait([&] { return Connections("logabort") == 1; }));
    cancelled->Leave();
    auto timed = Open("logtime4");
    ASSERT_TRUE(Wait([&] { return timed->Seat() == 1 && timed->RoundTripMs() >= 0; }));
    Action("logtime4", R"({"upgrade_delay":5})");
    timed->TestDropConnection();
    // Allow the first capped upgrade to fail and the next one to reach the fixture.
    RoomReconnect upgrades;
    upgrades.attempts = 1;
    const auto now = RoomClock::now();
    const auto first_cap = upgrades.UpgradeCap(now);
    upgrades.attempts = 2;
    const auto reconnect_wait = first_cap + upgrades.UpgradeCap(now);
    ASSERT_TRUE(Wait([&] { return Connections("logtime4") >= 3; }, reconnect_wait));
    Action("logtime4", R"({"upgrade_delay":0})");
    timed->Leave();
    auto moved = Open("logcolo4", R"({"cf_ray":"fixture-EWR","recovery_cf_ray":"fixture-IAD"})");
    ASSERT_TRUE(Wait([&] { return moved->Seat() == 1 && moved->RoundTripMs() >= 0; }));
    u32 before;
    moved->RoundTripMs(&before);
    Action("logcolo4", R"({"close":1013})");
    ASSERT_TRUE(Wait([&] {
      u32 after;
      moved->RoundTripMs(&after);
      return after > before && Connections("logcolo4") == 2;
    }));
    moved->Leave();
  }
  std::lock_guard lock(observed->mutex);
  ASSERT_FALSE(observed->lines.empty());
  bool saw_failure = false;
  bool saw_loss = false;
  bool saw_move = false;
  for (const auto& line : observed->lines)
  {
    EXPECT_EQ(line.find("ticket="), std::string::npos);
    EXPECT_EQ(line.find("loopback-fixture"), std::string::npos);
    saw_failure |= line.find("WebSocket connect failed") != std::string::npos;
    // The site keeps the class word, errno and colos only right after "<kind> self", before any
    // free text (close-reason=, last-error=).
    const auto front = [&](std::string_view kind, std::string_view colo) {
      const std::string head = fmt::format("{} self eof errno=", kind);
      const size_t at = line.find(head);
      const size_t colo_at = line.find(fmt::format(" colo={}", colo));
      const size_t reason_at = line.find(" close-reason=");
      return at != std::string::npos && colo_at != std::string::npos &&
             (reason_at == std::string::npos || colo_at < reason_at);
    };
    saw_loss |= front("room-away", "EWR->") &&
                line.find(" retry started attempt=0 close-code=1013") != std::string::npos;
    saw_move |= front("room-back", "EWR->IAD") &&
                line.find(" kept same-seat") != std::string::npos;
  }
  EXPECT_TRUE(saw_failure);
  EXPECT_TRUE(saw_loss);
  EXPECT_TRUE(saw_move);
}

TEST(OrcaRoomBlackout, StrictParsingWindowAndInertness)
{
  ScopedEnv session("ORCA_SESSION", "1");
  ScopedEnv before("ORCA_TEST_BLACKOUT_ROOM_AFTER", nullptr);
  ScopedEnv unset("ORCA_TEST_BLACKOUT", nullptr);
  EXPECT_FALSE(Orca::TestBlackoutConfig());
  {
    ScopedEnv variant_only("ORCA_TEST_BLACKOUT_ROOM_AFTER", "12");
    EXPECT_FALSE(Orca::TestBlackoutConfig());
  }
  for (const char* invalid : {"", "-1-15", "15-15", "15-3", "3-15junk", "3-15-20",
                              "0-2147483648", "a-b"})
  {
    ScopedEnv value("ORCA_TEST_BLACKOUT", invalid);
    EXPECT_FALSE(Orca::TestBlackoutConfig()) << invalid;
  }
  ScopedEnv value("ORCA_TEST_BLACKOUT", "75-90");
  const auto normal = Orca::TestBlackoutConfig();
  ASSERT_TRUE(normal);
  EXPECT_FALSE(normal->DirectBlocked(74999ms));
  EXPECT_TRUE(normal->DirectBlocked(75s));
  EXPECT_TRUE(normal->RoomBlocked(89999ms));
  EXPECT_FALSE(normal->DirectBlocked(90s));
  EXPECT_TRUE(Orca::TestOverridesActive());
  {
    ScopedEnv variant("ORCA_TEST_BLACKOUT_ROOM_AFTER", "12");
    const auto race = Orca::TestBlackoutConfig();
    ASSERT_TRUE(race);
    EXPECT_TRUE(race->RoomBlocked(77999ms));
    EXPECT_FALSE(race->RoomBlocked(78s));
    EXPECT_TRUE(race->DirectBlocked(89s));
    EXPECT_FALSE(race->DirectBlocked(90s));
  }
  for (const char* invalid : {"-1", "16", "12junk", "2147483648"})
  {
    ScopedEnv variant("ORCA_TEST_BLACKOUT_ROOM_AFTER", invalid);
    EXPECT_FALSE(Orca::TestBlackoutConfig());
  }
  ScopedEnv inactive("ORCA_SESSION", "0");
  EXPECT_FALSE(Orca::TestBlackoutConfig());
  EXPECT_FALSE(Orca::TestOverridesActive());
}

TEST_F(OrcaRoomRuntime, QuickBackAndSameIdJoinGrantHoldBeforeAnyStall)
{
  for (const char* event : {"back", "join"})
  {
    const std::string code = std::string("quick") + event;
    auto room = Open(code, R"({"joining":false})", false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    // A shared session: the friend's inputs reached us before its room socket blipped.
    Packet played = NeutralFrames(6000, 16);
    played.seat = 1;
    Action(code, InjectPacket(played));
    ASSERT_TRUE(Wait([&] { return !room->Receive().empty(); }));
    room->TakePeerEvents();
    Action(code, fmt::format(R"({{"message":{}}})", RosterMessage(event, false)));
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(room->TakePeerEvents().empty()); // no ResetPeer/Left or replacement arrival
    const auto now = RoomClock::now();
    // Back, not this poll, started the hold. A new hold here would expire a second later.
    EXPECT_FALSE(room->HoldInputSilence(now + 1s, now + ROOM_HOLD + 500ms));
    EXPECT_EQ(room->Seat(), 0);
    EXPECT_EQ(room->Code(), code);
    EXPECT_FALSE(Sent(code, "msg", "drop"));
  }
}

TEST_F(OrcaRoomRuntime, BackWithoutASharedSessionGrantsNoHold)
{
  // A friend's room socket blips while no session runs between us (the host in a single-player
  // mode, a joiner still waiting): no hold, so nothing expires and resets it 90 s later.
  for (const char* event : {"back", "join"})
  {
    const std::string code = std::string("idle") + event;
    auto room = Open(code, R"({"joining":false})", false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    room->TakePeerEvents();
    Action(code, fmt::format(R"({{"message":{}}})", RosterMessage(event, false)));
    std::this_thread::sleep_for(100ms);
    // With no session the back is train-0343's plain re-handshake; what matters is that no hold
    // is left to expire and reset the friend later.
    room->TakePeerEvents();
    EXPECT_FALSE(room->InputHoldFor("peer"));
    EXPECT_EQ(room->Code(), code);
  }
}

TEST_F(OrcaRoomRuntime, HostNoWordTimeoutKeepsQuickBackUntilInputsReturn)
{
  const std::string code = "hostquick";
  auto room = Open(code, R"({"joining":false})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  auto config = RoomSessionConfig();
  config.local_seat = 0;
  config.start_frame = 6000;
  RoomHashGame game;
  Session session(config, game, *room);
  Packet initial = NeutralFrames(6000, 16);
  initial.seat = 1;
  Action(code, InjectPacket(initial));
  int completed = 5999;
  ASSERT_TRUE(Wait([&] {
    const auto step = session.OnBoundary(completed, Pad{});
    if (step.kind == StepKind::Run)
      completed = game.Run();
    return step.kind == StepKind::Stall;
  }));
  room->TakePeerEvents();
  // This verdict arrives before the first HoldInputSilence call, with no direct packets.
  Action(code, fmt::format(R"({{"message":{}}})", RosterMessage("back", false)));
  std::this_thread::sleep_for(100ms);
  ASSERT_TRUE(room->TakePeerEvents().empty());
  const auto since = RoomClock::now();
  std::thread restore([&] {
    std::this_thread::sleep_for(12s);
    Packet recovered = NeutralFrames(6000, MAX_PADS_IN_PACKET);
    recovered.seat = 1;
    Action(code, InjectPacket(recovered));
  });
  bool gave_up = false;
  Rollback::OnlineMatch::StallBudget budget;
  const auto step = Rollback::StepSession(session, game, completed, [] { return Pad{}; },
      [&] { return budget.Spent(RoomClock::now(), since,
                               room->HoldInputSilence(since, RoomClock::now())) ||
                    !room->TakePeerEvents().empty() || RoomClock::now() - since > 18s; },
      1ms, 10s, &gave_up, nullptr,
      [&](RoomClock::time_point now) { return room->HoldInputSilence(since, now); });
  restore.join();
  EXPECT_GE(RoomClock::now() - since, 12s);
  EXPECT_FALSE(gave_up);
  EXPECT_EQ(step.kind, StepKind::Run) << session.Error();
  EXPECT_TRUE(session.Error().empty());
  EXPECT_GT(session.CurrentFrame(), 6000);
  EXPECT_FALSE(session.Idle());
  EXPECT_EQ(room->Code(), code);
  EXPECT_FALSE(Sent(code, "msg", "drop"));
  EXPECT_FALSE(Sent(code, "leave"));
}

TEST_F(OrcaRoomRuntime, QuickBackPreservesTheOriginalHoldExpiry)
{
  const std::string code = "backexpiry";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  room->TakePeerEvents();
  const auto now = RoomClock::now();
  ASSERT_TRUE(room->HoldInputSilence(now - 89s, now));
  Action(code, fmt::format(R"({{"message":{}}})", RosterMessage("back", true)));
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(room->TakePeerEvents().empty());
  ASSERT_TRUE(Wait([&] { return room->HostLeftPending(); }, 3s));
  const auto events = room->TakePeerEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, PeerEvent::Kind::Left);
  EXPECT_EQ(events[0].reason, "room-hold-expired");
  EXPECT_EQ(room->Seat(), 1);
  EXPECT_EQ(room->Code(), code);
}

TEST_F(OrcaRoomRuntime, BlackoutFailsUpgradesSilentlyThenRecovers)
{
  ScopedEnv active("ORCA_SESSION", "1");
  ScopedEnv blackout("ORCA_TEST_BLACKOUT", "1-4");
  ScopedEnv before("ORCA_TEST_BLACKOUT_ROOM_AFTER", nullptr);
  const std::string code = "blackout";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  room->TakePeerEvents();
  std::this_thread::sleep_for(2200ms);
  const auto dark = Request("/state/" + code);
  EXPECT_EQ(dark.get("connects").get<double>(), 1);
  EXPECT_EQ(dark.get("tickets").get<double>(), 1);
  EXPECT_EQ(dark.get("closes").get<double>(), 0); // no close frame, no phantom upgrade
  EXPECT_FALSE(Sent(code, "leave"));
  ASSERT_TRUE(Wait([&] { return Connections(code) == 2 && room->RoundTripMs() >= 0; }, 6s));
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(room->TakePeerEvents().empty());
  EXPECT_EQ(room->Code(), code);
  EXPECT_EQ(room->GetState(), RoomState::Waiting);
}

TEST_F(OrcaRoomRuntime, BlackoutRoomReturnsBeforeTheDirectWindowEnds)
{
  ScopedEnv active("ORCA_SESSION", "1");
  ScopedEnv blackout("ORCA_TEST_BLACKOUT", "1-16");
  ScopedEnv before("ORCA_TEST_BLACKOUT_ROOM_AFTER", "12");
  const std::string code = "blackrace";
  const auto opened = RoomClock::now();
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  room->TakePeerEvents();
  ASSERT_TRUE(Wait([&] { return Connections(code) == 2; }, 8s));
  EXPECT_GE(RoomClock::now() - opened, 4s);
  EXPECT_LT(RoomClock::now() - opened, 12s);
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(room->TakePeerEvents().empty());
  EXPECT_EQ(room->Code(), code);
  EXPECT_EQ(room->Seat(), 1);
}

namespace
{
struct FriendsHeadlessCore
{
  FriendsHeadlessCore()
  {
    old_user = File::GetUserPath(D_USER_IDX);
    directory = File::CreateTempDir();
    File::SetUserPath(D_USER_IDX, directory + DIR_SEP);
    ::Config::Init();
    SConfig::Init();
    system.Initialize();
    old_wii = system.IsWii();
    system.SetIsWii(false);
    system.GetMemory().Init();
    Orca::Engine::SetModule(&module);
  }
  ~FriendsHeadlessCore()
  {
    Orca::UX::SelectModule(nullptr);
    system.GetMemory().Shutdown();
    system.SetIsWii(old_wii);
    SConfig::Shutdown();
    ::Config::Shutdown();
    File::SetUserPath(D_USER_IDX, old_user);
    File::DeleteDirRecursively(directory);
  }
  Core::System& system = Core::System::GetInstance();
  Orca::UX::GenericModule module;
  std::string old_user, directory;
  bool old_wii = false;
};

ReplayArchive FriendsReplay(int first, int current, int friends = 0)
{
  ReplayArchive replay;
  replay.first_frame = first;
  replay.origin_hash = 1234;
  replay.frames.resize(current - first + 1);
  auto ports = std::make_shared<std::vector<Orca::Events::PortInfo>>();
  for (int port = 0; port <= friends; ++port)
    ports->push_back({port, fmt::format("Player{}", port), false, {}, {}});
  for (auto& frame : replay.frames)
    frame.ports = ports;
  return replay;
}
}

TEST_F(OrcaRoomRuntime, LaunchJoinHostGoneBeforeCapsEndsAtTheBoundary)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  for (const bool soft : {false, true})
  {
    SCOPED_TRACE(fmt::format("caps join={}", soft));
    Orca::Status::SetAppCaps(soft ? "join" : "");
    const std::string code = soft ? "gonesoft370" : "goneboot370";
    Request("/configure/" + code,
            R"({"refuse":{"code":"host_gone"},"refuse_status":410})");
    Orca::Online::StartRoom(code, true, true);
    ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
    ASSERT_EQ(Orca::Online::Joining(), true);
    ASSERT_EQ(Orca::Online::RoomErrorCode(), "host_gone");
    EXPECT_EQ(Orca::Online::StatusLine(),
              "Your friend left that game. Ask them for a new invite.");
    const auto result = Rollback::OnlineMatch::PumpLaunchJoinBoundaryForTest();
    EXPECT_EQ(result.finished, !soft);
    EXPECT_EQ(result.has_port, soft);
    if (soft)
      EXPECT_FALSE(result.joining);
    // The stopped headless CPU must not advance a frame or touch a port after End().
    EXPECT_EQ(result.frame, -1);
    EXPECT_EQ(Connections(code), 1);
    Orca::Online::Shutdown();
  }
}

TEST_F(OrcaRoomRuntime, LaunchJoinWithoutAssignedSeatFallsBackToPortOne)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  Request("/configure/noseat393",
          R"({"refuse":{"code":"host_gone"},"refuse_status":410})");
  Orca::Online::StartRoom("noseat393", true, true);
  ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
  ASSERT_EQ(Orca::Online::Joining(), true);
  ASSERT_EQ(Orca::Online::RoomErrorCode(), "host_gone");
  ASSERT_EQ(Orca::Online::Seat(), -1);
  // Boundary -> FailJoin -> BecomeSolo(own_boot) -> Reseat, using the unassigned room seat.
  const auto result = Rollback::OnlineMatch::PumpLaunchJoinBoundaryForTest();
  EXPECT_FALSE(result.finished);
  EXPECT_FALSE(result.joining);
  EXPECT_TRUE(result.has_port);
  EXPECT_EQ(result.local_seat, 0);
  EXPECT_EQ(result.frame, -1);
  EXPECT_TRUE(result.log_empty);
  EXPECT_TRUE(result.queued_local_empty);
  EXPECT_EQ(Connections("noseat393"), 1);
}

TEST(OrcaReseat, UnassignedAndOutOfRangeSeatsDiscardQueuesAndPreserveHistory)
{
  Pads pads{};
  pads[0][0] = 0x12;
  pads[1][1] = 0x34;
  const std::vector<Pads> history{pads, Rollback::OnlineMatch::CanonicalBootPads()};
  for (const int seat : {-1, MAX_SEATS})
  {
    SCOPED_TRACE(seat);
    const auto result = Rollback::OnlineMatch::PumpReseatForTest(seat, history);
    EXPECT_EQ(result.local_seat, 0);
    EXPECT_EQ(result.running, -1);
    EXPECT_EQ(result.origin_frame, -1);
    EXPECT_EQ(result.log_base, 17);
    EXPECT_EQ(result.log, history);
    EXPECT_TRUE(result.queued_local_empty);
  }
}

TEST(OrcaReseat, UnassignedDoublesLeadAlsoDiscardsCoSeats)
{
  for (const int seat : {-1, MAX_SEATS})
  {
    SCOPED_TRACE(seat);
    const auto result = Rollback::OnlineMatch::PumpReseatForTest(seat, {}, {1, 3});
    EXPECT_EQ(result.local_seat, 0);
    EXPECT_TRUE(result.queued_local_empty);
    EXPECT_TRUE(result.co_seats.empty());
  }
}

TEST_F(OrcaRoomRuntime, FriendsReopenJoinsTheFriendWhoReturnedFirst)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  const std::string code = "friends359";
  // The friend rejoined during the old host's reopen delay and is now the room's host.
  Request("/configure/" + code,
          fmt::format(R"({{"joining":true,"compatibility":"{}"}})",
                      Orca::Online::CompatibilityKey()));
  Orca::Online::StartRoom(code, false, true, true);
  ASSERT_TRUE(Wait([] { return Orca::Online::InRoom() || Orca::Online::RoomEnded(); }));
  auto* room = static_cast<YouGameRoom*>(Orca::Online::Transport());
  ASSERT_NE(room, nullptr);
  ASSERT_TRUE(room->ErrorCode().empty()) << room->Status();
  const u64 generation = room->Generation();
  EXPECT_EQ(room->Joining(), true);
  EXPECT_EQ(room->Seat(), 1);
  EXPECT_TRUE(Rollback::OnlineMatch::PumpRoomReopenForTest());
  EXPECT_EQ(Orca::Online::Code(), code);
  EXPECT_EQ(room->Generation(), generation);
  ASSERT_TRUE(Wait([&] { return Sent(code, "msg", "hello"); }));
  const auto state = Request("/state/" + code);
  EXPECT_EQ(state.get("connects").get<double>(), 1);
  EXPECT_EQ(state.get("closes").get<double>(), 0);
  for (const auto& message : state.get("messages").get<picojson::array>())
  {
    EXPECT_NE(message.get("t").to_str(), "leave");
    const auto& data = message.get("d");
    if (data.is<picojson::object>() && data.get("k").to_str() == "hello")
      EXPECT_FALSE(data.get("h").evaluate_as_boolean());
  }
  EXPECT_TRUE(room->Connected());
  EXPECT_TRUE(room->ErrorCode().empty());
}

TEST_F(OrcaRoomRuntime, FriendsAutomaticReopenWithoutAHostFallsBackWithEitherCapability)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  for (const bool soft : {false, true})
  {
    SCOPED_TRACE(fmt::format("caps join={}", soft));
    Orca::Status::SetAppCaps(soft ? "join" : "");
    const std::string code = soft ? "rsoft359" : "return359";
    const std::string fresh = soft ? "freshsoft359" : "fresh359";
    const ScopedEnv fresh_room("ORCA_TEST_ROOM", fresh.c_str());
    // The friend holds the room's host seat, but its Orca is also waiting to join a host.
    Request("/configure/" + code,
            fmt::format(R"({{"joining":true,"host":false,"compatibility":"{}"}})",
                        Orca::Online::CompatibilityKey()));
    Request("/configure/" + fresh, R"({"joining":false})");
    Orca::Online::StartRoom(code, false, true, true);
    ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
    auto* room = static_cast<YouGameRoom*>(Orca::Online::Transport());
    ASSERT_NE(room, nullptr);
    const u64 generation = room->Generation();
    EXPECT_EQ(Rollback::OnlineMatch::PumpRoomReopenFailureForTest([&] {
                ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }, 10s));
                EXPECT_EQ(room->ErrorCode(), "peer_left");
              }), 1);
    ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
    EXPECT_EQ(Orca::Online::Code(), fresh);
    EXPECT_EQ(Orca::Online::Joining(), false);
    auto* reopened = static_cast<YouGameRoom*>(Orca::Online::Transport());
    ASSERT_NE(reopened, nullptr);
    EXPECT_NE(reopened->Generation(), generation);
    EXPECT_TRUE(reopened->ErrorCode().empty());
    Orca::Online::Shutdown();
  }
}

TEST_F(OrcaRoomRuntime, FriendsAutomaticReopenWaitsUntilAfterThePause)
{
  FriendsHeadlessCore core;
  {
    const ScopedEnv offline("ORCA_SESSION", "0");
    ASSERT_TRUE(Rollback::OnlineMatch::BeginPause());
  }
  const ScopedEnv session("ORCA_SESSION", "1");
  const Common::ScopeGuard cleanup([] {
    Rollback::OnlineMatch::EndPause();
    Rollback::OnlineMatch::TakePausedTime();
    Orca::Online::Shutdown();
  });
  const std::string code = "paused359";
  Request("/configure/" + code,
          fmt::format(R"({{"joining":true,"compatibility":"{}"}})",
                      Orca::Online::CompatibilityKey()));
  Orca::Online::StartRoom(code, false, true, true);
  ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
  EXPECT_FALSE(Rollback::OnlineMatch::PumpRoomReopenForTest());
  EXPECT_FALSE(Rollback::OnlineMatch::PumpRoomReopenForTest());
  EXPECT_TRUE(Rollback::OnlineMatch::Pausing());
  Rollback::OnlineMatch::EndPause();
  EXPECT_TRUE(Rollback::OnlineMatch::PumpRoomReopenForTest());
  EXPECT_EQ(Orca::Online::Code(), code);
}

TEST(OrcaRoomResilience, FourShortLivedRejoinsEndTheRoomExactlyOnce)
{
  std::vector<RoomTestStep> steps{{RK::Message, 0, RosterMessage("welcome")},
                                 {RK::SocketClose, 100, "WebSocket closed by the server"}};
  for (int i = 1; i <= 4; ++i)
  {
    steps.push_back({RK::Message, i * 1000, RosterMessage("welcome")});
    steps.push_back({RK::SocketClose, i * 1000 + 100, "WebSocket closed by the server"});
  }
  steps.push_back({RK::SocketClose, 5000, "WebSocket closed by the server"});
  steps.push_back({RK::Tick, 6000});
  const auto trace = TraceRoomForTest(TraceOptions(), steps);
  unsigned endings = 0;
  for (size_t i = 0; i < trace.size(); ++i)
  {
    EXPECT_EQ(trace[i].state == RoomState::Ended, i >= 9);
    for (const auto& line : trace[i].notices)
    {
      if (line.find("ended room-unstable") != std::string::npos)
      {
        ++endings;
        EXPECT_EQ(line.find("room-away self eof"), 0u);
        EXPECT_NE(line.find("short-lived=4 window_s=300"), std::string::npos);
      }
    }
  }
  EXPECT_EQ(endings, 1u);
  EXPECT_EQ(trace.back().error_code, "room-unstable");
}

TEST(OrcaRoomResilience, LongLivedRejoinsNeitherCountNorResetShortLivedHistory)
{
  RoomRejoinFlaps flaps;
  const auto start = RoomClock::time_point{};
  EXPECT_FALSE(flaps.Lost(start));  // The initial socket isn't a rejoin.
  for (const auto seconds : {1, 2, 3})
  {
    flaps.Rejoined(start + std::chrono::seconds(seconds));
    EXPECT_FALSE(flaps.Lost(start + std::chrono::seconds(seconds) + 1ms));
    EXPECT_FALSE(flaps.Lost(start + std::chrono::seconds(seconds) + 2ms));
  }
  for (const auto seconds : {4, 65, 127})
  {
    flaps.Rejoined(start + std::chrono::seconds(seconds));
    EXPECT_FALSE(flaps.Lost(start + std::chrono::seconds(seconds) + 60s));
    EXPECT_EQ(flaps.Count(start + std::chrono::seconds(seconds) + 60s), 3u);
  }
  flaps.Rejoined(start + 188s);
  EXPECT_TRUE(flaps.Lost(start + 188s + 59999ms));
}

TEST(OrcaRoomResilience, ShortLivedRejoinsOutsideTheRollingWindowNeverEndTheRoom)
{
  RoomRejoinFlaps flaps;
  const auto start = RoomClock::time_point{};
  std::vector<RoomTestStep> steps{{RK::Message, 0, RosterMessage("welcome")},
                                 {RK::SocketClose, 0, "WebSocket closed by the server"}};
  for (int i = 0; i < 10; ++i)
  {
    flaps.Rejoined(start + i * 100s);
    EXPECT_FALSE(flaps.Lost(start + i * 100s + 1ms));
    EXPECT_LE(flaps.Count(start + i * 100s + 1ms), 3u);
    steps.push_back({RK::Message, i * 100000 + 80000, RosterMessage("welcome")});
    steps.push_back({RK::SocketClose, i * 100000 + 99999, "WebSocket closed by the server"});
  }
  const auto trace = TraceRoomForTest(TraceOptions(), steps);
  for (const auto& snapshot : trace)
    EXPECT_NE(snapshot.state, RoomState::Ended);
  EXPECT_EQ(flaps.Count(start + 1200s + 1ms), 0u);
}

TEST(OrcaRoomResilience, ThreeShortLivedRejoinsAndHealthyRejoinsKeepTheRoom)
{
  std::vector<RoomTestStep> steps{{RK::Message, 0, RosterMessage("welcome")},
                                 {RK::SocketClose, 0, "WebSocket closed by the server"}};
  for (int i = 1; i <= 3; ++i)
  {
    steps.push_back({RK::Message, i * 1000, RosterMessage("welcome")});
    steps.push_back({RK::SocketClose, i * 1000 + 1, "WebSocket closed by the server"});
  }
  for (int i = 0; i < 3; ++i)
  {
    steps.push_back({RK::Message, i * 61000 + 4000, RosterMessage("welcome")});
    steps.push_back({RK::SocketClose, i * 61000 + 64000, "WebSocket closed by the server"});
  }
  const auto trace = TraceRoomForTest(TraceOptions(), steps);
  for (const auto& snapshot : trace)
    EXPECT_NE(snapshot.state, RoomState::Ended);
  ASSERT_FALSE(trace.back().notices.empty());
  EXPECT_NE(trace.back().notices.back().find("short-lived=3 window_s=300"), std::string::npos);
}

TEST(OrcaRoomResilience, ReconnectUpgradeCapsGrowWithinTheOriginalHold)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 1000, "WebSocket closed by the server"},
      {RK::PollReconnect, 1000}, {RK::ConnectFailure, 1000, "timeout"},
      {RK::PollReconnect, 1500}, {RK::ConnectFailure, 1500, "timeout"},
      {RK::PollReconnect, 2500}, {RK::ConnectFailure, 2500, "timeout"},
      {RK::PollReconnect, 89000}, {RK::ConnectFailure, 89000, "timeout"},
      {RK::PollReconnect, 91000},
  });
  for (const auto& [index, cap] : std::vector<std::pair<size_t, int>>{
           {3, 5000}, {5, 8000}, {7, 10000}, {9, 2000}})
  {
    ASSERT_FALSE(trace[index].notices.empty());
    EXPECT_NE(trace[index].notices.front().find(fmt::format("cap_ms={}", cap)), std::string::npos);
    EXPECT_NE(trace[index].notices.front().find("ticket_age="), std::string::npos);
  }
  EXPECT_EQ(trace.back().state, RoomState::Ended);
  RoomReconnect reconnect;
  const auto now = RoomClock::time_point{};
  ASSERT_TRUE(reconnect.Start(now, 0));
  EXPECT_EQ(reconnect.UpgradeCap(now, now + 1234ms), 1234ms);
  EXPECT_EQ(reconnect.UpgradeCap(now, now - 1ms), 0ms);
}

TEST_F(OrcaRoomRuntime, ReconnectUsesFreshTicketsAfterLongRoomsAndKeepsRecentSeatTickets)
{
  for (const auto age : {100s, 240s, 1800s})
  {
    SCOPED_TRACE(age.count());
    const std::string code = fmt::format("aged{}", age.count());
    auto room = Open(code);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
    u32 before;
    room->RoundTripMs(&before);
    room->TestDropConnection(age);
    ASSERT_TRUE(Wait([&] {
      u32 sequence;
      room->RoundTripMs(&sequence);
      return sequence > before && Connections(code) == 2;
    }));
    auto state = Request("/state/" + code);
    const int ticket = age < 240s ? 1 : 2;
    EXPECT_EQ(state.get("tickets").get<double>(), ticket);
    auto uses = state.get("ticket_uses").get<picojson::array>();
    ASSERT_EQ(uses.size(), 2u);
    EXPECT_EQ(uses[0].get<double>(), 1);
    EXPECT_EQ(uses[1].get<double>(), ticket);
    // A successful mint records its age too: the next drop keeps that new seat ticket.
    room->RoundTripMs(&before);
    room->TestDropConnection();
    ASSERT_TRUE(Wait([&] {
      u32 sequence;
      room->RoundTripMs(&sequence);
      return sequence > before && Connections(code) == 3;
    }));
    state = Request("/state/" + code);
    EXPECT_EQ(state.get("tickets").get<double>(), ticket);
    uses = state.get("ticket_uses").get<picojson::array>();
    ASSERT_EQ(uses.size(), 3u);
    EXPECT_EQ(uses.back().get<double>(), ticket);
  }
}

TEST_F(OrcaRoomRuntime, ReconnectTicketRefreshFailureRetriesAfterTheCachedTicketRefusal)
{
  const std::string code = "refresh368";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  Action(code, R"({"reject_first_ticket":true,"ticket_fail_count":1})");
  u32 before;
  room->RoundTripMs(&before);
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] {
    u32 sequence;
    room->RoundTripMs(&sequence);
    return room->GetState() == RoomState::Ended || sequence > before;
  }));
  EXPECT_NE(room->GetState(), RoomState::Ended) << room->Status();
  EXPECT_TRUE(room->ErrorCode().empty());
  const auto state = Request("/state/" + code);
  EXPECT_EQ(state.get("tickets").get<double>(), 3);
  const auto uses = state.get("ticket_uses").get<picojson::array>();
  ASSERT_EQ(uses.size(), 4u);
  EXPECT_EQ(uses[1].get<double>(), 1);
  EXPECT_EQ(uses[2].get<double>(), 1);
  EXPECT_EQ(uses[3].get<double>(), 3);
}

TEST_F(OrcaRoomRuntime, ReconnectAllowsASlowUpgradeWithoutBlockingLeave)
{
  const std::string code = "slowup368";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  Action(code, R"({"upgrade_delay":3})");
  u32 before;
  room->RoundTripMs(&before);
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] {
    u32 sequence;
    room->RoundTripMs(&sequence);
    return sequence > before;
  }, 6s));
  EXPECT_EQ(Connections(code), 2);
  Action(code, R"({"upgrade_delay":20})");
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] { return Connections(code) == 3; }));
  // The in-flight reconnect keeps its delay; the separate best-effort leave can finish.
  Action(code, R"({"upgrade_delay":0})");
  const auto start = RoomClock::now();
  room->Leave();
  EXPECT_LT(RoomClock::now() - start, 2s);
  EXPECT_TRUE(Sent(code, "leave"));
}

TEST_F(OrcaRoomRuntime, ReconnectAllowsASlowTicketMintOnItsWorkerAndCancelsLeave)
{
  const std::string code = "slowmint368";
  auto room = Open(code);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  Action(code, R"({"ticket_delay":11})");
  u32 before;
  room->RoundTripMs(&before);
  room->TestDropConnection(1800s);
  ASSERT_TRUE(Wait([&] {
    u32 sequence;
    room->RoundTripMs(&sequence);
    return sequence > before;
  }, 15s));
  EXPECT_EQ(Connections(code), 2);
  EXPECT_TRUE(room->ErrorCode().empty());
  Action(code, R"({"ticket_delay":20})");
  room->TestDropConnection(1800s);
  ASSERT_TRUE(Wait([&] { return Request("/state/" + code).get("tickets").get<double>() == 3; }));
  const auto start = RoomClock::now();
  room->Leave();
  EXPECT_LT(RoomClock::now() - start, 2s);
}

TEST_F(OrcaRoomRuntime, KeepRoomOpenReplacesUnstableCodesAndPreservesItsGiveUpBudget)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv fresh_room("ORCA_TEST_ROOM", nullptr);
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  for (const bool time_limit : {false, true})
  {
    SCOPED_TRACE(time_limit);
    const auto room_code = [&](int number) {
      return fmt::format("flap370{}x{}", time_limit ? 1 : 0, number);
    };
    Request("/configure/" + room_code(0), R"({"joining":false})");
    Orca::Online::StartRoom(room_code(0), false, true);
    const auto end_unstable = [&] {
      ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
      auto* room = static_cast<YouGameRoom*>(Orca::Online::Transport());
      ASSERT_NE(room, nullptr);
      for (int i = 0; i < 5; ++i)
      {
        ASSERT_TRUE(Wait([&] { return room->RoundTripMs() >= 0; }));
        u32 before;
        room->RoundTripMs(&before);
        Action(Orca::Online::Code(), R"({"close":1013})");
        ASSERT_TRUE(Wait([&] {
          u32 after;
          room->RoundTripMs(&after);
          return room->GetState() == RoomState::Ended || after > before;
        }));
      }
      ASSERT_TRUE(Orca::Online::RoomEnded());
      EXPECT_EQ(Orca::Online::RoomErrorCode(), "room-unstable");
      EXPECT_TRUE(Orca::Online::ReportSessionEnd("room ended", false));
    };
    auto* out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    Orca::Status::SetOutput(out);
    const Common::ScopeGuard output_cleanup([&] {
      Orca::Status::SetOutput(nullptr);
      std::fclose(out);
    });
    std::vector<std::chrono::seconds> times;
    if (time_limit)
      times = {0s, 1s, 2s, 301s, 302s};
    else
      for (int i = 0; i <= 22; ++i)
        times.push_back(std::chrono::seconds(i));
    const int last_host_step = time_limit ? 2 : 20;
    int step = 0;
    const auto attempts = Rollback::OnlineMatch::TraceRoomReopenForTest(times, [&] {
      if (step <= last_host_step)
      {
        ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
        EXPECT_EQ(Orca::Online::Code(), room_code((step + 1) / 2));
        if (step % 2 == 0)
        {
          end_unstable();
          const std::string next = room_code(step / 2 + 1);
          Request("/configure/" + next, R"({"joining":false})");
          SetEnv("ORCA_TEST_ROOM", next.c_str());
        }
      }
      ++step;
    });
    for (size_t i = 0; i < attempts.size(); ++i)
      EXPECT_EQ(attempts[i], time_limit ? std::min<unsigned>(1, static_cast<unsigned>((i + 1) / 2)) :
                                         std::min<unsigned>(10, static_cast<unsigned>((i + 1) / 2)));
    const unsigned reopens = time_limit ? 1 : 10;
    for (unsigned i = 0; i <= reopens; ++i)
      EXPECT_EQ(Connections(room_code(i)), 5);  // No old code was opened again.
    EXPECT_EQ(Connections(room_code(reopens + 1)), 0);
    std::rewind(out);
    std::string lines;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), out))
      lines += buffer;
    const std::string notice = "orca error network The room keeps disconnecting. Opening a new room; send your friend a new invite.";
    unsigned notices = 0;
    for (size_t at = 0; (at = lines.find(notice, at)) != std::string::npos; at += notice.size())
      ++notices;
    EXPECT_EQ(notices, reopens) << lines;
    const std::string network = "orca error network ";
    unsigned errors = 0;
    for (size_t at = 0; (at = lines.find(network, at)) != std::string::npos; at += network.size())
      ++errors;
    EXPECT_EQ(errors, reopens + 1);  // One per replacement, then the final give-up.
    EXPECT_EQ(lines.find("orca error room-unstable"), std::string::npos);
    const std::string give_up = "orca state no-room\n";
    const auto at = lines.find(give_up);
    ASSERT_NE(at, std::string::npos) << lines;
    EXPECT_EQ(lines.find(give_up, at + give_up.size()), std::string::npos);
    Orca::Online::Shutdown();
  }
}

TEST_F(OrcaRoomRuntime, KeepRoomOpenUnstableRefusalsImmediatelyOpenNewCodes)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv fresh_room("ORCA_TEST_ROOM", nullptr);
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  for (const bool initial : {false, true})
  {
    const std::string code = initial ? "uinitial370" : "urejoin370";
    const std::string next = code + "n";
    const std::string refusal =
        R"({"joining":false,"refuse":{"error":"room unstable","code":"room_unstable"},"refuse_status":409})";
    Request("/configure/" + code, initial ? refusal : R"({"joining":false})");
    Orca::Online::StartRoom(code, false, true);
    if (!initial)
    {
      ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
      Action(code, R"({"refuse":{"error":"room unstable","code":"room_unstable"},"refuse_status":409,"close":1013})");
    }
    ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
    ASSERT_EQ(Orca::Online::RoomErrorCode(), "room-unstable");
    Request("/configure/" + next, R"({"joining":false})");
    SetEnv("ORCA_TEST_ROOM", next.c_str());
    const auto attempts = Rollback::OnlineMatch::TraceRoomReopenForTest({0s, 1s}, [&] {
      ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
      EXPECT_EQ(Orca::Online::Code(), next);
    });
    EXPECT_EQ(attempts, (std::vector<unsigned>{1, 1}));
    EXPECT_EQ(Connections(code), initial ? 1 : 2);
    EXPECT_EQ(Connections(next), 1);
    Orca::Online::Shutdown();
  }
}

TEST_F(OrcaRoomRuntime, KeepRoomOpenHealthyReplacementEndsTheUnstableEpisode)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv fresh_room("ORCA_TEST_ROOM", "healthy370");
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  const std::string code = "healthy370";
  Request("/configure/oldhealth370",
          R"({"joining":false,"refuse":{"code":"room_unstable"},"refuse_status":409})");
  Request("/configure/" + code, R"({"joining":false})");
  Orca::Online::StartRoom("oldhealth370", false, true);
  ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
  ASSERT_EQ(Orca::Online::RoomErrorCode(), "room-unstable");
  int step = 0;
  const auto attempts = Rollback::OnlineMatch::TraceRoomReopenForTest(
      {0s, 1s, 300s, 301s, 1801s, 1802s, 1804s, 1805s}, [&] {
        if (step <= 4 || step >= 6)
        {
          ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
          EXPECT_EQ(Orca::Online::Code(), code);
        }
        if (step == 4)
        {
          // An ordinary final room loss after thirty minutes of healthy replacement hosting.
          Action(code, R"({"close":4003})");
          ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
          EXPECT_EQ(Orca::Online::RoomErrorCode(), "network");
        }
        ++step;
      });
  // The replacement keeps attempt 1 until exactly five healthy minutes have elapsed.
  // The later ordinary loss starts at attempt 1 again, rather than giving up immediately.
  EXPECT_EQ(attempts, (std::vector<unsigned>{1, 1, 1, 0, 0, 0, 1, 0}));
  EXPECT_EQ(Connections("oldhealth370"), 1);
  EXPECT_EQ(Connections(code), 2);
}

TEST_F(OrcaRoomRuntime, KeepRoomOpenStopsAfterTenFailuresOrFiveMinutesAndTellsTheAppOnce)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  const std::vector<std::vector<std::chrono::seconds>> times{
      {0s, 2s, 6s, 14s, 30s, 60s, 90s, 120s, 150s, 180s, 210s, 211s, 600s},
      {0s, 2s, 299s, 300s, 301s, 600s},
  };
  for (size_t i = 0; i < times.size(); ++i)
  {
    SCOPED_TRACE(i);
    const std::string code = fmt::format("giveup368{}", i);
    const ScopedEnv fresh_room("ORCA_TEST_ROOM", code.c_str());
    Request("/configure/" + code, R"({"joining":false,"refuse":"fixture unavailable","refuse_status":503})");
    Orca::Online::StartRoom(code, false, true);
    ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
    ASSERT_EQ(Orca::Online::RoomErrorCode(), "network");
    auto* out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    Orca::Status::SetOutput(out);
    const Common::ScopeGuard output_cleanup([&] {
      Orca::Status::SetOutput(nullptr);
      std::fclose(out);
    });
    const auto attempts = Rollback::OnlineMatch::TraceRoomReopenForTest(times[i], [&] {
      ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
    });
    ASSERT_EQ(attempts.size(), times[i].size());
    EXPECT_EQ(attempts, i == 0 ? std::vector<unsigned>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 10}) :
                                std::vector<unsigned>({0, 1, 2, 2, 2, 2}));
    EXPECT_EQ(Connections(code), i == 0 ? 11 : 3);
    std::rewind(out);
    std::string lines;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), out))
      lines += buffer;
    const std::string error = "orca error network The room couldn't reconnect.";
    const std::string state = "orca state no-room\n";
    ASSERT_NE(lines.find(error), std::string::npos) << lines;
    ASSERT_NE(lines.find(state), std::string::npos) << lines;
    EXPECT_EQ(lines.find(error, lines.find(error) + error.size()), std::string::npos);
    EXPECT_EQ(lines.find(state, lines.find(state) + state.size()), std::string::npos);
    EXPECT_GT(lines.find(state), lines.find(error));
    Orca::Online::Shutdown();
  }
}

TEST_F(OrcaRoomRuntime, KeepRoomOpenCancelsAPendingReopenAtTheNetworkDeadline)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  const std::string code = "pending368";
  const ScopedEnv fresh_room("ORCA_TEST_ROOM", code.c_str());
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  Request("/configure/" + code, R"({"joining":false,"refuse":"fixture unavailable","refuse_status":503})");
  Orca::Online::StartRoom(code, false, true);
  ASSERT_TRUE(Wait([] { return Orca::Online::RoomEnded(); }));
  Action(code, R"({"upgrade_delay":20})");
  int step = 0;
  const auto attempts = Rollback::OnlineMatch::TraceRoomReopenForTest({0s, 2s, 300s, 600s}, [&] {
    if (step++ == 1)
    {
      ASSERT_TRUE(Wait([&] { return Connections(code) == 2; }));
      EXPECT_FALSE(Orca::Online::RoomEnded());
    }
    else
    {
      EXPECT_TRUE(Orca::Online::RoomEnded());
    }
  });
  EXPECT_EQ(attempts, (std::vector<unsigned>{0, 1, 1, 1}));
  EXPECT_EQ(Orca::Online::Transport(), nullptr);
  EXPECT_EQ(Connections(code), 2);
}

TEST_F(OrcaRoomRuntime, CompletedRecoveryEndsEitherSessionAwayFromItsNextFrame)
{
  FriendsHeadlessCore core;
  const ScopedEnv session_env("ORCA_SESSION", "1");
  Orca::Status::SetAppCaps("join");
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Status::SetAppCaps("");
  });
  for (const bool joining : {false, true})
  {
    SCOPED_TRACE(fmt::format("joining={}", joining));
    const std::string code = joining ? "settledj359" : "settled359";
    Request("/configure/" + code,
            fmt::format(R"({{"queue":"casual","recovery":true,"joining":{},"compatibility":"{}"}})",
                        joining, Orca::Online::CompatibilityKey()));
    Orca::Online::StartRoom(code, joining, false);
    ASSERT_TRUE(Wait([] { return Orca::Online::InRoom(); }));
    auto* room = static_cast<YouGameRoom*>(Orca::Online::Transport());
    ASSERT_NE(room, nullptr);
    Action(code, fmt::format(R"({{"message":{}}})",
                            QueueRoster("start", "casual", "true", joining)));
    ASSERT_TRUE(Wait([&] { return room->MatchLive(); }));
    ASSERT_TRUE(room->HoldInputSilence(RoomClock::now(), RoomClock::now()));
    Action(code, R"({"message":{"t":"result","matchId":"queue-recovery-match","ranking":["me","peer"],"disposition":"completed"}})");
    ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }));
    ASSERT_EQ(room->ErrorCode(), "match-over");

    RoomHashGame game;
    Orca::Net::Config config;
    config.local_seat = joining ? 1 : 0;
    config.start_frame = 10;
    auto session = std::make_unique<Session>(config, game, *room);
    // Rejoin can finish while the session is at a different frame from the emulation boundary.
    EXPECT_EQ(Rollback::OnlineMatch::PumpFriendsHostBoundaryForTest(
                  session, 10, {joining ? 0 : 1}, [] {}, [] { return std::vector<int>{}; }, joining), 11);
    EXPECT_EQ(session, nullptr);
    Orca::Online::Shutdown();
  }
}

TEST_F(OrcaRoomRuntime, RankedRecordingUsesTheMatchedRoomAndAgreedCapability)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  // Use the commands the queue driver actually sends, so its capability agreement exercises the
  // same recording decision as the app. No subprocess, account or external service is involved.
  std::string driver;
  const auto root = std::filesystem::path(__FILE__).parent_path() / "../../../..";
  ASSERT_TRUE(File::ReadFileToString((root / "Tools/orca/queue-e2e.mjs").string(), driver));
  Orca::Profile profile;
  profile.game_id = "RSBE01";
  profile.revision = 2;
  Orca::SetActiveProfile(profile);
  Orca::UX::SelectModule(Orca::ActiveProfile());
  const Common::ScopeGuard cleanup([] {
    Orca::Online::Shutdown();
    Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::Auto);
    Orca::Status::SetAppCaps("");
    Orca::SetActiveProfile(std::nullopt);
  });
  for (const bool q2 : {false, true})
  {
    const std::string caps =
        std::string(Orca::Status::CAPS) + " record" + (q2 ? " queue2" : "");
    for (const bool joining : {false, true})
    {
      SCOPED_TRACE(fmt::format("q2={} joining={}", q2, joining));
      const std::string code = fmt::format("record{}{}", q2 ? 2 : 1, joining ? 1 : 0);
      Request("/configure/" + code,
              fmt::format(R"({{"queue":"ranked","joining":{},"compatibility":"{}"}})",
                          joining, Orca::Online::CompatibilityKey()));
      Orca::Status::SetAppCaps(caps);
      ASSERT_EQ(Orca::Status::Cap("queue2"), q2);
      Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::Auto);
      const std::string solo = "own" + code;
      Request("/configure/" + solo, R"({"joining":false})");
      Orca::Online::StartRoom(solo, false);
      ASSERT_TRUE(Wait([&] { return Orca::Online::Seat() == 0; }));
      ASSERT_EQ(Orca::Online::RoomQueue(), "private");
      // HostInPlay/JoinInPlay replace the solo/search room with this matched room.
      Orca::Online::ShutdownInBackground();
      Orca::Online::StartRoom(code, joining, false);
      ASSERT_TRUE(Wait([&] { return Orca::Online::Seat() == (joining ? 1 : 0); }));
      EXPECT_EQ(Orca::Online::Code(), code);
      EXPECT_EQ(Orca::Online::RoomQueue(), "ranked");
      ASSERT_TRUE(Orca::Status::Cap(Orca::Status::RECORD_CAP));
      EXPECT_TRUE(Orca::Online::RecordWanted());
      Rollback::ReplayStream::Writer writer("", true, true);
      const auto replay = FriendsReplay(230, 300);
      ASSERT_TRUE(writer.Pump(replay, replay.EndFrame(), 300, nullptr, true));
      EXPECT_TRUE(Wait([&] { return Sent(code, "rs"); }));
      fmt::print("record proof: q{} code={} seat={} queue={} cap={} auto={} stream={}\n",
                 q2 ? 2 : 1, Orca::Online::Code(), Orca::Online::Seat(),
                 Orca::Online::RoomQueue(), Orca::Status::Cap(Orca::Status::RECORD_CAP),
                 Orca::Online::RecordWanted(), Sent(code, "rs"));
      Orca::Status::SetAppCaps(q2 ? "join host queue2" : "join host");
      EXPECT_FALSE(Orca::Status::Cap(Orca::Status::RECORD_CAP));
      EXPECT_FALSE(Orca::Online::RecordWanted());
      Orca::Status::SetAppCaps(caps);
      Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::Off);
      EXPECT_FALSE(Orca::Online::RecordWanted());
      const std::string prefix = fmt::format("const CAPS_Q{} = \"caps ", q2 ? 2 : 1);
      const auto command = driver.find(prefix);
      ASSERT_NE(command, std::string::npos);
      const auto first = command + prefix.size();
      const auto last = driver.find('"', first);
      ASSERT_NE(last, std::string::npos);
      Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::Auto);
      Orca::Status::SetAppCaps(driver.substr(first, last - first));
      fmt::print("queue driver proof: q{} code={} queue={} cap={} auto={}\n", q2 ? 2 : 1,
                 Orca::Online::Code(), Orca::Online::RoomQueue(),
                 Orca::Status::Cap(Orca::Status::RECORD_CAP), Orca::Online::RecordWanted());
      EXPECT_TRUE(Orca::Status::Cap(Orca::Status::RECORD_CAP));
      EXPECT_TRUE(Orca::Online::RecordWanted());
      Orca::Online::Shutdown();
    }
    for (const std::string queue : {"casual", "private"})
    {
      SCOPED_TRACE(fmt::format("q2={} queue={}", q2, queue));
      const std::string code = fmt::format("{}rec{}", queue, q2 ? 2 : 1);
      Request("/configure/" + code,
              fmt::format(R"({{"queue":"{}","joining":false}})", queue));
      Orca::Status::SetAppCaps(caps);
      Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::Auto);
      Orca::Online::StartRoom(code, false, queue == "private");
      ASSERT_TRUE(Wait([&] { return Orca::Online::Seat() == 0; }));
      EXPECT_EQ(Orca::Online::RoomQueue(), queue);
      EXPECT_FALSE(Orca::Online::RecordWanted());
      const auto replay = FriendsReplay(230, 300);
      Rollback::ReplayStream::Writer quiet("", true, true);
      ASSERT_TRUE(quiet.Pump(replay, replay.EndFrame(), 300, nullptr, true));
      EXPECT_FALSE(Sent(code, "rs"));
      Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::On);
      EXPECT_TRUE(Orca::Online::RecordWanted());
      Rollback::ReplayStream::Writer enabled("", true, true);
      ASSERT_TRUE(enabled.Pump(replay, replay.EndFrame(), 300, nullptr, true));
      EXPECT_TRUE(Wait([&] { return Sent(code, "rs"); }));
      Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::Off);
      EXPECT_FALSE(Orca::Online::RecordWanted());
      Orca::Status::SetAppCaps("");
      Orca::Online::SetRecordChoice(Orca::Online::RecordChoice::On);
      EXPECT_FALSE(Orca::Online::RecordWanted());
      Orca::Online::Shutdown();
    }
  }
}

TEST_F(OrcaRoomRuntime, ReturningFriendUsesActualHostCurrentMatchKeyframe)
{
  // Core RAM represents the running match. The origin/menu has a different checksum.
  FriendsHeadlessCore core;
  constexpr int origin = 1524;
  constexpr int current = 6000;
  core.system.GetMemory().GetRAM()[0] = 0x42;
  const auto expected_match_hash = Rollback::RamChecksum(
      {core.system.GetMemory().GetRAM(), core.system.GetMemory().GetRamSize()}, {});
  for (bool session_alive : {true, false})
  {
    SCOPED_TRACE(session_alive);
    const std::string code = session_alive ? "midlive" : "mididle";
    auto room = Open(code, R"({"joining":false})", false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    room->TakePeerEvents();
    Packet ack;
    ack.seat = 0;
    ack.ack[1] = current - 1;
    room->Send(ack);
    // A shared session: the friend's inputs up to our stalled frame reached us first.
    Packet played = NeutralFrames(current - 16, 16);
    played.seat = 1;
    Action(code, InjectPacket(played));
    ASSERT_TRUE(Wait([&] { return !room->Receive().empty(); }));
    Action(code, fmt::format(R"({{"message":{}}})", RosterMessage("back", false)));
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(room->TakePeerEvents().empty());
    // The returning inputs cannot cover our stalled frame: use the real Receive->Tick resync.
    Packet gap = NeutralFrames(current + 1000, 16);
    gap.seat = 1;
    Action(code, InjectPacket(gap));
    EXPECT_TRUE(Wait([&] {
      room->Receive();
      return Sent(code, "msg", "drop");
    }));
    // Tick's fresh hello reaches the real handshake, which announces a recovering arrival.
    ASSERT_TRUE(Wait([&] { return room->ArrivalPending(); }));
    ReplayArchive replay;
    replay.first_frame = origin;
    replay.origin_hash = 0x1234; // menu's checksum, deliberately not the running match
    replay.frames.resize(current - origin + 1);
    const auto ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
        std::vector<Orca::Events::PortInfo>{{0, "Me", false, {}, {}},
                                            {1, "Friend", true, {}, {}}});
    for (auto& frame : replay.frames)
      frame.ports = ports;
    RoomHashGame game;
    auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                          std::string* error) {
      return room->FreshTicket(fresh, ticket, server, error);
    });
    const auto result = Rollback::OnlineMatch::PumpRecoveryHostForTest(
        core.system, *room, game, std::move(replay), current, session_alive, std::move(store));
    EXPECT_FALSE(result.fresh_start);
    EXPECT_TRUE(result.seated);
    EXPECT_EQ(result.current_frame, current);
    EXPECT_EQ(result.keyframe_frame, current);
    ASSERT_TRUE(Wait([&] { return Sent(code, "msg", "kf"); }));
    bool checked = false;
    const auto state = Request("/state/" + code);
    for (const auto& message : state.get("messages").get<picojson::array>())
    {
      const auto& data = message.get("d");
      if (!data.is<picojson::object>() || data.get("k").to_str() != "kf")
        continue;
      auto offer = KeyframeOfferFromJson(data.get<picojson::object>());
      ASSERT_TRUE(offer);
      EXPECT_EQ(offer->frame, current);
      EXPECT_NE(offer->frame, origin);
      auto blob = offer->inline_blob;
      ASSERT_FALSE(blob.empty());
      ASSERT_TRUE(DecryptKeyframe(offer->frame, offer->key, &blob));
      ReplayArchive unpacked;
      int frame = -1;
      ASSERT_TRUE(UnpackKeyframe(blob, &frame, &unpacked));
      EXPECT_EQ(frame, current);
      EXPECT_EQ(unpacked.first_frame, origin);
      EXPECT_NE(unpacked.target_hash, unpacked.origin_hash);
      EXPECT_EQ(unpacked.target_hash, expected_match_hash);
      checked = true;
    }
    EXPECT_TRUE(checked);
    EXPECT_EQ(core.system.GetMemory().GetRAM()[0], 0x42); // match state never restored to menu
    EXPECT_EQ(room->Code(), code);
    EXPECT_EQ(room->GetState(), RoomState::Waiting);
  }
}

TEST_F(OrcaRoomRuntime, QueueBackAndJoinKeepTheirOriginalRuntimePolicy)
{
  for (const char* queue : {"casual", "ranked"})
  {
    for (const char* event : {"back", "join"})
    {
      const std::string code = std::string(queue) + event;
      auto room = Open(code, fmt::format(R"({{"queue":"{}","joining":false}})", queue), false, false);
      ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
      room->TakePeerEvents();
      Action(code, fmt::format(R"({{"message":{}}})", RosterMessage(event, false)));
      std::vector<PeerEvent> events;
      ASSERT_TRUE(Wait([&] {
        auto next = room->TakePeerEvents();
        events.insert(events.end(), next.begin(), next.end());
        return events.size() >= 2;
      }));
      ASSERT_EQ(events.size(), 2u);
      EXPECT_EQ(events[0].kind, PeerEvent::Kind::Left);
      EXPECT_EQ(events[1].kind, PeerEvent::Kind::Arrived);
      EXPECT_FALSE(events[1].recovering);
      const auto now = RoomClock::now();
      EXPECT_FALSE(room->HoldInputSilence(now, now));
      EXPECT_EQ(room->Queue(), queue);
    }
  }
}

TEST_P(OrcaRoomStallRuntime, OrdinaryStallsLeaveRecoveryAllowanceForDarkJoiner)
{
  OpenDirect("budgetdark", true);
  ExpectDarkRecovery(15s);
}

TEST_P(OrcaRoomStallRuntime, DarkHostFifteenSecondsKeepsTheSessionAfterAnotherStall)
{
  OpenDirect("budgethost", false);
  ExpectDarkRecovery(15s);
}

TEST_P(OrcaRoomStallRuntime, DarkJoinerSixtySecondsKeepsTheSessionAfterAnotherStall)
{
  OpenDirect("budgetlong", true);
  ExpectDarkRecovery(60s);
}

TEST_P(OrcaRoomStallRuntime, FourthRecoveredOutageInTenMinutesCounts)
{
  OpenDirect("budgetcap", true);
  ASSERT_TRUE(session);
  ASSERT_EQ(StallFor(200ms).kind, StepKind::Run);
  for (int outage = 0; outage < 3; ++outage)
  {
    // A recovered outage worth an excuse is at least MIN_EXCUSED_STALL long.
    ASSERT_EQ(StallFor(2100ms, false, true).kind, StepKind::Run)
        << "outage " << outage + 1 << ": " << session->Error() << ": " << session->Describe();
    EXPECT_TRUE(last_excused);
    EXPECT_EQ(budget.Recoveries(), outage + 1);
  }
  ASSERT_EQ(StallFor(14900ms, false, true).kind, StepKind::Run);
  EXPECT_FALSE(last_excused);
  EXPECT_EQ(budget.Recoveries(), 3);
  EXPECT_GE(budget.Stalled(RoomClock::now()), 15s);
  EXPECT_EQ(StallFor(100ms).kind, StepKind::Ended);
}

TEST_P(OrcaRoomStallRuntime, RollbackSplitsOneOutageWithoutSpendingAnotherRecovery)
{
  OpenDirect("budgetsplt", true);
  ASSERT_TRUE(session);
  ASSERT_EQ(StallFor(100ms).kind, StepKind::Run) << session->Error() << ": " << session->Describe();
  Action(room_code, fmt::format(R"({{"message":{}}})", PresenceRoster("away")));
  ASSERT_TRUE(Wait([&] { return room->RoomRecovering(RoomClock::now()); }));
  // Frame 20 changes a prediction, but frames 16..19 are still missing: Rollback returns
  // while the original stall is alive, and several replay steps precede its next stall poll.
  Packet partial = NeutralPacket(20, 1);
  partial.pads[0][0] = 1;
  std::atomic<bool> partial_done{false};
  // The two halves together last past MIN_EXCUSED_STALL, as a real outage does.
  std::thread partial_sender([&] {
    std::this_thread::sleep_for(1200ms);
    while (!partial_done)
    {
      direct.Send(EncodePacket(partial));
      std::this_thread::sleep_for(2ms);
    }
  });
  Step step{};
  const auto deadline = RoomClock::now() + 5s;
  for (int boundary = 0; boundary < 30; ++boundary)
  {
    step = Boundary(deadline);
    if (step.kind != StepKind::Run)
      break;
  }
  partial_done = true;
  partial_sender.join();
  ASSERT_EQ(step.kind, StepKind::Rollback) << session->Error();
  EXPECT_TRUE(session->InputStalled());
  EXPECT_EQ(budget.Recoveries(), 0);
  // The room back and the rest of the inputs arrive after the rollback has returned.
  Action(room_code, fmt::format(R"({{"message":{}}})", PresenceRoster("back")));
  Packet remaining = NeutralPacket(16, 16);
  remaining.pads[4][0] = 1;
  std::atomic<bool> remaining_done{false};
  std::thread remaining_sender([&] {
    std::this_thread::sleep_for(1100ms);
    while (!remaining_done)
    {
      direct.Send(EncodePacket(remaining));
      std::this_thread::sleep_for(2ms);
    }
  });
  for (int boundary = 0; boundary < 30 && session->InputStalled(); ++boundary)
    step = Boundary(RoomClock::now() + 5s);
  remaining_done = true;
  remaining_sender.join();
  for (int boundary = 0; boundary < 30 && session->Resimulating(); ++boundary)
    step = Boundary(RoomClock::now() + 5s);
  EXPECT_EQ(step.kind, StepKind::Run) << session->Error();
  EXPECT_FALSE(session->InputStalled());
  EXPECT_EQ(budget.Recoveries(), 1);
  EXPECT_LT(budget.Stalled(RoomClock::now()), 200ms);
  EXPECT_EQ(hold_calls, 0);
}

TEST_P(OrcaRoomStallRuntime, RankedStallsKeepTheBudgetAndSilenceLimit)
{
  OpenDirect("budgetrank", true, "ranked");
  ASSERT_TRUE(session);
  EXPECT_FALSE(room->RoomRecovering(RoomClock::now()));
  ASSERT_EQ(StallFor(9s).kind, StepKind::Run);
  EXPECT_EQ(StallFor(7s).kind, StepKind::Ended);
  EXPECT_EQ(budget.Recoveries(), 0);
  EXPECT_GE(budget.Stalled(RoomClock::now()), 15s);
}

TEST_P(OrcaRoomStallRuntime, CasualStallsKeepTheBudgetAndSilenceLimit)
{
  OpenDirect("budgetcasual", true, "casual");
  ASSERT_TRUE(session);
  EXPECT_FALSE(room->RoomRecovering(RoomClock::now()));
  ASSERT_EQ(StallFor(9s).kind, StepKind::Run);
  EXPECT_EQ(StallFor(7s).kind, StepKind::Ended);
  EXPECT_EQ(budget.Recoveries(), 0);
  EXPECT_GE(budget.Stalled(RoomClock::now()), 15s);
}


TEST(OrcaDoublesAdmission, WindowFillsEarlyAndLateArrivalsWaitForMenus)
{
  using Rollback::OnlineMatch::FriendsBatch;
  const auto now = RoomClock::time_point{};
  FriendsBatch batch;
  EXPECT_FALSE(batch.Ready(true, 1, 0, false, now));
  EXPECT_FALSE(batch.Ready(true, 2, 0, false, now + 1s));
  EXPECT_TRUE(batch.Ready(true, 3, 0, false, now + 1s));
  batch.Clear();
  EXPECT_FALSE(batch.Ready(false, 3, 0, false, now));
  EXPECT_FALSE(batch.Ready(false, 3, 0, false, now + 30s));
  EXPECT_TRUE(batch.Ready(true, 3, 0, false, now + 31s));
  EXPECT_FALSE(batch.Ready(true, 1, 1, false, now + 32s));
  EXPECT_TRUE(batch.Ready(false, 1, 1, true, now + 32s));
  batch.Clear();
  EXPECT_FALSE(batch.Ready(true, 1, 0, false, now));
  EXPECT_FALSE(batch.Ready(true, 1, 0, false, now + 1999ms));
  EXPECT_TRUE(batch.Ready(true, 1, 0, false, now + 2s));
}

TEST_F(OrcaRoomRuntime, DoublesBatchOffersOneCaptureWithEveryPortValue)
{
  FriendsHeadlessCore core;
  for (const char* queue : {"private", "casual"})
  {
    SCOPED_TRACE(queue);
    const std::string code = std::string("btch") + queue;
    auto room = Open(code, fmt::format(
        R"({{"joining":false,"peers":3,"doubles":true,"queue":"{}"}})", queue), false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    ASSERT_TRUE(room->Doubles());
    constexpr int origin = 1524, current = 1600;
    ReplayArchive replay = FriendsReplay(origin, current);
    RoomHashGame game;
    auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                          std::string* error) {
      return room->FreshTicket(fresh, ticket, server, error);
    });
    const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(
        core.system, *room, game, replay, current, {}, true, std::move(store));
    EXPECT_EQ(result.seats, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(result.keyframe_frame, current);
    EXPECT_FALSE(result.fresh_start);
    ASSERT_TRUE(Wait([&] {
      const auto state = Request("/state/" + code);
      std::set<std::string> recipients;
      for (const auto& message : state.get("messages").get<picojson::array>())
        if (message.get("d").is<picojson::object>() && message.get("d").get("k").to_str() == "kf")
          recipients.insert(message.get("to").to_str());
      return recipients.size() == 3;
    }));
    const auto state = Request("/state/" + code);
    std::set<std::string> captures;
    std::set<std::string> recipients;
    std::optional<std::vector<KeyframeInfo::Name>> names;
    for (const auto& message : state.get("messages").get<picojson::array>())
    {
      const auto& data = message.get("d");
      if (!data.is<picojson::object>() || data.get("k").to_str() != "kf")
        continue;
      const auto offer = KeyframeOfferFromJson(data.get<picojson::object>());
      ASSERT_TRUE(offer);
      EXPECT_TRUE(offer->shared);
      EXPECT_EQ(offer->frame, current);
      captures.insert(offer->id);
      recipients.insert(message.get("to").to_str());
      if (!names)
        names = offer->names;
      EXPECT_EQ(offer->names, *names);
      std::set<int> ports;
      for (const auto& name : offer->names)
      {
        ports.insert(name.seat);
        if (name.seat > 0)
          EXPECT_EQ(name.controls, (std::vector<u8>{static_cast<u8>(name.seat)}));
      }
      EXPECT_EQ(ports, (std::set<int>{0, 1, 2, 3}));
    }
    EXPECT_EQ(captures.size(), 1u);
    EXPECT_EQ(recipients.size(), 3u);
  }
}

TEST_F(OrcaRoomRuntime, CasualDoublesUsesAllFourSeats)
{
  auto room = Open("casdbl4", R"({"queue":"casual","doubles":true,"peers":3,"joining":false})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  EXPECT_TRUE(room->Doubles());
  EXPECT_EQ(room->Queue(), "casual");
  EXPECT_EQ(room->PlayQueue(), "private");
  std::set<int> seats;
  ASSERT_TRUE(Wait([&] {
    for (const auto& event : room->TakePeerEvents())
      if (event.kind == PeerEvent::Kind::Arrived)
      {
        EXPECT_TRUE(event.doubles);
        seats.insert(event.seat);
      }
    return seats.size() == 3;
  }));
  EXPECT_EQ(seats, (std::set<int>{1, 2, 3}));
}

TEST_F(OrcaRoomRuntime, BatchEnvelopeDeliversOnlyThisRecipientsPacket)
{
  const std::string code = "batchp444";
  auto room = Open(code, R"({"doubles":true})");
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1 && room->RoundTripMs() >= 0; }));
  room->Receive();
  Packet mine = Frames(0, 100, 4);
  Packet other = Frames(0, 500, 4);
  const std::string envelope = fmt::format(
      R"({{"message":{{"t":"msg","from":"peer","d":{{"k":"bp","p":[[1,{}],[2,{}]]}}}}}})",
      EncodePacket(mine), EncodePacket(other));
  Action(code, envelope);
  std::vector<Packet> got;
  ASSERT_TRUE(Wait([&] { got = room->Receive(); return !got.empty(); }));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].first_frame, 100);
  EXPECT_EQ(got[0].pads, mine.pads);
  // Duplicate recipient entries are rejected as a whole.
  Action(code, fmt::format(
      R"({{"message":{{"t":"msg","from":"peer","d":{{"k":"bp","p":[[1,{}],[1,{}]]}}}}}})",
      EncodePacket(mine), EncodePacket(other)));
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(room->Receive().empty());
}


TEST_F(OrcaRoomRuntime, ThreePeersKeepIndependentAwayBackAndResumeHolds)
{
  const std::string code = "holds444";
  auto room = Open(code, R"({"joining":false,"peers":3,"doubles":true})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  room->TakePeerEvents();
  Action(code, R"({"message":{"t":"away","player":{"id":"peer2"}}})");
  std::this_thread::sleep_for(100ms);
  const auto start = RoomClock::now();
  EXPECT_TRUE(room->HoldInputSilence(start - 15s, start, std::vector<int>{2}));
  EXPECT_TRUE(room->InputHoldFor("peer2"));
  EXPECT_FALSE(room->InputHoldFor("peer"));
  EXPECT_FALSE(room->InputHoldFor("peer3"));
  EXPECT_EQ(room->Code(), code);
  EXPECT_TRUE(room->TakePeerEvents().empty());
  Action(code, R"({"message":{"t":"back","player":{"id":"peer2"}}})");
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(room->TakePeerEvents().empty());
  Packet ack;
  ack.seat = 0;
  ack.ack = {-1, 199, 199, 199};
  room->Send(ack);
  auto returning = Frames(2, 190, 16);
  Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer2","d":{}}}}})",
                           EncodePacket(returning)));
  ASSERT_TRUE(Wait([&] { return !room->Receive().empty(); }));
  EXPECT_FALSE(room->InputHoldFor("peer2"));
  EXPECT_EQ(room->Code(), code);
  EXPECT_EQ(room->GetState(), RoomState::Waiting);
}

TEST_F(OrcaRoomRuntime, ThirdRetainedPeerGetsCurrentMatchCaptureWithTwoOthersSeated)
{
  FriendsHeadlessCore core;
  const std::string code = "resume444";
  auto room = Open(code, R"({"joining":false,"peers":3,"doubles":true})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  room->TakePeerEvents();
  Action(code, R"({"message":{"t":"msg","from":"peer3","d":{"k":"hello","c":"test",
                   "r":true,"h":false,"doubles":true,"resume":true,"resume_id":"fixture-cycle"}}})");
  ASSERT_TRUE(Wait([&] { return room->ArrivalPending(); }));
  ReplayArchive replay = FriendsReplay(1524, 6000, 3);
  RoomHashGame game;
  auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                        std::string* error) {
    return room->FreshTicket(fresh, ticket, server, error);
  });
  const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(
      core.system, *room, game, replay, 6000, {1, 2, 3}, false, std::move(store));
  EXPECT_EQ(result.seats, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(result.keyframe_frame, 6000);
  EXPECT_FALSE(result.fresh_start);
  EXPECT_EQ(room->Code(), code);
  ASSERT_TRUE(Wait([&] { return Sent(code, "msg", "kf"); }));
}


TEST_F(OrcaRoomRuntime, LateFriendsAreHeldByTheActualHostDuringAFight)
{
  FriendsHeadlessCore core;
  const std::string code = "late444";
  auto room = Open(code, R"({"joining":false,"peers":3,"doubles":true})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  ReplayArchive replay = FriendsReplay(1524, 6000, 1);
  RoomHashGame game;
  auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                        std::string* error) {
    return room->FreshTicket(fresh, ticket, server, error);
  });
  const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(
      core.system, *room, game, replay, 6000, {1}, false, std::move(store));
  EXPECT_EQ(result.seats, (std::vector<int>{1}));
  EXPECT_FALSE(result.keyframe_frame);
  EXPECT_FALSE(result.fresh_start);
  ASSERT_TRUE(Wait([&] { return Sent(code, "msg", "hold"); }));
  EXPECT_FALSE(Sent(code, "msg", "kf"));
  EXPECT_EQ(room->Code(), code);
}


TEST_F(OrcaRoomRuntime, SimultaneousRecoveriesShareHostOwnedCapture)
{
  FriendsHeadlessCore core;
  const std::string code = "resmulti44";
  auto room = Open(code, R"({"joining":false,"peers":3,"doubles":true})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  room->TakePeerEvents();
  for (int seat : {2, 3})
    Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer{}","d":{{"k":"hello","c":"test",
                            "r":true,"h":false,"doubles":true,"resume":true,"resume_id":"fixture-group"}}}}}})", seat));
  ASSERT_TRUE(Wait([&] { return room->ArrivalPending(); }));
  std::this_thread::sleep_for(100ms);
  RoomHashGame game;
  auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                        std::string* error) {
    return room->FreshTicket(fresh, ticket, server, error);
  });
  const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(
      core.system, *room, game, FriendsReplay(1524, 6000, 3), 6000, {1, 2, 3}, false, std::move(store));
  EXPECT_EQ(result.seats, (std::vector<int>{1, 2, 3}));
  ASSERT_TRUE(Wait([&] {
    const auto snapshot = Request("/state/" + code);
    std::set<std::string> offered;
    for (const auto& message : snapshot.get("messages").get<picojson::array>())
      if (message.get("d").is<picojson::object>() && message.get("d").get("k").to_str() == "kf")
        offered.insert(message.get("to").to_str());
    return offered.size() == 2;
  }));
  const auto state = Request("/state/" + code);
  std::set<std::string> ids, recipients;
  for (const auto& message : state.get("messages").get<picojson::array>())
  {
    const auto& data = message.get("d");
    if (!data.is<picojson::object>() || data.get("k").to_str() != "kf")
      continue;
    const auto offer = KeyframeOfferFromJson(data.get<picojson::object>());
    ASSERT_TRUE(offer);
    EXPECT_TRUE(offer->shared);
    ids.insert(offer->id);
    recipients.insert(message.get("to").to_str());
  }
  EXPECT_EQ(ids.size(), 1u);
  EXPECT_EQ(recipients, (std::set<std::string>{"peer2", "peer3"}));
}

// Fixed wire fixtures from 3bfe032623 (0.3.44); doubles is absent in every input.
TEST(OrcaLegacy044, PacketAndKeyframeOfferGolden)
{
  EXPECT_EQ(EncodePacket(Sample()),
      R"({"a":[130,-1,-1,-1],"c":121,"f":120,"h":"fedcba9876543210","j":5,"k":"p","l":64,"n":3,"p":"00ff807f0001a00f01ff807f0001a10f02ff807f0001a20f","r":42,"s":1,"sp":[1,2,2,3],"v":-17,"x":60})");
  KeyframeInfo info;
  info.frame = 240;
  info.id = "fixture";
  info.size = 3;
  info.hash = "fixture-hash";
  info.names = {{0, 0, "Host", {1, 2}, {}}, {1, 240, "Friend", {3}, {}}};
  info.names_version = 2;
  info.inline_blob = {1, 2, 3};
  EXPECT_EQ(picojson::value(KeyframeOfferToJson(info)).serialize(),
      R"({"b":"AQID","f":240,"id":"fixture","k":"kf","key":"","n":3,"nm":[[0,0,"Host","0102"],[1,240,"Friend","03"]],"nv":2,"x":"fixture-hash"})");
}

TEST(OrcaLegacy044, WelcomeAndHelloGolden)
{
  ScopedEnv direct("ORCA_DIRECT", "1");
  auto options = TraceOptions();
  options.recovery_enabled = false;
  options.controls = {1, 2};
  const auto trace = TraceRoomForTest(options, {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Tick, 1100},
      {RK::Message, 1101, PeerHello()}, {RK::Tick, 2200}});
  EXPECT_EQ(trace[0].state, RoomState::Waiting);
  EXPECT_TRUE(trace[0].events.empty());
  ASSERT_EQ(trace[2].events.size(), 1u);
  EXPECT_EQ(trace[2].events[0].seat, 0);
  EXPECT_TRUE(trace[2].events[0].host);
  EXPECT_EQ(trace[2].events[0].name, "Friend");
  std::vector<std::string> first_hello, second_hello;
  for (const auto& message : trace[1].sent)
    if (message.find("hello") != std::string::npos) first_hello.push_back(message);
  for (const auto& message : trace[3].sent)
    if (message.find("hello") != std::string::npos) second_hello.push_back(message);
  ASSERT_EQ(first_hello.size(), 1u);
  EXPECT_EQ(first_hello[0], R"({"d":{"c":"test","ct":"0102","dl":1,"h":false,"k":"hello","r":false},"t":"msg","to":"peer"})");
  EXPECT_TRUE(second_hello.empty());
}

INSTANTIATE_TEST_SUITE_P(LegacyAndDoubles, OrcaRoomStallRuntime, testing::Values(0, 2, 4));

TEST_F(OrcaRoomRuntime, Legacy044PeerInDoublesIsRefusedWithRoomFull)
{
  FriendsHeadlessCore core;
  const std::string code = "legacy444";
  auto room = Open(code, R"({"joining":false,"doubles":true,"legacy":true,"size":2})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->ArrivalPending(); }));
  ReplayArchive replay = FriendsReplay(1524, 6000, 0);
  RoomHashGame game;
  const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(
      core.system, *room, game, replay, 6000, {}, true, nullptr);
  EXPECT_TRUE(result.seats.empty());
  ASSERT_TRUE(Wait([&] { return Sent(code, "msg", "drop"); }));
  EXPECT_NE(Request("/state/" + code).serialize().find("room_full"), std::string::npos);
  EXPECT_EQ(room->GetState(), RoomState::Waiting);
}

TEST(OrcaDoublesTransport, SwitchingFanoutKeepsPendingHistoryAndInputs)
{
  for (const bool to_seats_first : {false, true})
  {
    std::optional<Packet> broadcast;
    std::map<int, std::optional<Packet>> addressed;
    Packet old = Frames(0, 100, 8);
    old.history_seat = 1;
    old.history_first = 20;
    old.history.resize(128);
    Packet next = Frames(0, 108, 8);
    if (to_seats_first)
    {
      CoalesceToSeats(&broadcast, &addressed, {{1, old}, {2, next}});
      CoalesceBroadcast(&broadcast, &addressed, next);
    }
    else
    {
      CoalesceBroadcast(&broadcast, &addressed, old);
      CoalesceToSeats(&broadcast, &addressed, {{1, next}, {2, next}});
    }
    ASSERT_TRUE(addressed[1]);
    EXPECT_EQ(addressed[1]->history, old.history);
    EXPECT_EQ(addressed[1]->history_first, 20);
    EXPECT_EQ(addressed[1]->first_frame, 100);
    EXPECT_EQ(addressed[1]->pads.size(), 16u);
  }
}

TEST(OrcaDoublesTransport, BroadcastHistoryReachesANewRecipientDuringTheSwitch)
{
  std::optional<Packet> broadcast;
  std::map<int, std::optional<Packet>> addressed;
  Packet old = Frames(0, 100, 8);
  old.history_seat = 1;
  old.history_first = 20;
  old.history.resize(128);
  CoalesceToSeats(&broadcast, &addressed, {{1, old}});
  Packet next = Frames(0, 108, 8);
  next.history_seat = 2;
  next.history_first = 40;
  next.history.resize(128);
  CoalesceBroadcast(&broadcast, &addressed, next);
  ASSERT_TRUE(addressed[1]);
  EXPECT_EQ(addressed[1]->history_first, 20);
  EXPECT_EQ(addressed[1]->history_seat, 1);
  ASSERT_TRUE(addressed[2]);
  EXPECT_EQ(addressed[2]->history_first, 40);
  EXPECT_EQ(addressed[2]->history_seat, 2);
  EXPECT_EQ(addressed[2]->history, next.history);
}

TEST_F(OrcaRoomRuntime, HostCommandCapturesForPrivateRoomsWithoutQueueReady)
{
  FriendsHeadlessCore core;
  // `host <code>` is also used for tournaments: the ticket flag doesn't change WELCOME's queue.
  // No queue2 pick/ready identity is needed to join the host's current character select.
  const ScopedEnv session("ORCA_SESSION", "1");
  Orca::Status::SetAppCaps("join host queue2");
  Common::ScopeGuard clear_caps([] { Orca::Status::SetAppCaps(""); });
  Orca::UX::QueueApp::End();
  ASSERT_FALSE(Orca::UX::QueueApp::OwnIdentity().HasPick());
  for (bool tournament : {false, true})
  {
    SCOPED_TRACE(tournament);
    // Field trace: origin at 230, character select at 313, first notice at running frame 1178.
    for (int current : {313, 1179})
    {
      const std::string code = fmt::format("host{}{}", tournament ? "t" : "p", current);
      auto room = Open(code, tournament ? R"({"joining":false,"tourney":1})" :
                                         R"({"joining":false})", false, false);
      ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
      ASSERT_EQ(room->Queue(), "private");
      RoomHashGame game;
      auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                            std::string* error) {
        return room->FreshTicket(fresh, ticket, server, error);
      });
      const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(
          core.system, *room, game, FriendsReplay(230, current), current, {}, true,
          std::move(store), true);
      if (current == 313)
      {
        EXPECT_FALSE(result.fresh_start);
        EXPECT_TRUE(result.seated);
        EXPECT_EQ(result.keyframe_frame, current);
      }
      else
      {
        // A long private history follows the existing friends policy, rather than holding forever.
        EXPECT_TRUE(result.fresh_start);
      }
    }
  }
}

TEST_F(OrcaRoomRuntime, HostCommandKeepsCasualAndRankedFreshStarts)
{
  FriendsHeadlessCore core;
  for (const std::string queue : {"casual", "ranked"})
  {
    SCOPED_TRACE(queue);
    auto room = Open("host" + queue,
                     fmt::format(R"({{"joining":false,"queue":"{}"}})", queue), false, false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    RoomHashGame game;
    const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(
        core.system, *room, game, FriendsReplay(230, 313), 313, {}, true, nullptr, true);
    EXPECT_TRUE(result.fresh_start);
    EXPECT_FALSE(result.keyframe_frame);
    EXPECT_FALSE(result.seated);
  }
}

TEST_F(OrcaRoomRuntime, FullRankedDoublesRoomRetriesRefusedBeginWithoutRosterChange)
{
  const std::string code = "begretry394";
  Request("/configure/" + code,
      R"({"joining":false,"queue":"ranked","doubles":true,"peers":3,"teams":true})");
  auto options = TraceOptions(false);
  options.room_code = code;
  options.mode = "orca-rsbe01-2v2-ranked";
  options.friends_room = false;
  auto room = std::make_unique<YouGameRoom>(options);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }))
      << room->Status() << " seat=" << room->Seat() << " rtt=" << room->RoundTripMs();
  room->SetOpponentPlugged(true);
  const auto begins = [&] {
    const auto messages = Request("/state/" + code).get("messages");
    return std::ranges::count_if(messages.get<picojson::array>(), [](const auto& message) {
      return message.get("t").to_str() == "begin";
    });
  };
  ASSERT_TRUE(Wait([&] { return begins() == 1; }));
  const auto refused_at = RoomClock::now();
  Action(code, R"({"message":{"t":"command-error","action":"begin","error":"try again"}})");
  EXPECT_FALSE(Wait([&] { return begins() > 1; }, 9s));
  ASSERT_TRUE(Wait([&] { return begins() == 2; }, 4s));
  EXPECT_GE(RoomClock::now() - refused_at, 10s);
  EXPECT_EQ(Connections(code), 1);
}

TEST_F(OrcaRoomRuntime, PrivateTournamentTicketBeginsAndReportsTheMatch)
{
  const std::string code = "tourney1";
  auto room = Open(code, R"({"joining":false,"tourney":1})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  room->SetOpponentPlugged(true);
  ASSERT_TRUE(Wait([&] { return Sent(code, "begin"); }));
  EXPECT_EQ(room->Queue(), "private");
  GameReport report;
  report.kind = GameReport::Kind::Win;
  report.winner_seat = 0;
  report.detail = "{}";
  room->ReportGame(report);  // Held until the room assigns the match.
  Action(code, R"({"message":{"t":"start","matchId":"t1","participants":[{"id":"me","connectionId":"me","slot":0},{"id":"peer","connectionId":"peer","slot":1}]}})");
  ASSERT_TRUE(Wait([&] { return Sent(code, "finish"); }));
  const auto state = Request("/state/" + code);
  for (const auto& message : state.get("messages").get<picojson::array>())
    if (message.get("t").to_str() == "finish")
    {
      EXPECT_EQ(message.get("matchId").to_str(), "t1");
      EXPECT_EQ(message.get("winner").to_str(), "me");
    }
}

TEST_F(OrcaRoomRuntime, OrdinaryPrivateTicketsKeepReportsLocal)
{
  for (const std::string flag : {"", ",\"tourney\":0", ",\"tourney\":true", ",\"tourney\":\"1\""})
  {
    const std::string code = "private" + std::to_string(flag.size());
    auto room = Open(code, "{\"joining\":false" + flag + "}", false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    room->SetOpponentPlugged(true);
    GameReport report;
    report.kind = GameReport::Kind::Draw;
    report.detail = "{}";
    room->ReportGame(report);
    Action(code, R"({"message":{"t":"start","matchId":"p1"}})");
    ASSERT_TRUE(Wait([&] { return room->MatchLive(); }));
    room->ReportGame(report);
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(Sent(code, "begin"));
    EXPECT_FALSE(Sent(code, "finish"));
    EXPECT_FALSE(Sent(code, "game-report"));
  }
}

TEST(OrcaRoom, ScriptedInputMarksReportsAndNormalPlayPreservesBytes)
{
  for (const auto& [script, commands, harness] :
       {std::tuple{"", "", false}, std::tuple{"", "0", false},
        std::tuple{"fixture-pads.txt", "", true}, std::tuple{"", "1", true}})
  {
    ScopedEnv input("YG_INPUT", script);
    ScopedEnv test_commands("ORCA_TEST_COMMANDS", commands);
    EXPECT_EQ(Orca::HarnessInputActive(), harness);
    for (const char* kind : {"lobby_result", "begin", "set", "game-start", "game-report",
                             "finish", "hello"})
    {
      const std::string normal = fmt::format("{{\"t\":\"{}\",\"detail\":{{}}}}", kind);
      const std::string marked = Orca::HarnessReport(normal);
      if (!harness)
        EXPECT_EQ(marked, normal);
      picojson::value parsed;
      ASSERT_TRUE(picojson::parse(parsed, marked).empty());
      const auto& object = parsed.get<picojson::object>();
      EXPECT_EQ(object.contains("harness"), harness);
      if (harness)
        EXPECT_TRUE(object.at("harness").get<bool>());
    }
    EXPECT_TRUE(Orca::HarnessReport("").empty());
  }
}

TEST(OrcaRoom, HarnessHelloIsOptionalOnTheActualRoomPath)
{
  for (int mode = 0; mode < 3; ++mode)
  {
    ScopedEnv input("YG_INPUT", mode == 1 ? "fixture-pads.txt" : "");
    ScopedEnv commands("ORCA_TEST_COMMANDS", mode == 2 ? "1" : "");
    const auto trace = TraceRoomForTest(TraceOptions(), {
        {RK::Message, 0, RosterMessage("welcome")}, {RK::Tick, 1100}});
    bool saw_hello = false;
    for (const auto& text : trace.back().sent)
    {
      picojson::value parsed;
      ASSERT_TRUE(picojson::parse(parsed, text).empty());
      if (!parsed.contains("d") || !parsed.get("d").is<picojson::object>())
        continue;
      const auto& hello = parsed.get("d");
      if (hello.get("k").to_str() != "hello")
        continue;
      saw_hello = true;
      EXPECT_EQ(hello.contains("harness"), mode != 0);
      if (mode != 0)
        EXPECT_TRUE(hello.get("harness").get<bool>());
    }
    EXPECT_TRUE(saw_hello);
  }
}

TEST_F(OrcaRoomRuntime, HarnessMarkerFollowsActualBeginGameStartReportAndFinish)
{
  for (int mode = 0; mode < 3; ++mode)
  {
    ScopedEnv input("YG_INPUT", mode == 1 ? "fixture-pads.txt" : "");
    ScopedEnv commands("ORCA_TEST_COMMANDS", mode == 2 ? "1" : "");
    const std::string code = "harness" + std::to_string(mode);
    auto room = Open(code, R"({"joining":false,"queue":"ranked"})", false, false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
    room->SetOpponentPlugged(true);
    ASSERT_TRUE(Wait([&] { return Sent(code, "begin"); }));
    Action(code, R"({"message":{"t":"start","matchId":"h1","participants":[{"id":"me","connectionId":"me","slot":0},{"id":"peer","connectionId":"peer","slot":1}]}})");
    room->ReportGameStart(100);
    ASSERT_TRUE(Wait([&] { return Sent(code, "game-start"); }));
    GameReport report;
    report.kind = GameReport::Kind::Win;
    report.winner_seat = 0;
    report.start = 100;
    report.detail = "{}";
    room->ReportGame(report);
    ASSERT_TRUE(Wait([&] { return Sent(code, "game-report"); }));
    room->ReportFault();
    ASSERT_TRUE(Wait([&] { return Sent(code, "finish"); }));
    const auto state = Request("/state/" + code);
    std::set<std::string> seen;
    for (const auto& message : state.get("messages").get<picojson::array>())
    {
      const auto type = message.get("t").to_str();
      if (type != "begin" && type != "game-start" && type != "game-report" && type != "finish")
        continue;
      seen.insert(type);
      EXPECT_EQ(message.contains("harness"), mode != 0) << type;
      if (mode != 0)
        EXPECT_TRUE(message.get("harness").get<bool>()) << type;
    }
    EXPECT_EQ(seen.size(), 4u);
  }
}

TEST_F(OrcaRoomRuntime, SetBlockStillControlsTheEndAfterTwoAgreedWins)
{
  const std::string code = "block0361";
  auto room = Open(code, R"({"joining":false,"queue":"ranked"})", false, false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  Action(code, R"({"message":{"t":"start","matchId":"block-set","participants":[{"id":"me","connectionId":"me","slot":0},{"id":"peer","connectionId":"peer","slot":1}]}})");
  Orca::UX::SetBlock::SetState set;
  for (int n = 1; n <= 3; ++n)
  {
    auto& record = set.records[n - 1];
    record.start = n * 120;
    record.winner = 0;
    const auto result = Orca::UX::FromRecord(set, n - 1);
    ASSERT_TRUE(result.from_set_block);
    GameReport report;
    report.kind = GameReport::Kind::Win;
    report.winner_seat = result.winner_port;
    report.start = result.start;
    report.number = result.number;
    report.from_set_block = result.from_set_block;
    report.detail = result.DetailJson();
    report.set_done = n == 3 ? 1 : 0;
    report.set_winner_seat = n == 3 ? 0 : -1;
    room->ReportGame(report);
    ASSERT_TRUE(Wait([&] {
      const auto state = Request("/state/" + code);
      for (const auto& message : state.get("messages").get<picojson::array>())
        if (message.get("t").to_str() == "game-report" &&
            message.get("id").to_str() == "g" + std::to_string(n * 120))
          return true;
      return false;
    }));
    Action(code, fmt::format(R"({{"message":{{"t":"game-result","matchId":"block-set","id":"g{}","ranking":["me","peer"]}}}})", n * 120));
    ASSERT_TRUE(Wait([&] { return room->KnownSetGames() >= std::min(n, 2); }));
    if (n < 3)
      EXPECT_FALSE(Sent(code, "finish"));
  }
  ASSERT_TRUE(Wait([&] { return Sent(code, "finish"); }));
  const auto state = Request("/state/" + code);
  int reports = 0, finishes = 0;
  for (const auto& message : state.get("messages").get<picojson::array>())
  {
    if (message.get("t").to_str() == "game-report")
      ++reports;
    if (message.get("t").to_str() == "finish")
    {
      ++finishes;
      EXPECT_EQ(reports, 3);
      EXPECT_EQ(message.serialize(), R"({"matchId":"block-set","t":"finish","winner":"me"})");
    }
  }
  EXPECT_EQ(finishes, 1);
}

TEST_F(OrcaRoomRuntime, SceneGameOneStartStillPrecedesTheForfeitingLeave)
{
  const std::string code = "scene0361";
  auto room = Open(code, R"({"joining":false,"queue":"ranked"})", false, false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->RoundTripMs() >= 0; }));
  Action(code, R"({"message":{"t":"start","matchId":"scene-leave","participants":[{"id":"me","connectionId":"me","slot":0},{"id":"peer","connectionId":"peer","slot":1}]}})");
  Orca::UX::ResultsTracker tracker;
  Orca::UX::Reading reading;
  reading.scene = Orca::UX::Reading::Scene::Other;
  tracker.Store(119, reading, 0);
  reading.scene = Orca::UX::Reading::Scene::Fight;
  tracker.Store(120, reading, 0);
  std::vector<int> starts;
  EXPECT_TRUE(tracker.Confirm(120, -1, &starts).empty());
  ASSERT_EQ(starts, std::vector<int>{120});
  room->ReportGameStart(starts[0]);
  ASSERT_TRUE(Wait([&] { return Sent(code, "game-start"); }));
  room->Leave();
  const auto state = Request("/state/" + code);
  int began = 0, left = 0;
  for (const auto& message : state.get("messages").get<picojson::array>())
  {
    const auto type = message.get("t").to_str();
    if (type == "game-start")
    {
      ++began;
      EXPECT_EQ(message.serialize(), R"({"id":"g120","matchId":"scene-leave","t":"game-start"})");
    }
    if (type == "leave")
    {
      ++left;
      EXPECT_EQ(began, 1);
    }
    EXPECT_NE(type, "game-report");
    EXPECT_NE(type, "finish");
  }
  EXPECT_EQ(began, 1);
  EXPECT_EQ(left, 1);
}

TEST(OrcaRoom, CoSeatPacketRoundTrips)
{
  Packet sent = Sample();
  sent.co = {{3, Frames(3, 10, 3).pads}, {0, Frames(0, 20, 3).pads},
             {2, std::vector<Pad>(3, UNPLUGGED_PAD)}};
  sent.roster = std::array<SeatPlan, MAX_SEATS>{};
  sent.groups = std::array<int, MAX_SEATS>{1, 1, 1, 1};
  Packet got;
  ASSERT_TRUE(DecodePacket(EncodePacket(sent), &got));
  ExpectSame(sent, got);
  EXPECT_EQ(got.co, sent.co);
  EXPECT_EQ(got.groups, sent.groups);
  EXPECT_EQ(got.roster, sent.roster);
  sent.groups = std::array<int, MAX_SEATS>{0, 1, -1, 3};
  EXPECT_EQ(EncodePacket(sent).find("\"rg\""), std::string::npos);
}

TEST(OrcaRoom, DirectTrimKeepsEveryCoSeatAligned)
{
  for (const int count : {1, 3})
  {
    Packet sent = Frames(0, 100, MAX_PADS_IN_PACKET);
    for (int seat = 1; seat <= count; ++seat)
      sent.co.emplace_back(seat, Frames(seat, 100 + seat, MAX_PADS_IN_PACKET).pads);
    ASSERT_GT(EncodePacket(sent).size(), DirectLink::MAX_PAYLOAD);
    const std::string payload = EncodeDirectPacket(sent);
    EXPECT_LE(payload.size(), DirectLink::MAX_PAYLOAD);
    Packet got;
    ASSERT_TRUE(DecodePacket(payload, &got));
    EXPECT_EQ(got.first_frame, sent.first_frame);
    ASSERT_GT(got.pads.size(), 1u);
    ASSERT_LT(got.pads.size(), sent.pads.size());
    EXPECT_EQ(got.pads, (std::vector<Pad>(sent.pads.begin(),
                                        sent.pads.begin() + got.pads.size())));
    ASSERT_EQ(got.co.size(), sent.co.size());
    for (size_t i = 0; i < got.co.size(); ++i)
    {
      EXPECT_EQ(got.co[i].first, sent.co[i].first);
      EXPECT_EQ(got.co[i].second, (std::vector<Pad>(sent.co[i].second.begin(),
          sent.co[i].second.begin() + got.pads.size())));
    }
  }
}

TEST(OrcaRoom, MalformedCoSeatPacketsAreRefused)
{
  const std::string pad = "0000000000000000";
  for (const std::string& co : std::vector<std::string>{
      "null", "{}", "[1]", "[[2]]", "[[2,0]]", "[[1,\"" + pad + "\"]]",
      "[[-1,\"" + pad + "\"]]", "[[4,\"" + pad + "\"]]",
      "[[2.5,\"" + pad + "\"]]", "[[2,\"0000\"]]",
      "[[2,\"000000000000000z\"]]", "[[2,\"\"]]",
      "[[2,\"" + pad + pad + "\"]]",
      "[[2,\"" + pad + "\"],[2,\"" + pad + "\"]]",
      "[[0,\"" + pad + "\"],[2,\"" + pad + "\"],[3,\"" + pad +
          "\"],[0,\"" + pad + "\"]]"})
  {
    Packet got = Sample();
    const auto before = EncodePacket(got);
    EXPECT_FALSE(DecodePacket(fmt::format(
        R"({{"k":"p","s":1,"f":0,"c":1,"p":"{}","a":[-1,-1,-1,-1],"co":{}}})",
        pad, co), &got)) << co;
    EXPECT_EQ(EncodePacket(got), before);
  }
  Packet empty;
  empty.seat = 1;
  empty.co = {{2, {}}};
  Packet got;
  ASSERT_TRUE(DecodePacket(EncodePacket(empty), &got));
  EXPECT_EQ(got.co, empty.co);
}

TEST(OrcaRoom, RosterGroupsAreBounded)
{
  for (const char* groups : {"null", "{}", "[0,1,2]", "[0,1,2,3,0]",
                                   "[0,1,2,4]", "[0,1,2,-2]", "[0,1,2,1.5]",
                                   "[0,1,2,\"1\"]"})
  {
    Packet got;
    EXPECT_FALSE(DecodePacket(fmt::format(
        R"({{"k":"p","s":0,"f":0,"c":0,"p":"","a":[-1,-1,-1,-1],"ro":[[-1,-1,-1],[-1,-1,-1],[-1,-1,-1],[-1,-1,-1]],"rg":{}}})",
        groups), &got)) << groups;
  }
  Packet got;
  EXPECT_FALSE(DecodePacket(
      R"({"k":"p","s":0,"f":0,"c":0,"p":"","a":[-1,-1,-1,-1],"rg":[0,0,-1,-1]})", &got));
}

namespace
{
Packet CoFrames(int first, int count)
{
  Packet packet = Frames(0, first, count);
  packet.co = {{2, Frames(2, first + 20, count).pads},
               {3, Frames(3, first + 40, count).pads}};
  return packet;
}
}  // namespace

TEST(OrcaRoom, CoalesceKeepsEverySeatsEarlierFrames)
{
  for (const auto& [first, count] : {std::pair{104, 4}, {108, 4}, {106, 60}})
  {
    const Packet old = CoFrames(100, 8);
    const Packet next = CoFrames(first, count);
    std::optional<Packet> pending = old;
    CoalescePacket(&pending, next);
    ASSERT_TRUE(pending);
    const int from = std::max(100, first + count - MAX_PADS_IN_PACKET);
    const auto expected = CoFrames(from, first + count - from);
    EXPECT_EQ(pending->first_frame, expected.first_frame);
    EXPECT_EQ(pending->pads, expected.pads);
    EXPECT_EQ(pending->co, expected.co);
    Packet got;
    ASSERT_TRUE(DecodePacket(EncodePacket(*pending), &got));
    EXPECT_EQ(got.co, expected.co);
  }
}

TEST(OrcaRoom, CoalesceSeatChangesStayAligned)
{
  Packet old = CoFrames(100, 8);
  old.co.erase(old.co.begin() + 1);
  Packet next = CoFrames(104, 4);
  next.co.erase(next.co.begin());
  std::optional<Packet> pending = old;
  CoalescePacket(&pending, next);
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->first_frame, 100);
  std::map<int, std::vector<Pad>> expected;
  expected[2] = Frames(2, 120, 4).pads;
  expected[2].insert(expected[2].end(), 4, UNPLUGGED_PAD);
  expected[3] = std::vector<Pad>(4, UNPLUGGED_PAD);
  const auto tail = Frames(3, 144, 4).pads;
  expected[3].insert(expected[3].end(), tail.begin(), tail.end());
  const std::map<int, std::vector<Pad>> got(pending->co.begin(), pending->co.end());
  EXPECT_EQ(got, expected);
  Packet decoded;
  EXPECT_TRUE(DecodePacket(EncodePacket(*pending), &decoded));
}

TEST(OrcaRoom, CoalesceBroadcastAndAddressedKeepEverySeat)
{
  for (bool addressed_first : {false, true})
  {
    std::optional<Packet> broadcast;
    std::map<int, std::optional<Packet>> addressed;
    const auto old = CoFrames(100, 8), next = CoFrames(104, 8);
    if (addressed_first)
    {
      CoalesceToSeats(&broadcast, &addressed, {{1, old}, {2, old}});
      CoalesceBroadcast(&broadcast, &addressed, next);
    }
    else
    {
      CoalesceBroadcast(&broadcast, &addressed, old);
      CoalesceToSeats(&broadcast, &addressed, {{1, next}, {2, next}});
    }
    for (int seat : {1, 2})
    {
      ASSERT_TRUE(addressed.at(seat));
      EXPECT_EQ(addressed.at(seat)->first_frame, 100);
      EXPECT_EQ(addressed.at(seat)->pads, CoFrames(100, 12).pads);
      EXPECT_EQ(addressed.at(seat)->co, CoFrames(100, 12).co);
    }
  }
}

namespace
{
std::string CoRoster(const char* type, bool joining, bool mine, bool theirs,
                     bool include_peer = true, bool reverse = false)
{
  picojson::value value;
  picojson::parse(value, RosterMessage(type, joining, include_peer));
  auto& participants = value.get<picojson::object>()["participants"].get<picojson::array>();
  if (mine)
    participants.emplace_back(picojson::object{{"id", picojson::value("local:me:1")},
        {"connectionId", picojson::value("me")},
        {"slot", picojson::value(2.0)}, {"localIndex", picojson::value(1.0)},
        {"name", picojson::value("Me 2")}});
  if (theirs && include_peer)
    participants.emplace_back(picojson::object{{"connectionId", picojson::value("peer")},
        {"slot", picojson::value(3.0)}, {"localIndex", picojson::value(1.0)},
        {"name", picojson::value("Friend 2")}});
  if (reverse)
    std::reverse(participants.begin(), participants.end());
  return value.serialize();
}
}  // namespace

namespace
{
// These transport cases start with an established group. Create it through the real page
// command before their first observation, so a first-welcome cleanup is a separate case.
std::vector<RoomTestSnapshot> TraceEstablishedCoRoomForTest(
    const RoomOptions& options, std::vector<RoomTestStep> steps)
{
  picojson::value welcome;
  if (steps.empty() || !picojson::parse(welcome, steps.front().text).empty() ||
      !welcome.get("participants").is<picojson::array>())
    return TraceRoomForTest(options, steps);
  auto& participants = welcome.get<picojson::object>()["participants"].get<picojson::array>();
  const auto removed = std::erase_if(participants, [&](const picojson::value& p) {
    return p.get("connectionId") == welcome.get("me") && p.get("localIndex").is<double>() &&
           p.get("localIndex").get<double>() > 0;
  });
  if (!removed)
    return TraceRoomForTest(options, steps);
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  const bool had_cap = Orca::Status::Cap("coseats");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([&] { Orca::Status::SetAppCaps(had_cap ? "coseats" : ""); });
  welcome.get<picojson::object>()["room"] = picojson::value(picojson::object{
      {"queue", picojson::value("private")}, {"doubles", picojson::value(true)}});
  picojson::value established;
  picojson::parse(established, steps.front().text);
  established.get<picojson::object>()["t"] = picojson::value("lobby");
  const int at = steps.front().milliseconds;
  steps.front().text = established.serialize();
  steps.insert(steps.begin(), {RK::PageEvent, at,
      R"({"slug":"co-page","cmd":"local-add"})"});
  steps.insert(steps.begin(), {RK::Message, at, welcome.serialize()});
  auto trace = TraceRoomForTest(options, steps);
  trace.erase(trace.begin(), trace.begin() + 2);
  return trace;
}
}  // namespace

TEST(OrcaRoom, CoRosterKeepsLeadSeatsAndLinks)
{
  const ScopedEnv direct("ORCA_DIRECT", "1");
  for (bool joining : {false, true})
  for (bool reverse : {false, true})
  {
    const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(joining), {
        {RK::Message, 0, RosterMessage("welcome", joining)},
        {RK::Message, 0, PeerHello(joining)},
        {RK::LinkPeer, 0, "peer"},
        {RK::Message, 1, CoRoster("lobby", joining, true, true, true, reverse)},
        {RK::Message, 2, PeerPacket(joining ? 0 : 1)}});
    EXPECT_EQ(trace[3].state, RoomState::Waiting);
    EXPECT_TRUE(trace[3].error_code.empty());
    EXPECT_EQ(trace[3].seat, joining ? 1 : 0);
    EXPECT_EQ(trace[3].co_seats, (std::vector<int>{2}));
    EXPECT_EQ(trace[3].local_indices, (std::array<int, MAX_SEATS>{0, 0, 1, 1}));
    EXPECT_EQ(trace[3].groups[2], (std::vector<int>{joining ? 1 : 0, 2}));
    EXPECT_EQ(trace[3].groups[3], (std::vector<int>{joining ? 0 : 1, 3}));
    EXPECT_EQ(trace[3].linked_seats, (std::vector<int>{joining ? 0 : 1}));
    ASSERT_EQ(trace[3].events.size(), 1u);
    EXPECT_EQ(trace[3].events[0].kind, PeerEvent::Kind::CoSeatArrived);
    EXPECT_EQ(trace[3].events[0].lead, joining ? 0 : 1);
    EXPECT_EQ(trace[3].events[0].seat, 3);
    EXPECT_EQ(trace[4].packets.size(), 1u);
  }
}

TEST(OrcaRoom, CoRosterAccessorsUseLocalOrdinalOrder)
{
  picojson::value welcome;
  ASSERT_TRUE(picojson::parse(welcome, CoRoster("welcome", false, true, false, false)).empty());
  auto& participants = welcome.get<picojson::object>()["participants"].get<picojson::array>();
  participants[1].get<picojson::object>()["localIndex"] = picojson::value(2.0);
  participants.emplace_back(picojson::object{{"connectionId", picojson::value("me")},
      {"slot", picojson::value(3.0)}, {"localIndex", picojson::value(1.0)}});
  std::reverse(participants.begin(), participants.end());
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {{RK::Message, 0, welcome.serialize()}});
  EXPECT_EQ(trace[0].seat, 0);
  EXPECT_EQ(trace[0].co_seats, (std::vector<int>{3, 2}));
  EXPECT_EQ(trace[0].local_indices, (std::array<int, MAX_SEATS>{0, -1, 2, 1}));
  EXPECT_EQ(trace[0].groups[2], (std::vector<int>{0, 3, 2}));
  EXPECT_TRUE(trace[0].groups[1].empty());
}

TEST(OrcaRoom, CoRosterArrivesAndLeavesAsAGroup)
{
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoRoster("welcome", false, true, true)},
      {RK::Message, 0, PeerHello(false)},
      {RK::Message, 1, CoRoster("leave", false, true, false, false)}});
  ASSERT_EQ(trace[1].events.size(), 1u);
  EXPECT_EQ(trace[1].events[0].seat, 1);
  EXPECT_EQ(trace[1].events[0].co_seats, (std::vector<int>{3}));
  ASSERT_EQ(trace[2].events.size(), 2u);
  for (int i = 0; i < 2; ++i)
  {
    EXPECT_EQ(trace[2].events[i].kind, PeerEvent::Kind::Left);
    EXPECT_EQ(trace[2].events[i].seat, i == 0 ? 1 : 3);
  }
  EXPECT_EQ(trace[2].local_indices[3], -1);
  EXPECT_TRUE(trace[2].groups[3].empty());
}

TEST(OrcaRoom, CoRosterPartialLeaveKeepsTheLead)
{
  const ScopedEnv direct("ORCA_DIRECT", "1");
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoRoster("welcome", false, true, true)},
      {RK::Message, 0, PeerHello(false)},
      {RK::LinkPeer, 0, "peer"},
      {RK::Message, 1, CoRoster("lobby", false, true, false)},
      {RK::Message, 2, PeerPacket(1)}});
  ASSERT_EQ(trace[3].events.size(), 1u);
  EXPECT_EQ(trace[3].events[0].kind, PeerEvent::Kind::CoSeatLeft);
  EXPECT_EQ(trace[3].events[0].seat, 3);
  EXPECT_EQ(trace[3].linked_seats, (std::vector<int>{1}));
  EXPECT_EQ(trace[4].packets.size(), 1u);
}

TEST(OrcaRoom, CoRosterResetAndAwayTimeoutLeaveEverySeat)
{
  for (bool timeout : {false, true})
  {
    std::vector<RoomTestStep> steps{
        {RK::Message, 0, CoRoster("welcome", false, true, true)},
        {RK::Message, 0, PeerHello(false)}};
    if (timeout)
    {
      steps.push_back({RK::Message, 1000, CoRoster("away", false, true, true)});
      const int expiry = 1000 + static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(ROOM_HOLD).count());
      steps.push_back({RK::Tick, expiry - 1});
      steps.push_back({RK::Tick, expiry});
    }
    else
    {
      steps.push_back({RK::Message, 1000, PeerHello(false, true)});
    }
    const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), steps);
    const auto& events = trace.back().events;
    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events[0].kind, PeerEvent::Kind::Left);
    EXPECT_EQ(events[0].seat, 1);
    EXPECT_EQ(events[1].kind, PeerEvent::Kind::Left);
    EXPECT_EQ(events[1].seat, 3);
    if (!timeout)
    {
      ASSERT_EQ(events.size(), 3u);
      EXPECT_EQ(events[2].kind, PeerEvent::Kind::Arrived);
      EXPECT_EQ(events[2].co_seats, (std::vector<int>{3}));
    }
  }
}

TEST(OrcaRoom, CoPacketsFollowTheCurrentRosterOnRelayAndDirect)
{
  for (bool direct : {false, true})
  {
    Packet valid = Frames(1, 20, 3);
    valid.co = {{3, Frames(3, 40, 3).pads}};
    Packet wrong = valid;
    wrong.co[0].first = 2;
    const auto step = [direct](const Packet& packet, int time) {
      return RoomTestStep{direct ? RK::DirectPacket : RK::Message, time,
          direct ? EncodePacket(packet) : fmt::format(
              R"({{"t":"msg","from":"peer","d":{}}})", EncodePacket(packet)), 1};
    };
    const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {
        {RK::Message, 0, CoRoster("welcome", false, true, true)},
        {RK::Message, 0, PeerHello(false)}, step(valid, 1), step(wrong, 2),
        {RK::Message, 3, CoRoster("lobby", false, true, false)}, step(valid, 4)});
    ASSERT_EQ(trace[2].packets.size(), 1u);
    EXPECT_EQ(trace[2].packets[0].co, valid.co);
    EXPECT_TRUE(trace[3].packets.empty());
    ASSERT_EQ(trace[5].packets.size(), 1u);
    EXPECT_EQ(trace[5].packets[0].co, valid.co);
  }
}
TEST_F(OrcaRoomRuntime, FreshTicketReusesTheOriginalLobby)
{
  for (const char* queue : {"private", "casual", "ranked"})
  {
    const std::string code = std::string("colob") + queue;
    const std::string lobby = R"({"version":1,"minPlayers":2,"maxLocalPlayers":1,"compatibility":"original-lobby"})";
    auto room = Open(code, fmt::format(R"({{"queue":"{}","lobby":{}}})", queue, lobby));
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
    // Later metadata must not replace the room's original lobby.
    picojson::value welcome;
    ASSERT_TRUE(picojson::parse(welcome, RosterMessage("welcome")).empty());
    auto& message = welcome.get<picojson::object>();
    message["room"] = picojson::value(picojson::object{{"queue", picojson::value(queue)},
        {"lobby", picojson::value(picojson::object{{"maxLocalPlayers", picojson::value(2.0)}})}});
    message["participants"].get<picojson::array>()[0].get<picojson::object>()["name"] =
        picojson::value("Again");
    Action(code, picojson::value(picojson::object{{"message", welcome}}).serialize());
    ASSERT_TRUE(Wait([&] { return room->SeatName(1) == "Again"; }));
    std::string ticket, store, error;
    ASSERT_TRUE(room->FreshTicket(true, &ticket, &store, &error)) << error;
    const auto lobbies = Request("/state/" + code).get("ticket_lobbies");
    ASSERT_TRUE(lobbies.is<picojson::array>());
    ASSERT_EQ(lobbies.get<picojson::array>().size(), 2u);
    EXPECT_EQ(lobbies.get<picojson::array>()[0].get("maxLocalPlayers").get<double>(), 2);
    picojson::value expected;
    ASSERT_TRUE(picojson::parse(expected, lobby).empty());
    EXPECT_EQ(lobbies.get<picojson::array>()[1], expected);
  }
}

TEST_F(OrcaRoomRuntime, FreshTicketKeepsTicketLobbyWithoutWelcomeLobby)
{
  const std::string code = "coticket01";
  const std::string lobby = R"({"version":1,"minPlayers":2,"maxLocalPlayers":2,"compatibility":"ticket-lobby"})";
  auto room = Open(code, fmt::format(R"({{"ticket_lobby":{}}})", lobby));
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
  std::string ticket, store, error;
  ASSERT_TRUE(room->FreshTicket(true, &ticket, &store, &error)) << error;
  const auto lobbies = Request("/state/" + code).get("ticket_lobbies");
  ASSERT_TRUE(lobbies.is<picojson::array>());
  ASSERT_EQ(lobbies.get<picojson::array>().size(), 2u);
  EXPECT_EQ(lobbies.get<picojson::array>()[0].get("maxLocalPlayers").get<double>(), 2);
  picojson::value expected;
  ASSERT_TRUE(picojson::parse(expected, lobby).empty());
  EXPECT_EQ(lobbies.get<picojson::array>()[1], expected);
}

TEST(OrcaRoom, CoHostLeaveIsReportedOnce)
{
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoRoster("welcome", true, true, true)},
      {RK::Message, 0, PeerHello(true)},
      {RK::Message, 1, CoRoster("leave", true, true, false, false)}});
  ASSERT_EQ(trace.back().events.size(), 2u);
  EXPECT_EQ(trace.back().events[0].seat, 0);
  EXPECT_TRUE(trace.back().events[0].host);
  EXPECT_EQ(trace.back().events[1].seat, 3);
  EXPECT_FALSE(trace.back().events[1].host);
}

TEST(OrcaRoom, MissingCoSeatHoldsItsHostGroup)
{
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoRoster("welcome", true, true, true)},
      {RK::Message, 0, PeerHello(true)},
      {RK::InputSilence, 1000, "3", 1000}});
  ASSERT_TRUE(trace.back().input_held);
  EXPECT_TRUE(*trace.back().input_held);
  ASSERT_TRUE(trace.back().host_input_deadline);
  EXPECT_EQ(*trace.back().host_input_deadline, RoomClock::time_point{} + 1000ms + ROOM_HOLD);
}

TEST(OrcaRoom, RetryCoSeatResetsItsWholeGroupOnce)
{
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoRoster("welcome", true, true, true)},
      {RK::Message, 0, PeerHello(true)},
      {RK::RetryPeer, 1000, "", 0}, {RK::RetryPeer, 1000, "", 3}, {RK::Tick, 1001}});
  const auto& events = trace.back().events;
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, PeerEvent::Kind::Left);
  EXPECT_EQ(events[0].seat, 0);
  EXPECT_TRUE(events[0].host);
  EXPECT_EQ(events[1].kind, PeerEvent::Kind::Left);
  EXPECT_EQ(events[1].seat, 3);
  EXPECT_FALSE(events[1].host);
}

TEST(OrcaRoom, HostGroupsFollowCurrentRoomOwnershipOnRelayAndDirect)
{
  for (const bool direct : {false, true})
  {
    Packet valid = Frames(0, 20, 3);
    valid.co = {{3, Frames(3, 40, 3).pads}};
    std::array<SeatPlan, MAX_SEATS> roster{};
    roster.fill(SeatPlan{0, NEVER, 0});
    valid.roster = roster;
    valid.groups = std::array<int, MAX_SEATS>{0, 1, 1, 0};
    const auto step = [direct](const Packet& packet, int time) {
      return RoomTestStep{direct ? RK::DirectPacket : RK::Message, time,
          direct ? EncodePacket(packet) : fmt::format(
              R"({{"t":"msg","from":"peer","d":{}}})", EncodePacket(packet)), 0};
    };
    Packet wrong_co = valid;
    (*wrong_co.groups)[3] = 1;
    Packet wrong_local = valid;
    (*wrong_local.groups)[2] = 0;
    Packet scheduled = valid;
    (*scheduled.roster)[3].unplug_from = 60;
    const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(true), {
        {RK::Message, 0, CoRoster("welcome", true, true, true)},
        {RK::Message, 0, PeerHello(true)}, step(valid, 1), step(wrong_co, 2), step(wrong_local, 3),
        {RK::Message, 4, CoRoster("lobby", true, true, false)}, step(scheduled, 5)});
    EXPECT_EQ(trace[2].packets.size(), 1u);
    EXPECT_TRUE(trace[3].packets.empty());
    EXPECT_TRUE(trace[4].packets.empty());
    // The host's scheduled unplug can outlive the participant's presence in the room.
    EXPECT_EQ(trace[6].packets.size(), 1u);
  }
}

TEST(OrcaRoom, OnlyTheRoomHostSuppliesGroups)
{
  Packet packet = Frames(1, 20, 3);
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster.fill(SeatPlan{0, NEVER, 0});
  packet.roster = roster;
  packet.groups = std::array<int, MAX_SEATS>{0, 1, 0, 1};
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoRoster("welcome", false, true, true)},
      {RK::Message, 0, PeerHello(false)},
      {RK::Message, 1, fmt::format(R"({{"t":"msg","from":"peer","d":{}}})", EncodePacket(packet))}});
  EXPECT_TRUE(trace.back().packets.empty());
}

TEST(OrcaRoom, VacantCoSeatInputsStopWhenTheSlotIsTakenAgain)
{
  for (const bool direct : {false, true})
  {
    Packet packet = Frames(1, 20, 3);
    packet.co = {{3, Frames(3, 40, 3).pads}};
    const auto step = [direct](const Packet& value, int time) {
      return RoomTestStep{direct ? RK::DirectPacket : RK::Message, time,
          direct ? EncodePacket(value) : fmt::format(
              R"({{"t":"msg","from":"peer","d":{}}})", EncodePacket(value)), 1};
    };
    picojson::value replacement;
    ASSERT_TRUE(picojson::parse(replacement, CoRoster("lobby", false, true, false)).empty());
    replacement.get<picojson::object>()["participants"].get<picojson::array>().emplace_back(
        picojson::object{{"connectionId", picojson::value("replacement")},
                        {"slot", picojson::value(3.0)}, {"localIndex", picojson::value(0.0)}});
    Packet lead_only = packet;
    lead_only.co.clear();
    const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {
        {RK::Message, 0, CoRoster("welcome", false, true, false)},
        {RK::Message, 0, PeerHello(false)}, step(packet, 1),
        {RK::Message, 2, CoRoster("lobby", false, true, true)}, step(packet, 3),
        {RK::Message, 4, CoRoster("lobby", false, true, false)}, step(packet, 5),
        {RK::Message, 6, replacement.serialize()}, step(packet, 7), step(packet, 8),
        step(lead_only, 9),
        {RK::Message, 10, CoRoster("lobby", false, true, false)}, step(packet, 11)});
    EXPECT_TRUE(trace[2].packets.empty());
    EXPECT_EQ(trace[4].packets.size(), 1u);
    EXPECT_EQ(trace[6].packets.size(), 1u);
    for (const int index : {8, 9, 12})
    {
      ASSERT_EQ(trace[index].packets.size(), 1u);
      EXPECT_EQ(trace[index].packets[0].pads, packet.pads);
      EXPECT_TRUE(trace[index].packets[0].co.empty());
    }
    EXPECT_EQ(trace[10].packets.size(), 1u);
  }
}

TEST(OrcaRoom, OwnCoSeatRemovalReachesTheHostEventPath)
{
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoRoster("welcome", false, true, true)},
      {RK::Message, 1, CoRoster("lobby", false, false, true)}});
  ASSERT_EQ(trace.back().events.size(), 1u);
  EXPECT_EQ(trace.back().events[0].kind, PeerEvent::Kind::CoSeatLeft);
  EXPECT_EQ(trace.back().events[0].seat, 2);
  EXPECT_EQ(trace.back().events[0].lead, 0);
}

TEST_F(OrcaRoomRuntime, CoSeatArrivalUsesOneGroupAndOneKeyframeOffer)
{
  FriendsHeadlessCore core;
  for (const bool recovering : {false, true})
  {
    const std::string code = recovering ? "corecover" : "cogroup";
    auto room = Open(code,
        R"({"joining":false,"doubles":true,"co_seats":{"me":[2],"peer":[3]}})", false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->ArrivalPending(); }));
    if (recovering)
    {
      room->TakePeerEvents();
      Action(code, R"({"message":{"t":"msg","from":"peer","d":{"k":"hello","c":"test",
          "r":true,"h":false,"doubles":true,"ct":"01","resume":true,"resume_id":"co-cycle"}}})");
      ASSERT_TRUE(Wait([&] { return room->ArrivalPending(); }));
    }
    RoomHashGame game;
    auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                          std::string* error) {
      return room->FreshTicket(fresh, ticket, server, error);
    });
    const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(core.system, *room, game,
        FriendsReplay(1524, 1600), 1600, {}, true, std::move(store));
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.seats, (std::vector<int>{1, 3}));
    EXPECT_EQ(result.groups, (std::array<int, MAX_SEATS>{0, 1, 0, 1}));
    EXPECT_EQ(result.plan[2], (SeatPlan{0, NEVER, 0}));
    EXPECT_EQ(result.plan[1].live_from, 1600);
    EXPECT_EQ(result.plan[3].live_from, 1600);
    EXPECT_EQ(result.plan[1].plug_from, result.plan[3].plug_from);
    ASSERT_TRUE(Wait([&] { return Sent(code, "msg", "kf"); }));
    int offers = 0;
    const auto state = Request("/state/" + code);
    for (const auto& message : state.get("messages").get<picojson::array>())
      if (message.get("d").is<picojson::object>() && message.get("d").get("k").to_str() == "kf")
      {
        ++offers;
        EXPECT_EQ(message.get("to").to_str(), "peer");
      }
    EXPECT_EQ(offers, 1);
  }
}


TEST_F(OrcaRoomRuntime, HostSchedulesOwnCoSeatLeaveAhead)
{
  FriendsHeadlessCore core;
  const std::string code = "coownleave";
  auto room = Open(code,
      R"({"joining":false,"doubles":true,"co_seats":{"me":[2],"peer":[3]}})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->ArrivalPending(); }));
  room->TakePeerEvents();
  RoomHashGame game;
  const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(core.system, *room, game,
      FriendsReplay(1524, 1600, 3), 1600, {1, 3}, true, nullptr, false,
      [&](const std::function<void()>& pump) {
        Action(code, fmt::format(R"({{"message":{}}})", CoRoster("lobby", false, false, true)));
        ASSERT_TRUE(Wait([&] { return room->CoSeats().empty(); }));
        pump();
      });
  ASSERT_TRUE(result.error.empty()) << result.error;
  EXPECT_EQ(result.plan[2].unplug_from, 1600 + 7 + 2 * 6 + 4);
  EXPECT_EQ(result.plan[0].unplug_from, NEVER);
  EXPECT_EQ(result.groups[2], 0);
}

TEST_F(OrcaRoomRuntime, HostRetiresWaitingCoSeatAndKeepsItsLead)
{
  FriendsHeadlessCore core;
  const std::string code = "cowaitleave";
  auto room = Open(code,
      R"({"joining":false,"doubles":true,"co_seats":{"me":[2],"peer":[3]}})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->ArrivalPending(); }));
  RoomHashGame game;
  auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                        std::string* error) {
    return room->FreshTicket(fresh, ticket, server, error);
  });
  const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(core.system, *room, game,
      FriendsReplay(1524, 1600), 1600, {}, true, std::move(store), false,
      [&](const std::function<void()>& pump) {
        Action(code, fmt::format(R"({{"message":{}}})", CoRoster("lobby", false, true, false)));
        ASSERT_TRUE(Wait([&] { return room->GroupOf(3).empty(); }));
        pump();
      });
  ASSERT_TRUE(result.error.empty()) << result.error;
  EXPECT_EQ(result.plan[3].live_from, NEVER);
  EXPECT_EQ(result.plan[1].live_from, 1600);
  EXPECT_EQ(result.plan[1].unplug_from, NEVER);
  EXPECT_EQ(result.groups[1], 1);
}

TEST_F(OrcaRoomRuntime, CoSeatOrdinalStopsAtLocalOwnership)
{
  const std::string code = "coordinal";
  auto room = Open(code,
      R"({"joining":false,"doubles":true,"co_seats":{"me":[2],"peer":[3]}})", false);
  ASSERT_TRUE(Wait([&] { return room->CoSeats() == std::vector<int>{2}; }));
  EXPECT_EQ(room->LocalIndex(2, true), 1);
  EXPECT_EQ(room->LocalIndex(3), 1);
  EXPECT_EQ(room->LocalIndex(3, true), -1);
  picojson::value roster;
  ASSERT_TRUE(picojson::parse(roster, CoRoster("lobby", false, false, true)).empty());
  roster.get<picojson::object>()["participants"].get<picojson::array>().emplace_back(
      picojson::object{{"connectionId", picojson::value("replacement")},
                      {"slot", picojson::value(2.0)}, {"localIndex", picojson::value(0.0)}});
  Action(code, fmt::format(R"({{"message":{}}})", roster.serialize()));
  ASSERT_TRUE(Wait([&] { return room->GroupOf(2) == std::vector<int>{2}; }));
  EXPECT_EQ(room->LocalIndex(2), 0);
  EXPECT_EQ(room->LocalIndex(2, true), -1);
  EXPECT_EQ(room->LocalIndex(0, true), 0);
}

TEST(OrcaRoom, RetakenCoSeatKeepsTheFormerOwnersLeadPacket)
{
  for (const bool direct : {false, true})
  {
    auto replacement = picojson::value{};
    ASSERT_TRUE(picojson::parse(replacement, CoRoster("lobby", false, false, false)).empty());
    replacement.get<picojson::object>()["participants"].get<picojson::array>().emplace_back(
        picojson::object{{"connectionId", picojson::value("replacement")},
                        {"slot", picojson::value(3.0)}, {"localIndex", picojson::value(0.0)}});
    Packet packet = Frames(1, 20, 3);
    packet.co = {{3, Frames(3, 40, 3).pads}};
    const auto step = [direct](const Packet& value, int time) {
      return RoomTestStep{direct ? RK::DirectPacket : RK::Message, time,
          direct ? EncodePacket(value) : fmt::format(
              R"({{"t":"msg","from":"peer","d":{}}})", EncodePacket(value)), 1};
    };
    const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(false), {
        {RK::Message, 0, CoRoster("welcome", false, false, true)},
        {RK::Message, 0, PeerHello(false)},
        {RK::Message, 1, CoRoster("lobby", false, false, false)},
        {RK::Message, 2, replacement.serialize()}, step(packet, 3)});
    ASSERT_EQ(trace.back().packets.size(), 1u);
    EXPECT_EQ(trace.back().packets[0].pads, packet.pads);
    EXPECT_TRUE(trace.back().packets[0].co.empty());
  }
}

TEST_F(OrcaRoomRuntime, RetakenCoSeatRetiresBeforeHeldAdmission)
{
  FriendsHeadlessCore core;
  const std::string code = "coretake";
  auto room = Open(code,
      R"({"joining":false,"doubles":true,"co_seats":{"peer":[3]}})", false);
  ASSERT_TRUE(Wait([&] { return room->ArrivalPending(); }));
  room->TakePeerEvents();
  RoomHashGame game;
  Orca::Net::Config config;
  config.seats = MAX_SEATS;
  config.per_peer_inputs = true;
  config.authority_seat = 0;
  config.start_frame = 1600;
  std::array<SeatPlan, MAX_SEATS> plan{};
  plan[0] = {0, NEVER, 0};
  config.plan = plan;
  auto session = std::make_unique<Session>(config, game, *room);
  session->SetGroup(1, {3});
  session->AddPeerBatch({1, 3}, 1600, 1600);
  Packet caught_up = Frames(1, 1600, 1);
  caught_up.co = {{3, caught_up.pads}};
  caught_up.history_ack = 1599;
  caught_up.current_frame = 1600;
  Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer","d":{}}}}})",
                           EncodePacket(caught_up)));
  ASSERT_TRUE(Wait([&] {
    session->OnBoundary(1599, Pad{});
    return session->Plan()[3].plug_from != NEVER;
  }));
  session->UnplugSeat(3);
  const int unplug = session->Plan()[3].unplug_from;
  const auto lead_plan = session->Plan()[1];
  ASSERT_GT(unplug, session->CurrentFrame());
  auto replacement = picojson::value{};
  ASSERT_TRUE(picojson::parse(replacement, CoRoster("lobby", false, false, false)).empty());
  replacement.get<picojson::object>()["participants"].get<picojson::array>().emplace_back(
      picojson::object{{"connectionId", picojson::value("replacement")},
                      {"slot", picojson::value(3.0)}, {"localIndex", picojson::value(0.0)}});
  Action(code, fmt::format(R"({{"message":{}}})", replacement.serialize()));
  ASSERT_TRUE(Wait([&] { return room->GroupOf(3) == std::vector<int>{3}; }));
  Action(code, R"({"message":{"t":"msg","from":"replacement","d":{"k":"hello","c":"test",
      "r":true,"h":false,"doubles":true,"ct":"03"}}})");
  ASSERT_TRUE(Wait([&] { return room->ArrivalPending(); }));
  EXPECT_TRUE(Rollback::OnlineMatch::PumpHeldFriendsEventsForTest(*room, session, {1, 3}));
  EXPECT_EQ(session->Plan()[3].live_from, NEVER);
  EXPECT_EQ(session->Plan()[3].unplug_from, unplug);
  EXPECT_EQ(session->Plan()[1], lead_plan);
  EXPECT_TRUE(session->Error().empty()) << session->Error();
}

TEST(OrcaRoom, RetakenCoSeatKeepsTheHostsScheduledRoster)
{
  picojson::value replacement;
  ASSERT_TRUE(picojson::parse(replacement, CoRoster("lobby", true, false, false)).empty());
  replacement.get<picojson::object>()["participants"].get<picojson::array>().emplace_back(
      picojson::object{{"connectionId", picojson::value("replacement")},
                      {"slot", picojson::value(3.0)}, {"localIndex", picojson::value(0.0)}});
  Packet packet = Frames(0, 20, 3);
  std::array<SeatPlan, MAX_SEATS> plan{};
  plan[0] = plan[1] = {0, NEVER, 0};
  plan[3] = {0, 80, 0};
  packet.roster = plan;
  packet.groups = std::array<int, MAX_SEATS>{0, 1, -1, 0};
  const auto trace = TraceEstablishedCoRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoRoster("welcome", true, false, true)},
      {RK::Message, 0, PeerHello(true)},
      {RK::Message, 1, CoRoster("lobby", true, false, false)},
      {RK::Message, 2, replacement.serialize()},
      {RK::Message, 3, fmt::format(R"({{"t":"msg","from":"peer","d":{}}})",
                                  EncodePacket(packet))}});
  ASSERT_EQ(trace.back().packets.size(), 1u);
  EXPECT_EQ(trace.back().packets[0].groups, packet.groups);
  EXPECT_EQ(trace.back().packets[0].roster, packet.roster);
}

TEST_F(OrcaRoomRuntime, LocalOrdinalUsesPositionAfterRemoveAndReadd)
{
  const ScopedEnv enabled("ORCA_SESSION", "1");
  const Common::ScopeGuard cleanup([] { Orca::Online::Shutdown(); });
  const std::string code = "coindex";
  Request("/configure/" + code, fmt::format(
      R"({{"joining":false,"doubles":true,"co_seats":{{"me":[2]}},"compatibility":"{}"}})",
      Orca::Online::CompatibilityKey()));
  Orca::Online::StartRoom(code, false, true);
  ASSERT_TRUE(Wait([] { return Orca::Online::CoSeats() == std::vector<int>{2}; }));
  auto roster = picojson::value{};
  ASSERT_TRUE(picojson::parse(roster, CoRoster("lobby", false, true, false)).empty());
  for (auto& participant : roster.get<picojson::object>()["participants"].get<picojson::array>())
    if (participant.get("slot").get<double>() == 2)
      participant.get<picojson::object>()["localIndex"] = picojson::value(2.0);
  Action(code, fmt::format(R"({{"message":{}}})", roster.serialize()));
  auto* room = static_cast<YouGameRoom*>(Orca::Online::Transport());
  ASSERT_TRUE(Wait([&] { return room->LocalIndex(2) == 2; }));
  EXPECT_EQ(Orca::Online::LocalIndex(0), 0);
  EXPECT_EQ(Orca::Online::LocalIndex(2), 1);
  EXPECT_EQ(Orca::Online::LocalIndex(1), -1);
  EXPECT_EQ(Orca::Online::LocalIndex(-1), -1);
}

namespace
{
std::string CoPageWelcome(bool joining, const char* queue, bool doubles, bool mine = false)
{
  picojson::value value;
  picojson::parse(value, CoRoster("welcome", joining, mine, false));
  value.get<picojson::object>()["room"] = picojson::value(picojson::object{
      {"queue", picojson::value(queue)}, {"doubles", picojson::value(doubles)}});
  return value.serialize();
}
}  // namespace

TEST(OrcaRoom, CoPageMirrorRequiresFriendsDoublesAndAgreedCap)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  for (const char* queue : {"private", "casual", "ranked"})
    for (const bool doubles : {false, true})
      for (const bool cap : {false, true})
      {
        SCOPED_TRACE(fmt::format("{} doubles={} cap={}", queue, doubles, cap));
        Orca::Status::SetAppCaps(cap ? "coseats" : "");
        const auto welcome = CoPageWelcome(false, queue, doubles, true);
        const auto trace = TraceRoomForTest(TraceOptions(false), {{RK::Message, 0, welcome}});
        const picojson::value mirror(trace.back().mirror);
        EXPECT_EQ(mirror.get("lobby").get("maxLocalPlayers").get<double>(),
                  cap && doubles && std::string_view(queue) != "ranked" ? 2 : 1);
        picojson::value original;
        ASSERT_TRUE(picojson::parse(original, welcome).empty());
        if (std::string_view(queue) == "ranked" && doubles)
        {
          EXPECT_EQ(trace.back().state, RoomState::Ended);
          EXPECT_TRUE(mirror.get("participants").get<picojson::array>().empty());
        }
        else
          EXPECT_EQ(mirror.get("participants"), original.get("participants"));
        EXPECT_EQ(mirror.get("me"), original.get("me"));
      }
}

TEST(OrcaRoom, PageMirrorSerializesOnlyAfterItsInputsChange)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  Orca::Status::SetAppCaps("");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  std::vector<RoomTestStep> steps{
      {RK::Message, 0, CoPageWelcome(false, "private", true)}};
  for (int tick = 1; tick <= 100; ++tick)
    steps.push_back({RK::Tick, tick});
  steps.insert(steps.end(), {
      {RK::AppCaps, 101, "coseats"}, {RK::Tick, 102},
      {RK::Message, 103, PeerHello(false)}, {RK::Tick, 104},
      {RK::Message, 105, CoRoster("lobby", false, false, true)}, {RK::Tick, 106},
      {RK::AppCaps, 107, ""}, {RK::Tick, 108}, {RK::Tick, 109}});
  const auto trace = TraceRoomForTest(TraceOptions(false), steps);
  ASSERT_EQ(trace[0].mirror_serializations, 1u);
  for (size_t tick = 1; tick <= 100; ++tick)
  {
    EXPECT_EQ(trace[tick].mirror_serializations, 1u) << tick;
    EXPECT_EQ(trace[tick].published_mirror, picojson::value(trace[tick].mirror).serialize());
  }
  for (const size_t index : {102u, 103u, 104u, 105u, 106u, 108u, 109u})
    EXPECT_EQ(trace[index].published_mirror, picojson::value(trace[index].mirror).serialize()) << index;
  EXPECT_EQ(trace[102].mirror_serializations, 2u);
  EXPECT_EQ(trace[103].mirror_serializations, 3u);  // The first hello makes ready true.
  EXPECT_EQ(trace[104].mirror_serializations, 3u);
  EXPECT_EQ(trace[105].mirror_serializations, 4u);  // Roster and revision.
  EXPECT_EQ(trace[108].mirror_serializations, 5u);
  EXPECT_EQ(trace[109].mirror_serializations, 5u);
}

namespace
{
bool HasRoomKind(const RoomTestSnapshot& snapshot, const char* kind);
}

TEST(OrcaRoom, FirstWelcomeRemovesInheritedLocalsWithoutCountingThem)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  for (const bool joining : {false, true})
    for (const bool cap : {false, true})
      for (const char* enabled : {"0", "1"})
      {
        const ScopedEnv switch_value("ORCA_COSEATS", enabled);
        Orca::Status::SetAppCaps(cap ? "coseats" : "");
        const auto inherited = CoPageWelcome(joining, "private", true, true);
        const auto trace = TraceRoomForTest(TraceOptions(joining), {
            {RK::Message, 0, inherited}, {RK::Tick, 1}, {RK::SessionActive, 2, "", 1},
            {RK::Message, 3, inherited}, {RK::Tick, 4},
            {RK::Message, 5, CoPageWelcome(joining, "private", true)}, {RK::Tick, 1006},
            {RK::SessionActive, 1007, "", 1}});
        EXPECT_EQ(trace[0].sent, (std::vector<std::string>{
            R"({"id":"local:me:1","t":"local-remove"})"}));
        for (const size_t index : {0u, 1u, 2u, 3u, 4u, 5u, 6u})
        {
          EXPECT_TRUE(trace[index].co_seats.empty());
          EXPECT_EQ(trace[index].local_indices[2], -1);
        }
        EXPECT_FALSE(HasRoomKind(trace[1], "hello"));
        ASSERT_TRUE(trace[2].input_held);
        EXPECT_FALSE(*trace[2].input_held);
        EXPECT_TRUE(trace[3].sent.empty());
        EXPECT_TRUE(HasRoomKind(trace[6], "hello"));
        ASSERT_TRUE(trace[7].input_held);
        EXPECT_TRUE(*trace[7].input_held);
      }
}

TEST(OrcaRoom, FirstWelcomeKeepsAnExplicitPendingLocalAdd)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(true), {
      {RK::PageEvent, 0, R"({"slug":"co-page","cmd":"local-add"})"},
      {RK::Message, 1, CoPageWelcome(true, "private", true, true)}, {RK::Tick, 2}});
  EXPECT_EQ(trace[1].co_seats, (std::vector<int>{2}));
  for (const auto& step : trace)
    for (const auto& wire : step.sent)
    {
      picojson::value value;
      ASSERT_TRUE(picojson::parse(value, wire).empty());
      EXPECT_NE(value.get("t").to_str(), "local-remove");
    }
}

TEST(OrcaRoom, CoPageCommandsPinEveryGateAndStatus)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  struct Gate { const char* queue; bool doubles; bool cap; bool active; bool tournament; const char* reason; };
  for (const auto& gate : std::vector<Gate>{
      {"private", true, true, false, false, ""},
      {"casual", true, true, false, false, ""},
      {"casual", false, true, false, false, "queue"},
      {"casual", true, true, false, true, "queue"},
      {"ranked", true, true, false, false, "queue"},
      {"private", true, true, false, true, "queue"},
      {"private", false, true, false, false, "not-doubles"},
      {"private", true, false, false, false, "no-cap"},
      {"private", true, true, true, false, "in-match"}})
    for (const char* action : {"local-add", "local-remove"})
    {
      SCOPED_TRACE(fmt::format("{} {}", action, gate.reason));
      Orca::Status::SetAppCaps(gate.cap ? "coseats" : "");
      std::vector<RoomTestStep> steps{
          {RK::Message, 0, CoPageWelcome(false, gate.queue, gate.doubles)}};
      if (std::string_view(action) == "local-remove")
        steps.push_back({RK::Message, 1, CoRoster("lobby", false, true, false)});
      steps.push_back({RK::SessionActive, 1, "", gate.active ? 1u : 0u});
      if (gate.tournament)
        steps.push_back({RK::Tournament, 1});
      steps.push_back({RK::PageEvent, 2, fmt::format(
          R"({{"slug":"co-page","cmd":"{}","id":"local:me:1"}})", action)});
      const auto trace = TraceRoomForTest(TraceOptions(false), steps);
      const auto& last = trace.back();
      const std::string_view reason = gate.active && std::string_view(action) == "local-remove" ?
                                          "" : gate.reason;
      if (reason.empty())
      {
        ASSERT_EQ(last.sent.size(), 1u);
        picojson::value sent;
        ASSERT_TRUE(picojson::parse(sent, last.sent[0]).empty());
        EXPECT_EQ(sent.get("t").to_str(), action);
        EXPECT_EQ(sent.get<picojson::object>().size(), std::string_view(action) == "local-add" ? 1u : 2u);
        if (std::string_view(action) == "local-remove")
          EXPECT_EQ(sent.get("id").to_str(), "local:me:1");
        EXPECT_TRUE(last.notices.empty());
      }
      else
      {
        EXPECT_TRUE(last.sent.empty());
        const auto expected = fmt::format("Room: {} local player isn't available here ({})",
            std::string_view(action) == "local-add" ? "Add" : "Remove", reason);
        EXPECT_EQ(last.notices, (std::vector<std::string>{expected}));
        EXPECT_EQ(last.status, expected);
      }
    }
  Orca::Status::SetAppCaps("coseats");
  const auto ignored = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoPageWelcome(false, "private", true)},
      {RK::PageEvent, 1, R"({"slug":"another-page","cmd":"local-add"})"},
      {RK::PageEvent, 2, R"({"slug":"co-page","cmd":"local-remove","id":"me"})"}});
  EXPECT_TRUE(ignored[1].sent.empty());
  EXPECT_TRUE(ignored[1].notices.empty());
  EXPECT_TRUE(ignored[2].sent.empty());
  EXPECT_TRUE(ignored[2].notices.empty());
}

TEST(OrcaRoom, OwnCoSeatCanLeaveDuringAHostOrJoinerSession)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  for (const bool joining : {false, true})
  {
    const auto trace = TraceRoomForTest(TraceOptions(joining), {
        {RK::Message, 0, CoPageWelcome(joining, "private", true)},
        {RK::PageEvent, 1, R"({"slug":"co-page","cmd":"local-add"})"},
        {RK::Message, 2, CoRoster("lobby", joining, true, false)}, {RK::Tick, 3},
        {RK::SessionActive, 4, "", 1},
        {RK::PageEvent, 5, R"({"slug":"co-page","cmd":"local-remove","id":"local:me:1"})"},
        {RK::Message, 6, CoRoster("lobby", joining, false, false)},
        {RK::PageEvent, 7, R"({"slug":"co-page","cmd":"local-add"})"},
        {RK::PageEvent, 8, R"({"slug":"co-page","cmd":"local-remove","id":"local:peer:1"})"}});
    ASSERT_TRUE(trace[4].input_held);
    EXPECT_TRUE(*trace[4].input_held);
    EXPECT_EQ(trace[5].sent, (std::vector<std::string>{R"({"id":"local:me:1","t":"local-remove"})"}));
    EXPECT_TRUE(trace[5].notices.empty());
    EXPECT_TRUE(trace[6].co_seats.empty());
    ASSERT_EQ(trace[6].events.size(), 1u);
    EXPECT_EQ(trace[6].events[0].kind, PeerEvent::Kind::CoSeatLeft);
    EXPECT_EQ(trace[6].events[0].seat, 2);
    EXPECT_EQ(trace[7].notices, (std::vector<std::string>{
        "Room: Add local player isn't available here (in-match)"}));
    EXPECT_TRUE(trace[8].sent.empty());
    EXPECT_EQ(trace[8].notices, (std::vector<std::string>{
        "Room: Remove local player isn't available here (not-local)"}));
  }
}

TEST(OrcaRoom, CoJoinerAddsBeforeHelloAndWaitsForItsRoster)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(true), {
      {RK::PageEvent, 0, R"({"slug":"co-page","cmd":"local-add"})"},
      {RK::Message, 0, CoPageWelcome(true, "private", true)},
      {RK::Message, 1, PeerHello(true)}, {RK::Tick, 1000},
      {RK::Message, 1200, CoRoster("lobby", true, true, false)}, {RK::Tick, 1201},
      {RK::PageEvent, 1202, R"({"slug":"co-page","cmd":"local-add"})"}});
  EXPECT_TRUE(trace[0].sent.empty());
  ASSERT_EQ(trace[1].sent, (std::vector<std::string>{R"({"t":"local-add"})"}));
  for (const auto& wire : trace[3].sent)
  {
    picojson::value value;
    ASSERT_TRUE(picojson::parse(value, wire).empty());
    const auto& data = value.get("d");
    EXPECT_FALSE(data.is<picojson::object>() && data.get("k").to_str() == "hello");
  }
  EXPECT_EQ(trace[4].co_seats, (std::vector<int>{2}));
  ASSERT_FALSE(trace[5].sent.empty());
  bool hello = false;
  for (const auto& wire : trace[5].sent)
  {
    picojson::value value;
    ASSERT_TRUE(picojson::parse(value, wire).empty());
    const auto& data = value.get("d");
    hello |= data.is<picojson::object>() && data.get("k").to_str() == "hello";
  }
  EXPECT_TRUE(hello);
  EXPECT_TRUE(trace[6].sent.empty());
  EXPECT_EQ(trace[6].notices, (std::vector<std::string>{
      "Room: Add local player isn't available here (in-match)"}));
}

namespace
{
bool HasRoomKind(const RoomTestSnapshot& snapshot, const char* kind)
{
  for (const auto& wire : snapshot.sent)
  {
    picojson::value value;
    if (picojson::parse(value, wire).empty() && value.get("d").is<picojson::object>() &&
        value.get("d").get("k").to_str() == kind)
      return true;
  }
  return false;
}
const std::string CO_PAGE_ADD = R"({"slug":"co-page","cmd":"local-add"})";
}

TEST(OrcaRoom, CoJoinerLateAcceptKeepsHelloHeldUntilTheRoster)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoPageWelcome(true, "private", true)},
      {RK::PageEvent, 1, CO_PAGE_ADD}, {RK::Message, 2, PeerHello(true)},
      {RK::Tick, 2501}, {RK::Tick, 9000},
      {RK::Message, 9001, CoRoster("lobby", true, true, false)}, {RK::Tick, 9002}});
  EXPECT_FALSE(HasRoomKind(trace[3], "hello"));
  EXPECT_FALSE(HasRoomKind(trace[4], "hello"));
  EXPECT_EQ(trace[5].co_seats, (std::vector<int>{2}));
  EXPECT_TRUE(HasRoomKind(trace[6], "hello"));
  EXPECT_TRUE(trace[3].notices.empty());
}

TEST(OrcaRoom, CoJoinerAddErrorResolvesTheHeldHello)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoPageWelcome(true, "private", true)},
      {RK::PageEvent, 1, CO_PAGE_ADD}, {RK::Message, 2, PeerHello(true)},
      {RK::Message, 3000, R"({"t":"command-error","action":"local-add","error":"No local player slot is available"})"},
      {RK::Tick, 3001}});
  EXPECT_TRUE(trace[3].co_seats.empty());
  EXPECT_TRUE(HasRoomKind(trace[4], "hello"));
}

TEST(OrcaRoom, CoJoinerTimeoutCancelsBeforeSendingHelloOrLeaves)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  for (const bool removed : {false, true})
  {
    std::vector<RoomTestStep> steps{
        {RK::Message, 0, CoPageWelcome(true, "private", true)},
        {RK::PageEvent, 1, CO_PAGE_ADD}, {RK::Message, 2, PeerHello(true)},
        {RK::Tick, 10000}, {RK::Tick, 10001},
        {RK::Message, 11000, CoRoster("lobby", true, true, false)}, {RK::Tick, 11001}};
    if (removed)
      steps.push_back({RK::Message, 12000, CoPageWelcome(true, "private", true)});
    steps.push_back({RK::Tick, 20001});
    const auto trace = TraceRoomForTest(TraceOptions(true), steps);
    EXPECT_TRUE(trace[3].notices.empty());
    EXPECT_EQ(trace[4].notices, (std::vector<std::string>{
        "Room: Add local player timed out; waiting for its removal"}));
    EXPECT_FALSE(HasRoomKind(trace[4], "hello"));
    EXPECT_FALSE(HasRoomKind(trace[6], "hello"));
    bool remove_sent = false;
    for (const auto& wire : trace[5].sent)
    {
      picojson::value value;
      ASSERT_TRUE(picojson::parse(value, wire).empty());
      remove_sent |= value.get("t").to_str() == "local-remove";
    }
    EXPECT_TRUE(remove_sent);
    if (removed)
    {
      EXPECT_TRUE(trace[7].co_seats.empty());
      EXPECT_TRUE(HasRoomKind(trace.back(), "hello"));
    }
    else
    {
      EXPECT_EQ(trace.back().state, RoomState::Ended);
      EXPECT_EQ(trace.back().status, "Room: Local player removal did not finish; leave and rejoin the room");
      EXPECT_FALSE(HasRoomKind(trace.back(), "hello"));
    }
  }
  // No response at all cannot prove whether the add was accepted: leave at the cleanup bound.
  const auto missing = TraceRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoPageWelcome(true, "private", true)}, {RK::PageEvent, 1, CO_PAGE_ADD},
      {RK::Message, 2, PeerHello(true)}, {RK::Tick, 10001}, {RK::Tick, 20001}});
  EXPECT_EQ(missing.back().state, RoomState::Ended);
  EXPECT_FALSE(HasRoomKind(missing.back(), "hello"));
}

TEST_F(OrcaRoomRuntime, LaggingRoomRosterCannotRetireTheHostsOwnCoSeat)
{
  const std::string code = "coownlag";
  auto room = Open(code, R"({"joining":false,"doubles":true,"co_seats":{"peer":[3]}})", false);
  ASSERT_TRUE(Wait([&] { return room->ArrivalPending(); }));
  room->TakePeerEvents();
  RoomHashGame game;
  Orca::Net::Config config;
  config.seats = MAX_SEATS;
  config.local_seat = config.authority_seat = 0;
  config.co_seats = {3};
  config.per_peer_inputs = true;
  config.start_frame = 1600;
  std::array<SeatPlan, MAX_SEATS> plan{};
  plan[0] = plan[3] = {0, NEVER, 0};
  config.plan = plan;
  auto session = std::make_unique<Session>(config, game, *room);
  Rollback::OnlineMatch::PumpHeldFriendsEventsForTest(*room, session, {});
  EXPECT_TRUE(session->IsLocal(3));
  EXPECT_EQ(session->LeadOf(3), 0);
  EXPECT_EQ(session->Plan()[3], plan[3]);
}

TEST_F(OrcaRoomRuntime, QueueTicketRequestAndReconnectKeepOneLocalPlayer)
{
  for (const char* queue : {"casual", "ranked"})
  {
    const std::string code = std::string("cop4") + queue;
    auto room = Open(code, fmt::format(R"({{"queue":"{}","lobby":{{"maxLocalPlayers":1}}}})", queue),
                     true, false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
    std::string ticket, store, error;
    ASSERT_TRUE(room->FreshTicket(true, &ticket, &store, &error)) << error;
    const auto lobbies = Request("/state/" + code).get("ticket_lobbies");
    ASSERT_EQ(lobbies.get<picojson::array>().size(), 2u);
    EXPECT_EQ(lobbies.get<picojson::array>()[0].get("maxLocalPlayers").get<double>(), 2);
    EXPECT_EQ(lobbies.get<picojson::array>()[1].get("maxLocalPlayers").get<double>(), 1);
  }
}

TEST(OrcaRoom, PendingLocalAddReservesTheSoloHostUntilItsRosterArrives)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoPageWelcome(false, "private", true)},
      {RK::PageEvent, 1, R"({"slug":"co-page","cmd":"local-add"})"},
      {RK::SessionActive, 2, "", 1},
      {RK::PageEvent, 3, R"({"slug":"co-page","cmd":"local-add"})"},
      {RK::Message, 4, CoRoster("lobby", false, true, false)},
      {RK::SessionActive, 5, "", 1}});
  EXPECT_EQ(trace[1].sent, (std::vector<std::string>{R"({"t":"local-add"})"}));
  ASSERT_TRUE(trace[2].input_held.has_value());
  EXPECT_FALSE(*trace[2].input_held);
  EXPECT_TRUE(trace[3].sent.empty());
  EXPECT_EQ(trace[3].notices, (std::vector<std::string>{
      "Room: Add local player isn't available here (in-match)"}));
  EXPECT_EQ(trace[4].co_seats, (std::vector<int>{2}));
  ASSERT_TRUE(trace[5].input_held.has_value());
  EXPECT_TRUE(*trace[5].input_held);
}

TEST_F(OrcaRoomRuntime, TournamentReconnectKeepsOneWithoutReturnedLobby)
{
  const std::string code = "cop4tourney";
  auto room = Open(code, R"({"tourney":1})");
  ASSERT_TRUE(Wait([&] { return room->Seat() == 1; }));
  std::string ticket, store, error;
  ASSERT_TRUE(room->FreshTicket(true, &ticket, &store, &error)) << error;
  const auto lobbies = Request("/state/" + code).get("ticket_lobbies");
  ASSERT_EQ(lobbies.get<picojson::array>().size(), 2u);
  EXPECT_EQ(lobbies.get<picojson::array>()[0].get("maxLocalPlayers").get<double>(), 2);
  EXPECT_EQ(lobbies.get<picojson::array>()[1].get("maxLocalPlayers").get<double>(), 1);
}

TEST(OrcaRoom, CoStartupNeverAddsWithoutAPageCommand)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv stdio("YOUGAME_STDIO", "1");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  Orca::Status::SetAppCaps("coseats");
  Orca::UX::StartControllerStreamFromStdio();
  const Common::ScopeGuard cleanup([] {
    Orca::UX::StopControllerStream();
    Orca::Status::SetAppCaps("");
  });
  for (const bool pair : {false, true})
  {
    picojson::array pads;
    for (const int ordinal : pair ? std::vector<int>{0, 1} : std::vector<int>{1})
    {
      picojson::array buttons;
      for (int index = 0; index < 17; ++index)
        buttons.emplace_back(picojson::object{{"value", picojson::value(0.0)},
                                              {"pressed", picojson::value(false)}});
      pads.emplace_back(picojson::object{{"index", picojson::value(static_cast<double>(ordinal))},
          {"seat", picojson::value(static_cast<double>(ordinal))},
          {"mapping", picojson::value("standard")},
          {"axes", picojson::value(picojson::array(4, picojson::value(0.0)))},
          {"buttons", picojson::value(buttons)}});
    }
    picojson::object snapshot{{"schema", picojson::value(1.0)}, {"session", picojson::value("co-startup")},
        {"sequence", picojson::value(pair ? 2.0 : 1.0)}, {"sentAt", picojson::value(1000.0)},
        {"receivedAt", picojson::value(1000.0)}, {"owned", picojson::value(false)},
        {"suspended", picojson::value(false)}, {"ports", picojson::value(picojson::array{})},
        {"pads", picojson::value(pads)}};
    const auto event = picojson::value(picojson::object{{"push", picojson::value(picojson::object{
        {"type", picojson::value("controllers")}, {"snapshot", picojson::value(snapshot)}})}});
    EXPECT_TRUE(Orca::Bridge::Route({"yg " + event.serialize()}).empty());
    // Adapter ports always carry seats, even when the page never requested Player 2.
    snapshot["owned"] = picojson::value(true);
    picojson::array ports;
    for (int port = 0; port < 4; ++port)
      ports.emplace_back(picojson::object{{"port", picojson::value(static_cast<double>(port))},
          {"seat", picojson::value(static_cast<double>(port))},
          {"connected", picojson::value(port < 2)},
          {"type", port < 2 ? picojson::value("wired") : picojson::value()},
          {"buttons", picojson::value(0.0)},
          {"axes", picojson::value(picojson::array(4, picojson::value(128.0)))},
          {"triggers", picojson::value(picojson::array(2, picojson::value(0.0)))},
          {"origin", picojson::value(picojson::array(6, picojson::value(128.0)))}});
    snapshot["ports"] = picojson::value(ports);
    const auto adapters = picojson::value(picojson::object{{"push", picojson::value(picojson::object{
        {"type", picojson::value("controllers")}, {"snapshot", picojson::value(snapshot)}})}});
    EXPECT_TRUE(Orca::Bridge::Route({"yg " + adapters.serialize()}).empty());
    for (const bool joining : {false, true})
    {
      const auto trace = TraceRoomForTest(TraceOptions(joining), {
          {RK::Message, 0, CoPageWelcome(joining, "private", true)}});
      EXPECT_TRUE(trace[0].sent.empty());
      EXPECT_TRUE(trace[0].co_seats.empty());
    }
  }
}

TEST_F(OrcaRoomRuntime, QueuedFriendJoinRequestsTheHostsPrivateLobby)
{
  for (const bool searching : {false, true})
  {
    Orca::UX::QueueApp::Begin(false);
    if (searching)
      Orca::UX::Search::Begin(false);
    const Common::ScopeGuard cleanup([] {
      Orca::UX::QueueApp::End();
      Orca::UX::Search::End();
    });
    const std::string code = searching ? "cofriendsea" : "cofriendcss";
    auto room = Open(code, R"({"doubles":true,"required_local_players":2})", true, false);
    ASSERT_TRUE(Wait([&] { return room->Connected() || room->GetState() == RoomState::Ended; }));
    EXPECT_TRUE(room->Connected()) << room->Status();
    const auto lobbies = Request("/state/" + code).get("ticket_lobbies");
    ASSERT_EQ(lobbies.get<picojson::array>().size(), 1u);
    EXPECT_EQ(lobbies.get<picojson::array>()[0].get("maxLocalPlayers").get<double>(), 2);
  }
}

TEST(OrcaRoom, PrivateRoomKindEnablesCoSeatsAfterAQueuedFriendJoin)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  auto options = TraceOptions(true);
  options.friends_room = false;
  const auto trace = TraceRoomForTest(options, {
      {RK::Message, 0, CoPageWelcome(true, "private", true)},
      {RK::PageEvent, 1, R"({"slug":"co-page","cmd":"local-add"})"}});
  EXPECT_EQ(picojson::value(trace[0].mirror).get("lobby").get("maxLocalPlayers").get<double>(), 2);
  EXPECT_EQ(trace[1].sent, (std::vector<std::string>{R"({"t":"local-add"})"}));
}

TEST(OrcaRoom, CoRemoveErrorDoesNotResolveAPendingAdd)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoPageWelcome(true, "private", true)}, {RK::PageEvent, 1, CO_PAGE_ADD},
      {RK::Message, 2, PeerHello(true)},
      {RK::Message, 3, R"({"t":"command-error","action":"local-remove","error":"Only your additional local players can be removed"})"},
      {RK::Tick, 1000}, {RK::SessionActive, 1001, "", 1},
      {RK::Message, 3000, CoRoster("lobby", true, true, false)}, {RK::Tick, 3001}});
  EXPECT_FALSE(HasRoomKind(trace[4], "hello"));
  ASSERT_TRUE(trace[5].input_held);
  EXPECT_FALSE(*trace[5].input_held);
  EXPECT_EQ(trace[6].co_seats, (std::vector<int>{2}));
  EXPECT_TRUE(HasRoomKind(trace[7], "hello"));
}

TEST(OrcaRoom, CoCommandErrorsUseTheRoomsRefusalSentences)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  for (const auto& [error, reason] : std::vector<std::pair<const char*, const char*>>{
      {"No local player slot is available", "full"},
      {"Only your additional local players can be removed", "not-local"},
      {"The set is over", "in-match"}, {"This match is complete", "in-match"},
      {"A future room refusal", "room-refused"}})
    for (const char* action : {"local-add", "local-remove"})
    {
      const auto trace = TraceRoomForTest(TraceOptions(true), {
          {RK::Message, 0, CoPageWelcome(true, "private", true)},
          {RK::Message, 1, fmt::format(
              R"({{"t":"command-error","action":"{}","error":"{}"}})", action, error)}});
      EXPECT_EQ(trace.back().notices, (std::vector<std::string>{fmt::format(
          "Room: {} local player isn't available here ({})",
          std::string_view(action) == "local-add" ? "Add" : "Remove", reason)}));
    }
}

TEST(OrcaRoom, CoPendingHostAddKeepsExistingFriendsOffersOnTheirLegacySchedule)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  KeyframeInfo info{120, "replay-120-00112233", 1234, "00112233aabbccdd", std::string(64, '0')};
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoPageWelcome(false, "private", true)},
      {RK::Message, 1, PeerHello(false)},
      {RK::OfferKeyframe, 2, picojson::value(KeyframeOfferToJson(info)).serialize(), 1},
      {RK::Tick, 1000}, {RK::PageEvent, 1001, CO_PAGE_ADD}, {RK::Tick, 2000}});
  ASSERT_TRUE(HasRoomKind(trace[3], "kf"));
  EXPECT_TRUE(HasRoomKind(trace[5], "kf"));
  EXPECT_FALSE(HasRoomKind(trace[5], "hello"));
}

TEST(OrcaRoom, CoJoinerAcceptsKeyframeOffersOnlyAfterItsPendingAddResolves)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  KeyframeInfo info{120, "replay-120-00112233", 1234, "00112233aabbccdd", std::string(64, '0')};
  const auto offer = fmt::format(R"({{"t":"msg","from":"peer","d":{}}})",
      picojson::value(KeyframeOfferToJson(info)).serialize());
  const auto trace = TraceRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoPageWelcome(true, "private", true)}, {RK::PageEvent, 1, CO_PAGE_ADD},
      {RK::Message, 2, PeerHello(true)}, {RK::Message, 3, offer},
      {RK::Message, 3000, CoRoster("lobby", true, true, false)}, {RK::Tick, 3001},
      {RK::Message, 3002, offer}});
  EXPECT_TRUE(trace[3].events.empty());
  ASSERT_EQ(trace[6].events.size(), 1u);
  EXPECT_EQ(trace[6].events[0].kind, PeerEvent::Kind::Keyframe);
}

TEST(OrcaRoom, SingleSeatSessionStartKeepsLegacyNoRoomAndEndedRoomBehavior)
{
  EXPECT_TRUE(Orca::Online::TryStartSession({}));
  EXPECT_FALSE(Orca::Online::TryStartSession({2}));
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoPageWelcome(false, "private", true)},
      {RK::SocketClose, 1, "room-closed", 4004}, {RK::SessionActive, 2, "", 1}});
  ASSERT_EQ(trace[1].state, RoomState::Ended);
  ASSERT_TRUE(trace[2].input_held);
  EXPECT_TRUE(*trace[2].input_held);
}

TEST_F(OrcaRoomRuntime, LocalRosterCountSurvivesMissingDevicesAndIgnoresStraySeatTags)
{
  FriendsHeadlessCore core;
  const ScopedEnv stdio("YOUGAME_STDIO", "1");
  const ScopedEnv virtual_only("ORCA_VIRTUAL_PAD_ONLY", "1");
  // Open() uses the loopback dev ticket, so start the controller stream separately.
  const ScopedEnv ticket_stdio("YOUGAME_STDIO", "0");
  Orca::UX::StartControllerStreamFromStdio();
  const Common::ScopeGuard cleanup([] {
    Orca::Status::SetOutput(nullptr);
    Orca::UX::StopControllerStream();
    Orca::UX::SetLocalPadCount(1);
  });
  int sequence = 0;
  for (const bool pair : {false, true})
  {
    const auto snapshot = [&](bool tag_second) {
      picojson::array pads;
      for (int index = 0; index < 2; ++index)
      {
        picojson::array buttons;
        for (int button = 0; button < 17; ++button)
          buttons.emplace_back(picojson::object{{"value", picojson::value(button == index ? 1.0 : 0.0)},
              {"pressed", picojson::value(button == index)}});
        picojson::object pad{{"index", picojson::value(static_cast<double>(index))},
            {"mapping", picojson::value("standard")},
            {"axes", picojson::value(picojson::array(4, picojson::value(0.0)))},
            {"buttons", picojson::value(buttons)}};
        if (index == 0 || tag_second)
          pad["seat"] = picojson::value(static_cast<double>(pair ? index : 1));
        pads.emplace_back(pad);
      }
      picojson::object value{{"schema", picojson::value(1.0)},
          {"session", picojson::value("roster-count")},
          {"sequence", picojson::value(static_cast<double>(++sequence))},
          {"sentAt", picojson::value(1000.0)}, {"receivedAt", picojson::value(1000.0)},
          {"owned", picojson::value(false)}, {"suspended", picojson::value(false)},
          {"ports", picojson::value(picojson::array{})}, {"pads", picojson::value(pads)}};
      const ScopedEnv controllers_stdio("YOUGAME_STDIO", "1");
      EXPECT_TRUE(Orca::Bridge::Route({"yg " + picojson::value(picojson::object{
          {"push", picojson::value(picojson::object{{"type", picojson::value("controllers")},
              {"snapshot", picojson::value(value)}})}}).serialize()}).empty());
    };
    snapshot(false);
    const std::string code = pair ? "corosterpair" : "corostersolo";
    auto room = Open(code, pair ?
        R"({"joining":false,"doubles":true,"co_seats":{"me":[2],"peer":[3]}})" :
        R"({"joining":false,"doubles":true,"co_seats":{"peer":[3]}})", false);
    ASSERT_TRUE(Wait([&] { return room->Seat() == 0 && room->ArrivalPending(); }));
    RoomHashGame game;
    auto store = MakeHttpKeyframeStore([&](bool fresh, std::string* ticket, std::string* server,
                                          std::string* error) {
      return room->FreshTicket(fresh, ticket, server, error);
    });
    auto* output = std::tmpfile();
    ASSERT_NE(output, nullptr);
    const Common::ScopeGuard close([&] { Orca::Status::SetOutput(nullptr); std::fclose(output); });
    const auto result = Rollback::OnlineMatch::PumpFriendsHostForTest(core.system, *room, game,
        FriendsReplay(1524, 1600), 1600, {}, true, std::move(store), false,
        [&](const std::function<void()>&) {
          const ScopedEnv printing("ORCA_SESSION", "1");
          Orca::Status::SetOutput(output);
          snapshot(false);
          ASSERT_TRUE(Orca::UX::LocalPad(0, true));
          EXPECT_EQ(Orca::UX::LocalPad(0, true)->button, PAD_BUTTON_A);
          if (pair)
          {
            const auto neutral = Orca::UX::LocalPad(1, true);
            ASSERT_TRUE(neutral);
            EXPECT_TRUE(neutral->isConnected);
            EXPECT_EQ(neutral->button, 0);
            EXPECT_EQ(neutral->stickX, 128);
          }
          snapshot(true);
          EXPECT_EQ(Orca::UX::LocalPad(0, true)->button, PAD_BUTTON_A);
          if (pair)
            EXPECT_EQ(Orca::UX::LocalPad(1, true)->button, PAD_BUTTON_X);
        });
    EXPECT_TRUE(result.error.empty()) << result.error;
    if (pair)
    {
      EXPECT_EQ(room->CoSeats(), (std::vector<int>{2}));
      EXPECT_EQ(room->GroupOf(2), (std::vector<int>{0, 2}));
    }
    std::rewind(output);
    std::string lines;
    char buffer[256]{};
    while (std::fgets(buffer, sizeof(buffer), output))
      lines += buffer;
    EXPECT_EQ(lines, pair ? "orca seats 1=pad:0 2=none 3=none 4=none\n"
                            "orca seats 1=pad:0 2=pad:1 3=none 4=none\n" : "");
  }
}

TEST(OrcaRoom, SoloHostDoesNotPlugALocalSeatDuringAddCleanup)
{
  FriendsHeadlessCore core;
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  std::vector<Pads> frames;
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoPageWelcome(false, "private", true)}, {RK::PageEvent, 1, CO_PAGE_ADD},
      {RK::Tick, 10001}, {RK::Message, 11000, CoRoster("lobby", false, true, false)},
      {RK::Tick, 11001}, {RK::Message, 12000, CoPageWelcome(false, "private", true)},
      {RK::PageEvent, 12001, CO_PAGE_ADD},
      {RK::Message, 12002, CoRoster("lobby", false, true, false)}},
      [&](YouGameRoom& room) {
        frames.push_back(Rollback::OnlineMatch::PumpSoloRoomForTest(core.system, room));
      });
  ASSERT_EQ(trace[3].co_seats, (std::vector<int>{2}));  // Raw roster, still being cancelled.
  ASSERT_EQ(frames.size(), 8u);
  for (size_t frame = 0; frame < 7; ++frame)
    EXPECT_EQ(frames[frame][2], UNPLUGGED_PAD) << frame;
  EXPECT_EQ(frames[7][2], Pad{});  // A later, completed Add really plugs it.
}

TEST(OrcaRoom, CoPageStatusDistinguishesFullFromNotReady)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::PageEvent, 0, R"({"slug":"co-page","cmd":"local-remove","id":"local:me:1"})"},
      {RK::Message, 1, CoPageWelcome(false, "private", true)}, {RK::PageEvent, 2, CO_PAGE_ADD},
      {RK::Message, 3, CoRoster("lobby", false, true, false)}, {RK::PageEvent, 4, CO_PAGE_ADD}});
  EXPECT_TRUE(trace[0].sent.empty());
  EXPECT_EQ(trace[0].notices, (std::vector<std::string>{
      "Room: Remove local player isn't available here (not-ready)"}));
  EXPECT_TRUE(trace[4].sent.empty());
  EXPECT_EQ(trace[4].notices, (std::vector<std::string>{
      "Room: Add local player isn't available here (full)"}));
}

TEST(OrcaRoom, JoinerPageAddAfterTheWelcomeTickIsAMidSessionAdd)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(true), {
      {RK::Message, 0, CoPageWelcome(true, "private", true)}, {RK::Tick, 1001},
      {RK::PageEvent, 1002, CO_PAGE_ADD}, {RK::Tick, 2002}});
  EXPECT_TRUE(HasRoomKind(trace[1], "hello"));
  EXPECT_TRUE(trace[2].sent.empty());
  EXPECT_EQ(trace[2].notices, (std::vector<std::string>{
      "Room: Add local player isn't available here (in-match)"}));
  EXPECT_TRUE(trace[3].co_seats.empty());
  // The page must send Add with or before joining; after hello it needs P5.
}

TEST(OrcaRoom, QueueDoublesWelcomeAndRankedRefusal)
{
  for (const char* queue : {"private", "casual", "ranked"})
  {
    SCOPED_TRACE(queue);
    bool doubles = false;
    const auto welcome = CoPageWelcome(false, queue, true);
    const auto trace = TraceRoomForTest(TraceOptions(false), {
        {RK::Message, 0, welcome}, {RK::Message, 1, welcome}, {RK::Tick, 1001}},
        [&](YouGameRoom& room) { doubles = room.Doubles(); });
    if (std::string_view(queue) == "ranked")
    {
      EXPECT_FALSE(doubles);
      EXPECT_EQ(trace.back().state, RoomState::Ended);
      EXPECT_EQ(trace.back().status, "Room: ranked doubles requires four single-seat players and two complete teams");
      size_t lines = 0;
      for (const auto& step : trace)
        lines += std::count(step.notices.begin(), step.notices.end(),
                            "Room: ranked doubles requires four single-seat players and two complete teams");
      EXPECT_EQ(lines, 1u);
      for (const auto& step : trace)
        for (const auto& wire : step.sent)
        {
          picojson::value message;
          ASSERT_TRUE(picojson::parse(message, wire).empty());
          // A forced trace tick may ping; the refused room never sends gameplay or results.
          EXPECT_EQ(message.get("t").to_str(), "ping");
        }
      EXPECT_TRUE(picojson::value(trace.back().mirror).get("participants").get<picojson::array>().empty());
    }
    else
    {
      EXPECT_TRUE(doubles);
      EXPECT_NE(trace.back().state, RoomState::Ended);
      EXPECT_TRUE(picojson::value(trace.back().mirror).get("doubles").get<bool>());
      EXPECT_EQ(picojson::value(trace.back().mirror).get("size").get<double>(), 4);
    }
  }
}

TEST(OrcaRoom, ParticipantTeamsFollowWelcomeLobbyAndStart)
{
  auto roster = [](const char* kind, bool swap) {
    picojson::value value;
    picojson::parse(value, CoPageWelcome(false, "casual", true));
    value.get<picojson::object>()["t"] = picojson::value(kind);
    auto& participants = value.get<picojson::object>()["participants"].get<picojson::array>();
    participants.emplace_back(picojson::object{{"connectionId", picojson::value("guest")},
        {"slot", picojson::value(2.0)}, {"localIndex", picojson::value(0.0)}});
    participants.emplace_back(picojson::object{{"connectionId", picojson::value("peer")},
        {"slot", picojson::value(3.0)}, {"localIndex", picojson::value(1.0)}});
    for (auto& p : participants)
      p.get<picojson::object>()["team"] = picojson::value(
          double((int(p.get("slot").get<double>()) % 2) ^ int(swap)));
    return value.serialize();
  };
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, roster("welcome", false)},
      {RK::Message, 1, roster("lobby", true)},
      {RK::Message, 2, roster("start", false)},
      {RK::Message, 3, CoPageWelcome(false, "casual", true)}});
  EXPECT_EQ(trace[0].teams, (std::map<int, int>{{0, 0}, {1, 1}, {2, 0}, {3, 1}}));
  EXPECT_EQ(trace[1].teams, (std::map<int, int>{{0, 1}, {1, 0}, {2, 1}, {3, 0}}));
  EXPECT_EQ(trace[2].teams, trace[0].teams);
  EXPECT_TRUE(trace[3].teams.empty());
}

TEST(OrcaRoom, InvalidOrPartialTeamsAreIgnoredForTheRoomOnce)
{
  for (const auto& bad : std::vector<picojson::value>{picojson::value(), picojson::value(-1.0),
           picojson::value(2.0), picojson::value(0.5), picojson::value(true), picojson::value("0")})
  {
    picojson::value good;
    picojson::parse(good, CoPageWelcome(false, "casual", true));
    for (auto& p : good.get<picojson::object>()["participants"].get<picojson::array>())
      p.get<picojson::object>()["team"] = picojson::value(p.get("slot").get<double>());
    auto invalid = good;
    auto& p = invalid.get<picojson::object>()["participants"].get<picojson::array>().back().get<picojson::object>();
    if (bad.is<picojson::null>())
      p.erase("team");
    else
      p["team"] = bad;
    const auto trace = TraceRoomForTest(TraceOptions(false), {
        {RK::Message, 0, invalid.serialize()}, {RK::Message, 1, invalid.serialize()},
        {RK::Message, 2, good.serialize()}});
    size_t lines = 0;
    for (const auto& step : trace)
    {
      EXPECT_TRUE(step.teams.empty());
      const picojson::value mirror(step.mirror);
      for (const auto& participant : mirror.get("participants").get<picojson::array>())
        EXPECT_EQ(participant.get<picojson::object>().count("team"), 0u);
      lines += std::count(step.notices.begin(), step.notices.end(),
                          "Room: invalid or incomplete teams; ignoring teams for this room");
    }
    EXPECT_EQ(lines, 1u);
  }
}

TEST(OrcaRoom, QueueDoublesTicketSeatsIncludeAnAddBeforeWelcome)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Tick, 0},
      {RK::PageEvent, 1, R"({"slug":"co-page","cmd":"local-add"})"},
      {RK::Message, 2, CoPageWelcome(false, "casual", true)},
      {RK::Message, 3, CoPageWelcome(false, "casual", true, true)},
      {RK::PageEvent, 4, R"({"slug":"co-page","cmd":"local-remove","id":"local:me:1"})"},
      {RK::Message, 5, CoPageWelcome(false, "casual", true)}});
  EXPECT_EQ(trace[0].ticket_local_seats, 1);
  for (const int step : {1, 2, 3})
    EXPECT_EQ(trace[step].ticket_local_seats, 2);
  EXPECT_EQ(trace[2].sent, (std::vector<std::string>{R"({"t":"local-add"})"}));
  EXPECT_EQ(trace[4].sent, (std::vector<std::string>{R"({"id":"local:me:1","t":"local-remove"})"}));
  EXPECT_EQ(trace[5].ticket_local_seats, 1);
}

TEST_F(OrcaRoomRuntime, TicketRequestsPublishActualLocalSeatCount)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const std::string code = "dblticketls";
  auto room = Open(code, R"({"queue":"casual","doubles":true,"joining":false,"co_seats":{"me":[2]}})", false);
  ASSERT_TRUE(Wait([&] { return room->Seat() == 0; }));
  ASSERT_TRUE(Wait([&] { return room->CoSeats() == std::vector<int>{2}; }));
  std::string ticket, store, error;
  ASSERT_TRUE(room->FreshTicket(true, &ticket, &store, &error)) << error;
  const auto seats = Request("/state/" + code).get("ticket_local_seats");
  ASSERT_TRUE(seats.is<picojson::array>());
  ASSERT_EQ(seats.get<picojson::array>().size(), 2u);
  ASSERT_TRUE(seats.get<picojson::array>()[0].is<double>());
  ASSERT_TRUE(seats.get<picojson::array>()[1].is<double>());
  EXPECT_EQ(seats.get<picojson::array>()[0].get<double>(), 1);
  EXPECT_EQ(seats.get<picojson::array>()[1].get<double>(), 2);
  const auto present = Request("/state/" + code).get("ticket_has_local_seats");
  ASSERT_TRUE(present.is<picojson::array>());
  ASSERT_EQ(present.get<picojson::array>().size(), 2u);
  EXPECT_TRUE(present.get<picojson::array>()[0].get<bool>());
  EXPECT_TRUE(present.get<picojson::array>()[1].get<bool>());
}

TEST(OrcaRoom, CasualDoublesDoesNotSendSinglesResults)
{
  picojson::value welcome;
  ASSERT_TRUE(picojson::parse(welcome, QueueRoster("welcome", "casual", "false", false)).empty());
  auto& room = welcome.get<picojson::object>()["room"].get<picojson::object>();
  room["doubles"] = picojson::value(true);
  room["size"] = picojson::value(4.0);
  auto start = welcome;
  start.get<picojson::object>()["t"] = picojson::value("start");
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, welcome.serialize()}, {RK::Message, 1, start.serialize()},
      {RK::GameStart, 2, "", 120}, {RK::ReportGame, 3, "1", 120},
      {RK::Tick, 5}});
  for (const auto& step : trace)
    for (const auto& wire : step.sent)
    {
      picojson::value message;
      ASSERT_TRUE(picojson::parse(message, wire).empty());
      EXPECT_NE(message.get("t").to_str(), "begin");
      EXPECT_NE(message.get("t").to_str(), "game-start");
      EXPECT_NE(message.get("t").to_str(), "game-report");
      EXPECT_NE(message.get("t").to_str(), "finish");
    }
}

TEST(OrcaRoom, SentLocalAddCountsInTicketsUntilTheRoomAnswers)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  const ScopedEnv enabled("ORCA_COSEATS", "1");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, CoPageWelcome(false, "casual", true)},
      {RK::PageEvent, 1, R"({"slug":"co-page","cmd":"local-add"})"},
      {RK::AppCaps, 2, ""}, {RK::Tick, 3}});
  EXPECT_EQ(trace[0].ticket_local_seats, 1);
  for (const int step : {1, 2, 3})
    EXPECT_EQ(trace[step].ticket_local_seats, 2);
}

namespace
{
std::string RankedDoublesRoster(const char* kind, int mine = 0)
{
  picojson::array players, participants;
  for (int slot = 0; slot < 4; ++slot)
  {
    const auto id = fmt::format("p{}", slot);
    players.emplace_back(picojson::object{{"id", picojson::value(id)},
                                        {"name", picojson::value(id)}});
    participants.emplace_back(picojson::object{{"id", picojson::value(id)},
        {"connectionId", picojson::value(id)}, {"slot", picojson::value(double(slot))},
        {"localIndex", picojson::value(0.0)}, {"team", picojson::value(double(slot % 2))}});
  }
  return picojson::value(picojson::object{{"t", picojson::value(kind)},
      {"lifecycle", picojson::value("game")},
      {"me", picojson::value(fmt::format("p{}", mine))}, {"host", picojson::value("p0")},
      {"revision", picojson::value(1.0)}, {"matchId", picojson::value("doubles-set")},
      {"room", picojson::value(picojson::object{{"queue", picojson::value("ranked")},
          {"doubles", picojson::value(true)}, {"size", picojson::value(4.0)},
          {"mode", picojson::value("orca-rsbe01-2v2-ranked")}})},
      {"players", picojson::value(players)}, {"participants", picojson::value(participants)}}).serialize();
}
}

TEST(OrcaRoomRankedDoubles, FourSingleSeatAccountsAreAdmittedAndInvalidMapsRefuseOnce)
{
  const auto valid = RankedDoublesRoster("welcome");
  for (int bad = -1; bad < 8; ++bad)
  {
    picojson::value value;
    ASSERT_TRUE(picojson::parse(value, valid).empty());
    auto& o = value.get<picojson::object>();
    auto& ps = o["participants"].get<picojson::array>();
    if (bad == 0) ps[3].get<picojson::object>().erase("team");
    if (bad == 1) ps[3].get<picojson::object>()["team"] = picojson::value(0.0);
    if (bad == 2) ps[3].get<picojson::object>()["team"] = picojson::value(0.5);
    if (bad == 3) ps[3].get<picojson::object>()["slot"] = picojson::value(2.0);
    if (bad == 4) ps[3].get<picojson::object>()["connectionId"] = picojson::value("p2");
    if (bad == 5) ps[3].get<picojson::object>()["localIndex"] = picojson::value(1.0);
    if (bad == 6) o["room"].get<picojson::object>()["size"] = picojson::value(3.0);
    if (bad == 7) ps[3].get<picojson::object>()["id"] = picojson::value("p2");
    bool doubles = false;
    const auto trace = TraceRoomForTest(TraceOptions(false), {
        {RK::Message, 0, value.serialize()}, {RK::Message, 1, value.serialize()}},
        [&](YouGameRoom& room) { doubles = room.Doubles(); });
    SCOPED_TRACE(bad);
    if (bad < 0)
    {
      EXPECT_TRUE(doubles);
      EXPECT_NE(trace.back().state, RoomState::Ended);
      EXPECT_EQ(trace.back().teams.size(), 4u);
    }
    else
    {
      EXPECT_EQ(trace.back().state, RoomState::Ended);
      EXPECT_EQ(trace.back().status,
          "Room: ranked doubles requires four single-seat players and two complete teams");
      EXPECT_EQ(trace[0].notices.size() + trace[1].notices.size(), 1u);
    }
  }
}

TEST(OrcaRoomRankedDoubles, EachSeatReportsGamesBeforeTheTeamFinalAndSeesItsTeamScore)
{
  for (int mine = 0; mine < 4; ++mine)
  {
    int step = 0;
    const auto trace = TraceRoomForTest(TraceOptions(mine != 0), {
        {RK::Message, 0, RankedDoublesRoster("welcome", mine)},
        {RK::Message, 1, RankedDoublesRoster("start", mine)}, {RK::Tick, 2},
        {RK::Message, 3, R"({"t":"game-result","matchId":"doubles-set","id":"g120","ranking":["p1","p3","p0","p2"],"detail":{"tw":1}})"},
        {RK::Tick, 4},
        {RK::Message, 5, R"({"t":"game-result","matchId":"doubles-set","id":"g360","ranking":["p1","p3","p0","p2"],"detail":{"tw":1}})"},
        {RK::Message, 6, R"({"t":"result","matchId":"doubles-set","ranking":["p1","p3","p0","p2"],"detail":{"tw":1},"ratings":{"p0":{"before":1000,"after":980},"p1":{"before":1001,"after":1021},"p2":{"before":1002,"after":982},"p3":{"before":1003,"after":1023}}})"}},
        [&](YouGameRoom& room) {
          if (step == 1 || step == 3)
          {
            GameReport report;
            report.kind = GameReport::Kind::Win;
            report.winner_seat = 1;
            report.start = step == 1 ? 120 : 360;
            report.detail = R"({"f":700,"st":2,"c":[0,3,2,1],"s":[0,1,0,0],"to":0,"tw":1})";
            room.ReportGameStart(report.start);
            room.ReportGame(report);
          }
          ++step;
        });
    std::vector<std::string> kinds;
    for (const auto& frame : trace)
    {
      for (const auto& wire : frame.sent)
      {
        picojson::value message;
        ASSERT_TRUE(picojson::parse(message, wire).empty());
        const auto kind = message.get("t").to_str();
        if (kind != "game-report" && kind != "finish") continue;
        kinds.push_back(kind);
        ASSERT_TRUE(message.get("ranking").is<picojson::array>());
        EXPECT_EQ(message.get("ranking").serialize(), R"(["p1","p3","p0","p2"])");
        ASSERT_TRUE(message.get("detail").get("tw").is<double>());
        EXPECT_EQ(message.get("detail").get("tw").get<double>(), 1);
      }
    }
    EXPECT_EQ(kinds, (std::vector<std::string>{"game-report", "game-report", "finish"}));
    ASSERT_FALSE(trace.back().results.empty());
    const auto& result = trace.back().results.back();
    EXPECT_NE(result.find(mine % 2 ? "\"out\":\"won\"" : "\"out\":\"lost\""), std::string::npos);
    EXPECT_NE(result.find(mine % 2 ? "\"score\":[2,0]" : "\"score\":[0,2]"), std::string::npos);
    EXPECT_NE(result.find(fmt::format("\"r\":[{},{}]", 1000 + mine,
                                     (mine % 2 ? 1020 : 980) + mine)), std::string::npos);
  }
}

TEST(OrcaRoomRankedDoubles, HostBeginsOnlyAfterTheFourSeatGameIsPlugged)
{
  picojson::value welcome;
  ASSERT_TRUE(picojson::parse(welcome, RankedDoublesRoster("welcome")).empty());
  welcome.get<picojson::object>().erase("matchId");
  int step = 0;
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, welcome.serialize()}, {RK::Tick, 1}, {RK::Tick, 2}},
      [&](YouGameRoom& room) { if (step++ == 1) room.SetOpponentPlugged(true); });
  for (const auto& wire : trace[1].sent)
    EXPECT_EQ(wire.find("\"t\":\"begin\""), std::string::npos);
  bool began = false;
  for (const auto& wire : trace[2].sent)
    began |= wire.find("\"t\":\"begin\"") != std::string::npos;
  EXPECT_TRUE(began);
}

TEST(OrcaRoomRankedDoubles, RefusedBeginRetriesOnDeadlineOrRosterAndStopsAtFive)
{
  picojson::value welcome;
  ASSERT_TRUE(picojson::parse(welcome, RankedDoublesRoster("welcome")).empty());
  welcome.get<picojson::object>().erase("matchId");
  auto roster = welcome;
  roster.get<picojson::object>()["t"] = picojson::value("lobby");
  const std::string refusal = R"({"t":"command-error","action":"begin","error":"try again"})";
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, welcome.serialize()}, {RK::Tick, 1},
      {RK::Message, 2, refusal}, {RK::Tick, 10001}, {RK::Tick, 10002},
      {RK::Message, 10003, refusal}, {RK::Message, 10004, roster.serialize()},
      {RK::Tick, 10005}, {RK::Message, 10006, refusal}, {RK::Tick, 20006},
      {RK::Message, 20007, refusal}, {RK::Tick, 30007},
      {RK::Message, 30008, refusal}, {RK::Tick, 40008},
      {RK::Message, 40009, roster.serialize()}, {RK::Tick, 40010}},
      [](YouGameRoom& room) { room.SetOpponentPlugged(true); });
  for (size_t i = 0; i < trace.size(); ++i)
  {
    SCOPED_TRACE(i);
    const auto begins = std::ranges::count_if(trace[i].sent, [](const auto& wire) {
      return wire.find("\"t\":\"begin\"") != std::string::npos;
    });
    EXPECT_EQ(begins, i == 1 || i == 4 || i == 7 || i == 9 || i == 11 ? 1 : 0);
  }
}

TEST(OrcaRoomRankedDoubles, ALeaverLosesItsWholeTeamIncludingWhenItIsOurPartner)
{
  for (const int leaver : {0, 1, 2})
  {
    const int mine = leaver == 0 ? 1 : 0;
    auto leave = picojson::value{};
    ASSERT_TRUE(picojson::parse(leave, RankedDoublesRoster("leave", mine)).empty());
    auto& message = leave.get<picojson::object>();
    message["player"] = picojson::value(picojson::object{{"id", picojson::value(fmt::format("p{}", leaver))}});
    auto& participants = message["participants"].get<picojson::array>();
    participants.erase(participants.begin() + leaver);
    const auto trace = TraceRoomForTest(TraceOptions(mine != 0), {
        {RK::Message, 0, RankedDoublesRoster("welcome", mine)},
        {RK::Message, 1, RankedDoublesRoster("start", mine)}, {RK::GameStart, 2, "", 120},
        {RK::Tick, 3}, {RK::Message, 4, leave.serialize()}});
    bool finished = false;
    for (const auto& wire : trace.back().sent)
    {
      picojson::value final;
      ASSERT_TRUE(picojson::parse(final, wire).empty());
      if (final.get("t").to_str() != "finish") continue;
      finished = true;
      ASSERT_TRUE(final.get("detail").get("tw").is<double>());
      EXPECT_EQ(final.get("detail").get("tw").get<double>(), 1 - leaver % 2);
      EXPECT_EQ(final.get("ranking").serialize(), leaver == 1 ?
          R"(["p0","p2","p1","p3"])" : R"(["p1","p3","p0","p2"])");
    }
    EXPECT_TRUE(finished);
    EXPECT_EQ(trace.back().teams.size(), 4u);
  }
}

TEST(OrcaRoomRankedDoubles, NativeJointWinnersMustMatchExactlyOneAssignedPair)
{
  using Result = Orca::Engine::GameResult;
  for (int first = 0; first < 4; ++first)
    for (int second = first + 1; second < 4; ++second)
      for (int team = 0; team < 2; ++team)
      {
        Result result;
        result.kind = Result::Kind::Win;
        result.players = 4;
        result.winner_port = first;
        result.winning_ports = (1u << first) | (1u << second);
        result.characters = {10, 11, 12, 13};
        result.stocks = {0, 1, 2, 3};
        result.start = 120;
        std::map<int, int> teams;
        for (int slot = 0; slot < 4; ++slot)
          teams[slot] = slot == first || slot == second ? team : 1 - team;
        const auto report = FromGameResult(result, teams);
        EXPECT_EQ(report.kind, GameReport::Kind::Win);
        EXPECT_EQ(report.winner_seat, first);
        picojson::value detail;
        ASSERT_TRUE(picojson::parse(detail, report.detail).empty());
        EXPECT_EQ(detail.get("tw").get<double>(), team);
        EXPECT_EQ(detail.get("c").serialize(), "[10,11,12,13]");
        EXPECT_EQ(detail.get("s").serialize(), "[0,1,2,3]");
        EXPECT_TRUE(detail.get("lg").is<picojson::null>());
        // A native winner pair that differs from the room's partition is never a win.
        result.winning_ports ^= 15;
        EXPECT_EQ(FromGameResult(result, teams).why, "teams");
        result.winning_ports ^= 15;
        teams.erase(3);
        EXPECT_EQ(FromGameResult(result, teams).why, "teams");
      }
}

TEST(OrcaRoomRankedDoubles, DrawsVoidsAndTwoPortResultsDoNotClaimATeamWin)
{
  using Result = Orca::Engine::GameResult;
  const std::map<int, int> teams{{0, 0}, {1, 1}, {2, 0}, {3, 1}};
  Result result;
  result.kind = Result::Kind::Draw;
  result.players = 4;
  EXPECT_EQ(FromGameResult(result).why, "ports");
  auto report = FromGameResult(result, teams);
  EXPECT_EQ(report.kind, GameReport::Kind::Draw);
  EXPECT_EQ(report.detail.find("\"tw\""), std::string::npos);
  result.players = 2;
  EXPECT_EQ(FromGameResult(result, teams).why, "ports");
  // The ordinary singles report keeps its existing shape and SetBlock metadata.
  result.kind = Result::Kind::Win;
  result.winner_port = 1;
  result.from_set_block = true;
  result.set_done = 1;
  result.set_winner = 1;
  report = FromGameResult(result);
  EXPECT_EQ(report.kind, GameReport::Kind::Win);
  EXPECT_EQ(report.detail, result.DetailJson());
  EXPECT_TRUE(report.from_set_block);
  EXPECT_EQ(report.set_done, 1);
  result.kind = Result::Kind::Void;
  result.why = "nocontest";
  EXPECT_EQ(FromGameResult(result, teams).why, "nocontest");
}

TEST(OrcaRoomRankedDoubles, ALeaveBeforeTheFirstGameIsVoidWithoutAWinningTeam)
{
  picojson::value leave;
  ASSERT_TRUE(picojson::parse(leave, RankedDoublesRoster("leave")).empty());
  auto& o = leave.get<picojson::object>();
  o["player"] = picojson::value(picojson::object{{"id", picojson::value("p2")}});
  auto& participants = o["participants"].get<picojson::array>();
  participants.erase(participants.begin() + 2);
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, RankedDoublesRoster("welcome")},
      {RK::Message, 1, RankedDoublesRoster("start")}, {RK::Message, 2, leave.serialize()}});
  bool finished = false;
  for (const auto& wire : trace.back().sent)
  {
    picojson::value message;
    ASSERT_TRUE(picojson::parse(message, wire).empty());
    if (message.get("t").to_str() != "finish") continue;
    finished = true;
    ASSERT_TRUE(message.get("void").is<bool>());
    EXPECT_TRUE(message.get("void").get<bool>());
    EXPECT_TRUE(message.get("detail").is<picojson::null>());
  }
  EXPECT_TRUE(finished);
}

TEST(OrcaRoomRankedDoubles, ADesyncFinalIsVoidAndKeepsItsReasonOnTheVerdict)
{
  int step = 0;
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, RankedDoublesRoster("welcome")},
      {RK::Message, 1, RankedDoublesRoster("start")}, {RK::Tick, 2},
      {RK::Message, 3, R"({"t":"result","matchId":"doubles-set","void":true})"}},
      [&](YouGameRoom& room) { if (step++ == 1) room.ReportDesync(); });
  bool finished = false;
  for (const auto& wire : trace[2].sent)
  {
    picojson::value message;
    ASSERT_TRUE(picojson::parse(message, wire).empty());
    if (message.get("t").to_str() != "finish") continue;
    finished = true;
    ASSERT_TRUE(message.get("void").is<bool>());
    EXPECT_TRUE(message.get("void").get<bool>());
    EXPECT_TRUE(message.get("detail").is<picojson::null>());
  }
  EXPECT_TRUE(finished);
  ASSERT_FALSE(trace.back().results.empty());
  EXPECT_NE(trace.back().results.back().find("\"why\":\"desync\""), std::string::npos);
}


namespace
{
std::string RankedArrival(const char* kind, int count, bool nested = false)
{
  picojson::value value;
  picojson::parse(value, RankedDoublesRoster(kind));
  auto& m = value.get<picojson::object>();
  if (std::string_view(kind) != "start")
    m.erase("matchId");
  m["participants"].get<picojson::array>().resize(count);
  m["players"].get<picojson::array>().resize(count);
  if (nested)
  {
    auto& room = m["room"].get<picojson::object>();
    for (const char* field : {"participants", "lifecycle", "revision", "matchId"})
      if (m.count(field))
      {
        room[field] = m.at(field);
        m.erase(field);
      }
  }
  return value.serialize();
}
}

TEST(OrcaRoomRankedDoubles, ArrivalsAreAdmittedUntilFourCanBegin)
{
  for (const bool nested : {false, true})
  {
    SCOPED_TRACE(nested);
    int step = 0;
    const auto trace = TraceRoomForTest(TraceOptions(false), {
        {RK::Message, 0, RankedArrival("welcome", 1, nested)}, {RK::Tick, 1},
        {RK::Message, 2, RankedArrival("join", 2, nested)}, {RK::Tick, 3},
        {RK::Message, 4, RankedArrival("join", 3, nested)}, {RK::Tick, 5},
        {RK::Message, 6, RankedArrival("join", 4, nested)}, {RK::Tick, 7},
        {RK::Message, 8, RankedArrival("start", 4, nested)}},
        [&](YouGameRoom& room) { if (step++ == 0) room.SetOpponentPlugged(true); });
    for (size_t i = 0; i < trace.size(); ++i)
    {
      EXPECT_NE(trace[i].state, RoomState::Ended) << i;
      EXPECT_EQ(std::ranges::any_of(trace[i].sent, [](const auto& wire) {
        return wire.find("\"t\":\"begin\"") != std::string::npos;
      }), i == 7) << i;
    }
    EXPECT_EQ(picojson::value(trace.back().mirror).get("matchId").to_str(), "doubles-set");
    EXPECT_EQ(trace.back().teams.size(), 4u);
  }
}

TEST(OrcaRoomRankedDoubles, InvalidArrivalsRefuseOnceAndSeenSeatsKeepTheirTeam)
{
  for (int bad = 0; bad < 7; ++bad)
  {
    SCOPED_TRACE(bad);
    picojson::value value;
    picojson::parse(value, RankedArrival("join", 3));
    auto& ps = value.get<picojson::object>()["participants"].get<picojson::array>();
    if (bad == 0) ps[1].get<picojson::object>()["team"] = picojson::value(0.0);
    if (bad == 1) ps[0].get<picojson::object>()["team"] = picojson::value(1.0);
    if (bad == 2) ps[2].get<picojson::object>()["localIndex"] = picojson::value(1.0);
    if (bad == 3) ps[2].get<picojson::object>()["slot"] = picojson::value(1.0);
    if (bad == 4) ps[2].get<picojson::object>()["id"] = picojson::value("p1");
    if (bad == 5) ps[2].get<picojson::object>()["connectionId"] = picojson::value("p1");
    if (bad == 6) ps[2].get<picojson::object>()["id"] = picojson::value(std::string(129, 'x'));
    const auto trace = TraceRoomForTest(TraceOptions(false), {
        {RK::Message, 0, RankedArrival("welcome", 2)},
        // Removing a seat must not forget its earlier team.
        {RK::Message, 1, RankedArrival("lobby", 1)},
        {RK::Message, 2, value.serialize()}, {RK::Message, 3, value.serialize()}});
    EXPECT_NE(trace[0].state, RoomState::Ended);
    EXPECT_NE(trace[1].state, RoomState::Ended);
    EXPECT_EQ(trace[2].state, RoomState::Ended);
    EXPECT_EQ(trace[2].error_code, "room_mismatch");
    size_t refusals = 0;
    for (const auto& step : trace)
      refusals += std::count(step.notices.begin(), step.notices.end(),
          "Room: ranked doubles requires four single-seat players and two complete teams");
    EXPECT_EQ(refusals, 1u);
  }
}

TEST(OrcaRoomRankedDoubles, PartialStartCannotFreezeAMatch)
{
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, RankedArrival("welcome", 3)},
      {RK::Message, 1, RankedArrival("start", 3)}});
  EXPECT_NE(trace[0].state, RoomState::Ended);
  EXPECT_EQ(trace[1].state, RoomState::Ended);
  EXPECT_TRUE(picojson::value(trace[1].mirror).get("matchId").is<picojson::null>());
}

TEST(OrcaRoomRankedDoubles, LocalAddIsRefusedWithRankedReasonBeforeSending)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  for (const bool early : {false, true})
  {
    SCOPED_TRACE(early);
    std::vector<RoomTestStep> steps;
    if (early)
      steps.push_back({RK::PageEvent, 0, CO_PAGE_ADD});
    steps.push_back({RK::Message, 1, RankedArrival("welcome", 1)});
    if (!early)
      steps.push_back({RK::PageEvent, 2, CO_PAGE_ADD});
    steps.push_back({RK::Tick, 3});
    const auto trace = TraceRoomForTest(TraceOptions(false), steps);
    size_t refusals = 0;
    for (const auto& frame : trace)
    {
      EXPECT_NE(frame.state, RoomState::Ended);
      for (const auto& wire : frame.sent)
        EXPECT_EQ(wire.find("local-add"), std::string::npos);
      refusals += std::count(frame.notices.begin(), frame.notices.end(),
          "Room: Ranked doubles is one player per computer (ranked)");
    }
    EXPECT_EQ(refusals, 1u);
    EXPECT_TRUE(trace.back().co_seats.empty());
  }
}

TEST_F(OrcaRoomRuntime, RankedDoublesTicketsOmitLocalSeatsAtMintAndRefresh)
{
  for (const bool explicit_mode : {false, true})
  {
    SCOPED_TRACE(explicit_mode);
    const std::string code = explicit_mode ? "rankedlsmode" : "rankedlsroom";
    Request("/configure/" + code,
        R"({"queue":"ranked","doubles":true,"joining":false,"peers":3,"teams":true})");
    auto options = TraceOptions(false);
    options.room_code = code;
    options.mode = explicit_mode ? "orca-rsbe01-2v2-ranked" : "orca-rsbe01";
    auto room = std::make_unique<YouGameRoom>(options);
    ASSERT_TRUE(Wait([&] { return room->Doubles(); }));
    std::string ticket, store, error;
    ASSERT_TRUE(room->FreshTicket(true, &ticket, &store, &error)) << error;
    const auto seats = Request("/state/" + code).get("ticket_local_seats");
    ASSERT_TRUE(seats.is<picojson::array>());
    ASSERT_EQ(seats.get<picojson::array>().size(), 2u);
    if (explicit_mode)
      EXPECT_TRUE(seats.get<picojson::array>()[0].is<picojson::null>());
    else
      EXPECT_EQ(seats.get<picojson::array>()[0].get<double>(), 1);
    EXPECT_TRUE(seats.get<picojson::array>()[1].is<picojson::null>());
    const auto present = Request("/state/" + code).get("ticket_has_local_seats");
    ASSERT_TRUE(present.is<picojson::array>());
    ASSERT_EQ(present.get<picojson::array>().size(), 2u);
    EXPECT_EQ(present.get<picojson::array>()[0].get<bool>(), !explicit_mode);
    EXPECT_FALSE(present.get<picojson::array>()[1].get<bool>());
  }
}

TEST(OrcaRoomRankedDoubles, TicketSeatsAreAbsentForRankedRequestsAndKnownRooms)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv game("YOUGAME_GAME", "co-page");
  Orca::Status::SetAppCaps("coseats");
  const Common::ScopeGuard cleanup([] { Orca::Status::SetAppCaps(""); });
  for (const bool explicit_mode : {false, true})
  {
    auto options = TraceOptions(false);
    if (explicit_mode)
      options.mode = "orca-rsbe01-2v2-ranked";
    const auto trace = TraceRoomForTest(options, {
        {RK::PageEvent, 0, CO_PAGE_ADD},
        {RK::Message, 1, RankedArrival("welcome", 1)}});
    if (explicit_mode)
      EXPECT_FALSE(trace[0].ticket_local_seats);
    else
      EXPECT_EQ(trace[0].ticket_local_seats, 2);
    EXPECT_FALSE(trace[1].ticket_local_seats);
  }
}

TEST(OrcaRoomRankedDoubles, ARoomNoticeWithoutARosterKeepsThePartialSeats)
{
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, RankedArrival("welcome", 2)},
      {RK::Message, 1, R"({"t":"away","player":{"id":"p1","away":true},"players":[{"id":"p0"},{"id":"p1","away":true}],"host":"p0"})"},
      {RK::Message, 2, RankedArrival("join", 3)}});
  for (const auto& frame : trace)
    EXPECT_NE(frame.state, RoomState::Ended);
  EXPECT_EQ(trace[1].teams.size(), 2u);
  EXPECT_EQ(trace[2].teams.size(), 3u);
}

TEST(OrcaRoomRankedDoubles, ASeatFirstSeenAtStartKeepsItsTeamForTheNextStart)
{
  picojson::value changed;
  picojson::parse(changed, RankedArrival("start", 4));
  auto& ps = changed.get<picojson::object>()["participants"].get<picojson::array>();
  ps[2].get<picojson::object>()["team"] = picojson::value(1.0);
  ps[3].get<picojson::object>()["team"] = picojson::value(0.0);
  const auto trace = TraceRoomForTest(TraceOptions(false), {
      {RK::Message, 0, RankedArrival("welcome", 2)},
      {RK::Message, 1, RankedArrival("start", 4)},
      {RK::Message, 2, changed.serialize()}});
  EXPECT_NE(trace[1].state, RoomState::Ended);
  EXPECT_EQ(trace[2].state, RoomState::Ended);
  EXPECT_EQ(trace[2].error_code, "room_mismatch");
}
