// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/Queue.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/LocalStore.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Games/Brawl/CharOrder.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/QueueApp.h"

namespace Orca::UX::Queue
{
namespace
{
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;
using Rollback::InputGate::Masks;

// Brawl's character select (BrawlHeaders' muSelCharPlayerArea).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x04;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_KIND = 0x1B4;
constexpr u32 AREA_CHARACTER = 0x1B8;
constexpr u32 AREA_COSTUME = 0x1BC;  // m_charColorNo
constexpr u32 AREA_IN_HAND = 0x1F8;  // u8: 1 the token is in the hand, 0 down
constexpr u32 AREA_FLYING = 0x1F9;   // u8: 1 while the token flies
constexpr u32 KIND_HUMAN = 1;

constexpr u16 BUTTON_B = PAD_BUTTON_B;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
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
};

bool RegionMapped(const GuestMemory& m)
{
  return m.Valid(MatchBlock::QUEUE) && m.Valid(REGION_END - 1);
}

std::string Clock(int frames)
{
  const int seconds = (std::max(frames, 0) + 59) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}
}  // namespace

// ---- The host's own pick after a fresh start ----

OwnStep StepOwnPick(const CssPort& port, const Identity& pick, OwnSteer* st)
{
  OwnStep out;
  const int t = st->frames++;
  const float prev_x = st->has_prev ? st->prev_x : port.hand_x;
  const float prev_y = st->has_prev ? st->prev_y : port.hand_y;
  st->has_prev = true;
  st->prev_x = port.hand_x;
  st->prev_y = port.hand_y;
  if (!pick.HasPick() || t >= STEER_LIMIT_FRAMES)
  {
    out.done = true;
    return out;
  }
  // Press every fourth frame, released between, as the joiner's steering does.
  const bool edge = (t & 3) == 0;
  // The select's first frames: the hand may still be coming in.
  if (t < STEER_SETTLE_FRAMES)
    return out;
  if (port.placed)
  {
    if (port.character != pick.character)
    {
      st->costume_from = -1;
      out.buttons = edge ? BUTTON_B : 0;
      return out;
    }
    if (port.costume == pick.costume || pick.character == RANDOM_CHARACTER)
    {
      out.done = true;
      return out;
    }
    if (st->costume_from < 0)
      st->costume_from = t;
    if (t - st->costume_from >= COSTUME_LIMIT_FRAMES)
    {
      out.done = true;
      return out;
    }
    out.buttons = edge ? PAD_BUTTON_X : 0;
    return out;
  }
  if (port.hand_target == Rules::CSS_HAND_GRID_HOLDING && port.character == pick.character)
  {
    // Over the pick: A once the hand has nearly stopped (the character read lags the hand).
    out.buttons = edge && HandStill(port.hand_x, port.hand_y, prev_x, prev_y) ? PAD_BUTTON_A : 0;
    return out;
  }
  const auto [x, y] = SteerToward(port.hand_x, port.hand_y, pick.x, pick.y);
  out.stick_x = x;
  out.stick_y = y;
  return out;
}

namespace
{
struct OwnPickLocal
{
  bool armed = false;
  Identity pick;
  OwnSteer steer;
  // The first first-run frame seen while armed (-1: none yet).
  int first = -1;
  std::optional<GCPadStatus> pad;
};
OwnPickLocal& Own()
{
  static OwnPickLocal own;
  return own;
}

// First runs only: one frame of the host's own pick.
void SteerOwnPickFrame(const View& v, int frame)
{
  OwnPickLocal& o = Own();
  o.pad.reset();
  if (!o.armed)
    return;
  if (o.first < 0 || frame < o.first)
    o.first = frame;
  if (!(v.queue2 && !v.solo && v.css && v.game1 && v.ports[0].readable))
  {
    if (frame - o.first >= STEER_LIMIT_FRAMES)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port 1's own pick never met the room's character "
                               "select; the player picks by hand",
                     frame);
      o.armed = false;
    }
    return;
  }
  const OwnStep step = StepOwnPick(v.ports[0], o.pick, &o.steer);
  if (step.done)
  {
    const CssPort& one = v.ports[0];
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port 1's own pick is {} (character {:#x} costume {}, "
                             "{} frames)",
                   frame,
                   one.placed && one.character == o.pick.character ? "back in" :
                                                                     "left to the player",
                   one.character, one.costume, o.steer.frames);
    o.armed = false;
    return;
  }
  GCPadStatus pad;
  pad.button = step.buttons;
  pad.stickX = step.stick_x;
  pad.stickY = step.stick_y;
  pad.substickX = GCPadStatus::C_STICK_CENTER_X;
  pad.substickY = GCPadStatus::C_STICK_CENTER_Y;
  pad.triggerLeft = 0;
  pad.triggerRight = 0;
  pad.isConnected = true;
  o.pad = pad;
}
}  // namespace

void ArmOwnPick(const Identity& pick)
{
  OwnPickLocal& o = Own();
  o = {};
  o.armed = pick.HasPick();
  o.pick = pick;
  if (o.armed)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Queue: port 1's own pick (character {:#x} costume {}) goes back on "
                             "the room's character select",
                   pick.character, pick.costume);
  }
}

void DisarmOwnPick()
{
  Own() = {};
}

bool OwnPickArmed()
{
  return Own().armed;
}

std::optional<GCPadStatus> OwnPickPad()
{
  return Own().armed ? Own().pad : std::nullopt;
}

// ---- Where a pick's square is ----

std::optional<PickPlaces::Placed> PickPlaces::Frame(int port, const CssPort& css)
{
  if (port < 0 || port >= 2)
    return std::nullopt;
  Hover& hover = m_hover[port];
  if (!css.placed && css.hand_target == Rules::CSS_HAND_GRID_HOLDING &&
      css.character != NO_CHARACTER)
  {
    if (hover.character != css.character)
    {
      hover.character = css.character;
      hover.first_x = css.hand_x;
      hover.first_y = css.hand_y;
    }
    hover.last_x = css.hand_x;
    hover.last_y = css.hand_y;
  }
  std::optional<Placed> out;
  if (css.placed && !m_was_placed[port])
  {
    Placed p;
    p.character = css.character;
    p.costume = css.costume;
    p.learned = hover.character == css.character;
    p.last_x = hover.last_x;
    p.last_y = hover.last_y;
    p.first_x = hover.first_x;
    p.first_y = hover.first_y;
    if (p.learned)
    {
      m_places[css.character] = {(hover.first_x + hover.last_x) / 2,
                                 (hover.first_y + hover.last_y) / 2};
    }
    out = p;
  }
  m_was_placed[port] = css.placed;
  return out;
}

std::optional<std::pair<float, float>> PickPlaces::Find(int character) const
{
  const auto it = m_places.find(character);
  if (it == m_places.end())
    return std::nullopt;
  return it->second;
}

Identity Publishable(Identity pick, const std::optional<std::pair<float, float>>& place)
{
  if (!pick.HasPick())
    return pick;
  if (!place)
  {
    pick.character = -1;
    pick.x = pick.y = 0;
    return pick;
  }
  pick.x = place->first;
  pick.y = place->second;
  return pick;
}

// ---- Memory ----

bool CssTaskReadable(const GuestMemory& m)
{
  if (ReadSceneName(m) != "scSelctCharacter" || !Pointer(m, SCENE_MANAGER))
    return false;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return false;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return false;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  return Pointer(m, task + TASK_AREAS + 4);
}

View ReadView(const GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  View v;
  const Rules::Header h = Rules::ReadHeader(m);
  if (!h.Queue2() || !RegionMapped(m))
    return v;
  // The ruleset's queue timers. A queue header is locked, so it names a builtin ruleset; none only
  // in a build whose ruleset files don't load (OrcaRulesets.BuiltinsAreTheOldTables).
  const Ranked::Ruleset* rules = h.ruleset == Rules::Ruleset::Brawl ?
                                     &Rulesets::RulesFor(h.mode == Rules::Mode::Casual) :
                                     Rulesets::ByBlock(static_cast<u8>(h.ruleset));
  if (!rules)
    return v;
  v.ready_frames = static_cast<u16>(rules->queue_ready_seconds * 60);
  v.skip_frames = rules->queue_skip_frames;
  v.queue2 = true;
  v.solo = h.Solo();
  v.ranked = h.mode == Rules::Mode::Ranked;
  v.game1 = (m.Read8(MatchBlock::FOUGHT) & 1) == 0;
  v.plugged = m.Read8(MatchBlock::PLUGGED);
  for (int p = 0; p < 2; ++p)
  {
    v.raw[p] = m.Read16(RAW + 2 * static_cast<u32>(p));
    v.raw_prev[p] = m.Read16(RAW_PREV + 2 * static_cast<u32>(p));
  }
  v.hand2_prev_x = std::bit_cast<float>(m.Read32(HAND_PREV));
  v.hand2_prev_y = std::bit_cast<float>(m.Read32(HAND_PREV + 4));
  if (v.ranked && !v.game1)
  {
    const CharOrder::State order = CharOrder::ReadState(m);
    v.order_done = order.step != CharOrder::Step::First && order.step != CharOrder::Step::Second;
    v.order_locked = CharOrder::LockedPorts(CharOrder::ReadSet(m), CharOrder::ReadCss(m), order);
    v.order_locked_in = order.step == CharOrder::Step::Done && v.order_locked == 3;
  }
  for (const Events::PortInfo& info : ports)
  {
    if (info.port != 1 || info.queue.empty())
      continue;
    if (const auto id = DecodeIdentity(info.queue); id && id->HasPick())
      v.pick2 = id;
  }
  if (ReadSceneName(m) != "scSelctCharacter" || !Pointer(m, SCENE_MANAGER))
    return v;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return v;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return v;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  if (!Pointer(m, task + TASK_AREAS + 4))
    return v;
  v.css = true;
  for (u32 i = 0; i < 2; ++i)
  {
    const u32 area = m.Read32(task + TASK_AREAS + 4 * i);
    CssPort& p = v.ports[i];
    if (!Pointer(m, area + AREA_KIND) || !Pointer(m, area + AREA_CHARACTER) ||
        !Pointer(m, area + AREA_COSTUME) || !m.Valid(area + AREA_FLYING))
    {
      continue;
    }
    p.readable = true;
    p.human = m.Read32(area + AREA_KIND) == KIND_HUMAN;
    p.character = static_cast<int>(m.Read32(area + AREA_CHARACTER));
    p.costume = static_cast<int>(m.Read32(area + AREA_COSTUME));
    p.placed = p.human && p.character != NO_CHARACTER && m.Read8(area + AREA_IN_HAND) == 0 &&
               m.Read8(area + AREA_FLYING) == 0;
    p.name_list = Rules::ReadCssNameList(m, static_cast<int>(i));
    const Rules::CssHand hand = Rules::ReadCssHand(m, static_cast<int>(i));
    if (hand.valid)
    {
      p.hand_target = hand.target;
      p.hand_button = hand.button;
      p.hand_x = hand.x;
      p.hand_y = hand.y;
    }
  }
  return v;
}

State ReadState(const GuestMemory& m)
{
  State s;
  if (!RegionMapped(m))
    return s;
  s.ready = m.Read8(READY);
  s.flags = m.Read8(FLAGS);
  s.timeout_who = m.Read8(TIMEOUT_WHO);
  s.locked = m.Read8(LOCKED);
  s.timer_start = m.Read32(TIMER_START);
  s.timer = m.Read16(TIMER);
  s.steer = m.Read16(STEER);
  s.steer_start = m.Read32(STEER_START);
  s.costume_start = m.Read32(COSTUME_START);
  s.pick_set = m.Read8(PICK) == 1;
  s.pick_character = m.Read8(PICK + 1);
  s.pick_costume = m.Read8(PICK + 2);
  s.pick_x = std::bit_cast<float>(m.Read32(PICK + 4));
  s.pick_y = std::bit_cast<float>(m.Read32(PICK + 8));
  return s;
}

int WriteState(GuestMemory& m, const State& s)
{
  if (!RegionMapped(m))
    return 0;
  Writer w{m};
  w.U8(READY, s.ready);
  w.U8(FLAGS, s.flags);
  w.U8(TIMEOUT_WHO, s.timeout_who);
  w.U8(LOCKED, s.locked);
  w.U32(TIMER_START, s.timer_start);
  w.U16(TIMER, s.timer);
  w.U16(STEER, s.steer);
  w.U32(STEER_START, s.steer_start);
  w.U32(COSTUME_START, s.costume_start);
  w.U8(PICK, s.pick_set ? 1 : 0);
  w.U8(PICK + 1, s.pick_character);
  w.U8(PICK + 2, s.pick_costume);
  w.U8(PICK + 3, 0);
  w.U32(PICK + 4, std::bit_cast<u32>(s.pick_x));
  w.U32(PICK + 8, std::bit_cast<u32>(s.pick_y));
  return w.changed;
}

int Latch(GuestMemory& m, const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  // Runs under any header that locks the rules, since OnlineRules.h and CharOrder.h read RAW too,
  // and with the character order's test knob on its character select.
  if (!RegionMapped(m) ||
      !(Rules::ReadHeader(m).Locked() ||
        (CharOrder::TestActive() && MatchBlock::Present(m) && CharOrder::ReadCss(m).on_css)))
  {
    return 0;
  }
  Writer w{m};
  for (u32 p = 0; p < 2; ++p)
  {
    const u16 old = m.Read16(RAW + 2 * p);
    const u16 now =
        raw[p] && raw[p]->isConnected ? static_cast<u16>(raw[p]->button & Rollback::InputGate::ALL_BUTTONS) :
                                        0;
    w.U16(RAW_PREV + 2 * p, old);
    w.U16(RAW + 2 * p, now);
  }
  // Port 2's hand at frame start; the steering's A waits for it to be still.
  const Rules::CssHand hand = Rules::ReadCssHand(m, 1);
  w.U32(HAND_PREV, std::bit_cast<u32>(hand.valid ? hand.x : 0.0f));
  w.U32(HAND_PREV + 4, std::bit_cast<u32>(hand.valid ? hand.y : 0.0f));
  return w.changed;
}

// ---- The app's side and the local lines ----

namespace
{
// Local state updated on first runs only: what was printed, the skip hold, token positions.
struct Local
{
  bool printed_ready = false;
  Identity printed;
  SkipHold skip;
  // Each character's place on the select, learned from the hands (PickPlaces).
  PickPlaces places;
  // The one-time tip: loaded from the seen-file on first use; port 1's B held so far.
  bool tip_loaded = false;
  OnceTip tip;
  int tip_b_frames = 0;
};
Local& L()
{
  static Local l;
  return l;
}

std::string NameFor(const std::vector<Events::PortInfo>& ports, int port, int* rating)
{
  for (const Events::PortInfo& p : ports)
  {
    if (p.port != port)
      continue;
    if (rating)
    {
      if (const auto id = DecodeIdentity(p.queue))
        *rating = id->rating;
    }
    if (!p.name.empty())
      return p.name;
  }
  return fmt::format("Player {}", port + 1);
}

}  // namespace

Text TextFor(const View& v, const State& s, const std::vector<Events::PortInfo>& ports, int local,
             float skip_progress)
{
  Text out;
  std::string& line = out.line;
  std::string& note = out.note;
  std::array<std::string, 4>& labels = out.labels;
  std::array<bool, 4>& locked = out.locked;
  const std::string mode = v.ranked ? "Ranked" : "Casual";
  if (v.queue2 && v.css && v.solo)
  {
    if (s.ready & 1)
    {
      line = fmt::format("{} · Searching for an opponent…", mode);
      note = "B to cancel";
    }
    else
    {
      line = mode;
      note = "Pick your character, then press Start to search";
    }
  }
  else if (v.queue2 && v.css)
  {
    // Each player's name by their panel, with no rating. A locked-in player gets a LOCKED IN
    // badge.
    for (int p = 0; p < 2; ++p)
    {
      if (!(v.plugged & (1 << p)))
        continue;
      labels[p] = NameFor(ports, p, nullptr);
      locked[p] = ((s.ready | v.order_locked) >> p) & 1;
    }
    const int other = local == 0 ? 1 : 0;
    const std::string them = NameFor(ports, other, nullptr);
    if ((v.plugged & 3) != 3)
    {
      line = fmt::format("{} · Opponent found", mode);
      note = fmt::format("Waiting for {}…", them);
    }
    else if (s.flags & FLAG_TIMED_OUT)
    {
      line = "Time's up";
      note = (s.timeout_who >> local) & 1 ? "You didn't ready up" :
                                            fmt::format("{} didn't ready up", them);
    }
    else if (s.flags & FLAG_GO)
    {
      line = fmt::format("{} · Both locked in · Here we go", mode);
    }
    else if (ReadyPhase(v) && !OrderLockedIn(v))
    {
      const bool mine = (s.ready >> local) & 1;
      const bool theirs = (s.ready >> other) & 1;
      // In game 1 the picks are already in, so only Start. Later games pick with A, then Start.
      const bool picks = !v.game1;
      const std::string clock = Timed(v) ? " · " + Clock(v.ready_frames - s.timer) : std::string();
      if (mine && theirs)
        line = "Both locked in";
      else if (mine)
        line = fmt::format("You're locked in · {}: {}{}", them,
                           picks ? "pick and press Start" : "press Start", clock);
      else if (theirs && !picks)
        line = fmt::format("{} locked in · Press Start to lock in{}", them, clock);
      else if (theirs)
        line = fmt::format("{} locked in · Your pick: {}{}", them,
                           v.ports[local].placed ? "Start locks it in" :
                                                   "A on a character, then Start",
                           clock);
      else
        line = (picks ? (v.ports[local].placed ? "Start locks in your pick · B to change" :
                                                  "Pick with A, then press Start to lock in") :
                        "Press Start to lock in") +
               clock;
      note = mine && picks ? "B to change" : "";
      // Casual only. Ranked never offers the skip.
      if (MaySkip(v, s) && skip_progress > 0)
      {
        const int bars = std::clamp(static_cast<int>(skip_progress * 10), 0, 10);
        note = "Finding someone else " + std::string(static_cast<size_t>(bars), '|') +
               std::string(static_cast<size_t>(10 - bars), '.');
      }
      else if (MaySkip(v, s))
      {
        note += note.empty() ? "Hold B to leave" : " · Hold B to leave";
      }
    }
  }
  return out;
}

namespace
{
// The one-time tip, on a first run: `on_select` the queue's own select, alone. Whether it was seen,
// and keeping it as seen, go through the engine's store (Orca/LocalStore.h): no file I/O here.
void TipFrame(bool on_select, bool ready, bool b_held, int frame)
{
  Local& l = L();
  l.tip_b_frames = on_select && b_held ? l.tip_b_frames + 1 : 0;
  if (on_select && !l.tip_loaded)
  {
    l.tip_loaded = true;
    if (Orca::LocalStore::Seen(TIP_KEY))
    {
      l.tip.over = true;
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: the hold-B tip shows (not seen yet)", frame);
    }
  }
  const OnceTip::Step step = l.tip.Frame(on_select, ready, l.tip_b_frames);
  QueueApp::SetTipShown(step.show);
  if (!step.keep)
    return;
  if (!Orca::LocalStore::MarkSeen(TIP_KEY))
    WARN_LOG_FMT(ROLLBACK, "Queue: couldn't keep the hold-B tip as seen");
  else
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: the hold-B tip is kept as seen", frame);
}
}  // namespace

OnceTip::Step OnceTip::Frame(bool on_select, bool ready, int b_frames)
{
  Step step;
  if (over || !on_select)
    return step;
  // Readying (Start) or holding B ends it; holding B is the lesson learned, so it counts as seen.
  const bool held = b_frames >= TIP_HOLD_FRAMES;
  if (ready || held || shown >= TIP_FRAMES)
  {
    over = true;
    step.keep = held && !kept;
    kept = kept || step.keep;
    return step;
  }
  ++shown;
  step.show = true;
  step.keep = shown >= TIP_SEEN_FRAMES && !kept;
  kept = kept || step.keep;
  return step;
}

std::string Tip()
{
  return QueueApp::TipShown() ? std::string(TIP_TEXT) : std::string();
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory m(guard);
  // After a room that should not keep this player ready, clear ready at a solo frame.
  std::string cleared;
  const bool replay_clear = Orca::Net::ReplayScope::Playing() &&
                            Orca::Net::ReplayScope::Current()->clear_ready;
  if (replay_clear || (alone && !resimulating && QueueApp::TakeClearReady()))
  {
    if (auto* replay = Orca::Net::ReplayScope::Current(); replay && !Orca::Net::ReplayScope::Playing())
      replay->clear_ready = true;
    const Rules::Header h = Rules::ReadHeader(m);
    if (h.Solo() && RegionMapped(m))
    {
      // A Start still held from the room's last screen must be released before it counts.
      m.Write8(FLAGS, static_cast<u8>(m.Read8(FLAGS) | FLAG_SWALLOW_START));
      if (m.Read8(READY) & 1)
      {
        m.Write8(READY, static_cast<u8>(m.Read8(READY) & ~1));
        const std::string why = QueueApp::ClearReadyWhy();
        NOTICE_LOG_FMT(ROLLBACK, "Queue: not ready after the room ({})", why);
        cleared = "cleared after the room: " + why;
      }
    }
  }
  const View v = ReadView(m, ports);
  if (!resimulating)
  {
    QueueApp::SetOnQueueCss(v.queue2 && v.css);
    SteerOwnPickFrame(v, frame);
  }
  const State before = ReadState(m);
  const State after = Advance(v, before, frame);
  if (after != before)
  {
    WriteState(m, after);
    if (!resimulating)
    {
      if ((after.flags & FLAG_GO) && !(before.flags & FLAG_GO))
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: both ready, on to the stage", frame);
      if ((after.locked & 2) && !(before.locked & 2))
      {
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port 2's pick is in (character {:#x}, costume "
                       "{}, {} frames)",
                       frame, v.ports[1].character, v.ports[1].costume, after.steer);
      }
      if (!v.solo && after.ready != before.ready)
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: ready {:#x}", frame, after.ready);
    }
  }

  // Record the casual timeout for this frame until it is confirmed (QueueApp::NoteTimeout). A
  // re-run replaces the first run's value.
  QueueApp::NoteTimeout(frame, (after.flags & FLAG_TIMED_OUT) && v.queue2 && !v.solo ?
                                   static_cast<u8>(0x80 | (after.timeout_who & 3)) :
                                   0);

  if (resimulating)
    return;
  if (after.pick_set && !(after.locked & 2) && after.steer_start != 0 && after.steer % 10 == 0 &&
      after.steer <= STEER_LIMIT_FRAMES)
  {
    const Rollback::InputGate::Masks g = Gate(v, after);
    const CssPort& two = v.ports[1];
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: steering port 2 ({} frames): hand {} at {:.2f}, "
                   "{:.2f} (was {:.2f}, {:.2f}) over {:#x}{}, stick {} {}, press {:#x}",
                   frame, after.steer, two.hand_target, two.hand_x, two.hand_y, v.hand2_prev_x,
                   v.hand2_prev_y, two.character, two.placed ? " (down)" : "", g[1].steer_x,
                   g[1].steer_y, g[1].press);
  }
  int local = 0;
  for (const Events::PortInfo& p : ports)
  {
    if (!p.remote)
      local = p.port;
  }
  Local& l = L();
  for (int p = 0; p < 2 && v.queue2 && v.css; ++p)
  {
    const auto placed = l.places.Frame(p, v.ports[p]);
    if (!placed)
      continue;
    if (placed->learned)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port {} put its token on character {:#x} costume "
                     "{} (the hand held it at {:.2f}, {:.2f}; over it from {:.2f}, {:.2f})",
                     frame, p + 1, placed->character, placed->costume, placed->last_x,
                     placed->last_y, placed->first_x, placed->first_y);
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port {}'s token is on character {:#x} costume {} "
                     "(no hand over it, so no place learned)",
                     frame, p + 1, placed->character, placed->costume);
    }
  }
  const bool solo_css = v.queue2 && v.solo && v.css;
  // Only a solo frame can set this. A non-solo frame (a pending `host` or `join`) leaves it.
  if (!solo_css)
    QueueApp::SetOnSoloCss(false);
  else if (alone)
    QueueApp::SetOnSoloCss(true);
  if (solo_css && alone)
  {
    const CssPort& own = v.ports[0];
    const bool ready = after.ready & 1;
    QueueApp::SetSoloReady(ready);
    Identity pick;
    pick.character = own.placed ? own.character : -1;
    pick.costume = own.costume;
    if (ready && (!l.printed_ready || l.printed.character != pick.character ||
                  l.printed.costume != pick.costume))
    {
      // Only a pick whose place is known is put back by steering; otherwise the player picks by
      // hand in the room.
      const Identity published = Publishable(pick, l.places.Find(pick.character));
      QueueApp::SetPick(published);
      l.printed_ready = true;
      l.printed = pick;
      const std::string text = fmt::format("orca queue ready {} {} {}", v.ranked ? "ranked" : "casual",
                                           pick.character, pick.costume);
      if (published.HasPick())
      {
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: {} (the hand at {:.1f}, {:.1f})", frame, text,
                       published.x, published.y);
      }
      else
      {
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: {} (its place is unknown: picked by hand in the "
                       "room)",
                       frame, text);
      }
      Orca::Status::Line(text);
    }
    else if (!ready && l.printed_ready)
    {
      l.printed_ready = false;
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: orca queue unready ({})", frame,
                     cleared.empty() ? UnreadyReason(v, before, after) : cleared);
      Orca::Status::Line("orca queue unready");
    }
  }
  else if (v.queue2 && !v.solo)
  {
    // In a room. Ready is printed again once back on the player's own character select.
    l.printed_ready = false;
    QueueApp::SetSoloReady(false);
  }
  else if (!v.queue2)
  {
    l.printed_ready = false;
    QueueApp::SetSoloReady(false);
  }

  // Skip: B held 1.5 s on a casual room's character select, read from this player's own latched
  // input for the frame that just ran. Its first frame does what any B does there.
  const bool may_skip = MaySkip(v, after) && local >= 0 && local < 2;
  if (l.skip.Step(may_skip, may_skip && (v.raw[local] & BUTTON_B) != 0, v.skip_frames))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: B held {} frames: orca queue skip", frame,
                   l.skip.frames);
    Orca::Status::Line("orca queue skip");
  }
  TipFrame(solo_css && alone, after.ready & 1, v.raw[0] & BUTTON_B, frame);
  Text text = TextFor(v, after, ports, local, l.skip.Progress(v.skip_frames));
  SetQueueLines(std::move(text.line), std::move(text.note), std::move(text.labels), text.locked);
}

Masks GateFrame(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const Rules::Header h = Rules::ReadHeader(m);
  if (!h.Queue2())
    return {};
  // The gate reads memory only. Port 2's pick comes from the region's copy (PICK).
  const View v = ReadView(m, {});
  return Gate(v, ReadState(m));
}

bool OwnsStart(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const Rules::Header h = Rules::ReadHeader(m);
  return h.Queue2() && !h.Solo() && ReadSceneName(m) == "scSelctCharacter";
}

void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory m(guard);
  Latch(m, raw);
}
}  // namespace Orca::UX::Queue
