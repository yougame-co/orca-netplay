// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/OrbCombo.h"

#include <string_view>

#include "Common/Logging/Log.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/OverlayKey.h"

namespace Orca::UX
{
namespace
{
void Print(std::string_view line)
{
  if (!line.empty())
    Status::Line(line);
}
}  // namespace

OrbCombo::Step OrbCombo::Next(u16 buttons)
{
  const bool start = (buttons & START) != 0;
  Step step;
  // Start pressed (not held from before) while Up is down.
  if (!m_held && start && !m_start && (buttons & UP) != 0)
  {
    m_held = true;
    m_pressing = true;
    step.fired = true;
  }
  else if (m_pressing && !start)
  {
    m_pressing = false;
    step.released = true;
  }
  m_start = start;
  if (m_held)
  {
    // Unmask once both Start and the D-pad are released.
    if ((buttons & (START | DPAD)) == 0)
      m_held = false;
    buttons &= static_cast<u16>(~(START | DPAD));
  }
  step.buttons = buttons;
  return step;
}

GCPadStatus FilterLocalPad(GCPadStatus pad, bool first_run, bool live, OverlayKey::Ms now)
{
  static OrbCombo s_combo;
  if (first_run && live)
  {
    const bool start = (pad.button & OrbCombo::START) != 0;
    const OrbCombo::Step step = s_combo.Next(pad.button);
    pad.button = step.buttons;
    if (step.fired)
    {
      INFO_LOG_FMT(CORE, "Orca: the YouGame shortcut (Up + Start)");
      Print(OverlayKey::Pad().Down(Status::Cap(Status::TALK_CAP), now));
    }
    else if (step.released)
    {
      Print(OverlayKey::Pad().Release(now));
    }
    else if (start)
    {
      OverlayKey::Pad().Seen(now);
    }
  }
  else
  {
    if (!live)
      Print(OverlayKey::Pad().Cancel());
    if (s_combo.Held())
      pad.button &= static_cast<u16>(~(OrbCombo::START | OrbCombo::DPAD));
  }
  return pad;
}

void LocalPadWaiting(std::optional<u16> buttons, OverlayKey::Ms now)
{
  // Not s_combo: its mask moves with frames only, so the pads the session sends stay the same.
  OverlayKey::Press& press = OverlayKey::Pad();
  if (!press.IsDown() || !buttons)
    return;
  if ((*buttons & OrbCombo::START) != 0)
    press.Seen(now);
  else
    Print(press.Release(now));
}

void LocalPadStopped()
{
  Print(OverlayKey::Pad().Cancel());
}
}  // namespace Orca::UX
