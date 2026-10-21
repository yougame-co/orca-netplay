// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/Orca/Session/Session.h"
#include "InputCommon/GCPadStatus.h"

// Orca's 8-byte wire form of a GameCube pad; all zeros means connected, nothing pressed, sticks
// centred. Both players decode every port, their own included, from these bytes, so whatever the
// encoding drops (analog A/B, origin bits) is dropped on both machines alike.
namespace Orca::Net
{
// Button and trigger bits a game can read. PAD_USE_ORIGIN and PAD_GET_ORIGIN are local calibration
// state and never travel.
constexpr u16 PAD_WIRE_BUTTONS = 0x1F7F;
static_assert((PAD_WIRE_BUTTONS >> 8 & PAD_WIRE_UNPLUGGED) == 0,
              "the unplugged mark must not be a button bit");

inline Pad EncodePad(const GCPadStatus& status)
{
  const u16 buttons = status.button & PAD_WIRE_BUTTONS;
  return Pad{static_cast<u8>(buttons >> 8),
             static_cast<u8>(buttons & 0xFF),
             static_cast<u8>(status.stickX - GCPadStatus::MAIN_STICK_CENTER_X),
             static_cast<u8>(status.stickY - GCPadStatus::MAIN_STICK_CENTER_Y),
             static_cast<u8>(status.substickX - GCPadStatus::C_STICK_CENTER_X),
             static_cast<u8>(status.substickY - GCPadStatus::C_STICK_CENTER_Y),
             status.triggerLeft,
             status.triggerRight};
}

inline GCPadStatus DecodePad(const Pad& pad)
{
  GCPadStatus status;
  if (IsUnplugged(pad))
  {
    // A disconnected pad answers nothing, so the game sees an empty port.
    status.isConnected = false;
    return status;
  }
  status.button = static_cast<u16>((pad[0] << 8) | pad[1]) & PAD_WIRE_BUTTONS;
  status.stickX = static_cast<u8>(pad[2] + GCPadStatus::MAIN_STICK_CENTER_X);
  status.stickY = static_cast<u8>(pad[3] + GCPadStatus::MAIN_STICK_CENTER_Y);
  status.substickX = static_cast<u8>(pad[4] + GCPadStatus::C_STICK_CENTER_X);
  status.substickY = static_cast<u8>(pad[5] + GCPadStatus::C_STICK_CENTER_Y);
  status.triggerLeft = pad[6];
  status.triggerRight = pad[7];
  status.analogA = 0;
  status.analogB = 0;
  status.isConnected = true;
  return status;
}
}  // namespace Orca::Net
