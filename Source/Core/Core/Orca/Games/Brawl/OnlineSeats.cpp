// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/OnlineSeats.h"

#include <bit>
#include <string>

#include "Common/Logging/Log.h"
#include "Core/Orca/Games/Brawl/CssTitle.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX::OnlineSeats
{
namespace
{
namespace B = MatchBlock;

constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 AREA_KIND = 0x1B4;

// Where the executable's vtables are (both games run the same one).
constexpr u32 VTABLES_FROM = 0x80400000;
constexpr u32 VTABLES_TO = 0x80600000;

constexpr const char* SCENE_CSS = "scSelctCharacter";
constexpr const char* SCENE_BETWEEN = "scMemoryChange";

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p) && m.Valid(p + 3);
}

// gmSelCharData, if it can be read.
u32 Record(const GuestMemory& m)
{
  if (!Pointer(m, GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + GLOBAL_SEL_CHAR))
    return 0;
  const u32 record = m.Read32(global + GLOBAL_SEL_CHAR);
  return Pointer(m, record) ? record : 0;
}

// The character select's player area of port `port` (0-3) with its kind readable, on the
// character select only.
u32 Area(const GuestMemory& m, int port)
{
  const u32 area = Rules::ReadCssArea(m, port);
  return Pointer(m, area) && Pointer(m, area + AREA_KIND) ? area : 0;
}

bool Supported()
{
  return Rules::ProfileRuleset() != Rules::Ruleset::None;
}
}  // namespace

bool Online(const Rules::Header& header, u32 pick, int players)
{
  if (header.Locked())
    return true;
  if ((pick >= PICK_FRIENDS_FIRST && pick <= PICK_FRIENDS_LAST) || pick == PICK_CASUAL ||
      pick == PICK_RANKED)
  {
    return true;
  }
  return players >= 2;
}

bool SeatHidden(bool online, const Rules::Header& header, int port, u32 kind, bool plugged,
                bool searching)
{
  if (!online || kind != AREA_KIND_NONE || plugged)
    return false;
  // The opponent's seat on a queue select: always in a queue room, and on the queue's own select
  // while it searches (the SEARCHING plate).
  if (port == 1 && header.Locked() && (!header.Solo() || searching))
    return false;
  return true;
}

bool HandMayPressA(const Rules::CssHand& hand)
{
  // An unreadable hand (the select being built or torn down) gets no A.
  if (!hand.valid)
    return false;
  if (hand.target == Rules::CSS_HAND_BUTTON &&
      (hand.button == Rules::CSS_BUTTON_PLAYER_TYPE || hand.button == CSS_BUTTON_NAME))
  {
    return false;
  }
  // A NaN position fails this test: no A.
  return hand.y >= TYPE_BUTTONS_A_FLOOR;
}

bool PressesJoin(const Rules::CssHand& hand, int port, u32 kind, bool token_in_hand,
                 bool token_flying)
{
  return hand.valid && hand.target == Rules::CSS_HAND_BUTTON &&
         hand.button == Rules::CSS_BUTTON_PLAYER_TYPE && hand.panel == port &&
         kind == AREA_KIND_NONE && !token_in_hand && !token_flying;
}

u8 PluggedInMemory(const GuestMemory& m)
{
  if (m.Valid(B::FRIENDS_SEEN) && m.Read8(B::FRIENDS_TAG) == B::FRIENDS_TAG_VALUE)
    return m.Read8(B::FRIENDS_SEEN);
  return 1;
}

bool ReadOnline(const GuestMemory& m, u8 plugged)
{
  return Online(Rules::ReadHeader(m), CssTitle::ReadPick(m), std::popcount(plugged));
}

int ClearRecord(GuestMemory& m, u8 plugged)
{
  if (ReadSceneName(m) != SCENE_BETWEEN || !ReadOnline(m, plugged))
    return 0;
  const u32 record = Record(m);
  if (!record)
    return 0;
  int changed = 0;
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 at = record + RECORD_PLAYERS + port * RECORD_PLAYER_SIZE + RECORD_STATE;
    if (!m.Valid(at))
      continue;
    const u8 state = m.Read8(at);
    const u8 want = RecordStateFor(state, (plugged >> port) & 1);
    if (want != state)
    {
      m.Write8(at, want);
      ++changed;
    }
  }
  return changed;
}

int ClearPanels(GuestMemory& m, u8 plugged)
{
  if (ReadSceneName(m) != SCENE_CSS || !ReadOnline(m, plugged))
    return 0;
  int changed = 0;
  for (int port = 0; port < 4; ++port)
  {
    const u32 area = Area(m, port);
    if (!area)
      continue;
    const u32 kind = m.Read32(area + AREA_KIND);
    const u32 want = PanelKindFor(kind);
    if (want != kind)
    {
      m.Write32(area + AREA_KIND, want);
      changed += 4;
    }
  }
  return changed;
}

int ApplySeats(GuestMemory& m, u8 plugged, u8* hidden_out)
{
  if (hidden_out)
    *hidden_out = 0;
  if (ReadSceneName(m) != SCENE_CSS)
    return 0;
  const Rules::Header header = Rules::ReadHeader(m);
  const bool online = Online(header, CssTitle::ReadPick(m), std::popcount(plugged));
  const bool searching = header.Solo() && (Queue::ReadState(m).ready & 1) != 0;
  int written = 0;
  u8 hidden = 0;
  for (int port = 0; port < 4; ++port)
  {
    const u32 area = Area(m, port);
    if (!area)
      continue;
    const bool hide = SeatHidden(online, header, port, m.Read32(area + AREA_KIND),
                                 ((plugged >> port) & 1) != 0, searching);
    if (hide)
      hidden |= static_cast<u8>(1 << port);
    for (u32 slot = 0; slot < AREA_MODEL_COUNT; ++slot)
    {
      if (!Pointer(m, area + AREA_MODELS + 4 * slot))
        continue;
      const u32 object = m.Read32(area + AREA_MODELS + 4 * slot);
      if (!Pointer(m, object) || !Pointer(m, object + MUOBJECT_SCENE_MODEL))
        continue;
      const u32 model = m.Read32(object + MUOBJECT_SCENE_MODEL);
      if (!Pointer(m, model) || !Pointer(m, model + SCENE_MODEL_FLAGS))
        continue;
      // An nw4r scene object starts with its vtable, in the executable's data.
      if (const u32 vtable = m.Read32(model); vtable < VTABLES_FROM || vtable >= VTABLES_TO)
        continue;
      const u32 flags = m.Read32(model + SCENE_MODEL_FLAGS);
      // Only the game's flags, with or without Orca's two bits: anything else isn't a panel model,
      // so leave it alone.
      if ((flags & ~MODEL_FLAGS_HIDDEN) != MODEL_FLAGS_GAME)
        continue;
      const u32 want = hide ? (flags | MODEL_FLAGS_HIDDEN) : (flags & ~MODEL_FLAGS_HIDDEN);
      if (want != flags)
      {
        m.Write32(model + SCENE_MODEL_FLAGS, want);
        ++written;
      }
    }
  }
  if (hidden_out)
    *hidden_out = hidden;
  return written;
}

Rollback::InputGate::Masks GateMasks(const GuestMemory& m)
{
  Rollback::InputGate::Masks masks{};
  if (ReadSceneName(m) != SCENE_CSS)
    return masks;
  const Rules::Header header = Rules::ReadHeader(m);
  // Under a header the online rules pass A only in the grid (OnlineRules.h).
  const u8 plugged = PluggedInMemory(m);
  if (header.Locked() || !Online(header, CssTitle::ReadPick(m), std::popcount(plugged)))
    return masks;
  for (int port = 0; port < Rollback::InputGate::PORTS; ++port)
  {
    const Rules::CssHand hand = Rules::ReadCssHand(m, port);
    const u32 area = Area(m, port);
    const Rules::CssToken token = Rules::ReadCssToken(m, port);
    const bool return_token = std::popcount(plugged) > 2 && ((plugged >> port) & 1) && area &&
                              m.Read32(area + AREA_KIND) == AREA_KIND_HUMAN && token.valid &&
                              token.in_hand && !token.flying && hand.valid &&
                              hand.target == Rules::CSS_HAND_BUTTON &&
                              hand.button == Rules::CSS_BUTTON_PLAYER_TYPE && hand.panel == port;
    const bool list = Rules::ReadCssNameList(m, port);
    // Returning the held token to one's own human panel lets this friend sit out next round.
    if (return_token)
    {
      masks[port].a_centres_stick = true;
    }
    // The player's own tag and its controls: A on their own name button and in its list.
    else if (Rules::CssNameTakesA(hand, port, list))
    {
      masks[port].a_centres_stick = true;
      if (list)
        masks[port].buttons |= PAD_TRIGGER_L;
    }
    else if (!HandMayPressA(hand))
    {
      masks[port].buttons |= PAD_BUTTON_A;
    }
    if (((plugged >> port) & 1) && area && token.valid &&
        PressesJoin(hand, port, m.Read32(area + AREA_KIND), token.in_hand, token.flying))
    {
      masks[port].press |= PAD_BUTTON_A;
    }
  }
  return masks;
}

void ClearFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged)
{
  if (!Supported())
    return;
  GuardMemory m(guard);
  const int record = ClearRecord(m, plugged);
  const int panels = ClearPanels(m, plugged);
  // Log first runs only: a re-run from an earlier snapshot writes the same bytes again.
  if ((record > 0 || panels > 0) && !resimulating)
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Online seats: frame {}: {} record state(s) and {} panel(s) cleared (no CPUs "
                   "online; ports plugged {:#x})",
                   frame, record, panels / 4, plugged);
  }
}

void SeatsFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged)
{
  if (!Supported())
    return;
  GuardMemory m(guard);
  u8 hidden = 0;
  const int written = ApplySeats(m, plugged, &hidden);
  if (written > 0 && !resimulating)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online seats: frame {}: panels hidden {:#x} ({} model flags written)",
                   frame, hidden, written);
  }
}
}  // namespace Orca::UX::OnlineSeats
