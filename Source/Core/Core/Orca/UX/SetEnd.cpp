// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/SetEnd.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <utility>

#include <fmt/format.h>

#include <imgui.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Widgets.h"
#include "Core/Rollback/Harness.h"

namespace Orca::UX::SetEnd
{
namespace
{
const char* OutcomeName(Outcome out)
{
  switch (out)
  {
  case Outcome::Won:
    return "won";
  case Outcome::Lost:
    return "lost";
  case Outcome::Draw:
    return "draw";
  default:
    return "void";
  }
}

// 0 at `from`, 1 `length` later.
double Progress(double now, double from, double length)
{
  return length > 0 ? std::clamp((now - from) / length, 0.0, 1.0) : 1.0;
}

// An entrance: from 86% up, a little past full size, settling at it.
float Entrance(double t)
{
  if (t >= 1)
    return 1;
  const double eased = 1 - std::pow(1 - t, 3);
  return static_cast<float>(0.86 + 0.14 * eased + 0.05 * std::sin(std::numbers::pi * t));
}

// Fades in over ENTER_MS from `at` (from a quarter: the entrance's scale does the rest).
float FadeIn(double now, double at)
{
  return static_cast<float>(0.25 + 0.75 * Progress(now, at, Model::ENTER_MS));
}

// Fades in over ENTER_MS from `at`, out over FADE_MS before `end`.
float Alpha(double now, double at, double end)
{
  const double in = FadeIn(now, at);
  const double out = end > now ? std::clamp((end - now) / Model::FADE_MS, 0.0, 1.0) : 0.0;
  return static_cast<float>(std::min(in, out));
}
}  // namespace

// ---- RatingCount ----

void RatingCount::Start(int before, double now)
{
  *this = RatingCount{};
  m_started = true;
  m_start = now;
  m_before = before;
  m_from = before;
}

void RatingCount::Arrive(int after, double now, int before)
{
  if (after < 0)
    return;
  if (!m_started)
    Start(before, now);
  if (m_after < 0)
  {
    if (before >= 0)
    {
      m_before = before;
      m_from = before;
    }
    m_after = after;
    m_arrived = now;
    return;
  }
  if (after == m_after)
    return;
  // A different number: count on from where the count stands.
  m_from = At(now).shown;
  m_after = after;
  m_arrived = now;
}

double RatingCount::LandsAt() const
{
  if (m_after < 0)
    return -1;
  if (m_from < 0 || m_from == m_after)
    return m_arrived;
  return m_arrived + COUNT_MS;
}

RatingCount::View RatingCount::At(double now) const
{
  View v;
  if (!m_started)
    return v;
  if (m_after < 0 || now < m_arrived)
  {
    if (now - m_start >= WAIT_MS)
    {
      v.phase = Phase::Gone;
      return v;
    }
    v.phase = Phase::Updating;
    v.shown = m_before;
    v.badge_pop = 0;
    return v;
  }
  v.has_delta = m_before >= 0;
  v.delta = v.has_delta ? m_after - m_before : 0;
  const double land = LandsAt();
  if (now < land)
  {
    v.phase = Phase::Counting;
    const double t = Progress(now, m_arrived, COUNT_MS);
    const double eased = 1 - std::pow(1 - t, 3);
    v.shown = m_from + static_cast<int>(std::lround((m_after - m_from) * eased));
    v.badge_pop = 0;
    return v;
  }
  v.phase = Phase::Landed;
  v.shown = m_after;
  // A change pops as it lands; no change (or no number before) just shows.
  const bool moved = v.has_delta && v.delta != 0;
  const double u = Progress(now, land, POP_MS);
  if (moved && u < 1)
  {
    const double s = std::sin(std::numbers::pi * u);
    v.pop = static_cast<float>(1 + POP_SCALE * s);
    // The change grows in to its full size and past it, then settles.
    v.badge_pop = static_cast<float>(u < 0.4 ? (u / 0.4) * (1 + POP_SCALE) :
                                               1 + POP_SCALE * (1 - (u - 0.4) / 0.6));
  }
  return v;
}

int SecondsLeft(double deadline_ms, double now_ms)
{
  const double left = deadline_ms - now_ms;
  if (!(left > 0))
    return 0;
  return static_cast<int>(std::ceil(left / 1000 - 1e-9));
}

// ---- Model ----

std::string Model::Name() const
{
  return m_opponent.empty() ? std::string("Your opponent") : m_opponent;
}

std::string Model::Opponent() const
{
  std::lock_guard lk(m_lock);
  return m_opponent;
}

void Model::ResetLocked()
{
  m_opponent.clear();
  m_ranked_live = false;
  m_casual_live = false;
  m_game_began = false;
  m_left_at.reset();
  m_stall_at.reset();
  m_dropped = false;
  m_forfeited_at.reset();
  m_panel = false;
  m_panel_ranked = true;
  m_out = Outcome::Won;
  m_mine = m_theirs = 0;
  m_by_forfeit = false;
  m_panel_at = -1;
  m_block_key = 0;
  m_block_key_set = false;
  m_rating = RatingCount{};
  m_rating_from_room = false;
  m_note_title.clear();
  m_note_line.clear();
  m_note_tone = Widgets::Tone::Neutral;
  m_note_at = -1;
  m_self_left_at = -1;
  m_note_after_set = false;
  m_set_over = false;
  m_set_over_leaving = false;
  m_hint = false;
  m_hint_alone = false;
  m_hint_progress = 0;
}

void Model::MatchBegan(bool ranked, std::string opponent, double now)
{
  std::lock_guard lk(m_lock);
  (void)now;
  ResetLocked();
  m_opponent = std::move(opponent);
  m_ranked_live = ranked;
  m_casual_live = !ranked;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: a {} match against {} began", ranked ? "ranked" : "casual",
                 Name());
}

void Model::OpenPanel(bool ranked, Outcome out, int mine, int theirs, bool forfeit,
                      int rating_before, double now)
{
  m_panel = true;
  m_panel_ranked = ranked;
  m_out = out;
  m_mine = mine;
  m_theirs = theirs;
  m_by_forfeit = forfeit;
  m_panel_at = now;
  m_rating = RatingCount{};
  m_rating_from_room = false;
  if (ranked)
    m_rating.Start(rating_before, now);
}

void Model::BlockSetOver(u32 key, bool won, int mine, int theirs, int rating_before, double now)
{
  std::lock_guard lk(m_lock);
  if (m_block_key_set && m_block_key == key)
    return;
  m_block_key_set = true;
  m_block_key = key;
  m_ranked_live = false;
  m_left_at.reset();
  m_stall_at.reset();
  m_dropped = false;
  if (m_panel && m_panel_ranked)
    return;  // the room's verdict came first
  OpenPanel(true, won ? Outcome::Won : Outcome::Lost, mine, theirs, false, rating_before, now);
  NOTICE_LOG_FMT(ROLLBACK, "Set end: {} {}–{} against {} (the match block), rating {} going in",
                 won ? "VICTORY" : "DEFEAT", mine, theirs, Name(), rating_before);
}

void Model::SetVerdict(Outcome out, bool forfeit, int mine, int theirs, int before, int after,
                       int rating_before, double now)
{
  std::lock_guard lk(m_lock);
  const bool was_gone = m_left_at || m_stall_at;
  m_ranked_live = false;
  m_left_at.reset();
  m_stall_at.reset();
  m_dropped = false;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: the room's verdict: {}{} {}–{}{}{}", OutcomeName(out),
                 forfeit ? " by forfeit" : "", mine, theirs,
                 after >= 0 ? fmt::format(", rating {} -> {}", before, after) : std::string(),
                 was_gone ? " (the opponent was gone)" : "");
  if (out == Outcome::Void || out == Outcome::Draw)
  {
    m_panel = false;
    m_note_title = "No contest";
    m_note_line = "This set doesn't count";
    m_note_tone = Widgets::Tone::Neutral;
    m_note_at = now;
    m_note_after_set = false;
    return;
  }
  if (forfeit && out == Outcome::Won)
  {
    // "<name> forfeited · You win the set" first, then the panel.
    m_forfeited_at = now;
    OpenPanel(true, out, mine, theirs, true, rating_before, now + FORFEITED_MS);
  }
  else if (!m_panel || !m_panel_ranked || forfeit)
  {
    OpenPanel(true, out, mine, theirs, forfeit, rating_before, now);
  }
  else
  {
    // The match block already opened the panel. The room's score and outcome take precedence.
    m_out = out;
    if (mine + theirs > 0)
    {
      m_mine = mine;
      m_theirs = theirs;
    }
  }
  if (after >= 0)
  {
    m_rating.Arrive(after, std::max(now, m_panel_at), before);
    m_rating_from_room = true;
  }
}

void Model::CasualGame(Outcome out, int mine, int theirs, double now)
{
  std::lock_guard lk(m_lock);
  m_casual_live = false;
  if (out == Outcome::Void)
  {
    m_note_title = "No contest";
    m_note_line = "That game didn't count";
    m_note_tone = Widgets::Tone::Neutral;
    m_note_at = now;
    m_note_after_set = false;
    return;
  }
  OpenPanel(false, out, mine, theirs, false, -1, now);
  NOTICE_LOG_FMT(ROLLBACK, "Set end: casual game {} against {}, {}–{} so far", OutcomeName(out),
                 Name(), mine, theirs);
}

void Model::QueueRating(int rating, double now)
{
  std::lock_guard lk(m_lock);
  if (!m_panel || !m_panel_ranked || !m_rating.Started() || m_rating_from_room ||
      now - m_panel_at >= PANEL_MAX_MS)
  {
    return;
  }
  // The app resends the old rating when it searches again. Ignore it.
  if (!m_rating.Arrived() && rating == m_rating.Before())
    return;
  if (m_rating.At(now).phase == RatingCount::Phase::Gone)
    return;
  m_rating.Arrive(rating, std::max(now, m_panel_at));
  NOTICE_LOG_FMT(ROLLBACK, "Set end: the page's rating {} (was {})", rating, m_rating.Before());
}

void Model::GameBegan(double now)
{
  std::lock_guard lk(m_lock);
  (void)now;
  if (!m_ranked_live || m_game_began)
    return;
  m_game_began = true;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: the set's first game began: leaving it from here on loses it");
}

void Model::OpponentLeft(double now)
{
  std::lock_guard lk(m_lock);
  if (!m_ranked_live || m_left_at)
    return;
  m_left_at = now;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: {} left the room mid-set: {} in {:.0f} s", Name(),
                 m_game_began ? "their forfeit" : "no contest", LEAVE_FORFEIT_MS / 1000);
}

void Model::OpponentBack(double now)
{
  std::lock_guard lk(m_lock);
  (void)now;
  if (!m_left_at && !m_stall_at)
    return;
  m_left_at.reset();
  m_stall_at.reset();
  m_dropped = false;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: {} is back in the room", Name());
}

void Model::Stall(int stalled_ms, double now)
{
  std::lock_guard lk(m_lock);
  if (!m_ranked_live || m_dropped || m_stall_at || stalled_ms < STALL_NOTICE_MS)
    return;
  m_stall_at = now - stalled_ms;
  NOTICE_LOG_FMT(
      ROLLBACK, "Set end: {} stopped sending inputs {} ms ago: {}", Name(), stalled_ms,
      m_left_at ? fmt::format("{} is the leave's", m_game_began ? "their forfeit" : "no contest") :
                  fmt::format("{} in {:.1f} s", m_game_began ? "their forfeit" : "no contest",
                              (STALL_FORFEIT_MS - stalled_ms) / 1000));
}

void Model::StallOver(bool resumed, double now)
{
  std::lock_guard lk(m_lock);
  (void)now;
  if (!resumed || !m_stall_at || m_dropped)
    return;
  m_stall_at.reset();
  NOTICE_LOG_FMT(ROLLBACK, "Set end: {}'s inputs came again", Name());
}

void Model::OpponentDropped(double now)
{
  std::lock_guard lk(m_lock);
  if (!m_ranked_live || m_dropped)
    return;
  m_dropped = true;
  if (!m_left_at && !m_stall_at)
    m_stall_at = now - STALL_FORFEIT_MS;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: {} unplugged after the stall: waiting for the verdict",
                 Name());
}

void Model::CasualOpponentLeft(double now)
{
  std::lock_guard lk(m_lock);
  m_casual_live = false;
  m_note_title = fmt::format("{} left", Name());
  m_note_line = "Searching again";
  m_note_tone = Widgets::Tone::Neutral;
  m_note_at = now;
  m_note_after_set = false;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: {} left the casual room", Name());
}

void Model::SelfLeft(bool no_show, double now)
{
  std::lock_guard lk(m_lock);
  // Shown once: a no-show's "Time's up" stays when the leave that follows says it again.
  if (m_self_left_at >= 0 && now - m_self_left_at < NOTICE_MS)
    return;
  m_ranked_live = false;
  m_left_at.reset();
  m_stall_at.reset();
  m_dropped = false;
  m_panel = false;
  // The room voids a set left before its first game (a no-show always is) and rates nobody.
  const char* const counts = m_game_began ? "This set counts as a loss" : "This set doesn't count";
  m_note_title = no_show ? "Time's up" : "You left";
  m_note_line = no_show ? fmt::format("You didn't pick in time · {}", counts) : std::string(counts);
  m_note_tone = m_game_began ? Widgets::Tone::Loss : Widgets::Tone::Neutral;
  m_note_at = now;
  m_self_left_at = now;
  m_note_after_set = false;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: this player left the set{}: {}", no_show ? " (no pick)" : "",
                 m_game_began ? "a loss" : "before its first game, no contest");
}

void Model::NoVerdict(double now)
{
  std::lock_guard lk(m_lock);
  (void)now;
  const bool was_gone = m_left_at || m_stall_at;
  m_ranked_live = false;
  m_left_at.reset();
  m_stall_at.reset();
  m_dropped = false;
  if (was_gone)
    NOTICE_LOG_FMT(ROLLBACK, "Set end: no verdict came: the countdown goes");
}

void Model::SetOver(double now)
{
  std::lock_guard lk(m_lock);
  (void)now;
  if (m_set_over)
    return;
  m_set_over = true;
  // The set is decided, so any forfeit countdown is over.
  m_ranked_live = false;
  m_left_at.reset();
  m_stall_at.reset();
  m_dropped = false;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: set over: the room stays open until the players leave it");
}

void Model::OpponentLeftAfterSet(double now)
{
  std::lock_guard lk(m_lock);
  if (!m_set_over || m_set_over_leaving)
    return;
  // No second line: the leave hint below says how to leave.
  m_note_title = fmt::format("{} left", Name());
  m_note_line.clear();
  m_note_tone = Widgets::Tone::Neutral;
  m_note_at = now;
  m_note_after_set = true;
  NOTICE_LOG_FMT(ROLLBACK, "Set end: {} left the set's room (set over: not a forfeit)", Name());
}

void Model::LeaveHint(bool shown, bool opponent_gone, float progress)
{
  std::lock_guard lk(m_lock);
  m_hint = shown && m_set_over && !m_set_over_leaving;
  m_hint_alone = opponent_gone;
  m_hint_progress = m_hint ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
}

void Model::LeavingSetRoom(double now)
{
  std::lock_guard lk(m_lock);
  (void)now;
  if (!m_set_over || m_set_over_leaving)
    return;
  m_set_over_leaving = true;
  m_hint = false;
  m_hint_progress = 0;
  // "<name> left" is over: this player is on the way out too.
  if (m_note_at >= 0 && m_note_after_set)
    m_note_at = -1;
}

bool Model::InSetOver() const
{
  std::lock_guard lk(m_lock);
  return m_set_over;
}

std::optional<Notice> Model::NoticeAt(double now, bool* hides_waiting) const
{
  if (m_forfeited_at && now < *m_forfeited_at + FORFEITED_MS)
  {
    Notice n;
    n.title = fmt::format("{} forfeited", Name());
    n.line = "You win the set";
    n.tone = Widgets::Tone::Win;
    n.alpha = Alpha(now, *m_forfeited_at, *m_forfeited_at + FORFEITED_MS);
    n.scale = Entrance(Progress(now, *m_forfeited_at, ENTER_MS));
    *hides_waiting = true;
    return n;
  }
  if (m_ranked_live && (m_left_at || m_stall_at))
  {
    // After a leave, the room's grace period sets the forfeit. A stall alone uses the session's
    // limit.
    const bool left = m_left_at.has_value();
    const double total = left ? LEAVE_FORFEIT_MS : STALL_FORFEIT_MS;
    const double deadline = (left ? *m_left_at : *m_stall_at) + total;
    if (now <= deadline + NO_VERDICT_MS)
    {
      Notice n;
      n.title = fmt::format("{} disconnected", Name());
      n.seconds = SecondsLeft(deadline, now);
      char clock[16];
      // Before the set's first game the room voids it instead: no contest.
      const char* const what = m_game_began ? "They forfeit" : "No contest";
      n.line = n.seconds > 0 ? fmt::format("{} in {}", what, Widgets::ClockText(n.seconds, clock)) :
                               fmt::format("{} now", what);
      n.fraction = static_cast<float>(std::clamp((deadline - now) / total, 0.0, 1.0));
      n.tone = Widgets::Tone::Warning;
      const double shown_at = left ? *m_left_at : *m_stall_at + STALL_NOTICE_MS;
      n.alpha = FadeIn(now, shown_at);
      n.scale = Entrance(Progress(now, shown_at, ENTER_MS));
      *hides_waiting = true;
      return n;
    }
  }
  if (m_note_at >= 0 && now >= m_note_at && now < m_note_at + NOTICE_MS)
  {
    Notice n;
    n.title = m_note_title;
    n.line = m_note_line;
    n.tone = m_note_tone;
    n.alpha = Alpha(now, m_note_at, m_note_at + NOTICE_MS);
    n.scale = Entrance(Progress(now, m_note_at, ENTER_MS));
    return n;
  }
  return std::nullopt;
}

std::optional<Panel> Model::PanelAt(double now) const
{
  if (!m_panel || now < m_panel_at)
    return std::nullopt;
  double end;
  if (m_panel_ranked)
  {
    const double land = m_rating.LandsAt();
    end = land >= 0 ? std::max(m_panel_at + PANEL_MS, land + PANEL_AFTER_LAND_MS) :
                      m_panel_at + std::max(PANEL_MS, RatingCount::WAIT_MS);
    end = std::min(end, m_panel_at + PANEL_MAX_MS);
  }
  else
  {
    end = m_panel_at + CASUAL_MS;
  }
  if (now >= end)
    return std::nullopt;
  Panel p;
  p.ranked = m_panel_ranked;
  p.out = m_out;
  if (m_panel_ranked)
    p.title = m_out == Outcome::Won ? "VICTORY" : "DEFEAT";
  else
    p.title = m_out == Outcome::Won ? "YOU WON" : m_out == Outcome::Lost ? "YOU LOST" : "DRAW";
  p.score = fmt::format("{}–{}", m_mine, m_theirs);
  const std::string vs = m_opponent.empty() ? std::string() : "vs " + m_opponent;
  p.detail = m_by_forfeit ? (vs.empty() ? std::string("by forfeit") : vs + " · by forfeit") : vs;
  if (m_panel_ranked)
  {
    p.rating = m_rating.At(now);
    p.show_rating = p.rating.phase != RatingCount::Phase::None &&
                    p.rating.phase != RatingCount::Phase::Gone &&
                    (p.rating.shown >= 0 || p.rating.phase == RatingCount::Phase::Updating);
  }
  p.alpha = Alpha(now, m_panel_at, end);
  p.scale = Entrance(Progress(now, m_panel_at, ENTER_MS));
  return p;
}

View Model::At(double now) const
{
  std::lock_guard lk(m_lock);
  View v;
  v.notice = NoticeAt(now, &v.hides_waiting);
  v.panel = PanelAt(now);
  if (m_hint)
  {
    Hint h;
    h.progress = m_hint_progress;
    // The same words while B is held: the bar under them fills.
    h.text = m_hint_alone ? "Press Start to continue · Hold B to leave" : "Hold B to leave";
    v.hint = std::move(h);
  }
  return v;
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

// ---- Drawing (styling lives in Widgets.h) ----

namespace
{
using Widgets::Tone;

Tone ToneOf(Outcome out)
{
  return out == Outcome::Won ? Tone::Win : out == Outcome::Lost ? Tone::Loss : Tone::Neutral;
}

// The panel, its top at `top`. Returns its bottom.
float DrawPanel(ImDrawList* dl, float unit, float cx, float top, const Panel& p)
{
  const float u = unit * p.scale;
  const float a = p.alpha;
  const Tone tone = ToneOf(p.out);
  const float pad = 2.2f * u;
  const ImVec2 title_s = p.ranked ? Widgets::MeasureTitle(u, p.title) :
                                    Widgets::Measure(Widgets::HEADING_SIZE * u, p.title);
  const float title_h = title_s.y, title_w = title_s.x;
  const float score_size = (p.ranked ? Widgets::HEADING_SIZE : Widgets::LINE_SIZE * 1.2f) * u;
  const std::string score_line =
      p.ranked || p.detail.empty() ? p.score : fmt::format("{}  {}", p.score, p.detail);
  const ImVec2 score_s = Widgets::Measure(score_size, score_line);
  const ImVec2 detail_s = p.ranked && !p.detail.empty() ?
                              Widgets::Measure(Widgets::LINE_SIZE * u, p.detail) :
                              ImVec2(0, 0);
  // The rating row: the number, and the change beside it once it is in.
  const float number_h = Widgets::MeasureNumber(u, "0").y;
  const float small_h = Widgets::Measure(Widgets::SMALL_SIZE * u, "R").y;
  float height = pad + title_h + 0.4f * u + score_s.y;
  if (detail_s.y > 0)
    height += 0.3f * u + detail_s.y;
  if (p.show_rating)
  {
    height += 1.6f * u + 0.3f * u;  // the rule
    if (p.rating.shown >= 0)
      height += number_h;
    if (p.rating.phase == RatingCount::Phase::Updating)
      height += 0.4f * u + small_h;
  }
  height += pad;
  const float width =
      std::max({title_w + 8 * u, score_s.x + 6 * u, detail_s.x + 6 * u, (p.ranked ? 46 : 30) * u});
  const ImVec2 min(cx - width / 2, top), max(cx + width / 2, top + height);
  Widgets::Panel(dl, min, max, u, tone, a);
  float y = top + pad;
  if (p.ranked)
    Widgets::Title(dl, cx, y, u, p.title, tone, a);
  else
    Widgets::Line(dl, cx, y, Widgets::HEADING_SIZE * u, p.title, tone, a);
  y += title_h + 0.4f * u;
  Widgets::Line(dl, cx, y, score_size, score_line, Tone::Neutral, a);
  y += score_s.y;
  if (detail_s.y > 0)
  {
    y += 0.3f * u;
    Widgets::Line(dl, cx, y, Widgets::LINE_SIZE * u, p.detail, Tone::Neutral, a * 0.85f);
    y += detail_s.y;
  }
  if (p.show_rating)
  {
    y += 0.8f * u;
    dl->AddLine(ImVec2(min.x + 4 * u, y), ImVec2(max.x - 4 * u, y),
                IM_COL32(255, 255, 255, static_cast<int>(60 * std::clamp(a, 0.0f, 1.0f))),
                std::max(1.0f, 0.25f * u));
    y += 0.8f * u + 0.3f * u;
    const RatingCount::View& r = p.rating;
    if (r.shown >= 0)
    {
      const bool badge = r.has_delta && r.phase == RatingCount::Phase::Landed && r.badge_pop > 0;
      const ImVec2 badge_s = badge ? Widgets::DeltaBadgeSize(u, r.delta) : ImVec2(0, 0);
      char digits[16];
      std::snprintf(digits, sizeof(digits), "%d", r.shown);
      const float number_w = Widgets::MeasureNumber(u, digits).x;
      // Centre the number and the change together, keeping them apart as both swell. The number
      // dims while it waits.
      const float gap = badge ? 2.0f * u : 0;
      const float left = cx - (number_w + gap + badge_s.x) / 2;
      const float na = r.phase == RatingCount::Phase::Updating ? a * 0.55f : a;
      Widgets::BigNumber(dl, left + number_w / 2, y, u, r.shown, na, r.pop);
      if (badge)
      {
        const float number_right = left + number_w / 2 + number_w * r.pop / 2;
        const float badge_left = number_right + gap + badge_s.x * (r.badge_pop - 1) / 2;
        Widgets::DeltaBadge(dl, badge_left, y + number_h / 2, u, r.delta, a, r.badge_pop);
      }
      y += number_h;
    }
    if (r.phase == RatingCount::Phase::Updating)
    {
      y += 0.4f * u;
      Widgets::Line(dl, cx, y, Widgets::SMALL_SIZE * u, "Rating updating…", Tone::Neutral,
                    a * 0.75f);
      y += small_h;
    }
  }
  return max.y;
}

// The notice, its top at `top`. Returns its bottom.
float DrawNotice(ImDrawList* dl, float unit, float cx, float top, const Notice& n)
{
  const float u = unit * n.scale;
  const float a = n.alpha;
  const float pad = 2.0f * u, side = 3.2f * u;
  const float title_size = Widgets::HEADING_SIZE * u, line_size = Widgets::LINE_SIZE * u;
  const ImVec2 ts = Widgets::Measure(title_size, n.title);
  const ImVec2 ls = n.line.empty() ? ImVec2(0, 0) : Widgets::Measure(line_size, n.line);
  const float text_h = ts.y + (ls.y > 0 ? 0.4f * u + ls.y : 0);
  const float text_w = std::max(ts.x, ls.x);
  const bool ring = n.seconds >= 0;
  const float r = ring ? Widgets::CountdownRadius(u) : 0;
  const float gap = ring ? 3.0f * u : 0;
  const float inner_h = std::max(text_h, 2 * r);
  const float width = std::max(text_w + gap + 2 * r + 2 * side, 30 * u);
  const float height = inner_h + 2 * pad;
  const ImVec2 min(cx - width / 2, top), max(cx + width / 2, top + height);
  Widgets::Panel(dl, min, max, u, n.tone, a);
  // The text block on the left (centred when there is no ring), the ring on the right.
  const float block_w = text_w + gap + 2 * r;
  const float text_cx = ring ? cx - block_w / 2 + text_w / 2 : cx;
  float y = top + pad + (inner_h - text_h) / 2;
  Widgets::Line(dl, text_cx, y, title_size, n.title, n.tone, a);
  y += ts.y + 0.4f * u;
  if (ls.y > 0)
    Widgets::Line(dl, text_cx, y, line_size, n.line, Tone::Neutral, a);
  if (ring)
  {
    const ImVec2 c(cx + block_w / 2 - r, top + pad + inner_h / 2);
    Widgets::Countdown(dl, c, u, n.seconds, n.fraction, n.tone, a);
  }
  return max.y;
}
}  // namespace

void Draw(ImDrawList* dl, float unit, float pic_x, float pic_y, float pic_w, float pic_h,
          const View& view)
{
  if (!dl || !(unit > 0) || view.Empty())
    return;
  const ImVec2 pic_min(pic_x, pic_y), pic_size(pic_w, pic_h);
  const float cx = pic_min.x + pic_size.x / 2;
  float panel_bottom = -1;
  if (view.panel)
  {
    const float top = pic_min.y + pic_size.y * (view.panel->ranked ? 0.15f : 0.20f);
    panel_bottom = DrawPanel(dl, unit, cx, top, *view.panel);
  }
  if (view.notice)
  {
    // Under the panel when there is one, else a little above the picture's middle.
    const float top = panel_bottom >= 0 ? panel_bottom + 2 * unit : pic_min.y + pic_size.y * 0.30f;
    DrawNotice(dl, unit, cx, top, *view.notice);
  }
  if (view.hint)
  {
    // Near the bottom, under the game's own results.
    Widgets::Hint(dl, cx, pic_min.y + pic_size.y * 0.88f, unit, view.hint->text,
                  view.hint->progress, 1.0f);
  }
}

// ---- The demo (tests only) ----

double DemoFrame(double now)
{
  static const std::string s_demo = [] {
    const char* v = std::getenv("ORCA_UX_SETEND_DEMO");
    return v && TestKnobsAllowed() ? std::string(v) : std::string();
  }();
  static const int s_demo_at = [] {
    const char* v = std::getenv("ORCA_UX_SETEND_DEMO_AT");
    return v ? std::atoi(v) : -1;
  }();
  if (s_demo.empty())
    return now;
  if (s_demo_at >= 0)
  {
    // Use game frames as the clock, starting at the requested frame.
    const int frame = Rollback::Harness::ShownFrame();
    if (frame < s_demo_at)
      return now;
    now = 1e6 + (frame - s_demo_at) * (1000.0 / 60.0);
  }
  static double s_start = -1;
  static int s_step = 0;
  static int s_loop = -1;
  if (s_start < 0)
    s_start = now;
  const int loop = static_cast<int>((now - s_start) / 16000);
  const double t = std::fmod(now - s_start, 16000);
  Model& m = Current();
  if (loop != s_loop)
  {
    s_loop = loop;
    s_step = 0;
    m.MatchBegan(s_demo != "casual", "bo", now);
    // Each ranked screen shows a set that reached its first game.
    m.GameBegan(now);
  }
  // Each step once per loop, at its time.
  const auto at = [&](int step, double when) {
    if (s_step == step && t >= when)
    {
      ++s_step;
      return true;
    }
    return false;
  };
  if (s_demo == "victory" || s_demo == "defeat")
  {
    const bool won = s_demo == "victory";
    if (at(0, 500))
      m.BlockSetOver(static_cast<u32>(loop + 1), won, won ? 2 : 1, won ? 1 : 2, 1532, now);
    if (at(1, 2500))
      m.QueueRating(won ? 1548 : 1517, now);
  }
  else if (s_demo == "countdown")
  {
    if (at(0, 500))
      m.OpponentLeft(now);
    if (at(1, 500 + Model::LEAVE_FORFEIT_MS))
      m.SetVerdict(Outcome::Won, true, 1, 0, -1, -1, 1532, now);
    if (at(2, 500 + Model::LEAVE_FORFEIT_MS + Model::FORFEITED_MS + 600))
      m.QueueRating(1549, now);
  }
  else if (s_demo == "casual")
  {
    if (at(0, 500))
      m.CasualGame(Outcome::Won, 2, 1, now);
    if (at(1, 8000))
      m.CasualOpponentLeft(now);
  }
  else if (s_demo == "left")
  {
    if (at(0, 500))
      m.SelfLeft(false, now);
  }
  else if (s_demo == "stay")
  {
    // After the set on the results screen: VICTORY, the hint, B held, the opponent leaving.
    if (at(0, 500))
    {
      m.BlockSetOver(static_cast<u32>(loop + 1), true, 2, 1, 1532, now);
      m.SetVerdict(Outcome::Won, false, 2, 1, -1, -1, 1532, now);
      m.SetOver(now);
    }
    if (at(1, 2000))
      m.QueueRating(1548, now);
    const bool alone = t >= 11000;
    if (alone && at(2, 11000))
      m.OpponentLeftAfterSet(now);
    const float held = t >= 8000 && t < 9500 ? static_cast<float>((t - 8000) / 1500) : 0.0f;
    m.LeaveHint(t >= 500 && t < 15500, alone, held);
  }
  return now;
}
}  // namespace Orca::UX::SetEnd
