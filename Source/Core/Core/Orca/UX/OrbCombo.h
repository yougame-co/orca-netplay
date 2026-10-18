// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/OverlayKey.h"
#include "InputCommon/GCPadStatus.h"

// Controller shortcut for the YouGame overlay: hold D-pad Up and press Start. A GameCube pad has no
// Home button. Orca prints `orca orb` and the app opens the overlay. With the app's "talk" cap it
// prints the press and the release of Start instead (UX/OverlayKey.h), so holding it talks.
//
// From the Start press until both are released, the game sees neither Start nor the D-pad. The
// session records and sends this masked pad, so every machine sees the same input and it cannot
// desync.
namespace Orca::UX
{
class OrbCombo
{
public:
  static constexpr u16 START = PAD_BUTTON_START;
  static constexpr u16 UP = PAD_BUTTON_UP;
  static constexpr u16 DPAD = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;

  struct Step
  {
    u16 buttons = 0;  // what the game gets
    bool fired = false;
    // Start let go after firing, once per firing. The D-pad may stay masked after it.
    bool released = false;
  };
  // Feeds one frame's sample of the local buttons. Pure, no I/O.
  Step Next(u16 buttons);
  bool Held() const { return m_held; }

private:
  bool m_held = false;      // fired; Start and the D-pad stay masked until both are released
  bool m_start = false;     // Start was down in the last sample
  bool m_pressing = false;  // fired and Start not yet let go
};

// Applies the shortcut to the local pad before the session sends it. The combo only advances on a
// first run (rollback re-runs discard their reads) while `live`, because an unfocused pad reads as
// neutral and must not look like a release: a press still down then is cancelled instead. `now`
// times the read for the release (OverlayKey::Press::Release). CPU thread only.
GCPadStatus FilterLocalPad(GCPadStatus pad, bool first_run, bool live,
                           OverlayKey::Ms now = OverlayKey::NowMs());
// While the session holds a frame on a friend's input (each stall tick) no frame reads the pad, so
// a talk press would not end until the game moved again, up to the stall limit later. `buttons` is
// the local pad as a frame would read it now (nullopt: no fresh read, as for a test script's pad);
// Start let go ends the press now. Focus is not checked here: the input gate moves only with the
// frames' pad poll, so focus lost during a stall is handled when frames resume (FilterLocalPad
// cancels), and the page's own blur already ends its side. Only the press's lines change: the
// combo and its mask advance on frames alone, so what the session records and sends is the same
// bytes as without this. Does nothing unless a talk press is down. CPU thread only.
void LocalPadWaiting(std::optional<u16> buttons, OverlayKey::Ms now = OverlayKey::NowMs());
// The local pad is no longer read (the game paused or is stopping): a press still down is
// cancelled. Any thread.
void LocalPadStopped();
}  // namespace Orca::UX
