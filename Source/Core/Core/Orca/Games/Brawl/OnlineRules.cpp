// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/OnlineRules.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineSeats.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX::Rules
{
namespace
{
namespace B = MatchBlock;

constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_MODE_MELEE = 0x08;
constexpr u32 GLOBAL_SET_RULE = 0x1C;
// gmGlobalModeMelee player init data: 4 x 0x5C from +0x98 (+0x00 character, +0x01 type: 0 human,
// 1 CPU, 3 none).
constexpr u32 MELEE_PLAYERS = 0x98;
constexpr u32 MELEE_PLAYER_SIZE = 0x5C;
constexpr u8 PLAYER_HUMAN = 0;

// gmSetRule (BrawlHeaders gm_set_rule.h): +0x02 rule in the low 3 bits (0 time, 1 stock), +0x04
// stocks, +0x05 handicap, +0x06 damage ratio x10, +0x07 stage choice (0 Choose, 1 Random), +0x08
// stock match time limit in minutes, +0x09 team attack, +0x0A pause.
constexpr u32 RULE_MODE = 0x02;
constexpr u32 RULE_STOCKS = 0x04;
constexpr u32 RULE_HANDICAP = 0x05;
constexpr u32 RULE_RATIO = 0x06;
constexpr u32 RULE_STAGE_CHOICE = 0x07;
constexpr u32 RULE_STOCK_MINUTES = 0x08;
constexpr u32 RULE_TEAM_ATTACK = 0x09;
constexpr u32 RULE_PAUSE = 0x0A;
constexpr u32 RULE_SAVED_FROM = 0x02;
constexpr u32 RULE_SAVED_SIZE = 10;  // +0x02..+0x0B
constexpr u8 STAGE_CHOOSE = 0;

// gmGlobalRecord menu data (0x9017B640 + 0x810, both games): +0x00 item frequency (u8, 0 None),
// +0x08 item switch (64 bits), +0x20 Random Stage Switch Melee page, +0x24 its Brawl page.
constexpr u32 MENU_DATA = 0x9017BE50;
constexpr u32 MENU_ITEM_FREQUENCY = MENU_DATA + 0x00;
constexpr u32 MENU_ITEM_SWITCH_HI = MENU_DATA + 0x08;
constexpr u32 MENU_ITEM_SWITCH_LO = MENU_DATA + 0x0C;
constexpr u32 MENU_STAGES_MELEE = MENU_DATA + 0x20;
constexpr u32 MENU_STAGES_BRAWL = MENU_DATA + 0x24;

// Project+'s stage switch data (Net-Random.asm RSS_EXDATA) and strike table (STAGE_STRIKE_TABLE,
// 6 bytes per page, 5 pages).
constexpr u32 PPLUS_RSS = 0x8042C4E8;
constexpr u32 PPLUS_STRIKES = 0x8042C822;
constexpr u32 PPLUS_STRIKES_SIZE = 0x1E;

// The 2024 Proposed stage list, from Project+'s preset 3 (pf/stage/switch/Switch03.rss): random
// masks, hazards off and page layouts. Page 0 holds the nine legal stages, all in the random set.
constexpr std::array<u8, B::SAVED_RSS_SIZE> PPLUS_2024_RSS{
    0x00, 0x00, 0x00, 0x03, 0x01, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x1C, 0x05, 0x0B,
    0x23, 0x08, 0x1A, 0x00, 0x28, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1B, 0x01, 0x38, 0x24, 0x3B, 0x04, 0x02, 0x2D, 0x31, 0x20, 0x0C, 0x39,
    0x15, 0x06, 0x33, 0x07, 0x32, 0x21, 0x09, 0x1F, 0x3D, 0x2E, 0x27, 0x35, 0x0A, 0x18, 0x0D, 0x36,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x0F, 0x3C, 0x10,
    0x34, 0x25, 0x3E, 0x0E, 0x22, 0x19, 0x14, 0x12, 0x26, 0x11, 0x13, 0x17, 0x16, 0x37, 0x2F, 0x1D,
    0x3A, 0x2C, 0x1B, 0x1E, 0x30, 0x2B, 0x00, 0x00,
};

// Character select player areas and hands (see OnlineRules.h).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x04;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_HAND = 0x1A8;
// Area fields: +0x1B4 kind (1 human), +0x1B8 character under the hand or the token's, +0x1F8 u8 1
// while the token is in the hand, +0x1F9 u8 1 while it is moving.
constexpr u32 AREA_KIND = 0x1B4;
constexpr u32 AREA_KIND_HUMAN = 1;
constexpr u32 AREA_CHARACTER = 0x1B8;
constexpr u32 AREA_IN_HAND = 0x1F8;
constexpr u32 AREA_FLYING = 0x1F9;
constexpr u32 HAND_TARGET = 0x80;
constexpr u32 HAND_X = 0x90;
constexpr u32 HAND_Y = 0x94;
constexpr u32 HAND_BUTTON = 0xAC;
constexpr u32 HAND_PANEL = 0xB0;

constexpr const char* SCENE_CSS = "scSelctCharacter";
constexpr const char* SCENE_SSS = "scSelStage";
constexpr const char* SCENE_FIGHT = "scMelee";
constexpr const char* SCENE_BETWEEN = "scMemoryChange";
constexpr const char* SCENE_RESULTS = "scVsResult";

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

// Whether port `port` (0 or 1) held `button` on the previous frame, from the queue's raw-button
// latch (Queue.h RAW). Other ports always return false.
bool RawHeld(const GuestMemory& m, int port, u16 button)
{
  if (port < 0 || port > 1 || !m.Valid(Queue::RAW + 2 * static_cast<u32>(port) + 1))
    return false;
  return (m.Read16(Queue::RAW + 2 * static_cast<u32>(port)) & button) != 0;
}

u32 GlobalField(const GuestMemory& m, u32 field)
{
  if (!Pointer(m, GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + field))
    return 0;
  const u32 p = m.Read32(global + field);
  return Pointer(m, p) ? p : 0;
}

// Writes only what differs, counting the bytes changed.
struct Writer
{
  GuestMemory& m;
  int changed = 0;
  void U8(u32 a, u8 v)
  {
    if (!m.Valid(a) || m.Read8(a) == v)
      return;
    m.Write8(a, v);
    ++changed;
  }
  void U16(u32 a, u16 v)
  {
    U8(a, static_cast<u8>(v >> 8));
    U8(a + 1, static_cast<u8>(v));
  }
  void U32(u32 a, u32 v)
  {
    U16(a, static_cast<u16>(v >> 16));
    U16(a + 2, static_cast<u16>(v));
  }
  void Zero(u32 a, u32 n)
  {
    for (u32 i = 0; i < n; ++i)
      U8(a + i, 0);
  }
};

bool BlockMapped(const GuestMemory& m)
{
  return m.Valid(B::BASE) && m.Valid(B::BASE + B::SIZE - 1);
}

// Current picks from the match's init data.
std::array<PlayerPick, 4> Picks(const GuestMemory& m)
{
  std::array<PlayerPick, 4> out{};
  const u32 melee = GlobalField(m, GLOBAL_MODE_MELEE);
  if (!melee || !m.Valid(melee + MELEE_PLAYERS + 4 * MELEE_PLAYER_SIZE - 1))
    return out;
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 p = melee + MELEE_PLAYERS + i * MELEE_PLAYER_SIZE;
    out[i].human = m.Read8(p + 1) == PLAYER_HUMAN;
    const u8 character = m.Read8(p);
    if (out[i].human && character != CHARACTER_NONE)
      out[i].character = character;
  }
  return out;
}

bool AnyIceClimbers(const std::array<PlayerPick, 4>& ports)
{
  for (const PlayerPick& p : ports)
  {
    if (p.human && p.character == CHARACTER_ICE_CLIMBERS)
      return true;
  }
  return false;
}

void ForceRules(Writer& w, const Header& h, const Ranked::Ruleset& rules)
{
  const u32 rule = GlobalField(w.m, GLOBAL_SET_RULE);
  if (rule && w.m.Valid(rule + RULE_PAUSE))
  {
    w.U8(rule + RULE_MODE, static_cast<u8>((w.m.Read8(rule + RULE_MODE) & 0xF8) | 1));
    // A ranked set with a tiebreak game (Brawl) owns its stocks and time
    // (RankedSet::WriteSetRules), written earlier this frame; otherwise the ruleset's, every game.
    const bool set_rules = h.mode == Mode::Ranked && rules.SetWritesGameRules();
    if (!set_rules)
      w.U8(rule + RULE_STOCKS, rules.stocks);
    w.U8(rule + RULE_HANDICAP, 0);
    w.U8(rule + RULE_RATIO, 10);
    // Both modes use the stage select: casual's preferred-stage turns, ranked's strikes and picks.
    w.U8(rule + RULE_STAGE_CHOICE, STAGE_CHOOSE);
    if (!set_rules)
      w.U8(rule + RULE_STOCK_MINUTES, rules.minutes);
    w.U8(rule + RULE_TEAM_ATTACK, 1);
    w.U8(rule + RULE_PAUSE, 0);
  }
  // No items.
  w.U8(MENU_ITEM_FREQUENCY, 0);
  w.U32(MENU_ITEM_SWITCH_HI, 0);
  w.U32(MENU_ITEM_SWITCH_LO, 0);
}

void ForceStages(Writer& w, const Header& h, const std::string& scene)
{
  if (h.ruleset == Ruleset::Brawl)
  {
    // Follow picks on the character select and on the way to the fight (Random resolves as the
    // character select ends). The list is frozen during the fight.
    if (scene != SCENE_CSS && scene != SCENE_SSS && scene != SCENE_BETWEEN)
      return;
    const std::array<PlayerPick, 4> ports = Picks(w.m);
    const bool ice = AnyIceClimbers(ports);
    const u32 stages = h.mode == Mode::Ranked ? BRAWL_RANKED_BRAWL_PAGE :
                          BRAWL_LEGAL_BRAWL_PAGE & ~(ice ? BRAWL_FINAL_DESTINATION_BIT : 0);
    w.U32(MENU_STAGES_BRAWL, stages);
    w.U32(MENU_STAGES_MELEE, BRAWL_LEGAL_MELEE_PAGE);
    return;
  }
  if (h.ruleset == Ruleset::PPlus && scene == SCENE_CSS)
  {
    // Write before the stage select builds its pages from it.
    for (u32 i = 0; i < B::SAVED_RSS_SIZE; ++i)
      w.U8(PPLUS_RSS + i, PPLUS_2024_RSS[i]);
    // Casual reads strike marks as preferred stages, so clear any left over.
    if (h.mode == Mode::Casual)
      w.Zero(PPLUS_STRIKES, PPLUS_STRIKES_SIZE);
  }
}

// dnet.cmnu's default for every cheat and gameplay line (each line's value word, at line + 8), with
// Code Menu Activation OFF. Not here: the Character Select lines (they follow the fighters), and
// the replay, tag, costume, crowd and HUD colour lines.
constexpr std::array<CodeMenuWord, 73> CODE_MENU_DEFAULTS{{
    // Endless Friendlies mode and stages, Alternate Stages, Autoskip Results, Activation (OFF)
    {0x804E0910, 0},
    {0x804E098C, 0},
    {0x804E09E4, 0},
    {0x804E0A38, 0},
    {CODE_MENU_ACTIVATION, CODE_MENU_OFF},
    // Debug Mode, Hitbox and Collision Display, Stage Collisions, Camera Lock, Draw DI, FPS, HUD
    {0x804E0D3C, 0},
    {0x804E0D68, 0},
    {0x804E0DB8, 0},
    {0x804E0DEC, 0},
    {0x804E0E40, 0},
    {0x804E0E70, 0},
    {0x804E0E9C, 0},
    {0x804E0ECC, 1},
    // P1-P4: Infinite Shield, Select Percent (0.0), Press DPad to select percent, Disable DPad,
    // Input Buffer, Automatic L-Cancelling, ALC Modifier (0.5), Red Flash on L-Cancel Failure
    {0x804E0F08, 0},
    {0x804E1198, 0},
    {0x804E11D0, 0},
    {0x804E1210, 0},
    {0x804E1240, 0},
    {0x804E12B4, 0},
    {0x804E1308, 0x3F000000},
    {0x804E1340, 0},
    {0x804E13F0, 0},
    {0x804E145C, 0},
    {0x804E1494, 0},
    {0x804E14D4, 0},
    {0x804E1504, 0},
    {0x804E1534, 0},
    {0x804E156C, 0x3F000000},
    {0x804E15A4, 0},
    {0x804E1654, 0},
    {0x804E16C0, 0},
    {0x804E16F8, 0},
    {0x804E1738, 0},
    {0x804E1768, 0},
    {0x804E1798, 0},
    {0x804E17D0, 0x3F000000},
    {0x804E1808, 0},
    {0x804E18B8, 0},
    {0x804E1924, 0},
    {0x804E195C, 0},
    {0x804E199C, 0},
    {0x804E19CC, 0},
    {0x804E19FC, 0},
    {0x804E1A34, 0x3F000000},
    {0x804E1A6C, 0},
    // Random Angle, War and Big Head modes
    {0x804E1B8C, 0},
    {0x804E1BEC, 0},
    {0x804E1C18, 0},
    // Gameplay constants (floats): hitstun, hitlag, hitlag max, electric hitlag, SDI, ASDI,
    // walljump, shield size min, shield damage, base shield damage, shield size, shield tilt, wall
    // bounce, knockback decay
    {0x804E1E28, 0x3ECCCCCD},
    {0x804E1E64, 0x3EAAAAAB},
    {0x804E1EA0, 0x41F00000},
    {0x804E1ED8, 0x3FC00000},
    {0x804E1F1C, 0x40C00000},
    {0x804E1F50, 0x40400000},
    {0x804E1F88, 0x3F666666},
    {0x804E1FD0, 0x3E19999A},
    {0x804E2014, 0x3F800000},
    {0x804E2054, 0},
    {0x804E2090, 0x3F800000},
    {0x804E20D0, 0x3F000000},
    {0x804E2110, 0x3F4CCCCD},
    {0x804E2158, 0x3D50E560},
    // Staling, item grab toggle, jumpsquat mode, value, minimum and maximum
    {0x804E2194, 0},
    {0x804E21E0, 0},
    {0x804E2228, 0},
    {0x804E22D8, 3},
    {0x804E2318, 1},
    {0x804E2360, 0xE10},
    // Flight Mode, its speeds and accelerations
    {0x804E23BC, 0},
    {0x804E23EC, 0x40000000},
    {0x804E2428, 0x40000000},
    {0x804E24A0, 0x3F800000},
    {0x804E24E0, 0x3F800000},
}};
static_assert(std::ranges::all_of(CODE_MENU_DEFAULTS, [](const CodeMenuWord& word) {
  return word.address >= CODE_MENU && word.address % 4 == 0 && word.address + 4 <= CODE_MENU_END;
}));

// Project+'s Code Menu under a locking header: off, closed, every value at its default.
void ForceCodeMenu(Writer& w)
{
  if (!CodeMenuPresent(w.m))
    return;
  // Open: close it as its own Start does, giving the game its pause word back.
  if (w.m.Read32(CODE_MENU_STATE) == CODE_MENU_OPEN)
    w.U32(CODE_MENU_GAME_PAUSE, w.m.Read32(CODE_MENU_SAVED_PAUSE));
  // Every other state too: 1 opens without the activation check, 3 swaps characters.
  w.U32(CODE_MENU_STATE, 0);
  // One read per word: CodeMenuPresent checked the range is mapped.
  for (const CodeMenuWord& word : CODE_MENU_DEFAULTS)
  {
    if (w.m.Read32(word.address) != word.value)
      w.U32(word.address, word.value);
  }
}

// gmSelCharData, the character select's record: each port's slot at +0x0A + 4 x port, its
// character at OnlineSeats::RECORD_PLAYERS + port x RECORD_PLAYER_SIZE. Project+'s character select
// writes Giga Bowser or Wario-Man there as it ends, when the player holds shield (L; R is a costume
// button) on a Bowser or Wario pick. On the way to the fight the record's characters are copied
// into the init data again.
constexpr u32 RECORD_SLOTS = 0x0A;
constexpr u32 RECORD_SLOT_SIZE = 4;
constexpr u8 SLOT_BOWSER = 0x0C;
constexpr u8 SLOT_WARIO = 0x15;
constexpr u8 SLOT_GIGA_BOWSER = 0x38;
constexpr u8 SLOT_WARIO_MAN = 0x36;

// `v` with `from` replaced by `to`.
u8 Swap(u8 v, u8 from, u8 to)
{
  return v == from ? to : v;
}

// Project+'s banned fighters become Bowser and Wario, from the character select until the fight
// loads: in the match's init data and in the record the init data is copied from.
void ForceLegalFighters(Writer& w, const std::string& scene)
{
  if (scene != SCENE_CSS && scene != SCENE_SSS && scene != SCENE_BETWEEN)
    return;
  const auto legal = [](u8 c) {
    return Swap(Swap(c, CHARACTER_GIGA_BOWSER, CHARACTER_BOWSER), CHARACTER_WARIO_MAN,
                CHARACTER_WARIO);
  };
  if (const u32 melee = GlobalField(w.m, GLOBAL_MODE_MELEE);
      melee && w.m.Valid(melee + MELEE_PLAYERS + 4 * MELEE_PLAYER_SIZE - 1))
  {
    for (u32 i = 0; i < 4; ++i)
    {
      const u32 p = melee + MELEE_PLAYERS + i * MELEE_PLAYER_SIZE;
      w.U8(p, legal(w.m.Read8(p)));
    }
  }
  namespace S = OnlineSeats;
  if (const u32 record = GlobalField(w.m, S::GLOBAL_SEL_CHAR);
      record && w.m.Valid(record + S::RECORD_PLAYERS + 4 * S::RECORD_PLAYER_SIZE - 1))
  {
    for (u32 i = 0; i < 4; ++i)
    {
      const u32 p = record + S::RECORD_PLAYERS + i * S::RECORD_PLAYER_SIZE;
      w.U8(p, legal(w.m.Read8(p)));
      const u32 slot = record + RECORD_SLOTS + i * RECORD_SLOT_SIZE;
      w.U8(slot, Swap(Swap(w.m.Read8(slot), SLOT_GIGA_BOWSER, SLOT_BOWSER), SLOT_WARIO_MAN,
                      SLOT_WARIO));
    }
  }
}

// Ranked character select no-ready timer (MatchBlock CSS_TIMER, CSS_NO_SHOW). Elapsed time is the
// frame number minus CSS_TIMER_START, never an incremented counter, so rollback re-runs agree.
void ReadyTimer(Writer& w, const Header& h, const Ranked::Ruleset& rules, const std::string& scene,
                int frame)
{
  const u32 ready_frames = static_cast<u32>(rules.css_ready_seconds) * 60;
  const auto stop = [&] {
    w.U16(B::CSS_TIMER, 0);
    w.U32(B::CSS_TIMER_START, 0);
  };
  if (scene == SCENE_FIGHT)
    w.U8(B::FOUGHT, static_cast<u8>(w.m.Read8(B::FOUGHT) | 1));
  if (scene != SCENE_CSS)
  {
    stop();
    w.U8(B::CSS_NO_SHOW, 0);
    return;
  }
  if (h.mode != Mode::Ranked || (w.m.Read8(B::FOUGHT) & 1) || w.m.Read8(B::CSS_NO_SHOW) != 0)
    return;
  // Ports 1 and 2, the queue match's players, must both be plugged in.
  if ((w.m.Read8(B::PLUGGED) & 3) != 3)
  {
    stop();
    return;
  }
  const std::array<PlayerPick, 4> picks = Picks(w.m);
  int ready = 0, waiting = -1;
  for (int i = 0; i < 2; ++i)
  {
    if (picks[i].character >= 0)
      ++ready;
    else if (waiting < 0)
      waiting = i;
  }
  if (ready == 0 || waiting < 0)
  {
    stop();
    return;
  }
  const u32 now = static_cast<u32>(frame) + 1;
  u32 start = w.m.Read32(B::CSS_TIMER_START);
  // Not running, or a start from another frame numbering (in the future or far past): restart.
  if (start == 0 || start > now || now - start > ready_frames * 2)
  {
    start = now;
    w.U32(B::CSS_TIMER_START, start);
  }
  const u32 t = now - start;
  if (t >= ready_frames)
  {
    stop();
    w.U8(B::CSS_NO_SHOW, static_cast<u8>(waiting + 1));
    return;
  }
  w.U16(B::CSS_TIMER, static_cast<u16>(t));
}

// ---- Session state (CPU thread) ----
std::atomic<Mode> s_wanted{Mode::None};
std::atomic<u8> s_wanted_flags{0};
std::atomic<u64> s_generation{0};
std::atomic<bool> s_in_place{true};
std::atomic<bool> s_header_free{false};
// The joiner's room, checked against the host's header (ExpectHeader).
struct Expectation
{
  Mode mode = Mode::None;
  std::string code;
  u8 flags = 0;
  int known_games = -1;
};
std::mutex s_mutex;
// Room code for the wanted header. Guarded by s_mutex.
std::string s_wanted_code;
std::optional<Expectation> s_expect;
std::optional<std::string> s_mismatch;

// No-show byte per frame until that frame is confirmed (like Results.h's tracker).
struct NoShowTracker
{
  std::map<int, u8> pending;
  u64 resyncs = 0;
  bool started = false;
  u8 last = 0;
  int last_frame = -1;
};
NoShowTracker& NoShows()
{
  static NoShowTracker t;
  return t;
}
}  // namespace

Header ReadHeader(const GuestMemory& m)
{
  Header h;
  if (!BlockMapped(m) || m.Read32(B::MAGIC) != B::MAGIC_VALUE ||
      m.Read8(B::VERSION) != B::VERSION_VALUE)
  {
    return h;
  }
  h.present = true;
  const u8 mode = m.Read8(B::MODE);
  const u8 ruleset = m.Read8(B::RULESET);
  h.mode = mode <= 2 ? static_cast<Mode>(mode) : Mode::None;
  h.ruleset = ruleset <= 2 ? static_cast<Ruleset>(ruleset) : Ruleset::None;
  h.flags = m.Read8(B::FLAGS) & (B::FLAG_SOLO | B::FLAG_QUEUE2);
  h.coin = m.Read8(B::COIN);
  h.room = m.Read32(B::ROOM);
  return h;
}

Header HeaderFor(Mode mode, Ruleset ruleset, std::string_view code, u8 flags)
{
  if (mode == Mode::None || ruleset == Ruleset::None)
    return {};
  Header h;
  h.present = true;
  h.mode = mode;
  h.ruleset = ruleset;
  h.flags = flags & (B::FLAG_SOLO | B::FLAG_QUEUE2);
  // The queue's own character select has no room, so no coin and no hash.
  if ((h.flags & B::FLAG_SOLO) == 0)
  {
    h.coin = MatchBlock::CoinFromCode(code);
    h.room = MatchBlock::RoomHash(code);
  }
  return h;
}

bool HeaderIs(const Header& in_memory, const Header& wanted)
{
  if (!wanted.Locked())
    return !in_memory.Locked();
  return in_memory == wanted;
}

Ruleset ProfileRuleset()
{
  // The module's answer (BrawlGame.h): Brawl rev 2 plays Brawl's, Project+'s launcher on it
  // Project+'s, while their addresses hold (in mod mode, the builtin's guard words).
  return Builtin().ProfileRuleset();
}

std::array<PlayerPick, 4> ReadPicks(const GuestMemory& m)
{
  return Picks(m);
}

std::span<const CodeMenuWord> CodeMenuDefaults()
{
  return CODE_MENU_DEFAULTS;
}

bool CodeMenuPresent(const GuestMemory& m)
{
  if (!m.Valid(CODE_MENU) || !m.Valid(CODE_MENU_END - 1) || !Pointer(m, CODE_MENU_GAME_PAUSE))
    return false;
  // The page shown, within the file; the root page, as the menu itself checks; and two lines'
  // sizes and pointers (Code Menu Activation, Flight Mode).
  const u32 page = m.Read32(CODE_MENU);
  return page >= CODE_MENU_ROOT && page < CODE_MENU_END &&
         m.Read32(CODE_MENU + 4) == CODE_MENU_ROOT && m.Read16(0x804E0C30) == 0x0058 &&
         m.Read32(0x804E0C48) == 0x804E0C30 &&
         m.Read16(0x804E23B4) == 0x0030 && m.Read32(0x804E23CC) == 0x804E0F00;
}

namespace
{
// A header, for the log.
std::string Describe(const Header& h)
{
  if (!h.present)
    return "no header";
  return fmt::format("mode {} ruleset {} flags {:#04x} coin {} room {:08x}", static_cast<int>(h.mode),
                     static_cast<int>(h.ruleset), h.flags, h.coin, h.room);
}

// Resets every track's set state after the header: stage flow, ranked set and character order.
void FreshSet(Writer& w, Mode mode, Ruleset ruleset, u8 coin)
{
  w.Zero(B::SET, B::SET_SIZE);
  w.Zero(B::STEP, B::STEP_SIZE);
  w.Zero(B::CSS_TIMER, B::PHASE1_SIZE);
  w.Zero(B::CHAR_ORDER, B::CHAR_ORDER_SIZE);
  w.Zero(B::QUEUE, B::QUEUE_SIZE);
  w.Zero(B::STAGE_CURSORS_SHARED, B::STAGE_CURSORS_SHARED_SIZE);
  w.Zero(B::STAGE_CURSORS, B::STAGE_CURSORS_SIZE);
  w.changed += MatchBlock::Write(
      w.m, MatchBlock::State::Fresh(static_cast<u8>(mode), static_cast<u8>(ruleset), coin));
  w.changed += SetBlock::WriteSet(w.m, SetBlock::SetState{});
}
}  // namespace

int WriteHeader(GuestMemory& m, Mode mode, Ruleset ruleset, u8 coin, u32 room, u8 queue_flags)
{
  if (auto* replay = Orca::Net::ReplayScope::Current(); replay && !Orca::Net::ReplayScope::Playing())
    replay->header = {static_cast<u8>(mode), static_cast<u8>(ruleset), coin, queue_flags, room};
  if (!BlockMapped(m))
    return 0;
  Writer w{m};
  const Header now = ReadHeader(m);
  if (mode == Mode::None || ruleset == Ruleset::None)
  {
    // No Orca header (still the game's unused code, or zeros): leave it alone.
    if (!now.present)
      return 0;
    const u8 flags = m.Read8(B::SAVED_FLAGS);
    if (flags & 1)
    {
      if (const u32 rule = GlobalField(m, GLOBAL_SET_RULE); rule && m.Valid(rule + RULE_PAUSE))
      {
        for (u32 i = 0; i < RULE_SAVED_SIZE; ++i)
          w.U8(rule + RULE_SAVED_FROM + i, m.Read8(B::SAVED_RULES + i));
      }
      w.U32(MENU_ITEM_FREQUENCY, m.Read32(B::SAVED_ITEMS));
      w.U32(MENU_ITEM_SWITCH_HI, m.Read32(B::SAVED_ITEMS + 4));
      w.U32(MENU_ITEM_SWITCH_LO, m.Read32(B::SAVED_ITEMS + 8));
      w.U32(MENU_STAGES_MELEE, m.Read32(B::SAVED_STAGES));
      w.U32(MENU_STAGES_BRAWL, m.Read32(B::SAVED_STAGES + 4));
    }
    if ((flags & 2) && m.Valid(PPLUS_RSS + B::SAVED_RSS_SIZE - 1))
    {
      for (u32 i = 0; i < B::SAVED_RSS_SIZE; ++i)
        w.U8(PPLUS_RSS + i, m.Read8(B::SAVED_RSS + i));
    }
    // Project+'s Code Menu opens again as the player had it. Its values stay at their defaults.
    if ((flags & 4) && CodeMenuPresent(m) && m.Read8(B::SAVED_CODE_MENU) <= CODE_MENU_OFF)
      w.U32(CODE_MENU_ACTIVATION, m.Read8(B::SAVED_CODE_MENU));
    w.Zero(B::BASE, B::SIZE);
    // Also clear the character order and ranked set beyond the first 512 bytes.
    w.Zero(B::CHAR_ORDER, B::FULL_SIZE - (B::CHAR_ORDER - B::BASE));
    // Restore the original first word, which marks the block as free for a new header.
    w.U32(B::MAGIC, FreeSpace::kMatchBlockOriginalWord);
    return w.changed;
  }
  coin &= 1;
  queue_flags &= B::FLAG_SOLO | B::FLAG_QUEUE2;
  if (now.present && now.mode == mode && now.ruleset == ruleset && m.Read8(B::COIN) == coin &&
      m.Read32(B::ROOM) == room && m.Read8(B::FLAGS) == queue_flags)
  {
    return 0;
  }
  // Write only over sora_scene's unused code (checked by its first word) or an existing header.
  // Before the module loads, this memory belongs to something else.
  if (!now.present && m.Read32(B::MAGIC) != FreeSpace::kMatchBlockOriginalWord)
    return 0;
  if (!now.present)
  {
    // Fresh block: zero it, then save the player's own rules for restoring later.
    w.Zero(B::BASE, B::FULL_SIZE);
    u8 flags = 0;
    if (const u32 rule = GlobalField(m, GLOBAL_SET_RULE); rule && m.Valid(rule + RULE_PAUSE))
    {
      for (u32 i = 0; i < RULE_SAVED_SIZE; ++i)
        w.U8(B::SAVED_RULES + i, m.Read8(rule + RULE_SAVED_FROM + i));
      if (m.Valid(MENU_STAGES_BRAWL))
      {
        w.U32(B::SAVED_ITEMS, m.Read32(MENU_ITEM_FREQUENCY));
        w.U32(B::SAVED_ITEMS + 4, m.Read32(MENU_ITEM_SWITCH_HI));
        w.U32(B::SAVED_ITEMS + 8, m.Read32(MENU_ITEM_SWITCH_LO));
        w.U32(B::SAVED_STAGES, m.Read32(MENU_STAGES_MELEE));
        w.U32(B::SAVED_STAGES + 4, m.Read32(MENU_STAGES_BRAWL));
        flags |= 1;
      }
    }
    if (ruleset == Ruleset::PPlus && m.Valid(PPLUS_RSS + B::SAVED_RSS_SIZE - 1))
    {
      for (u32 i = 0; i < B::SAVED_RSS_SIZE; ++i)
        w.U8(B::SAVED_RSS + i, m.Read8(PPLUS_RSS + i));
      flags |= 2;
    }
    if (ruleset == Ruleset::PPlus && CodeMenuPresent(m) &&
        m.Read32(CODE_MENU_ACTIVATION) <= CODE_MENU_OFF)
    {
      w.U8(B::SAVED_CODE_MENU, static_cast<u8>(m.Read32(CODE_MENU_ACTIVATION)));
      flags |= 4;
    }
    w.U8(B::SAVED_FLAGS, flags);
  }
  // Write the magic last so a reader never sees a half-written header. Then start a fresh set.
  w.U8(B::VERSION, B::VERSION_VALUE);
  w.U8(B::MODE, static_cast<u8>(mode));
  w.U8(B::RULESET, static_cast<u8>(ruleset));
  w.U8(B::COIN, coin);
  w.U32(B::ROOM, room);
  w.U8(B::FLAGS, queue_flags);
  w.U32(B::MAGIC, B::MAGIC_VALUE);
  FreshSet(w, mode, ruleset, coin);
  return w.changed;
}

int ApplyLocks(GuestMemory& m, int frame)
{
  const Header h = ReadHeader(m);
  if (!h.Locked())
    return 0;
  // A locked header names a builtin ruleset (ReadHeader); none only in a build whose ruleset files
  // don't load (OrcaRulesets.BuiltinsAreTheOldTables), which then applies no locks.
  const Ranked::Ruleset* rules = h.ruleset == Ruleset::Brawl && h.mode == Mode::Casual ?
                                     &Rulesets::Brawl2025() :
                                     Rulesets::ByBlock(static_cast<u8>(h.ruleset));
  if (!rules)
    return 0;
  Writer w{m};
  const std::string scene = ReadSceneName(m);
  ForceRules(w, h, *rules);
  if (h.ruleset == Ruleset::PPlus)
  {
    ForceCodeMenu(w);
    ForceLegalFighters(w, scene);
  }
  ForceStages(w, h, scene);
  ReadyTimer(w, h, *rules, scene, frame);
  return w.changed;
}

Rollback::InputGate::Masks GateMasks(const GuestMemory& m)
{
  Rollback::InputGate::Masks masks{};
  const Header h = ReadHeader(m);
  if (!h.Locked())
    return masks;
  const std::string scene = ReadSceneName(m);
  // Project+'s Code Menu and debug codes open on L+R+Down: Down is dropped while both are held.
  if (h.ruleset == Ruleset::PPlus)
  {
    for (Rollback::InputGate::Mask& mask : masks)
      mask.drop_with_lr = PAD_BUTTON_DOWN;
  }
  u16 buttons = 0;
  if (scene == SCENE_CSS)
  {
    // Project+'s Code Menu opens from the character select with the D-pad.
    if (h.ruleset == Ruleset::PPlus)
      buttons |= PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;
    // Brawl's bottom row sits lower than Project+'s.
    const float a_bottom = h.ruleset == Ruleset::Brawl ? CSS_A_BOTTOM_BRAWL : CSS_A_BOTTOM;
    for (int port = 0; port < Rollback::InputGate::PORTS; ++port)
    {
      masks[port].buttons = buttons;
      // B only to pick the token back up; held B with the token in hand backs out to the menus.
      // On the queue's own character select B may back out (Queue.h handles a ready player).
      if (!h.Solo() && !CssBMayUnpick(m, port))
        masks[port].buttons |= PAD_BUTTON_B;
      // A only in the grid. On the queue's own character select, port 1 may also press BACK
      // (CssBackTakesA) and, while not searching, its own name button and list (CssNameTakesA),
      // each with the stick centred while A is down.
      const CssHand hand = ReadCssHand(m, port);
      const bool own_select = h.Solo() && port == 0;
      const bool list = own_select && ReadCssNameList(m, port);
      if (own_select && m.Valid(Queue::RAW + 1) &&
          CssBackTakesA(hand, RawHeld(m, port, PAD_BUTTON_A)))
      {
        masks[port].a_centres_stick = true;
      }
      else if (own_select && !(Queue::ReadState(m).ready & 1) && CssNameTakesA(hand, port, list))
      {
        masks[port].a_centres_stick = true;
        if (list)
          masks[port].buttons |= PAD_TRIGGER_L;
      }
      else if (!CssHandMayPressA(hand, a_bottom))
      {
        masks[port].buttons |= PAD_BUTTON_A;
      }
    }
    return masks;
  }
  else if (scene == SCENE_SSS)
  {
    // No way back to the character select once the stage is being picked.
    buttons = PAD_BUTTON_B;
    // Project+: L/R alternate stages, Z hazards, L+R+A back to the character select.
    if (h.ruleset == Ruleset::PPlus)
      buttons |= PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z;
  }
  else if (scene == SCENE_FIGHT)
  {
    // No pause. Project+ pauses on a held Start regardless of the rule.
    buttons = PAD_BUTTON_START;
  }
  else if (scene == SCENE_RESULTS && h.mode == Mode::Ranked)
  {
    // Ranked: after the set, holding B leaves the room (Rollback/OnlineMatch.cpp SetOverLeave).
    // Whatever B does on this screen natively, the hold must not move the other player's game.
    buttons = PAD_BUTTON_B;
  }
  for (Rollback::InputGate::Mask& mask : masks)
    mask.buttons = buttons;
  return masks;
}

Rollback::InputGate::Masks SessionMasks(const GuestMemory& m)
{
  Rollback::InputGate::Masks masks{};
  // Z on the results screen saves a replay, which a session cannot do (see OnlineRules.h).
  if (ReadSceneName(m) == SCENE_RESULTS)
  {
    for (Rollback::InputGate::Mask& mask : masks)
      mask.buttons = PAD_TRIGGER_Z;
  }
  return masks;
}

u32 ReadCssArea(const GuestMemory& m, int port)
{
  if (port < 0 || port >= 4 || ReadSceneName(m) != SCENE_CSS || !Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return 0;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return 0;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  const u32 area_at = task + TASK_AREAS + 4 * static_cast<u32>(port);
  return Pointer(m, area_at) ? m.Read32(area_at) : 0;
}

CssHand ReadCssHand(const GuestMemory& m, int port)
{
  CssHand hand;
  const u32 area = ReadCssArea(m, port);
  if (!area || !Pointer(m, area + AREA_HAND))
    return hand;
  const u32 p = m.Read32(area + AREA_HAND);
  if (!Pointer(m, p + HAND_TARGET) || !Pointer(m, p + HAND_X) || !Pointer(m, p + HAND_Y) ||
      !Pointer(m, p + HAND_BUTTON))
  {
    return hand;
  }
  hand.valid = true;
  hand.target = m.Read32(p + HAND_TARGET);
  hand.button = m.Read32(p + HAND_BUTTON);
  if (Pointer(m, p + HAND_PANEL))
  {
    const u32 panel = m.Read32(p + HAND_PANEL);
    hand.panel = panel < 4 ? static_cast<int>(panel) : -1;
  }
  hand.x = std::bit_cast<float>(m.Read32(p + HAND_X));
  hand.y = std::bit_cast<float>(m.Read32(p + HAND_Y));
  return hand;
}

bool CssHandMayPressA(const CssHand& hand, float bottom)
{
  if (!hand.valid)
    return false;
  switch (hand.target)
  {
  case CSS_HAND_GRID:
  case CSS_HAND_OWN_TOKEN:
  case CSS_HAND_TAKING:
  case CSS_HAND_GRID_HOLDING:
  case CSS_HAND_PLACING:
    // NaN fails both comparisons, so no A.
    return hand.y < CSS_A_TOP && hand.y > bottom;
  default:
    return false;
  }
}

bool ReadCssNameList(const GuestMemory& m, int port)
{
  const u32 area = ReadCssArea(m, port);
  return area && Pointer(m, area + CSS_AREA_NAME_LIST) && m.Read32(area + CSS_AREA_NAME_LIST) != 0;
}

CssToken ReadCssToken(const GuestMemory& m, int port)
{
  CssToken token;
  const u32 area = ReadCssArea(m, port);
  if (!area || !Pointer(m, area + AREA_KIND) || !Pointer(m, area + AREA_CHARACTER) ||
      !m.Valid(area + AREA_FLYING))
  {
    return token;
  }
  token.valid = true;
  token.human = m.Read32(area + AREA_KIND) == AREA_KIND_HUMAN;
  token.character = static_cast<int>(m.Read32(area + AREA_CHARACTER));
  token.in_hand = m.Read8(area + AREA_IN_HAND) != 0;
  token.flying = m.Read8(area + AREA_FLYING) != 0;
  return token;
}

bool CssBMayUnpick(const GuestMemory& m, int port)
{
  // The latch holds only ports 1 and 2; nobody else plays a queue match.
  if (port < 0 || port > 1 || !m.Valid(Queue::RAW + 2 * static_cast<u32>(port) + 1))
    return false;
  return CssBUnpicks(ReadCssToken(m, port), RawHeld(m, port, PAD_BUTTON_B));
}

bool SetWanted(Mode mode, u8 flags, std::string_view code)
{
  if (mode == Mode::None)
    flags = 0;
  // Only a room's header uses a code.
  if (mode == Mode::None || (flags & B::FLAG_SOLO) != 0)
    code = {};
  bool changed = s_wanted_flags.exchange(flags) != flags;
  changed = s_wanted.exchange(mode) != mode || changed;
  {
    std::lock_guard lock(s_mutex);
    if (s_wanted_code != code)
    {
      s_wanted_code = code;
      changed = true;
    }
  }
  if (changed)
  {
    ++s_generation;
    s_in_place = false;
  }
  return changed;
}

Mode Wanted()
{
  return s_wanted;
}

u8 WantedFlags()
{
  return s_wanted_flags;
}

u64 WantedGeneration()
{
  return s_generation;
}

bool HeaderInPlace()
{
  return s_in_place;
}

void MemoryReplaced()
{
  s_in_place = false;
  NoShows() = {};
}

void SetHeaderFree(bool free)
{
  s_header_free = free;
}

void ExpectHeader(Mode mode, std::string_view code, u8 flags, int known_games)
{
  std::lock_guard lock(s_mutex);
  s_expect = Expectation{mode, std::string(code), flags, known_games};
}

bool SetFitsJoin(const SetBlock::SetState& set, int known_games)
{
  if (known_games < 0)
    return true;
  if (set.games > known_games || set.wins[0] + set.wins[1] > known_games)
    return false;
  if (known_games > 0)
    return true;
  // None known: the set hasn't begun.
  return set.games == 0 && set.wins == std::array<u8, 2>{0, 0} && set.done == 0 &&
         set.winner == SetBlock::NO_PORT && set.last_winner == SetBlock::NO_PORT && !set.tiebreak &&
         set.tb_streak == 0 && std::ranges::all_of(set.records, [](const SetBlock::GameRecord& r) {
           return r == SetBlock::GameRecord{};
         });
}

bool HeaderFitsRoom(const Header& h, Mode mode, Ruleset ruleset, std::string_view code, u8 flags)
{
  // A room's header never has FLAG_SOLO.
  return HeaderIs(h, HeaderFor(mode, ruleset, code, static_cast<u8>(flags & ~B::FLAG_SOLO)));
}

std::optional<std::string> TakeMismatch()
{
  std::lock_guard lock(s_mutex);
  return std::exchange(s_mismatch, std::nullopt);
}

std::optional<TestQueue> ParseTestQueue(const std::string& v)
{
  std::vector<std::string> parts;
  for (size_t start = 0;;)
  {
    const size_t colon = v.find(':', start);
    parts.push_back(v.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
    if (colon == std::string::npos)
      break;
    start = colon + 1;
  }
  if (parts.empty() || (parts[0] != "casual" && parts[0] != "ranked") || parts.size() > 3)
    return std::nullopt;
  TestQueue out{parts[0] == "casual" ? Mode::Casual : Mode::Ranked, 0, 0};
  bool coin_seen = false;
  for (size_t i = 1; i < parts.size(); ++i)
  {
    if ((parts[i] == "0" || parts[i] == "1") && !coin_seen && out.flags == 0)
    {
      coin_seen = true;
      out.coin = static_cast<u8>(parts[i] == "1");
    }
    else if (parts[i] == "q2" && out.flags == 0)
    {
      out.flags = B::FLAG_QUEUE2;
    }
    else if (parts[i] == "solo" && out.flags == 0)
    {
      out.flags = B::FLAG_SOLO | B::FLAG_QUEUE2;
    }
    else
    {
      return std::nullopt;
    }
  }
  return out;
}

std::optional<TestQueue> TestQueueSpec()
{
  static const std::optional<TestQueue> spec = []() -> std::optional<TestQueue> {
    const std::string v = Orca::TestQueueSpec();
    const auto parsed = ParseTestQueue(v);
    if (!parsed && !v.empty())
      ERROR_LOG_FMT(ROLLBACK,
                    "Online rules: ORCA_TEST_QUEUE={}: not casual or ranked[:0|1][:q2|:solo]", v);
    return parsed;
  }();
  return TestKnobsAllowed() ? spec : std::nullopt;
}

std::optional<Mode> TestQueueMode()
{
  const auto spec = TestQueueSpec();
  return spec ? std::optional<Mode>(spec->mode) : std::nullopt;
}

std::optional<int> ConfirmNoShow(int confirmed)
{
  NoShowTracker& t = NoShows();
  std::optional<int> out;
  for (auto it = t.pending.begin(); it != t.pending.end() && it->first <= confirmed;
       it = t.pending.erase(it))
  {
    if (it->second != 0 && t.last == 0 && !out)
      out = it->second - 1;
    t.last = it->second;
    t.last_frame = it->first;
  }
  return out;
}

void OnFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, bool alone,
             u8 plugged)
{
  const Ruleset ruleset = ProfileRuleset();
  if (ruleset == Ruleset::None)
  {
    // No ruleset means no header, so keyframes never wait.
    if (!resimulating)
      s_in_place = true;
    return;
  }
  GuardMemory m(guard);
  if (Orca::Net::ReplayScope::Playing())
  {
    // Never another game's ruleset (Net::HeadersFitRuleset refuses such a replay first).
    if (const auto& header = Orca::Net::ReplayScope::Current()->header;
        header && (header->ruleset == 0 || static_cast<Ruleset>(header->ruleset) == ruleset))
    {
      WriteHeader(m, static_cast<Mode>(header->mode), static_cast<Ruleset>(header->ruleset),
                  header->coin, header->room, header->flags);
    }
  }
  // The room code (made by the server) decides game 1's first striker, so neither player chooses.
  Header wanted;
  {
    std::string code;
    {
      std::lock_guard lock(s_mutex);
      code = s_wanted_code;
    }
    wanted = HeaderFor(s_wanted, ruleset, code, s_wanted_flags);
  }
  // The test override's header depends only on the override and the synced ports, so it is safe to
  // write on re-runs too. A real host's header changes only on frames nobody else runs.
  const std::optional<TestQueue> test = TestQueueSpec();
  if (test)
  {
    u8 flags = test->flags;
    // ORCA_TEST_QUEUE=<mode>:solo with ORCA_TEST_QUEUE_PICK: port 2 plugging in stands for the
    // found match, so the header switches from solo to the room's, as when the app starts hosting.
    if ((flags & B::FLAG_SOLO) && (plugged & 2) && !Orca::TestQueuePickSpec().empty())
      flags = B::FLAG_QUEUE2;
    wanted = HeaderFor(test->mode, ruleset, {}, flags);
    wanted.coin = test->coin;
    wanted.room = 0;
  }
  if (!Orca::Net::ReplayScope::Playing() && (((alone || s_header_free) && !resimulating) || test))
  {
    if (const int changed =
            WriteHeader(m, wanted.mode, ruleset, wanted.coin, wanted.room, wanted.flags);
        changed > 0 && !resimulating)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online rules: header {}{} at frame {} ({} bytes{}), game 1's "
                     "first striker port {}",
                     wanted.mode == Mode::Casual ? "casual" :
                     wanted.mode == Mode::Ranked ? "ranked" :
                                                   "cleared",
                     wanted.mode == Mode::None            ? "" :
                     (wanted.flags & B::FLAG_SOLO) != 0   ? " (the queue's own character select)" :
                     (wanted.flags & B::FLAG_QUEUE2) != 0 ? " (queue2)" :
                                                            "",
                     frame, changed, alone ? "" : ", a friend on the way", wanted.coin + 1);
    }
  }
  // Whether a keyframe made this frame would carry the wanted header. This hook is the only writer.
  if (!resimulating)
    s_in_place = test.has_value() || HeaderIs(ReadHeader(m), wanted);
  // Synced ports in play, for the ready timer.
  if (ReadHeader(m).Locked() && m.Read8(B::PLUGGED) != plugged)
    m.Write8(B::PLUGGED, plugged);
  const int locked = ApplyLocks(m, frame);
  if (locked > 0 && !resimulating)
    DEBUG_LOG_FMT(ROLLBACK, "Online rules: {} bytes forced at frame {}", locked, frame);
  // Joiner's header check, once its keyframe has loaded.
  if (!resimulating)
  {
    std::lock_guard lock(s_mutex);
    if (s_expect)
    {
      const Header h = ReadHeader(m);
      const Expectation expect = std::move(*s_expect);
      s_expect.reset();
      // With ORCA_TEST_QUEUE on both sides, the host's header comes from the override.
      const bool ok =
          test ? h.Locked() && h.mode == test->mode && h.ruleset == ruleset :
                 HeaderFitsRoom(h, expect.mode, ruleset, expect.code, expect.flags);
      // A ranked room's set as the host's game holds it: never games this player didn't see.
      const SetBlock::SetState set = SetBlock::ReadSet(m);
      const bool set_ok =
          test || expect.mode != Mode::Ranked || SetFitsJoin(set, expect.known_games);
      if (ok && !set_ok)
      {
        WARN_LOG_FMT(ROLLBACK,
                     "Online rules: the host's game holds a set at {} games ({}-{}, done {}), this "
                     "player knows of {} in the room: mismatch",
                     set.games, set.wins[0], set.wins[1], set.done, expect.known_games);
        s_mismatch = "mismatch";
      }
      else if (!ok)
      {
        const std::string room =
            test ? std::string("the test knob's mode") :
                   Describe(HeaderFor(expect.mode, ruleset, expect.code, expect.flags));
        WARN_LOG_FMT(ROLLBACK,
                     "Online rules: the host's game says {}, this room wants {}{}: mismatch",
                     Describe(h), room,
                     h.Solo() ? " (the host's header is its own character select's)" : "");
        s_mismatch = "mismatch";
      }
      else
      {
        NOTICE_LOG_FMT(ROLLBACK, "Online rules: the host's header matches this room ({})",
                       Describe(h));
      }
    }
  }
  // Record this frame's no-show byte until it is confirmed. A re-run replaces the first run's.
  NoShowTracker& t = NoShows();
  const u64 resyncs = Orca::Events::Resyncs();
  if (!t.started || t.resyncs != resyncs)
  {
    t = {};
    t.started = true;
    t.resyncs = resyncs;
  }
  if (frame > t.last_frame)
  {
    const u8 value = ReadHeader(m).Locked() ? m.Read8(B::CSS_NO_SHOW) : 0;
    if (Orca::Net::ReplayScope::NetworkPlaying())
    {
      t.pending.clear();
      t.last = value;
      t.last_frame = frame;
    }
    else
      t.pending[frame] = value;
    while (t.pending.size() > 4096)
      t.pending.erase(t.pending.begin());
  }
}

}  // namespace Orca::UX::Rules
