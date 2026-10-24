// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>

#include <gtest/gtest.h>

#include "VideoCommon/VideoConfig.h"

namespace
{
class VideoConfigValidity : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_saved_backend_info = std::move(g_backend_info);
    g_backend_info = {};
    g_backend_info.Adapters = {"Adapter"};
  }

  void TearDown() override { g_backend_info = std::move(m_saved_backend_info); }

private:
  BackendInfo m_saved_backend_info;
};

TEST_F(VideoConfigValidity, KnownModesValidateByDefault)
{
  EXPECT_TRUE(g_backend_info.bAAModesReady);
  g_backend_info.AAModes = {1, 4};
  VideoConfig config;
  config.iMultisamples = 4;
  config.VerifyValidity();
  EXPECT_EQ(config.iMultisamples, 4u);

  config.iMultisamples = 8;
  config.VerifyValidity();
  EXPECT_EQ(config.iMultisamples, 1u);

  g_backend_info.AAModes.clear();
  config.iMultisamples = 4;
  config.VerifyValidity();
  EXPECT_EQ(config.iMultisamples, 1u);
}

TEST_F(VideoConfigValidity, DeferredModesPreserveSupportedAAAndValidateAdapter)
{
  g_backend_info.bAAModesReady = false;
  VideoConfig config;
  config.iAdapter = 5;
  config.iMultisamples = 8;
  config.VerifyValidity();
  EXPECT_EQ(config.iAdapter, 0);
  EXPECT_EQ(config.iMultisamples, 8u);

  g_backend_info.AAModes = {1, 4, 8};
  g_backend_info.bAAModesReady = true;
  config.VerifyValidity();
  EXPECT_EQ(config.iMultisamples, 8u);
}

TEST_F(VideoConfigValidity, DeferredUnsupportedAAIsClampedWhenReady)
{
  g_backend_info.bAAModesReady = false;
  VideoConfig config;
  config.iMultisamples = 8;
  config.VerifyValidity();
  EXPECT_EQ(config.iMultisamples, 8u);

  g_backend_info.AAModes = {1, 4};
  g_backend_info.bAAModesReady = true;
  config.VerifyValidity();
  EXPECT_EQ(config.iMultisamples, 1u);
}
}  // namespace
