// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/YgOrbDraw.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Orca/UX/YgCard.h"

namespace Orca::UX::YgOrb
{
namespace
{
ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// Matches the app's button: half opacity at rest, 0.85 with unread lines, full when lit.
float ButtonAlpha(const Place& p)
{
  return p.lit ? 1.0f : p.badge > 0 ? 0.85f : 0.5f;
}

std::string BadgeText(int badge)
{
  return badge > 9 ? "9+" : std::to_string(badge);
}

// The badge: 19 high, at least 19 wide, 6 above the top and 7 past the right, on the button's
// 40-unit grid scaled by `k`.
Kit::Box BadgeBox(const Place& p, float k)
{
  const float px = 11.5f * k;
  const float w = std::max(19.0f * k, Yg::TextWidth(BadgeText(p.badge), px) + 8.0f * k);
  const float right = p.x + p.size + 7.0f * k, top = p.y - 6.0f * k;
  return Kit::Box{ImVec2(right - w, top), ImVec2(right, top + 19.0f * k)};
}
}  // namespace

Kit::Box ButtonBox(const Model::View& view)
{
  if (!view.on || view.place.size <= 0)
    return Kit::Box{ImVec2(0, 0), ImVec2(0, 0)};
  const Place p = ScaledPlace(view.place, view.view_width, ImGui::GetIO().DisplaySize.x);
  const float k = p.size / 40;
  Kit::Box box{ImVec2(p.x, p.y), ImVec2(p.x + p.size, p.y + p.size)};
  if (p.away)
  {
    box.max.x = std::max(box.max.x, p.x + p.size + 5.0f * k);
    box.max.y = std::max(box.max.y, p.y + p.size + 5.0f * k);
  }
  if (p.badge > 0)
  {
    const Kit::Box b = BadgeBox(p, k);
    box.min.y = std::min(box.min.y, b.min.y - 2.0f * k);
    box.max.x = std::max(box.max.x, b.max.x + 2.0f * k);
  }
  return box;
}

void DrawButton(ImDrawList* dl, const Model::View& view)
{
  if (!view.on || view.place.size <= 0)
    return;
  const Place p = ScaledPlace(view.place, view.view_width, ImGui::GetIO().DisplaySize.x);
  const float k = p.size / 40, alpha = ButtonAlpha(p);
  const ImVec2 c(p.x + p.size / 2, p.y + p.size / 2);
  Yg::RoundLogo(dl, ImVec2(p.x, p.y), p.size, alpha);
  // Blink: a white ring grows from the disc's edge to 2.1x its size and fades. Listening: the same
  // ring in voice green, over and over.
  if (view.ring >= 0)
  {
    const float scale = 1 + 1.1f * view.ring;
    const float fade = 0.95f * (1 - view.ring);
    dl->AddCircle(c, (p.size / 2 - k) * scale,
                  view.listening ? Rgba(43, 166, 64, fade) : Rgba(255, 255, 255, fade), 48,
                  2 * k * scale);
  }
  // Held seat: an amber dot at the bottom right.
  if (p.away)
  {
    const ImVec2 dot(p.x + p.size - 3.5f * k, p.y + p.size - 3.5f * k);
    dl->AddCircleFilled(dot, 8.5f * k, Rgba(0, 0, 0, alpha), 24);
    dl->AddCircleFilled(dot, 6.5f * k, Rgba(242, 161, 20, alpha), 24);
  }
  // Unread count badge at the top right.
  if (p.badge > 0)
  {
    const Kit::Box b = BadgeBox(p, k);
    const float r = (b.max.y - b.min.y) / 2;
    dl->AddRectFilled(ImVec2(b.min.x - 2 * k, b.min.y - 2 * k),
                      ImVec2(b.max.x + 2 * k, b.max.y + 2 * k), Rgba(0, 0, 0, alpha), r + 2 * k);
    dl->AddRectFilled(b.min, b.max, Rgba(255, 255, 255, alpha), r);
    const std::string text = BadgeText(p.badge);
    const float px = 11.5f * k;
    const float w = Yg::TextWidth(text, px);
    Yg::Text(dl, ImVec2((b.min.x + b.max.x - w) / 2, b.min.y + (b.max.y - b.min.y - px) / 2), px,
             Rgba(221, 0, 0, alpha), text);
  }
}

namespace
{
// Notices as last copied out by Model::Lines. Video thread only.
struct NoticeCache
{
  std::uint64_t generation = 0;
  std::vector<Model::Entry> entries;
};
NoticeCache s_notices;

// Hold-to-talk's row as last copied out by Model::TalkRow. Video thread only.
struct TalkCache
{
  std::uint64_t generation = 0;
  Model::TalkEntry entry;
};
TalkCache s_talk;

// Small cache of avatar colours and initials, so names aren't converted every frame. Video thread
// only.
struct AvatarMemo
{
  std::string name;
  ImU32 colour = 0;
  std::string initial;
};
std::array<AvatarMemo, 8> s_avatars;
std::size_t s_avatar_next = 0;
const AvatarMemo& AvatarFor(std::string_view name)
{
  for (const AvatarMemo& a : s_avatars)
  {
    if (!a.name.empty() && a.name == name)
      return a;
  }
  AvatarMemo& a = s_avatars[s_avatar_next++ % s_avatars.size()];
  a.name = name;
  a.colour = Yg::AvatarColour(name);
  a.initial = Yg::InitialOf(name);
  return a;
}
void DrawAvatar(ImDrawList* dl, ImVec2 c, float r, std::string_view name, float alpha)
{
  const AvatarMemo& a = AvatarFor(name);
  Yg::AvatarOf(dl, c, r, a.colour, a.initial, alpha);
}

// Pill metrics matching the app's own pill, on the button's 40-unit grid scaled by `k`. Text starts
// 54 in, clearing the button.
struct Pill
{
  float k, pad_left, pad_right, pad_y, gap, avatar, avatar_gap, name_px, text_px, line, hint_gap,
      key_px, key_h, key_pad, verb_px, max_width, radius;
};
Pill PillFor(float k)
{
  Pill m;
  m.k = k;
  m.pad_left = 54 * k;
  m.pad_right = 12 * k;
  m.pad_y = 7 * k;
  m.gap = 6 * k;
  m.avatar = 26 * k;
  m.avatar_gap = 10 * k;
  m.name_px = 13.5f * k;
  m.text_px = 14.5f * k;
  m.line = 1.35f;
  m.hint_gap = 12 * k;
  m.key_px = 11 * k;
  m.key_h = 18 * k;
  m.key_pad = 5 * k;
  m.verb_px = 12.5f * k;
  m.max_width = 480 * k;
  m.radius = 20 * k;
  return m;
}

constexpr std::string_view ELLIPSIS = "…";

// Fits `text` on one line. Returns the kept prefix and whether it was cut (draw "…" after it).
std::pair<std::string_view, bool> OneLine(std::string_view text, float px, float width)
{
  const auto measure = [px](std::string_view t) { return Yg::TextWidth(t, px); };
  if (measure(text) <= width)
    return {text, false};
  const float room = std::max(0.0f, width - measure(ELLIPSIS));
  std::string_view head = text.substr(0, FitPrefix(text, room, measure));
  while (!head.empty() && head.back() == ' ')
    head.remove_suffix(1);
  return {head, true};
}

// Fits `text` on one line keeping its end (live words: the newest). Returns the kept suffix and
// whether it was cut (draw "…" before it).
std::pair<std::string_view, bool> TailLine(std::string_view text, float px, float width)
{
  const auto measure = [px](std::string_view t) { return Yg::TextWidth(t, px); };
  if (measure(text) <= width)
    return {text, false};
  const float room = std::max(0.0f, width - measure(ELLIPSIS));
  std::string_view tail = text.substr(FitSuffix(text, room, measure));
  while (!tail.empty() && tail.front() == ' ')
    tail.remove_prefix(1);
  return {tail, true};
}

// A chat message wrapped to at most two lines.
struct TwoLines
{
  std::string_view first, second;
  bool cut = false;
};
TwoLines Wrap2(std::string_view text, float px, float width)
{
  const auto measure = [px](std::string_view t) { return Yg::TextWidth(t, px); };
  TwoLines out;
  std::size_t n = FitPrefix(text, width, measure);
  // If nothing fits, take one whole UTF-8 character anyway.
  if (n == 0 && !text.empty())
  {
    n = 1;
    while (n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
      ++n;
  }
  out.first = text.substr(0, n);
  std::string_view rest = text.substr(out.first.size());
  while (!out.first.empty() && out.first.back() == ' ')
    out.first.remove_suffix(1);
  while (!rest.empty() && rest.front() == ' ')
    rest.remove_prefix(1);
  if (!rest.empty())
  {
    const auto [line, cut] = OneLine(rest, px, width);
    out.second = line;
    out.cut = cut;
  }
  return out;
}

float TextWithEllipsis(ImDrawList* dl, ImVec2 pos, float px, ImU32 colour, std::string_view text,
                       bool cut)
{
  Yg::Text(dl, pos, px, colour, text);
  float w = Yg::TextWidth(text, px);
  if (cut)
  {
    Yg::Text(dl, ImVec2(pos.x + w, pos.y), px, colour, ELLIPSIS);
    w += Yg::TextWidth(ELLIPSIS, px);
  }
  return w;
}

// Rows drawn as name over message: chat lines and direct-message notices, so the message is shown.
bool Worded(const PillRow& r)
{
  return r.type == PillRow::Type::Chat || (r.type == PillRow::Type::Notice && r.kind == Kind::Dm);
}

// One laid-out row. String views point into the caller's rows and the notice cache.
struct RowLayout
{
  const PillRow* row = nullptr;
  float height = 0;
  float width = 0;  // avatar, gap and text
  // `a` is the name (empty for toasts), `b` the text.
  std::string_view a, b;
  bool cut = false;
  bool head_cut = false;  // "…" before `b`: live words keep their end
  bool name_cut = false;
  TwoLines body;  // chat text
};

// Talk rows that keep the end of their text: the mic is open, or the words are being sent.
bool LiveWords(const PillRow& r)
{
  return r.type == PillRow::Type::Talk &&
         (r.talk == Talk::Phase::Listen || r.talk == Talk::Phase::Send);
}
}  // namespace

Kit::Box DrawPill(ImDrawList* dl, const Model::View& view, std::span<const PillRow> rows,
                  double now_ms, float right_limit)
{
  const Kit::Box none{ImVec2(0, 0), ImVec2(0, 0)};
  if (!view.on || view.place.size <= 0)
    return none;
  if (view.generation != s_notices.generation)
    s_notices.generation = Current().Lines(s_notices.generation, &s_notices.entries);
  if (view.talk_generation != s_talk.generation)
    s_talk.generation = Current().TalkRow(s_talk.generation, &s_talk.entry);
  const Talk& talk = s_talk.entry.talk;
  const PillRow talk_row{PillRow::Type::Talk,
                         talk.name,
                         talk.text,
                         {},
                         {},
                         Kind::Join,
                         TalkAlpha(talk, s_talk.entry.at, now_ms),
                         s_talk.entry.at,
                         talk.phase};
  // Gather visible rows: the caller's, then the notices.
  std::array<PillRow, 16> all;
  std::size_t n = 0;
  for (const PillRow& r : rows)
  {
    if (r.alpha > 0 && n < all.size())
      all[n++] = r;
  }
  for (const Model::Entry& e : s_notices.entries)
  {
    const float a = LineAlpha(e.notice, e.at, now_ms);
    if (a <= 0 || n >= all.size())
      continue;
    all[n++] = PillRow{PillRow::Type::Notice, e.notice.name, e.notice.text, e.notice.key,
                       e.notice.verb, e.notice.kind, a, e.at};
  }
  if (n == 0 && talk_row.alpha <= 0)
    return none;
  // Stable insertion sort by arrival time; there are only a few rows.
  for (std::size_t i = 1; i < n; ++i)
  {
    for (std::size_t j = i; j > 0 && all[j - 1].at > all[j].at; --j)
      std::swap(all[j - 1], all[j]);
  }
  // The newest PILL_ROWS, then the talk row, which keeps its place below them.
  std::array<const PillRow*, PILL_ROWS + 1> shown;
  std::size_t shown_n = 0;
  for (std::size_t i = n > PILL_ROWS ? n - PILL_ROWS : 0; i < n; ++i)
    shown[shown_n++] = &all[i];
  if (talk_row.alpha > 0)
    shown[shown_n++] = &talk_row;

  const Place p = ScaledPlace(view.place, view.view_width, ImGui::GetIO().DisplaySize.x);
  const Pill m = PillFor(p.size / 40);
  const float display_w = ImGui::GetIO().DisplaySize.x;
  const float max_w = std::max(
      m.pad_left + 120 * m.k,
      std::min({m.max_width, display_w - p.x - 24 * m.k, right_limit - p.x}));

  // Only the newest row with an action shows its hint.
  const PillRow* hint = nullptr;
  for (std::size_t i = 0; i < shown_n; ++i)
  {
    if (!shown[i]->key.empty() && !shown[i]->verb.empty())
      hint = shown[i];
  }
  const float key_w = hint ? Yg::TextWidth(hint->key, m.key_px) + 2 * m.key_pad : 0;
  const float hint_w = hint ? m.hint_gap + key_w + 5 * m.k + Yg::TextWidth(hint->verb, m.verb_px) : 0;
  const float words_room =
      std::max(60 * m.k, max_w - m.pad_left - m.pad_right - hint_w - m.avatar - m.avatar_gap);

  std::array<RowLayout, PILL_ROWS + 1> laid;
  std::size_t count = 0;
  float content_h = 0, content_w = 0, alpha = 0;
  for (std::size_t i = 0; i < shown_n; ++i)
  {
    const PillRow& r = *shown[i];
    RowLayout l;
    l.row = &r;
    float words = 0;
    if (Worded(r))
    {
      const auto [name, name_cut] = OneLine(r.name, m.name_px, words_room);
      const TwoLines body = Wrap2(r.text, m.text_px, words_room);
      l.a = name;
      l.name_cut = name_cut;
      l.b = body.first;
      l.cut = body.cut;
      l.body = body;
      words = std::max(Yg::TextWidth(name, m.name_px) + (name_cut ? Yg::TextWidth(ELLIPSIS, m.name_px) : 0),
                       Yg::TextWidth(body.first, m.text_px));
      if (!body.second.empty())
        words = std::max(words, Yg::TextWidth(body.second, m.text_px) +
                                    (body.cut ? Yg::TextWidth(ELLIPSIS, m.text_px) : 0));
      const int lines = body.second.empty() ? 1 : 2;
      l.height = std::max(m.avatar, m.name_px * m.line + lines * m.text_px * m.line);
    }
    else
    {
      // A notice or talk row is "<name> <text>" on one line; a toast is text only.
      const bool named = r.type != PillRow::Type::Toast;
      const auto [nm, nm_cut] =
          named ? OneLine(r.name, m.name_px, words_room / 2) : std::pair<std::string_view, bool>{};
      const float name_w =
          named ? Yg::TextWidth(nm, m.name_px) + (nm_cut ? Yg::TextWidth(ELLIPSIS, m.name_px) : 0) : 0;
      const float space = named ? Yg::TextWidth(" ", m.text_px) : 0;
      const float room = std::max(20 * m.k, words_room - name_w - space);
      const auto [said, cut] =
          LiveWords(r) ? TailLine(r.text, m.text_px, room) : OneLine(r.text, m.text_px, room);
      l.a = nm;
      l.name_cut = nm_cut;
      l.b = said;
      if (LiveWords(r))
        l.head_cut = cut;
      else
        l.cut = cut;
      words = name_w + space + Yg::TextWidth(said, m.text_px) +
              (cut ? Yg::TextWidth(ELLIPSIS, m.text_px) : 0);
      l.height = std::max(m.avatar, m.text_px * m.line);
    }
    l.width = m.avatar + m.avatar_gap + words;
    content_w = std::max(content_w, l.width);
    content_h += l.height + (count ? m.gap : 0);
    alpha = std::max(alpha, r.alpha);
    laid[count++] = l;
  }

  const float w = std::min(max_w, m.pad_left + content_w + hint_w + m.pad_right);
  const float h = std::max(p.size, content_h + 2 * m.pad_y);
  const ImVec2 min(p.x, p.y), max(p.x + w, p.y + h);
  // Background: shadow, slate fill, hairline edge, as in YgCard.
  dl->AddRectFilled(ImVec2(min.x, min.y + 3 * m.k), ImVec2(max.x, max.y + 6 * m.k),
                    Rgba(0, 0, 0, 0.35f * alpha), m.radius);
  dl->AddRectFilled(min, max, Rgba(15, 23, 42, 0.95f * alpha), m.radius);
  dl->AddRect(min, max, Yg::Edge(0.22f * alpha), m.radius, 0, std::max(1.0f, m.k));

  float y = p.y + (h - content_h) / 2;
  const float x0 = p.x + m.pad_left;
  const float r_av = m.avatar / 2;
  for (std::size_t i = 0; i < count; ++i)
  {
    const RowLayout& l = laid[i];
    const PillRow& r = *l.row;
    const float a = r.alpha;
    const float tx = x0 + m.avatar + m.avatar_gap;
    if (Worded(r))
    {
      const ImVec2 c(x0 + r_av, y + r_av);
      DrawAvatar(dl, c, r_av, r.name, a);
      TextWithEllipsis(dl, ImVec2(tx, y), m.name_px, Yg::Ink(a), l.a, l.name_cut);
      const float by = y + m.name_px * m.line;
      const TwoLines& body = l.body;
      if (body.second.empty())
      {
        Yg::Text(dl, ImVec2(tx, by), m.text_px, Yg::Ink2(a), body.first);
      }
      else
      {
        Yg::Text(dl, ImVec2(tx, by), m.text_px, Yg::Ink2(a), body.first);
        TextWithEllipsis(dl, ImVec2(tx, by + m.text_px * m.line), m.text_px, Yg::Ink2(a),
                         body.second, body.cut);
      }
    }
    else
    {
      const ImVec2 c(x0 + r_av, y + l.height / 2);
      if (r.type == PillRow::Type::Toast)
      {
        Yg::Logo(dl, ImVec2(c.x - r_av, c.y - r_av), m.avatar, a);
      }
      else if (r.type == PillRow::Type::Talk)
      {
        // Who it goes to: a green ring while the mic is open or the words go, a green tick once
        // sent, greyed when nothing was sent.
        if (LiveWords(r))
          dl->AddCircle(c, r_av + 3 * m.k, Rgba(43, 166, 64, a), 32, 2 * m.k);
        DrawAvatar(dl, c, r_av, r.name, r.talk == Talk::Phase::Error ? 0.45f * a : a);
        if (r.talk == Talk::Phase::Sent)
        {
          const ImVec2 mark(c.x + r_av * 0.75f, c.y + r_av * 0.75f);
          const float s = 3 * m.k, t = std::max(1.0f, 1.6f * m.k);
          dl->AddCircleFilled(mark, 9 * m.k, Rgba(15, 23, 42, a), 16);
          dl->AddCircleFilled(mark, 7 * m.k, Rgba(43, 166, 64, a), 16);
          dl->AddLine(ImVec2(mark.x - s, mark.y), ImVec2(mark.x - s / 3, mark.y + s * 0.7f),
                      Rgba(255, 255, 255, a), t);
          dl->AddLine(ImVec2(mark.x - s / 3, mark.y + s * 0.7f),
                      ImVec2(mark.x + s, mark.y - s * 0.7f), Rgba(255, 255, 255, a), t);
        }
      }
      else
      {
        // Avatar marks: green ring for voice or call, amber ring for away, greyed for leave,
        // green dot for back, green plus for join.
        if (r.kind == Kind::Voice || r.kind == Kind::Call)
          dl->AddCircle(c, r_av + 3 * m.k, Rgba(43, 166, 64, a), 32, 2 * m.k);
        if (r.kind == Kind::Away)
          dl->AddCircle(c, r_av + 3 * m.k, Rgba(242, 161, 20, a), 32, 2 * m.k);
        DrawAvatar(dl, c, r_av, r.name, r.kind == Kind::Leave ? 0.45f * a : a);
        if (r.kind == Kind::Join || r.kind == Kind::Back)
        {
          const ImVec2 mark(c.x + r_av * 0.75f, c.y + r_av * 0.75f);
          dl->AddCircleFilled(mark, 9 * m.k, Rgba(15, 23, 42, a), 16);
          dl->AddCircleFilled(mark, 7 * m.k, Rgba(43, 166, 64, a), 16);
          if (r.kind == Kind::Join)
          {
            const float s = 3.5f * m.k, t = std::max(1.0f, 1.6f * m.k);
            dl->AddLine(ImVec2(mark.x - s, mark.y), ImVec2(mark.x + s, mark.y), Rgba(255, 255, 255, a), t);
            dl->AddLine(ImVec2(mark.x, mark.y - s), ImVec2(mark.x, mark.y + s), Rgba(255, 255, 255, a), t);
          }
        }
      }
      // Name in bright ink, text softer, on the row's centre line.
      const float ty = c.y - m.text_px * m.line / 2 + (m.text_px * (m.line - 1)) / 2;
      float x = tx;
      if (!l.a.empty())
      {
        x += TextWithEllipsis(dl, ImVec2(x, ty + (m.text_px - m.name_px) * 0.6f), m.name_px,
                              Yg::Ink(a), l.a, l.name_cut) +
             Yg::TextWidth(" ", m.text_px);
      }
      if (l.head_cut)
      {
        Yg::Text(dl, ImVec2(x, ty), m.text_px, Yg::Ink2(a), ELLIPSIS);
        x += Yg::TextWidth(ELLIPSIS, m.text_px);
      }
      TextWithEllipsis(dl, ImVec2(x, ty), m.text_px, Yg::Ink2(a), l.b, l.cut);
    }
    // The hint at the pill's right end: the key in a keycap, then the verb.
    if (hint == &r)
    {
      const float cy = y + l.height / 2;
      const float hx = max.x - m.pad_right - hint_w + m.hint_gap;
      const ImVec2 kmin(hx, cy - m.key_h / 2), kmax(hx + key_w, cy + m.key_h / 2);
      dl->AddRectFilled(kmin, kmax, Rgba(255, 255, 255, 0.16f * a), 5 * m.k);
      Yg::Text(dl, ImVec2(hx + m.key_pad, cy - m.key_px * 0.62f), m.key_px, Rgba(255, 255, 255, a),
               r.key);
      Yg::Text(dl, ImVec2(kmax.x + 5 * m.k, cy - m.verb_px * 0.62f), m.verb_px, Yg::Ink2(0.9f * a),
               r.verb);
    }
    y += l.height + m.gap;
  }
  return Kit::Box{min, max};
}
}  // namespace Orca::UX::YgOrb
