// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Core.h"
#include "Core/Orca/BootThrottle.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Rollback/Harness.h"

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

using namespace Orca::Net;

class InputGame final : public Game
{
public:
  bool Save(int) override { return true; }
  bool Load(int) override { return true; }
  void SetPads(int frame, const Pads& pads) override
  {
    running = frame;
    inputs.push_back(pads);
  }
  void SetResimulating(bool) override {}
  std::optional<u64> SnapshotChecksum(int) override { return std::nullopt; }
  void ResetPacing() override {}
  int running = -1;
  std::vector<Pads> inputs;
};

class InputLink final : public Transport
{
public:
  void Send(const Packet& packet) override { peer->pending.push_back(packet); }
  std::vector<Packet> Receive() override { return std::exchange(pending, {}); }
  bool Connected() const override { return true; }
  InputLink* peer = nullptr;
  std::vector<Packet> pending;
};

// Run in a fresh child: the harness caches its input file, and other suites can leave threads.
// _Exit avoids running process-wide teardown from the assertion child.
void ScriptedRun(bool online)
{
  const std::string directory = File::CreateTempDir();
  const std::string script = directory + "/input.yg";
  {
    File::IOFile file(script, "w");
    const std::string rule = "0 1000 1 A SX=200\n";
    if (!file.WriteBytes(rule.data(), rule.size()))
      std::_Exit(2);
  }
  SetEnv("ORCA_SESSION", "1");
  SetEnv("ORCA_TEST_DEV_GAME", online ? "RSBE01" : "");
  SetEnv("YG_INPUT", script.c_str());
  if (Orca::Online::Enabled() != online || !Rollback::Harness::Active())
    std::_Exit(3);
  Core::SetIsThrottlerTempDisabled(false);
  Orca::BootThrottle::Begin();
  Orca::BootThrottle::FinishAtFirstFrame();
  const bool unthrottled = Core::GetIsThrottlerTempDisabled();
  if (!online)
  {
    File::DeleteDirRecursively(directory);
    std::_Exit(unthrottled ? 0 : 4);
  }

  // Two real input-exchange sessions; one samples the script, one a pad. The host pacer
  // schedules one boundary per tick, or four when the boot throttle was left disabled.
  InputGame games[2];
  InputLink links[2];
  links[0].peer = &links[1];
  links[1].peer = &links[0];
  Config a;
  a.input_delay = 2;
  a.max_delay = 2;
  a.time_sync = false;  // Expose pacing errors instead of hiding them in time-sync waits.
  Config b = a;
  b.local_seat = 1;
  Session sessions[] = {Session(a, games[0], links[0]), Session(b, games[1], links[1])};
  int stalls = 0;
  for (int tick = 0; tick < 600; ++tick)
  {
    for (int seat = 0; seat < 2; ++seat)
    {
      const int frames = seat == 0 && unthrottled ? 4 : 1;
      for (int frame = 0; frame < frames; ++frame)
      {
        GCPadStatus pad{};
        pad.isConnected = true;
        if (seat == 0)
          pad = *Rollback::Harness::InputOverride(0);
        const Step step = sessions[seat].OnBoundary(games[seat].running, EncodePad(pad));
        if (step.kind == StepKind::Stall)
          ++stalls;
        else if (step.kind != StepKind::Run)
          std::_Exit(5);
      }
    }
  }
  bool delivered = games[1].inputs.size() == 600;
  for (std::size_t frame = 2; frame < games[1].inputs.size(); ++frame)
  {
    const GCPadStatus pad = DecodePad(games[1].inputs[frame][0]);
    delivered = delivered && pad.button == PAD_BUTTON_A && pad.stickX == 200;
  }
  std::fprintf(stderr, "scripted online: %d stalls, frames %d/%d, input delay 2, connected %d\n",
               stalls, games[0].running + 1, games[1].running + 1,
               static_cast<int>(links[0].Connected() && links[1].Connected()));
  File::DeleteDirRecursively(directory);
  std::_Exit(!unthrottled && stalls == 0 && delivered ? 0 : 6);
}
class OrcaHarnessInput : public testing::Test
{
protected:
  void SetUp() override
  {
    m_style = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
  void TearDown() override { GTEST_FLAG_SET(death_test_style, m_style); }

private:
  std::string m_style;
};
}  // namespace

TEST_F(OrcaHarnessInput, OnlineScriptUsesSessionPacingAndDelayedLocalInput)
{
  ASSERT_EXIT(ScriptedRun(true), testing::ExitedWithCode(0), "0 stalls, frames 600/600");
}

TEST_F(OrcaHarnessInput, OfflineScriptKeepsUnthrottledPacing)
{
  ASSERT_EXIT(ScriptedRun(false), testing::ExitedWithCode(0), "");
}
