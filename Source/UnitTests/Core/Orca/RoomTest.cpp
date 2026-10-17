// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/LogManager.h"
#include "Common/WebSocket.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Games/Generic/GenericModule.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/RoomResilience.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/UX.h"
#include "Core/Rollback/OnlineMatch.h"
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
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 409)"), "room_full");
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 0)"), "network");
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
           {0, "server-silent"}})
  {
    const auto trace = TraceRoomForTest(TraceOptions(), {
        {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
        {RK::DirectInput, 1000, "peer"}, {RK::SocketClose, 1000, reason, code},
        {RK::PollReconnect, 1499}, {RK::PollReconnect, 1500},
        {RK::Message, 2000, RosterMessage("welcome")},
    });
    EXPECT_TRUE(trace[3].reconnect_started);
    EXPECT_EQ(trace[4].reconnect_attempts, 0u);
    EXPECT_EQ(trace[5].reconnect_attempts, 1u);
    EXPECT_FALSE(trace[6].reconnecting);
    EXPECT_TRUE(trace[6].events.empty());
    ASSERT_FALSE(trace[5].notices.empty());
    EXPECT_NE(trace[5].notices.back().find("close-code=" + std::to_string(code)), std::string::npos);
    EXPECT_NE(trace[5].notices.back().find("close-reason=" + reason), std::string::npos);
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

TEST(OrcaRoomResilience, ReconnectRetriesNeverExtendTheHoldAndLogTheFinalOutcomeOnce)
{
  const auto trace = TraceRoomForTest(TraceOptions(), {
      {RK::Message, 0, RosterMessage("welcome")}, {RK::Message, 0, PeerHello()},
      {RK::SocketClose, 1000, "WebSocket closed by the server", 1013},
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
  std::string url;
  std::vector<std::unique_ptr<ScopedEnv>> vars;
};

}  // namespace

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
           {1008, "message-limit"}, {4001, "kicked"}, {4003, "locked"}})
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
  EXPECT_EQ(room->ErrorCode(), "reconnect-limit");
  room->Leave();  // final teardown must not make a new connection for leave
  EXPECT_EQ(Connections(code), 7);
  EXPECT_FALSE(Sent(code, "leave"));
}

TEST_F(OrcaRoomRuntime, FirstUpgradeRefusalsMakeOnlyOneRequest)
{
  for (const auto& [code, body] : std::vector<std::pair<std::string, std::string>>{
           {"fullroom", "Room is full"}, {"complete", "This match is complete"},
           {"seatbusy", "seat in use"}, {"unknown4", "unclassified"}})
  {
    auto room = Open(code, fmt::format(R"({{"refuse":"{}"}})", body));
    ASSERT_TRUE(Wait([&] { return room->GetState() == RoomState::Ended; }, 1s));
    if (body == "This match is complete")
      EXPECT_TRUE(room->ErrorCode() == "match-over" || room->ErrorCode() == "room_full");
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
  Action(code, R"({"ticket_delay":6})");
  room->TestDropConnection();
  ASSERT_TRUE(Wait([&] { return Request("/state/" + code).get("tickets").get<double>() >= 2; }));
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
class OrcaRoomStallRuntime : public OrcaRoomRuntime
{
protected:
  void OpenDirect(const std::string& code, bool joining, const std::string& queue = "private",
                  bool recovery_enabled = true)
  {
    vars.push_back(std::make_unique<ScopedEnv>("ORCA_DIRECT", "1"));
    vars.push_back(std::make_unique<ScopedEnv>("ORCA_DIRECT_LOOPBACK", "1"));
    vars.push_back(std::make_unique<ScopedEnv>("ORCA_DIRECT_STUN", "0"));
    room_code = code;
    local_seat = joining ? 1 : 0;
    room = Open(code, fmt::format(R"({{"direct":true,"joining":{},"queue":"{}"}})", joining, queue),
                joining, queue == "private", recovery_enabled);
    ASSERT_TRUE(Wait([&] { return room->Seat() == local_seat && room->RoundTripMs() >= 0; }));
    DirectOptions options;
    options.me = "peer";
    options.room = code;
    options.ice.stun_host.clear();
    options.loopback = true;
    direct.Start(std::move(options));
    direct.AddPeer("me", local_seat);
    size_t seen = 0;
    ASSERT_TRUE(Wait([&] {
      for (auto& [to, message] : direct.TakeSignals())
        Action(code, fmt::format(R"({{"message":{{"t":"msg","from":"peer","d":{}}}}})",
                                 picojson::value(message).serialize()));
      const auto state = Request("/state/" + code);
      const auto& messages = state.get("messages").get<picojson::array>();
      for (; seen < messages.size(); ++seen)
      {
        const auto& data = messages[seen].get("d");
        if (data.is<picojson::object>() && data.get("k").to_str().starts_with("dl"))
          direct.OnSignal("me", data.get<picojson::object>());
      }
      // Link setup must not leave game inputs in flight when the session is constructed.
      direct.Send(EncodePacket(NeutralPacket(0, 0)));
      room->Receive();
      return room->Direct().transport == 1;
    }, 10s));
    room->TakePeerEvents();
    auto config = RoomSessionConfigForSeat();
    session = std::make_unique<Session>(config, game, *room);
  }
  Packet NeutralPacket(int first, int count)
  {
    Packet packet;
    packet.seat = 1 - local_seat;
    packet.first_frame = first;
    packet.current_frame = first + count;
    packet.pads.resize(count);
    return packet;
  }
  Orca::Net::Config RoomSessionConfigForSeat()
  {
    Orca::Net::Config config;
    config.local_seat = local_seat;
    config.time_sync = false;
    config.fixed_delay = 2;
    config.max_delay = 2;
    config.checksum_every = 1000000;
    return config;
  }

  Step Boundary(RoomClock::time_point deadline, int* polls = nullptr)
  {
    const auto start = RoomClock::now();
    const int recoveries_before = budget.Recoveries();
    const auto result = Rollback::StepSession(*session, game, completed, [] { return Pad{}; }, [&] {
      if (polls)
        ++*polls;
      return room->HostLeftPending() || RoomClock::now() > deadline;
    }, 1ms, 10s, nullptr, nullptr,
    [&](RoomClock::time_point now) {
      ++hold_calls;
      return room->HoldInputSilence(start, now);
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
      Action(room_code, fmt::format(R"({{"message":{}}})", RosterMessage("away", local_seat == 1)));
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
        Action(room_code, fmt::format(R"({{"message":{}}})", RosterMessage("back", local_seat == 1)));
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
    for (int boundary = 0; boundary < 100; ++boundary)
    {
      result = Boundary(begin + duration + 5s, &polls);
      if (polls > 0 || result.kind == StepKind::Ended)
        break;
    }
    restored = true;
    restore.join();
    restored = false;
    if (peer_away)
      EXPECT_TRUE(Wait([&] { return !room->RoomRecovering(RoomClock::now()); }));
    next_input += 16;
    return result;
  }
  std::string room_code;
  int local_seat = 1, completed = -1, next_input = 0, hold_calls = 0;
  std::unique_ptr<YouGameRoom> room;
  DirectLink direct;
  RoomHashGame game;
  std::unique_ptr<Session> session;
  Rollback::OnlineMatch::StallBudget budget;
  std::atomic<bool> restored{false};
  bool last_excused = false;

  void ExpectDarkRecovery(std::chrono::milliseconds duration)
  {
    ASSERT_TRUE(session);
    for (int small = 0; small < 3; ++small)
      ASSERT_EQ(StallFor(100ms).kind, StepKind::Run);
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
    ASSERT_TRUE(Wait([&] { return Connections("logtime4") >= 3; }));
    Action("logtime4", R"({"upgrade_delay":0})");
    timed->Leave();
  }
  std::lock_guard lock(observed->mutex);
  ASSERT_FALSE(observed->lines.empty());
  bool saw_failure = false;
  for (const auto& line : observed->lines)
  {
    EXPECT_EQ(line.find("ticket="), std::string::npos);
    EXPECT_EQ(line.find("loopback-fixture"), std::string::npos);
    saw_failure |= line.find("WebSocket connect failed") != std::string::npos;
  }
  EXPECT_TRUE(saw_failure);
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

TEST_F(OrcaRoomRuntime, ReturningFriendUsesActualHostCurrentMatchKeyframe)
{
  // Core RAM represents the running match. The origin/menu has a different checksum.
  struct HeadlessCore
  {
    HeadlessCore()
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
    ~HeadlessCore()
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
  } core;
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

TEST_F(OrcaRoomStallRuntime, OrdinaryStallsLeaveRecoveryAllowanceForDarkJoiner)
{
  OpenDirect("budgetdark", true);
  ExpectDarkRecovery(15s);
}

TEST_F(OrcaRoomStallRuntime, DarkHostFifteenSecondsKeepsTheSessionAfterAnotherStall)
{
  OpenDirect("budgethost", false);
  ExpectDarkRecovery(15s);
}

TEST_F(OrcaRoomStallRuntime, DarkJoinerSixtySecondsKeepsTheSessionAfterAnotherStall)
{
  OpenDirect("budgetlong", true);
  ExpectDarkRecovery(60s);
}

TEST_F(OrcaRoomStallRuntime, FourthRecoveredOutageInTenMinutesCounts)
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

TEST_F(OrcaRoomStallRuntime, RollbackSplitsOneOutageWithoutSpendingAnotherRecovery)
{
  OpenDirect("budgetsplt", true);
  ASSERT_TRUE(session);
  ASSERT_EQ(StallFor(100ms).kind, StepKind::Run);
  Action(room_code, fmt::format(R"({{"message":{}}})", RosterMessage("away", true)));
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
  Action(room_code, fmt::format(R"({{"message":{}}})", RosterMessage("back", true)));
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

TEST_F(OrcaRoomStallRuntime, RankedStallsKeepTheBudgetAndSilenceLimit)
{
  OpenDirect("budgetrank", true, "ranked");
  ASSERT_TRUE(session);
  EXPECT_FALSE(room->RoomRecovering(RoomClock::now()));
  ASSERT_EQ(StallFor(9s).kind, StepKind::Run);
  EXPECT_EQ(StallFor(7s).kind, StepKind::Ended);
  EXPECT_EQ(budget.Recoveries(), 0);
  EXPECT_GE(budget.Stalled(RoomClock::now()), 15s);
}

TEST_F(OrcaRoomStallRuntime, CasualStallsKeepTheBudgetAndSilenceLimit)
{
  OpenDirect("budgetcasual", true, "casual");
  ASSERT_TRUE(session);
  EXPECT_FALSE(room->RoomRecovering(RoomClock::now()));
  ASSERT_EQ(StallFor(9s).kind, StepKind::Run);
  EXPECT_EQ(StallFor(7s).kind, StepKind::Ended);
  EXPECT_EQ(budget.Recoveries(), 0);
  EXPECT_GE(budget.Stalled(RoomClock::now()), 15s);
}
