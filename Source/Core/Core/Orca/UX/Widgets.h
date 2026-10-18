// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <imgui.h>

// Overlay building blocks for screens such as the set's end (SetEnd.h): panel, title, text line,
// big number, countdown, rating change and hold hint. They draw with Kit.h in the game's own fonts
// and menu style. Callers lay them out in overlay units (1% of the picture's height).
// Host-only drawing: nothing here reads or writes emulated state.
namespace Orca::UX::Widgets
{
// Picks a widget's colours.
enum class Tone : int
{
  Neutral,  // panel ink
  Win,      // gold
  Loss,     // red
  Warning,  // amber
};

// Sizes, in overlay units.
constexpr float TITLE_SIZE = 10.0f;   // VICTORY / DEFEAT
constexpr float HEADING_SIZE = 5.0f;  // "bo disconnected", "YOU WON"
constexpr float LINE_SIZE = 3.4f;     // "2-0 vs bo"
constexpr float SMALL_SIZE = 2.8f;    // "Rating updating…"
constexpr float HINT_SIZE = 3.2f;     // "Hold B to leave"
constexpr float NUMBER_SIZE = 6.5f;   // rating
constexpr float COUNTDOWN_SIZE = 6.0f;  // countdown digits

// Size of a Line's `text` at `size` pixels.
ImVec2 Measure(float size, std::string_view text);
// Sizes of a Title's text and a BigNumber's digits.
ImVec2 MeasureTitle(float unit, std::string_view text);
ImVec2 MeasureNumber(float unit, std::string_view digits);

// The game-styled panel behind a group of lines.
void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha);

// Big title text (VICTORY, DEFEAT) centred on `centre_x`, scaled about its centre. Returns its
// size at scale 1.
ImVec2 Title(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
             Tone tone, float alpha, float scale = 1);

// A line of text `size` pixels tall, centred on `centre_x`. Returns its size.
ImVec2 Line(ImDrawList* dl, float centre_x, float top, float size, std::string_view text, Tone tone,
            float alpha);

// A big number (a rating), scaled about its centre. Returns its size at scale 1.
ImVec2 BigNumber(ImDrawList* dl, float centre_x, float top, float unit, int value, float alpha,
                 float scale = 1);

// Seconds left inside a ring that drains as `fraction` goes from 1 to 0. Returns the outer radius.
float Countdown(ImDrawList* dl, ImVec2 centre, float unit, int seconds, float fraction, Tone tone,
                float alpha);
// The countdown's outer radius, for layout.
float CountdownRadius(float unit);

// A rating change badge ("+16" green, "-16" red, "+0" grey). Returns its size at scale 1.
ImVec2 DeltaBadge(ImDrawList* dl, float left, float centre_y, float unit, int delta, float alpha,
                  float scale = 1);
// The badge's size, for layout.
ImVec2 DeltaBadgeSize(float unit, int delta);

// A hint ("Hold B to leave") with a bar that fills with `progress` (0..1). Returns its size.
ImVec2 Hint(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
            float progress, float alpha);

// Formats seconds as "m:ss".
std::string_view ClockText(int seconds, char (&buffer)[16]);
}  // namespace Orca::UX::Widgets
