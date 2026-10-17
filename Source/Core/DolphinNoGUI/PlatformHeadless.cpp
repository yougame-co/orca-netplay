// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <thread>

#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "VideoCommon/Present.h"

namespace
{
class PlatformHeadless final : public Platform
{
public:
  void SetTitle(const std::string& title) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

protected:
  // Orca tests (ORCA_TEST_COMMANDS with ORCA_TEST_PRESENT): `rect` sizes the test image like a
  // window, so `view` and the overlay render as they would on screen.
  void EmbedSetRect(const Embed::Rect& rect) override;
};

void PlatformHeadless::EmbedSetRect(const Embed::Rect& rect)
{
  VideoCommon::Presenter::RequestTestPresentSize(rect.w, rect.h);
}

void PlatformHeadless::SetTitle(const std::string& title)
{
  std::fprintf(stdout, "%s\n", title.c_str());
}

void PlatformHeadless::MainLoop()
{
  while (m_running.IsSet())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

WindowSystemInfo PlatformHeadless::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Headless;
  wsi.display_connection = nullptr;
  wsi.render_window = nullptr;
  wsi.render_surface = nullptr;
  return wsi;
}

}  // namespace

std::unique_ptr<Platform> Platform::CreateHeadlessPlatform()
{
  return std::make_unique<PlatformHeadless>();
}
