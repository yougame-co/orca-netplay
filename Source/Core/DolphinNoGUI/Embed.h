// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Embed mode: Orca draws inside the YouGame desktop app's window instead of its own. The app starts
// it with
//   --embed --parent <handle> --rect X Y W H     (or --rect X,Y,W,H)   [--embed-adopted]
// then sends one command per line on stdin and reads one event per line on stdout. Lines starting
// with "yg " are the stdio bridge's (Orca/Bridge.h), never commands.
// See ORCA.md, "Embedding".
// With cap "shortcuts": `embed shortcuts menu=Shift+Tab chat=Shift+Enter` atomically replaces
// the two window shortcuts. Either field may be omitted; a bare command clears both. Each combo
// is distinct Shift/Ctrl/Alt modifiers joined by '+', then Tab, Enter, A-Z, F1-F12 or Backquote.
// Key identities are physical US keyboard positions, matching Quartz/DInput/evdev mapping
// names. X11 uses XKB physical key names for both window events and XInput2 metadata;
// existing logical XInput2 display/mapping labels remain intact. For example, AZERTY A
// at the US Q position is shortcut Q. No layout-dependent event letter is compared to
// a physical backend label. Embedded input is classified from existing controller snapshots
// while the app grants game focus; standalone windows use their own keyboard events.
// Malformed lines keep the previous bindings. Chords and their modifiers are hidden from controller
// mappings; edges use `yg {"overlay":"down|up"}` or `yg {"chat":"down|up|cancel"}` on stdout.
namespace Embed
{
struct Rect
{
  int x = 0;
  int y = 0;
  int w = 1;
  int h = 1;
};

// Actual bounds in screen coordinates (macOS points; Windows/X11 pixels).
struct WindowBounds
{
  double x, y, w, h;
  double scale;
};
bool TraceEnabled();
void Trace(const Rect& requested, const WindowBounds& actual);

struct Options
{
  bool enabled = false;
  // HWND on Windows, CGWindowID (the window number) on macOS, the X11 window id on Linux, as a
  // decimal number.
  unsigned long long parent = 0;
  Rect rect;
  // The requested --rect was TooSmall, so rect was raised to MIN_SIDE and the view starts hidden.
  bool too_small = false;
  // --embed-adopted (Windows, the sandbox): the window starts as a hidden top-level window and
  // waits for the app's launcher to make it a child of `parent`, since a sandboxed process may not
  // create a window under the app's. Elsewhere it changes nothing: a Mac's Orca window is top-level
  // anyway.
  bool adopted = false;
};

// One decisive self-embedding result, held until ready. Adopted sessions leave this disabled.
class Result
{
public:
  explicit Result(bool enabled = false) : m_enabled(enabled) {}
  void Decide(std::string_view reason = {});
  bool Decided() const { return !m_enabled || m_decided; }
  std::optional<std::string> TakeLine(bool ready);

private:
  bool m_enabled = false;
  bool m_decided = false;
  std::optional<std::string> m_line;
};

// Removes --embed, --embed-adopted, --parent and --rect from argv so the regular parser never sees
// them. Returns nullopt and sets *error when they are malformed.
std::optional<Options> TakeArguments(std::vector<char*>* argv, std::string* error);

// Parses "X Y W H" or "X,Y,W,H"; normally clamps w/h to 1. Raw sizes are for tracing commands.
std::optional<Rect> ParseRect(const std::string& text, bool clamp_size = true);

// A rect with a side under MIN_SIDE pixels hides the game instead of resizing it, until a big
// enough rect arrives. A --rect that small opens the window at MIN_SIDE, hidden. Very thin windows
// can crash the overlay.
constexpr int MIN_SIDE = 32;
bool TooSmall(const Rect& rect);

// Gives the protocol a private copy of stdout and points fd 1 at stderr, so log output never mixes
// with protocol lines. Returns the protocol stream. Call once, at startup.
std::FILE* ClaimStdout();

// Writes one protocol line. Adds the newline; embedded newlines become spaces.
void Out(const std::string& line);

// Reads stdin on its own thread. A closed stdin counts as "quit". Bridge lines go to Orca::Bridge
// on that thread; a line over Orca::Bridge::MAX_LINE is dropped.
void StartReading();
// Returns the commands read since the last call, oldest first.
std::vector<std::string> TakeCommands();
}  // namespace Embed
