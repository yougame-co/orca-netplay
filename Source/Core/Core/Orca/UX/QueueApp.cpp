// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/QueueApp.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "Common/Logging/Log.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Search.h"

namespace Orca::UX::QueueApp
{
namespace
{
void Put32(std::vector<u8>& out, u32 v)
{
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

u32 Get32(const std::vector<u8>& b, size_t at)
{
  return u32(b[at]) << 24 | u32(b[at + 1]) << 16 | u32(b[at + 2]) << 8 | u32(b[at + 3]);
}

std::mutex s_mutex;
bool s_active = false;
bool s_ranked = false;
int s_rating = -1;
// This player's pick on the queue's own character select, and where it was placed.
Identity s_pick;
std::atomic<bool> s_solo_ready{false};
std::atomic<bool> s_on_solo_css{false};
std::atomic<bool> s_on_queue_css{false};
std::atomic<bool> s_clear_ready{false};
// For the log. Guarded by s_mutex.
std::string s_clear_ready_why;
std::atomic<bool> s_last_timeout_mine{false};
std::atomic<bool> s_tip_on{false};

// The casual timeout per frame, until that frame is confirmed (like OnlineRules' no-show).
struct TimeoutTracker
{
  std::map<int, u8> pending;  // bit 7: timed out; bits 0-1: who wasn't ready
  u64 resyncs = 0;
  bool started = false;
  u8 last = 0;
  int last_frame = -1;
};
TimeoutTracker& Timeouts()
{
  static TimeoutTracker t;
  return t;
}

void PublishIdentity()
{
  Identity id;
  {
    std::lock_guard lk(s_mutex);
    id = s_pick;
    id.rating = s_rating;
  }
  Orca::Events::SetOwnQueue(id.rating < 0 && !id.HasPick() ? std::vector<u8>{} :
                                                              EncodeIdentity(id));
}
}  // namespace

// ---- The queue identity ----

std::vector<u8> EncodeIdentity(const Identity& id)
{
  std::vector<u8> out;
  out.push_back(1);
  const u16 rating = id.rating < 0 || id.rating > 0xFFFE ? 0xFFFF : static_cast<u16>(id.rating);
  out.push_back(static_cast<u8>(rating >> 8));
  out.push_back(static_cast<u8>(rating));
  out.push_back(id.HasPick() && id.character <= 0xFE ? static_cast<u8>(id.character) : 0xFF);
  out.push_back(static_cast<u8>(std::clamp(id.costume, 0, 0xFF)));
  Put32(out, std::bit_cast<u32>(id.x));
  Put32(out, std::bit_cast<u32>(id.y));
  return out;
}

std::optional<Identity> DecodeIdentity(const std::vector<u8>& b)
{
  // Newer versions may append bytes; only the known ones are read.
  if (b.size() < 13 || b[0] != 1)
    return std::nullopt;
  Identity id;
  const u16 rating = static_cast<u16>(b[1] << 8 | b[2]);
  id.rating = rating == 0xFFFF ? -1 : rating;
  id.character = b[3] == 0xFF ? -1 : b[3];
  id.costume = b[4];
  id.x = std::bit_cast<float>(Get32(b, 5));
  id.y = std::bit_cast<float>(Get32(b, 9));
  // An out-of-range position is dropped, and the player picks by hand.
  if (!std::isfinite(id.x) || !std::isfinite(id.y) || std::fabs(id.x) > 100 ||
      std::fabs(id.y) > 100)
  {
    id.character = -1;
    id.x = id.y = 0;
  }
  return id;
}

void Begin(bool ranked)
{
  {
    std::lock_guard lk(s_mutex);
    s_active = true;
    s_ranked = ranked;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Queue: on the {} queue's own character select", ranked ? "ranked" :
                                                                                       "casual");
}

void End()
{
  {
    std::lock_guard lk(s_mutex);
    s_active = false;
    s_pick = {};
  }
  SetSoloReady(false);
  s_tip_on = false;
  PublishIdentity();
}

bool Active()
{
  std::lock_guard lk(s_mutex);
  return s_active;
}

bool Ranked()
{
  std::lock_guard lk(s_mutex);
  return s_ranked;
}

void SetOwnRating(int rating)
{
  {
    std::lock_guard lk(s_mutex);
    s_rating = rating;
  }
  PublishIdentity();
}

int OwnRating()
{
  std::lock_guard lk(s_mutex);
  return s_rating;
}

bool SoloReady()
{
  return s_solo_ready;
}

Identity OwnIdentity()
{
  std::lock_guard lk(s_mutex);
  Identity id = s_pick;
  id.rating = s_rating;
  return id;
}

bool OnSoloCss()
{
  return s_on_solo_css;
}

bool OnQueueCss()
{
  return s_on_queue_css;
}

void AfterRestore(bool keep_ready, const char* why)
{
  if (keep_ready)
    return;
  {
    std::lock_guard lk(s_mutex);
    s_clear_ready_why = why && *why ? why : "after the room";
  }
  s_clear_ready = true;
}

void ClearReady()
{
  s_clear_ready = true;
}

bool TakeTimeoutWasMine()
{
  return s_last_timeout_mine.exchange(false);
}

void ConfirmTimeout(int confirmed, int local_port)
{
  TimeoutTracker& t = Timeouts();
  for (auto it = t.pending.begin(); it != t.pending.end() && it->first <= confirmed;
       it = t.pending.erase(it))
  {
    if ((it->second & 0x80) && !(t.last & 0x80))
    {
      const bool mine = local_port >= 0 && local_port < 2 && ((it->second >> local_port) & 1);
      s_last_timeout_mine = mine;
      NOTICE_LOG_FMT(ROLLBACK, "Queue: the ready timer ran out at frame {}: orca queue timeout {}",
                     it->first, mine ? "me" : "them");
      Orca::Status::Line(mine ? "orca queue timeout me" : "orca queue timeout them");
    }
    t.last = it->second;
    t.last_frame = it->first;
  }
}

void ResetTimeouts()
{
  Timeouts() = {};
  s_last_timeout_mine = false;
}

void SetSoloReady(bool ready)
{
  if (s_solo_ready.exchange(ready) != ready)
    Search::SetWaiting(ready);
}

void SetOnSoloCss(bool on)
{
  // Returning from a room can keep the player ready and begin another search.
  if (s_on_solo_css.exchange(on) != on)
    Search::SetWaiting(on && s_solo_ready);
}

void SetOnQueueCss(bool on)
{
  s_on_queue_css = on;
}

void SetPick(const Identity& pick)
{
  {
    std::lock_guard lk(s_mutex);
    s_pick = pick;
  }
  PublishIdentity();
}

void SetTipShown(bool shown)
{
  s_tip_on = shown;
}

bool TipShown()
{
  return s_tip_on;
}

bool TakeClearReady()
{
  return s_clear_ready.exchange(false);
}

std::string ClearReadyWhy()
{
  std::lock_guard lk(s_mutex);
  return s_clear_ready_why;
}

void NoteTimeout(int frame, u8 value)
{
  TimeoutTracker& t = Timeouts();
  const u64 resyncs = Orca::Events::Resyncs();
  if (!t.started || t.resyncs != resyncs)
  {
    t = {};
    t.started = true;
    t.resyncs = resyncs;
  }
  if (frame > t.last_frame)
  {
    if (Orca::Net::ReplayScope::NetworkPlaying())
    {
      t.pending.clear();
      t.last = value;
      t.last_frame = frame;
      s_last_timeout_mine = false;
    }
    else
      t.pending[frame] = value;
    while (t.pending.size() > 4096)
      t.pending.erase(t.pending.begin());
  }
}
}  // namespace Orca::UX::QueueApp
