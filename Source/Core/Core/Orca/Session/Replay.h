// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <charconv>
#include <bit>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Session.h"

namespace Orca::Net
{
// Network joins contain only controller inputs and typed Orca UI events. They never contain
// Dolphin device state, RAM images, NAND files, host addresses or arbitrary memory writes.
constexpr size_t MAX_REPLAY_FRAMES = 60 * 60 * 60;

inline bool ValidReplayId(std::string_view id)
{
  if (!id.starts_with("replay-"))
    return false;
  id.remove_prefix(7);
  const auto dash = id.find('-');
  if (dash == std::string_view::npos || dash == 0 || dash > 6 || id.size() != dash + 9 ||
      (dash > 1 && id.front() == '0'))
    return false;
  unsigned frame = 0;
  const auto parsed = std::from_chars(id.data(), id.data() + dash, frame);
  if (parsed.ec != std::errc{} || parsed.ptr != id.data() + dash || frame > MAX_REPLAY_FRAMES)
    return false;
  for (const char c : id.substr(dash + 1))
  {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

// Only persisted recordings use seat labels. Live play and live join histories keep the synced
// names, so their full-RAM peer checksums and the game's displayed names agree.
inline std::vector<Events::PortInfo> SyncedPorts(const std::vector<Events::PortInfo>& ports)
{
  auto synced = ports;
  for (auto& port : synced)
    port.remote = false;
  return synced;
}

inline std::vector<Events::PortInfo> SeatLabelPorts(const std::vector<Events::PortInfo>& ports)
{
  auto labeled = ports;
  for (auto& port : labeled)
    port.name = "P" + std::to_string(port.port + 1);
  return labeled;
}

inline std::vector<Events::PortInfo> RecordingPorts(const std::vector<Events::PortInfo>& ports)
{
  auto recorded = SeatLabelPorts(ports);
  for (auto& port : recorded)
    port.remote = false;
  return recorded;
}

struct ReplayHeader
{
  u8 mode = 0;
  u8 ruleset = 0;
  u8 coin = 0;
  u8 flags = 0;
  u32 room = 0;
  bool operator==(const ReplayHeader&) const = default;
};

// The main menu exits a fresh start may record (UX/OnlineMenu.h FreshMove): With Friends, Casual,
// Ranked. Only ever on a replay's first frame.
constexpr u8 MENU_EXIT_FRIENDS = 25;
constexpr u8 MENU_EXIT_CASUAL = 30;
constexpr u8 MENU_EXIT_RANKED = 31;
inline bool ValidMenuExit(u32 exit)
{
  return exit == MENU_EXIT_FRIENDS || exit == MENU_EXIT_CASUAL || exit == MENU_EXIT_RANKED;
}

// Name-free tag assignment decisions: names can affect which existing slot is reused and
// whether a tag is created. Recording these decisions preserves slot and control choices
// when playback substitutes seat labels.
struct TagAssignment
{
  int port = 0;
  int slot = 0;
  bool created = false;
  bool controls = false;
  bool operator==(const TagAssignment&) const = default;
};

struct ReplayFrame
{
  Pads pads{};
  std::shared_ptr<const std::vector<Events::PortInfo>> ports;
  std::optional<ReplayHeader> header;
  // Room team 1's seat mask; zero clears it. Only complete two-versus-two maps are events.
  std::optional<u8> room_teams;
  bool clear_ready = false;
  std::optional<std::vector<TagAssignment>> tag_assignments;
  // A fresh start's way out of the built main menu (MENU_EXIT_*), or 0.
  u8 menu_exit = 0;
  // The seed this history's game draws its random numbers from (UX/OnlineMenu.h
  // FreshMove::ApplySeed), or 0 for the canonical boot's own. Only ever on a replay's first frame.
  u32 seed = 0;
};

inline bool ValidRoomTeams(u32 teams)
{
  return teams == 0 || (teams < 16 && std::popcount(teams) == 2);
}

struct ReplayArchive
{
  // Local verification state, deliberately absent from every wire/file codec.
  std::map<int, u64> checksums;
  u64 origin_hash = 0;
  u64 target_hash = 0;
  // The origin's frame: frames[i] is frame first_frame + i. -1 while there is no origin yet, so
  // nothing is recorded.
  int first_frame = -1;
  std::vector<ReplayFrame> frames;
  // The host's pre-frame hook has already run at the boundary being offered.
  ReplayFrame boundary;
  // The frame after the last one held.
  int EndFrame() const { return first_frame + static_cast<int>(frames.size()); }
  // The frame's entry, or null outside [first_frame, EndFrame()).
  ReplayFrame* At(int frame)
  {
    if (first_frame < 0 || frame < first_frame || frame >= EndFrame())
      return nullptr;
    return &frames[static_cast<size_t>(frame - first_frame)];
  }
  const ReplayFrame* At(int frame) const { return const_cast<ReplayArchive*>(this)->At(frame); }
};

// Whether every match-block header the replay writes (its frames' and its boundary's) is for
// `ruleset` (the running game's) or for none: a host's history never selects another game's rules,
// and so another game's memory writes, on this machine.
inline bool HeadersFitRuleset(const ReplayArchive& replay, u8 ruleset)
{
  const auto fits = [ruleset](const ReplayFrame& frame) {
    return !frame.header || frame.header->ruleset == 0 || frame.header->ruleset == ruleset;
  };
  return fits(replay.boundary) && std::all_of(replay.frames.begin(), replay.frames.end(), fits);
}

// CPU-thread-only context for the existing trusted UI handlers. Exogenous changes are recorded
// when first applied and replayed at exactly the same phase; resimulation retains those events.
class ReplayScope
{
public:
  ReplayScope(ReplayFrame* frame, bool playback, bool network = false, bool view_names = false,
              bool recorded = false)
      : m_previous(s_current), m_playback(s_playback), m_network(s_network),
        m_view_names(s_view_names), m_recorded(s_recorded)
  {
    s_current = frame;
    s_playback = playback;
    s_network = network;
    s_view_names = view_names;
    s_recorded = recorded;
  }
  ~ReplayScope()
  {
    s_current = m_previous;
    s_playback = m_playback;
    s_network = m_network;
    s_view_names = m_view_names;
    s_recorded = m_recorded;
  }
  static ReplayFrame* Current() { return s_current; }
  static bool Playing() { return s_current && s_playback; }
  static bool NetworkPlaying() { return Playing() && s_network; }
  static bool ViewNamesPlaying() { return Playing() && s_view_names; }
  static bool RecordedPlaying() { return Playing() && s_recorded; }

private:
  ReplayFrame* m_previous;
  bool m_playback;
  bool m_network;
  bool m_view_names;
  bool m_recorded;
  static inline thread_local ReplayFrame* s_current = nullptr;
  static inline thread_local bool s_playback = false;
  static inline thread_local bool s_network = false;
  static inline thread_local bool s_view_names = false;
  static inline thread_local bool s_recorded = false;
};

class ReplayRecordingScope
{
public:
  explicit ReplayRecordingScope(ReplayArchive* archive) : m_previous(s_archive) { s_archive = archive; }
  ~ReplayRecordingScope() { s_archive = m_previous; }
  // The entry for `frame`, grown as needed. Null with no archive, before its origin
  // (first_frame), or past MAX_REPLAY_FRAMES.
  static ReplayFrame* Frame(int frame)
  {
    if (!s_archive || s_archive->first_frame < 0 || frame < s_archive->first_frame ||
        static_cast<size_t>(frame) > MAX_REPLAY_FRAMES)
      return nullptr;
    const size_t index = static_cast<size_t>(frame - s_archive->first_frame);
    if (s_archive->frames.size() <= index)
      s_archive->frames.resize(index + 1);
    return &s_archive->frames[index];
  }
  static void RecordPads(int frame, const Pads& pads)
  {
    if (auto* target = Frame(frame))
      target->pads = pads;
  }

private:
  ReplayArchive* m_previous;
  static inline thread_local ReplayArchive* s_archive = nullptr;
};
}  // namespace Orca::Net
