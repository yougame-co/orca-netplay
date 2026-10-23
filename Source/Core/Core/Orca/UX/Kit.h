// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

// Orca's overlay kit: the drawing primitives behind every element Orca adds to the screen, styled
// to look like part of Brawl's or Project+'s own menus rather than text over the picture.
//
//   - Text uses the game's own fonts, read at run time from the player's disc (GameFont.h): Body is
//     the menus' rounded sans, Heavy the display face. Until the fonts load, ImGui's font is used
//     with the same colours, outline and shadow.
//   - Look picks the style: Brawl (rose plates, white rim, cream-to-gold titles) or Project+ (flat
//     teal slabs, white-to-mint titles). Set by the frame hook from the session's ruleset.
//   - Plate: rounded gradient panel with rim, gloss and drop shadow.
//   - Banner: a band with cut ends and a heavy title (like "READY TO FIGHT").
//   - Title / Label: heavy italic title, or smaller outlined body text.
//   - Timer: "0:24" on a small plate; gold under 10 s, red and pulsing under 5.
//   - Badge: a pill in a port's colour ("LOCKED IN", name tabs).
//   - Pointer / Frame: a player's cursor ring or a framed tile, with their tab.
//   - PortraitFrame / Ribbon: character select portrait highlight and its LOCKED IN ribbon.
//   - SetDots: a best-of-three's games as dots; SearchRing: an empty seat while searching.
//   - Connection: link quality lamp with one to three lit arcs.
//
// Drawn with the host's ImGui over the presented picture, so it is display only and never touches
// emulation. Sizes are pixels; `unit` is 1% of the picture's height (OverlayUnit).
namespace Orca::UX::GameFont
{
struct Atlas;
}

namespace Orca::UX::Kit
{
enum class Face
{
  Body,
  Heavy,
};

enum class Align
{
  Left,
  Centre,
  Right,
};

// Which game's menus the kit imitates.
enum class Look
{
  Brawl,
  ProjectPlus,
};
// Set by the frame hook from the session's ruleset. Any thread.
void SetLook(Look look);
Look CurrentLook();

// Kit colours. PortTone() maps a port to its panel colour.
enum class Tone
{
  Panel,  // the look's own panel colour (Brawl rose, Project+ teal)
  Gold,  // agreement, current turn, a win
  Grey,  // inactive, hints
  Red,   // port 1, a loss, time running out
  Blue,  // port 2
  Yellow,
  Green,
};
Tone PortTone(int port);

struct TextLook
{
  Face face = Face::Body;
  float px = 20;  // line height in pixels
  ImU32 top = IM_COL32_WHITE;
  ImU32 bottom = IM_COL32_WHITE;
  float outline = 0;  // pixels; 0 = none
  ImU32 outline_colour = IM_COL32(12, 14, 24, 255);
  ImVec2 shadow{0, 0};  // offset in pixels; (0, 0) = none
  ImU32 shadow_colour = IM_COL32(0, 0, 0, 150);
  float italic = 0;    // rightward shear per pixel of height
  float tracking = 0;  // pixels added after every glyph
};

// Body text: white, outlined, small shadow.
TextLook LabelLook(float unit, float size = 3.0f);
// Title text: heavy italic in the look's title gradient, outlined and shadowed.
TextLook TitleLook(float unit, float size = 5.0f);

// The disc to read fonts from. They are loaded once, on a background thread, the first time the
// kit draws. Any thread, before Init.
void SetGameDisc(std::string path);
// Call each overlay frame before drawing (video thread): uploads the fonts once loaded.
void BeginFrame();
// The backend is shutting down (video thread, through OSD::ReleaseHostOverlay): frees the fonts'
// textures while their device is alive. The next backend's first frame uploads them again.
void ReleaseGpu();
// Waits for the font loader thread at exit.
void Shutdown();
// Whether `face` has switched to the game's font yet.
bool HasGameFont(Face face);

ImVec2 Measure(const TextLook& look, std::string_view text);
// Draws `text` with its top-left at `pos` (moved for `align`); returns its size.
ImVec2 Text(ImDrawList* dl, ImVec2 pos, const TextLook& look, std::string_view text,
            float alpha = 1, Align align = Align::Left);

// Word-wraps `text` to `width`. A word longer than the width gets its own line.
std::vector<std::string> Wrap(const TextLook& look, std::string_view text, float width);

struct Box
{
  ImVec2 min, max;
};

// A game-style panel over [min, max].
void Plate(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha = 1);
// A band with `title` centred, and an optional `note` below it in body text ('\n' separates
// lines; size the band with BannerHeight). Returns the band.
Box Banner(ImDrawList* dl, float left, float right, float top, float height, float unit,
           Tone tone, std::string_view title, std::string_view note = {}, float alpha = 1);
// Height of a Banner whose title row is `row` tall, with `note_lines` lines of note.
float BannerHeight(float row, std::size_t note_lines);
// Width a Banner needs for its text, cut ends and margin.
float BannerWidth(float height, float unit, std::string_view title, std::string_view note = {});
// Heavy italic title at `pos`. Returns its box.
Box Title(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 5.0f,
          Align align = Align::Left, float alpha = 1);
// One line of body text. Returns its box.
Box Label(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 3.0f,
          Align align = Align::Left, ImU32 colour = IM_COL32_WHITE, float alpha = 1);
// One line of body text on a padded plate (toasts, chat, hints). Returns the plate.
Box LabelPlate(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 3.0f,
               Align align = Align::Left, Tone tone = Tone::Panel, float alpha = 1);
// "m:ss" on a plate. `now_s` (ImGui's clock) drives the final-seconds pulse. Returns the plate.
Box Timer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s, float alpha = 1);
// A pill with `text` in white capitals. Returns the pill.
Box Badge(ImDrawList* dl, ImVec2 centre, float unit, std::string_view text, Tone tone,
          float size = 3.6f, float alpha = 1);
// Blink animation for whoever has to act. 2.5 beats per second (under the 3 Hz flash limit), lit
// for 60% of each beat, with the cursor swelling up to 15%. The first 0.8 s of a turn adds three
// ripples so the eye catches the handover. `ripples[i]` runs 0..1, or -1 when inactive.
struct Blink
{
  float on = 0;
  float swell = 0;
  std::array<float, 3> ripples{-1, -1, -1};
};
Blink Attention(double now_s, double since_s);
// Plain: no turn. Acting: blinks with a glow. Waiting: ring greyed.
enum class PointerMode
{
  Plain,
  Acting,
  Waiting,
};
// A player's cursor ring in their port's colour, with `tag` on a tab to the lower left or right.
// While Acting it blinks from `since_s`.
void Pointer(ImDrawList* dl, ImVec2 at, float radius, float unit, int port, std::string_view tag,
             PointerMode mode, bool tag_left, double now_s, double since_s);
// Same, with `turn` meaning Acting since time zero (demos and tests).
void Pointer(ImDrawList* dl, ImVec2 at, float radius, float unit, int port, std::string_view tag,
             bool turn, bool tag_left, double now_s);
// A turn label next to a cursor: `title` over optional `detail` on a pill, with a timer when
// `seconds` >= 0. Placed above the cursor (or below with `below`, or near the top edge), kept
// within [clamp_min, clamp_max]. `rim` 0..1 lights a white rim (used on the local player's turn).
// Returns the pill.
Box TurnTag(ImDrawList* dl, ImVec2 at, float radius, float unit, std::string_view title,
            std::string_view detail, Tone tone, int seconds, double now_s, bool lean_left,
            ImVec2 clamp_min, ImVec2 clamp_max, float rim = 0, float size = 3.4f,
            bool below = false);
// One-beat-per-second pulse: `swell` 0..1..0 on a cosine, `ripple` 0..1 through the beat.
struct Beat
{
  float swell = 0;
  float ripple = 0;
};
Beat TurnPulse(double now_s);
// Frames a character select portrait with a soft glow (gold when locked in, the port's colour on
// their pick).
void PortraitFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone);
// A ribbon with heavy `text` shrunk to fit, e.g. LOCKED IN on a portrait. Returns the ribbon.
Box Ribbon(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, std::string_view text, Tone tone);
// Three dots for a best-of-three. games[i] is the winning port (0 or 1) or -1; `current` is the
// 0-based game in progress (brighter rim), or -1. Returns the dots' box.
Box SetDots(ImDrawList* dl, ImVec2 centre, float unit, const std::array<int, 3>& games, int current);
// A spinning gold arc on a pale track, for an empty seat while searching.
void SearchRing(ImDrawList* dl, ImVec2 centre, float radius, float unit, double now_s);
// Which on-screen game text a timer sits beside, so it matches that text's style.
enum class Lettering
{
  Band,  // the READY TO FIGHT! band
  Line,  // the stage select's STAGE SELECT title
};
// A plate-less "m:ss" timer in `lettering`'s style, swelling on each of the last five seconds.
// Returns the box of "0:00" at `size`, so the layout never shifts.
Box GameTimer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s,
              Lettering lettering, float size = 4.2f);
// A control guide entry: a GameCube button name (empty for plain words) and what it does.
struct GuideItem
{
  std::string button;
  std::string label;
  bool operator==(const GuideItem&) const = default;
};
// Parses a hint into guide entries, e.g. "B to change · Hold B to leave" becomes B "Change" and
// B "Hold: leave". Parts that name no button stay as words.
std::vector<GuideItem> GuideOf(std::string_view hint);
// A control guide like the games' own, right-aligned at `right`: buttons in GameCube colours with
// labels. Shrinks to as little as 70% to fit `max_width` (0 = unlimited). Returns its box.
Box Guide(ImDrawList* dl, float right, float cy, float unit, const std::vector<GuideItem>& items,
          float alpha = 1, float max_width = 0);
// Frames a tile in `tone` with `tag` on a tab on its top edge.
void Frame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, std::string_view tag,
           bool tag_left, float inset = 0);
// Connection lamp with `bars` (1-3) arcs lit: blue for three, gold for two, red for one.
void Connection(ImDrawList* dl, ImVec2 centre, float radius, float unit, int bars,
                float alpha = 1);

// The look's accent colour (Brawl gold, Project+ mint), e.g. for a chat sender's name.
ImU32 Accent();
// Body text styled to sit on a `tone` plate. `size` is in overlay units.
TextLook InkLook(float unit, float size, Tone tone = Tone::Panel);
// The size Badge() would draw.
ImVec2 BadgeSize(float unit, std::string_view text, Tone tone, float size = 3.6f);
// Several lines on one padded plate (e.g. a wrapped chat message). The first `lead` bytes of the
// first line use the accent colour (the sender's name). Returns the plate.
Box TextPlate(ImDrawList* dl, ImVec2 pos, float unit, const std::vector<std::string>& lines,
              float size = 3.0f, std::size_t lead = 0, Tone tone = Tone::Panel, float alpha = 1);
// A button-style disc (shadow, rim, gloss), used behind the lamp and the countdown.
void Disc(ImDrawList* dl, ImVec2 centre, float radius, float unit, Tone tone, float alpha = 1);

// Tests only: draw `face` with `atlas` as `texture` (null restores the normal one). The atlas must
// outlive its use.
void UseAtlasForTests(Face face, const GameFont::Atlas* atlas, ImTextureID texture);
// Tests only: BeginFrame uploads these as if read from the disc, without the reader thread (both
// null: nothing read).
void UseFontsForTests(const GameFont::Atlas* body, const GameFont::Atlas* heavy);

// ORCA_UX_KIT_DEMO=1 draws every primitive (DrawDemo) instead of the overlay, for design checks.
int DemoMode();
void DrawDemo(ImDrawList* dl, ImVec2 picture_min, ImVec2 picture_size, float unit, double now_s);
}  // namespace Orca::UX::Kit
