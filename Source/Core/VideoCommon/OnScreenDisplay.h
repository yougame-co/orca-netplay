// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/Assets/CustomTextureData.h"

namespace OSD
{
enum class MessageType
{
  NetPlayPing,
  NetPlayBuffer,
  // Orca: status line for a player joining a match; each one replaces the last.
  OrcaJoin,

  // This entry must be kept last so that persistent typed messages are
  // displayed before other messages
  Typeless,
};

namespace Color
{
constexpr u32 CYAN = 0xFF00FFFF;
constexpr u32 GREEN = 0xFF00FF00;
constexpr u32 RED = 0xFFFF0000;
constexpr u32 YELLOW = 0xFFFFFF30;
}  // namespace Color

namespace Duration
{
constexpr u32 SHORT = 2000;
constexpr u32 NORMAL = 5000;
constexpr u32 VERY_LONG = 10000;
}  // namespace Duration

// On-screen message display (colored yellow by default)
void AddMessage(std::string message, u32 ms = Duration::SHORT, u32 argb = Color::YELLOW,
                const VideoCommon::CustomTextureData::ArraySlice::Level* icon = nullptr);
void AddTypedMessage(MessageType type, std::string message, u32 ms = Duration::SHORT,
                     u32 argb = Color::YELLOW,
                     const VideoCommon::CustomTextureData::ArraySlice::Level* icon = nullptr);

// Draw the current messages on the screen. Only call once per frame.
void DrawMessages();
void ClearMessages();
// Orca: removes the message of this type from the screen.
void DiscardTypedMessage(MessageType type);

// Orca: an extra ImGui layer (the online overlay) drawn after the messages on every presented
// frame. Pass an empty function to clear it. `release` frees what the layer made with g_gfx: the
// backend calls it on the video thread as it shuts down, while its device is still alive.
void SetHostOverlay(std::function<void()> draw, std::function<void()> release = {});
void DrawHostOverlay();
void ReleaseHostOverlay();
// Orca: y position in pixels where this frame's messages end, or 0 with none, so the host
// overlay can stay clear of them.
float MessagesBottom();

void SetObscuredPixelsLeft(int width);
void SetObscuredPixelsTop(int height);
}  // namespace OSD
