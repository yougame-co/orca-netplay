// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include <gtest/gtest.h>

#include "Core/IOS/IOS.h"

// The session device budget (IOS::HLE::SessionAllowsDevice): a guest in an Orca session opens only
// what Brawl and Project+ open, whichever IOS it reloads into.
TEST(OrcaDeviceBudget, WhatTheGamesOpenIsAllowed)
{
  for (const std::string_view path :
       {"/dev/di", "/dev/es", "/dev/fs", "/dev/aes", "/dev/sha", "/dev/stm/immediate",
        "/dev/stm/eventhook", "/dev/sdio/slot0", "/dev/sdio/slot1", "/dev/usb/oh1",
        "/dev/usb/oh1/57e/305", "/dev/net/kd/request", "/dev/net/kd/time", "/dev/net/ncd/manage"})
  {
    EXPECT_TRUE(IOS::HLE::SessionAllowsDevice(path)) << path;
  }
}

TEST(OrcaDeviceBudget, EverythingElseIsAbsent)
{
  // WFS (host files under <user>/WFS after a reload into IOS59), USB, the keyboard, the network
  // stack, Wi-Fi, Dolphin's own device, cIOS names, and near misses.
  for (const std::string_view path :
       {"/dev/usb/wfssrv", "/dev/wfsi", "/dev/usb/hid", "/dev/usb/ven", "/dev/usb/kbd",
        "/dev/usb/oh0", "/dev/usb/oh0/57e/308", "/dev/net/ip/top", "/dev/net/ssl",
        "/dev/net/wd/command", "/dev/dolphin", "/dev/flash", "/dev/mload", "/dev/usb123",
        "/dev/fs/", "/dev/fsx", "/dev/es ", "/dev/", "/dev/usb/oh1/57e/305/x", "/dev/DI"})
  {
    EXPECT_FALSE(IOS::HLE::SessionAllowsDevice(path)) << path;
  }
}

TEST(OrcaDeviceBudget, NandPathsAreNotDevices)
{
  // Files go through /dev/fs, which keeps them in the session NAND.
  EXPECT_TRUE(IOS::HLE::SessionAllowsDevice("/title/00010000/52534245/data/advsv0.bin"));
  EXPECT_TRUE(IOS::HLE::SessionAllowsDevice("/shared2/sys/SYSCONF"));
  EXPECT_TRUE(IOS::HLE::SessionAllowsDevice("/tmp/0765"));
}
