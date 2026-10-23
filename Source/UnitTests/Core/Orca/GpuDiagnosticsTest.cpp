// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "Core/Orca/GpuDiagnostics.h"

namespace
{
namespace Diag = Orca::GpuDiagnostics;
constexpr u64 MB = 1024 * 1024;
using namespace std::chrono_literals;
}  // namespace

TEST(OrcaGpuDiagnostics, MemoryIncludesSystemProcessAndJob)
{
  char text[384];
  Diag::Memory memory{17571 * MB, 18725 * MB, 4210 * MB, 812 * MB, 1103 * MB,
                      "yes", 1250 * MB, 6144 * MB};
  const size_t length = Diag::FormatMemory(text, sizeof(text), memory);
  EXPECT_EQ(std::string(text, length), "orca mem commit 17571/18725 MB avail 4210 MB ws 812 MB "
                                      "priv 1103 MB job peak 1250/6144 MB");
  memory.job_limit = 0;
  Diag::FormatMemory(text, sizeof(text), memory);
  EXPECT_NE(std::string(text).find("job peak 1250/unlimited MB"), std::string::npos);
}

TEST(OrcaGpuDiagnostics, MissingQueriesAreNotReportedAsZero)
{
  char text[384];
  Diag::Memory memory;
  Diag::FormatMemory(text, sizeof(text), memory);
  EXPECT_STREQ(text, "orca mem commit unknown/unknown MB avail unknown MB ws unknown MB "
                     "priv unknown MB job none");
  memory.job = "yes";
  Diag::FormatMemory(text, sizeof(text), memory);
  EXPECT_NE(std::string(text).find("job peak unknown/unknown MB"), std::string::npos);
  memory.job = "unknown";
  Diag::FormatMemory(text, sizeof(text), memory);
  EXPECT_NE(std::string(text).find("job unknown"), std::string::npos);
}

TEST(OrcaGpuDiagnostics, ByteValuesUseWholeMegabytesWithoutNarrowing)
{
  char text[384];
  Diag::Memory memory;
  memory.commit = std::numeric_limits<u64>::max();
  memory.limit = 9 * MB + MB - 1;
  memory.available = 0;
  Diag::FormatMemory(text, sizeof(text), memory);
  EXPECT_NE(std::string(text).find("commit 17592186044415/9 MB avail 0 MB"), std::string::npos);
}

TEST(OrcaGpuDiagnostics, BothGpuSegmentsAndQueryFailures)
{
  char text[384];
  Diag::FormatGpuMemory(text, sizeof(text), {0, 4096 * MB, 1536 * MB, 256 * MB},
                        {0, 8192 * MB, 64 * MB, 512 * MB});
  EXPECT_STREQ(text, "orca gpu mem LOCAL budget 4096 MB usage 1536 MB reservation-available 256 MB "
                     "NON_LOCAL budget 8192 MB usage 64 MB reservation-available 512 MB");
  Diag::FormatGpuMemory(text, sizeof(text), {0x887A0005, 0, 0, 0}, {0x80004002, 0, 0, 0});
  EXPECT_STREQ(text, "orca gpu mem LOCAL unavailable 0x887A0005 NON_LOCAL unavailable 0x80004002");
}

TEST(OrcaGpuDiagnostics, AdapterIdentityAndCapacity)
{
  char text[768];
  Diag::FormatAdapter(text, sizeof(text), {"NVIDIA GeForce RTX 2060\n", "32.0.15.9999", 0x10de,
                                          0x1f08, 6144 * MB, 0, 8192 * MB});
  EXPECT_STREQ(text, "orca gpu adapter NVIDIA GeForce RTX 2060  vendor 0x10DE device 0x1F08 "
                     "driver 32.0.15.9999 dedicated-video 6144 MB dedicated-system 0 MB "
                     "shared-system 8192 MB");
}

TEST(OrcaGpuDiagnostics, LossContainsRenderAndSceneContext)
{
  char text[512];
  Diag::Render render;
  render.backend = "D3D11";
  render.mode = "asynchronous-uber";
  render.feature_level = 0xb000;
  render.removed_reason = 0x887a0006;
  render.scale = 2;
  render.pixel = 300;
  render.vertex = 90;
  render.uber_pixel = 16;
  render.uber_vertex = 8;
  render.presented = 4250;
  render.scene = "scMelee";
  render.scene_frame = 4000;
  render.frame = 4249;
  Diag::FormatRender(text, sizeof(text), render);
  EXPECT_STREQ(text, "orca gpu loss backend D3D11 feature 0xB000 shaders asynchronous-uber scale 2 "
                     "pixel 300 vertex 90 uber-pixel 16 uber-vertex 8 presented 4250 scene scMelee "
                     "scene-at 4000 frame 4249 removed 0x887A0006");
  render.scene = "unknown";
  render.scene_frame.reset();
  render.frame.reset();
  render.removed_reason = 0;
  Diag::FormatRender(text, sizeof(text), render);
  EXPECT_NE(
      std::string(text).find("scene unknown scene-at unknown frame unknown removed 0x00000000"),
      std::string::npos);
}

TEST(OrcaGpuDiagnostics, TdrDefaultsExplicitOverridesAndReadFailure)
{
  char text[160];
  const std::string_view defaults[] = {"default", "default", "default"};
  Diag::FormatTdr(text, sizeof(text), defaults);
  EXPECT_STREQ(text, "orca gpu tdr TdrDelay default TdrDdiDelay default TdrLevel default");
  const std::string_view overrides[] = {"10", "20", "0"};
  Diag::FormatTdr(text, sizeof(text), overrides);
  EXPECT_STREQ(text, "orca gpu tdr TdrDelay 10 TdrDdiDelay 20 TdrLevel 0");
  const std::string_view failed[] = {"unknown", "default", "4294967295"};
  Diag::FormatTdr(text, sizeof(text), failed);
  EXPECT_STREQ(text, "orca gpu tdr TdrDelay unknown TdrDdiDelay default TdrLevel 4294967295");
}

TEST(OrcaGpuDiagnostics, EveryFormatterBoundsAndTerminatesItsOutput)
{
  const auto formats = std::array{
      +[](char* out, size_t size) { return Diag::FormatMemory(out, size, {}); },
      +[](char* out, size_t size) { return Diag::FormatGpuMemory(out, size, {}, {}); },
      +[](char* out, size_t size) { return Diag::FormatAdapter(out, size, {}); },
      +[](char* out, size_t size) { return Diag::FormatRender(out, size, {}); },
      +[](char* out, size_t size) {
        const std::string_view values[] = {"default", "default", "default"};
        return Diag::FormatTdr(out, size, values);
      }};
  for (const auto format : formats)
  {
    char text[8] = {};
    text[7] = '!';
    EXPECT_EQ(format(text, 7), 6u);
    EXPECT_EQ(text[6], '\0');
    EXPECT_EQ(text[7], '!');
    EXPECT_EQ(format(text, 1), 0u);
    EXPECT_EQ(text[0], '\0');
    EXPECT_EQ(format(nullptr, 0), 0u);
  }
}

TEST(OrcaGpuDiagnostics, SixtySecondBoundaryAndMonotonicClock)
{
  const auto start = Diag::Clock::time_point{};
  EXPECT_TRUE(Diag::SampleDue(start, std::nullopt));
  EXPECT_FALSE(Diag::SampleDue(start, start));
  EXPECT_FALSE(Diag::SampleDue(start + 60s - 1ns, start));
  EXPECT_TRUE(Diag::SampleDue(start + 60s, start));
  EXPECT_TRUE(Diag::SampleDue(start + 600s, start));
  EXPECT_FALSE(Diag::SampleDue(start - 1s, start));
}

TEST(OrcaGpuDiagnostics, PeriodicSamplesShareLimitButBootErrorAndLossAreForced)
{
  const auto start = Diag::Clock::time_point{};
  std::optional<Diag::Clock::time_point> last;
  int samples = 0;
  const auto sample = [&](auto offset, bool force = false) {
    if (Diag::SampleDue(start + offset, last, force))
    {
      last = start + offset;
      ++samples;
      return true;
    }
    return false;
  };
  EXPECT_TRUE(sample(0s));  // periodic
  EXPECT_FALSE(sample(0s));
  EXPECT_TRUE(sample(0s, true));  // boot immediately after periodic
  EXPECT_TRUE(sample(0s, true));  // second boot
  EXPECT_TRUE(sample(0s, true));  // error
  EXPECT_TRUE(sample(0s, true));  // loss
  EXPECT_FALSE(sample(1s));
  EXPECT_FALSE(sample(59s));
  EXPECT_TRUE(sample(60s));
  EXPECT_FALSE(sample(60s));
  EXPECT_TRUE(sample(61s, true));  // error resets the periodic deadline
  EXPECT_FALSE(sample(120s));
  EXPECT_TRUE(sample(121s));
  EXPECT_EQ(samples, 8);
  EXPECT_EQ(last, start + 121s);
}

TEST(OrcaGpuDiagnostics, ProcessMemoryLimitUsesProcessPeakAndLabel)
{
  char text[384];
  Diag::Memory memory;
  memory.job = "yes";
  memory.job_peak = 512 * MB;
  memory.job_limit = 2048 * MB;
  memory.process_limit = true;
  Diag::FormatMemory(text, sizeof(text), memory);
  EXPECT_NE(std::string(text).find("proc peak 512/2048 MB"), std::string::npos);
  EXPECT_EQ(std::string(text).find("job peak"), std::string::npos);
}

TEST(OrcaGpuDiagnostics, NegativeFramesAreUnknown)
{
  EXPECT_EQ(Diag::FrameNumber(-1), std::nullopt);
  EXPECT_EQ(Diag::FrameNumber(std::numeric_limits<int>::min()), std::nullopt);
  EXPECT_EQ(Diag::FrameNumber(0), 0u);
  EXPECT_EQ(Diag::FrameNumber(std::numeric_limits<int>::max()),
            static_cast<u64>(std::numeric_limits<int>::max()));
  Diag::Render render;
  render.scene = "scMelee";
  render.scene_frame = Diag::FrameNumber(-1);
  render.frame = Diag::FrameNumber(-2);
  char text[512];
  Diag::FormatRender(text, sizeof(text), render);
  EXPECT_NE(std::string(text).find("scene scMelee scene-at unknown frame unknown"), std::string::npos);
}
