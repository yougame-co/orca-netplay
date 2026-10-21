// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/OverlayKey.h"

#include <chrono>

#include "Core/Orca/Status.h"

namespace Orca::UX::OverlayKey
{
namespace
{
struct Lines
{
  std::string_view down, up, cancel;
};
constexpr Lines KEYS{"orca overlay down keys", "orca overlay up keys", "orca overlay cancel keys"};
constexpr Lines PAD{"orca overlay down pad", "orca overlay up pad", "orca overlay cancel pad"};
}  // namespace

Ms NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string_view Press::Down(bool talk, Ms now)
{
  std::lock_guard lock(m_lock);
  if (m_down)
    return {};
  if (!talk)
    return TOGGLE;
  m_down = true;
  m_down_at = now;
  m_seen_at = now;
  return (m_source == Source::Keys ? KEYS : PAD).down;
}

std::string_view Press::Up()
{
  std::lock_guard lock(m_lock);
  if (!m_down)
    return {};
  m_down = false;
  return (m_source == Source::Keys ? KEYS : PAD).up;
}

void Press::Seen(Ms now)
{
  std::lock_guard lock(m_lock);
  if (m_down && now > m_seen_at)
    m_seen_at = now;
}

std::string_view Press::Release(Ms now)
{
  std::lock_guard lock(m_lock);
  if (!m_down)
    return {};
  m_down = false;
  const Lines& lines = m_source == Source::Keys ? KEYS : PAD;
  if (now - m_seen_at > READ_GAP_MS && now - m_down_at >= PAGE_HOLD_MS)
    return lines.cancel;
  return lines.up;
}

std::string_view Press::Cancel()
{
  std::lock_guard lock(m_lock);
  if (!m_down)
    return {};
  m_down = false;
  return (m_source == Source::Keys ? KEYS : PAD).cancel;
}

bool Press::IsDown() const
{
  std::lock_guard lock(m_lock);
  return m_down;
}

Press& Keys()
{
  static Press s_keys(Source::Keys);
  return s_keys;
}

Press& Pad()
{
  static Press s_pad(Source::Pad);
  return s_pad;
}

void Emit(std::string_view line, Output output)
{
  if (!line.empty())
    (output ? output : Status::Line)(line);
}

void Tap(Output output)
{
  Emit(Keys().Down(Status::Cap(Status::TALK_CAP)), output);
  Emit(Keys().Up(), output);
}
}  // namespace Orca::UX::OverlayKey
