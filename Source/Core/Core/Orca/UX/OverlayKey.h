// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <string_view>

#include "Common/CommonTypes.h"

// Press and release of YouGame's overlay key, for hold-to-talk (cap "talk"). Orca reports the two
// presses only it can see: Shift+Tab while its own window has the keyboard, and the
// controller shortcut (UX/OrbCombo.h). With "talk" agreed a press prints
//   orca overlay down keys|pad
// and then exactly one of
//   orca overlay up keys|pad       let go
//   orca overlay cancel keys|pad   no longer read (focus lost, window hidden, game paused), or a
//                                  pad let go at a time Orca cannot place (Release below)
// The page tells a tap (its menu) from a hold (talk) by the time between them. Without "talk" a
// press prints "orca orb", a toggle, and nothing follows. Host-only: nothing here touches emulated
// state.
namespace Orca::UX::OverlayKey
{
inline constexpr std::string_view TOGGLE = "orca orb";

// Milliseconds on this machine's steady clock.
using Ms = s64;
Ms NowMs();

// The page's line between a tap and a hold: let go before this is a tap (its menu), still down at
// it is a hold (the mic opens). The page's own number (hold-to-talk protocol); keep them equal.
inline constexpr Ms PAGE_HOLD_MS = 250;
// A polled pad is read every frame (17 ms at 60 Hz) and, while a frame waits on a friend, every
// stall tick. Reads further apart than this (six frames) leave the moment Start was let go unknown.
inline constexpr Ms READ_GAP_MS = 100;

enum class Source
{
  Keys,
  Pad,
};

// One source's press. Each call returns the line to print, or an empty view. Thread-safe.
class Press
{
public:
  explicit Press(Source source) : m_source(source) {}
  // Pressed (never a key repeat). `talk`: the app agreed "talk". Nothing while already down.
  std::string_view Down(bool talk, Ms now = NowMs());
  // Let go, seen as it happened (a key-up): "up", once per "down".
  std::string_view Up();
  // A polled source still down at a read (`now`).
  void Seen(Ms now);
  // A polled source found let go at a read (`now`): "up", once per "down". When the read before
  // it that still saw the press is more than READ_GAP_MS back and the press is past PAGE_HOLD_MS,
  // the release fell somewhere in that gap: a tap could read as a hold, and a hold could run on
  // past the player's release. Then it prints "cancel" instead, and the page sends nothing.
  std::string_view Release(Ms now);
  // No longer read: "cancel", once per "down".
  std::string_view Cancel();
  bool IsDown() const;

private:
  mutable std::mutex m_lock;
  const Source m_source;
  bool m_down = false;
  Ms m_down_at = 0;  // the read that saw the press
  Ms m_seen_at = 0;  // the last read that saw it still down
};

// The process-wide presses.
Press& Keys();
Press& Pad();

// Emit through the platform's overlay channel, or Status::Line for host output.
using Output = void (*)(std::string_view);
void Emit(std::string_view line, Output output = nullptr);
// A logo click or host-menu command is an immediate keyboard tap on the page's menu.
void Tap(Output output = nullptr);
}  // namespace Orca::UX::OverlayKey
