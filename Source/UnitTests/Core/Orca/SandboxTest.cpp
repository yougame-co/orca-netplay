// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca in the YouGame app's sandbox (ORCA.md "The sandbox"): the stdio bridge's parsing and routing
// (Orca/Bridge.h), and the switches a sandboxed launch turns on (Orca/Sandbox.h, --embed-adopted).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#ifndef _WIN32
#ifdef __APPLE__
#include <sandbox.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Core/Orca/Bridge.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/PackJail.h"
#include "Core/Orca/Sandbox.h"
#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "DolphinNoGUI/Embed.h"
#include "InputCommon/GCPadStatus.h"

using namespace std::chrono_literals;
namespace Bridge = Orca::Bridge;
namespace Sandbox = Orca::Sandbox;

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

// One variable set for a test, put back as it was afterwards.
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

// Lines Orca wrote to the app, for the request tests.
std::mutex s_written_mutex;
std::vector<std::string> s_written;
void Capture(const std::string& line)
{
  std::lock_guard lock(s_written_mutex);
  s_written.push_back(line);
}
std::vector<std::string> Written()
{
  std::lock_guard lock(s_written_mutex);
  return s_written;
}

// The bridge as each test finds it: on, nothing counted, no writer, no handlers.
class OrcaBridge : public ::testing::Test
{
protected:
  void SetUp() override
  {
    Bridge::ResetForTests();
    Bridge::SetWriter(nullptr);
    std::lock_guard lock(s_written_mutex);
    s_written.clear();
  }
  void TearDown() override
  {
    Bridge::SetWriter(nullptr);
    Bridge::ResetForTests();
  }
  ScopedEnv m_stdio{"YOUGAME_STDIO", "1"};
};

double NowMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A controller snapshot push as the app writes it: port 1 holds `buttons` (wire mask) and its main
// stick's X.
std::string PadLine(int sequence, int buttons, int stick_x)
{
  std::string ports;
  for (int port = 0; port < 4; ++port)
  {
    const bool on = port == 0;
    ports += fmt::format(
        R"({}{{"adapterId":"s-gc","port":{},"seat":{},"connected":{},"type":{},"buttons":{},)"
        R"("axes":[{},128,128,128],"triggers":[0,0],"origin":[128,128,128,128,0,0],)"
        R"("calibrationRevision":0}})",
        port ? "," : "", port, port, on ? "true" : "false", on ? "\"wired\"" : "null",
        on ? buttons : 0, on ? stick_x : 128);
  }
  const double t = NowMs();
  return fmt::format(
      R"(yg {{"push":{{"type":"controllers","snapshot":{{"schema":1,"session":"stdio","sequence":{},)"
      R"("sentAt":{:.3f},"receivedAt":{:.3f},"source":"native","state":"connected","message":"",)"
      R"("owned":true,"suspended":false,"invalidReports":0,"ports":[{}],"pads":[]}}}}}})",
      sequence, t, t, ports);
}

picojson::object Object(const char* type)
{
  picojson::object o;
  o["type"] = picojson::value(type);
  return o;
}

// The id of the newest request written, or 0.
u64 NewestId()
{
  const auto lines = Written();
  if (lines.empty())
    return 0;
  picojson::value v;
  if (!lines.back().starts_with("yg ") || !picojson::parse(v, lines.back().substr(3)).empty() ||
      !v.is<picojson::object>())
  {
    return 0;
  }
  return static_cast<u64>(v.get("id").get<double>());
}
}  // namespace

// ---- Lines ----

TEST(OrcaBridgeLines, SplitsAcrossReadsAndDropsOverlongLinesWhole)
{
  Bridge::LineSplitter split(16);
  EXPECT_TRUE(split.Feed("rect 1 2").empty());
  auto lines = split.Feed(" 3 4\r\nshow\nhi");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "rect 1 2 3 4");
  EXPECT_EQ(lines[1], "show");
  EXPECT_EQ(split.Pending(), 2u);
  // "hi" grows past 16 bytes over two reads: dropped through its newline, and the next line is
  // whole.
  EXPECT_TRUE(split.Feed("0123456789").empty());
  EXPECT_TRUE(split.Feed("abcdefgh").empty());
  EXPECT_EQ(split.Dropped(), 1u);
  EXPECT_EQ(split.Pending(), 0u);
  lines = split.Feed("still the long one\nquit\n");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "quit");
  // Exactly the limit is kept; one more byte is not.
  lines = split.Feed(std::string(16, 'x') + "\n" + std::string(17, 'y') + "\nok\n");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], std::string(16, 'x'));
  EXPECT_EQ(lines[1], "ok");
  EXPECT_EQ(split.Dropped(), 2u);
  // Empty lines stay lines.
  lines = split.Feed("\n\n");
  EXPECT_EQ(lines.size(), 2u);
}

TEST(OrcaBridgeLines, ParsesOnlyTheShapesTheAppSends)
{
  using Kind = Bridge::Inbound::Kind;
  const auto kind = [](const std::string& line) { return Bridge::Parse(line).kind; };
  EXPECT_EQ(kind(R"(yg {"re":3,"ok":true,"result":{"url":"wss://x"}})"), Kind::Reply);
  EXPECT_EQ(Bridge::Parse(R"(yg {"re":3,"ok":false,"error":"no"})").re, 3u);
  EXPECT_EQ(kind(R"(yg {"push":{"type":"controllers","snapshot":{}}})"), Kind::Controllers);
  EXPECT_EQ(kind(R"(yg {"push":{"slug":"brawl","cmd":"leave"}})"), Kind::Push);
  // A page's push that claims to be controllers but names a game is the page's, not the helper's.
  EXPECT_EQ(kind(R"(yg {"push":{"type":"controllers","slug":"brawl"}})"), Kind::Push);

  // Everything else is ignored: not bridge lines, bad ids, missing fields, unknown messages.
  for (const char* line : {"rect 1 2 3 4",
                           "yg",
                           "yg ",
                           "yg{}",
                           "yg {}",
                           "yg []",
                           "yg null",
                           "yg {\"re\":1}",
                           R"(yg {"re":0,"ok":true})",
                           R"(yg {"re":-1,"ok":true})",
                           R"(yg {"re":1.5,"ok":true})",
                           R"(yg {"re":"1","ok":true})",
                           R"(yg {"re":9007199254740992,"ok":true})",
                           R"(yg {"re":1,"ok":"yes"})",
                           R"(yg {"push":1})",
                           R"(yg {"push":[]})",
                           R"(yg {"push":{},"extra":1})",
                           R"(yg {"key":"escape"})",
                           R"(yg {"id":1,"type":"hello"})",
                           R"(yg {"future":{"type":"rumble"}})",
                           "yg {\"re\":1,\"ok\":true",
                           "YG {\"re\":1,\"ok\":true}",
                           R"(yg {"re":1e400,"ok":true})"})
  {
    EXPECT_EQ(kind(line), Kind::Ignored) << line;
  }
  // Nested past the limit, and longer than the limit.
  EXPECT_EQ(kind("yg {\"push\":" + std::string(20, '[') + std::string(20, ']') + "}"),
            Kind::Ignored);
  const std::string filler(Bridge::MAX_INBOUND, 'a');
  EXPECT_EQ(kind(R"(yg {"push":{"slug":"s","cmd":")" + filler + "\"}}"), Kind::Ignored);
}

// ---- Routing ----

TEST_F(OrcaBridge, PassesCommandsOnInOrderAndKeepsBridgeLinesOut)
{
  const auto rest = Bridge::Route({"rect 0 0 640 480", R"(yg {"key":"escape"})", "yg nonsense",
                                   "show", R"(yg {"re":99,"ok":true,"result":{}})", "quit"});
  EXPECT_EQ(rest, (std::vector<std::string>{"rect 0 0 640 480", "show", "quit"}));
  const Bridge::Stats s = Bridge::GetStats();
  EXPECT_EQ(s.ignored, 2u);
  EXPECT_EQ(s.unmatched, 1u);  // nobody asked for 99
  EXPECT_EQ(s.replies, 0u);
}

TEST_F(OrcaBridge, OffWithoutYougameStdio)
{
  ScopedEnv off("YOUGAME_STDIO", nullptr);
  EXPECT_FALSE(Bridge::Enabled());
  int calls = 0;
  const u64 token = Bridge::SetPushHandler([&](const picojson::object&) { ++calls; });
  const auto rest = Bridge::Route({R"(yg {"push":{"slug":"brawl","cmd":"leave"}})", "show"});
  EXPECT_EQ(rest, std::vector<std::string>{"show"});
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(Bridge::GetStats().ignored, 1u);
  Bridge::ClearPushHandler(token);
}

TEST_F(OrcaBridge, LobbyPushesGoToTheCurrentHandlerOnly)
{
  std::vector<std::string> seen;
  const u64 first = Bridge::SetPushHandler(
      [&](const picojson::object& e) { seen.push_back("first " + e.at("cmd").to_str()); });
  Bridge::Route({R"(yg {"push":{"slug":"brawl","cmd":"leave"}})"});
  // A newer room's handler replaces it; the old room clearing its token later changes nothing.
  const u64 second = Bridge::SetPushHandler(
      [&](const picojson::object& e) { seen.push_back("second " + e.at("cmd").to_str()); });
  Bridge::ClearPushHandler(first);
  Bridge::Route({R"(yg {"push":{"slug":"brawl","cmd":"kick","id":"c2"}})"});
  Bridge::ClearPushHandler(second);
  Bridge::Route({R"(yg {"push":{"slug":"brawl","cmd":"leave"}})"});
  EXPECT_EQ(seen, (std::vector<std::string>{"first leave", "second kick"}));
  EXPECT_EQ(Bridge::GetStats().pushes, 2u);
  EXPECT_EQ(Bridge::GetStats().ignored, 1u);  // no handler for the last one
}

TEST_F(OrcaBridge, OnlyTheNewestControllerSnapshotOfAReadIsParsed)
{
  std::vector<double> sequences;
  const u64 token = Bridge::SetControllersHandler([&](const picojson::value& event) {
    sequences.push_back(event.get("snapshot").get("sequence").get<double>());
  });
  const auto rest = Bridge::Route({PadLine(5, 1, 128), "show", PadLine(6, 2, 128),
                                   PadLine(7, 4, 128), R"(yg {"push":{"slug":"s","cmd":"x"}})"});
  EXPECT_EQ(rest, std::vector<std::string>{"show"});
  EXPECT_EQ(sequences, std::vector<double>{7});
  EXPECT_EQ(Bridge::GetStats().coalesced, 2u);
  EXPECT_EQ(Bridge::GetStats().controllers, 1u);
  // One per read: each read's newest arrives.
  Bridge::Route({PadLine(8, 1, 128)});
  Bridge::Route({PadLine(9, 1, 128)});
  EXPECT_EQ(sequences, (std::vector<double>{7, 8, 9}));
  Bridge::ClearControllersHandler(token);
}

TEST_F(OrcaBridge, SnapshotsReachTheControllerCache)
{
  using namespace Orca::UX;
  StartControllerStreamFromStdio();
  EXPECT_FALSE(LatestPad().has_value());
  Bridge::Route({PadLine(1, 0x01, 170)});
  auto pad = LatestPad();
  ASSERT_TRUE(pad.has_value());
  EXPECT_EQ(pad->button & PAD_BUTTON_A, PAD_BUTTON_A);
  EXPECT_EQ(pad->stickX, 170);
  // Three in one read: the newest wins.
  Bridge::Route({PadLine(2, 0x02, 100), PadLine(3, 0x04, 90)});
  pad = LatestPad();
  ASSERT_TRUE(pad.has_value());
  EXPECT_EQ(pad->button & PAD_BUTTON_X, PAD_BUTTON_X);
  EXPECT_EQ(pad->stickX, 90);
  // A malformed snapshot is refused and changes nothing.
  Bridge::Route({R"(yg {"push":{"type":"controllers","snapshot":{"schema":9}}})"});
  EXPECT_EQ(GetControllerTiming().refused, 1u);
  EXPECT_EQ(LatestPad()->stickX, 90);
  StopControllerStream();
  // After the stream stops, snapshots are dropped.
  Bridge::Route({PadLine(4, 0x01, 10)});
  EXPECT_FALSE(LatestPad().has_value());
}

// ---- Requests ----

TEST_F(OrcaBridge, RequestsOnlyWhatOrcaAsksForAndOnlyWithAPipe)
{
  // No writer: refused at once, and online play is off (Online::Enabled).
  EXPECT_FALSE(Bridge::Ready());
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(Bridge::Request(Object("hello"), 5s).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  Bridge::SetWriter(&Capture);
  EXPECT_TRUE(Bridge::Ready());
  {
    ScopedEnv off("YOUGAME_STDIO", nullptr);
    EXPECT_FALSE(Bridge::Ready());
  }
  for (const char* type : {"submitScore", "mpFriend", "controllers", "", "save"})
    EXPECT_FALSE(Bridge::Request(Object(type), 50ms).has_value()) << type;
  picojson::object with_id = Object("hello");
  with_id["id"] = picojson::value(5.0);
  EXPECT_FALSE(Bridge::Request(with_id, 50ms).has_value());
  EXPECT_TRUE(Written().empty());
  // Too long to send.
  picojson::object big = Object("mpRoom");
  big["participants"] = picojson::value(std::string(Bridge::MAX_OUTBOUND, 'p'));
  EXPECT_FALSE(Bridge::Request(big, 50ms).has_value());
  EXPECT_TRUE(Written().empty());
  EXPECT_TRUE(Bridge::AllowedRequest("hello"));
  EXPECT_TRUE(Bridge::AllowedRequest("mpTicket"));
  EXPECT_TRUE(Bridge::AllowedRequest("mpRoom"));
}

TEST_F(OrcaBridge, ARequestGetsItsOwnReply)
{
  Bridge::SetWriter(&Capture);
  std::atomic<bool> done{false};
  // The app: answers each request it sees, a stale reply first.
  std::thread app([&] {
    u64 answered = 0;
    while (!done)
    {
      const u64 id = NewestId();
      if (id && id != answered)
      {
        answered = id;
        Bridge::Route(
            {fmt::format(R"(yg {{"re":{},"ok":true,"result":{{"room":"stale"}}}})", id + 100),
             fmt::format(R"(yg {{"re":{},"ok":true,"result":{{"room":"r{}"}}}})", id, id)});
      }
      std::this_thread::sleep_for(1ms);
    }
  });
  picojson::object ticket = Object("mpTicket");
  ticket["room"] = picojson::value("abcdef12");
  const auto reply = Bridge::Request(ticket, 5s);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->at("ok").get<bool>());
  const u64 id = NewestId();
  EXPECT_EQ(reply->at("result").get("room").to_str(), fmt::format("r{}", id));
  const auto second = Bridge::Request(Object("hello"), 5s);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->at("result").get("room").to_str(), fmt::format("r{}", id + 1));
  done = true;
  app.join();

  const auto lines = Written();
  ASSERT_EQ(lines.size(), 2u);
  picojson::value first;
  ASSERT_TRUE(lines[0].starts_with("yg {"));
  ASSERT_TRUE(picojson::parse(first, lines[0].substr(3)).empty());
  EXPECT_EQ(first.get("type").to_str(), "mpTicket");
  EXPECT_EQ(first.get("room").to_str(), "abcdef12");
  EXPECT_EQ(static_cast<u64>(first.get("id").get<double>()), id);
  EXPECT_EQ(Bridge::GetStats().requests, 2u);
  EXPECT_EQ(Bridge::GetStats().replies, 2u);
  EXPECT_GE(Bridge::GetStats().unmatched, 2u);
}

TEST_F(OrcaBridge, ARequestGivesUpOnTimeoutCancelAndAClosedStdin)
{
  Bridge::SetWriter(&Capture);
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(Bridge::Request(Object("hello"), 100ms).has_value());
  EXPECT_GE(std::chrono::steady_clock::now() - start, 100ms);
  // A reply after giving up is nobody's.
  Bridge::Route({fmt::format(R"(yg {{"re":{},"ok":true,"result":{{}}}})", NewestId())});
  EXPECT_EQ(Bridge::GetStats().unmatched, 1u);

  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(50ms);
    cancel = true;
  });
  start = std::chrono::steady_clock::now();
  EXPECT_FALSE(Bridge::Request(Object("mpRoom"), 10s, [&] { return cancel.load(); }).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  canceller.join();

  std::thread closer([] {
    std::this_thread::sleep_for(50ms);
    Bridge::Close();
  });
  start = std::chrono::steady_clock::now();
  EXPECT_FALSE(Bridge::Request(Object("mpTicket"), 10s).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  closer.join();
  // Closed: later requests fail at once, without writing.
  const size_t written = Written().size();
  EXPECT_FALSE(Bridge::Request(Object("hello"), 10s).has_value());
  EXPECT_EQ(Written().size(), written);
  EXPECT_EQ(Bridge::GetStats().unanswered, 3u);
}

// ---- The sandbox's switches ----

// The stdio bridge's heartbeat (Bridge::Heartbeat, said by Platform::UpdateRunningFlag): due at
// once, then once a period however often the main loop asks; a loop that stalled says it once when
// it comes back, not once per period it missed.
TEST(OrcaBridgeHeartbeat, DueAtOnceThenOncePerPeriod)
{
  EXPECT_EQ(Bridge::HEARTBEAT_LINE, "orca alive");
  EXPECT_EQ(Bridge::HEARTBEAT_PERIOD, 1000ms);
  Bridge::Heartbeat beat;
  const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
  EXPECT_TRUE(beat.Due(t0));
  EXPECT_FALSE(beat.Due(t0));
  EXPECT_FALSE(beat.Due(t0 + Bridge::HEARTBEAT_PERIOD - 1ms));
  EXPECT_TRUE(beat.Due(t0 + Bridge::HEARTBEAT_PERIOD));
  EXPECT_TRUE(beat.Due(t0 + 10s));
  EXPECT_FALSE(beat.Due(t0 + 10s + 500ms));
  EXPECT_TRUE(beat.Due(t0 + 11s));
}

TEST(OrcaSandbox, NetworkModes)
{
  bool known = false;
  EXPECT_EQ(Sandbox::ParseNetMode("", &known), Sandbox::NetMode::All);
  EXPECT_TRUE(known);
  EXPECT_EQ(Sandbox::ParseNetMode("internet", &known), Sandbox::NetMode::Internet);
  EXPECT_TRUE(known);
  EXPECT_EQ(Sandbox::ParseNetMode("internet-lan", &known), Sandbox::NetMode::InternetLan);
  EXPECT_TRUE(known);
  // An unknown word is the strictest mode.
  EXPECT_EQ(Sandbox::ParseNetMode("lan", &known), Sandbox::NetMode::Internet);
  EXPECT_FALSE(known);
  EXPECT_EQ(Sandbox::ParseNetMode("INTERNET", &known), Sandbox::NetMode::Internet);
  EXPECT_FALSE(known);

  EXPECT_TRUE(Sandbox::LoopbackAllowed(Sandbox::NetMode::All));
  EXPECT_FALSE(Sandbox::LoopbackAllowed(Sandbox::NetMode::Internet));
  EXPECT_FALSE(Sandbox::LoopbackAllowed(Sandbox::NetMode::InternetLan));
  EXPECT_TRUE(Sandbox::LanAllowed(Sandbox::NetMode::All));
  EXPECT_FALSE(Sandbox::LanAllowed(Sandbox::NetMode::Internet));
  EXPECT_TRUE(Sandbox::LanAllowed(Sandbox::NetMode::InternetLan));

  ScopedEnv net("ORCA_NET", "internet-lan");
  EXPECT_EQ(Sandbox::NetModeFromEnv(), Sandbox::NetMode::InternetLan);
  EXPECT_TRUE(Sandbox::Jailed());
  {
    ScopedEnv none("ORCA_NET", nullptr);
    EXPECT_EQ(Sandbox::NetModeFromEnv(), Sandbox::NetMode::All);
  }
}

TEST(OrcaSandbox, WhichAddressesEachModeMayUse)
{
  struct Case
  {
    const char* address;
    bool lan, loopback;
  };
  const Case cases[] = {
      {"127.0.0.1", true, true},
      {"127.8.9.10", true, true},
      {"::1", true, true},
      {"::ffff:127.0.0.1", true, true},
      {"10.0.0.5", true, false},
      {"172.16.0.1", true, false},
      {"172.31.255.255", true, false},
      {"172.32.0.1", false, false},
      {"172.15.0.1", false, false},
      {"192.168.1.20", true, false},
      {"192.169.1.20", false, false},
      {"169.254.3.4", true, false},
      {"fe80::1", true, false},
      {"febf::1", true, false},
      {"fec0::1", false, false},
      {"fd12:3456::1", true, false},
      {"fc00::1", true, false},
      {"::ffff:192.168.0.2", true, false},
      {"203.0.113.7", false, false},
      {"8.8.8.8", false, false},
      {"2001:db8::1", false, false},
      {"100.64.0.1", false, false},
      {"host.example", false, false},
      {"", false, false},
  };
  for (const Case& c : cases)
  {
    EXPECT_EQ(Sandbox::LanAddress(c.address), c.lan) << c.address;
    EXPECT_EQ(Sandbox::LoopbackAddress(c.address), c.loopback) << c.address;
    EXPECT_TRUE(Sandbox::AddressAllowed(Sandbox::NetMode::All, c.address)) << c.address;
    EXPECT_EQ(Sandbox::AddressAllowed(Sandbox::NetMode::Internet, c.address), !c.lan) << c.address;
    EXPECT_EQ(Sandbox::AddressAllowed(Sandbox::NetMode::InternetLan, c.address), !c.loopback)
        << c.address;
  }
}

TEST(OrcaSandbox, DirectLinksOfferOnlyCandidatesTheSandboxCanReach)
{
  const std::string description = "a=ice-ufrag:abcd\r\n"
                                  "a=ice-pwd:0123456789abcdef01234567\r\n"
                                  "a=candidate:1 1 UDP 2122317823 192.168.1.20 50000 typ host\r\n"
                                  "a=candidate:2 1 UDP 2122317567 2001:db8::20 50000 typ host\r\n"
                                  "a=candidate:3 1 UDP 2122317311 fe80::20 50000 typ host\r\n"
                                  "a=candidate:4 1 UDP 1686052607 203.0.113.7 61000 typ srflx "
                                  "raddr 0.0.0.0 rport 0\r\n"
                                  "a=candidate:5 1 UDP 16777215 198.51.100.40 3478 typ relay "
                                  "raddr 0.0.0.0 rport 0\r\n"
                                  "a=candidate:9 1 UDP 2122317823 127.0.0.1 50000 typ host\r\n"
                                  "a=end-of-candidates\r\n";
  const auto has = [](const std::string& text, const char* address) {
    return text.find(std::string(" ") + address + " ") != std::string::npos;
  };
  using Orca::Net::DescriptionFor;
  const std::string all = DescriptionFor(description, Sandbox::NetMode::All);
  for (const char* a :
       {"192.168.1.20", "2001:db8::20", "fe80::20", "203.0.113.7", "198.51.100.40", "127.0.0.1"})
  {
    EXPECT_TRUE(has(all, a)) << a;
  }
  const std::string internet = DescriptionFor(description, Sandbox::NetMode::Internet);
  for (const char* a : {"2001:db8::20", "203.0.113.7", "198.51.100.40"})
    EXPECT_TRUE(has(internet, a)) << a;
  for (const char* a : {"192.168.1.20", "fe80::20", "127.0.0.1"})
    EXPECT_FALSE(has(internet, a)) << a;
  const std::string lan = DescriptionFor(description, Sandbox::NetMode::InternetLan);
  for (const char* a : {"192.168.1.20", "2001:db8::20", "fe80::20", "203.0.113.7", "198.51.100.40"})
    EXPECT_TRUE(has(lan, a)) << a;
  EXPECT_FALSE(has(lan, "127.0.0.1"));
  // The ICE credentials stay in every mode.
  for (const std::string* d : {&all, &internet, &lan})
  {
    EXPECT_NE(d->find("a=ice-ufrag:abcd"), std::string::npos);
    EXPECT_NE(d->find("a=end-of-candidates"), std::string::npos);
  }
}

TEST(OrcaSandbox, SessionsLeaveTheAdapterAndTheMacKeyboardToTheApp)
{
  EXPECT_FALSE(Sandbox::AdapterAllowed(true));
  EXPECT_TRUE(Sandbox::AdapterAllowed(false));

  using Os = Sandbox::Os;
  // Only a Mac session on the stdio bridge, where the page sends the keyboard as a pad.
  EXPECT_FALSE(Sandbox::OsKeyboardAllowed(Os::MacOS, true, true));
  EXPECT_TRUE(Sandbox::OsKeyboardAllowed(Os::MacOS, true, false));
  EXPECT_TRUE(Sandbox::OsKeyboardAllowed(Os::MacOS, false, true));
  EXPECT_TRUE(Sandbox::OsKeyboardAllowed(Os::Windows, true, true));
  EXPECT_TRUE(Sandbox::OsKeyboardAllowed(Os::Other, true, true));
}

TEST(OrcaSandbox, EmbedAdoptedNeedsEmbed)
{
  std::string arg0 = "Orca", embed = "--embed", adopted = "--embed-adopted", parent = "--parent",
              handle = "1234", rect = "--rect", r = "10,20,640,480", e = "-e", disc = "disc.iso";
  std::vector<char*> argv{arg0.data(), embed.data(), adopted.data(), parent.data(), handle.data(),
                          rect.data(), r.data(),     e.data(),       disc.data()};
  std::string error;
  const auto options = Embed::TakeArguments(&argv, &error);
  ASSERT_TRUE(options.has_value()) << error;
  EXPECT_TRUE(options->enabled);
  EXPECT_TRUE(options->adopted);
  EXPECT_EQ(options->parent, 1234u);
  EXPECT_EQ(options->rect.w, 640);
  ASSERT_EQ(argv.size(), 3u);  // the regular parser sees only "-e disc.iso"
  EXPECT_STREQ(argv[1], "-e");

  std::vector<char*> plain{arg0.data(), embed.data(), parent.data(), handle.data()};
  const auto without = Embed::TakeArguments(&plain, &error);
  ASSERT_TRUE(without.has_value()) << error;
  EXPECT_FALSE(without->adopted);

  std::vector<char*> alone{arg0.data(), adopted.data()};
  EXPECT_FALSE(Embed::TakeArguments(&alone, &error).has_value());
  EXPECT_NE(error.find("--embed-adopted"), std::string::npos);
}

// The build declares its sandbox mode (Sys/Orca/features.json), which the release scripts copy into
// the manifest entry: the app jails only an Orca that says "sandbox": 1.
TEST(OrcaSandbox, TheBuildDeclaresItsSandboxMode)
{
  std::string path = File::GetSysDirectory() + "Orca/features.json";
  if (!File::Exists(path))
  {
    // The source tree's copy, from this file's own path.
    const std::string here = __FILE__;
    const size_t at = here.rfind("Source/UnitTests/");
    if (at != std::string::npos)
      path = here.substr(0, at) + "Data/Sys/Orca/features.json";
  }
  std::string text;
  if (!File::ReadFileToString(path, text))
    GTEST_SKIP() << "no Sys/Orca/features.json here";
  picojson::value v;
  ASSERT_TRUE(picojson::parse(v, text).empty()) << text;
  ASSERT_TRUE(v.is<picojson::object>());
  ASSERT_TRUE(v.get("sandbox").is<double>());
  EXPECT_EQ(v.get("sandbox").get<double>(), 1.0);
}

#ifndef _WIN32
// What a sandbox refusing a file looks like (EACCES), made with permissions. Skipped as root.
TEST(OrcaSandbox, AFileTheSandboxDidNotGrantIsSaidSo)
{
  if (geteuid() == 0)
    GTEST_SKIP() << "root reads everything";
  const std::string dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const std::string disc = dir + "/Brawl.iso";
  {
    File::IOFile f(disc, "wb");
    ASSERT_TRUE(f.WriteString("not really a disc"));
  }
  EXPECT_FALSE(Sandbox::ReadDenied(disc));
  EXPECT_FALSE(Sandbox::ReadDenied(dir + "/missing.iso"));
  EXPECT_FALSE(Sandbox::WriteDenied(dir));
  ASSERT_EQ(chmod(disc.c_str(), 0), 0);
  EXPECT_TRUE(Sandbox::ReadDenied(disc));
  {
    // Outside the sandbox it's a disc Orca can't read; inside, the app's cue to move it.
    ScopedEnv net("ORCA_NET", nullptr);
    if (!Sandbox::Jailed())
      EXPECT_EQ(Orca::CheckBootFile(disc).code, "disc_unreadable");
  }
  {
    ScopedEnv net("ORCA_NET", "internet");
    const Orca::DiscCheck check = Orca::CheckBootFile(disc);
    EXPECT_FALSE(check.ok);
    EXPECT_EQ(check.code, Sandbox::DISC_ERROR);
    EXPECT_EQ(check.sentence, Sandbox::DISC_SENTENCE);
    // A disc that really is missing is still missing.
    EXPECT_EQ(Orca::CheckBootFile(dir + "/missing.iso").code, "disc_missing");
  }
  ASSERT_EQ(chmod(dir.c_str(), 0500), 0);
  EXPECT_TRUE(Sandbox::WriteDenied(dir));
  chmod(dir.c_str(), 0700);
  chmod(disc.c_str(), 0600);
  // WriteDenied left nothing behind.
  const auto entries = File::ScanDirectoryTree(dir, false);
  EXPECT_EQ(entries.children.size(), 1u);
  File::DeleteDirRecursively(dir);
}
#endif

// ---- Live: a room on the stdio bridge ----

// The app's half of the stdio bridge, played by this test: it answers hello with the invite's room,
// mints mpTicket as an anonymous dev ticket (as fake_bridge.py does), records each mpRoom, and
// checks that Orca asks for nothing else and no more than the app's allowlist passes on.
class FakeApp
{
public:
  explicit FakeApp(std::string room) : m_room(std::move(room))
  {
    Bridge::ResetForTests();
    {
      std::lock_guard lock(s_written_mutex);
      s_written.clear();
    }
    Bridge::SetWriter(&Capture);
    m_thread = std::thread([this] { Run(); });
  }
  ~FakeApp()
  {
    m_done = true;
    m_thread.join();
    Bridge::SetWriter(nullptr);
    Bridge::ResetForTests();
  }
  FakeApp(const FakeApp&) = delete;
  FakeApp& operator=(const FakeApp&) = delete;

  std::vector<std::string> Rooms()
  {
    std::lock_guard lock(m_mutex);
    return m_rooms;
  }
  std::vector<std::string> Problems()
  {
    std::lock_guard lock(m_mutex);
    return m_problems;
  }

private:
  void Problem(std::string what)
  {
    std::lock_guard lock(m_mutex);
    m_problems.push_back(std::move(what));
  }

  void Answer(u64 id, const picojson::object& result)
  {
    picojson::object reply;
    reply["re"] = picojson::value(static_cast<double>(id));
    reply["ok"] = picojson::value(true);
    reply["result"] = picojson::value(result);
    Bridge::Route({"yg " + picojson::value(reply).serialize()});
  }

  void Handle(const std::string& line)
  {
    picojson::value v;
    if (!line.starts_with("yg ") || !picojson::parse(v, line.substr(3)).empty() ||
        !v.is<picojson::object>())
    {
      Problem("not a request: " + line);
      return;
    }
    const picojson::object& o = v.get<picojson::object>();
    const u64 id = static_cast<u64>(v.get("id").get<double>());
    const std::string type = v.get("type").to_str();
    if (type == "hello")
    {
      picojson::object result;
      result["room"] = picojson::value(m_room);
      Answer(id, result);
    }
    else if (type == "mpTicket")
    {
      // The app rebuilds a ticket request from these fields only; Orca sends nothing else.
      for (const auto& [key, value] : o)
      {
        if (key != "id" && key != "type" && key != "players" && key != "mode" && key != "ranked" &&
            key != "room" && key != "lobby")
        {
          Problem("mpTicket field " + key);
        }
      }
      if (v.get("ranked").evaluate_as_boolean())
        Problem("a ranked ticket");
      picojson::object dev;
      dev["dev"] = picojson::value("orca-stdio-test");
      dev["player_id"] =
          picojson::value(fmt::format("orca-stdio-{}", static_cast<u64>(NowMs()) % 1000000000));
      dev["name"] = picojson::value("stdio");
      for (const char* key : {"players", "mode", "room", "lobby"})
        dev[key] = v.get(key);
      Common::HttpRequest http(std::chrono::seconds(15));
      const auto response = http.Post(
          "https://yougame.co/api/multiplayer/ticket", picojson::value(dev).serialize(),
          {{"Content-Type", "application/json"}}, Common::HttpRequest::AllowedReturnCodes::All);
      picojson::value ticket;
      if (!response ||
          !picojson::parse(ticket, std::string(response->begin(), response->end())).empty() ||
          !ticket.is<picojson::object>())
      {
        Problem(fmt::format("no dev ticket (HTTP {})", http.GetLastResponseCode()));
        return;
      }
      if (http.GetLastResponseCode() != 200)
      {
        Problem(fmt::format("dev ticket refused (HTTP {}): {}", http.GetLastResponseCode(),
                            ticket.serialize()));
      }
      Answer(id, ticket.get<picojson::object>());
    }
    else if (type == "mpRoom")
    {
      {
        std::lock_guard lock(m_mutex);
        m_rooms.push_back(line);
      }
      Answer(id, {});
    }
    else
    {
      Problem("asked for " + type);
    }
  }

  void Run()
  {
    size_t handled = 0;
    while (!m_done)
    {
      const auto lines = Written();
      for (; handled < lines.size(); ++handled)
        Handle(lines[handled]);
      std::this_thread::sleep_for(2ms);
    }
  }

  const std::string m_room;
  std::atomic<bool> m_done{false};
  std::mutex m_mutex;
  std::vector<std::string> m_rooms, m_problems;
  std::thread m_thread;
};

// Under the app on the stdio bridge (YOUGAME_STDIO=1): the invite's room comes from hello, the
// ticket from mpTicket, the room is mirrored to the page with mpRoom, and the page's Leave arrives
// as a push. The friend joins with a dev ticket. ORCA_LIVE_TEST=1 (yougame.co's rooms; anonymous
// dev tickets for the dev game orca-stdio-test, so no account and no real player is involved).
TEST(OrcaLive, StdioBridgeMirrorsTheRoomAndTakesLeave)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  using namespace Orca::Net;
  const std::string room = fmt::format("orcas{}", static_cast<u64>(NowMs()) % 10000000);
  ScopedEnv stdio("YOUGAME_STDIO", "1");
  ScopedEnv game("YOUGAME_GAME", "orca-test");
  ScopedEnv bridge("YOUGAME_BRIDGE", nullptr);
  ScopedEnv dev("ORCA_TEST_DEV_GAME", nullptr);
  FakeApp app(room);

  RoomOptions options;
  // The protocol's prefix: yougame.co refuses an Orca lobby without it since 0.3.32 went current
  // (orca_outdated, web lib/orca-protocol.ts), dev games included.
  options.compatibility = "orca2:stdio-live-test";
  options.mode = "orca-test";
  YouGameRoom a(options);
  for (int i = 0; i < 1500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(10ms);
  ASSERT_EQ(a.GetState(), RoomState::Waiting)
      << a.Status() << (app.Problems().empty() ? "" : "; " + app.Problems().front());
  EXPECT_EQ(a.Code(), room);

  // The friend, without the app (its environment is read on its own thread, at its ticket).
  SetEnv("YOUGAME_STDIO", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", "orca-stdio-test");
  options.room_code = room;
  options.joining = true;
  YouGameRoom b(options);
  for (int i = 0; i < 1500 && b.Code().empty() && b.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(10ms);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  SetEnv("YOUGAME_STDIO", "1");
  for (int i = 0; i < 1500 && !(a.Connected() && b.Connected()); ++i)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(a.Connected()) << a.Status();
  ASSERT_TRUE(b.Connected()) << b.Status();
  // The page sees the friend arrive.
  const auto mirrored_ready = [&] {
    const auto rooms = app.Rooms();
    return !rooms.empty() && rooms.back().find("\"ready\":true") != std::string::npos;
  };
  for (int i = 0; i < 1000 && !mirrored_ready(); ++i)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(mirrored_ready());

  // Another game's Leave is not this one's; this game's ends the room (no "caps leave" here).
  Bridge::Route({R"(yg {"push":{"slug":"another-game","cmd":"leave"}})"});
  std::this_thread::sleep_for(300ms);
  EXPECT_NE(a.GetState(), RoomState::Ended);
  Bridge::Route({R"(yg {"push":{"slug":"orca-test","cmd":"leave"}})"});
  for (int i = 0; i < 1000 && a.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(10ms);
  EXPECT_EQ(a.GetState(), RoomState::Ended);
  EXPECT_EQ(a.Status(), "You left the room");
  EXPECT_EQ(a.ErrorCode(), "");
  // (The friend stays in the room, waiting: what a joiner does when its host leaves is the room's
  // business, not the bridge's.)
  b.Leave();

  // The page's last word is the room with nobody in it.
  a.Leave();
  const auto rooms = app.Rooms();
  ASSERT_FALSE(rooms.empty());
  EXPECT_NE(rooms.back().find("\"players\":[]"), std::string::npos) << rooms.back();
  EXPECT_NE(rooms.back().find("\"participants\":[]"), std::string::npos) << rooms.back();
  EXPECT_NE(rooms.back().find("\"code\":\"" + room + "\""), std::string::npos) << rooms.back();
  EXPECT_TRUE(app.Problems().empty()) << app.Problems().front();
  EXPECT_GE(Bridge::GetStats().replies, 3u);
}

#ifdef __APPLE__
TEST(OrcaSandboxDeathTest, PackJailRecognizesAnAppliedProcessSandbox)
{
  // Apply a fixture profile only in the test's child; the parent's sandbox is unchanged.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  EXPECT_EXIT(
      {
        char* error = nullptr;
        const int result = sandbox_init("(version 1)(allow default)(deny network*)", 0, &error);
        if (error)
          sandbox_free_error(error);
        _exit(result == 0 && Sandbox::PackJailed() ? 0 : 1);
      },
      ::testing::ExitedWithCode(0), "");
#pragma clang diagnostic pop
}
#endif

TEST(OrcaEmbed, ResultWaitsForReadyAndKeepsFirstDecision)
{
  Embed::Result result(true);
  EXPECT_FALSE(result.TakeLine(true));
  result.Decide();
  EXPECT_FALSE(result.TakeLine(false));
  result.Decide("parent-gone");
  EXPECT_EQ(result.TakeLine(true), "orca embed ok");
  EXPECT_FALSE(result.TakeLine(true));
  result.Decide("track-failed");
  EXPECT_FALSE(result.TakeLine(true));
}

TEST(OrcaEmbed, FailureFormatsAndAdoptedSilence)
{
  for (const char* reason : {"parent-gone", "setparent", "no-parent", "track-failed"})
  {
    Embed::Result result(true);
    result.Decide(reason);
    EXPECT_FALSE(result.TakeLine(false));
    result.Decide();
    EXPECT_EQ(result.TakeLine(true), std::string("orca embed failed ") + reason);
    EXPECT_FALSE(result.TakeLine(true));
  }
  Embed::Result adopted;
  adopted.Decide();
  adopted.Decide("setparent");
  EXPECT_FALSE(adopted.TakeLine(true));
}

namespace
{
class OrcaLinuxPackJail : public ::testing::Test
{
protected:
  void SetUp() override
  {
    root = File::CreateTempDir();
    ASSERT_FALSE(root.empty());
    std::filesystem::create_directories(root + "/proc/self/net");
    std::filesystem::create_directories(root + "/proc/1");
    Write("/proc/self/status", Status());
    Write("/proc/self/uid_map", "0 1000 1\n");
    Write("/proc/1/comm", "bwrap\n");
    Write("/proc/self/net/dev", NetworkHeader() + NetworkRow("lo"));
    probes.root_type = [] { return std::optional<long>(0x01021994); };
    probes.passwd_home = [] { return std::optional<std::string>("/home/host"); };
    probes.home_entries = [this](const std::string& base) {
      return std::optional<Sandbox::PackJailHomeEntries>(
          {false, base == "/home" ? home_names : var_home_names});
    };
    probes.root_device = [] { return std::optional<unsigned long long>(7); };
    probes.directory = [](const std::string&) {
      return std::optional<Sandbox::PackJailDirectory>({7, 0x01021994});
    };
    probes.exists = [](const std::string&) { return std::optional<bool>(false); };
    probes.connect_x = [this](int n, bool abstract) -> std::optional<bool> {
      connections.emplace_back(n, abstract);
      return n == 5 && !abstract;
    };
    probes.home = "/run/yg/home";
    probes.display = ":5";
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  static std::string Status()
  {
    return "Name:\ttest\nNoNewPrivs:\t1\nCapEff:\t0000000000000000\n"
           "CapBnd:\t0000000000000000\nCapPrm:\t0000000000000000\n";
  }
  static std::string NetworkHeader()
  {
    return "Inter-|   Receive                                                |  Transmit\n"
           " face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n";
  }
  static std::string NetworkRow(const std::string& name)
  {
    return "  " + name + ": 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
  }
  void Write(const std::string& path, const std::string& content)
  {
    std::ofstream file(root + path);
    file << content;
    ASSERT_TRUE(file.good());
  }
  Sandbox::PackJailResult Check() { return Sandbox::CheckLinuxPackJail(root, probes, online); }
  void Refuses(const char* name)
  {
    const auto result = Check();
    EXPECT_FALSE(result.jailed);
    EXPECT_EQ(result.failed, name);
    EXPECT_EQ(result.Status().find("orca pack jail refused " + std::string(name)), 0u);
    EXPECT_EQ(result.Status().find("/home/host"), std::string::npos);
    EXPECT_EQ(result.Status().find('\n'), std::string::npos);
  }
  std::string root;
  Sandbox::PackJailProbes probes;
  std::vector<std::string> home_names{"host"};
  std::vector<std::string> var_home_names;
  bool online = false;
  std::vector<std::pair<int, bool>> connections;
};
}  // namespace

TEST_F(OrcaLinuxPackJail, AllChecksPassAndOnlyOwnPathSocketConnects)
{
  const auto result = Check();
  ASSERT_TRUE(result.jailed);
  EXPECT_TRUE(result.failed.empty());
  EXPECT_EQ(result.pid1, "bwrap");
  EXPECT_EQ(result.interfaces, std::vector<std::string>{"lo"});
  EXPECT_NE(result.Status().find("interfaces=lo pid1=bwrap"), std::string::npos);
  EXPECT_NE(result.Status().find("host-home sensitive-home HOME"), std::string::npos);
  EXPECT_NE(result.Status().find("passwd: <present>"), std::string::npos);
  EXPECT_EQ(result.Status().find("passwd-home"), std::string::npos);
  EXPECT_EQ(result.Status().find("/home/host"), std::string::npos);
  EXPECT_EQ(connections.size(), 19u);
  EXPECT_EQ(std::count(connections.begin(), connections.end(), std::pair{5, false}), 1);
  EXPECT_EQ(std::count(connections.begin(), connections.end(), std::pair{5, true}), 0);
}

TEST_F(OrcaLinuxPackJail, EachPrivilegeFieldFailsClosed)
{
  for (const char* field : {"NoNewPrivs", "CapEff", "CapBnd", "CapPrm"})
  {
    SCOPED_TRACE(field);
    for (const char* value : {"", "garbage", "10000000000000000", "0 0", "-1"})
    {
      auto text = Status();
      const auto begin = text.find(std::string(field) + ':');
      const auto end = text.find('\n', begin);
      text.replace(begin, end - begin, std::string(field) + ": " + value);
      Write("/proc/self/status", text);
      Refuses(field);
    }
    auto text = Status();
    const auto begin = text.find(std::string(field) + ':');
    text.erase(begin, text.find('\n', begin) - begin + 1);
    Write("/proc/self/status", text);
    Refuses(field);
    Write("/proc/self/status", Status() + std::string(field) + ": 0\n");
    Refuses(field);
  }
}

TEST_F(OrcaLinuxPackJail, EachCapabilityWithOneBitAndNoNewPrivsZeroRefuse)
{
  for (const char* field : {"NoNewPrivs", "CapEff", "CapBnd", "CapPrm"})
  {
    auto text = Status();
    const auto begin = text.find(std::string(field) + ':');
    text.replace(begin, text.find('\n', begin) - begin,
                 std::string(field) + (std::string_view(field) == "NoNewPrivs" ? ": 0" : ": 1"));
    Write("/proc/self/status", text);
    Refuses(field);
  }
}

TEST_F(OrcaLinuxPackJail, FirstFailureWins)
{
  Write("/proc/self/status", "NoNewPrivs: 0\nCapEff: 1\nCapBnd: 1\nCapPrm: 1\n");
  std::filesystem::remove(root + "/proc/self/uid_map");
  Refuses("NoNewPrivs");
}

TEST_F(OrcaLinuxPackJail, IdentityAndMalformedUidMapsRefuse)
{
  for (const char* mapping : {"0 0 4294967295", " \t0\t 0  4294967295\n", "", "0 1000",
                              "0 1000 0", "-1 1000 1", "0 1000 1 trailing", "4294967295 0 2"})
  {
    Write("/proc/self/uid_map", mapping);
    Refuses("uid_map");
  }
  Write("/proc/self/uid_map", "  0\t1000 1\n1 100000 65536\n");
  EXPECT_TRUE(Check().jailed);
}

TEST_F(OrcaLinuxPackJail, TmpfsAndOverlayPassExt4AndStatfsFailureRefuse)
{
  probes.root_type = [] { return std::optional<long>(0x794c7630); };
  EXPECT_TRUE(Check().jailed);
  probes.root_type = [] { return std::optional<long>(0xef53); };
  Refuses("root-fs");
  probes.root_type = [] { return std::optional<long>(); };
  Refuses("root-fs");
}

TEST_F(OrcaLinuxPackJail, NoPasswdPlaceholderHomesWithProfileFolderAndOverlayPass)
{
  probes.passwd_home = [] { return std::optional<std::string>(); };
  for (const char* base : {"/home", "/var/home"})
  {
    SCOPED_TRACE(base);
    home_names = std::string(base) == "/home" ? std::vector<std::string>{"deck"} :
                                              std::vector<std::string>{};
    var_home_names = std::string(base) == "/var/home" ? std::vector<std::string>{"deck"} :
                                                      std::vector<std::string>{};
    const std::string home = std::string(base) + "/deck";
    probes.exists = [=](const std::string& path) {
      return std::optional<bool>(path == home + "/profile/orca/apps");
    };
    for (const long type : {0x01021994L, 0x794c7630L})
    {
      probes.root_type = [=] { return std::optional<long>(type); };
      probes.directory = [=](const std::string& path) {
        EXPECT_EQ(path, home);
        return std::optional<Sandbox::PackJailDirectory>({7, type});
      };
      const auto result = Check();
      EXPECT_TRUE(result.jailed);
      EXPECT_NE(result.Status().find("passwd: none"), std::string::npos);
    }
  }
}

TEST_F(OrcaLinuxPackJail, EveryHomeEntryMustBeAPlaceholderOnTheRootDevice)
{
  probes.passwd_home = [] { return std::optional<std::string>(); };
  for (const char* base : {"/home", "/var/home"})
  {
    SCOPED_TRACE(base);
    home_names = std::string(base) == "/home" ? std::vector<std::string>{"deck", "x"} :
                                              std::vector<std::string>{};
    var_home_names = std::string(base) == "/var/home" ? std::vector<std::string>{"deck", "x"} :
                                                      std::vector<std::string>{};
    for (const auto metadata : {Sandbox::PackJailDirectory{8, 0x01021994},
                                Sandbox::PackJailDirectory{8, 0x794c7630},
                                Sandbox::PackJailDirectory{7, 0xef53}})
    {
      probes.directory = [=](const std::string& path) {
        return std::optional(path == std::string(base) + "/x" ? metadata :
                                 Sandbox::PackJailDirectory{7, 0x01021994});
      };
      Refuses("host-home");
    }
  }
}

TEST_F(OrcaLinuxPackJail, HomeEntrySymlinkRegularFileAndProbeErrorsRefuse)
{
  probes.passwd_home = [] { return std::optional<std::string>(); };
  // The OS directory probe returns nullopt for a symlink, regular file or stat/statfs error.
  for (const char* entry : {"deck-symlink", "regular-file", "unreadable-directory"})
  {
    SCOPED_TRACE(entry);
    home_names = {entry};
    probes.directory = [](const std::string&) {
      return std::optional<Sandbox::PackJailDirectory>();
    };
    Refuses("host-home");
  }
  probes.directory = [](const std::string&) {
    return std::optional<Sandbox::PackJailDirectory>({7, 0x01021994});
  };
  probes.root_device = [] { return std::optional<unsigned long long>(); };
  Refuses("host-home");
}

TEST_F(OrcaLinuxPackJail, HomeRootsSymlinkAndEnumerationErrorsRefuse)
{
  for (const char* base : {"/home", "/var/home"})
  {
    SCOPED_TRACE(base);
    probes.home_entries = [=](const std::string& path) {
      return std::optional<Sandbox::PackJailHomeEntries>({path == base, {}});
    };
    Refuses("host-home symlink");
    probes.home_entries = [=](const std::string& path) {
      return path == base ? std::optional<Sandbox::PackJailHomeEntries>() :
                            std::optional<Sandbox::PackJailHomeEntries>({false, {}});
    };
    Refuses("host-home");
  }
}

TEST_F(OrcaLinuxPackJail, AbsentHomeRootsAndNoPasswdNeedNoDirectoryOrDeviceProbe)
{
  home_names.clear();
  probes.passwd_home = [] { return std::optional<std::string>(); };
  probes.directory = [](const std::string&) {
    ADD_FAILURE() << "Absent homes must not need directory metadata";
    return std::optional<Sandbox::PackJailDirectory>();
  };
  probes.root_device = [] {
    ADD_FAILURE() << "Absent homes must not need a device comparison";
    return std::optional<unsigned long long>();
  };
  EXPECT_TRUE(Check().jailed);
}

TEST_F(OrcaLinuxPackJail, PasswdHomeOutsideBothRootsMustBeAbsentOrAPlaceholder)
{
  home_names.clear();
  for (const char* home : {"/root", "/srv/player"})
  {
    SCOPED_TRACE(home);
    probes.passwd_home = [=] { return std::optional<std::string>(home); };
    probes.exists = [](const std::string&) { return std::optional<bool>(false); };
    EXPECT_TRUE(Check().jailed);
    probes.exists = [=](const std::string& path) { return std::optional<bool>(path == home); };
    probes.directory = [](const std::string&) {
      return std::optional<Sandbox::PackJailDirectory>({7, 0x794c7630});
    };
    EXPECT_TRUE(Check().jailed);
    probes.directory = [](const std::string&) {
      return std::optional<Sandbox::PackJailDirectory>({8, 0x794c7630});
    };
    Refuses("host-home");
    probes.directory = [](const std::string&) {
      return std::optional<Sandbox::PackJailDirectory>();
    };
    Refuses("host-home");
    probes.exists = [](const std::string&) { return std::optional<bool>(); };
    Refuses("host-home");
  }
}

TEST_F(OrcaLinuxPackJail, EverySensitivePathInEveryHomeAndLstatErrorsRefuseWithRelativeName)
{
  probes.passwd_home = [] { return std::optional<std::string>("/srv/player"); };
  home_names = {"deck", "x"};
  var_home_names = {"deck", "x"};
  for (const char* home : {"/home/deck", "/home/x", "/var/home/deck", "/var/home/x", "/srv/player"})
  {
    SCOPED_TRACE(home);
    for (const char* relative : {".ssh", ".gnupg", ".local/share/keyrings", ".mozilla",
                                 ".config/google-chrome", ".config/chromium",
                                 ".config/BraveSoftware", ".local/share/Steam"})
    {
      SCOPED_TRACE(relative);
      for (const auto present : {std::optional<bool>(true), std::optional<bool>()})
      {
        probes.exists = [=](const std::string& path) {
          return path == std::string(home) + '/' + relative ? present : std::optional<bool>(false);
        };
        Refuses((std::string("sensitive-home ") + relative).c_str());
      }
    }
  }
}

TEST_F(OrcaLinuxPackJail, SensitiveHomeReportsFirstPathOnly)
{
  probes.exists = [](const std::string&) { return std::optional<bool>(true); };
  Refuses("sensitive-home .ssh");
}

TEST_F(OrcaLinuxPackJail, HomeMustBeSetAndDifferentFromEveryHomeAndPasswdHome)
{
  var_home_names = {"deck"};
  probes.passwd_home = [] { return std::optional<std::string>("/srv/player"); };
  for (const auto& home : {std::optional<std::string>(), std::optional<std::string>(""),
                           std::optional<std::string>("/home/host"),
                           std::optional<std::string>("/var/home/deck"),
                           std::optional<std::string>("/srv/player")})
  {
    probes.home = home;
    Refuses("HOME");
  }
}

TEST_F(OrcaLinuxPackJail, DisplayMustBeLocal)
{
  probes.display.reset();
  Refuses("DISPLAY");
  for (const char* display : {"", ":", "host:5", "localhost:5", ":-1", ":5.", ":5.x", ":5.1.2",
                              ":2147483648", ":5\n"})
  {
    probes.display = display;
    Refuses("DISPLAY");
  }
  probes.display = ":5.1";
  EXPECT_TRUE(Check().jailed);
}

TEST_F(OrcaLinuxPackJail, EveryOtherAbstractAndPathSocketMustRefuse)
{
  for (int n = 0; n <= 9; ++n)
  {
    if (n == 5)
      continue;
    for (const bool abstract : {true, false})
    {
      probes.connect_x = [=](int display, bool is_abstract) -> std::optional<bool> {
        return (display == 5 && !is_abstract) || (display == n && is_abstract == abstract);
      };
      Refuses(abstract ? "x11-abstract" : "x11-path");
    }
  }
}

TEST_F(OrcaLinuxPackJail, OwnSocketMustConnect)
{
  probes.connect_x = [](int, bool) { return std::optional<bool>(false); };
  Refuses("x11-own");
}

TEST_F(OrcaLinuxPackJail, SocketProbeErrorsRefuse)
{
  probes.connect_x = [](int, bool) { return std::optional<bool>(); };
  Refuses("x11-abstract");
}

TEST_F(OrcaLinuxPackJail, OfflineEth0RefusesOnlineEth0Passes)
{
  Write("/proc/self/net/dev", NetworkHeader() + NetworkRow("lo") + NetworkRow("eth0"));
  Refuses("network");
  online = true;
  const auto result = Check();
  EXPECT_TRUE(result.jailed);
  EXPECT_EQ(result.interfaces, (std::vector<std::string>{"eth0", "lo"}));
  EXPECT_NE(result.Status().find("interfaces=eth0,lo"), std::string::npos);
}

TEST_F(OrcaLinuxPackJail, PidOneNameIsOnlyDiagnostic)
{
  Write("/proc/1/comm", "sh\n");
  EXPECT_TRUE(Check().jailed);
  EXPECT_EQ(Check().pid1, "sh");
}

TEST_F(OrcaLinuxPackJail, MissingFilesAndNetworkFileFailClosed)
{
  for (const auto& [path, failure] :
       {std::pair{"/proc/self/status", "NoNewPrivs"}, {"/proc/self/uid_map", "uid_map"},
        {"/proc/1/comm", "pid1-comm"}})
  {
    const auto backup = std::filesystem::path(root + path + ".backup");
    std::filesystem::rename(root + path, backup);
    Refuses(failure);
    std::filesystem::rename(backup, root + path);
  }
  std::filesystem::remove(root + "/proc/self/net/dev");
  Refuses("network");
  online = true;
  Refuses("network");
  std::filesystem::create_directory(root + "/proc/self/net/dev");
  Refuses("network");
}

TEST_F(OrcaLinuxPackJail, NetworkParsingFailsClosedEvenOnline)
{
  online = true;
  for (const auto& text : {
           std::string(), std::string("one header\n"), NetworkHeader(),
           NetworkHeader() + "\n", NetworkHeader() + "lo\n",
           NetworkHeader() + NetworkRow(""), NetworkHeader() + NetworkRow("lo eth0"),
           NetworkHeader() + NetworkRow("lo/eth0"), NetworkHeader() + NetworkRow("."),
           NetworkHeader() + NetworkRow("1234567890123456"),
           NetworkHeader() + "lo: 0 0\n",
           NetworkHeader() + NetworkRow("lo") + NetworkRow("lo"),
           NetworkHeader() + "lo: -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
           NetworkHeader() + "lo: 18446744073709551616 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
           NetworkHeader() + "lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 nope\n",
           NetworkHeader() + "lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 extra\n"})
  {
    SCOPED_TRACE(text);
    Write("/proc/self/net/dev", text);
    Refuses("network");
  }
}

TEST_F(OrcaLinuxPackJail, NetworkUsesProcessProcInsteadOfHostSysfs)
{
  std::filesystem::create_directories(root + "/sys/class/net/eth0");
  EXPECT_TRUE(Check().jailed);
  Write("/proc/self/net/dev", NetworkHeader() +
                                 "\tlo \t: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
  EXPECT_TRUE(Check().jailed);
}
