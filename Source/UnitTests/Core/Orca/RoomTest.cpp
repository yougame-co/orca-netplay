// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Status.h"

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
  using Clock = std::chrono::steady_clock;
  const Clock::time_point t0 = Clock::now();
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
  first->TestDropConnection();
  for (int i = 0; i < 500 && first->GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(first->ErrorCode(), "network") << first->Status();
  first.reset();
  friend_room->Leave();
  YouGameRoom again(options);
  for (int i = 0; i < 1000 && again.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_PLAYER", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  ASSERT_EQ(again.GetState(), RoomState::Waiting) << again.Status() << " / " << again.ErrorCode();
  EXPECT_EQ(again.Seat(), 0);
  EXPECT_EQ(again.Code(), options.room_code);
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
