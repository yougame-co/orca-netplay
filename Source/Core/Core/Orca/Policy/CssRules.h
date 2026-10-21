// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

// The character select's hand targets and two pure rules over them, which the policy machines
// (Policy/Queue.h, Policy/CharOrder.h) and the Brawl module's locks (Games/Brawl/OnlineRules.h)
// share. Moved with the machines in S2 commit 7, values unchanged; they are Brawl's (and Project+'s)
// own and become game data in S5.
namespace Orca::UX::Rules
{
// The hand's target (+0x80) and button (+0xAC) on the character select.
constexpr u32 CSS_HAND_NOTHING = 0;
constexpr u32 CSS_HAND_BUTTON = 1;  // button id at +0xAC: 0x1D player type, 0x1C name, ...
constexpr u32 CSS_HAND_GRID = 2;    // over the grid, token placed
constexpr u32 CSS_HAND_OWN_TOKEN = 3;  // A picks it up
constexpr u32 CSS_HAND_EXIT = 4;       // BACK (button 2) or Rules (4)
constexpr u32 CSS_HAND_TAKING = 6;     // token moving into the hand
constexpr u32 CSS_HAND_GRID_HOLDING = 7;  // over the grid, token in hand (A places it)
constexpr u32 CSS_HAND_PLACING = 8;       // token moving down
constexpr u32 CSS_BUTTON_PLAYER_TYPE = 0x1D;
constexpr u32 CSS_BUTTON_NAME = 0x1C;
constexpr u32 CSS_BUTTON_BACK = 0x02;  // with CSS_HAND_EXIT (Rules is 0x04)

// The hand is on BACK. Pure.
constexpr bool CssHandOnBack(u32 target, u32 button)
{
  return target == CSS_HAND_EXIT && button == CSS_BUTTON_BACK;
}

// One player's token, as the character select shows it. B on a placed token undoes the pick.
struct CssToken
{
  bool valid = false;  // the player's area could be read
  bool human = false;
  int character = 0x28;  // under the hand while held, else the token's (0x28 = none)
  bool in_hand = true;   // +0x1F8
  bool flying = false;   // +0x1F9: moving up or down
  // Placed on a character and still: B picks it up.
  constexpr bool Down() const { return valid && human && character != 0x28 && !in_hand && !flying; }
  bool operator==(const CssToken&) const = default;
};
// Whether B may reach the game, given the token and whether B was held last frame. Pure.
constexpr bool CssBUnpicks(const CssToken& token, bool b_held)
{
  return token.Down() && !b_held;
}
}  // namespace Orca::UX::Rules
