// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Session.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <utility>

#include <fmt/format.h>

namespace Orca::Net
{
namespace
{
// Input delay, Slippi-style: two frames (Config::input_delay), and rollback hides the rest of the
// link, spikes included. Delay costs every input all the time, while a rollback or stall costs only
// the frames it touches, so the adaptive delay covers only what rollback can't and a little delay
// cures. It rises for two reasons:
//
// - The typical link (LatencyDelay): the median of this player's recent relay round trips and the
//   slowest peer's reported median. A median ignores spikes in up to half the window but follows a
//   link that really got slower. Never a maximum or a mean.
// - Stalls the others had on this player's inputs. Each side reports how many of its stalls ended
//   within 1..4 frames (Packet::spared), i.e. how many a little more delay would have spared.
//   A raise is taken only when it would have spared stalls in RAISE_SPARED windows per frame: one
//   frame on every input is costly, so random spikes and long dropouts don't raise it. A player
//   raises its own delay only on others' stalls, and only from a player whose lag on its inputs
//   reached max_rollback. Stalls don't count when they coincide with a hitch on either machine, are
//   caused by the other side's own stall, happen while catching up or within SETTLE_SECONDS of a
//   plug-in, or while waiting for a peer's first packet.
//
// Rollbacks never move the delay; that's rollback doing its job.
//
// The delay comes back down a frame per window: a link-driven raise once the link has wanted less
// for LINK_HOLD_WINDOWS, a stall-driven one after STALL_HOLD_WINDOWS quiet windows. A step down
// also needs two frames of room, judged from how far the others ran past this player's inputs
// ("reach") over the last ROOM_WINDOWS. A raise soon after a step down undoes it and doubles the
// quiet run the next try needs. A delay the player chose (Config::fixed_delay) never adapts.
constexpr int MIN_RTT_SAMPLES = 3;
constexpr int LINK_HOLD_WINDOWS = 3;
constexpr int EDGE_WINDOWS = 3;
constexpr int RAISE_SPARED = 8;
constexpr int ROOM_WINDOWS = 5;
constexpr int LONE_WINDOWS = 1;
constexpr int STALL_HOLD_WINDOWS = ROOM_WINDOWS;
constexpr int MAX_STALL_HOLD_WINDOWS = 8 * STALL_HOLD_WINDOWS;
constexpr int PROBATION_WINDOWS = 60;
// A window with nothing to say (Session::m_need_history).
constexpr int NO_NEED = INT_MIN / 4;
// A stall measured this much over k frames still counts as within k (the stalled boundary is polled
// about every millisecond).
constexpr double SPARED_SLACK_MS = 2.0;
// Settling time after a plug-in (a match start or a drop-in): the game is loading, or a joiner that
// just caught up runs behind until time sync evens it out. No stall counts until then.
constexpr int SETTLE_SECONDS = 10;
// What the link needs: an input arrives one way after sampling and runs at the next boundary (up to
// a frame later); one more frame is kept free for jitter.
constexpr int LATENCY_SLACK_FRAMES = 2;
// Hysteresis: the link raises the delay only once its one-way time is this far past a frame
// boundary, and lowers it only once this far short, so a median wandering by a ms doesn't flip it.
constexpr double LINK_DEAD_BAND_MS = 4.0;
// A stall that began within this many frames after a hitch on any machine (or overlapped one) is
// the hitch's fault, not the link's.
constexpr int HITCH_FRAMES = 30;
// No working relay is ten seconds away.
constexpr int MAX_RTT_MS = 10000;
// Time sync averages this many frames, waits at most once per that many, and only when it is more
// than this many frames ahead on average; anything less is jitter.
constexpr int TIME_SYNC_FRAMES = 30;
constexpr double TIME_SYNC_LEAD = 1.0;
// Input lateness. An honest packet carries its sender's inputs up to its own frame plus its delay
// less one, whatever the link: both come from the same packet. A player whose inputs keep coming
// after its own frame, by more than LATE_SLACK frames on average for LATE_WINDOWS windows of
// LATE_WINDOW_FRAMES in a row, holds them back (a lag switch: this machine would run on guesses up
// to the rollback limit every frame). Such a player gets LATE_STALL_LIMIT frames of guesses rather
// than max_rollback, until it comes on time for LATE_WINDOWS windows again. Two-player sessions
// only: with a third player catching up, a sender's packets carry that player's oldest inputs
// first.
constexpr int LATE_WINDOW_FRAMES = 60;
constexpr double LATE_SLACK = 2.0;
constexpr int LATE_WINDOWS = 3;
constexpr int LATE_STALL_LIMIT = 3;
// Snapshot spacing: one more predicted frame between snapshots per this many ms a save costs (about
// what a non-rendered re-run frame costs on a slow PC), with hysteresis around each step.
constexpr double SAVE_MS_PER_SPACING = 3.0;
constexpr double SPACING_DEAD_BAND_MS = 0.25;
// Drop-in. A joiner counts as caught up within this many frames (plus the round trip) of the host.
// History goes out in bounded chunks (frames per packet, runs of equal pads, and distance past the
// joiner's reported frame) so a packet stays a few KB; an unacknowledged send is repeated after
// this many packets.
constexpr int CATCH_UP_SLACK = 8;
constexpr int MAX_HISTORY_RUNS = 32;
constexpr int MULTI_PEER_HISTORY_FRAMES = 128;
constexpr int HISTORY_AHEAD = 4096;
constexpr int HISTORY_RESEND_PACKETS = 120;
}  // namespace

double Game::NowMs() const
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Session::Session(const Config& config, Game& game, Transport& transport)
    : m_config(config), m_game(game), m_transport(transport), m_dirty(INT_MAX),
      m_delay(config.input_delay), m_hold_windows(STALL_HOLD_WINDOWS)
{
  m_need_history.fill(NO_NEED);
  for (auto& ack : m_peer_ack)
    ack.fill(-1);
  m_config.max_delay = std::max(m_config.max_delay, m_config.input_delay);
  m_config.frame_rate = std::max(m_config.frame_rate, 1);
  m_config.snapshot_every = std::clamp(m_config.snapshot_every, 0, MAX_SNAPSHOT_EVERY);
  if (m_config.fixed_delay)
  {
    m_fixed_delay = std::clamp(*m_config.fixed_delay, std::min(1, m_config.max_delay),
                               m_config.max_delay);
    m_delay = *m_fixed_delay;
  }
  m_stats.delay = m_delay;
  m_stats.delay_fixed = m_fixed_delay.has_value();
  ChooseSnapshotEvery();
  m_config.seats = std::clamp(m_config.seats, 1, MAX_SEATS);
  if (m_config.plan)
  {
    m_plan = *m_config.plan;
    if (m_config.local_seat >= 0 && m_config.local_seat < MAX_SEATS)
      m_own_live_from = m_plan[m_config.local_seat].live_from;
  }
  else
  {
    for (int s = 0; s < m_config.seats; ++s)
      m_plan[s] = {0, NEVER, 0};
  }
  m_frame = std::max(m_config.start_frame, 0);
  m_log_base = m_frame;
  m_window_start = m_frame;
  m_sync_start = m_frame;
  m_last_checksum_frame = m_frame - m_frame % m_config.checksum_every;
  // Frames before the input delay have no sampled input: both sides treat them as neutral (or as
  // the host's held pad when a friend just arrived). Sending them explicitly lets the other side
  // confirm them like any other frame.
  if (Plugged(m_config.local_seat, m_frame))
  {
    for (int f = m_frame; f < m_frame + m_delay; ++f)
      m_local[f] = m_config.start_pad;
  }
  for (int s = 0; s < m_config.seats; ++s)
    AdvanceReceived(s);
}

const Pad* Session::DepartedPad(int seat, int frame) const
{
  const auto old = m_departed.find(seat);
  if (old == m_departed.end() || frame < old->second.from || frame >= old->second.until)
    return nullptr;
  const auto& prefix = old->second;
  if (frame >= prefix.first_frame && frame - prefix.first_frame < static_cast<int>(prefix.pads.size()))
    return &prefix.pads[frame - prefix.first_frame];
  return frame >= prefix.first_frame && !prefix.pads.empty() ? &prefix.pads.back() : &UNPLUGGED_PAD;
}

bool Session::Plugged(int seat, int frame) const
{
  if (seat < 0 || seat >= m_config.seats)
    return false;
  if (frame >= m_log_base && frame < LogEnd())
    return !IsUnplugged(m_log[frame - m_log_base][seat]);
  if (const Pad* pad = DepartedPad(seat, frame))
    return !IsUnplugged(*pad);
  return frame >= m_plan[seat].plug_from && frame < m_plan[seat].unplug_from;
}

bool Session::RerunFrom(int frame)
{
  if (frame >= m_frame)
    return true;  // nothing ran yet
  // As a rollback to `frame` would: the newest snapshot at or before it, within the window.
  // Confirmed frames have none (only predicted and checksum frames are saved).
  const auto after = m_saved.upper_bound(frame);
  const int from = after == m_saved.begin() ? -1 : *std::prev(after);
  if (m_frame - frame > m_config.max_rollback || from < 0 ||
      m_frame - from > m_config.max_rollback + MAX_SNAPSHOT_EVERY - 1)
  {
    return false;
  }
  m_dirty = std::min(m_dirty, frame);
  return true;
}

int Session::StallLimit() const
{
  // How far past the slowest player's inputs this player may run.
  if (CatchingUp())
    return 0;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat && m_late[s] && InPlay(s))
      return std::min(m_config.max_rollback, LATE_STALL_LIMIT);
  }
  return m_config.max_rollback;
}

bool Session::LateSeat(int seat) const
{
  return seat >= 0 && seat < MAX_SEATS && m_late[seat];
}

void Session::ResetLateness(int seat)
{
  m_late[seat] = false;
  m_late_sum[seat] = 0;
  m_late_samples[seat] = 0;
  m_late_windows[seat] = 0;
  m_clean_windows[seat] = 0;
}

void Session::UpdateLateness()
{
  if (m_frame - m_late_window_start < LATE_WINDOW_FRAMES)
    return;
  m_late_window_start = m_frame;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat)
      continue;
    if (m_late_samples[s] >= LATE_WINDOW_FRAMES / 4)
    {
      const double average = static_cast<double>(m_late_sum[s]) / m_late_samples[s];
      if (average > LATE_SLACK)
      {
        m_clean_windows[s] = 0;
        if (++m_late_windows[s] >= LATE_WINDOWS && !m_late[s])
        {
          m_late[s] = true;
          ++m_stats.late_seats;
          m_delay_notes.push_back(fmt::format(
              "frame {}: player {}'s inputs come {:.1f} frames after its own frame: it gets {} "
              "frames of guesses",
              m_frame, s + 1, average, std::min(m_config.max_rollback, LATE_STALL_LIMIT)));
        }
      }
      else
      {
        m_late_windows[s] = 0;
        if (m_late[s] && ++m_clean_windows[s] >= LATE_WINDOWS)
        {
          m_late[s] = false;
          m_delay_notes.push_back(
              fmt::format("frame {}: player {}'s inputs are on time again", m_frame, s + 1));
        }
      }
    }
    m_late_sum[s] = 0;
    m_late_samples[s] = 0;
  }
}

int Session::SettleFrom() const
{
  // The newest plug-in of this player's controller or of any other player still in play.
  int newest = INT_MIN / 2;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if ((s == m_config.local_seat || InPlay(s)) && m_plan[s].plug_from != NEVER)
      newest = std::max(newest, m_plan[s].plug_from);
  }
  return newest + SETTLE_SECONDS * m_config.frame_rate;
}

int Session::PlayersInPlay() const
{
  int players = 0;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat || InPlay(s))
      ++players;
  }
  return players;
}

bool Session::InPlay(int seat) const
{
  return seat != m_config.local_seat && seat >= 0 && seat < m_config.seats &&
         m_plan[seat].live_from != NEVER;
}

std::vector<int> Session::MissingInputSeats() const
{
  std::vector<int> seats;
  for (int seat = 0; seat < m_config.seats; ++seat)
  {
    const int through = seat == m_config.local_seat ? DepartedThrough(seat) : KnownThrough(seat);
    if (through < m_frame - StallLimit())
      seats.push_back(seat);
  }
  return seats;
}

bool Session::CatchingUp() const
{
  return m_config.authority_seat >= 0 && m_config.authority_seat != m_config.local_seat &&
         m_plan[m_config.local_seat].plug_from == NEVER;
}

int Session::AuthorityFrame() const
{
  const int a = m_config.authority_seat;
  return a >= 0 && a != m_config.local_seat ? m_remote_frame[a] : m_frame;
}

void Session::AdvanceReceived(int seat)
{
  int& received = m_received[seat];
  // The log holds every seat's pads, and a seat's pads before it plugs in are known (unplugged).
  received = std::max(received, LogEnd() - 1);
  if (m_plan[seat].plug_from != NEVER)
    received = std::max(received, m_plan[seat].plug_from - 1);
  while (m_remote[seat].contains(received + 1))
    ++received;
}

int Session::DepartedThrough(int seat) const
{
  const auto old = m_departed.find(seat);
  if (old == m_departed.end())
    return NEVER;
  const auto& stay = old->second;
  const int through = std::max(LogEnd() - 1, stay.first_frame + static_cast<int>(stay.pads.size()) - 1);
  return through >= stay.until - 1 ? NEVER : through;
}

int Session::KnownThrough(int seat) const
{
  const SeatPlan& plan = m_plan[seat];
  int known = m_received[seat];
  // A friend may hear another seat ahead of the host. Only the host's acknowledged prefix is final.
  if (m_config.per_peer_inputs && m_config.authority_seat >= 0 &&
      m_config.local_seat != m_config.authority_seat && seat != m_config.authority_seat)
    known = std::min(known, m_authority_ack[seat]);
  const int departed = DepartedThrough(seat);
  if (plan.live_from == NEVER || plan.plug_from == NEVER ||
      (plan.unplug_from != NEVER && known >= plan.unplug_from - 1))
    return departed;
  return std::min(known, departed);
}

std::string Session::Describe() const
{
  const auto n = [](int v) { return v == NEVER ? std::string("-") : std::to_string(v); };
  std::string out = fmt::format("frame {} confirmed {} log [{}, {}) local [{}, {}]", m_frame,
                                ConfirmedFrame(), m_log_base, LogEnd(),
                                m_local.empty() ? -1 : m_local.begin()->first,
                                m_local.empty() ? -1 : m_local.rbegin()->first);
  for (int s = 0; s < m_config.seats; ++s)
  {
    const SeatPlan& p = m_plan[s];
    out +=
        fmt::format("; seat {} plug {} unplug {} live {} received {} acked {} at {} history {}/{} "
                    "values {}/{}",
                    s + 1, n(p.plug_from), n(p.unplug_from), n(p.live_from), m_received[s],
                    m_acked[s], m_remote_frame[s], m_history_sent[s], m_history_acked[s],
                    m_values_acked[s], m_values_required[s]);
  }
  return out;
}

void Session::SetLog(int base, std::vector<Pads> log)
{
  m_log_base = base;
  m_log = std::move(log);
  for (int s = 0; s < m_config.seats; ++s)
    AdvanceReceived(s);
}

std::vector<Pads> Session::TakeLog(int* base)
{
  // Every frame that ran is final for the machine handing its log back (the host going solo with
  // the others dropped): their last run is what its state holds.
  while (LogEnd() < m_frame)
  {
    const auto it = m_used.find(LogEnd());
    if (it == m_used.end())
      break;
    m_log.push_back(it->second);
  }
  *base = m_log_base;
  return std::move(m_log);
}

void Session::AddPeer(int seat, int live_from, int history_from)
{
  if (seat < 0 || seat >= m_config.seats || seat == m_config.local_seat)
    return;
  if (m_config.per_peer_inputs && InPlay(seat))
    DropSeat(seat);
  SeatPlan& plan = m_plan[seat];
  plan = {NEVER, NEVER, live_from};
  m_peer_ack[seat].fill(-1);
  m_acked[seat] = live_from - 1;
  m_received[seat] = -1;
  m_remote[seat].clear();
  m_remote_frame[seat] = 0;
  m_history_sent[seat] = m_history_acked[seat] = std::max(history_from, m_log_base) - 1;
  m_history_stuck[seat] = 0;
  m_values_acked[seat] = -1;
  m_peer_stalls[seat] = m_peer_stalls_seen[seat] = m_peer_hitches[seat] = 0;
  m_peer_spared[seat].fill(0);
  m_peer_spared_seen[seat].fill(0);
  m_peer_reports_spared[seat] = false;
  m_peer_stall_delay[seat] = INT_MAX;
  m_edge_frame[seat] = INT_MIN / 2;
  m_advantage[seat].reset();
  m_smoothed_advantage[seat].reset();
  m_remote_advantage[seat].reset();
  m_peer_rtt[seat].reset();
  m_peer_sequence[seat] = 0;
  m_peer_checksum_frame[seat] = -1;
  ResetLateness(seat);
  AdvanceReceived(seat);
}

void Session::RequireValues(int seat, int version)
{
  if (seat >= 0 && seat < MAX_SEATS)
    m_values_required[seat] = std::max(m_values_required[seat], version);
}

void Session::AddPeerBatch(const std::vector<int>& seats, int live_from, int history_from)
{
  int gate_from = std::max(m_frame, m_last_sent_newest + 1);
  const auto gate_needed = [&] {
    if (!m_start_gate)
      return false;
    if (m_start_gate->second > LogEnd())
      return true;
    for (int peer = 0; peer < m_config.seats; ++peer)
      if (InPlay(peer) && m_acked[peer] < m_start_gate->second)
        return true;
    return false;
  };
  if (gate_needed())
  {
    // Keep the same beginning until every in-play peer has acknowledged past the old end.
    // A peer may still hold its open-ended version if it missed the end-frame updates.
    gate_from = std::min(gate_from, m_start_gate->first);
    if (m_start_gate->second != NEVER)
      m_previous_start_gates.push_back(*m_start_gate);
  }
  for (const int seat : seats)
  {
    if (seat < 0 || seat >= m_config.seats || seat == m_config.local_seat)
      continue;
    AddPeer(seat, live_from, history_from);
    m_plug_batch.insert(seat);
  }
  m_start_gate = {gate_from, NEVER};
  // Extending a confirmed gate may cover a gap already run with Start up. Only changed,
  // unconfirmed pads need a rollback; confirmed log entries retain their exact inputs.
  for (auto used = m_used.lower_bound(LogEnd()); used != m_used.end(); ++used)
    if (used->first < m_frame && used->second != PadsFor(used->first))
      m_dirty = std::min(m_dirty, used->first);
}

void Session::SetValuesHeld(int version)
{
  m_values_held = std::max(m_values_held, version);
}

void Session::DropSeat(int seat)
{
  m_plug_batch.erase(seat);
  if (m_start_gate && m_start_gate->second == NEVER && m_plug_batch.empty())
  {
    m_start_gate->second = std::max({m_frame, m_last_sent_newest + 1, m_start_gate->first});
    for (const auto& gate : m_previous_start_gates)
      m_start_gate->second = std::max(m_start_gate->second, gate.second);
  }
  if (!InPlay(seat))
    return;
  SeatPlan& plan = m_plan[seat];
  if (plan.plug_from != NEVER)
  {
    const int last = std::max(plan.plug_from, m_received[seat] + 1);
    if (last < plan.unplug_from)
    {
      plan.unplug_from = last;
      for (int f = last; f < m_frame; ++f)
      {
        if (const auto used = m_used.find(f); used != m_used.end() && !IsUnplugged(used->second[seat]))
          m_dirty = std::min(m_dirty, f);
      }
    }
    if (m_config.per_peer_inputs)
    {
      // Keep one contiguous prefix across successive stays. A rebuilding friend may still need
      // an earlier stay even after another rebuild has begun. Pads also record unplugged gaps.
      DepartedInputs prefix;
      prefix.from = prefix.first_frame = std::min(m_log_base, plan.unplug_from);
      prefix.until = plan.unplug_from;
      for (int frame = prefix.first_frame; frame < prefix.until; ++frame)
      {
        Pad pad = UNPLUGGED_PAD;
        if (frame < LogEnd())
          pad = m_log[frame - m_log_base][seat];
        else if (const Pad* previous = DepartedPad(seat, frame))
          pad = *previous;
        else if (frame >= plan.plug_from && frame < plan.unplug_from)
        {
          const auto input = m_remote[seat].find(frame);
          if (input == m_remote[seat].end())
          {
            Fail("Missing completed seat input");
            return;
          }
          pad = input->second;
        }
        prefix.pads.push_back(pad);
      }
      m_departed[seat] = std::move(prefix);
    }
  }
  plan.live_from = NEVER;
}

void Session::DropAllPeers()
{
  for (int s = 0; s < m_config.seats; ++s)
    DropSeat(s);
}

void Session::RequestLeave()
{
  m_leaving = true;
}

bool Session::LeftDone() const
{
  const int unplug = m_plan[m_config.local_seat].unplug_from;
  const int a = m_config.authority_seat;
  return unplug != NEVER && m_frame > unplug && a >= 0 && a != m_config.local_seat &&
         m_acked[a] >= unplug - 1;
}

bool Session::Idle() const
{
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (InPlay(s))
      return false;
  }
  return m_resim_until < 0 && m_dirty == INT_MAX;
}

bool Session::Settled() const
{
  return m_resim_until < 0 && m_dirty == INT_MAX && ConfirmedFrame() >= m_frame - 1;
}

std::vector<Pad> Session::PendingLocal() const
{
  std::vector<Pad> pending;
  for (auto it = m_local.lower_bound(m_frame); it != m_local.end(); ++it)
    pending.push_back(it->second);
  return pending;
}

void Session::ApplyRoster(const std::array<SeatPlan, MAX_SEATS>& roster)
{
  for (int s = 0; s < m_config.seats; ++s)
  {
    const SeatPlan& now = roster[s];
    SeatPlan& mine = m_plan[s];
    if (now == mine)
      continue;
    // Until the host takes this player in, its roster can still hold whoever had the seat before:
    // this same player, back after its room connection dropped (bug reports 6afb0948, 22ba6446,
    // 96e56887), with that stay's plug-in, and its live_from (before the host heard of the drop) or
    // none (after). The host takes a joiner in from a frame at or after the keyframe this session
    // starts at, so a seat plan with an earlier live_from, or none, is an earlier stay's. Once the
    // host has named this player's live frame (m_own_live_from), every change applies.
    if (s == m_config.local_seat && m_own_live_from == NEVER &&
        (now.live_from == NEVER || now.live_from < m_config.start_frame))
    {
      continue;
    }
    if (s == m_config.local_seat)
    {
      // This player's own seat. The host names its live frame once; plugs it in once, at a frame
      // not yet run, and only once this machine holds the history up to that live frame; and only
      // ever brings its unplug frame earlier. Anything else ends the session.
      if (now.live_from != NEVER)
      {
        if (m_own_live_from == NEVER)
        {
          m_own_live_from = now.live_from;
        }
        else if (now.live_from != m_own_live_from)
        {
          FailPeer(fmt::format("The host moved this player's live frame from {} to {}",
                               m_own_live_from, now.live_from));
          return;
        }
      }
      if (now.plug_from != mine.plug_from &&
          (mine.plug_from != NEVER || now.plug_from < std::max(m_frame, LogEnd()) ||
           m_own_live_from == NEVER || LogEnd() < m_own_live_from))
      {
        FailPeer(fmt::format("The host moved this controller's plug-in from frame {} to {}",
                             mine.plug_from, now.plug_from));
        return;
      }
      if (now.unplug_from > mine.unplug_from)
      {
        FailPeer(fmt::format("The host moved this controller's unplug from frame {} to {}",
                             mine.unplug_from, now.unplug_from));
        return;
      }
    }
    // Frames that ran with this seat plugged in where it now isn't, or the reverse, run again.
    int changed = INT_MAX;
    if (now.plug_from != mine.plug_from)
      changed = std::min(now.plug_from, mine.plug_from);
    if (now.unplug_from != mine.unplug_from)
      changed = std::min({changed, now.unplug_from, mine.unplug_from});
    if (m_config.per_peer_inputs)
      changed = std::max(changed, LogEnd());
    if (changed < m_frame && changed >= LogEnd())
      m_dirty = std::min(m_dirty, changed);
    if (s != m_config.local_seat && mine.live_from == NEVER && now.live_from != NEVER)
      m_acked[s] = now.live_from - 1;
    // A different player in the seat: its packets count from 1 again.
    if (s != m_config.local_seat && now.live_from != mine.live_from && now.live_from != NEVER)
    {
      if (m_config.per_peer_inputs)
      {
        m_acked[s] = now.live_from - 1;
        m_received[s] = -1;
        m_remote[s].clear();
        m_remote_frame[s] = 0;
      }
      m_peer_sequence[s] = 0;
      m_peer_checksum_frame[s] = -1;
      ResetLateness(s);
    }
    mine = now;
    AdvanceReceived(s);
  }
}

void Session::ReceiveHistory(const Packet& packet)
{
  if (packet.history_first > LogEnd() || packet.history.size() > MAX_HISTORY_IN_PACKET)
    return;  // a gap: the host sends it again
  for (size_t i = 0; i < packet.history.size(); ++i)
  {
    const int f = packet.history_first + static_cast<int>(i);
    const Pads& pads = packet.history[i];
    if (f < m_log_base)
      continue;
    if (f < LogEnd())
    {
      if (m_log[f - m_log_base] != pads)
      {
        Fail(fmt::format("The host's history changed at frame {}", f));
        return;
      }
      continue;
    }
    // New history: the host's game before this player plays in it. Never past what the host ever
    // sends ahead of this machine (HISTORY_AHEAD; the rest comes again), and never a frame from this
    // player's live frame or plug-in on (set once each): those run with its own inputs.
    if (m_own_live_from == NEVER || f > m_frame + HISTORY_AHEAD)
      break;
    if (f >= m_own_live_from || f >= m_plan[m_config.local_seat].plug_from)
    {
      FailPeer(fmt::format("The host sent history for frame {}, past this player's live frame {}",
                           f, m_own_live_from));
      return;
    }
    if (const auto used = m_used.find(f); used != m_used.end() && used->second != pads)
      m_dirty = std::min(m_dirty, f);
    m_log.push_back(pads);
  }
  for (int s = 0; s < m_config.seats; ++s)
    AdvanceReceived(s);
}

void Session::AppendConfirmedToLog()
{
  const int final_through = std::min(ConfirmedFrame(), m_frame - 1);
  while (LogEnd() <= final_through && LogEnd() < m_dirty)
  {
    const auto it = m_used.find(LogEnd());
    if (it == m_used.end())
      break;
    m_log.push_back(it->second);
  }
  std::erase_if(m_previous_start_gates,
                [&](const auto& gate) { return gate.second <= LogEnd(); });
  std::erase_if(m_departed, [&](const auto& entry) {
    const auto& [seat, stay] = entry;
    if (LogEnd() < stay.until)
      return false;
    // The host also retains the prefix for every seated peer that has not confirmed it yet.
    if (m_config.local_seat == m_config.authority_seat)
      for (int peer = 0; peer < m_config.seats; ++peer)
        if (InPlay(peer) && m_peer_ack[peer][seat] < stay.until - 1 &&
            !(peer == seat && m_plan[seat].live_from >= stay.until))
          return false;
    return true;
  });
}

void Session::PlugCaughtUpPeers()
{
  if (!m_plug_batch.empty())
  {
    int worst_rtt = 0;
    for (int peer = 0; peer < m_config.seats; ++peer)
      if (InPlay(peer))
        worst_rtt = std::max(worst_rtt, RoundTripFrames(peer));
    bool ready = true;
    for (const int seat : m_plug_batch)
    {
      const int rtt = RoundTripFrames(seat);
      worst_rtt = std::max(worst_rtt, rtt);
      ready &= m_history_acked[seat] >= m_plan[seat].live_from - 1 &&
               m_remote_frame[seat] >= m_frame - CATCH_UP_SLACK - rtt;
      for (int peer = 0; peer < m_config.seats; ++peer)
      {
        if (InPlay(peer) && m_values_acked[peer] < m_values_required[seat])
          ready = false;
      }
    }
    if (ready)
    {
      int plug = m_frame + m_config.max_rollback + 2 * m_config.max_delay + worst_rtt + 4;
      for (const auto& gate : m_previous_start_gates)
        plug = std::max(plug, gate.second);
      m_start_gate->second = plug;
      for (const int seat : m_plug_batch)
      {
        m_plan[seat].plug_from = plug;
        AdvanceReceived(seat);
      }
      m_plug_batch.clear();
    }
  }
  // The host plugs a joiner in once it has replayed the history and caught up, far enough ahead
  // that every machine hears of it before running that frame: the joiner may be up to its delay
  // plus a rollback window ahead of what the host last heard.
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!InPlay(s) || m_plug_batch.contains(s))
      continue;
    SeatPlan& plan = m_plan[s];
    if (plan.plug_from != NEVER || m_history_acked[s] < plan.live_from - 1)
      continue;
    // Every player, the newcomer included, holds the values this seat plugs in with.
    bool values_held = true;
    for (int p = 0; p < m_config.seats; ++p)
    {
      if (InPlay(p) && m_values_acked[p] < m_values_required[s])
        values_held = false;
    }
    if (!values_held)
      continue;
    const int rtt_frames = RoundTripFrames(s);
    if (m_remote_frame[s] < m_frame - CATCH_UP_SLACK - rtt_frames)
      continue;
    plan.plug_from = m_frame + m_config.max_rollback + 2 * m_config.max_delay + rtt_frames + 4;
    AdvanceReceived(s);
  }
}

Step Session::FailPeer(std::string error)
{
  if (m_error.empty())
    m_error_from_peer = true;
  return Fail(std::move(error));
}

Step Session::Fail(std::string error)
{
  if (m_error.empty())
    m_error = std::move(error);
  if (m_resim_until >= 0)
  {
    m_resim_until = -1;
    m_game.SetResimulating(false);
  }
  return {StepKind::Ended, m_frame};
}

Step Session::OnBoundary(int completed_frame, const Pad& local_pad)
{
  if (!m_error.empty())
    return {StepKind::Ended, m_frame};
  // A re-run needs nothing from the transport: a connection lost mid re-run fails the session at
  // the next normal boundary.
  if (!m_transport.Connected() && m_resim_until < 0)
    return Fail("Connection lost");

  // A re-run frame just completed.
  if (m_resim_until >= 0)
  {
    const int next = completed_frame + 1;
    if (NeedsSnapshot(next) && !SaveOnce(next))
      return Fail(fmt::format("Saving frame {} failed", next));
    if (completed_frame < m_resim_until)
    {
      const Pads pads = PadsFor(next);
      m_used[next] = pads;
      m_game.SetPads(next, pads);
      return {StepKind::Run, next};
    }
    // Caught up with the newest frame already run: back to normal flow. This boundary's local input
    // was sampled and sent before the rollback started.
    m_resim_until = -1;
    m_game.SetResimulating(false);
  }
  else
  {
    if (completed_frame != m_frame - 1)
    {
      return Fail(fmt::format("Frame alignment lost: frame {} ended, expected {}", completed_frame,
                              m_frame - 1));
    }
    if (NeedsSnapshot(m_frame) && !SaveOnce(m_frame))
      return Fail(fmt::format("Saving frame {} failed", m_frame));

    // Running ahead of the others: give a frame back now, before it causes a stall here and deep
    // rollbacks there. Decided before sampling, so the frame's input is sampled after the wait.
    if (m_wait_pending)
    {
      m_wait_pending = false;
      m_waiting = true;
      ++m_stats.waits;
      return {StepKind::Wait, m_frame};
    }
    SampleLocal(local_pad);
  }

  ReceivePackets();
  if (!m_error.empty())
    return {StepKind::Ended, m_frame};
  // A stall ends when the input it waited on arrives, even if a rollback comes first; its length is
  // measured up to here (Packet::spared).
  if (m_stalling && m_stall_ended_ms < 0 && m_frame - ConfirmedFrame() <= StallLimit())
    m_stall_ended_ms = m_game.NowMs();
  CheckChecksums();
  if (!m_error.empty())
    return {StepKind::Ended, m_frame};
  if (m_config.authority_seat == m_config.local_seat)
    PlugCaughtUpPeers();
  SendPacket();

  // A frame already ran with a wrong guess: load the newest snapshot at or before it and re-run to
  // the newest frame. Frames before the wrong one re-run with the same inputs, so they come out the
  // same.
  if (m_dirty < m_frame)
  {
    const int dirty = m_dirty;
    m_dirty = INT_MAX;
    if (m_frame - dirty > m_config.max_rollback)
      return Fail(fmt::format("Rollback of {} frames exceeds the limit", m_frame - dirty));
    // There is always a snapshot at most k - 1 frames before any rollback target (NeedsSnapshot).
    const auto after = m_saved.upper_bound(dirty);
    const int from = after == m_saved.begin() ? -1 : *std::prev(after);
    const int depth = m_frame - from;
    if (from < 0 || depth > m_config.max_rollback + MAX_SNAPSHOT_EVERY - 1)
      return Fail(fmt::format("No snapshot to roll back to frame {} from", dirty));
    m_last_saved = -1;
    if (!m_game.Load(from))
      return Fail(fmt::format("Loading frame {} failed", from));
    // Snapshots after the loaded frame belong to the history being replaced.
    m_saved.erase(m_saved.upper_bound(from), m_saved.end());
    ++m_stats.rollbacks;
    m_stats.resimulated_frames += depth;
    m_stats.deepest_rollback = std::max(m_stats.deepest_rollback, depth);
    m_guessed_frames += m_frame - dirty;
    m_resim_until = m_frame - 1;
    m_game.SetResimulating(true);
    const Pads pads = PadsFor(from);
    m_used[from] = pads;
    m_game.SetPads(from, pads);
    return {StepKind::Rollback, from};
  }

  // Too far ahead of the slowest player's inputs: wait without running. A player still replaying
  // the host's game runs only confirmed frames, since guessing there would only roll back.
  if (m_frame - ConfirmedFrame() > StallLimit())
  {
    if (!m_stalling)
    {
      ++m_stats.stalls;
      m_stall_frame = m_frame;
      m_stall_began_ms = m_game.NowMs();
      m_stall_after_input = ConfirmedFrame() >= 0 && Plugged(m_config.local_seat, m_frame) &&
                            m_frame >= SettleFrom();
    }
    m_stalling = true;
    m_stall_ended_ms = -1;
    return {StepKind::Stall, m_frame};
  }
  // A stall just ended; judge it now that the packet ending it (and any hitch or stall it reports)
  // is in. See the rules at the top of this file. Its length, up to when the awaited input arrived,
  // says how many more frames of the others' delay would have spared it (Packet::spared).
  if (m_stalling && m_stall_after_input && m_last_hitch_frame < m_stall_frame - HITCH_FRAMES &&
      m_last_peer_stall_frame < m_stall_frame - HITCH_FRAMES)
  {
    m_stats.counted_stalls = ++m_link_stalls;
    const double held_ms =
        (m_stall_ended_ms >= 0 ? m_stall_ended_ms : m_game.NowMs()) - m_stall_began_ms;
    const double frame_ms = 1000.0 / m_config.frame_rate;
    for (int k = 0; k < SPARED_FRAMES; ++k)
    {
      if (!(held_ms > (k + 1) * frame_ms + SPARED_SLACK_MS))
        ++m_spared[k];
    }
    m_stats.spared_stalls = m_spared[0];
  }
  // After a stall or a wait, the time it held is not made up.
  if (m_stalling || m_waiting)
    m_game.ResetPacing();
  m_stalling = false;
  m_waiting = false;

  const int run = m_frame;
  // Every frame runs with this player's own input for it, the same one the others receive.
  const auto& local_plan = m_plan[m_config.local_seat];
  if (run >= local_plan.plug_from && run < local_plan.unplug_from && run >= LogEnd() &&
      !m_local.contains(run))
    return Fail(fmt::format("No local input for frame {}", run));
  const Pads pads = PadsFor(run);
  m_used[run] = pads;
  m_game.SetPads(run, pads);
  ++m_frame;
  if (m_frame - m_window_start >= m_config.frame_rate)
  {
    AdaptDelay();
    ChooseSnapshotEvery();
  }
  TrackAdvantage();
  UpdateLateness();

  AppendConfirmedToLog();
  // The host's call: a player who unplugged and whose last inputs are in is gone.
  if (m_config.authority_seat == m_config.local_seat)
  {
    for (int s = 0; s < m_config.seats; ++s)
    {
      const SeatPlan& plan = m_plan[s];
      if (InPlay(s) && plan.unplug_from != NEVER && m_frame > plan.unplug_from &&
          m_received[s] >= plan.unplug_from - 1)
      {
        DropSeat(s);
      }
    }
  }

  // History older than the deepest possible rollback (a snapshot up to MAX_SNAPSHOT_EVERY - 1
  // frames before the first wrong frame) is no longer needed. Keep one remote input before the
  // window, since predictions repeat the last known input.
  const int oldest = m_frame - m_config.max_rollback - MAX_SNAPSHOT_EVERY - 1;
  int oldest_unacked = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (InPlay(s))
      oldest_unacked = std::min(oldest_unacked, m_acked[s] + 1);
  }
  // A seated friend hears the batch after its live frame was chosen. Keep a replay-sized prefix
  // until the roster arrives and its new recipient ack can retain the exact range.
  const int keep_local = std::min({oldest, oldest_unacked,
      m_config.per_peer_inputs ? m_frame - HISTORY_AHEAD : oldest});
  std::erase_if(m_local, [keep_local](const auto& e) { return e.first < keep_local; });
  std::erase_if(m_used, [oldest](const auto& e) { return e.first < oldest; });
  m_saved.erase(m_saved.begin(), m_saved.lower_bound(oldest));
  for (int s = 0; s < m_config.seats; ++s)
  {
    auto& inputs = m_remote[s];
    while (inputs.size() > 1 && std::next(inputs.begin())->first < oldest)
      inputs.erase(inputs.begin());
  }
  std::erase_if(m_local_checksums, [this](const auto& e) { return e.first < m_frame - 600; });
  std::erase_if(m_peer_checksums, [this](const auto& e) { return e.first.first < m_frame - 600; });

  m_stats.confirmed_frame = ConfirmedFrame();
  return {StepKind::Run, run};
}

bool Session::SaveOnce(int frame)
{
  if (m_last_saved == frame)
    return true;
  if (!m_game.Save(frame))
    return false;
  m_last_saved = frame;
  m_saved.insert(frame);
  ++m_stats.saves;
  return true;
}

void Session::ChooseSnapshotEvery()
{
  // A save that costs more than a re-run frame is worth spreading out: one more predicted frame per
  // SAVE_MS_PER_SPACING. Re-chosen once a window; changing it at any frame is safe because
  // NeedsSnapshot measures from the newest snapshot, not from frame numbers.
  if (m_config.snapshot_every > 0)
  {
    m_snapshot_every = m_config.snapshot_every;
  }
  else
  {
    const double save_ms = std::isfinite(m_game.SaveMs()) ? m_game.SaveMs() : 0;
    int& k = m_snapshot_every;
    while (k < MAX_SNAPSHOT_EVERY && save_ms > SAVE_MS_PER_SPACING * k + SPACING_DEAD_BAND_MS)
      ++k;
    while (k > 1 && save_ms < SAVE_MS_PER_SPACING * (k - 1) - SPACING_DEAD_BAND_MS)
      --k;
  }
  m_stats.snapshot_every = m_snapshot_every;
}

void Session::SampleLocal(const Pad& local_pad)
{
  const int target = m_frame + m_delay;
  // Drop-in: no input while this controller isn't plugged in.
  const SeatPlan& plan = m_plan[m_config.local_seat];
  if (plan.plug_from == NEVER || target < plan.plug_from || target >= plan.unplug_from)
    return;
  const int newest = m_local.empty() ? plan.plug_from - 1 : m_local.rbegin()->first;
  // After a lower, the target already has an input (sent in an earlier packet): drop this sample,
  // and the next boundary's lands on the frame after. A stalled or waiting boundary repeats its
  // frame and lands here too.
  if (target <= newest)
    return;
  // After a raise, the skipped frames repeat the last input (a held direction stays held) and are
  // sent like any other, so the other players can confirm them.
  const Pad held = m_local.empty() ? Pad{} : m_local.rbegin()->second;
  for (int f = newest + 1; f < target; ++f)
    m_local[f] = held;
  m_local[target] = local_pad;
  if (m_delay_marks.empty() || m_delay_marks.back().second != m_delay)
  {
    m_delay_marks.emplace_back(target, m_delay);
    if (m_delay_marks.size() > 16)
      m_delay_marks.pop_front();
  }
}

int Session::DelayOfInput(int frame) const
{
  // The delay the input for `frame` was sampled at: the newest mark at or before it (the first
  // delay if none).
  for (auto it = m_delay_marks.rbegin(); it != m_delay_marks.rend(); ++it)
  {
    if (it->first <= frame)
      return it->second;
  }
  return m_delay_marks.empty() ? m_delay : m_delay_marks.front().second;
}

void Session::OnRoundTrip(int rtt_ms)
{
  // No working relay measures this; a broken clock or a ping answered after a freeze.
  if (rtt_ms < 0 || rtt_ms > MAX_RTT_MS)
    return;
  m_rtts.push_back(rtt_ms);
  if (static_cast<int>(m_rtts.size()) > RTT_SAMPLES)
    m_rtts.pop_front();
  const int count = static_cast<int>(m_rtts.size());
  if (count < MIN_RTT_SAMPLES)
    return;
  // The median (the lower one of an even count): what most round trips are, whatever the spikes.
  std::array<int, RTT_SAMPLES> sorted{};
  const auto end = std::copy(m_rtts.begin(), m_rtts.end(), sorted.begin());
  const auto middle = sorted.begin() + (count - 1) / 2;
  std::nth_element(sorted.begin(), middle, end);
  m_typical_rtt = *middle;
  m_stats.rtt_median = *m_typical_rtt;
  RaiseToLink();
}

void Session::RaiseToLink()
{
  // The typical link outgrew the rollback window plus the delay: raise to what it needs at once.
  // Lowering happens only in AdaptDelay, a frame at a time.
  if (m_fixed_delay || m_config.max_delay <= m_config.input_delay)
    return;
  const std::optional<int> link = LatencyDelay(-LINK_DEAD_BAND_MS);
  if (!link || *link <= m_delay)
    return;
  const int from = m_delay;
  m_delay = *link;
  m_delay_from_link = true;
  ++m_stats.delay_raises;
  m_stats.delay = m_delay;
  NoteDelay(from, fmt::format("the link (round trips to the relay: median {} ms here, {} ms "
                              "there) needs {}",
                              m_stats.rtt_median, m_stats.peer_rtt_median, *link));
}

void Session::NoteDelay(int from, std::string why)
{
  if (m_delay_notes.size() >= MAX_DELAY_NOTES)
    m_delay_notes.erase(m_delay_notes.begin());
  m_delay_notes.push_back(
      fmt::format("input delay {} -> {} at frame {}: {}", from, m_delay, m_frame, why));
}

void Session::OnLocalHitch()
{
  ++m_hitches;
  m_stats.hitches = m_hitches;
  m_last_hitch_frame = m_frame;
}

void Session::OnLoadHitch()
{
  ++m_stats.load_hitches;
  m_last_hitch_frame = m_frame;
}

void Session::SetFixedDelay(std::optional<int> frames)
{
  if (frames)
    frames = std::clamp(*frames, std::min(1, m_config.max_delay), m_config.max_delay);
  if (frames == m_fixed_delay)
    return;
  const int from = m_delay;
  m_fixed_delay = frames;
  if (m_fixed_delay)
  {
    m_delay = *m_fixed_delay;
  }
  else if (m_config.max_delay > m_config.input_delay)
  {
    // Adaptive again: start from what the link needs, with no stalls on record and a fresh hold.
    m_delay = LatencyDelay(-LINK_DEAD_BAND_MS).value_or(m_config.input_delay);
    m_delay_from_link = true;
    m_link_below_windows = 0;
    m_need_history.fill(NO_NEED);
    m_evidence_windows = 0;
    m_quiet_windows = 0;
    m_hold_windows = STALL_HOLD_WINDOWS;
    m_windows_since_lower = INT_MAX / 2;
  }
  else
  {
    m_delay = m_config.input_delay;
  }
  m_stats.delay = m_delay;
  m_stats.delay_fixed = m_fixed_delay.has_value();
  if (m_delay != from)
    NoteDelay(from, m_fixed_delay ? "chosen by the player" : "adaptive again, from the link's");
}

double Session::FramesFromMs(double ms) const
{
  return ms * m_config.frame_rate / 1000.0;
}

int Session::RoundTripFrames(int seat) const
{
  // Round trip from this machine to `seat` via the relay: our typical one plus theirs (ours twice
  // while only one is known; six frames while neither is).
  const std::optional<int> theirs = m_peer_rtt[seat];
  if (!m_typical_rtt && !theirs)
    return 6;
  const int mine = m_typical_rtt ? *m_typical_rtt : *theirs;
  const int other = theirs ? *theirs : mine;
  return static_cast<int>(std::ceil(FramesFromMs(mine + other)));
}

std::optional<int> Session::LatencyDelay(double margin_ms) const
{
  // Input delay covers only what the rollback window alone would stall on. One way through the
  // relay is half our typical round trip plus half the slowest player's. A sample taken d frames
  // ahead arrives one way later and runs at the next boundary, with LATENCY_SLACK_FRAMES for
  // jitter; the other side runs up to max_rollback frames on a guess before stalling. The epsilon
  // keeps an exact multiple of the frame time from rounding up; margin_ms applies the hysteresis.
  // Until our median and every other player's are in, the link is unknown (nullopt). Two frames
  // cover a round trip of about 240 ms between the two players.
  if (!m_typical_rtt)
    return std::nullopt;
  std::optional<int> peer;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!InPlay(s))
      continue;
    if (!m_peer_rtt[s])
      return std::nullopt;
    peer = std::max(peer.value_or(0), *m_peer_rtt[s]);
  }
  if (!peer)
    return std::nullopt;
  const double one_way = (*m_typical_rtt + *peer) / 2.0 + margin_ms;
  const int frames = static_cast<int>(std::ceil(FramesFromMs(one_way) - 1e-6));
  const int wanted = frames + LATENCY_SLACK_FRAMES - m_config.max_rollback;
  return std::clamp(wanted, m_config.input_delay, m_config.max_delay);
}

void Session::AdaptDelay()
{
  // Once a "second" (frame_rate frames run); the rules are at the top of this file.
  m_window_start = m_frame;
  m_last_resimulated = m_guessed_frames - m_window_resimulated;
  m_window_resimulated = m_guessed_frames;
  // This window's need: the least delay at which the others would have stalled on none of this
  // player's inputs. Only stalls from a player that was waiting on this one's inputs count (its lag
  // reached max_rollback within the last EDGE_WINDOWS). A stall that ended within k frames needs k
  // more frames than its awaited input carried; a longer one needs more than any raise could give.
  // No window holds more stalls than frames, even from a hostile count.
  int need = NO_NEED;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat)
      continue;
    if (m_peer_stalls[s] > m_peer_stalls_seen[s] &&
        m_frame - m_edge_frame[s] <= EDGE_WINDOWS * m_config.frame_rate)
    {
      const int stalls = std::min(m_peer_stalls[s] - m_peer_stalls_seen[s], m_config.frame_rate);
      int frames = 1;
      if (m_peer_reports_spared[s])
      {
        frames = SPARED_FRAMES + 1;
        int spared = 0;
        for (int k = 0; k < SPARED_FRAMES; ++k)
        {
          spared = std::max(spared, m_peer_spared[s][k] - m_peer_spared_seen[s][k]);
          if (spared >= stalls)
          {
            frames = k + 1;
            break;
          }
        }
      }
      const int carried = m_peer_stall_delay[s] != INT_MAX ? m_peer_stall_delay[s] : m_delay;
      need = std::max(need, carried + frames);
    }
    m_peer_stalls_seen[s] = m_peer_stalls[s];
    m_peer_spared_seen[s] = m_peer_spared[s];
    m_peer_stall_delay[s] = INT_MAX;
  }
  // And how close to the edge they ran: their furthest reach minus the window is the least delay
  // that kept them within it. Never above the delay: an uncounted stall (a hitch's, or settling
  // after a plug-in) is no reason to raise.
  need = std::max(need, std::min(std::exchange(m_window_peer_reach, 0) - m_config.max_rollback,
                                 m_delay));
  std::shift_left(m_need_history.begin(), m_need_history.end(), 1);
  m_need_history.back() = need;
  m_evidence_windows = std::min(m_evidence_windows + 1, NEED_WINDOWS);
  // Room for a frame less: in none of the last ROOM_WINDOWS would the others have stalled with a
  // frame less or run within a frame of their window's edge (two frames of room), and at most one
  // window stalled them even at this delay (a spike it couldn't spare either).
  static_assert(ROOM_WINDOWS <= NEED_WINDOWS && MAX_STALL_HOLD_WINDOWS < INT_MAX / 4);
  int tight = 0;
  int over = 0;
  for (int w = NEED_WINDOWS - ROOM_WINDOWS; w < NEED_WINDOWS; ++w)
  {
    tight += m_need_history[w] >= m_delay - 2 && m_need_history[w] <= m_delay ? 1 : 0;
    over += m_need_history[w] > m_delay ? 1 : 0;
  }
  const bool room = tight == 0 && over <= LONE_WINDOWS;
  // Consecutive quiet windows: none where a frame less would have left under two frames of room.
  const bool quiet = need < m_delay - 2 || need > m_delay;
  m_quiet_windows = quiet ? std::min(m_quiet_windows + 1, INT_MAX / 2) : 0;
  m_windows_since_lower = std::min(m_windows_since_lower + 1, INT_MAX / 2);
  // A step down that survived probation: halve the quiet run the next one needs, back toward
  // STALL_HOLD_WINDOWS.
  if (m_windows_since_lower == PROBATION_WINDOWS)
    m_hold_windows = std::max(m_hold_windows / 2, STALL_HOLD_WINDOWS);
  if (m_fixed_delay || m_config.max_delay <= m_config.input_delay)
    return;

  // What the link needs, with hysteresis. Unknown while another player's median is missing (one who
  // just arrived): nothing steps down for the link meanwhile, unless ours never came either.
  const int link = LatencyDelay(-LINK_DEAD_BAND_MS).value_or(m_typical_rtt ? m_delay :
                                                                             m_config.input_delay);
  const int link_holds =
      LatencyDelay(LINK_DEAD_BAND_MS).value_or(m_typical_rtt ? m_delay : m_config.input_delay);
  m_link_below_windows = link_holds < m_delay ? m_link_below_windows + 1 : 0;
  // A raise is worth it once it would have spared the others' stalls in at least RAISE_SPARED
  // windows per frame, among the windows since the last step down (at most NEED_WINDOWS): sustained
  // lateness a little delay cures. Raises go a frame at a time; the next window judges the next
  // frame.
  int raise_for = 0;
  int spared_windows = 0;
  for (int k = 1; m_delay + k <= m_config.max_delay && raise_for == 0; ++k)
  {
    int spared = 0;
    for (int w = NEED_WINDOWS - m_evidence_windows; w < NEED_WINDOWS; ++w)
      spared += m_need_history[w] > m_delay && m_need_history[w] <= m_delay + k ? 1 : 0;
    if (spared >= k * RAISE_SPARED)
    {
      raise_for = k;
      spared_windows = spared;
    }
  }
  // A link-driven delay follows the link back down once it has wanted less for a few windows (e.g.
  // a first median inflated by a keyframe transfer); a stall-driven one waits out the quiet hold,
  // then steps down a frame per window while there is room.
  const bool link_down = m_delay_from_link && m_link_below_windows >= LINK_HOLD_WINDOWS;
  const int from = m_delay;
  if (raise_for > 0)
  {
    // Stalls soon after a step down mean it was wrong: undo it, and require a longer quiet run next
    // time.
    std::string needs;
    for (int w = NEED_WINDOWS - m_evidence_windows; w < NEED_WINDOWS; ++w)
    {
      if (m_need_history[w] > m_delay)
        needs += fmt::format("{}{}", needs.empty() ? "" : " ", m_need_history[w]);
    }
    m_quiet_windows = 0;
    m_delay_from_link = false;
    const int raised = std::clamp(m_delay + 1, link, m_config.max_delay);
    const bool revert = m_windows_since_lower < PROBATION_WINDOWS;
    if (revert)
    {
      ++m_stats.delay_reverts;
      m_hold_windows = std::min(m_hold_windows * 2, MAX_STALL_HOLD_WINDOWS);
      m_windows_since_lower = INT_MAX / 2;
    }
    else
    {
      ++m_stats.delay_raises;
    }
    m_delay = raised;
    NoteDelay(from, fmt::format("{} frame{} more would have spared the others' stalls on this "
                                "player's inputs in {} of the last {} s (the delays those "
                                "seconds needed: {}){}",
                                raise_for, raise_for == 1 ? "" : "s", spared_windows,
                                m_evidence_windows, needs,
                                revert ? fmt::format("; undoes a step down, the next one waits "
                                                     "for {} quiet s",
                                                     m_hold_windows) :
                                         ""));
  }
  else if (link > m_delay)
  {
    RaiseToLink();
  }
  else if (m_delay > link_holds && room && (link_down || m_quiet_windows >= m_hold_windows))
  {
    // Above what the link needs, with room for a frame less: step down one. Earlier windows are
    // spent (the lateness that raised it has passed).
    std::string needs;
    for (int w = NEED_WINDOWS - ROOM_WINDOWS; w < NEED_WINDOWS; ++w)
    {
      needs += fmt::format("{}{}", needs.empty() ? "" : " ",
                           m_need_history[w] == NO_NEED ? std::string("-") :
                                                          std::to_string(m_need_history[w]));
    }
    --m_delay;
    ++m_stats.delay_lowers;
    m_windows_since_lower = 0;
    m_evidence_windows = 0;
    NoteDelay(from, link_down ? fmt::format("the link has needed {} for {} s", link_holds,
                                            m_link_below_windows) :
                                fmt::format("{} quiet s, and room for a frame less (in the "
                                            "last {} s, no second a frame less would have "
                                            "stalled the others or run them within a frame of "
                                            "their window's edge, nor more than one this delay "
                                            "did not spare either: the delays those seconds "
                                            "needed: {})",
                                            m_quiet_windows, ROOM_WINDOWS, needs));
  }
  m_stats.delay = m_delay;
}

void Session::TrackAdvantage()
{
  // GGPO-style time sync: a player running ahead keeps running frames the others' inputs haven't
  // reached, so it rolls back deep and stalls while they never wait. Each side reports its lead
  // (its frame minus the other's last reported one). Both leads include the same relay transit, so
  // half their difference is how far ahead this side really is.
  if (!m_config.time_sync || CatchingUp())
    return;
  std::optional<double> balance;
  for (int s = 0; s < m_config.seats; ++s)
  {
    // A player still catching up is behind by design: nothing to give back to it.
    if (s == m_config.local_seat || !m_advantage[s] || !InPlay(s) || !Plugged(s, m_frame))
      continue;
    auto& smoothed = m_smoothed_advantage[s];
    smoothed = smoothed ? *smoothed + (*m_advantage[s] - *smoothed) / 8 : *m_advantage[s];
    if (m_remote_advantage[s])
    {
      const double b = (*smoothed - *m_remote_advantage[s]) / 2;
      balance = std::max(balance.value_or(b), b);
    }
  }
  if (balance)
  {
    m_balance_sum += *balance;
    ++m_balance_samples;
  }
  if (m_frame - m_sync_start < TIME_SYNC_FRAMES)
    return;
  m_sync_start = m_frame;
  if (m_balance_samples > 0)
  {
    const double average = m_balance_sum / m_balance_samples;
    m_stats.frame_advantage = average;
    m_wait_pending = average > TIME_SYNC_LEAD;
  }
  m_balance_sum = 0;
  m_balance_samples = 0;
}

Pads Session::PadsFor(int frame)
{
  // The host's log decides every seat of a logged frame.
  if (frame >= m_log_base && frame < LogEnd())
    return m_log[frame - m_log_base];
  Pads pads{};
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!Plugged(s, frame))
      pads[s] = UNPLUGGED_PAD;
  }
  if (const auto it = m_local.find(frame);
      it != m_local.end() && Plugged(m_config.local_seat, frame))
  {
    pads[m_config.local_seat] = it->second;
  }
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat || !Plugged(s, frame))
      continue;
    const auto& inputs = m_remote[s];
    if (const auto exact = inputs.find(frame); exact != inputs.end())
    {
      pads[s] = exact->second;
      continue;
    }
    // Prediction: the last input known before this frame.
    const auto after = inputs.upper_bound(frame);
    if (after != inputs.begin())
      pads[s] = std::prev(after)->second;
  }
  for (const auto& [seat, prefix] : m_departed)
    if (const Pad* pad = DepartedPad(seat, frame))
      pads[seat] = *pad;
  const auto gates_frame = [frame](const auto& gate) {
    return frame >= gate.first && frame < gate.second;
  };
  if ((m_start_gate && gates_frame(*m_start_gate)) ||
      std::ranges::any_of(m_previous_start_gates, gates_frame))
  {
    for (Pad& pad : pads)
      pad[0] &= static_cast<u8>(~PAD_WIRE_START_HIGH);
  }
  return pads;
}

bool Session::NeedsSnapshot(int frame) const
{
  // Checksum frames are always saved, since their hash comes from the snapshot.
  if (frame % m_config.checksum_every == 0)
    return true;
  // A rollback only targets a frame that ran on a guess, which is past the newest confirmed one, so
  // the state at the start of a confirmed frame is never loaded.
  if (frame <= ConfirmedFrame())
    return false;
  // Save a predicted frame once it is k frames past the newest snapshot of this history. Any
  // rollback target ran predicted, so it was saved or has a snapshot less than k frames before it
  // that is still there (only a rollback to an earlier frame drops it, and that re-runs the target
  // too). So a rollback never re-runs more than k - 1 frames before the first wrong one. Saving
  // every frame divisible by k would not work: confirmed frames aren't saved, which could leave no
  // snapshot nearby.
  const auto newer = m_saved.lower_bound(frame);
  return newer == m_saved.begin() || frame - *std::prev(newer) >= m_snapshot_every;
}

int Session::ConfirmedFrame() const
{
  int confirmed = DepartedThrough(m_config.local_seat);
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat)
      confirmed = std::min(confirmed, KnownThrough(s));
  }
  // No seat has inputs still to come: everything that ran is confirmed.
  return confirmed >= NEVER ? m_frame : confirmed;
}

void Session::ReceivePackets()
{
  for (const Packet& packet : m_transport.Receive())
  {
    const int s = packet.seat;
    if (s < 0 || s >= m_config.seats || s == m_config.local_seat || !InPlay(s))
      continue;
    // Packets come from another machine: bound everything before using it. No honest peer sends
    // more than a packet's worth of inputs, inputs for frames it can't have reached, or frame
    // numbers near overflow. A joiner is far behind the host by design, so the host's frame and
    // checksums may be anywhere ahead (the host never sends inputs beyond what the joiner said it
    // can take).
    const int window = MAX_FRAMES_AHEAD + m_config.max_delay;
    const int far_limit = CatchingUp() ? (1 << 30) : m_frame + window;
    if (packet.pads.size() > static_cast<size_t>(MAX_PADS_IN_PACKET) || packet.first_frame < 0 ||
        packet.first_frame > m_frame + window ||
        packet.first_frame + static_cast<int>(packet.pads.size()) > m_frame + window + 1 ||
        packet.current_frame < 0 || packet.current_frame > far_limit ||
        (packet.checksum_frame && (*packet.checksum_frame < 0 || *packet.checksum_frame > far_limit)) ||
        packet.history.size() > static_cast<size_t>(MAX_HISTORY_IN_PACKET) ||
        packet.history_first < 0 || packet.history_ack > far_limit)
    {
      FailPeer(fmt::format("Player {} sent an invalid packet", s + 1));
      return;
    }
    // A packet can arrive twice (direct link and relay) and out of order. Inputs, history, acks,
    // running counts, a leave and a checksum are identical in every copy or only grow, so each copy
    // adds them. Fields that say where the sender is now (roster, frame, lead, round trip) come
    // only from a packet newer than any before it from that seat; an older one would set the roster
    // or time sync back.
    const bool stale = packet.sequence > 0 && packet.sequence <= m_peer_sequence[s];
    m_peer_sequence[s] = std::max(m_peer_sequence[s], packet.sequence);
    const bool from_authority = s == m_config.authority_seat;
    if (from_authority && m_config.per_peer_inputs)
    {
      if (!stale)
        for (int peer = 0; peer < m_config.seats; ++peer)
          m_authority_ack[peer] = std::max(m_authority_ack[peer], packet.ack[peer]);
      for (const auto& [seat, stay] : packet.departed)
      {
        if (seat < 0 || seat >= m_config.seats || seat == m_config.authority_seat || stay.from < 0 ||
            stay.until < stay.from || stay.until > NEVER || stay.first_frame < stay.from ||
            stay.pads.size() > MAX_PADS_IN_PACKET ||
            stay.first_frame + static_cast<int>(stay.pads.size()) > stay.until)
        {
          FailPeer("Invalid completed seat inputs");
          return;
        }
        if (stay.until <= LogEnd())
          continue;
        auto& held = m_departed[seat];
        if (held.from != NEVER && stay.until < held.until)
          continue;
        if (held.from == NEVER)
          held = {stay.from, stay.until, std::max(LogEnd(), stay.from), {}};
        held.from = std::min(held.from, stay.from);
        held.until = std::max(held.until, stay.until);
        const int discard = std::min(std::max(0, LogEnd() - held.first_frame),
                                     static_cast<int>(held.pads.size()));
        held.pads.erase(held.pads.begin(), held.pads.begin() + discard);
        held.first_frame = std::max(held.first_frame + discard, LogEnd());
        int seeded = 0;
        for (int frame = held.first_frame + static_cast<int>(held.pads.size());
             frame < std::min({stay.first_frame, stay.until, m_frame + HISTORY_AHEAD}) &&
             seeded < MAX_PADS_IN_PACKET; ++frame, ++seeded)
        {
          if (frame < m_plan[seat].plug_from || frame >= m_plan[seat].unplug_from)
            held.pads.push_back(UNPLUGGED_PAD);
          else if (seat == m_config.local_seat && m_local.contains(frame))
            held.pads.push_back(m_local.at(frame));
          else if (m_remote[seat].contains(frame))
            held.pads.push_back(m_remote[seat].at(frame));
          else
            break;
        }
        for (size_t i = 0; i < stay.pads.size(); ++i)
        {
          const int frame = stay.first_frame + static_cast<int>(i);
          const int end = held.first_frame + static_cast<int>(held.pads.size());
          if (frame > m_frame + HISTORY_AHEAD)
            break;
          if (frame < held.first_frame || frame > end)
            continue;
          if (frame == end)
            held.pads.push_back(stay.pads[i]);
          if (const auto used = m_used.find(frame); used != m_used.end() &&
              used->second[seat] != PadsFor(frame)[seat])
            m_dirty = std::min(m_dirty, frame);
        }
        if (!held.pads.empty())
          m_received[seat] = std::max(m_received[seat],
                                      held.first_frame + static_cast<int>(held.pads.size()) - 1);
      }
    }
    if (from_authority && packet.roster && !stale)
    {
      if (m_config.per_peer_inputs && packet.start_gate && packet.start_gate != m_start_gate)
      {
        const auto [from, until] = *packet.start_gate;
        if (from < 0 || until < from || until > NEVER)
        {
          FailPeer("Invalid batch start gate");
          return;
        }
        const int changed = m_start_gate && from == m_start_gate->first ?
            std::min(until, m_start_gate->second) : from;
        if (changed < m_frame && changed >= LogEnd())
          m_dirty = std::min(m_dirty, changed);
        // The authority may already have confirmed its earlier batch while this peer still
        // needs to rerun it. Keep that interval until our own confirmed log covers it.
        if (m_start_gate && from != m_start_gate->first && m_start_gate->second != NEVER &&
            m_start_gate->second > LogEnd())
          m_previous_start_gates.push_back(*m_start_gate);
        m_start_gate = packet.start_gate;
      }
      ApplyRoster(*packet.roster);
      if (!m_error.empty())
        return;
    }
    if (from_authority && packet.history_seat == m_config.local_seat && !packet.history.empty())
    {
      ReceiveHistory(packet);
      if (!m_error.empty())
        return;
    }
    if (m_config.authority_seat == m_config.local_seat)
    {
      if (packet.history_ack > m_history_acked[s])
      {
        m_history_acked[s] = std::min(packet.history_ack, LogEnd() - 1);
        m_history_stuck[s] = 0;
      }
      m_values_acked[s] = std::max(m_values_acked[s], packet.values_ack);
      // A player asks to leave: its controller unplugs a little ahead, so every machine hears of it
      // first. One still catching up just goes.
      if (packet.leaving)
      {
        SeatPlan& plan = m_plan[s];
        if (plan.plug_from == NEVER)
        {
          plan.live_from = NEVER;
          continue;
        }
        if (plan.unplug_from == NEVER)
        {
          plan.unplug_from = std::max(m_frame, plan.plug_from) + m_config.max_rollback +
                             2 * m_config.max_delay + 4;
        }
      }
    }
    for (size_t i = 0; i < packet.pads.size(); ++i)
    {
      const int f = packet.first_frame + static_cast<int>(i);
      const Pad& pad = packet.pads[i];
      // Inputs for frames this seat isn't plugged in at never run, so ignore them.
      if (!Plugged(s, f) || f < LogEnd())
        continue;
      // New input for a frame that already ran and can no longer be checked: the stall rule should
      // make this impossible, so fail loudly rather than drift.
      if (f > m_received[s] && f < m_frame && !m_used.contains(f) && !m_remote[s].contains(f))
      {
        Fail(fmt::format("Player {}'s input for frame {} arrived after that frame could be re-run",
                         s + 1, f));
        return;
      }
      auto [it, inserted] = m_remote[s].emplace(f, pad);
      if (!inserted)
      {
        if (it->second != pad)
        {
          Fail(fmt::format("Player {} changed its input for frame {}", s + 1, f));
          return;
        }
        continue;
      }
      // A frame that already ran with a different guess must be re-run.
      if (const auto used = m_used.find(f); used != m_used.end() && used->second[s] != PadsFor(f)[s])
        m_dirty = std::min(m_dirty, f);
    }
    AdvanceReceived(s);
    for (int peer = 0; peer < m_config.seats; ++peer)
      m_peer_ack[s][peer] = std::max(m_peer_ack[s][peer], packet.ack[peer]);
    const int ack = packet.ack[m_config.local_seat];
    const int acked_before = m_acked[s];
    if (ack >= -1 && ack <= m_frame + window)
      m_acked[s] = std::max(m_acked[s], ack);
    // An older copy: its checksum may still be new. Running counts below never exceed a newer
    // packet's.
    if (stale)
    {
      AddPeerChecksum(s, packet);
      continue;
    }
    // How far this player ran past the newest of our inputs it had, while it needed them: how deep
    // a change to them would roll it back. AdaptDelay's lowers watch this, and its stalls count for
    // our delay only when it reached max_rollback. Bounded, and ignored around a hitch on either
    // machine.
    if (ack >= 0 && Plugged(m_config.local_seat, packet.current_frame) &&
        Plugged(s, packet.current_frame) && m_last_hitch_frame < m_frame - HITCH_FRAMES)
    {
      const int lag = std::clamp(packet.current_frame - ack, 0, MAX_FRAMES_AHEAD);
      // Plus the delay that input carried: how far its frame ran ahead of this player's sampling (a
      // lag clamped at 0 makes it an upper bound).
      m_window_peer_reach = std::max(m_window_peer_reach, lag + DelayOfInput(ack));
      if (lag >= m_config.max_rollback)
        m_edge_frame[s] = m_frame;
    }
    m_remote_frame[s] = packet.current_frame;
    // How far its inputs trail its own frame (UpdateLateness). Time sync places it no further on
    // than its inputs have come: holding them back makes this machine wait as real lag would.
    int peer_frame = packet.current_frame;
    if (!CatchingUp() && PlayersInPlay() == 2 && Plugged(s, packet.current_frame) &&
        m_received[s] >= m_plan[s].plug_from)
    {
      m_late_sum[s] += packet.current_frame - m_received[s];
      ++m_late_samples[s];
      peer_frame = std::min(peer_frame, m_received[s] + 1);
    }
    // Our lead over this peer, transit included (it cancels against the peer's own figure).
    m_advantage[s] = m_frame - peer_frame;
    // Its own lead and round trip: telemetry, bounded rather than trusted.
    if (packet.advantage)
    {
      constexpr int bound = MAX_FRAMES_AHEAD * 10;
      m_remote_advantage[s] = std::clamp(*packet.advantage, -bound, bound) / 10.0;
    }
    if (packet.rtt && m_peer_rtt[s] != std::clamp(*packet.rtt, 0, MAX_RTT_MS))
    {
      m_peer_rtt[s] = std::clamp(*packet.rtt, 0, MAX_RTT_MS);
      m_stats.peer_rtt_median = -1;
      for (int other = 0; other < m_config.seats; ++other)
      {
        if (m_peer_rtt[other] && InPlay(other))
          m_stats.peer_rtt_median = std::max(m_stats.peer_rtt_median, *m_peer_rtt[other]);
      }
      RaiseToLink();
    }
    // Running counts only grow (a lower number is stale or hostile), and no more stalls can be
    // short than there were. A new stall of theirs just ended, so a stall here around now was
    // caused by it. It waited on the input after the newest of ours it had, whose delay AdaptDelay
    // judges it against.
    if (packet.stalls > m_peer_stalls[s])
    {
      m_peer_stalls[s] = packet.stalls;
      m_last_peer_stall_frame = m_frame;
      m_peer_stall_delay[s] = std::min(m_peer_stall_delay[s], DelayOfInput(acked_before + 1));
    }
    if (packet.spared)
    {
      m_peer_reports_spared[s] = true;
      for (int k = 0; k < SPARED_FRAMES; ++k)
      {
        m_peer_spared[s][k] =
            std::max(m_peer_spared[s][k], std::clamp((*packet.spared)[k], 0, m_peer_stalls[s]));
      }
    }
    if (packet.hitches > m_peer_hitches[s])
    {
      m_peer_hitches[s] = packet.hitches;
      m_stats.peer_hitches = std::max(m_stats.peer_hitches, packet.hitches);
      m_last_hitch_frame = m_frame;
    }
    AddPeerChecksum(s, packet);
  }
}

void Session::AddPeerChecksum(int seat, const Packet& packet)
{
  // A seat's checksums go out in frame order, one per packet: a frame at or before the newest taken
  // is a duplicate copy, already compared.
  if (!packet.checksum_frame || *packet.checksum_frame <= m_peer_checksum_frame[seat])
    return;
  m_peer_checksum_frame[seat] = *packet.checksum_frame;
  m_peer_checksums[{*packet.checksum_frame, seat}] = packet.checksum;
}

void Session::CheckChecksums()
{
  // The state at the start of frame f is final once every input before f is confirmed and no
  // earlier frame is waiting to be re-run.
  const int final_up_to = std::min({m_frame, ConfirmedFrame() + 1, m_dirty});
  int f = m_last_checksum_frame + m_config.checksum_every;
  while (f <= final_up_to)
  {
    const std::optional<u64> hash = m_game.SnapshotChecksum(f);
    if (!hash)
      break;  // not saved yet
    m_local_checksums[f] = *hash;
    m_pending_checksum = f;
    m_last_checksum_frame = f;
    f += m_config.checksum_every;
  }
  for (auto it = m_peer_checksums.begin(); it != m_peer_checksums.end();)
  {
    const auto mine = m_local_checksums.find(it->first.first);
    if (mine == m_local_checksums.end())
    {
      ++it;
      continue;
    }
    if (mine->second != it->second)
    {
      Fail(fmt::format("Desync at frame {}: {:016x} here, {:016x} there", it->first.first, mine->second,
                       it->second));
      return;
    }
    ++m_stats.checksums_matched;
    it = m_peer_checksums.erase(it);
  }
}

void Session::AddDeparted(Packet* packet, int recipient) const
{
  if (!m_config.per_peer_inputs || m_config.authority_seat != m_config.local_seat)
    return;
  for (const auto& [seat, stay] : m_departed)
  {
    int first = stay.first_frame;
    if (recipient == seat)
      first = std::max({first, std::min(m_plan[seat].live_from, stay.until),
                        m_peer_ack[seat][seat] + 1});
    else if (recipient >= 0)
      first = std::max(first, m_peer_ack[recipient][seat] + 1);
    else
    {
      int needed = stay.until;
      for (int peer = 0; peer < m_config.seats; ++peer)
      {
        if (!InPlay(peer))
          continue;
        const int ack = m_peer_ack[peer][seat] + 1;
        needed = std::min(needed, peer == seat ? std::max(ack, m_plan[seat].live_from) : ack);
      }
      first = std::max(first, needed);
    }
    first = std::min(first, stay.until);
    const int offset = first - stay.first_frame;
    const int count = std::min(MAX_PADS_IN_PACKET, static_cast<int>(stay.pads.size()) - offset);
    DepartedInputs copy{stay.from, stay.until, first, {}};
    if (count > 0)
      copy.pads.assign(stay.pads.begin() + offset, stay.pads.begin() + offset + count);
    packet->departed.emplace(seat, std::move(copy));
  }
}

void Session::AddHistory(Packet* packet)
{
  // The host sends one joiner at a time the log frames it lacks, from where the last send stopped
  // (the relay is ordered and reliable), or again from its acknowledgement if that stalls.
  const bool rotate = m_config.per_peer_inputs && PlayersInPlay() > 2;
  for (int offset = 0; offset < m_config.seats; ++offset)
  {
    const int s = (offset + (rotate ? m_history_next : 0)) % m_config.seats;
    if (!InPlay(s))
      continue;
    const int until = std::min(m_plan[s].live_from - 1, LogEnd() - 1);
    if (m_history_acked[s] >= until)
      continue;
    if (m_history_sent[s] > m_history_acked[s] && ++m_history_stuck[s] > HISTORY_RESEND_PACKETS)
    {
      m_history_sent[s] = m_history_acked[s];
      m_history_stuck[s] = 0;
    }
    const int first = std::max(m_history_sent[s], m_history_acked[s]) + 1;
    const int last = std::min({until, first + (rotate ? MULTI_PEER_HISTORY_FRAMES : MAX_HISTORY_IN_PACKET / 2) - 1,
                               std::max(m_remote_frame[s], m_log_base) + HISTORY_AHEAD});
    if (last < first)
      continue;
    packet->history_seat = s;
    packet->history_first = first;
    int runs = 0;
    for (int f = first; f <= last; ++f)
    {
      const Pads& pads = m_log[f - m_log_base];
      if (packet->history.empty() || packet->history.back() != pads)
      {
        if (++runs > MAX_HISTORY_RUNS)
          break;
      }
      packet->history.push_back(pads);
    }
    m_history_sent[s] = first + static_cast<int>(packet->history.size()) - 1;
    if (rotate)
      m_history_next = (s + 1) % m_config.seats;
    return;
  }
}

void Session::SendPacket()
{
  const bool drop_in = m_config.authority_seat >= 0;
  if (m_local.empty() && !drop_in)
    return;
  int oldest_unacked = INT_MAX;
  // The furthest frame every player still waiting for inputs can take. A catching-up player reports
  // its progress; beyond MAX_FRAMES_AHEAD past that, its bounds check would refuse the packet.
  int take_up_to = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!InPlay(s))
      continue;
    oldest_unacked = std::min(oldest_unacked, m_acked[s] + 1);
    if (drop_in)
      take_up_to = std::min(take_up_to, m_remote_frame[s] + MAX_FRAMES_AHEAD - 16);
  }
  const int newest = m_local.empty() ? -1 : m_local.rbegin()->first;
  int first = m_local.empty() ? 0 :
                                std::max({oldest_unacked, newest - m_config.max_pads_per_packet + 1,
                                          m_local.begin()->first});
  int last = newest;
  // A catching-up player needs the oldest inputs first, as many as fit.
  if (drop_in && !m_local.empty() && oldest_unacked != INT_MAX &&
      oldest_unacked >= m_local.begin()->first &&
      newest - oldest_unacked + 1 > m_config.max_pads_per_packet)
  {
    first = oldest_unacked;
    last = std::min(newest, first + MAX_PADS_IN_PACKET - 1);
  }
  last = std::min(last, take_up_to);
  // Nothing new and no checksum to report: stay quiet. A drop-in session always sends, since a
  // joiner paces itself on its frame, acks and roster.
  if (!drop_in && newest == m_last_sent_newest && !m_pending_checksum && first > newest)
    return;

  Packet packet;
  packet.seat = m_config.local_seat;
  // Sequence numbers stop short of the wire format's bound; 1 << 30 packets is years of play.
  m_send_sequence = std::min(m_send_sequence + 1, (1 << 30) - 1);
  packet.sequence = m_send_sequence;
  packet.first_frame = first <= last ? first : 0;
  for (int f = first; f <= last; ++f)
  {
    const auto it = m_local.find(f);
    packet.pads.push_back(it != m_local.end() ? it->second : Pad{});
  }
  if (drop_in)
  {
    packet.history_ack = LogEnd() - 1;
    packet.leaving = m_leaving;
    packet.values_ack = m_values_held;
    if (m_config.authority_seat == m_config.local_seat)
    {
      packet.roster = m_plan;
      packet.start_gate = m_start_gate;
      AddHistory(&packet);
    }
  }
  for (int s = 0; s < m_config.seats; ++s)
    packet.ack[s] = s == m_config.local_seat ? -1 : std::min(m_received[s], DepartedThrough(s));
  // A rebuilding seat can also need several packets of its own completed prefix.
  const auto completed = m_departed.find(m_config.local_seat);
  if (m_config.per_peer_inputs && completed != m_departed.end())
    packet.ack[m_config.local_seat] =
        std::min(DepartedThrough(m_config.local_seat), completed->second.until - 1);
  packet.current_frame = m_frame;
  packet.resimulated = m_last_resimulated;
  packet.stalls = m_link_stalls;
  packet.spared = m_spared;
  packet.hitches = m_hitches;
  std::optional<double> lead;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat && m_smoothed_advantage[s])
      lead = std::max(lead.value_or(*m_smoothed_advantage[s]), *m_smoothed_advantage[s]);
  }
  if (lead)
  {
    constexpr double bound = MAX_FRAMES_AHEAD;
    packet.advantage = static_cast<int>(std::lround(std::clamp(*lead, -bound, bound) * 10));
  }
  packet.rtt = m_typical_rtt;
  if (m_pending_checksum)
  {
    packet.checksum_frame = m_pending_checksum;
    packet.checksum = m_local_checksums[*m_pending_checksum];
    m_pending_checksum.reset();
  }
  m_last_sent_newest = newest;
  if (drop_in && m_config.per_peer_inputs && PlayersInPlay() > 2)
  {
    std::map<int, Packet> recipients;
    for (int seat = 0; seat < m_config.seats; ++seat)
    {
      if (!InPlay(seat))
        continue;
      Packet copy = packet;
      copy.pads.clear();
      const int available = std::min(newest, m_remote_frame[seat] + MAX_FRAMES_AHEAD - 16);
      int from = m_local.empty() ? 0 :
          std::max({m_acked[seat] + 1, newest - m_config.max_pads_per_packet + 1,
                    m_local.begin()->first});
      if (!m_local.empty() && m_acked[seat] + 1 >= m_local.begin()->first &&
          newest - m_acked[seat] > m_config.max_pads_per_packet)
        from = m_acked[seat] + 1;
      const int through = std::min(available, from + MAX_PADS_IN_PACKET - 1);
      copy.first_frame = from <= through ? from : 0;
      for (int frame = from; frame <= through; ++frame)
      {
        const auto input = m_local.find(frame);
        copy.pads.push_back(input != m_local.end() ? input->second : Pad{});
      }
      if (copy.history_seat != seat)
      {
        copy.history_seat = -1;
        copy.history_first = 0;
        copy.history.clear();
      }
      AddDeparted(&copy, seat);
      recipients.emplace(seat, std::move(copy));
    }
    m_transport.SendToSeats(recipients);
    return;
  }
  AddDeparted(&packet, -1);
  m_transport.Send(packet);
}
}  // namespace Orca::Net
