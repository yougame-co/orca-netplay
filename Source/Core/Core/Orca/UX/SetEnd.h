// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

struct ImDrawList;

// Forward-declared so this header doesn't need ImGui (the app's command loop includes it).
namespace Orca::UX::Widgets
{
enum class Tone : int;
}

// How a matchmade set ends on screen: the VICTORY / DEFEAT panel with the rating change, the
// "opponent disconnected" forfeit countdown, "You left", and the leave hint while both players stay
// on the results screen after the set. See ORCA.md, "Set end".
//
// Display only: nothing here reads or writes emulated memory or feeds back into the game. Inputs
// arrive from any thread (frame hook, room thread, app commands, the netplay session) and are
// guarded by a mutex; the overlay draws At().
namespace Orca::UX::SetEnd
{
// The rating counting from the old number to the new one. Pure, so tests can drive it with a clock.
class RatingCount
{
public:
  // "Rating updating…" this long at most, then no rating at all.
  static constexpr double WAIT_MS = 10000;
  static constexpr double COUNT_MS = 1000;
  // The number and the change swell briefly as the count lands.
  static constexpr double POP_MS = 450;
  static constexpr float POP_SCALE = 0.28f;

  enum class Phase
  {
    None,      // not started
    Updating,  // the set is over, the new rating isn't in: "Rating updating…"
    Counting,  // old -> new
    Landed,    // the new number (the pop, then still)
    Gone,      // nothing came in WAIT_MS: no rating shown
  };
  struct View
  {
    Phase phase = Phase::None;
    int shown = -1;  // the number to show (-1: none)
    // The change, once the new number is in and an old one was known. 0 shows as "±0".
    bool has_delta = false;
    int delta = 0;
    float pop = 1;        // the number's scale
    float badge_pop = 1;  // the change's scale (0 before it shows)
    bool operator==(const View&) const = default;
  };

  // The set ended at `now`; `before` is the rating going into it (-1: unknown).
  void Start(int before, double now);
  // The new rating. `before` (when >= 0) replaces the old number. The same number again changes
  // nothing; a different one counts on from where the count stands.
  void Arrive(int after, double now, int before = -1);
  bool Started() const { return m_started; }
  bool Arrived() const { return m_after >= 0; }
  int Before() const { return m_before; }
  int After() const { return m_after; }
  // When the count lands (-1 before a new number came).
  double LandsAt() const;
  View At(double now) const;

private:
  bool m_started = false;
  double m_start = 0;
  int m_before = -1;
  int m_from = -1;  // where the count starts: before, or where it stood when the number changed
  int m_after = -1;
  double m_arrived = -1;
};

// Whole seconds left before `deadline_ms`, rounded up, 0 at or past it.
int SecondsLeft(double deadline_ms, double now_ms);

enum class Outcome
{
  Won,
  Lost,
  Draw,
  Void,
};

// What the overlay draws this frame.
struct Notice
{
  std::string title;  // "bo disconnected", "bo forfeited", "You left"
  // "They forfeit in 0:09" ("No contest in 0:09" before the set's first game), "You win the set",
  // "This set counts as a loss" ("This set doesn't count")
  std::string line;
  int seconds = -1;   // the countdown's seconds left; -1: no countdown
  float fraction = 0;  // fraction of the countdown left
  Widgets::Tone tone{};  // Neutral
  float alpha = 1;
  float scale = 1;  // entrance animation
  bool operator==(const Notice&) const = default;
};
struct Panel
{
  bool ranked = true;
  Outcome out = Outcome::Won;
  std::string title;   // VICTORY, DEFEAT; casual YOU WON, YOU LOST, DRAW
  std::string score;   // "2–1", the player's games first
  std::string detail;  // "vs bo", "vs bo · by forfeit"
  bool show_rating = false;
  RatingCount::View rating;
  float alpha = 1;
  float scale = 1;  // entrance animation
  bool operator==(const Panel&) const = default;
};
// After the set, on the results screen: how to leave ("Hold B to leave") and the hold's progress.
struct Hint
{
  std::string text;
  float progress = 0;  // 0..1 of the hold (0: not held)
  bool operator==(const Hint&) const = default;
};
struct View
{
  std::optional<Notice> notice;
  std::optional<Panel> panel;
  std::optional<Hint> hint;
  // The notice replaces the overlay's "Waiting for <name>…" line.
  bool hides_waiting = false;
  bool Empty() const { return !notice && !panel && !hint; }
};

class Model
{
public:
  static constexpr double LEAVE_FORFEIT_MS = 15000;  // the rooms server's grace after a leave
  static constexpr double STALL_FORFEIT_MS = 10000;  // OnlineMatch's PEER_SILENCE_LIMIT
  static constexpr double STALL_NOTICE_MS = 1500;    // shorter stalls aren't shown
  // A countdown still without a verdict this long past its end goes away (OnlineMatch waits 30 s).
  static constexpr double NO_VERDICT_MS = 20000;
  static constexpr double FORFEITED_MS = 2500;  // "<name> forfeited · You win the set"
  static constexpr double PANEL_MS = 7000;      // the set's panel at least this long
  static constexpr double PANEL_AFTER_LAND_MS = 3000;  // and this long after the rating lands
  static constexpr double PANEL_MAX_MS = 15000;
  static constexpr double CASUAL_MS = 6000;    // a casual game's panel
  static constexpr double NOTICE_MS = 4000;    // "You left", "<name> left · Searching again"
  static constexpr double ENTER_MS = 220;
  static constexpr double FADE_MS = 400;

  // A ranked set or casual game against `opponent` began. Clears whatever the last one showed.
  void MatchBegan(bool ranked, std::string opponent, double now);
  // The match block saw the ranked set end on the results screen. `key` identifies the set so its
  // panel opens once; `rating_before` is this player's rating going in (-1: none).
  void BlockSetOver(u32 key, bool won, int mine, int theirs, int rating_before, double now);
  // The room's verdict on a ranked set, with the ratings it carries (-1: none).
  // Negative game counts show "Score pending" until all game receipts reconcile.
  void SetVerdict(Outcome out, bool forfeit, int mine, int theirs, int before, int after,
                  int rating_before, double now);
  // A casual game's verdict and the score against this opponent so far.
  void CasualGame(Outcome out, int mine, int theirs, double now);
  // The app's `queue rating <n>`: the new rating after the site rated the set.
  void QueueRating(int rating, double now);
  // The ranked set reached its first game: one of its fights began (its fighters in) on final
  // frames, or a game of it ended. Until then a leave, a stall or a no-show voids the set and rates
  // nobody (the room's rule, YouGame docs/ORCA_ONLINE_UX.md "3B leaving mid-game"); from then on
  // the one who goes forfeits it.
  void GameBegan(double now);
  // Mid-set, the opponent left the room. They forfeit LEAVE_FORFEIT_MS later unless they come back
  // (before the set's first game: no contest then).
  void OpponentLeft(double now);
  void OpponentBack(double now);
  // The session has been waiting `stalled_ms` for the opponent's inputs.
  void Stall(int stalled_ms, double now);
  // The wait is over: `resumed` if their inputs came, else the session gave up or stopped.
  void StallOver(bool resumed, double now);
  // Opted-in public rooms: a transport hold, never a forfeit countdown. Null clears it.
  void RoomHold(std::optional<double> deadline_ms);
  // The session gave up and unplugged the opponent. The countdown stays until the verdict.
  void OpponentDropped(double now);
  // Casual: the opponent left and the app searches again.
  void CasualOpponentLeft(double now);
  // This player left a ranked set, or picked no character in time (`no_show`): a loss once the set
  // reached its first game (GameBegan), else it doesn't count.
  void SelfLeft(bool no_show, double now);
  // No verdict will come: the countdown goes.
  void NoVerdict(double now);
  // The room's `set-over`: it stays open until the players leave. Nothing after this is a forfeit.
  void SetOver(double now);
  // After the set, the opponent left the room: "<name> left".
  void OpponentLeftAfterSet(double now);
  // After the set, each frame on the results screen: whether to show the leave hint, whether the
  // opponent is gone (Start also leaves), and how far B has been held (0..1).
  void LeaveHint(bool shown, bool opponent_gone, float progress);
  // After the set, this player is leaving the room. The opponent leaving is no longer news.
  void LeavingSetRoom(double now);
  bool InSetOver() const;

  View At(double now) const;
  // The opponent's name as the room gave it ("" none).
  std::string Opponent() const;

private:
  // Requires m_lock.
  void ResetLocked();
  void OpenPanel(bool ranked, Outcome out, int mine, int theirs, bool forfeit, int rating_before,
                 double now);
  std::optional<Notice> NoticeAt(double now, bool* hides_waiting) const;
  std::optional<Panel> PanelAt(double now) const;
  std::string Name() const;

  mutable std::mutex m_lock;
  std::string m_opponent;
  bool m_ranked_live = false;  // a ranked set is in progress (no verdict yet)
  bool m_casual_live = false;
  // The ranked set reached its first game (GameBegan): a leave from here on is a forfeit.
  bool m_game_began = false;
  std::optional<double> m_room_hold;

  // The opponent gone mid-set: since when, and why.
  std::optional<double> m_left_at;   // the room's leave
  std::optional<double> m_stall_at;  // a stall's start (STALL_NOTICE_MS long or more)
  bool m_dropped = false;            // the session unplugged them
  std::optional<double> m_forfeited_at;  // the forfeit verdict came: "<name> forfeited"

  bool m_panel = false;
  bool m_panel_ranked = true;
  Outcome m_out = Outcome::Won;
  int m_mine = 0, m_theirs = 0;
  bool m_by_forfeit = false;
  double m_panel_at = -1;  // when it shows (after "forfeited", FORFEITED_MS later)
  u32 m_block_key = 0;
  bool m_block_key_set = false;
  RatingCount m_rating;
  bool m_rating_from_room = false;  // the verdict carried the rating, so the app's line is no news

  // A short notice ("You left", "<name> left").
  std::string m_note_title, m_note_line;
  Widgets::Tone m_note_tone{};
  double m_note_at = -1;
  double m_self_left_at = -1;     // the note is SelfLeft's
  bool m_note_after_set = false;  // the note is the after-set "<name> left"

  // After the set: the room stays open, with the leave hint on the results screen.
  bool m_set_over = false;
  bool m_set_over_leaving = false;
  bool m_hint = false;
  bool m_hint_alone = false;
  float m_hint_progress = 0;
};

// The process-wide model.
Model& Current();
// The host's steady clock in milliseconds. Every caller passes this to the model.
double NowMs();

// Draws `view` centred across the game picture: the panel near the top, the notice below it, the
// leave hint near the bottom.
void Draw(ImDrawList* dl, float unit, float pic_x, float pic_y, float pic_w, float pic_h,
          const View& view);

// Tests only: with ORCA_UX_SETEND_DEMO=victory|defeat|countdown|casual|left|stay the model plays
// that screen on a 16 s loop, for screenshots. With ORCA_UX_SETEND_DEMO_AT=<frame> it starts at
// that game frame and runs on game frames instead of the host clock, so a screenshot at a given
// frame is reproducible. Returns the time to view the model at (`now` when unset).
double DemoFrame(double now);
}  // namespace Orca::UX::SetEnd
