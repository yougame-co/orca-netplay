// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"

// Draws YouGame-styled cards (chat messages, room notices) over the game, matching the cards on the
// YouGame site. Text is Roboto (Data/Sys/Orca/Fonts, Apache 2.0). Local display only: nothing here
// reads emulated memory. Call from the video thread inside the ImGui frame.
namespace Orca::UX::Yg
{
// Loads the card font into ImGui's atlas so the file is read in the menus, not mid-match.
// Cheap after the first call.
void Warm();
// The site's avatar colour for `name`: FNV-1a of the lowercased UTF-16 name over seven colours.
// Matches the site for ASCII and Latin-1 names.
ImU32 AvatarColour(std::string_view name);
// `text`'s width at `px` pixels in the card font.
float TextWidth(std::string_view text, float px);
// Wraps `text` into lines no wider than `width`, breaking long words if needed.
std::vector<std::string> Wrap(std::string_view text, float px, float width);
// Draws a chat message card at `pos`, at most `width` wide, and returns its box. `unit` is 1% of
// the picture's height.
Kit::Box ChatCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view name,
                  std::string_view text, float width, float alpha = 1);
// Draws a notice card (someone joined or left) with the YouGame logo in place of an avatar.
Kit::Box NoticeCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float width,
                    float alpha = 1);
// The YouGame logo, `size` pixels square, top-left at `pos`.
void Logo(ImDrawList* dl, ImVec2 pos, float size, float alpha = 1);

// Helpers for the YouGame button (YgOrb.h).
// The round logo (controller on a red disc), `size` pixels across, top-left at `pos`.
void RoundLogo(ImDrawList* dl, ImVec2 pos, float size, float alpha = 1);
// A line of text in the card font, top-left at `pos`, `px` pixels high.
void Text(ImDrawList* dl, ImVec2 pos, float px, ImU32 colour, std::string_view text);
// A card's slab over [min, max].
void CardSlab(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, float alpha = 1);
// Card colours: main text, secondary text, hairline edge.
ImU32 Ink(float alpha = 1);
ImU32 Ink2(float alpha = 1);
ImU32 Edge(float alpha = 1);
// An avatar: `name`'s initial on its colour, a disc of radius `r` round `centre`.
void Avatar(ImDrawList* dl, ImVec2 centre, float r, std::string_view name, float alpha = 1);
// Avatar with a precomputed colour and initial, for callers that draw the same person every frame.
std::string InitialOf(std::string_view name);
void AvatarOf(ImDrawList* dl, ImVec2 centre, float r, ImU32 colour, std::string_view initial,
              float alpha = 1);
}  // namespace Orca::UX::Yg
