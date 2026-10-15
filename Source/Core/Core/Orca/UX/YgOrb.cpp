// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/YgOrb.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <system_error>

#include "Core/Orca/UX/Chat.h"

namespace Orca::UX::YgOrb
{
namespace
{
// Splits at single spaces. Empty fields are kept, so the caller rejects "a  b".
std::vector<std::string_view> Fields(std::string_view line)
{
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at <= line.size())
  {
    const std::size_t space = line.find(' ', at);
    const std::size_t end = space == std::string_view::npos ? line.size() : space;
    out.push_back(line.substr(at, end - at));
    if (space == std::string_view::npos)
      break;
    at = space + 1;
  }
  return out;
}

// A whole decimal number in [lo, hi], digits only.
std::optional<int> Number(std::string_view field, int lo, int hi)
{
  if (field.empty() || field.size() > 5 ||
      !std::all_of(field.begin(), field.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  int value = 0;
  const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (ec != std::errc() || end != field.data() + field.size() || value < lo || value > hi)
    return std::nullopt;
  return value;
}

std::optional<Kind> KindOf(std::string_view field)
{
  if (field == "join")
    return Kind::Join;
  if (field == "leave")
    return Kind::Leave;
  if (field == "away")
    return Kind::Away;
  if (field == "back")
    return Kind::Back;
  if (field == "voice")
    return Kind::Voice;
  if (field == "call")
    return Kind::Call;
  if (field == "dm")
    return Kind::Dm;
  if (field == "invite")
    return Kind::Invite;
  return std::nullopt;
}

std::size_t CodePoints(std::string_view text)
{
  return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }));
}
}  // namespace

std::optional<Command> ParseOrb(std::string_view line)
{
  const std::vector<std::string_view> f = Fields(line);
  if (f.empty() || f[0] != "orb")
    return std::nullopt;
  Command c;
  if (f.size() == 2 && f[1] == "off")
  {
    c.type = Command::Type::Off;
    return c;
  }
  if (f.size() == 2 && f[1] == "blink")
  {
    c.type = Command::Type::Blink;
    return c;
  }
  if (f.size() != 7)
    return std::nullopt;
  const std::optional<int> x = Number(f[1], 0, 32767), y = Number(f[2], 0, 32767),
                           size = Number(f[3], 8, 1024), lit = Number(f[4], 0, 1),
                           away = Number(f[5], 0, 1), badge = Number(f[6], 0, 99);
  if (!x || !y || !size || !lit || !away || !badge)
    return std::nullopt;
  c.type = Command::Type::Place;
  c.place = Place{static_cast<float>(*x),
                  static_cast<float>(*y),
                  static_cast<float>(*size),
                  *lit == 1,
                  *away == 1,
                  *badge};
  return c;
}

std::optional<Notice> ParseNotice(std::string_view line)
{
  const std::vector<std::string_view> f = Fields(line);
  if ((f.size() != 4 && f.size() != 6) || f[0] != "notice")
    return std::nullopt;
  const std::optional<Kind> kind = KindOf(f[1]);
  if (!kind)
    return std::nullopt;
  Notice n;
  n.kind = *kind;
  std::optional<std::string> name = Chat::DecodeField(f[2], Chat::NAME_ENCODED, Chat::NAME_POINTS);
  std::optional<std::string> text = Chat::DecodeField(f[3], Chat::TEXT_ENCODED, Chat::TEXT_POINTS);
  if (!name || !text)
    return std::nullopt;
  n.name = std::move(*name);
  n.text = std::move(*text);
  if (f.size() == 6)
  {
    std::optional<std::string> key = Chat::DecodeField(f[4], HINT_POINTS * 12, HINT_POINTS);
    std::optional<std::string> verb = Chat::DecodeField(f[5], HINT_POINTS * 12, HINT_POINTS);
    if (!key || !verb)
      return std::nullopt;
    n.key = std::move(*key);
    n.verb = std::move(*verb);
  }
  return n;
}

std::optional<Talk> ParseTalk(std::string_view line)
{
  const std::vector<std::string_view> f = Fields(line);
  if (f.empty() || f[0] != "talk")
    return std::nullopt;
  Talk t;
  if (f.size() == 2 && f[1] == "off")
    return t;
  if (f.size() != 4)
    return std::nullopt;
  if (f[1] == "listen")
    t.phase = Talk::Phase::Listen;
  else if (f[1] == "send")
    t.phase = Talk::Phase::Send;
  else if (f[1] == "sent")
    t.phase = Talk::Phase::Sent;
  else if (f[1] == "error")
    t.phase = Talk::Phase::Error;
  else
    return std::nullopt;
  std::optional<std::string> name = Chat::DecodeField(f[2], Chat::NAME_ENCODED, Chat::NAME_POINTS);
  std::optional<std::string> text = Chat::DecodeField(f[3], Chat::TEXT_ENCODED, Chat::TEXT_POINTS);
  if (!name || !text)
    return std::nullopt;
  t.name = std::move(*name);
  t.text = std::move(*text);
  return t;
}

double TalkDwellMs(Talk::Phase phase)
{
  switch (phase)
  {
  case Talk::Phase::Off:
    return 0;
  case Talk::Phase::Listen:
  case Talk::Phase::Send:
    return TALK_STALE_MS;
  case Talk::Phase::Sent:
    return 3000;
  case Talk::Phase::Error:
    return 4000;
  }
  return 0;
}

float TalkAlpha(const Talk& talk, double at_ms, double now_ms)
{
  const double left = TalkDwellMs(talk.phase) - (now_ms - at_ms);
  if (left <= 0)
    return 0;
  return static_cast<float>(std::min(1.0, left / Model::FADE_MS));
}

Place ScaledPlace(const Place& place, float view_width, float display_width)
{
  if (!(view_width > 0) || !(display_width > 0))
    return place;
  const float k = display_width / view_width;
  Place p = place;
  p.x *= k;
  p.y *= k;
  p.size *= k;
  return p;
}

double DwellMs(const Notice& notice)
{
  switch (notice.kind)
  {
  case Kind::Voice:
  case Kind::Call:
    return 8000;
  case Kind::Join:
    return 3500;
  case Kind::Leave:
  case Kind::Back:
    return 2500;
  case Kind::Away:
    return 5000;
  case Kind::Dm:
  case Kind::Invite:
    break;
  }
  // Messages: 4 s plus 60 ms per character, up to 9 s.
  return std::min(9000.0, 4000.0 + 60.0 * static_cast<double>(CodePoints(notice.text)));
}

void Model::Apply(const Command& command, double now_ms)
{
  std::lock_guard lock(m_lock);
  switch (command.type)
  {
  case Command::Type::Off:
    // The lines hide with the button.
    m_on = false;
    m_blink_at = -1;
    if (!m_lines.empty())
    {
      m_lines.clear();
      ++m_generation;
    }
    if (m_talk.talk.phase != Talk::Phase::Off)
    {
      m_talk = TalkEntry{};
      ++m_talk_generation;
    }
    break;
  case Command::Type::Blink:
    m_blink_at = now_ms;
    break;
  case Command::Type::Place:
    m_on = true;
    m_place = command.place;
    break;
  }
}

float LineAlpha(const Notice& notice, double at_ms, double now_ms)
{
  const double left = DwellMs(notice) - (now_ms - at_ms);
  if (left <= 0)
    return 0;
  return static_cast<float>(std::min(1.0, left / Model::FADE_MS));
}

void Model::Add(Notice notice, double now_ms)
{
  std::lock_guard lock(m_lock);
  std::erase_if(m_lines, [&](const Entry& e) { return LineAlpha(e.notice, e.at, now_ms) <= 0; });
  m_lines.push_back(Entry{now_ms, std::move(notice)});
  while (m_lines.size() > KEEP)
    m_lines.pop_front();
  ++m_generation;
}

void Model::SetTalk(Talk talk, double now_ms)
{
  std::lock_guard lock(m_lock);
  if (talk.phase == Talk::Phase::Listen && m_talk.talk.phase != Talk::Phase::Listen)
    m_listen_at = now_ms;
  m_talk = TalkEntry{now_ms, std::move(talk)};
  ++m_talk_generation;
}

Model::View Model::At(double now_ms) const
{
  std::lock_guard lock(m_lock);
  View v;
  v.on = m_on;
  v.place = m_place;
  if (m_on && m_blink_at >= 0)
  {
    const double age = now_ms - m_blink_at;
    if (age >= 0 && age < RING_MS * RINGS)
      v.ring = static_cast<float>(std::fmod(age, RING_MS) / RING_MS);
  }
  const bool talking = TalkAlpha(m_talk.talk, m_talk.at, now_ms) > 0;
  // A blink comes first; otherwise the listening pulse, from the line that opened the mic.
  if (m_on && v.ring < 0 && talking && m_talk.talk.phase == Talk::Phase::Listen)
  {
    const double age = std::max(0.0, now_ms - m_listen_at);
    v.ring = static_cast<float>(std::fmod(age, LISTEN_RING_MS) / LISTEN_RING_MS);
    v.listening = true;
  }
  v.generation = m_generation;
  v.talk_generation = m_talk_generation;
  v.view_width = static_cast<float>(m_view_width);
  v.showing = talking || std::any_of(m_lines.begin(), m_lines.end(), [&](const Entry& e) {
                return LineAlpha(e.notice, e.at, now_ms) > 0;
              });
  return v;
}

void Model::SetView(int width, int height)
{
  std::lock_guard lock(m_lock);
  m_view_width = width > 0 && height > 0 ? width : 0;
}

std::uint64_t Model::Lines(std::uint64_t generation, std::vector<Entry>* out) const
{
  std::lock_guard lock(m_lock);
  if (generation != m_generation)
    out->assign(m_lines.begin(), m_lines.end());
  return m_generation;
}

std::uint64_t Model::TalkRow(std::uint64_t generation, TalkEntry* out) const
{
  std::lock_guard lock(m_lock);
  if (generation != m_talk_generation)
    *out = m_talk;
  return m_talk_generation;
}

bool Model::Hit(float x, float y) const
{
  std::lock_guard lock(m_lock);
  if (!m_on || m_place.size <= 0)
    return false;
  const float r = m_place.size / 2;
  const float dx = x - (m_place.x + r), dy = y - (m_place.y + r);
  return dx * dx + dy * dy <= r * r;
}

Model& Current()
{
  static Model s_model;
  return s_model;
}

double NowMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Apply(const Command& command)
{
  Current().Apply(command, NowMs());
}

void Show(Notice notice)
{
  Current().Add(std::move(notice), NowMs());
}

void SetTalk(Talk talk)
{
  Current().SetTalk(std::move(talk), NowMs());
}

void SetViewSize(int width, int height)
{
  Current().SetView(width, height);
}
}  // namespace Orca::UX::YgOrb
