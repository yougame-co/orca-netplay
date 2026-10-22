// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "Common/BroadcastRing.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Core/BroadcastSink.h"
#include "Core/Config/MainSettings.h"
#include "Core/System.h"

using namespace std::chrono_literals;

namespace
{
void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

class SinkFixture
{
public:
  SinkFixture()
  {
    SetEnv("ORCA_REPLAY_SOURCE", "fixture.ygr");
    SetEnv("ORCA_SESSION", "1");
    m_dir = File::CreateTempDir();
    EXPECT_FALSE(m_dir.empty());
    m_path = m_dir + "/sink.bin";
    std::vector<u32> memory(Broadcast::SIZE / 4);
    Broadcast::Initialize(memory);
    File::IOFile file(m_path, "wb");
    EXPECT_TRUE(file.WriteArray(memory.data(), memory.size()));
    EXPECT_TRUE(file.Close());
    SetEnv("ORCA_BROADCAST_SINK", m_path.c_str());
    Config::Init();
  }
  ~SinkFixture()
  {
    Broadcast::StopReplay();
    Broadcast::SetSilentAudio(false);
    Config::Shutdown();
    File::DeleteDirRecursively(m_dir);
  }
  std::vector<u32> Read()
  {
    std::vector<u32> memory(Broadcast::SIZE / 4);
    // The sink keeps the ring mapped read-write; on Windows only a sharing reader may open it.
    File::IOFile file(m_path, "rb", File::SharedAccess::ReadWrite);
    EXPECT_TRUE(file.ReadArray(memory.data(), memory.size()));
    return memory;
  }
  void InitNull()
  {
    Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
    AudioCommon::InitSoundStream(Core::System::GetInstance());
    EXPECT_EQ(Core::System::GetInstance().GetSoundStream()->GetMixer()->GetSampleRate(), 0u);
  }

private:
  std::string m_dir;
  std::string m_path;
};

void CheckSilentClock(bool sink_first)
{
  SinkFixture fixture;
  if (sink_first)
  {
    Broadcast::StartReplay();
    EXPECT_FALSE(Broadcast::SilentAudioRunning());
  }
  fixture.InitNull();
  const Common::ScopeGuard shutdown([] {
    AudioCommon::ShutdownSoundStream(Core::System::GetInstance());
  });
  const auto start = std::chrono::steady_clock::now();
  if (!sink_first)
    Broadcast::StartReplay();
  ASSERT_TRUE(Broadcast::Active());
  ASSERT_TRUE(Broadcast::SilentAudioRunning());
  std::this_thread::sleep_for(1s);
  Broadcast::StopReplay();
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  EXPECT_FALSE(Broadcast::SilentAudioRunning());
  auto memory = fixture.Read();
  const u32 count = Broadcast::Load(memory, 68);
  EXPECT_NEAR(count / elapsed, 100.0, 10.0);
  ASSERT_GE(count, 2u);
  EXPECT_EQ(Broadcast::Load(memory, 80), 2u);
  u64 previous = 0;
  for (u32 ticket = count > Broadcast::AUDIO_SLOTS ? count - Broadcast::AUDIO_SLOTS + 1 : 1;
       ticket <= count; ++ticket)
  {
    Broadcast::Packet packet;
    ASSERT_TRUE(Broadcast::Read(memory, true, ticket, &packet));
    EXPECT_EQ(packet.bytes.size(), Broadcast::AUDIO_BYTES);
    EXPECT_TRUE(std::all_of(packet.bytes.begin(), packet.bytes.end(), [](u8 b) { return b == 0; }));
    if (previous)
      EXPECT_EQ(packet.timestamp - previous, 10000u);
    previous = packet.timestamp;
  }
  std::this_thread::sleep_for(30ms);
  memory = fixture.Read();
  EXPECT_EQ(Broadcast::Load(memory, 68), count);
}

void CheckMixerCallbacks()
{
  SinkFixture fixture;
  Broadcast::StartReplay();
  EXPECT_TRUE(Broadcast::Active());
  EXPECT_FALSE(Broadcast::SilentAudioRunning());
  // A Mixer is several MB: on the heap, since Windows test threads have a 1 MB stack.
  const auto mixer = std::make_unique<Mixer>(Broadcast::RATE);
  std::array<s16, Broadcast::AUDIO_FRAMES * Broadcast::CHANNELS> samples{};
  EXPECT_EQ(mixer->Mix(samples.data(), Broadcast::AUDIO_FRAMES), Broadcast::AUDIO_FRAMES);
  auto memory = fixture.Read();
  EXPECT_EQ(Broadcast::Load(memory, 68), 1u);
  // A final mixed callback retains its sample bytes; no silent producer adds packets.
  samples.fill(1234);
  Broadcast::Audio(samples.data(), Broadcast::AUDIO_FRAMES, Broadcast::RATE);
  std::this_thread::sleep_for(40ms);
  Broadcast::StopReplay();
  memory = fixture.Read();
  EXPECT_EQ(Broadcast::Load(memory, 68), 2u);
  Broadcast::Packet packet;
  EXPECT_TRUE(Broadcast::Read(memory, true, 2, &packet));
  const auto* bytes = reinterpret_cast<const u8*>(samples.data());
  EXPECT_EQ(packet.bytes, std::vector<u8>(bytes, bytes + sizeof(samples)));
}

void CheckNoSink()
{
  SinkFixture fixture;
  SetEnv("ORCA_BROADCAST_SINK", "");
  fixture.InitNull();
  Broadcast::StartReplay();
  EXPECT_FALSE(Broadcast::Active());
  EXPECT_FALSE(Broadcast::SilentAudioRunning());
  AudioCommon::ShutdownSoundStream(Core::System::GetInstance());
  auto memory = fixture.Read();
  EXPECT_EQ(Broadcast::Load(memory, 68), 0u);
}

void CheckAudioShutdown()
{
  SinkFixture fixture;
  fixture.InitNull();
  Broadcast::StartReplay();
  EXPECT_TRUE(Broadcast::SilentAudioRunning());
  std::this_thread::sleep_for(40ms);
  AudioCommon::ShutdownSoundStream(Core::System::GetInstance());
  EXPECT_TRUE(Broadcast::Active());
  EXPECT_FALSE(Broadcast::SilentAudioRunning());
  auto memory = fixture.Read();
  const u32 count = Broadcast::Load(memory, 68);
  EXPECT_GT(count, 0u);
  std::this_thread::sleep_for(30ms);
  memory = fixture.Read();
  EXPECT_EQ(Broadcast::Load(memory, 68), count);
  Broadcast::StopReplay();
}
}  // namespace

// Sink setup is intentionally once per process, like a product replay. Isolate each lifetime.
TEST(BroadcastSinkDeathTest, NullBackendBeforeSinkProducesPacedSilence)
{
  ASSERT_EXIT({ CheckSilentClock(false); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); },
              ::testing::ExitedWithCode(0), "");
}

TEST(BroadcastSinkDeathTest, NullBackendAfterSinkProducesPacedSilence)
{
  ASSERT_EXIT({ CheckSilentClock(true); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); },
              ::testing::ExitedWithCode(0), "");
}

TEST(BroadcastSinkDeathTest, MixerCallbacksKeepTheirExistingAudioProducer)
{
  ASSERT_EXIT({ CheckMixerCallbacks(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); },
              ::testing::ExitedWithCode(0), "");
}

TEST(BroadcastSinkDeathTest, NoSinkNeverStartsAClock)
{
  ASSERT_EXIT({ CheckNoSink(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); },
              ::testing::ExitedWithCode(0), "");
}

TEST(BroadcastSinkDeathTest, AudioShutdownJoinsClockBeforeSinkStops)
{
  ASSERT_EXIT({ CheckAudioShutdown(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); },
              ::testing::ExitedWithCode(0), "");
}
