// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <map>
#include <string>
#include <vector>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <SFML/Network/UdpSocket.hpp>
#include <SFML/System/Time.hpp>
#include <gtest/gtest.h>
#include <fmt/format.h>

#include "Common/Analytics.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Config/Config.h"
#include "Common/HttpRequest.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Common/TraversalClient.h"
#include "Common/WebSocket.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/EXI/BBA/TAPServerConnection.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/Rollback/Loopback.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"
#include "UICommon/AutoUpdate.h"

using namespace std::chrono_literals;

namespace WiimoteReal
{
class WiimoteTestPeer
{
public:
  static bool Read(Wiimote& wiimote) { return wiimote.Read(); }
  static bool Write(Wiimote& wiimote, const Report& report)
  {
    return wiimote.Write({{}, report});
  }
};
}  // namespace WiimoteReal

namespace
{
class FakeBalanceBoard final : public WiimoteReal::Wiimote
{
public:
  FakeBalanceBoard() { m_index = WIIMOTE_BALANCE_BOARD; }
  ~FakeBalanceBoard() override { Shutdown(); }
  std::string GetId() const override { return "offline-balance-board-fixture"; }
  bool IsConnected() const override { return true; }
  int reads = 0;
  int writes = 0;

private:
  bool ConnectInternal() override { return true; }
  void DisconnectInternal() override {}
  void IOWakeup() override {}
  int IORead(u8* data) override
  {
    ++reads;
    data[0] = 0xa1;
    data[1] = 0x30;
    return 2;
  }
  int IOWrite(const u8*, size_t size) override
  {
    ++writes;
    return static_cast<int>(size);
  }
};

class ReplaySource
{
public:
  explicit ReplaySource(const char* value)
  {
    if (const char* old = std::getenv("ORCA_REPLAY_SOURCE"))
      m_old = old;
    Set(value);
  }
  ~ReplaySource() { Set(m_old ? m_old->c_str() : nullptr); }

  static void Set(const char* value)
  {
#ifdef _WIN32
    _putenv_s("ORCA_REPLAY_SOURCE", value ? value : "");
#else
    if (value)
      setenv("ORCA_REPLAY_SOURCE", value, 1);
    else
      unsetenv("ORCA_REPLAY_SOURCE");
#endif
  }

private:
  std::optional<std::string> m_old;
};
}  // namespace

TEST(OrcaOfflineReplay, BalanceBoardReadAndWriteNeverSendFilePlaybackDatagrams)
{
  // File playback must stand on its own, without the separate local-input switch.
  const char* old_input = std::getenv("ORCA_NO_LOCAL_INPUT");
  const std::optional<std::string> old_input_value =
      old_input ? std::make_optional<std::string>(old_input) : std::nullopt;
#ifdef _WIN32
  _putenv_s("ORCA_NO_LOCAL_INPUT", "");
#else
  unsetenv("ORCA_NO_LOCAL_INPUT");
#endif
  const Common::ScopeGuard restore_input([&] {
#ifdef _WIN32
    _putenv_s("ORCA_NO_LOCAL_INPUT", old_input_value ? old_input_value->c_str() : "");
#else
    if (old_input_value)
      setenv("ORCA_NO_LOCAL_INPUT", old_input_value->c_str(), 1);
    else
      unsetenv("ORCA_NO_LOCAL_INPUT");
#endif
  });
  sf::UdpSocket receiver;
  ASSERT_EQ(receiver.bind(0, sf::IpAddress::LocalHost), sf::Socket::Status::Done);
  receiver.setBlocking(false);
  sf::SocketSelector selector;
  selector.add(receiver);
  Config::Init();
  const Common::ScopeGuard shutdown_config([] { Config::Shutdown(); });
  Config::SetCurrent(Config::MAIN_BB_DUMP_PORT, static_cast<int>(receiver.getLocalPort()));
  {
    FakeBalanceBoard board;
    board.EventLinked();
    const WiimoteReal::Report output{0xa2, 0x12, 0x00};
    const ReplaySource replay(nullptr);
    for (const char* source : {"missing-file.ygr", "saved.ygr", static_cast<const char*>(nullptr),
                               "", "ws://127.0.0.1/watch", "wss://rooms.invalid/watch",
                               "missing-file.ygr"})
    {
      ReplaySource::Set(source);
      const bool offline = Common::OfflineReplay();
      for (const bool read : {false, true})
      {
        ASSERT_TRUE(read ? WiimoteReal::WiimoteTestPeer::Read(board) :
                           WiimoteReal::WiimoteTestPeer::Write(board, output));
        std::array<u8, WiimoteCommon::MAX_PAYLOAD> bytes{};
        size_t received = 0;
        std::optional<sf::IpAddress> sender;
        unsigned short port = 0;
        // Allow delivery to settle before asserting silence, including the first socket creation.
        (void)selector.wait(sf::milliseconds(50));
        const auto status = receiver.receive(bytes.data(), bytes.size(), received, sender, port);
        EXPECT_EQ(status, offline ? sf::Socket::Status::NotReady : sf::Socket::Status::Done)
            << "source=" << (source ? source : "unset") << " read=" << read;
        if (!offline)
        {
          WiimoteReal::Report expected = read ? WiimoteReal::Report(WiimoteCommon::MAX_PAYLOAD) :
                                               output;
          if (read)
          {
            expected[0] = 0xa1;
            expected[1] = 0x30;
          }
          EXPECT_EQ(WiimoteReal::Report(bytes.begin(), bytes.begin() + received), expected);
        }
      }
    }
    EXPECT_EQ(board.reads, 7);
    EXPECT_EQ(board.writes, 7);
  }
}

TEST(OrcaOfflineReplay, OnlyProductFileSourcesDisableNetworking)
{
  for (const char* source : {"match.ygr", "./saved match.ygr", "/tmp/match.ygr",
                             "C:\\replays\\match.ygr", "missing-file.ygr", "WS://file.ygr"})
  {
    const ReplaySource replay(source);
    EXPECT_TRUE(Rollback::ReplayStream::OfflineReplay()) << source;
    EXPECT_TRUE(Rollback::ReplayStream::ProductMode());
  }
  for (const char* source : {static_cast<const char*>(nullptr), "", "ws://127.0.0.1/watch/match",
                             "wss://rooms.invalid/watch/match"})
  {
    const ReplaySource replay(source);
    EXPECT_FALSE(Rollback::ReplayStream::OfflineReplay());
  }
}

TEST(OrcaOfflineReplay, HttpWebSocketAndTelemetryNeverConnectEvenToLoopback)
{
  // A listening endpoint distinguishes a refusal before connect from a failed request after it.
  sf::TcpListener listener;
  ASSERT_EQ(listener.listen(0, sf::IpAddress::LocalHost), sf::Socket::Status::Done);
  listener.setBlocking(false);
  const std::string origin = "127.0.0.1:" + std::to_string(listener.getLocalPort());
  Common::HttpRequest http(50ms);  // Also covers a client constructed before playback mode.
  const ReplaySource replay("saved.ygr");
  EXPECT_FALSE(http.Get("http://" + origin));
  EXPECT_FALSE(http.Post("http://" + origin, "report"));
  const std::vector<u8> bytes{1, 2};
  EXPECT_FALSE(http.Post("http://" + origin, bytes));
  std::vector<Common::HttpRequest::Multiform> form{{"report", "test"}};
  EXPECT_FALSE(http.PostMultiform("http://" + origin, form));
  EXPECT_EQ(http.GetLastResponseCode(), 0);

  Common::WebSocket socket;
  std::string error;
  EXPECT_FALSE(socket.Connect("ws://" + origin, 50ms, &error));
  EXPECT_EQ(error, "network off for file replay");
  EXPECT_FALSE(socket.IsOpen());
  Common::HttpAnalyticsBackend analytics("http://" + origin);
  analytics.Send("test report");

  Orca::UX::StartControllerStream("http://" + origin, "offline-fixture");
  double age = 0;
  EXPECT_EQ(Orca::UX::LatestSnapshot(&age), nullptr);
  Orca::UX::StopControllerStream();

  ExpansionInterface::TAPServerConnection tap(origin, [](std::string&&) {}, 1500);
  EXPECT_FALSE(tap.Activate());
  EXPECT_FALSE(tap.IsActivated());

  sf::TcpSocket accepted;
  EXPECT_EQ(listener.accept(accepted), sf::Socket::Status::NotReady);
}

TEST(OrcaOfflineReplay, DirectLinkRefusesBeforeStartingItsWorker)
{
  const ReplaySource replay("saved.ygr");
  EXPECT_FALSE(Orca::Net::DirectOptionsFromEnv());
  Orca::Net::DirectOptions options;
  options.me = "offline-player";
  options.room = "offline-room";
  options.ice.stun_host.clear();
  options.loopback = true;
  Orca::Net::DirectLink direct;
  EXPECT_FALSE(direct.Start(options));
  EXPECT_TRUE(direct.TakeSignals().empty());
  // Refusing the start must leave the object unstarted. A live watch can still start normally.
  ReplaySource::Set("wss://rooms.invalid/watch/match");
  EXPECT_TRUE(direct.Start(options));
  direct.Stop();
}

TEST(OrcaOfflineReplay, RoomsAndTicketRequestsRefuseSynchronously)
{
  const ReplaySource replay("saved.ygr");
  Orca::Net::YouGameRoom room({});
  EXPECT_EQ(room.GetState(), Orca::Net::RoomState::Ended);
  EXPECT_EQ(room.Status(), "network off for file replay");
  EXPECT_FALSE(room.Connected());
  std::string ticket, url, error;
  EXPECT_FALSE(room.FreshTicket(true, &ticket, &url, &error));
  EXPECT_EQ(error, "network off for file replay");
  Orca::Online::Start();
  EXPECT_EQ(Orca::Online::RoomGeneration(), 0u);
}

TEST(OrcaOfflineReplay, KeyframeUploadsDownloadsAndDeletesNeverAskForATicket)
{
  const ReplaySource replay("saved.ygr");
  int tickets = 0;
  auto store = Orca::Net::MakeHttpKeyframeStore(
      [&](bool, std::string*, std::string*, std::string*) {
        ++tickets;
        return false;
      });
  const std::vector<u8> bytes{1, 2, 3};
  const std::string hash = Orca::Net::KeyframeHash(bytes);
  Orca::Net::KeyframeInfo info{1, "replay-1-" + hash.substr(0, 8), bytes.size(), hash, ""};
  std::string error;
  EXPECT_FALSE(store->Put(info, bytes, &error));
  EXPECT_EQ(error, "network off for file replay");
  EXPECT_FALSE(store->Get(info, [](u64, u64) { return true; }, &error));
  EXPECT_EQ(error, "network off for file replay");
  store->Delete(info.id);
  store.reset();
  EXPECT_EQ(tickets, 0);
}

TEST(OrcaOfflineReplay, ClassicNetPlayAndTraversalRefuseBeforeSetup)
{
  const ReplaySource replay("saved.ygr");
  EXPECT_FALSE(Common::EnsureTraversalClient("127.0.0.1", 9, 9, 0));
  Common::ENet::WakeupThread(nullptr);
  EXPECT_FALSE(Common::ENet::SendPacket(nullptr, sf::Packet{}, 0));
  NetPlay::NetTraversalConfig traversal{};
  NetPlay::NetPlayClient client("127.0.0.1", 9, nullptr, "offline", traversal);
  EXPECT_FALSE(client.IsConnected());
  NetPlay::NetPlayServer server(0, false, nullptr, traversal);
  EXPECT_FALSE(server.is_connected);
}

TEST(OrcaOfflineReplay, UpdateCheckReturnsBeforeRequestOrUpdaterLaunch)
{
  class Checker final : public AutoUpdateChecker
  {
  public:
    bool notified = false;

  private:
    void OnUpdateAvailable(const NewVersionInformation&) override { notified = true; }
  } checker;
  const ReplaySource replay("saved.ygr");
  checker.CheckForUpdate("dev", "test-revision", AutoUpdateChecker::CheckType::Automatic);
  EXPECT_FALSE(checker.notified);
}

TEST(OrcaOfflineReplay, DebugListenerRefusesBeforeSocketSetup)
{
  const ReplaySource replay("saved.ygr");
  GDBStub::Init(0);
  EXPECT_FALSE(GDBStub::IsActive());
}

namespace
{
u64 CoReplayHash(u64 state, int frame, const Orca::Net::Pads& pads)
{
  state = (state ^ static_cast<u64>(frame)) * 1099511628211ull;
  for (const auto& pad : pads)
    for (const u8 byte : pad)
      state = (state ^ byte) * 1099511628211ull;
  return state;
}

class CoReplayGame final : public Orca::Net::Game
{
public:
  explicit CoReplayGame(Rollback::RingPort& port) : m_port(port) {}
  bool Save(int frame) override { m_saved[frame] = state; return true; }
  bool Load(int frame) override
  {
    const auto saved = m_saved.find(frame);
    if (saved == m_saved.end())
      return false;
    state = saved->second;
    m_saved.erase(m_saved.upper_bound(frame), m_saved.end());
    return true;
  }
  void SetPads(int frame, const Orca::Net::Pads& pads) override
  {
    m_frame = frame;
    m_port.SetPads(frame, pads);
  }
  void SetResimulating(bool) override {}
  void ResetPacing() override {}
  std::optional<u64> SnapshotChecksum(int frame) override
  {
    const auto saved = m_saved.find(frame);
    return saved == m_saved.end() ? std::nullopt : std::optional<u64>(saved->second);
  }
  int Run()
  {
    Orca::Net::Pads pads{};
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
      pads[seat] = Orca::Net::EncodePad(m_port.Pad(seat));
    state = CoReplayHash(state, m_frame, pads);
    return m_frame;
  }
  u64 state = 1;
private:
  Rollback::RingPort& m_port;
  std::map<int, u64> m_saved;
  int m_frame = -1;
};
}

TEST(OrcaOfflineReplay, CoSeatSessionRecordsFourPortsAndFilePlaybackKeepsEveryHash)
{
  const ReplaySource live(nullptr);
  using Orca::Net::Pad;
  using Orca::Net::Pads;
  std::vector<Rollback::Loopback::RecordedFrame> recording;
  u64 expected = 1;
  for (int frame = 0; frame < 500; ++frame)
  {
    Pads pads{};
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      GCPadStatus pad{};
      pad.isConnected = true;
      pad.button = seat % 2 ? PAD_BUTTON_B : PAD_BUTTON_A;
      pad.stickX = static_cast<u8>(128 + 10 * seat + (frame / 11) % 5);
      pad.stickY = pad.substickX = pad.substickY = 128;
      pads[seat] = Orca::Net::EncodePad(pad);
    }
    recording.push_back({pads, expected});
    expected = CoReplayHash(expected, frame, pads);
  }
  auto& system = Core::System::GetInstance();
  Rollback::RingPort port(system, 7);
  CoReplayGame game(port);
  Rollback::Loopback::VirtualRemote remote(recording, {1, 3}, 0, 9, 2, 2, 10, {2});
  Orca::Net::Config config;
  config.seats = 4;
  config.co_seats = {2};
  config.fixed_delay = 2;
  config.time_sync = false;
  config.checksum_every = 10;
  config.snapshot_every = 1;
  config.start_pad = recording[0].pads[0];
  config.start_pads[2] = recording[0].pads[2];
  Orca::Net::Session session(config, game, remote);
  Orca::Net::ReplayArchive archive;
  archive.first_frame = 0;
  archive.origin_hash = 1;
  int completed = -1;
  int polls = 0;
  {
    Orca::Net::ReplayRecordingScope scope(&archive);
    for (int guard = 0; guard < 4000 && completed < 320; ++guard)
    {
      if (!session.Resimulating())
        remote.Tick();
      const auto step = Rollback::StepSession(session, game, completed,
          std::function<Pad(int)>([&](int seat) {
            return recording.at(session.CurrentFrame() + session.Delay()).pads[seat];
          }), [&] { remote.TickWhileWaiting(); return ++polls > 4000; },
          std::chrono::microseconds(0));
      ASSERT_NE(step.kind, Orca::Net::StepKind::Ended) << session.Error();
      completed = game.Run();
    }
  }
  ASSERT_GE(completed, 320);
  ASSERT_GT(session.GetStats().checksums_matched, 10);
  const int end = session.FinalFrame();
  ASSERT_GT(end, 300);
  archive.frames.resize(end);
  const auto ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "A", false}, {1, "B", true},
                                          {2, "A2", false}, {3, "C", true}});
  std::map<int, u64> checksums;
  for (int frame = 0; frame < end; ++frame)
  {
    ASSERT_EQ(archive.frames[frame].pads, recording[frame].pads) << frame;
    archive.frames[frame].ports = ports;
    if (frame > 0 && frame % 10 == 0)
      checksums.emplace(frame, recording[frame].checksum);
  }
  const std::string directory = File::CreateTempDir();
  const Common::ScopeGuard cleanup([&] { File::DeleteDirRecursively(directory); });
  const std::string base = directory + "/coseats";
  {
    Rollback::ReplayStream::Writer writer(base);
    ASSERT_TRUE(writer.Pump(archive, end, end, &checksums));
  }
  const std::string path = base + "-1.ygr";
  File::IOFile file(path, "rb");
  ASSERT_TRUE(file);
  std::vector<u8> bytes(file.GetSize());
  ASSERT_TRUE(file.ReadBytes(bytes.data(), bytes.size()));
  Orca::Net::ReplayStreamReader reader;
  std::string error;
  ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size(), &error)) << error;
  ASSERT_TRUE(reader.Complete());
  EXPECT_EQ(reader.Checksums(), checksums);
  const ReplaySource offline(path.c_str());
  auto player = Rollback::ReplayStream::Player::FromEnv();
  ASSERT_TRUE(player);
  u64 replay_hash = 1;
  int played = 0;
  int running = -1;
  std::string stopped;
  Rollback::ReplayStream::PlayerHost host;
  host.restore_origin = [&] { replay_hash = 1; return true; };
  host.set_catching_up = [](bool) {};
  host.set_pads = [&](int frame, const Pads& pads) {
    EXPECT_EQ(pads, recording.at(frame).pads);
    EXPECT_EQ(replay_hash, recording.at(frame).checksum) << frame;
    replay_hash = CoReplayHash(replay_hash, frame, pads);
    ++played;
  };
  host.stopping = [] { return false; };
  host.stop = [&](const std::string& message) { stopped = message; };
  for (int guard = 0; guard < end + 5 && !player->Over(); ++guard)
    player->Step(system, host, &running, 1, 0, [](int, bool, const auto&, bool) {});
  EXPECT_TRUE(player->Over());
  EXPECT_EQ(played, end);
  EXPECT_EQ(replay_hash, recording.at(end).checksum);
  EXPECT_NE(stopped.find("The replay is over"), std::string::npos);
}

TEST(OrcaOfflineReplay, SingleSeatMachinesReplayFileMatchesPreCoSeatsBytes)
{
  using namespace Orca::Net;
  ReplayArchive archive;
  archive.first_frame = 100;
  archive.origin_hash = 0x123456789abcdef0ull;
  const auto ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "P1", false}, {1, "P2", false}});
  for (int frame = 0; frame < 12; ++frame)
  {
    ReplayFrame entry;
    entry.pads.fill(UNPLUGGED_PAD);
    for (int seat = 0; seat < 2; ++seat)
      entry.pads[seat] = Pad{static_cast<u8>(seat + 1), static_cast<u8>(frame / 3),
                             static_cast<u8>(frame + seat), 0, 0, 0, 0, 0};
    entry.ports = ports;
    if (frame == 0)
    {
      entry.seed = 0x31415926;
      entry.menu_exit = MENU_EXIT_FRIENDS;
    }
    entry.clear_ready = frame == 6;
    archive.frames.push_back(entry);
  }
  // Fixed provenance makes the entire file comparable across build revisions.
  ReplayStreamInfo info;
  info.compat = "single-seat-golden";
  info.build = "80448a9e1f";
  info.game_id = "RSBE01";
  info.origin_hash = archive.origin_hash;
  info.first_frame = archive.first_frame;
  info.segment = 1;
  info.seat_labels = true;
  const std::map<int, u64> checksums{{106, 0x9876543210abcdefull}, {112, 0x1020304050607080ull}};
  const auto encoded = PackReplayStream(info, archive, checksums);
  ASSERT_FALSE(encoded.empty());
  const std::string directory = File::CreateTempDir();
  const Common::ScopeGuard cleanup([&] { File::DeleteDirRecursively(directory); });
  const std::string path = directory + "/single-seat.ygr";
  {
    File::IOFile file(path, "wb");
    ASSERT_TRUE(file.WriteBytes(encoded.data(), encoded.size()));
  }
  File::IOFile file(path, "rb");
  ASSERT_TRUE(file);
  std::vector<u8> bytes(file.GetSize());
  ASSERT_TRUE(file.ReadBytes(bytes.data(), bytes.size()));
  std::string actual;
  for (const u8 byte : bytes)
    actual += fmt::format("{:02x}", byte);
  // Captured as a file using the replay codec from detached 80448a9e1f.
  const std::string_view expected =
      "5947525301000000a60000007b226275696c64223a2238303434386139653166222c22636f6d706174223a2273696e67"
      "6c652d736561742d676f6c64656e222c2266697273745f6672616d65223a3130302c2267616d65223a22525342453031"
      "222c226f726967696e5f68617368223a2231323334353637383961626364656630222c22726f6f6d223a22222c227365"
      "61745f6c6162656c73223a747275652c227365676d656e74223a312c2276223a327d59475243640000000c0000007000"
      "000002000000ba0000004a00af969da0a01e28b52ffd6028048505000405010002000100800002503100503219000000"
      "26594131010280010200038001030104040505060206020701070801020209800309030a02030b02030c6a000000efcd"
      "ab103254769870706050403020103220608324ba06c187299c91819c00ce9c19f813ab0165118605e3ab3367769170e8"
      "5c66168e300cff649900e95c06a436ace58db6627c7de624f91a117d79102c5880f84e1e570b3bef5a701bd8102ce0b8"
      "7f05f4a9e686048303192616";
  EXPECT_EQ(actual, expected);
  ReplayStreamReader reader(info.compat);
  std::string error;
  ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size(), &error)) << error;
  ASSERT_TRUE(reader.Complete());
  ASSERT_EQ(reader.Archive().frames.size(), archive.frames.size());
  EXPECT_EQ(reader.Checksums(), checksums);
  for (size_t frame = 0; frame < archive.frames.size(); ++frame)
    EXPECT_EQ(reader.Archive().frames[frame].pads, archive.frames[frame].pads);
}
