// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// A lost graphics device (Orca/GpuLost.h): the run log's line, the app's error code and sentence,
// and the device state a crash note carries.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Orca/GpuLost.h"

namespace
{
std::string State()
{
  char out[256];
  const size_t length = Orca::GpuLost::DeviceState(out, sizeof(out));
  return std::string(out, length);
}
}  // namespace

TEST(OrcaGpuLost, TheAppReadsTheCodeAndThePlayerTheSentence)
{
  // The page's lib/orca.ts orcaErrorProblem matches this code; an older page shows the sentence.
  EXPECT_EQ(Orca::GpuLost::CODE, "gpu_lost");
  EXPECT_EQ(Orca::GpuLost::SENTENCE,
            "Your graphics driver stopped responding. Try the Performance preset or a lower "
            "resolution in Graphics, and update your GPU driver.");
  // One word for the code and one line for the sentence (Status.h: "orca error <code> <sentence>").
  EXPECT_EQ(Orca::GpuLost::CODE.find(' '), std::string_view::npos);
  EXPECT_EQ(Orca::GpuLost::SENTENCE.find('\n'), std::string_view::npos);
}

TEST(OrcaGpuLost, ReasonNames)
{
  EXPECT_STREQ(Orca::GpuLost::ReasonName(0x887A0005), "device removed");
  EXPECT_STREQ(Orca::GpuLost::ReasonName(0x887A0006), "device hung");
  EXPECT_STREQ(Orca::GpuLost::ReasonName(0x887A0007), "device reset");
  EXPECT_STREQ(Orca::GpuLost::ReasonName(0x887A0020), "driver internal error");
  EXPECT_STREQ(Orca::GpuLost::ReasonName(0x887A0001), "invalid call");
  EXPECT_STREQ(Orca::GpuLost::ReasonName(0x12345678), "error");
}

TEST(OrcaGpuLost, TheLogLineSaysWhyAndWhere)
{
  char out[320];
  const size_t length =
      Orca::GpuLost::FormatLine(out, sizeof(out), "D3D11", 0x887A0006, "a 16x16x1 texture");
  EXPECT_EQ(std::string(out, length),
            "Orca: [VIDEO] the graphics device is lost (D3D11: device hung, 0x887A0006) at a "
            "16x16x1 texture; Orca stops");

  // Cut to fit, still terminated.
  char small[20];
  EXPECT_EQ(Orca::GpuLost::FormatLine(small, sizeof(small), "D3D11", 0x887A0005, "present"), 19u);
  EXPECT_STREQ(small, "Orca: [VIDEO] the g");
}

TEST(OrcaGpuLost, ReportedOnceAndKeptForCrashNotes)
{
  Orca::GpuLost::DeviceCreated("D3D11");
  EXPECT_FALSE(Orca::GpuLost::Lost());
  EXPECT_EQ(State(), "D3D11 running");

  // No game is running in a test, so this only logs and keeps the state: no error, no stop.
  EXPECT_TRUE(Orca::GpuLost::Report("D3D11", 0x887A0006, "a 16x16x1 texture"));
  EXPECT_TRUE(Orca::GpuLost::Lost());
  EXPECT_EQ(State(), "lost (D3D11: device hung, 0x887A0006) at a 16x16x1 texture");

  // Whatever fails next on the dead device says nothing more.
  EXPECT_FALSE(Orca::GpuLost::Report("D3D11", 0x887A0005, "present"));
  EXPECT_EQ(State(), "lost (D3D11: device hung, 0x887A0006) at a 16x16x1 texture");

  // Letting the device go keeps the loss as the state.
  Orca::GpuLost::DeviceClosed("D3D11");
  EXPECT_EQ(State(), "lost (D3D11: device hung, 0x887A0006) at a 16x16x1 texture");

  // A new device starts over.
  Orca::GpuLost::DeviceCreated("D3D11");
  EXPECT_FALSE(Orca::GpuLost::Lost());
  Orca::GpuLost::DeviceClosed("D3D11");
  EXPECT_EQ(State(), "D3D11 closed");
}

TEST(OrcaGpuLost, DeviceStateCutsToFit)
{
  Orca::GpuLost::SetDeviceState("D3D11 running");
  char out[6];
  EXPECT_EQ(Orca::GpuLost::DeviceState(out, sizeof(out)), 5u);
  EXPECT_STREQ(out, "D3D11");
  EXPECT_EQ(Orca::GpuLost::DeviceState(out, 0), 0u);

  // A state longer than the store keeps its start.
  const std::string longer(400, 'x');
  Orca::GpuLost::SetDeviceState(longer);
  EXPECT_EQ(State(), std::string(200, 'x'));
}
