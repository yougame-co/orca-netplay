// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

// The YouGame button and its notice lines, drawn by Orca over a full-screen game in place of the
// app's own button, which Orca's window covers. The app drives it over stdin:
//   orb <x> <y> <size> <lit> <away> <badge>
//       Show the button: top-left (x, y) and size in the view's device pixels (see ScaledPlace),
//       lit 0|1 (full or half strength), away 0|1 (amber held-seat dot), badge 0..99 unread.
//   orb blink    A white ring grows out of the button three times.
//   orb off      Hide the button and its lines.
//   notice <kind> <name> <text> [<key> <verb>]
//       A line beside the button. kind is join|leave|away|back|voice|call|dm|invite; fields are
//       URI-encoded as in `chat` (Chat.h); key and verb are an optional action hint.
//   talk listen|send|sent|error <name> <text>   /   talk off
//       Hold-to-talk (cap "talk"): one row after the notices, replaced by each line. listen: the
//       mic is open, name is who it goes to, text a hint or the live words; send: being sent; both
//       stay until the next line. sent and error fade by themselves. off removes it.
// On macOS clicks pass through to the app's button. On Windows a press on the disc prints
// `orca orb` instead of `orca click`. Host-only: nothing here touches emulated state. This header
// has no ImGui so DolphinNoGUI can include it; YgOrbDraw.h draws it.
namespace Orca::UX::YgOrb
{
enum class Kind
{
  Join,
  Leave,
  Away,
  Back,
  Voice,
  Call,
  Dm,
  Invite,
};

struct Place
{
  float x = 0;
  float y = 0;
  float size = 0;
  bool lit = false;
  bool away = false;
  int badge = 0;
};

struct Command
{
  enum class Type
  {
    Off,
    Blink,
    Place,
  };
  Type type = Type::Off;
  Place place;
};
// Parses a whole "orb ..." stdin line; nullopt when malformed.
std::optional<Command> ParseOrb(std::string_view line);

struct Notice
{
  Kind kind = Kind::Join;
  std::string name;
  std::string text;
  std::string key;   // empty: no hint
  std::string verb;
};
// Parses a whole "notice ..." stdin line; nullopt when malformed. The fields are player text, so
// never print or log them.
std::optional<Notice> ParseNotice(std::string_view line);
// Maximum code points in each decoded hint field.
inline constexpr std::size_t HINT_POINTS = 24;

// Hold-to-talk's row, from the page's `talk` lines.
struct Talk
{
  enum class Phase
  {
    Off,
    Listen,
    Send,
    Sent,
    Error,
  };
  Phase phase = Phase::Off;
  std::string name;
  std::string text;
};
// Parses a whole "talk ..." stdin line; nullopt when malformed. The fields are player text, so
// never print or log them.
std::optional<Talk> ParseTalk(std::string_view line);
// How long a talk row stays, in ms: sent 3 s, error 4 s. Listen and send stay until the next line,
// or TALK_STALE_MS without one (the page is gone; its own talk ends at 30 s).
inline constexpr double TALK_STALE_MS = 60000;
double TalkDwellMs(Talk::Phase phase);
// The talk row's opacity at `now_ms`, fading out over its last Model::FADE_MS; 0 when off.
float TalkAlpha(const Talk& talk, double at_ms, double now_ms);

// Converts `place` from the view's device pixels to backbuffer pixels (these differ on Retina
// Macs). Unscaled until the app has sent a `rect`.
Place ScaledPlace(const Place& place, float view_width, float display_width);

// How long a notice stays, in ms, matching the app's own button.
double DwellMs(const Notice& notice);

// A line's opacity at `now_ms`: 1, fading out over its last FADE_MS, then 0.
float LineAlpha(const Notice& notice, double at_ms, double now_ms);

// The button's state, driven by an explicit clock so tests can control it. At() never allocates;
// Lines() copies the lines out only when they change. Thread-safe.
class Model
{
public:
  static constexpr double FADE_MS = 400;
  static constexpr std::size_t KEEP = 3;
  // A blink is RINGS rings of RING_MS each.
  static constexpr double RING_MS = 700;
  static constexpr int RINGS = 3;
  // While listening, a green ring grows out of the button once per LISTEN_RING_MS.
  static constexpr double LISTEN_RING_MS = 1200;

  struct Entry
  {
    double at = 0;  // arrival time, ms
    Notice notice;
  };
  struct TalkEntry
  {
    double at = 0;  // arrival time, ms
    Talk talk;
  };
  struct View
  {
    bool on = false;
    Place place;
    // Blink ring progress 0..1, or negative for none.
    float ring = -1;
    // The ring is the listening pulse (green), not a blink.
    bool listening = false;
    // Bumped whenever the lines change.
    std::uint64_t generation = 0;
    // Bumped whenever the talk row changes.
    std::uint64_t talk_generation = 0;
    bool showing = false;
    // View width in device pixels from the last `rect`, or 0.
    float view_width = 0;
  };

  void Apply(const Command& command, double now_ms);
  void Add(Notice notice, double now_ms);
  // Replaces the talk row (phase Off removes it).
  void SetTalk(Talk talk, double now_ms);
  // The app's `rect`: the view's size in device pixels.
  void SetView(int width, int height);
  View At(double now_ms) const;
  // Copies the newest KEEP lines, oldest first, into `out` if `generation` is stale. Returns the
  // current generation.
  std::uint64_t Lines(std::uint64_t generation, std::vector<Entry>* out) const;
  // Copies the talk row into `out` if `generation` is stale. Returns the current talk generation.
  std::uint64_t TalkRow(std::uint64_t generation, TalkEntry* out) const;
  // Whether (x, y), in view pixels, is on the button's disc.
  bool Hit(float x, float y) const;

private:
  mutable std::mutex m_lock;
  bool m_on = false;
  Place m_place;
  double m_blink_at = -1;
  std::deque<Entry> m_lines;
  std::uint64_t m_generation = 0;
  TalkEntry m_talk;
  std::uint64_t m_talk_generation = 0;
  double m_listen_at = 0;  // when the mic opened: the pulse runs on through the live words
  int m_view_width = 0;
};

// Host controls require agreed cap "hostui" and never write a pad or guest memory.
// Up + Start opens this menu using the
// existing corner shortcut; D-pad moves, A selects and B closes. The focused app relays its
// feedback shortcut over stdin, without any host keyboard polling.
enum class HostAction
{
  Feedback,
  VolumeUp,
  VolumeDown,
  Mute,
  ExitFullscreen,
  Leave,
  Controls,
};
inline constexpr int HOST_ROWS = 7;
std::string_view HostLabel(HostAction action);
std::string_view HostLine(HostAction action);

class HostMenu
{
public:
  struct View
  {
    bool open = false;
    int selected = 0;
  };
  void Toggle();
  void Close();
  View At() const;
  std::optional<HostAction> Pad(u16 buttons, bool toggle);
  std::optional<HostAction> Move(int direction, bool select);
  bool FeedbackDue(double now_ms);
  void FeedbackFinished(double now_ms);
  bool HostDue(double now_ms);

private:
  mutable std::mutex m_lock;
  View m_view;
  u16 m_buttons = 0;
  double m_feedback_at = -5000;
  bool m_feedback_pending = false;
  std::deque<double> m_host_actions;
};

bool HostEnabled();
HostMenu& Menu();
// The app's placement, or a visible default with agreed hostui before its first orb line.
Model::View HostView(double now_ms);
// View pixels, matching the platform's input coordinates. Includes the feedback button and menu.
bool HitControls(float x, float y);
bool ClickControls(float x, float y);
void ToggleHostMenu();
enum class HostKey
{
  Feedback,
  Menu,
  Previous,
  Next,
  Select,
  Close,
};
bool HandleHostKey(HostKey key);
void NavigateHostPad(u16 buttons, bool toggle);
void ActivateHost(HostAction action);
// Publish a completed capture, then return its control line. Failure leaves no published file.
// The temporary file is <directory>/<name>.tmp and the final name is always bare.
std::optional<std::string> PublishFeedback(const std::string& directory, std::string_view name,
                                          bool saved);

// The process-wide button. The stdin thread updates it and the video thread draws it. NowMs() is
// steady time in ms.
Model& Current();
double NowMs();
// Apply the app's `orb`, `notice`, `talk` and `rect` lines. Any thread.
void Apply(const Command& command);
void Show(Notice notice);
void SetTalk(Talk talk);
void SetViewSize(int width, int height);
}  // namespace Orca::UX::YgOrb
