// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/NativeText.h"

#include <atomic>
#include <string_view>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Games/Brawl/CharOrder.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/Relabel.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"

namespace Orca::UX::NativeText
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x4;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;  // scSelctCharacter's muSelCharTask*
constexpr u32 TASK_AREAS = 0x44;           // muSelCharPlayerArea*[4]
constexpr u32 TASK_RULES_BOX = 0x538;      // the rules bar's message buffer
constexpr u32 AREA_INDEX = 0x1B0;          // the area's own index (0-3)
constexpr u32 AREA_KIND = 0x1B4;           // 1: a human has joined
constexpr u32 AREA_PLATE_BOX = 0x174;      // the name plate's message buffer
constexpr u32 KIND_HUMAN = 1;
constexpr u32 MAX_CAPACITY = 0x1000;
constexpr u8 SETUP_FIRST = 0x17;
constexpr u8 CODE_WIDTH = 0x11;  // + 4 bytes
constexpr u8 CODE_COLOUR = 0x12;  // 0x02 0x0C r g b a
constexpr size_t PLATE_CHARS = 10;
// The opponent's seat while the queue searches, and the game's own text for an empty seat.
constexpr std::string_view SEARCHING = "SEARCHING";
constexpr std::string_view NONE_SEAT = "  NONE";  // exactly as the game formats it

std::atomic<u8> s_shown{0};

bool Pointer(const GuestMemory& m, u32 address)
{
  return address % 4 == 0 && m.Valid(address) && m.Valid(address + 3);
}

u32 Task(const GuestMemory& m)
{
  if (ReadSceneName(m) != "scSelctCharacter" || !Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return 0;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return 0;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  return Pointer(m, task) && Pointer(m, task + TASK_RULES_BOX) ? task : 0;
}

// Player area `port`, if it is the character select's and says it is that port's.
u32 Area(const GuestMemory& m, u32 task, int port)
{
  if (!task)
    return 0;
  const u32 area = m.Read32(task + TASK_AREAS + 4 * static_cast<u32>(port));
  if (!Pointer(m, area) || !Pointer(m, area + AREA_KIND) || !Pointer(m, area + AREA_PLATE_BOX) ||
      m.Read32(area + AREA_INDEX) != static_cast<u32>(port))
  {
    return 0;
  }
  return area;
}

const Events::PortInfo* PortOf(const std::vector<Events::PortInfo>& ports, int port)
{
  for (const Events::PortInfo& p : ports)
  {
    if (p.port == port)
      return &p;
  }
  return nullptr;
}

std::string NameOf(const std::vector<Events::PortInfo>& ports, int port)
{
  const Events::PortInfo* p = PortOf(ports, port);
  std::string name = p ? PlateName(p->name) : std::string();
  return name.empty() ? fmt::format("P{}", port + 1) : name;
}

// The ranked character-order turn, e.g. "ADA picks, A then Start  0:43". The timer is left out when
// the READY TO FIGHT band shows it. Empty when no order is active.
std::string OrderLine(const GuestMemory& m, const std::array<std::string, 2>& names,
                      const std::vector<Events::PortInfo>& ports, bool band_clock)
{
  const CharOrder::SetView set = CharOrder::ReadSet(m);
  const CharOrder::CssView css = CharOrder::ReadCss(m);
  const CharOrder::State state = CharOrder::ReadState(m);
  const int first = state.first & 1;
  const int second = 1 - first;
  const auto todo = [&css](int port) {
    return css.ports[port].Down() ? std::string("Start locks it in") : std::string("A then Start");
  };
  const std::string left =
      band_clock ? std::string() : "  " + Clock(set.pick_frames - state.elapsed);
  if (set.ruleset != CharOrder::Ruleset::None && css.on_css && state.game == set.game)
  {
    if (state.step == CharOrder::Step::First && !(set.order == CharOrder::Order::Free &&
                                                   (state.locked & (1 << second))))
    {
      return fmt::format("{} picks, {}{}", names[first], todo(first), left);
    }
    if (state.step == CharOrder::Step::Second)
      return fmt::format("{} picks, {}{}", names[second], todo(second), left);
  }
  // Otherwise use CharOrder's own line, with every port marked remote so it reads in third person.
  std::vector<Events::PortInfo> third = ports;
  for (Events::PortInfo& p : third)
  {
    p.remote = true;
    if (p.port >= 0 && p.port < 2)
      p.name = names[p.port];
  }
  return CharOrder::Line(set, css, state, third);
}

// The rules bar's line in a queue room. Identical on both machines, so never "you".
std::string RoomLine(const GuestMemory& m, const Queue::View& v, const Queue::State& s,
                     const std::vector<Events::PortInfo>& ports, std::optional<bool> band_clock_in)
{
  // When READY TO FIGHT! is relabelled, the timer is shown there instead.
  const bool band_clock = band_clock_in ? *band_clock_in : Relabel::ClockInBand(m, ports);
  const std::array<std::string, 2> names{NameOf(ports, 0), NameOf(ports, 1)};
  std::string prefix;
  if (v.ranked)
  {
    const SetBlock::SetState set = SetBlock::ReadSet(m);
    if (set.done == 1 && set.winner < 2)
    {
      return fmt::format("{} wins the set {}-{}", names[set.winner], set.wins[set.winner],
                         set.wins[1 - set.winner]);
    }
    const int game = set.wins[0] + set.wins[1] + 1;
    // Later games show the set score instead of the game number.
    prefix = game == 1 ? "Game 1: " :
                         fmt::format("{} {}-{} {}: ", names[0], set.wins[0], set.wins[1], names[1]);
  }
  if ((v.plugged & 3) != 3)
    return prefix + fmt::format("Opponent found, waiting for {}...", names[(v.plugged & 2) ? 0 : 1]);
  if (s.flags & Queue::FLAG_TIMED_OUT)
  {
    const int who = s.timeout_who & 3;
    return prefix + (who == 3 ? std::string("Time's up, nobody locked in") :
                                fmt::format("Time's up, {} didn't lock in", names[who == 2 ? 1 : 0]));
  }
  if (s.flags & Queue::FLAG_GO)
    return prefix + "Both locked in, here we go!";
  if (v.ranked && !v.game1 && !v.order_done)
  {
    if (const std::string order = OrderLine(m, names, ports, band_clock); !order.empty())
      return prefix + order;
  }
  if (Queue::ReadyPhase(v) && !Queue::OrderLockedIn(v))
  {
    const bool picks = !v.game1;
    const std::string clock = Queue::Timed(v) && !band_clock ?
                                  "  " + Clock(v.ready_frames - s.timer) :
                                  std::string();
    const int ready = s.ready & 3;
    if (ready == 3)
      return prefix + "Both locked in";
    if (ready)
    {
      const int in = ready == 1 ? 0 : 1;
      return prefix + fmt::format("{} locked in, {} to {}{}", names[in], names[1 - in],
                                  picks ? "pick, then Start" : "press Start", clock);
    }
    return prefix + (picks ? "Pick with A, then Start to lock in" : "Press Start to lock in") +
           clock;
  }
  return prefix + (v.ranked ? "Ranked" : "Casual");
}
}  // namespace

std::optional<Box> ReadBox(const GuestMemory& m, u32 object)
{
  if (!Pointer(m, object) || !Pointer(m, object + BUFFER_DATA) ||
      m.Read32(object) != MESSAGE_BUFFER_VTABLE)
  {
    return std::nullopt;
  }
  Box b;
  b.object = object;
  b.capacity = m.Read32(object + BUFFER_CAPACITY);
  b.length = m.Read32(object + BUFFER_LENGTH);
  b.buffer = m.Read32(object + BUFFER_DATA);
  if (b.capacity > MAX_CAPACITY || b.length < 8 || b.length >= b.capacity || !m.Valid(b.buffer) ||
      !m.Valid(b.buffer + b.capacity - 1) || m.Read8(b.buffer) != SETUP_FIRST ||
      m.Read8(b.buffer + b.length) != END)
  {
    return std::nullopt;
  }
  const auto at = [&](u32 i) { return m.Read8(b.buffer + i); };
  // The set-up ends with its colours: 0x0C rgba, 0x04 rgba, 0x03 0x00 0x00.
  u32 words = 0;
  for (u32 i = 5; i + 8 <= b.length; ++i)
  {
    if (at(i) == 0x04 && at(i - 5) == 0x0C && at(i + 5) == 0x03 && at(i + 6) == 0 && at(i + 7) == 0)
    {
      words = i + 8;
      break;
    }
  }
  if (!words)
    return std::nullopt;
  // Skip the message's leading width and colour codes.
  for (;;)
  {
    if (words + 5 <= b.length && at(words) == CODE_WIDTH)
      words += 5;
    else if (words + 7 <= b.length && at(words) == CODE_COLOUR && at(words + 1) == 0x02 &&
             at(words + 2) == 0x0C)
      words += 7;
    else
      break;
  }
  u32 end = words;
  while (end < b.length && at(end) >= 0x20)
    ++end;
  b.words = words;
  b.words_end = end;
  return b;
}

std::string Words(const GuestMemory& m, const Box& box)
{
  std::string out;
  for (u32 i = box.words; i < box.words_end; ++i)
    out.push_back(static_cast<char>(m.Read8(box.buffer + i)));
  return out;
}

int SetWords(GuestMemory& m, const Box& box, std::string_view text)
{
  std::vector<u8> next;
  next.reserve(box.length + text.size() + 1);
  for (u32 i = 0; i < box.words; ++i)
    next.push_back(m.Read8(box.buffer + i));
  for (char c : text)
    next.push_back(static_cast<u8>(c));
  for (u32 i = box.words_end; i < box.length; ++i)
    next.push_back(m.Read8(box.buffer + i));
  next.push_back(END);
  if (next.size() > box.capacity)
    return 0;
  const u32 length = static_cast<u32>(next.size() - 1);
  int written = 0;
  for (u32 i = 0; i < next.size(); ++i)
  {
    if (m.Read8(box.buffer + i) != next[i])
    {
      m.Write8(box.buffer + i, next[i]);
      written = 1;
    }
  }
  if (m.Read32(box.object + BUFFER_LENGTH) != length)
  {
    m.Write32(box.object + BUFFER_LENGTH, length);
    written = 1;
  }
  return written;
}

std::string Ascii(std::string_view text, size_t max)
{
  std::string out;
  for (size_t i = 0; i < text.size() && out.size() < max; ++i)
  {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c >= 0x20 && c < 0x7F)
    {
      out.push_back(static_cast<char>(c));
      continue;
    }
    // UTF-8 marks the overlay's lines use.
    const std::string_view rest = text.substr(i);
    if (rest.starts_with("·") || rest.starts_with("–") || rest.starts_with("—"))
    {
      out.push_back('-');
      i += rest.starts_with("·") ? 1 : 2;
    }
    else if (rest.starts_with("…"))
    {
      out.append("...", std::min<size_t>(3, max - out.size()));
      i += 2;
    }
  }
  return out;
}

std::string PlateName(std::string_view username)
{
  std::string out = Ascii(username, PLATE_CHARS);
  for (char& c : out)
  {
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
    else if (c == '_')
      c = '-';
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

std::string Clock(int frames)
{
  const int seconds = frames <= 0 ? 0 : (frames + 59) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

CssWords Wanted(const GuestMemory& m, const std::vector<Events::PortInfo>& ports,
                std::optional<bool> band_clock)
{
  CssWords w;
  const u32 task = Task(m);
  if (!task)
    return w;
  w.on_css = true;
  const Queue::View v = Queue::ReadView(m, ports);
  for (int port = 0; port < 4; ++port)
  {
    const u32 area = Area(m, task, port);
    const Events::PortInfo* p = PortOf(ports, port);
    if (!area || !p || m.Read32(area + AREA_KIND) != KIND_HUMAN)
      continue;
    // The name only, no rating.
    w.plates[port] = PlateName(p->name);
  }
  if (v.queue2 && v.css)
  {
    const Queue::State s = Queue::ReadState(m);
    const std::string mode = v.ranked ? "Ranked" : "Casual";
    if (v.solo)
    {
      w.rules = (s.ready & 1) ? mode + ": searching for an opponent...  B to cancel" :
                                mode + ": pick, then press Start to search";
      // While searching, the opponent's empty seat says SEARCHING. Apply restores NONE after.
      if (const u32 area = Area(m, task, 1);
          (s.ready & 1) && area && m.Read32(area + AREA_KIND) != KIND_HUMAN)
      {
        w.plates[1] = std::string(SEARCHING);
      }
    }
    else
    {
      w.rules = Ascii(RoomLine(m, v, s, ports, band_clock));
    }
  }
  return w;
}

CssBoxes FindCssBoxes(const GuestMemory& m)
{
  CssBoxes boxes;
  const u32 task = Task(m);
  if (!task)
    return boxes;
  boxes.rules = ReadBox(m, m.Read32(task + TASK_RULES_BOX));
  for (int port = 0; port < 4; ++port)
  {
    if (const u32 area = Area(m, task, port))
      boxes.plates[port] = ReadBox(m, m.Read32(area + AREA_PLATE_BOX));
  }
  return boxes;
}

int Apply(GuestMemory& m, const std::vector<Events::PortInfo>& ports, u8* shown, bool rules_bar,
          std::optional<bool> band_clock)
{
  u8 now = 0;
  int written = 0;
  const CssWords w = Wanted(m, ports, band_clock);
  if (w.on_css)
  {
    const CssBoxes boxes = FindCssBoxes(m);
    for (int port = 0; port < 4; ++port)
    {
      // Restore NONE on the opponent's seat once the search stops.
      if (port == 1 && w.plates[port].empty() && boxes.plates[port] &&
          Words(m, *boxes.plates[port]) == SEARCHING)
      {
        written += SetWords(m, *boxes.plates[port], NONE_SEAT);
      }
      if (w.plates[port].empty() || !boxes.plates[port])
        continue;
      written += SetWords(m, *boxes.plates[port], w.plates[port]);
      if (const auto box = ReadBox(m, boxes.plates[port]->object);
          box && Words(m, *box) == w.plates[port])
      {
        now |= static_cast<u8>(1 << port);
      }
    }
    if (rules_bar && !w.rules.empty() && boxes.rules)
    {
      written += SetWords(m, *boxes.rules, w.rules);
      if (const auto box = ReadBox(m, boxes.rules->object); box && Words(m, *box) == w.rules)
        now |= SHOWN_RULES;
    }
  }
  if (shown)
    *shown = now;
  return written;
}

void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Events::PortInfo>& ports, std::optional<bool> band_clock)
{
  // Brawl rev 2 gets plates and the rules bar. Project+ uses the same boxes but only gets plates:
  // its rules bar has no width code, so a longer line would not be drawn.
  const Rules::Ruleset ruleset = Rules::ProfileRuleset();
  if (ruleset != Rules::Ruleset::Brawl && ruleset != Rules::Ruleset::PPlus)
  {
    s_shown.store(0, std::memory_order_relaxed);
    return;
  }
  GuardMemory memory(guard);
  u8 shown = 0;
  const int written = Apply(memory, ports, &shown, ruleset == Rules::Ruleset::Brawl, band_clock);
  if (resimulating)
    return;
  const u8 before = s_shown.exchange(shown, std::memory_order_relaxed);
  if (written && shown != before)
  {
    const CssWords w = Wanted(memory, ports, band_clock);
    NOTICE_LOG_FMT(ROLLBACK, "Orca: native text: plates '{}' '{}', rules bar '{}' (shown {:02x})",
                   w.plates[0], w.plates[1],
                   ruleset == Rules::Ruleset::Brawl ? w.rules : std::string("the game's"), shown);
  }
}

u8 Shown()
{
  return s_shown.load(std::memory_order_relaxed);
}
}  // namespace Orca::UX::NativeText
