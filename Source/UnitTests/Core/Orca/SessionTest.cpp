// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/FPURoundMode.h"
#include "Common/HttpRequest.h"
#include "Common/WebSocket.h"

#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/RoomResilience.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/YouGameRoom.h"

namespace Rollback
{
namespace OnlineMatch
{
class StallBudget;
}
// Declared here instead of including Core/Rollback/SessionPort.h, whose includes bring in a
// `Config` namespace that clashes with this file's Config.
Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad()>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr,
                            std::chrono::nanoseconds* waited = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence = {},
                            OnlineMatch::StallBudget* stall_budget = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering = {});
}  // namespace Rollback

using namespace Orca::Net;

namespace
{
// A uniform integer in [lo, hi] that is the same on every platform. std::uniform_int_distribution
// differs between standard libraries, so the same seed would simulate different networks.
template <typename T>
T Uniform(std::mt19937& rng, T lo, T hi)
{
  const u64 span = static_cast<u64>(hi - lo) + 1;
  constexpr u64 RANGE = u64{1} << 32;  // mt19937 draws 32 bits
  const u64 limit = RANGE - RANGE % span;
  u64 draw;
  do
  {
    draw = static_cast<u64>(rng());
  } while (draw >= limit);
  return static_cast<T>(lo + static_cast<T>(draw % span));
}

u64 Mix(u64 h, u64 v)
{
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0xff51afd7ed558ccdull;
}

// A deterministic "game": its state is a hash of every frame number and every pad it ran with.
class FakeGame final : public Game
{
public:
  bool Save(int frame) override
  {
    // Saving the same frame twice between loads is wasted work (a full snapshot in Dolphin).
    if (!m_saved_since_load.insert(frame).second)
      ++m_double_saves;
    ++m_saves;
    // Like SnapshotRing: a new frame takes the next slot, evicting what it held.
    if (ring_slots > 0 && !m_snapshots.contains(frame))
    {
      m_ring.resize(ring_slots, -1);
      m_snapshots.erase(m_ring[m_next]);
      m_ring[m_next] = frame;
      m_next = (m_next + 1) % ring_slots;
    }
    m_snapshots[frame] = m_state;
    return true;
  }
  bool Load(int frame) override
  {
    m_saved_since_load.clear();
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return false;
    m_state = it->second;
    m_loads.emplace_back(frame, m_state);
    // Like SnapshotRing: the snapshots after the loaded frame are gone (the re-run saves them
    // again), and the next save takes the slot after the loaded one.
    m_snapshots.erase(m_snapshots.upper_bound(frame), m_snapshots.end());
    if (ring_slots > 0)
    {
      const auto slot = std::find(m_ring.begin(), m_ring.end(), frame);
      for (int& f : m_ring)
      {
        if (f > frame)
          f = -1;
      }
      m_next = static_cast<size_t>(slot - m_ring.begin() + 1) % ring_slots;
    }
    return true;
  }
  void SetPads(int frame, const Pads& pads) override
  {
    m_history[frame] = m_state;  // the state at the start of `frame` (re-runs overwrite)
    m_ran_pads[frame] = pads;
    m_pads_frame = frame;
    m_pads = pads;
  }
  void SetResimulating(bool resimulating) override { m_resimulating = resimulating; }
  std::optional<u64> SnapshotChecksum(int frame) override
  {
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return std::nullopt;
    return it->second;
  }
  void ResetPacing() override { ++m_pacing_resets; }
  double SaveMs() const override { return save_ms; }
  double NowMs() const override { return now_ms; }

  // Runs the frame whose pads were set last; returns its number.
  int Run()
  {
    u64 h = Mix(m_state, static_cast<u64>(m_pads_frame));
    for (const Pad& pad : m_pads)
      for (u8 b : pad)
        h = Mix(h, b);
    m_state = h;
    // A bug that corrupts this frame on every run, re-runs included.
    if (m_corrupt_at == m_pads_frame)
      m_state ^= 1;
    return m_pads_frame;
  }

  // Slots like RingPort's ring (0: keep every snapshot), what a save reports it costs, and the
  // simulated clock (Game::NowMs).
  size_t ring_slots = 0;
  double save_ms = 0;
  double now_ms = 0;
  std::vector<int> m_ring;
  size_t m_next = 0;
  std::map<int, u64> m_snapshots;
  std::set<int> m_saved_since_load;
  int m_saves = 0;
  int m_double_saves = 0;
  std::vector<std::pair<int, u64>> m_loads;
  std::map<int, u64> m_history;
  std::map<int, Pads> m_ran_pads;
  u64 m_state = 1;
  int m_pads_frame = -1;
  Pads m_pads{};
  bool m_resimulating = false;
  int m_pacing_resets = 0;
  std::optional<int> m_corrupt_at;
};

// Both directions of a link, with latency and jitter counted in ticks (one tick = one frame).
struct FakeNet
{
  struct InFlight
  {
    int deliver_at;
    Packet packet;
  };
  std::deque<InFlight> queues[2];
  int now = 0;
  int latency = 0;
  int jitter = 0;
  // Extra latency on the way to each end (index: the receiving end).
  std::array<int, 2> extra{0, 0};
  std::mt19937 rng{1234};
  // An optional direct link beside the relay (off while direct_latency < 0): unordered, lossy,
  // and without the drop-in history.
  int direct_latency = -1;
  int direct_jitter = 0;
  int direct_loss_percent = 0;
  std::array<std::vector<InFlight>, 2> direct;
  int direct_delivered = 0;
  bool relay_open = true;
  // A sender that rewrites its own packets (a modified client), by sending end.
  std::function<void(int from, Packet* packet)> tamper;

  class End final : public Transport
  {
  public:
    End(FakeNet& net, int index) : m_net(net), m_index(index) {}
    void Send(const Packet& sent) override
    {
      Packet packet = sent;
      if (m_net.tamper)
        m_net.tamper(m_index, &packet);
      int delay = m_net.latency + m_net.extra[1 - m_index];
      if (m_net.jitter > 0)
        delay += Uniform(m_net.rng, 0, m_net.jitter);
      // A relayed WebSocket is ordered: never deliver before an earlier packet.
      auto& queue = m_net.queues[1 - m_index];
      int at = m_net.now + delay;
      if (!queue.empty())
        at = std::max(at, queue.back().deliver_at);
      if (m_net.relay_open)
        queue.push_back({at, packet});
      if (m_net.direct_latency >= 0 &&
          Uniform(m_net.rng, 0, 99) >= m_net.direct_loss_percent)
      {
        Packet copy = packet;
        copy.history_seat = -1;
        copy.history_first = 0;
        copy.history.clear();
        const int wobble =
            m_net.direct_jitter > 0 ? Uniform(m_net.rng, 0, m_net.direct_jitter) : 0;
        m_net.direct[1 - m_index].push_back(
            {m_net.now + m_net.direct_latency + wobble, std::move(copy)});
      }
    }
    std::vector<Packet> Receive() override
    {
      std::vector<Packet> out;
      auto& fast = m_net.direct[m_index];
      for (auto it = fast.begin(); it != fast.end();)
      {
        if (it->deliver_at > m_net.now)
        {
          ++it;
          continue;
        }
        out.push_back(std::move(it->packet));
        it = fast.erase(it);
        ++m_net.direct_delivered;
      }
      auto& queue = m_net.queues[m_index];
      while (!queue.empty() && queue.front().deliver_at <= m_net.now)
      {
        out.push_back(queue.front().packet);
        queue.pop_front();
      }
      return out;
    }
    bool Connected() const override { return true; }

  private:
    FakeNet& m_net;
    int m_index;
  };
};

struct Player
{
  Player(const Config& config, FakeNet& net, int index)
      : game(RingGame(config)), transport(net, index), session(config, game, transport),
        seat(index)
  {
  }

  // A game whose snapshots live in a ring as small as RingPort's.
  static FakeGame RingGame(const Config& config)
  {
    FakeGame game;
    game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
    return game;
  }

  // Inputs that change every few frames, differently per seat, so guesses are often wrong.
  // tag_ticks writes the tick into the pad so a test can tell which sample a frame carries.
  Pad LocalPad(int tick) const
  {
    Pad pad{};
    if (still)
      return pad;
    const int phase = tick + seat * 37;
    pad[0] = static_cast<u8>((phase / 7) % 4);
    pad[1] = static_cast<u8>((phase / 11) % 3 == 0 ? 0x80 : 0);
    if (tag_ticks)
    {
      pad[2] = static_cast<u8>(tick & 0xff);
      pad[3] = static_cast<u8>(tick >> 8);
      pad[4] = 1;
    }
    return pad;
  }

  // An independent model of which sample each frame must carry: the sample at frame F goes to
  // F + delay; frames a raise skips repeat the previous input; a sample whose frame already has
  // one (after a lower, or a repeated stalled boundary) is dropped.
  void Expect(const Pad& pad, int input_delay, int frame, int delay)
  {
    if (expected.empty())
    {
      for (int f = 0; f < input_delay; ++f)
        expected[f] = Pad{};
    }
    const int target = frame + delay;
    const int newest = expected.empty() ? -1 : expected.rbegin()->first;
    if (target <= newest)
    {
      if (delay < last_delay)
        ++dropped_after_lower;
      last_delay = delay;
      return;
    }
    if (target > newest + 1 && newest >= 0)
      ++filled;
    const Pad held = newest >= 0 ? expected.rbegin()->second : Pad{};
    for (int f = newest + 1; f < target; ++f)
      expected[f] = held;
    expected[target] = pad;
    last_delay = delay;
  }

  // The tick a tagged pad was sampled at (-1: neutral, never sampled).
  static int TickOf(const Pad& pad) { return pad[4] ? pad[2] | pad[3] << 8 : -1; }

  // One tick of wall time: re-run any frames a rollback needs, then run at most one new frame.
  void Tick(int tick)
  {
    game.now_ms = tick * 1000.0 / 60;
    // The model applies to the tick's first boundary, which samples unless it is a Wait.
    const int frame = session.CurrentFrame();
    const int delay = session.Delay();
    for (int guard = 0; guard < 64; ++guard)
    {
      const Step step = session.OnBoundary(completed, LocalPad(tick));
      if (guard == 0)
      {
        first_steps.push_back({tick, frame, step.kind});
        if (model_input_delay >= 0 && step.kind != StepKind::Wait)
          Expect(LocalPad(tick), model_input_delay, frame, delay);
      }
      if (step.kind == StepKind::Ended || step.kind == StepKind::Stall ||
          step.kind == StepKind::Wait)
        return;
      const bool new_frame = step.kind == StepKind::Run && !session.Resimulating();
      completed = game.Run();
      if (new_frame)
        return;
    }
    FAIL() << "too many boundaries in one tick";
  }

  FakeGame game;
  FakeNet::End transport;
  Session session;
  int seat;
  int completed = -1;
  bool tag_ticks = false;
  // Never touch the controller: every guess is right, so nothing rolls back.
  bool still = false;
  // >= 0: track `expected` (the session's input_delay).
  int model_input_delay = -1;
  std::map<int, Pad> expected;
  int last_delay = -1;
  int filled = 0;
  int dropped_after_lower = 0;
  struct First
  {
    int tick;
    int frame;
    StepKind kind;
  };
  // The first boundary of every tick: the frame it was at and what it returned.
  std::vector<First> first_steps;
};

struct Match
{
  explicit Match(Config config, int latency, int jitter)
  {
    net.latency = latency;
    net.jitter = jitter;
    config.local_seat = 0;
    a = std::make_unique<Player>(config, net, 0);
    config.local_seat = 1;
    b = std::make_unique<Player>(config, net, 1);
  }
  void Play(int ticks)
  {
    const int end = played + ticks;
    for (int t = played; t < end; ++t)
    {
      net.now = t;
      // Both ends measure their round trip to the relay every half second (by default the link's
      // one-way latency, as the relay sits halfway).
      if (rtt_samples && t % 30 == 0)
      {
        for (int i = 0; i < 2; ++i)
        {
          int rtt = net.latency;
          if (net.jitter > 0)
            rtt += Uniform(rtt_rng, 0, net.jitter);
          (i == 0 ? a : b)->session.OnRoundTrip(relay_rtt_ms[i] >= 0 ? relay_rtt_ms[i] :
                                                                        rtt * 1000 / 60);
        }
      }
      a->Tick(t);
      b->Tick(t);
      // A machine whose clock runs fast: one extra frame every `a_extra_every` ticks.
      if (a_extra_every > 0 && t % a_extra_every == a_extra_every - 1)
        a->Tick(t);
    }
    played = end;
  }
  // Frames both players have confirmed must have identical saved states.
  int CompareConfirmed() const
  {
    const int confirmed = std::min(a->session.GetStats().confirmed_frame,
                                   b->session.GetStats().confirmed_frame);
    int compared = 0;
    for (const auto& [frame, state] : a->game.m_history)
    {
      if (frame > confirmed)
        continue;
      const auto other = b->game.m_history.find(frame);
      if (other == b->game.m_history.end())
        continue;
      EXPECT_EQ(state, other->second) << "frame " << frame;
      ++compared;
    }
    return compared;
  }

  // Each frame both players confirmed must carry, on the other side, the sample the model expects.
  // Returns the frames checked.
  static int CheckSamples(const Player& from, const Player& to)
  {
    const int confirmed = to.session.GetStats().confirmed_frame;
    int checked = 0;
    for (const auto& [frame, pad] : from.expected)
    {
      if (frame > confirmed)
        break;
      const auto ran = to.game.m_ran_pads.find(frame);
      if (ran == to.game.m_ran_pads.end())
        continue;
      EXPECT_EQ(ran->second[from.seat], pad) << "seat " << from.seat << " frame " << frame;
      ++checked;
    }
    return checked;
  }

  FakeNet net;
  std::unique_ptr<Player> a, b;
  bool rtt_samples = false;
  std::array<int, 2> relay_rtt_ms{-1, -1};
  std::mt19937 rtt_rng{99};
  int a_extra_every = 0;
  int played = 0;
};

// The newest frame this player ran with confirmed inputs (confirmed_frame may run ahead of it).
int Settled(const Player& p)
{
  return std::min(p.session.GetStats().confirmed_frame, p.session.CurrentFrame() - 1);
}

void ExpectClean(const Match& match)
{
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
}

// A match through YouGame's relay in simulated wall-clock time (microseconds), modelling two Orca
// processes: each side paces frames like Dolphin's throttle, polls stalls like StepSession, and
// coalesces and sends packets like YouGameRoom's room thread. Packets travel as their JSON.
//
// With a DropIn, side 1 joins side 0's running game like OnlineMatch: the host stores a keyframe
// and starts its session; the joiner loads it, replays the host's log unthrottled until it is
// within CATCH_UP_BEHIND frames, then plugs in.
class RelayMatch
{
public:
  using Micros = s64;
  static constexpr Micros MS = 1000;
  static constexpr Micros FRAME_US = 16667;
  // OnlineMatch's: a joiner further behind the host than this runs unthrottled.
  static constexpr int CATCH_UP_BEHIND = 12;

  struct Side
  {
    // Extra delay (plus 0..jitter) on each message this side sends and receives, kept in order,
    // only between hold_from_s and hold_to_s.
    int hold_out_ms = 0;
    int hold_in_ms = 0;
    int jitter_out_ms = 0;
    int jitter_in_ms = 0;
    int hold_from_s = -1'000'000;
    int hold_to_s = 1'000'000;
    // One way between this machine and the relay.
    int relay_ms = 8;
    // Frames per second relative to 60 (1.01: a clock 1% fast).
    double speed = 1.0;
    // Host time one re-run frame takes.
    Micros rerun_us = 0;
    // The player changes its input about once every this many frames.
    int input_every = 7;
    // The room thread's least time between two sends (YouGameRoom's SEND_GAP).
    int send_gap_ms = 10;
    // Host hitches (disc read, shader compile): 5-20 frame stops about every hitch_every_s seconds
    // (0: never), plus hitch_burst more in the first ten seconds.
    int hitch_every_s = 0;
    int hitch_burst = 0;
    bool report_hitches = true;
    // Wi-Fi spikes: nothing leaves or reaches this machine for a while, then it all arrives at
    // once. Fixed (spike_every_s, spike_ms), drawn from ranges (spike_gap_ms, spike_len_ms), or
    // random Poisson arrivals (spike_mean_gap_ms), only between spike_from_s and spike_to_s.
    int spike_every_s = 0;
    int spike_ms = 0;
    // A direct link beside the relay (-1: none): unordered, lossy, no drop-in history.
    int direct_ms = -1;
    int direct_jitter_ms = 0;
    int direct_loss_percent = 0;
    std::array<int, 2> spike_gap_ms{0, 0};
    std::array<int, 2> spike_len_ms{0, 0};
    int spike_mean_gap_ms = 0;
    int spike_from_s = 0;
    int spike_to_s = 1'000'000;
    // The spikes hold only what reaches this machine (its download stalls; what it sends goes).
    bool spike_in_only = false;
  };

  // Side 1 arrives join_s seconds in; the host starts its session keyframe_frames later (storing
  // the keyframe), and side 1 starts replaying load_ms after that, catch_up_frame_us per frame.
  struct DropIn
  {
    int join_s = 30;
    int keyframe_frames = 261;
    int load_ms = 1900;
    Micros catch_up_frame_us = 3000;
  };

  // `seed` varies every random draw.
  RelayMatch(Config config, const Side& a, const Side& b,
             std::optional<DropIn> drop_in = std::nullopt, u32 seed = 0)
      : m_drop_in(drop_in), m_rng(4321 + seed)
  {
    for (int i = 0; i < 2; ++i)
    {
      config.local_seat = i;
      m_peers[i] = std::make_unique<Peer>(config, i == 0 ? a : b, i, seed);
      if (!m_drop_in)
        m_peers[i]->session.emplace(config, m_peers[i]->game, m_peers[i]->link);
    }
  }

  // Runs until both sides have run `frames` frames (or ten minutes of wall time pass).
  void Play(int frames)
  {
    for (; m_now < END; m_now += MS)
    {
      if (Running(0) && Running(1) && SessionOf(0).CurrentFrame() >= frames &&
          SessionOf(1).CurrentFrame() >= frames)
      {
        return;
      }
      if (!TickOnce())
        return;
    }
  }

  // Runs `seconds` more of wall time.
  void PlaySeconds(int seconds)
  {
    const Micros until = m_now + seconds * 1000 * MS;
    for (; m_now < until && m_now < END; m_now += MS)
    {
      if (!TickOnce())
        return;
    }
  }

  bool Running(int i) const { return m_peers[i]->session.has_value(); }
  const Session& SessionOf(int i) const { return *m_peers[i]->session; }
  const FakeGame& GameOf(int i) const { return m_peers[i]->game; }
  int HitchesOf(int i) const { return m_peers[i]->hitches_reported; }
  int DirectReceivedBy(int i) const { return m_peers[i]->direct_received; }
  // The most each side's delay reached since `MarkDelays`, and every delay change it noted.
  int MaxDelayOf(int i) const { return m_peers[i]->max_delay; }
  void MarkDelays()
  {
    for (const auto& p : m_peers)
      p->max_delay = p->session ? p->session->Delay() : 0;
  }
  const std::vector<std::string>& NotesOf(int i) const { return m_peers[i]->notes; }
  // Drop-in: the frame the friend's controller plugged in at (-1: not yet).
  int PluggedFrame() const { return m_plugged_frame; }

  int CompareConfirmed() const
  {
    const int confirmed = std::min(SessionOf(0).GetStats().confirmed_frame,
                                   SessionOf(1).GetStats().confirmed_frame);
    int compared = 0;
    for (const auto& [frame, state] : GameOf(0).m_history)
    {
      const auto other = GameOf(1).m_history.find(frame);
      if (frame > confirmed || other == GameOf(1).m_history.end())
        continue;
      EXPECT_EQ(state, other->second) << "frame " << frame;
      ++compared;
    }
    return compared;
  }

  void ExpectClean() const
  {
    for (int i = 0; i < 2; ++i)
    {
      ASSERT_TRUE(Running(i)) << "side " << i << " never started";
      EXPECT_TRUE(SessionOf(i).Error().empty()) << SessionOf(i).Error();
    }
  }

  // One line per side, for the record: what a test's numbers were.
  void Print(const char* name) const
  {
    for (int i = 0; i < 2; ++i)
    {
      if (!Running(i))
        continue;
      const Stats& s = SessionOf(i).GetStats();
      std::printf("[ %s %c ] rollbacks %d, re-run %d, deepest %d, stalls %d (%d counted, %d a "
                  "frame would spare), waits %d, delay %d (+%d -%d reverted %d, most %d), "
                  "advantage %.2f, round trip %d ms, hitches %d\n",
                  name, 'A' + i, s.rollbacks, s.resimulated_frames, s.deepest_rollback, s.stalls,
                  s.counted_stalls, s.spared_stalls, s.waits, s.delay, s.delay_raises,
                  s.delay_lowers, s.delay_reverts, m_peers[i]->max_delay, s.frame_advantage,
                  m_peers[i]->rtt, m_peers[i]->hitches_reported);
      if (std::getenv("ORCA_TEST_DELAY_NOTES"))
      {
        for (const std::string& note : m_peers[i]->notes)
          std::printf("    %c: %s\n", 'A' + i, note.c_str());
      }
    }
  }

private:
  static constexpr Micros END = 900'000 * MS;

  struct Wire
  {
    Micros due;
    int from;
    int to;  // == from: a ping, back from the relay as a pong
    Micros ping_sent;
    std::string text;
  };

  struct Link final : Transport
  {
    void Send(const Packet& packet) override
    {
      CoalescePacket(&outgoing, packet);
      if (direct)
        direct_out.push_back(packet);
    }
    std::vector<Packet> Receive() override { return std::exchange(inbox, {}); }
    bool Connected() const override { return true; }
    std::optional<Packet> outgoing;
    std::vector<Packet> inbox;
    bool direct = false;
    std::vector<Packet> direct_out;
  };

  struct Peer
  {
    Peer(const Config& config, const Side& s, int index, u32 seed)
        : side(s), seat(index), period(static_cast<Micros>(std::llround(FRAME_US / s.speed))),
          start(static_cast<Micros>(s.relay_ms + s.hold_in_ms) * MS)
    {
      game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
      link.direct = s.direct_ms >= 0;
      // The room's start message reaches each machine after its own way from the relay.
      next_call = start;
      pace = start;
      std::mt19937 rng(77 + index + seed * 13);
      const auto hitch = [&rng](Micros at) {
        return std::pair{at, Uniform(rng, 5, 20) * FRAME_US};
      };
      for (int i = 0; i < s.hitch_burst; ++i)
      {
        const Micros at = start + Uniform<Micros>(rng, 0, 10'000 * MS);
        hitches.push_back(hitch(at));
      }
      for (Micros at = start; s.hitch_every_s > 0 && at < END;)
      {
        at += Uniform<Micros>(rng, s.hitch_every_s * 500 * MS, s.hitch_every_s * 1500 * MS);
        hitches.push_back(hitch(at));
      }
      std::sort(hitches.begin(), hitches.end());
      for (Micros at = start; s.spike_every_s > 0 && at < END;)
      {
        at += Uniform<Micros>(rng, s.spike_every_s * 900 * MS, s.spike_every_s * 1100 * MS);
        spikes.emplace_back(at, at + s.spike_ms * MS);
      }
      for (Micros at = start; s.spike_gap_ms[1] > 0 && at < END;)
      {
        at += Uniform<Micros>(rng, s.spike_gap_ms[0], s.spike_gap_ms[1]) * MS;
        const Micros length = Uniform<Micros>(rng, s.spike_len_ms[0], s.spike_len_ms[1]) * MS;
        if (at >= s.spike_from_s * 1000 * MS && at < s.spike_to_s * 1000 * MS)
          spikes.emplace_back(at, at + length);
      }
      // Poisson: a geometric number of milliseconds to the next start (no distribution object, so
      // every platform draws the same), never inside the spike before it.
      for (Micros at = start; s.spike_mean_gap_ms > 0 && at < END;)
      {
        do
          at += MS;
        while (Uniform(rng, 0, s.spike_mean_gap_ms - 1) != 0);
        if (!spikes.empty())
          at = std::max(at, spikes.back().second);
        const Micros length = Uniform<Micros>(rng, s.spike_len_ms[0], s.spike_len_ms[1]) * MS;
        if (at >= s.spike_from_s * 1000 * MS && at < s.spike_to_s * 1000 * MS)
          spikes.emplace_back(at, at + length);
      }
    }
    Side side;
    FakeGame game;
    Link link;
    std::optional<Session> session;
    int seat;
    Micros period;
    Micros start;
    int completed = -1;
    // When the emulator next calls the session, and when the current boundary was due.
    Micros next_call;
    Micros pace;
    // Inside one boundary's StepSession loop (stall polls, a wait).
    bool in_step = false;
    bool waited = false;
    int resets_before = 0;
    Micros last_send = -1'000'000 * MS;
    Micros last_ping = -1'000'000 * MS;
    int rtt = -1;
    u32 rtt_sequence = 0;
    u32 fed_sequence = 0;
    std::vector<int> rtt_history;
    bool seeded = false;
    std::deque<Wire> held_out, uplink, downlink, held_in;
    // Direct copies on their way to this machine, in no order.
    std::vector<Wire> direct_in;
    int direct_received = 0;
    // Hitches (start, length), and when the last boundary returned: frame times are measured from
    // there, so stalls and waits don't count as hitches.
    std::vector<std::pair<Micros, Micros>> hitches;
    size_t next_hitch = 0;
    std::optional<Micros> last_return;
    int hitches_reported = 0;
    // Each Wi-Fi spike's start and end, in order.
    std::vector<std::pair<Micros, Micros>> spikes;
    // Drop-in: frames the host ran alone before its session, and whether the joiner is catching up.
    int solo_frame = 0;
    std::vector<Pads> solo_log;
    bool catching_up = false;
    int max_delay = 0;
    std::vector<std::string> notes;
  };

  Peer& At(int i) { return *m_peers[i]; }

  bool TickOnce()
  {
    if (::testing::Test::HasFatalFailure())
      return false;
    StartDropIn();
    MoveMessages();
    for (int i = 0; i < 2; ++i)
    {
      RoomThread(i);
      Emulate(i);
    }
    return true;
  }

  Micros Hold(const Peer& p, int ms, int jitter_ms)
  {
    if (m_now < p.side.hold_from_s * 1000 * MS || m_now >= p.side.hold_to_s * 1000 * MS)
      return 0;
    const int extra = jitter_ms > 0 ? Uniform(m_rng, 0, jitter_ms) : 0;
    return static_cast<Micros>(ms + extra) * MS;
  }

  // When this machine's radio can next move a message: now, or the end of the spike it is in.
  static Micros Radio(const Peer& p, Micros now)
  {
    for (const auto& [from, to] : p.spikes)
    {
      if (now < from)
        break;
      if (now < to)
        return to;
    }
    return now;
  }

  static void Enter(std::deque<Wire>& queue, Wire wire, Micros due)
  {
    wire.due = queue.empty() ? due : std::max(due, queue.back().due);
    queue.push_back(std::move(wire));
  }

  void MoveDirect()
  {
    for (int i = 0; i < 2; ++i)
    {
      Peer& from = At(i);
      for (Packet& packet : std::exchange(from.link.direct_out, {}))
      {
        if (Uniform(m_direct_rng, 0, 99) < from.side.direct_loss_percent)
          continue;
        packet.history_seat = -1;
        packet.history_first = 0;
        packet.history.clear();
        const int extra = from.side.direct_jitter_ms > 0 ?
                              Uniform(m_direct_rng, 0, from.side.direct_jitter_ms) :
                              0;
        At(1 - i).direct_in.push_back({Radio(from, m_now) + (from.side.direct_ms + extra) * MS, i,
                                       1 - i, 0, EncodePacket(packet)});
      }
    }
    for (int i = 0; i < 2; ++i)
    {
      Peer& p = At(i);
      if (Radio(p, m_now) != m_now)
        continue;
      for (auto it = p.direct_in.begin(); it != p.direct_in.end();)
      {
        if (it->due > m_now)
        {
          ++it;
          continue;
        }
        Packet packet;
        ASSERT_TRUE(DecodePacket(it->text, &packet)) << it->text;
        p.link.inbox.push_back(std::move(packet));
        ++p.direct_received;
        it = p.direct_in.erase(it);
      }
    }
  }

  // The pad a player holds at wall time `at`: it changes every few frames.
  static Pad PadAt(const Peer& p, Micros at)
  {
    const int tick = static_cast<int>(at / FRAME_US) + p.seat * 37;
    const int every = p.side.input_every;
    Pad pad{};
    pad[0] = static_cast<u8>((tick / every) % 4);
    pad[1] = static_cast<u8>((tick * 7 / (every * 11)) % 3 == 0 ? 0x80 : 0);
    return pad;
  }

  static Config DropInConfig(const Peer& p, int start_frame)
  {
    Config config;
    config.seats = MAX_SEATS;
    config.local_seat = p.seat;
    config.authority_seat = 0;
    config.start_frame = start_frame;
    std::array<SeatPlan, MAX_SEATS> plan{};
    plan[0] = {0, NEVER, 0};
    config.plan = plan;
    return config;
  }

  void StartDropIn()
  {
    if (!m_drop_in)
      return;
    Peer& host = At(0);
    Peer& friend_ = At(1);
    const Micros arrive = host.start + m_drop_in->join_s * 1000 * MS;
    if (m_keyframe < 0 && m_now >= arrive)
      m_keyframe = host.solo_frame;
    const Micros host_starts = arrive + m_drop_in->keyframe_frames * FRAME_US;
    if (!host.session && m_now >= host_starts)
    {
      // The session starts at the host's next frame with the frames since the keyframe in its
      // log; the pad held now fills the frames the delay skips.
      const int next = host.solo_frame;
      Config config = DropInConfig(host, next);
      config.start_pad = host.solo_log.empty() ? Pad{} : host.solo_log.back()[0];
      host.session.emplace(config, host.game, host.link);
      host.session->SetLog(m_keyframe, std::vector<Pads>(host.solo_log.begin() + m_keyframe,
                                                         host.solo_log.end()));
      host.session->AddPeer(1, next, m_keyframe);
      host.completed = next - 1;
      host.in_step = false;
    }
    if (host.session && !friend_.session &&
        m_now >= host_starts + static_cast<Micros>(m_drop_in->load_ms) * MS)
    {
      const auto keyframe = host.game.m_history.find(m_keyframe);
      ASSERT_NE(keyframe, host.game.m_history.end()) << "no keyframe " << m_keyframe;
      friend_.game.m_state = keyframe->second;
      friend_.session.emplace(DropInConfig(friend_, m_keyframe), friend_.game, friend_.link);
      friend_.completed = m_keyframe - 1;
      friend_.next_call = friend_.pace = m_now;
      friend_.catching_up = true;
    }
  }

  // The host's game before its session: frames at its own pace, logged.
  void RunSolo(Peer& p)
  {
    while (p.next_call <= m_now)
    {
      const int f = p.solo_frame++;
      Pads pads;
      pads.fill(UNPLUGGED_PAD);
      pads[0] = PadAt(p, p.next_call);
      p.game.SetPads(f, pads);
      p.game.Run();
      p.solo_log.push_back(pads);
      p.next_call += p.period;
      p.pace = p.next_call;
    }
  }

  void MoveMessages()
  {
    MoveDirect();
    const auto due = [this](std::deque<Wire>& q) { return !q.empty() && q.front().due <= m_now; };
    const auto pop = [](std::deque<Wire>& q) {
      Wire w = std::move(q.front());
      q.pop_front();
      return w;
    };
    for (int i = 0; i < 2; ++i)
    {
      while (due(At(i).held_out))
      {
        const Micros sent = At(i).side.spike_in_only ? m_now : Radio(At(i), m_now);
        Enter(At(i).uplink, pop(At(i).held_out), sent + At(i).side.relay_ms * MS);
      }
    }
    for (int i = 0; i < 2; ++i)
    {
      while (due(At(i).uplink))
      {
        Wire w = pop(At(i).uplink);
        const int to = w.to;
        Enter(At(to).downlink, std::move(w), m_now + At(to).side.relay_ms * MS);
      }
    }
    for (int i = 0; i < 2; ++i)
    {
      Peer& p = At(i);
      while (due(p.downlink))
      {
        Enter(p.held_in, pop(p.downlink),
              Radio(p, m_now) + Hold(p, p.side.hold_in_ms, p.side.jitter_in_ms));
      }
      while (due(p.held_in))
      {
        const Wire w = pop(p.held_in);
        if (w.from == i)
        {
          p.rtt = static_cast<int>((m_now - w.ping_sent) / MS);
          ++p.rtt_sequence;
          p.rtt_history.push_back(p.rtt);
          continue;
        }
        // Anything that arrives before this side's session starts is dropped.
        if (!p.session)
          continue;
        Packet packet;
        ASSERT_TRUE(DecodePacket(w.text, &packet)) << w.text;
        p.link.inbox.push_back(std::move(packet));
      }
    }
  }

  void RoomThread(int i)
  {
    Peer& p = At(i);
    if (m_now - p.last_ping >= 1000 * MS)
    {
      p.last_ping = m_now;
      Enter(p.held_out, {0, i, i, m_now, {}},
            m_now + Hold(p, p.side.hold_out_ms, p.side.jitter_out_ms));
    }
    if (p.link.outgoing && m_now - p.last_send >= p.side.send_gap_ms * MS)
    {
      p.last_send = m_now;
      Enter(p.held_out, {0, i, 1 - i, 0, EncodePacket(*p.link.outgoing)},
            m_now + Hold(p, p.side.hold_out_ms, p.side.jitter_out_ms));
      p.link.outgoing.reset();
    }
  }

  void Emulate(int i)
  {
    Peer& p = At(i);
    if (!p.session)
    {
      if (m_drop_in && i == 0)
        RunSolo(p);
      return;
    }
    Session& session = *p.session;
    for (int guard = 0; p.next_call <= m_now; ++guard)
    {
      ASSERT_LT(guard, 64) << "side " << i << " ran away";
      const Micros at = p.next_call;
      if (!p.in_step)
      {
        // Once per boundary: report a hitch (unless catching up) and feed a new round trip.
        p.in_step = true;
        p.waited = false;
        p.resets_before = p.game.m_pacing_resets;
        if (p.side.report_hitches && !p.catching_up && p.last_return &&
            at - *p.last_return > 2 * FRAME_US)
        {
          session.OnLocalHitch();
          ++p.hitches_reported;
        }
        if (!p.seeded)
        {
          // At the first boundary the session takes the lobby's recent pings.
          p.seeded = true;
          const size_t n = p.rtt_history.size();
          const size_t keep = static_cast<size_t>(RTT_SAMPLES);
          for (size_t k = n > keep ? n - keep : 0; k < n; ++k)
            session.OnRoundTrip(p.rtt_history[k]);
          p.fed_sequence = p.rtt_sequence;
        }
        else if (p.rtt >= 0 && p.rtt_sequence != p.fed_sequence)
        {
          p.fed_sequence = p.rtt_sequence;
          session.OnRoundTrip(p.rtt);
        }
      }
      p.game.now_ms = static_cast<double>(at) / MS;
      const Step step = session.OnBoundary(p.completed, PadAt(p, at));
      for (std::string& note : session.TakeDelayNotes())
        p.notes.push_back(std::move(note));
      if (step.kind == StepKind::Wait)
      {
        p.waited = true;
        p.next_call = at + FRAME_US;
        continue;
      }
      if (step.kind == StepKind::Stall)
      {
        p.next_call = at + MS;
        continue;
      }
      if (step.kind == StepKind::Ended)
      {
        p.next_call = std::numeric_limits<Micros>::max();
        return;
      }
      p.in_step = false;
      p.last_return = at;
      bool reset = p.waited || p.game.m_pacing_resets != p.resets_before;
      p.completed = p.game.Run();
      p.max_delay = std::max(p.max_delay, session.Delay());
      if (session.Resimulating())
      {
        p.next_call = at + p.side.rerun_us;
        continue;
      }
      if (m_drop_in && i == 1)
      {
        // Unthrottled while far behind the host; pacing restarts once it is close.
        const bool was = p.catching_up;
        p.catching_up = session.CatchingUp() ||
                        session.AuthorityFrame() - step.frame > CATCH_UP_BEHIND;
        reset = reset || (was && !p.catching_up);
        if (m_plugged_frame < 0 && session.Plugged(1, step.frame))
          m_plugged_frame = step.frame;
        if (p.catching_up)
        {
          p.next_call = at + m_drop_in->catch_up_frame_us;
          continue;
        }
      }
      p.pace = std::max((reset ? at : p.pace) + p.period, at - 100 * MS);
      p.next_call = std::max(p.pace, at);
      // A hitch: this frame takes that much longer; the throttle makes up what it can afterwards.
      if (p.next_hitch < p.hitches.size() && p.hitches[p.next_hitch].first <= at)
        p.next_call += p.hitches[p.next_hitch++].second;
    }
  }

  std::array<std::unique_ptr<Peer>, 2> m_peers;
  std::optional<DropIn> m_drop_in;
  int m_keyframe = -1;
  int m_plugged_frame = -1;
  Micros m_now = -3000 * MS;  // the lobby: pings before the match starts
  std::mt19937 m_rng;
  std::mt19937 m_direct_rng{8765};
};
}  // namespace

TEST(OrcaSession, NoLatencyNeverRollsBack)
{
  Match match(Config{}, 0, 0);
  match.Play(600);
  EXPECT_TRUE(match.a->session.Error().empty()) << match.a->session.Error();
  EXPECT_TRUE(match.b->session.Error().empty()) << match.b->session.Error();
  EXPECT_EQ(match.a->session.GetStats().rollbacks, 0);
  EXPECT_EQ(match.b->session.GetStats().rollbacks, 0);
  EXPECT_GT(match.CompareConfirmed(), 500);
  EXPECT_GT(match.a->session.GetStats().checksums_matched, 5);
}

TEST(OrcaSession, SavesOnlyFramesItMayRollBackTo)
{
  // Remote inputs arrive within the input delay, so no frame runs on a guess: only checksum frames
  // (every 60) need a snapshot.
  Match match(Config{}, 1, 0);
  match.Play(600);
  EXPECT_TRUE(match.a->session.Error().empty()) << match.a->session.Error();
  EXPECT_EQ(match.a->session.GetStats().rollbacks, 0);
  EXPECT_LE(match.a->game.m_snapshots.size(), 20u);
  EXPECT_GT(match.a->session.GetStats().checksums_matched, 5);
}

TEST(OrcaSession, LatencyRollsBackAndStaysInSync)
{
  Match match(Config{}, 4, 3);
  match.Play(1200);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
    EXPECT_GT(p->session.GetStats().rollbacks, 10);
    EXPECT_LE(p->session.GetStats().deepest_rollback, Config{}.max_rollback);
    EXPECT_GT(p->session.GetStats().checksums_matched, 10);
  }
  EXPECT_GT(match.CompareConfirmed(), 1000);
}

TEST(OrcaSession, LatencyBeyondTheWindowStalls)
{
  Config config;
  config.max_rollback = 4;
  Match match(config, 10, 2);
  match.Play(900);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
    EXPECT_GT(p->session.GetStats().stalls, 0);
    EXPECT_GT(p->game.m_pacing_resets, 0);
    EXPECT_LE(p->session.GetStats().deepest_rollback, config.max_rollback);
  }
  EXPECT_GT(match.CompareConfirmed(), 300);
}

// A lag switch: one player holds each of its inputs back until just before the other would stall
// on it, while its packets give its true frame. The other machine ran max_rollback frames on
// guesses every frame (its opponent reacting to its inputs ~7 frames sooner than it could). It now
// finds that player late within a few seconds and runs only LATE_STALL_LIMIT frames on guesses for
// it; an honest link, however slow or jittery, is never found late.
TEST(OrcaSession, InputsHeldBackAreFoundLate)
{
  constexpr int HELD = 5;
  Match match(Config{}, 1, 0);
  match.net.tamper = [](int from, Packet* packet) {
    if (from != 1 || packet->pads.empty())
      return;
    const int last = packet->current_frame - HELD;
    const int keep = last - packet->first_frame + 1;
    if (keep <= 0)
    {
      packet->pads.clear();
      packet->first_frame = 0;
    }
    else if (keep < static_cast<int>(packet->pads.size()))
    {
      packet->pads.resize(static_cast<size_t>(keep));
    }
  };
  // How many frames the victim runs on guesses past the late player's inputs, at most, over
  // `ticks`.
  const auto guessing = [&match](int ticks) {
    int most = 0;
    for (int i = 0; i < ticks; ++i)
    {
      match.Play(1);
      most = std::max(most, match.a->session.CurrentFrame() - 1 -
                                match.a->session.GetStats().confirmed_frame);
    }
    return most;
  };
  const int early = guessing(150);
  EXPECT_GE(early, HELD) << "the late player's inputs come HELD frames late";
  EXPECT_FALSE(match.a->session.LateSeat(1));
  match.Play(600);
  ExpectClean(match);
  EXPECT_TRUE(match.a->session.LateSeat(1));
  EXPECT_FALSE(match.b->session.LateSeat(0));
  EXPECT_EQ(match.a->session.GetStats().late_seats, 1);
  // Found late, it gets LATE_STALL_LIMIT (3) frames of guesses, where it had max_rollback.
  const int late = guessing(600);
  EXPECT_LE(late, 3) << "before it was found late: " << early;
  ExpectClean(match);
  EXPECT_GT(match.CompareConfirmed(), 1000);

  // Honest links: slow, jittery, lossy direct beside the relay. Nobody is late.
  for (const auto& [latency, jitter, direct_loss] :
       {std::tuple{4, 3, -1}, std::tuple{12, 6, -1}, std::tuple{6, 4, 25}})
  {
    SCOPED_TRACE(fmt::format("latency {} jitter {} direct loss {}", latency, jitter, direct_loss));
    Match honest(Config{}, latency, jitter);
    if (direct_loss >= 0)
    {
      honest.net.direct_latency = 1;
      honest.net.direct_jitter = 2;
      honest.net.direct_loss_percent = direct_loss;
    }
    honest.Play(1800);
    ExpectClean(honest);
    EXPECT_EQ(honest.a->session.GetStats().late_seats, 0);
    EXPECT_EQ(honest.b->session.GetStats().late_seats, 0);
  }
}

// A late port name asks for a re-run. RerunFrom refuses when no snapshot is in reach (frames that
// ran confirmed have none) instead of failing the session later.
TEST(OrcaSession, RerunOnlyWithASnapshotInReach)
{
  // No guesses, so only checksum frames (every 60) are saved; frame ~100 is 40 past the newest.
  {
    Match match(Config{}, 1, 0);
    match.Play(100);
    Session& session = match.a->session;
    ASSERT_GT(session.CurrentFrame() % 60, Config{}.max_rollback + MAX_SNAPSHOT_EVERY);
    EXPECT_FALSE(session.RerunFrom(session.CurrentFrame() - 2));
    // Beyond the rollback window, never.
    EXPECT_FALSE(session.RerunFrom(session.CurrentFrame() - Config{}.max_rollback - 1));
    // A frame not run yet needs nothing.
    EXPECT_TRUE(session.RerunFrom(session.CurrentFrame()));
    match.Play(200);
    EXPECT_TRUE(session.Error().empty()) << session.Error();
    EXPECT_EQ(session.GetStats().rollbacks, 0);
    EXPECT_GT(match.CompareConfirmed(), 250);
  }
  // Inputs later than the delay: every frame is a guess and saved, so a re-run is in reach.
  {
    Match match(Config{}, 4, 0);
    match.Play(300);
    Session& session = match.a->session;
    const int rollbacks = session.GetStats().rollbacks;
    EXPECT_TRUE(session.RerunFrom(session.CurrentFrame() - 2));
    match.Play(1);
    EXPECT_GT(session.GetStats().rollbacks, rollbacks);
    match.Play(300);
    EXPECT_TRUE(session.Error().empty()) << session.Error();
    EXPECT_TRUE(match.b->session.Error().empty()) << match.b->session.Error();
    EXPECT_GT(match.CompareConfirmed(), 500);
  }
}

TEST(OrcaSession, DetectsDesync)
{
  Match match(Config{}, 3, 1);
  match.b->game.m_corrupt_at = 100;
  match.Play(600);
  const bool detected = match.a->session.Error().find("Desync") != std::string::npos ||
                        match.b->session.Error().find("Desync") != std::string::npos;
  EXPECT_TRUE(detected) << match.a->session.Error() << " / " << match.b->session.Error();
}

TEST(OrcaPadCodec, NeutralIsZeroAndRoundTrips)
{
  EXPECT_EQ(EncodePad(GCPadStatus{}), Pad{});

  GCPadStatus status;
  status.button = PAD_BUTTON_A | PAD_TRIGGER_Z | PAD_GET_ORIGIN | PAD_USE_ORIGIN;
  status.stickX = 0;
  status.stickY = 255;
  status.substickX = 37;
  status.substickY = 200;
  status.triggerLeft = 255;
  status.triggerRight = 12;
  status.analogA = 99;
  const GCPadStatus back = DecodePad(EncodePad(status));
  EXPECT_EQ(back.button, PAD_BUTTON_A | PAD_TRIGGER_Z);  // origin bits never travel
  EXPECT_EQ(back.stickX, 0);
  EXPECT_EQ(back.stickY, 255);
  EXPECT_EQ(back.substickX, 37);
  EXPECT_EQ(back.substickY, 200);
  EXPECT_EQ(back.triggerLeft, 255);
  EXPECT_EQ(back.triggerRight, 12);
  EXPECT_EQ(back.analogA, 0);
  EXPECT_TRUE(back.isConnected);
}

TEST(OrcaSession, RejectsHostilePackets)
{
  struct Inject final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override { return std::exchange(queue, {}); }
    bool Connected() const override { return true; }
    std::vector<Packet> queue;
  };
  const auto run = [](Packet bad) {
    FakeGame game;
    Inject net;
    Config config;
    Session session(config, game, net);
    int completed = -1;
    for (int f = 0; f < 3; ++f)
    {
      session.OnBoundary(completed, Pad{});
      completed = game.Run();
    }
    bad.seat = 1;
    net.queue.push_back(bad);
    session.OnBoundary(completed, Pad{});
    return session.Error();
  };
  Packet huge_frame;
  huge_frame.first_frame = 2147483000;
  huge_frame.pads.resize(1);
  EXPECT_NE(run(huge_frame).find("invalid packet"), std::string::npos);

  Packet many_pads;
  many_pads.first_frame = 0;
  many_pads.pads.resize(100000);
  EXPECT_NE(run(many_pads).find("invalid packet"), std::string::npos);

  Packet negative;
  negative.first_frame = -5;
  negative.pads.resize(1);
  EXPECT_NE(run(negative).find("invalid packet"), std::string::npos);

  Packet fine;
  fine.first_frame = 0;
  fine.pads.resize(4);
  EXPECT_TRUE(run(fine).empty());
}

// Live check against YouGame's rooms worker, only with ORCA_LIVE_TEST=1 (writes nothing: an
// anonymous dev game whose results are never recorded).
TEST(OrcaLive, WebSocketReachesARoom)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  const std::string site = std::getenv("ORCA_SITE") ? std::getenv("ORCA_SITE") : "https://yougame.co";

  Common::HttpRequest http(std::chrono::seconds(15));
  const std::string body = R"({"dev":"orca-live-test","player_id":"orca-live-0000000001","name":"Orca","players":2,"mode":"orca-v1","room":"orcalive1",)"
                           R"("lobby":{"version":1,"minPlayers":2,"maxLocalPlayers":1,"compatibility":"orca-test"}})";
  const auto response = http.Post(site + "/api/multiplayer/ticket", body,
                                  {{"Content-Type", "application/json"}},
                                  Common::HttpRequest::AllowedReturnCodes::All);
  ASSERT_TRUE(response.has_value()) << "no response from " << site;
  const std::string text(response->begin(), response->end());
  ASSERT_EQ(http.GetLastResponseCode(), 200) << text;
  picojson::value json;
  ASSERT_TRUE(picojson::parse(json, text).empty()) << text;
  const std::string url = json.get("url").to_str();
  const std::string ticket = json.get("ticket").to_str();
  std::string ws_url = url;
  if (ws_url.starts_with("https://"))
    ws_url.replace(0, 8, "wss://");
  else if (ws_url.starts_with("http://"))
    ws_url.replace(0, 7, "ws://");
  ws_url += "/room/orcalive1?ticket=" + http.EscapeComponent(ticket);

  Common::WebSocket ws;
  std::string error;
  ASSERT_TRUE(ws.Connect(ws_url, std::chrono::seconds(10), &error)) << error;
  std::optional<std::string> first;
  for (int i = 0; i < 20 && !first; ++i)
    first = ws.ReceiveText(std::chrono::milliseconds(500));
  ASSERT_TRUE(first.has_value()) << ws.Error();
  picojson::value welcome;
  ASSERT_TRUE(picojson::parse(welcome, *first).empty()) << *first;
  EXPECT_EQ(welcome.get("t").to_str(), "welcome") << *first;
  EXPECT_TRUE(ws.SendText(R"({"t":"ping","at":1})", std::chrono::seconds(2))) << ws.Error();
  bool pong = false;
  for (int i = 0; i < 20 && !pong; ++i)
  {
    if (auto m = ws.ReceiveText(std::chrono::milliseconds(500)))
      pong = m->find("\"pong\"") != std::string::npos;
  }
  EXPECT_TRUE(pong);
  ws.Close();
}

TEST(OrcaSessionDelay, DeepRollbacksAloneRaiseNothing)
{
  // 6-8 frames of latency at delay 2: deep rollbacks the window (7) still covers. Rollbacks alone
  // never raise the delay; only stalls do.
  Match match(Config{}, 6, 2);
  match.Play(1200);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_GT(stats.rollbacks, 100);
    EXPECT_GE(stats.deepest_rollback, 4);
    EXPECT_EQ(stats.counted_stalls, 0);
    EXPECT_EQ(stats.delay_raises, 0);
    EXPECT_EQ(p->session.Delay(), 2);
  }
  EXPECT_GT(match.CompareConfirmed(), 1000);
}

TEST(OrcaSessionDelay, StaysPutWhenQuietOrDisabled)
{
  Match quiet(Config{}, 1, 0);
  quiet.Play(1200);
  ExpectClean(quiet);
  EXPECT_EQ(quiet.a->session.Delay(), 2);
  EXPECT_EQ(quiet.a->session.GetStats().delay_raises, 0);

  Config fixed;
  fixed.max_delay = fixed.input_delay;
  Match off(fixed, 6, 2);
  off.Play(1200);
  ExpectClean(off);
  EXPECT_EQ(off.a->session.Delay(), 2);
  EXPECT_EQ(off.a->session.GetStats().delay_raises, 0);
  EXPECT_GT(off.CompareConfirmed(), 1000);
}

TEST(OrcaSessionDelay, SteadyHighRoundTripRaises)
{
  // Rollback hides latency up to its window (7, less 2 for the boundary and jitter); only the
  // rest goes into input delay. 50 ms to the relay: 3 frames one way, all hidden.
  Match near_match(Config{}, 3, 0);
  near_match.rtt_samples = true;
  near_match.Play(600);
  ExpectClean(near_match);
  for (const Player* p : {near_match.a.get(), near_match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }

  // 150 ms each: 9 frames one way, 4 of delay and 5 rolled back. Each side raises once both
  // medians (three round trips each) are known, and stays. At most one stall, at the start.
  Match far_match(Config{}, 9, 0);
  far_match.rtt_samples = true;
  far_match.Play(50);
  for (const Player* p : {far_match.a.get(), far_match.b.get()})
    EXPECT_EQ(p->session.Delay(), 2);
  far_match.Play(130);
  for (const Player* p : {far_match.a.get(), far_match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 4);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
  }
  far_match.Play(1200);
  ExpectClean(far_match);
  for (const Player* p : {far_match.a.get(), far_match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 4);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 0);
    EXPECT_LE(stats.stalls, 1);
    EXPECT_GT(stats.rollbacks, 0);
    EXPECT_LE(stats.deepest_rollback, Config{}.max_rollback);
  }
  EXPECT_GT(far_match.CompareConfirmed(), 1200);

  // One huge round trip moves nothing, nor do one side's alone (the other may be next to the
  // relay). Once typical on both sides, the delay is capped at max_delay.
  Match huge(Config{}, 0, 0);
  huge.a->session.OnRoundTrip(1000);
  huge.Play(120);
  EXPECT_EQ(huge.a->session.Delay(), Config{}.input_delay);
  huge.a->session.OnRoundTrip(1000);
  huge.a->session.OnRoundTrip(1000);
  huge.Play(60);
  EXPECT_EQ(huge.a->session.Delay(), Config{}.input_delay);
  for (int i = 0; i < 3; ++i)
    huge.b->session.OnRoundTrip(1000);
  huge.Play(2);
  EXPECT_EQ(huge.a->session.Delay(), Config{}.max_delay);
  EXPECT_EQ(huge.b->session.Delay(), Config{}.max_delay);
}

TEST(OrcaSessionDelay, LinkRaisesAtOnceAndFallsAFrameAtATime)
{
  // 250 ms to the relay: both delays jump from 2 to max_delay in one step, filling the skipped
  // frames. Then at 16 ms each side steps down one frame per STALL_HOLD_WINDOWS quiet seconds,
  // dropping one sample per step. Every frame must carry the sample the model predicts.
  Match match(Config{}, 5, 1);
  match.rtt_samples = true;
  match.relay_rtt_ms = {250, 250};
  for (Player* p : {match.a.get(), match.b.get()})
  {
    p->tag_ticks = true;
    p->model_input_delay = Config{}.input_delay;
  }
  match.Play(180);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), Config{}.max_delay);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
    EXPECT_EQ(p->filled, 1);
  }
  match.relay_rtt_ms = {16, 16};
  match.Play(3000);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 4);
    EXPECT_EQ(stats.delay_reverts, 0);
    EXPECT_EQ(stats.stalls, 0);
    EXPECT_EQ(p->filled, 1);
    EXPECT_EQ(p->dropped_after_lower, 4);
  }
  EXPECT_GT(Match::CheckSamples(*match.a, *match.b), 2900);
  EXPECT_GT(Match::CheckSamples(*match.b, *match.a), 2900);
  EXPECT_GT(match.CompareConfirmed(), 2900);
}

TEST(OrcaSessionDelay, StepsDownOnlyWithHeadroomAndUndoesAWrongOne)
{
  // Ten frames each way, no round trips measured: at delay 2 the window is a frame short, so
  // both stall and raise to 3. There they run at the window's edge, so neither steps down.
  Match match(Config{}, 10, 0);
  match.Play(1800);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 0);
  }
  const int stalls = match.a->session.GetStats().stalls;

  // Eight frames: one frame of room at delay 2. A step down needs two frames of room at the lower
  // delay, so both stay at 3.
  match.net.latency = 8;
  match.Play(60 * 15);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 0);
  }

  // Seven frames: two frames of room at 2. After five quiet seconds both step down.
  match.net.latency = 7;
  for (int i = 0; i < 20 && (match.a->session.GetStats().delay_lowers == 0 ||
                             match.b->session.GetStats().delay_lowers == 0);
       ++i)
  {
    match.Play(60);
  }
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 1);
  }
  EXPECT_EQ(match.a->session.GetStats().stalls, stalls);

  // Ten again right after: stalls return and the step down is reverted (not counted as a raise).
  match.net.latency = 10;
  for (int i = 0; i < 9 && (match.a->session.GetStats().delay_reverts == 0 ||
                            match.b->session.GetStats().delay_reverts == 0);
       ++i)
  {
    match.Play(60);
  }
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 1);
    EXPECT_EQ(stats.delay_reverts, 1);
  }
  // After a revert, the next step down needs ten quiet windows, not five.
  match.net.latency = 7;
  match.Play(60 * 8);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 1);
  }
  match.Play(60 * 5);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 2);
    EXPECT_EQ(p->session.GetStats().delay_reverts, 1);
  }
  EXPECT_GT(match.CompareConfirmed(), 3000);
}

TEST(OrcaSessionTimeSync, FastSideWaits)
{
  Config config;
  config.max_delay = config.input_delay;  // keep the delay out of it
  const auto play = [&config](bool time_sync, int extra_every, bool tagged = false) {
    config.time_sync = time_sync;
    auto match = std::make_unique<Match>(config, 3, 1);
    match->rtt_samples = true;
    match->a_extra_every = extra_every;
    for (Player* p : {match->a.get(), match->b.get()})
    {
      p->tag_ticks = tagged;
      p->model_input_delay = tagged ? config.input_delay : -1;
    }
    match->Play(2400);
    ExpectClean(*match);
    EXPECT_GT(match->CompareConfirmed(), 2000);
    return match;
  };
  // A's clock runs 2.5% fast (one extra frame every 40 ticks).
  const auto synced = play(true, 40);
  const auto unsynced = play(false, 40);
  const Stats& a = synced->a->session.GetStats();
  const Stats& b = synced->b->session.GetStats();
  EXPECT_GT(a.waits, 30);
  EXPECT_LE(a.waits, 2500 / 30);
  EXPECT_EQ(b.waits, 0);
  EXPECT_EQ(unsynced->a->session.GetStats().waits, 0);
  // Without time sync the fast side runs into the rollback window and stalls. With it, nobody
  // stalls, the lead stays under two frames, and re-simulation drops by at least a third.
  const Stats& ua = unsynced->a->session.GetStats();
  const Stats& ub = unsynced->b->session.GetStats();
  EXPECT_GT(ua.stalls, 30);
  EXPECT_EQ(a.stalls, 0);
  EXPECT_EQ(b.stalls, 0);
  EXPECT_LT(a.frame_advantage, 2.0);
  EXPECT_LT((a.resimulated_frames + b.resimulated_frames) * 3,
            (ua.resimulated_frames + ub.resimulated_frames) * 2);

  // Equal clocks: nobody waits.
  const auto even = play(true, 0);
  EXPECT_EQ(even->a->session.GetStats().waits, 0);
  EXPECT_EQ(even->b->session.GetStats().waits, 0);

  // Each wait resets pacing and samples nothing, so the frame it held carries the fresh pad read
  // after the wait. Checked on what B ran: a boundary at frame F lands at F + delay.
  const auto tagged = play(true, 40, true);
  const Player& ta = *tagged->a;
  const Player& tb = *tagged->b;
  const int delay = config.input_delay;
  EXPECT_GT(ta.session.GetStats().waits, 30);
  EXPECT_GE(ta.game.m_pacing_resets, ta.session.GetStats().waits);
  EXPECT_EQ(ta.session.GetStats().stalls, 0);
  EXPECT_GT(Match::CheckSamples(ta, tb), 2000);
  EXPECT_GT(Match::CheckSamples(tb, ta), 2000);
  int waits_checked = 0;
  for (size_t i = 0; i + 1 < ta.first_steps.size(); ++i)
  {
    const Player::First& wait = ta.first_steps[i];
    const Player::First& after = ta.first_steps[i + 1];
    if (wait.kind != StepKind::Wait || wait.frame + delay > Settled(tb))
      continue;
    ASSERT_EQ(after.frame, wait.frame);
    EXPECT_EQ(tb.game.m_ran_pads.at(wait.frame + delay)[0], ta.LocalPad(after.tick))
        << "wait at tick " << wait.tick;
    ++waits_checked;
  }
  EXPECT_GT(waits_checked, 30);
  // B never waits or stalls: its samples advance exactly one tick per frame.
  for (int f = delay + 1; f <= Settled(ta); ++f)
  {
    EXPECT_EQ(Player::TickOf(ta.game.m_ran_pads.at(f)[1]),
              Player::TickOf(ta.game.m_ran_pads.at(f - 1)[1]) + 1)
        << "frame " << f;
  }
}

// The same through Rollback::StepSession: a wait holds the boundary for a frame period inside the
// call, and the held frame plays the pad read after the wait.
TEST(OrcaSessionTimeSync, StepSessionReadsThePadAgainAfterAWait)
{
  Config config;
  config.max_delay = config.input_delay;  // keep the delay out of it
  config.time_sync = true;
  Match match(config, 3, 1);
  for (Player* p : {match.a.get(), match.b.get()})
    p->tag_ticks = true;
  Player& a = *match.a;
  Player& b = *match.b;
  struct Held
  {
    int frame;
    int tick_after;  // the tick whose pad the session read after the wait
  };
  std::vector<Held> held;
  // A's wait reads the next tick's pad, so A then skips that tick.
  bool skip_next = false;
  const auto step_a = [&](int tick) {
    if (std::exchange(skip_next, false))
      return;
    for (int guard = 0; guard < 64; ++guard)
    {
      const int frame = a.session.CurrentFrame();
      const int waits_before = a.session.GetStats().waits;
      int reads = 0;
      int asked = 0;
      const Step step = Rollback::StepSession(
          a.session, a.game, a.completed, [&] { return a.LocalPad(tick + reads++); },
          // Asked once per Wait and once per Stall poll. The network only moves between ticks,
          // so a stall gives up here and retries next tick.
          [&] { return ++asked > a.session.GetStats().waits - waits_before; },
          std::chrono::microseconds{0});
      if (a.session.GetStats().waits > waits_before)
      {
        held.push_back({frame, tick + 1});
        skip_next = true;
      }
      if (step.kind == StepKind::Ended)
      {
        ASSERT_TRUE(a.session.Error().empty()) << a.session.Error();
        return;
      }
      const bool new_frame = step.kind == StepKind::Run && !a.session.Resimulating();
      a.completed = a.game.Run();
      if (new_frame)
        return;
    }
    FAIL() << "too many boundaries in one tick";
  };
  std::mt19937 rtt_rng{99};
  for (int t = 0; t < 1200; ++t)
  {
    match.net.now = t;
    if (t % 30 == 0)
    {
      for (Player* p : {&a, &b})
      {
        const int rtt = match.net.latency + Uniform(rtt_rng, 0, match.net.jitter);
        p->session.OnRoundTrip(rtt * 1000 / 60);
      }
    }
    step_a(t);
    b.Tick(t);
    // A's clock runs 2.5% fast: one extra frame every 40 ticks.
    if (t % 40 == 39)
      step_a(t);
  }
  ExpectClean(match);
  EXPECT_GT(match.CompareConfirmed(), 1000);
  EXPECT_GT(a.session.GetStats().waits, 5);
  EXPECT_EQ(static_cast<int>(held.size()), a.session.GetStats().waits);
  int checked = 0;
  const int delay = config.input_delay;
  for (const Held& h : held)
  {
    if (h.frame + delay > Settled(b))
      continue;
    // B ran A's read from after the wait, not the older one; the tags tell them apart.
    const Pad ran = b.game.m_ran_pads.at(h.frame + delay)[0];
    EXPECT_EQ(Player::TickOf(ran), h.tick_after) << "wait at frame " << h.frame;
    EXPECT_EQ(ran, a.LocalPad(h.tick_after)) << "wait at frame " << h.frame;
    ++checked;
  }
  EXPECT_GT(checked, 5);
}

TEST(OrcaSessionTimeSync, UnequalRelayRoundTrips)
{
  // A is 17 ms from the relay, B 227 ms. Time sync uses frame numbers, not round trips, so the
  // asymmetry must not make A wait.
  Config config;
  Match match(config, 7, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {17, 227};
  // Too few round trips yet: the delay is input_delay's.
  match.Play(30);
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(match.b->session.Delay(), 2);
  // Both use the shared median, (17 + 227) / 2 = 122 ms: 8 frames one way, 5 rolled back, so
  // both pick 3. B must not take its own 227 ms for the whole link.
  match.Play(150);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
  }
  match.Play(1620);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().waits, 0);
    EXPECT_LT(std::abs(p->session.GetStats().frame_advantage), 1.0);
  }
  EXPECT_GT(match.CompareConfirmed(), 1600);
}

namespace
{
void ExpectNoFrameSavedTwice(int snapshot_every)
{
  SCOPED_TRACE(::testing::Message() << "snapshot every " << snapshot_every);
  // Stalls and time-sync waits repeat the boundary of the same frame many times: one snapshot each.
  Config stalling;
  stalling.snapshot_every = snapshot_every;
  stalling.max_rollback = 4;
  Match stalls(stalling, 10, 2);
  stalls.Play(900);
  Config waiting;
  waiting.snapshot_every = snapshot_every;
  waiting.max_delay = waiting.input_delay;
  Match waits(waiting, 3, 1);
  waits.rtt_samples = true;
  waits.a_extra_every = 40;
  waits.Play(1200);
  for (const Match* match : {&stalls, &waits})
  {
    ExpectClean(*match);
    for (const Player* p : {match->a.get(), match->b.get()})
    {
      EXPECT_GT(p->game.m_saves, 100 / snapshot_every);
      EXPECT_EQ(p->game.m_double_saves, 0);
      EXPECT_EQ(p->session.GetStats().saves, p->game.m_saves);
    }
    EXPECT_GT(match->CompareConfirmed(), 300);
  }
  EXPECT_GT(stalls.a->session.GetStats().stalls, 0);
  EXPECT_GT(waits.a->session.GetStats().waits, 0);
}
}  // namespace

TEST(OrcaSession, NeverSavesAFrameTwice)
{
  for (int k = 1; k <= MAX_SNAPSHOT_EVERY; ++k)
    ExpectNoFrameSavedTwice(k);
}

TEST(OrcaSessionDelay, FixedDelaySamplesLandDelayFramesLater)
{
  // At a fixed delay d with no stall or wait, frames below d run neutral and tick t's sample runs
  // at frame t + d on both machines (also for d = 0).
  for (const int delay : {2, 0})
  {
    Config config;
    config.input_delay = delay;
    config.max_delay = delay;
    Match match(config, 1, 0);
    for (Player* p : {match.a.get(), match.b.get()})
      p->tag_ticks = true;
    match.Play(300);
    ExpectClean(match);
    for (const Player* from : {match.a.get(), match.b.get()})
    {
      const Player* to = from == match.a.get() ? match.b.get() : match.a.get();
      EXPECT_EQ(from->session.GetStats().stalls, 0);
      EXPECT_EQ(from->session.GetStats().waits, 0);
      const int confirmed = Settled(*to);
      ASSERT_GT(confirmed, 250);
      for (int f = 0; f <= confirmed; ++f)
      {
        const Pad& ran = to->game.m_ran_pads.at(f)[from->seat];
        EXPECT_EQ(ran, f < delay ? Pad{} : from->LocalPad(f - delay))
            << "delay " << delay << " seat " << from->seat << " frame " << f;
      }
    }
    EXPECT_GT(match.CompareConfirmed(), 250);
  }
}

TEST(OrcaSessionDelay, RaiseDuringAStall)
{
  // A delay raised during a stall applies at the next sample; the frames in between repeat the
  // sample taken when the stall began.
  Config config;
  config.max_rollback = 2;
  config.time_sync = false;
  Match match(config, 6, 0);
  for (Player* p : {match.a.get(), match.b.get()})
    p->tag_ticks = true;
  Player& a = *match.a;
  while (a.first_steps.empty() || a.first_steps.back().kind != StepKind::Stall)
  {
    match.Play(1);
    ASSERT_LT(match.played, 10);
  }
  const Player::First stall = a.first_steps.back();
  a.session.SetFixedDelay(6);
  ASSERT_EQ(a.session.Delay(), 6);
  match.Play(1);
  const Player::First next = a.first_steps.back();
  ASSERT_EQ(next.kind, StepKind::Stall);
  ASSERT_EQ(next.frame, stall.frame);
  match.Play(300);
  ExpectClean(match);
  const auto ran = [&](int f) { return match.b->game.m_ran_pads.at(f)[0]; };
  // Sampled at delay 2 when the stall began, then at delay 6 on the next boundary.
  EXPECT_EQ(ran(stall.frame + 2), a.LocalPad(stall.tick));
  for (int f = stall.frame + 3; f < stall.frame + 6; ++f)
    EXPECT_EQ(ran(f), a.LocalPad(stall.tick)) << "frame " << f;
  EXPECT_EQ(ran(stall.frame + 6), a.LocalPad(next.tick));
  EXPECT_GT(match.CompareConfirmed(), 200);
}

TEST(OrcaSession, HostilePeerTelemetryIsBounded)
{
  // A peer with good inputs but the worst telemetry it may send: extreme re-simulation counts,
  // leads, frames and round trips must stay bounded.
  struct Hostile final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      const int up_to = session->CurrentFrame() + 4;
      for (; next <= up_to; ++next)
        packet.pads.push_back(Pad{});
      packet.ack = {session->CurrentFrame(), -1, -1, -1};
      packet.current_frame = flip ? 0 : session->CurrentFrame() + MAX_FRAMES_AHEAD;
      flip = !flip;
      packet.resimulated = INT_MAX;
      packet.advantage = INT_MIN;
      packet.rtt = INT_MAX;
      packet.stalls = INT_MAX;
      packet.hitches = INT_MAX;
      return {packet};
    }
    bool Connected() const override { return true; }
    Session* session = nullptr;
    int next = 0;
    bool flip = false;
  };
  FakeGame game;
  Hostile net;
  Config config;
  Session session(config, game, net);
  net.session = &session;
  session.OnRoundTrip(30);
  int completed = -1;
  int frames = 0;
  for (int i = 0; i < 1200; ++i)
  {
    const Step step = session.OnBoundary(completed, Pad{});
    ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
    if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
    {
      completed = game.Run();
      ++frames;
    }
  }
  const Stats& stats = session.GetStats();
  EXPECT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(stats.stalls, 0);
  EXPECT_LE(stats.delay, config.max_delay);
  EXPECT_LE(session.Delay(), config.max_delay);
  EXPECT_GT(stats.waits, 0);
  EXPECT_LE(stats.waits, frames / 30 + 1);
}

TEST(OrcaSession, DepthOneRollbacksLoadTheCorrectedState)
{
  // Every remote input arrives a frame late and differs from the guess. Each depth-1 rollback must
  // load the state saved after the previous frame's re-run, not the uncorrected one. Checked
  // against a game run straight with the right inputs.
  const auto remote = [](int f) { return Pad{static_cast<u8>(f % 5), static_cast<u8>(f % 3)}; };
  struct Late final : Transport
  {
    explicit Late(std::function<Pad(int)> remote_pad) : pad(std::move(remote_pad)) {}
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      for (; next < session->CurrentFrame(); ++next)
        packet.pads.push_back(pad(next));
      packet.current_frame = session->CurrentFrame();
      return {packet};
    }
    bool Connected() const override { return true; }
    std::function<Pad(int)> pad;
    Session* session = nullptr;
    int next = 0;
  };
  FakeGame game;
  Late net(remote);
  Config config;
  config.time_sync = false;
  Session session(config, game, net);
  net.session = &session;
  int completed = -1;
  for (int i = 0; i < 1000; ++i)
  {
    const Step step = session.OnBoundary(completed, Pad{});
    ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
    if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
      completed = game.Run();
  }
  const Stats& stats = session.GetStats();
  EXPECT_GT(stats.rollbacks, 300);
  EXPECT_EQ(stats.deepest_rollback, 1);

  FakeGame straight;
  for (int f = 0; f <= stats.confirmed_frame; ++f)
  {
    straight.SetPads(f, Pads{Pad{}, remote(f)});
    straight.Run();
  }
  for (const auto& [frame, state] : game.m_loads)
  {
    if (frame <= stats.confirmed_frame)
      EXPECT_EQ(state, straight.m_history.at(frame)) << "load of frame " << frame;
  }
  int compared = 0;
  for (int f = 0; f <= stats.confirmed_frame; ++f, ++compared)
    EXPECT_EQ(game.m_history.at(f), straight.m_history.at(f)) << "frame " << f;
  EXPECT_GT(compared, 300);
}

namespace
{
// A burst of B's packets reaches A `late` frames late, so A stalls once, briefly. With `hitch`,
// B's emulator froze instead and B reports it.
void LateBurst(Match& match, bool hitch = false, int late = 10)
{
  if (hitch)
    match.b->session.OnLocalHitch();
  match.net.extra[0] = late;
  match.Play(1);
  match.net.extra[0] = 0;
}
}  // namespace

TEST(OrcaSessionDelay, StallsARaiseWouldSpareRaiseThenDecay)
{
  // A's stalls on B's inputs are for B's delay to answer, weighed by how many a raise would have
  // spared in the last 45 seconds. A 14-frame burst holds A six frames, beyond any raise within
  // max_delay, so it never raises anything.
  {
    Match match(Config{}, 1, 0);
    match.Play(700);
    for (int i = 0; i < 20; ++i)
    {
      LateBurst(match, false, 14);
      match.Play(299);
    }
    ExpectClean(match);
    EXPECT_EQ(match.a->session.GetStats().counted_stalls, 20);
    EXPECT_EQ(match.a->session.GetStats().spared_stalls, 0);
    for (const Player* p : {match.a.get(), match.b.get()})
    {
      EXPECT_EQ(p->session.Delay(), 2);
      EXPECT_EQ(p->session.GetStats().delay_raises, 0);
    }
  }
  // A 10-frame burst holds A two frames, which two more frames of B's delay would spare: worth it
  // after sixteen such seconds in 45 (eight per frame). The sixteenth takes B to 3, then 4; A's
  // delay stays. Once quiet, B steps back down a frame a second.
  Match match(Config{}, 1, 0);
  match.Play(700);
  for (int i = 0; i < 15; ++i)
  {
    LateBurst(match, false, 10);
    match.Play(119);
  }
  EXPECT_EQ(match.a->session.GetStats().counted_stalls, 15);
  EXPECT_EQ(match.a->session.GetStats().spared_stalls, 0);
  EXPECT_EQ(match.b->session.GetStats().stalls, 0);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
  LateBurst(match, false, 10);
  match.Play(59);
  EXPECT_EQ(match.b->session.Delay(), 3);
  match.Play(60);
  EXPECT_EQ(match.b->session.Delay(), 4);
  EXPECT_EQ(match.b->session.GetStats().delay_raises, 2);
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(match.a->session.GetStats().delay_raises, 0);
  match.Play(60 * 3);
  EXPECT_EQ(match.b->session.Delay(), 4);
  match.Play(60 * 5);
  ExpectClean(match);
  EXPECT_EQ(match.b->session.Delay(), 2);
  EXPECT_EQ(match.b->session.GetStats().delay_lowers, 2);
  EXPECT_EQ(match.b->session.GetStats().delay_reverts, 0);
  EXPECT_EQ(match.a->session.GetStats().delay_lowers, 0);
  EXPECT_GT(match.CompareConfirmed(), 2800);
}

TEST(OrcaSessionDelay, OneHiccupBothWaysRaisesNothing)
{
  // The relay holds both directions at once: one event, not two stalls in a row.
  Match match(Config{}, 1, 0);
  match.Play(700);
  match.net.extra = {10, 10};
  match.Play(1);
  match.net.extra = {0, 0};
  match.Play(299);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 1);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
}

TEST(OrcaSessionDelay, StallsOnAPeerHitchRaiseNothing)
{
  // Stalls caused by a hitch B reports: no delay cures a frozen emulator, so neither side raises.
  Match match(Config{}, 1, 0);
  match.Play(700);
  for (int i = 0; i < 4; ++i)
  {
    LateBurst(match, true);
    match.Play(59);
  }
  match.Play(120);
  ExpectClean(match);
  EXPECT_EQ(match.a->session.GetStats().stalls, 4);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
  EXPECT_GT(match.CompareConfirmed(), 800);
}

TEST(OrcaSessionDelay, ALoadHitchCountsApartAndItsStallsDont)
{
  // A frame only a game load made slow: counted apart from hitches, never reported to the other
  // side, and a stall around it doesn't count, as around a hitch.
  Match match(Config{}, 1, 0);
  match.Play(700);
  match.a->session.OnLoadHitch();
  LateBurst(match, false, 14);
  match.Play(120);
  ExpectClean(match);
  const auto a = match.a->session.GetStats();
  EXPECT_EQ(a.stalls, 1);
  EXPECT_EQ(a.counted_stalls, 0);
  EXPECT_EQ(a.hitches, 0);
  EXPECT_EQ(a.load_hitches, 1);
  EXPECT_EQ(match.b->session.GetStats().peer_hitches, 0);
}

TEST(OrcaSessionDelay, NoStallCountsWhileSettling)
{
  // No stall counts in the first ten seconds after controllers plug in (boot, loading, a drop-in
  // settling). After that, enough of them raise B's delay.
  Match match(Config{}, 1, 0);
  match.Play(60);
  // Longer bursts: time sync recovers only a frame per 30, so A still lags from the last one.
  for (int i = 0; i < 5; ++i)
  {
    LateBurst(match, false, 14);
    match.Play(59);
  }
  match.Play(60);
  ExpectClean(match);
  ASSERT_LT(match.b->session.CurrentFrame(), 600);
  EXPECT_EQ(match.a->session.GetStats().stalls, 5);
  EXPECT_EQ(match.a->session.GetStats().counted_stalls, 0);
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  match.Play(300);
  ASSERT_GT(match.b->session.CurrentFrame(), 600);
  for (int i = 0; i < 16; ++i)
  {
    LateBurst(match, false, 10);
    match.Play(119);
  }
  ExpectClean(match);
  EXPECT_EQ(match.a->session.GetStats().counted_stalls, 16);
  EXPECT_EQ(match.a->session.GetStats().delay_raises, 0);
  EXPECT_GE(match.b->session.GetStats().delay_raises, 1);
}

TEST(OrcaSessionDelay, RoundTripsSetTheFloor)
{
  // Round trips of 150 ms (4 frames of delay) on a link that is really one frame: the round trips
  // alone hold the delay there, and quiet play never lowers it below.
  Match match(Config{}, 1, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {150, 150};
  match.Play(60 * 60);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 4);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 0);
  }
  EXPECT_GT(match.CompareConfirmed(), 3500);
}

// One side's Wi-Fi drops out about every 11 s (a Mac's radio going off channel for AWDL) while
// the other hitches about as often. Rollback covers each spike, so neither delay leaves two.
// Spikes past the rollback window stall the other side once each, still too rare to raise.
TEST(OrcaSessionDelay, WifiSpikesKeepTheDelayAtTwo)
{
  for (const int spike_ms : {90, 250})
  {
    SCOPED_TRACE(::testing::Message() << "spikes of " << spike_ms << " ms");
    RelayMatch::Side a, b;
    a.relay_ms = 17;
    b.relay_ms = 11;
    a.spike_every_s = 11;
    a.spike_ms = spike_ms;
    b.hitch_every_s = spike_ms == 90 ? 11 : 0;
    a.input_every = b.input_every = 3;
    a.speed = 1.002;
    RelayMatch match(Config{}, a, b);
    match.Play(12000);
    match.Print(spike_ms == 90 ? "Wi-Fi spikes, friend hitching" : "Wi-Fi spikes past the window");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const Stats& stats = match.SessionOf(i).GetStats();
      EXPECT_EQ(match.SessionOf(i).Delay(), 2) << "side " << i;
      EXPECT_EQ(stats.delay_raises, 0) << "side " << i;
      EXPECT_EQ(stats.delay_reverts, 0) << "side " << i;
      EXPECT_GT(stats.rollbacks, 10) << "side " << i;
      EXPECT_LE(stats.deepest_rollback, Config{}.max_rollback) << "side " << i;
    }
    if (spike_ms == 250)
      EXPECT_GE(match.SessionOf(1).GetStats().counted_stalls, 10);
    EXPECT_GT(match.CompareConfirmed(), 11000);
  }
}

// A delay the player chose never adapts. Back to adaptive, it starts over from the link's need.
TEST(OrcaSessionDelay, FixedDelayNeverAdapts)
{
  // Fixed at 2 on a link a frame beyond the window: both sides stall all along and stay at 2.
  Config two;
  two.fixed_delay = 2;
  Match slow(two, 10, 0);
  slow.rtt_samples = true;
  slow.relay_rtt_ms = {300, 300};
  slow.Play(1800);
  ExpectClean(slow);
  for (const Player* p : {slow.a.get(), slow.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.FixedDelay().value_or(-1), 2);
    EXPECT_TRUE(stats.delay_fixed);
    EXPECT_GT(stats.counted_stalls, 15);
    EXPECT_EQ(stats.delay_raises + stats.delay_lowers + stats.delay_reverts, 0);
  }
  EXPECT_GT(slow.CompareConfirmed(), 1000);

  // Fixed at 5 on a quiet link: it never comes down; back to adaptive, it drops at once. The
  // other side fixed 1 before its first frame and stays there.
  Config five;
  five.fixed_delay = 5;
  Match quiet(five, 1, 0);
  quiet.rtt_samples = true;
  quiet.b->session.SetFixedDelay(1);
  quiet.Play(1800);
  EXPECT_EQ(quiet.a->session.Delay(), 5);
  EXPECT_EQ(quiet.b->session.Delay(), 1);
  quiet.a->session.SetFixedDelay(std::nullopt);
  EXPECT_EQ(quiet.a->session.Delay(), 2);
  EXPECT_FALSE(quiet.a->session.GetStats().delay_fixed);
  quiet.Play(600);
  ExpectClean(quiet);
  EXPECT_EQ(quiet.a->session.Delay(), 2);
  EXPECT_EQ(quiet.b->session.Delay(), 1);
  for (const Player* p : {quiet.a.get(), quiet.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 0);
    EXPECT_EQ(p->session.GetStats().delay_raises + p->session.GetStats().delay_lowers, 0);
  }
  EXPECT_GT(quiet.CompareConfirmed(), 2300);

  // Out of range: clamped to 1..max_delay.
  Match bounds(Config{}, 1, 0);
  bounds.a->session.SetFixedDelay(40);
  EXPECT_EQ(bounds.a->session.Delay(), Config{}.max_delay);
  bounds.a->session.SetFixedDelay(-3);
  EXPECT_EQ(bounds.a->session.Delay(), 1);
}

// A first median inflated by a keyframe transfer raises both sides to max_delay. Once round trips
// are normal again, a delay the link set follows it back down a frame a second.
TEST(OrcaSessionDelay, AnInflatedFirstMedianComesBackDown)
{
  Match match(Config{}, 1, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {300, 300};
  match.Play(70);
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_EQ(p->session.Delay(), Config{}.max_delay);
  match.relay_rtt_ms = {16, 16};
  match.Play(600);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 4);
    EXPECT_EQ(stats.delay_reverts, 0);
  }
}

// With three players, another's stall counts toward our delay only when it was waiting on our
// inputs (its lag on them reached the rollback window), not on the third player's.
TEST(OrcaSessionDelay, StallsCountOnlyFromPlayersWaitingOnUs)
{
  struct Scripted final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      std::vector<Packet> out;
      const int now = session->CurrentFrame();
      for (const int seat : {1, 2})
      {
        Packet packet;
        packet.seat = seat;
        packet.first_frame = next[seat];
        for (; next[seat] <= now + 4; ++next[seat])
          packet.pads.push_back(Pad{});
        packet.current_frame = now;
        packet.ack = {std::max(now - lag[seat], 0), -1, -1, -1};
        packet.stalls = stalls[seat];
        out.push_back(std::move(packet));
      }
      return out;
    }
    bool Connected() const override { return true; }
    Session* session = nullptr;
    std::array<int, 3> next{0, 0, 0};
    std::array<int, 3> lag{0, 2, 2};
    std::array<int, 3> stalls{0, 0, 0};
  };
  FakeGame game;
  Scripted net;
  Config config;
  config.seats = 3;
  config.time_sync = false;
  Session session(config, game, net);
  net.session = &session;
  int completed = -1;
  const auto play = [&](int frames) {
    const int until = session.CurrentFrame() + frames;
    while (session.CurrentFrame() < until)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
      ASSERT_NE(step.kind, StepKind::Stall);
      completed = game.Run();
    }
  };
  play(700);
  // Player 2 stalls while only two frames behind our inputs: it waited on player 3.
  for (int i = 0; i < 5; ++i)
  {
    ++net.stalls[1];
    play(60);
  }
  EXPECT_EQ(session.Delay(), 2);
  EXPECT_EQ(session.GetStats().delay_raises, 0);
  // At the edge of its window on our inputs: ours to answer. With no stall lengths reported, each
  // counts as one a frame would spare, so the eighth in 45 seconds raises.
  net.lag[1] = config.max_rollback + 1;
  for (int i = 0; i < 7; ++i)
  {
    ++net.stalls[1];
    play(60);
  }
  EXPECT_EQ(session.Delay(), 2);
  ++net.stalls[1];
  play(60);
  EXPECT_EQ(session.Delay(), 3);
  EXPECT_EQ(session.GetStats().delay_raises, 1);
}

// A raise of k frames needs stalls it would have spared in 8k of the last 45 seconds. Stalls longer
// than max_delay could cover never count; two-frame stalls raise after sixteen seconds of them.
TEST(OrcaSessionDelay, StallsWeighByWhatARaiseWouldSpare)
{
  struct Scripted final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      const int now = session->CurrentFrame();
      for (; next <= now + 4; ++next)
        packet.pads.push_back(Pad{});
      packet.current_frame = now;
      packet.ack = {std::max(now - lag, 0), -1, -1, -1};
      packet.stalls = stalls;
      packet.spared = spared;
      return {packet};
    }
    bool Connected() const override { return true; }
    Session* session = nullptr;
    int next = 0;
    int lag = 0;
    int stalls = 0;
    std::array<int, SPARED_FRAMES> spared{};
  };
  FakeGame game;
  Scripted net;
  Config config;
  config.time_sync = false;
  Session session(config, game, net);
  net.session = &session;
  int completed = -1;
  const auto play = [&](int frames) {
    const int until = session.CurrentFrame() + frames;
    while (session.CurrentFrame() < until)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
      ASSERT_NE(step.kind, StepKind::Stall);
      completed = game.Run();
    }
  };
  // One stall a second on our inputs, lasting `frames` frames (0: longer than SPARED_FRAMES).
  const auto stall = [&](int frames) {
    net.lag = config.max_rollback + 1;
    ++net.stalls;
    for (int k = 0; k < SPARED_FRAMES; ++k)
      net.spared[k] += frames > 0 && frames <= k + 1 ? 1 : 0;
    play(60);
  };
  play(700);
  for (int i = 0; i < 30; ++i)
    stall(0);
  EXPECT_EQ(session.Delay(), 2);
  for (int i = 0; i < 15; ++i)
    stall(2);
  EXPECT_EQ(session.Delay(), 2);
  stall(2);
  EXPECT_EQ(session.Delay(), 3);
  play(60);
  EXPECT_EQ(session.Delay(), 4);
  EXPECT_EQ(session.GetStats().delay_raises, 2);
  EXPECT_TRUE(session.Error().empty()) << session.Error();
}

// One player changes its chosen delay several times mid-match: a longer delay fills the frames it
// skips, a shorter one drops samples for taken frames, and both machines stay in sync.
TEST(OrcaSessionDelay, ChosenDelayChangesMidMatchStayInSync)
{
  Config config;
  Match match(config, 3, 1);
  for (Player* p : {match.a.get(), match.b.get()})
    p->tag_ticks = true;
  match.a->model_input_delay = config.input_delay;
  match.b->session.SetFixedDelay(4);
  match.b->model_input_delay = 4;
  match.Play(200);
  for (const std::optional<int> delay : {std::optional<int>(1), std::optional<int>(6),
                                         std::optional<int>(3), std::optional<int>()})
  {
    match.a->session.SetFixedDelay(delay);
    match.Play(150);
  }
  ExpectClean(match);
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(match.b->session.Delay(), 4);
  // 2 -> 1 and 6 -> 3 dropped samples, 1 -> 6 skipped frames, 3 -> 2 (adaptive) dropped one more.
  EXPECT_EQ(match.a->filled, 1);
  EXPECT_EQ(match.a->dropped_after_lower, 3);
  EXPECT_EQ(match.b->filled, 0);
  EXPECT_GT(Match::CheckSamples(*match.a, *match.b), 700);
  EXPECT_GT(Match::CheckSamples(*match.b, *match.a), 700);
  EXPECT_GT(match.CompareConfirmed(), 700);
}

TEST(OrcaSessionDelay, StallBeforeFirstInputRaisesNothing)
{
  // Both sides stall once waiting for the other's first packet. That is the trip, not a delay too
  // short, so neither raises.
  Config config;
  config.input_delay = 6;
  config.max_delay = 7;
  Match match(config, 8, 0);
  match.Play(600);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 1);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
    EXPECT_EQ(p->session.Delay(), 6);
  }
  EXPECT_GT(match.CompareConfirmed(), 500);
}

namespace
{
// A slow, jittery link ("T3"): A holds every message 60 ms each way plus 0-70 ms of jitter on the
// way in, A's clock runs 1% fast, and inputs change about every 40 frames.
RelayMatch::Side T3SideA()
{
  RelayMatch::Side a;
  a.hold_out_ms = 60;
  a.hold_in_ms = 60;
  a.jitter_in_ms = 70;
  a.speed = 1.01;
  a.input_every = 40;
  return a;
}

RelayMatch::Side T3SideB()
{
  RelayMatch::Side b;
  b.input_every = 40;
  return b;
}
}  // namespace

TEST(OrcaSessionRelay, SlowJitteryLinkStallsRarely)
{
  // A's stalls come from late bursts on its way in, so only B's delay can help: B raises to 3, A
  // stays at 2, and B never steps back down into A's stalls.
  RelayMatch match(Config{}, T3SideA(), T3SideB());
  match.Play(1800);
  const int early_stalls = match.SessionOf(0).GetStats().stalls;
  match.Play(12000);
  match.Print("T3");
  match.ExpectClean();
  const Stats& a = match.SessionOf(0).GetStats();
  const Stats& b = match.SessionOf(1).GetStats();
  // Nearly all of A's stalls come in the first thirty seconds, before B's raise.
  EXPECT_LE(a.stalls, 30);
  EXPECT_LE(a.stalls - early_stalls, 6);
  EXPECT_LE(b.stalls, 5);
  // B raised on A's stalls; A's delay stays where the round trips put it.
  EXPECT_GE(b.delay_raises, 1);
  EXPECT_GE(match.SessionOf(1).Delay(), 3);
  EXPECT_EQ(b.delay_reverts, 0);
  EXPECT_EQ(a.delay_raises, 0);
  EXPECT_EQ(match.SessionOf(0).Delay(), 2);
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

TEST(OrcaSessionRelay, JitterBothWaysEitherSideFast)
{
  // A's jitter both ways, either clock fast: the fast side runs up to a frame ahead and stalls,
  // and the other side raises once the stalls are sustained. Most come in the first twenty
  // seconds; an occasional one after raises nothing.
  for (const bool a_fast : {true, false})
  {
    RelayMatch::Side a = T3SideA();
    RelayMatch::Side b = T3SideB();
    a.jitter_out_ms = 70;
    a.speed = a_fast ? 1.01 : 1.0;
    b.speed = a_fast ? 1.0 : 1.01;
    RelayMatch match(Config{}, a, b);
    match.Play(1200);
    const std::array<int, 2> early{match.SessionOf(0).GetStats().stalls,
                                   match.SessionOf(1).GetStats().stalls};
    match.Play(12000);
    match.Print(a_fast ? "both ways, A fast" : "both ways, B fast");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const int stalls = match.SessionOf(i).GetStats().stalls;
      EXPECT_LE(early[i], 60) << "a_fast " << a_fast << " side " << i;
      EXPECT_LE(stalls - early[i], 20) << "a_fast " << a_fast << " side " << i;
    }
    // The slow side's delay rises for the fast side's stalls (both rise for the link's median).
    EXPECT_GE(match.SessionOf(a_fast ? 1 : 0).Delay(), 4);
    EXPECT_GT(match.CompareConfirmed(), 11000);
  }
}

TEST(OrcaSessionRelay, LanWithHitchesTracksTheLink)
{
  // A fast link where both emulators hitch (B twice as often, as when compiling shaders). Stalls
  // from reported hitches don't count, so the delay stays on the link. Unreported (a control), the
  // stalls count but are too sparse to raise anything.
  RelayMatch::Side a, b;
  a.relay_ms = b.relay_ms = 14;
  a.speed = 1.01;
  a.input_every = b.input_every = 40;
  a.hitch_every_s = 12;
  b.hitch_every_s = 6;
  a.hitch_burst = b.hitch_burst = 4;
  {
    RelayMatch::Side quiet_a = a, quiet_b = b;
    quiet_a.report_hitches = quiet_b.report_hitches = false;
    RelayMatch unreported(Config{}, quiet_a, quiet_b);
    unreported.Play(12000);
    unreported.Print("LAN, hitches unreported");
    unreported.ExpectClean();
    EXPECT_GT(unreported.SessionOf(0).GetStats().counted_stalls, 15);
    for (int i = 0; i < 2; ++i)
      EXPECT_EQ(unreported.SessionOf(i).GetStats().delay_raises, 0) << "side " << i;
  }
  RelayMatch match(Config{}, a, b);
  match.Play(12000);
  match.Print("LAN, hitches");
  match.ExpectClean();
  const int hitches = match.HitchesOf(0) + match.HitchesOf(1);
  EXPECT_GT(match.HitchesOf(1), 25);
  for (int i = 0; i < 2; ++i)
  {
    const Stats& stats = match.SessionOf(i).GetStats();
    EXPECT_LE(match.SessionOf(i).Delay(), 3) << "side " << i;
    EXPECT_EQ(stats.delay_raises, 0) << "side " << i;
    EXPECT_EQ(stats.counted_stalls, 0) << "side " << i;
    // No stalls beyond the hitches.
    EXPECT_LE(stats.stalls, hitches + 1) << "side " << i;
  }
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

namespace
{
// A clean relay link between a Mac on Wi-Fi and a wired PC: short, slightly uneven round trips,
// clocks a hair apart, inputs changing every few frames. No spike reaches past the window.
RelayMatch::Side CleanMac()
{
  RelayMatch::Side mac;
  mac.relay_ms = 9;
  mac.jitter_in_ms = mac.jitter_out_ms = 8;
  mac.speed = 1.002;
  mac.input_every = 5;
  return mac;
}

RelayMatch::Side CleanPc()
{
  RelayMatch::Side pc;
  pc.relay_ms = 8;
  pc.jitter_in_ms = pc.jitter_out_ms = 3;
  pc.input_every = 5;
  return pc;
}

// The Mac's Wi-Fi on a bad day: its radio drops out both ways for 80-220 ms every 2-5 s.
RelayMatch::Side SpikyMac()
{
  RelayMatch::Side mac = CleanMac();
  mac.spike_gap_ms = {2000, 5000};
  mac.spike_len_ms = {80, 220};
  return mac;
}

// Every delay change both sides noted, for a failure's message.
std::string Notes(const RelayMatch& match)
{
  std::string out;
  for (int i = 0; i < 2; ++i)
  {
    for (const std::string& note : match.NotesOf(i))
      out += fmt::format("\n  {}: {}", static_cast<char>('A' + i), note);
  }
  return out;
}
}  // namespace

// A player drops into the host's game on a clean link, early, at character select or mid-match.
// The catch-up ends a dozen frames behind and the host stalls until time sync evens them out. That
// is not the link, so neither delay moves; the settle period starts when the joiner plugs in.
TEST(OrcaSessionRelay, DropInOnACleanLinkKeepsTwo)
{
  for (const int join_s : {10, 30, 75})
  {
    SCOPED_TRACE(::testing::Message() << "join at " << join_s << " s");
    for (const bool mac_hosts : {true, false})
    {
      SCOPED_TRACE(::testing::Message() << (mac_hosts ? "Mac hosts" : "PC hosts"));
      RelayMatch::DropIn drop_in;
      drop_in.join_s = join_s;
      // A slow machine replays under 2x speed, so it plugs in further behind.
      drop_in.catch_up_frame_us = mac_hosts ? 3000 : 9000;
      RelayMatch match(Config{}, mac_hosts ? CleanMac() : CleanPc(),
                       mac_hosts ? CleanPc() : CleanMac(), drop_in);
      match.PlaySeconds(join_s + 25);
      ASSERT_GE(match.PluggedFrame(), 0)
          << "the friend never plugged in: host "
          << (match.Running(0) ? match.SessionOf(0).Describe() : "not started") << "; friend "
          << (match.Running(1) ? match.SessionOf(1).Describe() + " " + match.SessionOf(1).Error() :
                                 "not started");
      match.MarkDelays();
      match.PlaySeconds(300);
      match.Print(fmt::format("drop-in at {} s, {}", join_s, mac_hosts ? "Mac hosts" : "PC hosts")
                      .c_str());
      match.ExpectClean();
      // The joiner stalls at the end of its catch-up, by design.
      EXPECT_GT(match.SessionOf(1).GetStats().stalls, 0);
      for (int i = 0; i < 2; ++i)
      {
        const Stats& stats = match.SessionOf(i).GetStats();
        EXPECT_EQ(stats.counted_stalls, 0) << "side " << i;
        EXPECT_EQ(stats.delay_raises, 0) << "side " << i << Notes(match);
        EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
        EXPECT_EQ(match.SessionOf(i).Delay(), 2) << "side " << i;
      }
      EXPECT_GT(match.SessionOf(0).GetStats().checksums_matched, 250);
      EXPECT_GT(match.CompareConfirmed(), 300 * 60);
    }
  }
}

// Wi-Fi dropouts of 80-220 ms every 2-5 s, mostly past the rollback window: each costs a stall.
// A frame more of delay would hide few of them while costing every input a frame, so both delays
// stay at 2, whichever side hosts.
TEST(OrcaSessionRelay, SpikesEveryFewSecondsKeepTwo)
{
  for (const bool mac_hosts : {true, false})
  {
    SCOPED_TRACE(::testing::Message() << (mac_hosts ? "Mac hosts" : "PC hosts"));
    RelayMatch::DropIn drop_in;
    drop_in.join_s = 45;
    RelayMatch match(Config{}, mac_hosts ? SpikyMac() : CleanPc(),
                     mac_hosts ? CleanPc() : SpikyMac(), drop_in);
    match.PlaySeconds(60);
    ASSERT_GE(match.PluggedFrame(), 0) << "the friend never plugged in";
    match.MarkDelays();
    match.PlaySeconds(300);
    match.Print(mac_hosts ? "Mac spikes, Mac hosts" : "Mac spikes, PC hosts");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const Stats& stats = match.SessionOf(i).GetStats();
      EXPECT_GT(stats.counted_stalls, 20) << "side " << i;
      EXPECT_EQ(stats.delay_raises, 0) << "side " << i << Notes(match);
      EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
    }
    EXPECT_GT(match.CompareConfirmed(), 300 * 60);
  }
}

// A fast clock keeps time sync waiting again and again. Those waits say nothing about the link,
// so no delay moves.
TEST(OrcaSessionRelay, SteadySmallWaitsRaiseNothing)
{
  RelayMatch::Side a = CleanMac();
  RelayMatch::Side b = CleanPc();
  a.speed = 1.015;
  a.jitter_in_ms = a.jitter_out_ms = 15;
  RelayMatch match(Config{}, a, b);
  match.Play(12000);
  match.Print("fast clock, jitter");
  match.ExpectClean();
  EXPECT_GT(match.SessionOf(0).GetStats().waits, 100);
  for (int i = 0; i < 2; ++i)
  {
    EXPECT_EQ(match.SessionOf(i).GetStats().delay_raises, 0) << "side " << i << Notes(match);
    EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i;
  }
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

// The same dropouts at random (Poisson, so they sometimes bunch up), every 3.5 s or 2.7 s on
// average. A frame more of delay would spare only the shortest, never eight seconds' worth in 45,
// so neither delay moves on any draw.
TEST(OrcaSessionRelay, SpikesAtRandomKeepTwo)
{
  for (const int mean_gap_ms : {3500, 2700})
  {
    for (const bool mac_hosts : {true, false})
    {
      for (u32 seed = 0; seed < 4; ++seed)
      {
        SCOPED_TRACE(::testing::Message() << "a spike every " << mean_gap_ms << " ms on average, "
                                          << (mac_hosts ? "Mac hosts" : "PC hosts") << ", draw "
                                          << seed);
        RelayMatch::Side mac = CleanMac();
        mac.spike_mean_gap_ms = mean_gap_ms;
        mac.spike_len_ms = {80, 220};
        RelayMatch::DropIn drop_in;
        drop_in.join_s = 45;
        RelayMatch match(Config{}, mac_hosts ? mac : CleanPc(), mac_hosts ? CleanPc() : mac,
                         drop_in, seed);
        match.PlaySeconds(60);
        ASSERT_GE(match.PluggedFrame(), 0) << "the friend never plugged in";
        match.MarkDelays();
        match.PlaySeconds(300);
        match.ExpectClean();
        for (int i = 0; i < 2; ++i)
        {
          const Stats& stats = match.SessionOf(i).GetStats();
          EXPECT_GT(stats.counted_stalls, 20) << "side " << i;
          EXPECT_EQ(stats.delay_raises + stats.delay_reverts, 0) << "side " << i << Notes(match);
          EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
        }
        EXPECT_GT(match.CompareConfirmed(), 300 * 60);
      }
    }
  }
}

// A spike every 2 s on average: a chance bunch may justify a raise to 3, which comes back down
// once quiet. At most once each way per side in five minutes, never above 3.
TEST(OrcaSessionRelay, SpikesAtRandomEveryTwoSecondsRarelyMove)
{
  for (const bool mac_hosts : {true, false})
  {
    for (u32 seed = 0; seed < 4; ++seed)
    {
      SCOPED_TRACE(::testing::Message() << (mac_hosts ? "Mac hosts" : "PC hosts") << ", draw "
                                        << seed);
      RelayMatch::Side mac = CleanMac();
      mac.spike_mean_gap_ms = 2000;
      mac.spike_len_ms = {80, 220};
      RelayMatch::DropIn drop_in;
      drop_in.join_s = 45;
      RelayMatch match(Config{}, mac_hosts ? mac : CleanPc(), mac_hosts ? CleanPc() : mac,
                       drop_in, seed);
      match.PlaySeconds(60);
      ASSERT_GE(match.PluggedFrame(), 0) << "the friend never plugged in";
      match.MarkDelays();
      match.PlaySeconds(300);
      match.ExpectClean();
      for (int i = 0; i < 2; ++i)
      {
        const Stats& stats = match.SessionOf(i).GetStats();
        EXPECT_LE(stats.delay_raises + stats.delay_reverts, 1) << "side " << i << Notes(match);
        EXPECT_LE(stats.delay_lowers, 1) << "side " << i << Notes(match);
        EXPECT_LE(match.MaxDelayOf(i), 3) << "side " << i << Notes(match);
      }
      EXPECT_GT(match.CompareConfirmed(), 300 * 60);
    }
  }
}

namespace
{
// A slow, bursty uplink (90 ms plus 0-120 ms of jitter, in order). The median round trip says
// delay 2, but the jitter delivers the Mac's inputs late in clusters, stalling the PC briefly.
RelayMatch::Side SlowUplinkMac()
{
  RelayMatch::Side mac = CleanMac();
  mac.hold_out_ms = 90;
  mac.jitter_out_ms = 120;
  return mac;
}
}  // namespace

// Sustained lateness the median misses: a frame more of the Mac's delay would spare nearly every
// PC stall, so the Mac rises to 3 and stays while the PC's delay never moves. With realistic
// re-run costs (3 ms a frame), a stall is timed to when its input came, not to the end of the
// rollback it set off, so the result is the same.
TEST(OrcaSessionRelay, ClusteredLatenessRaisesTheLateSide)
{
  for (const RelayMatch::Micros rerun_us : {0, 3000})
  {
    for (u32 seed = 0; seed < 4; ++seed)
    {
      SCOPED_TRACE(::testing::Message() << "draw " << seed << ", re-runs " << rerun_us << " us");
      RelayMatch::Side mac = SlowUplinkMac();
      RelayMatch::Side pc = CleanPc();
      mac.rerun_us = pc.rerun_us = rerun_us;
      RelayMatch match(Config{}, mac, pc, std::nullopt, seed);
      match.PlaySeconds(rerun_us ? 120 : 60);
      EXPECT_EQ(match.SessionOf(0).Delay(), 3) << Notes(match);
      match.MarkDelays();
      match.PlaySeconds(rerun_us ? 200 : 260);
      match.Print("slow, bursty uplink");
      match.ExpectClean();
      const Stats& mac_stats = match.SessionOf(0).GetStats();
      const Stats& pc_stats = match.SessionOf(1).GetStats();
      EXPECT_EQ(match.SessionOf(0).Delay(), 3) << Notes(match);
      EXPECT_EQ(mac_stats.delay_raises, 1) << Notes(match);
      EXPECT_EQ(mac_stats.delay_lowers + mac_stats.delay_reverts, 0) << Notes(match);
      EXPECT_EQ(pc_stats.delay_raises + pc_stats.delay_lowers + pc_stats.delay_reverts, 0)
          << Notes(match);
      EXPECT_EQ(match.MaxDelayOf(1), 2) << Notes(match);
      EXPECT_LE(pc_stats.stalls, 30);
      // Nearly all of the PC's counted stalls ended within a frame.
      EXPECT_GE(pc_stats.spared_stalls * 4, pc_stats.counted_stalls * 3);
      EXPECT_GT(match.CompareConfirmed(), 300 * 60);
    }
  }
}

// The same lateness for one minute: the Mac's delay rises, then returns to 2 within about ten
// seconds of the uplink clearing, and stays.
TEST(OrcaSessionRelay, SustainedLatenessRaisesThenComesBackDown)
{
  for (u32 seed = 0; seed < 4; ++seed)
  {
    SCOPED_TRACE(::testing::Message() << "draw " << seed);
    RelayMatch::Side mac = SlowUplinkMac();
    mac.hold_from_s = 60;
    mac.hold_to_s = 120;
    RelayMatch match(Config{}, mac, CleanPc(), std::nullopt, seed);
    match.PlaySeconds(60);
    match.MarkDelays();
    match.PlaySeconds(60);
    match.Print("a slow, bursty minute");
    EXPECT_EQ(match.MaxDelayOf(0), 3) << Notes(match);
    EXPECT_EQ(match.MaxDelayOf(1), 2) << Notes(match);
    match.PlaySeconds(12);
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      EXPECT_EQ(match.SessionOf(i).Delay(), 2) << "side " << i << Notes(match);
      EXPECT_EQ(match.SessionOf(i).GetStats().delay_reverts, 0) << "side " << i << Notes(match);
    }
    match.MarkDelays();
    match.PlaySeconds(60);
    for (int i = 0; i < 2; ++i)
      EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
    EXPECT_LE(match.SessionOf(1).GetStats().stalls, 30);
    EXPECT_GT(match.CompareConfirmed(), 180 * 60);
  }
}

// Dropouts a frame would cure: 130-150 ms every 2.1 s, just past the rollback window. A frame or
// two more of delay spares nearly all the stalls, so both delays rise and stay.
TEST(OrcaSessionRelay, DropoutsAFrameWouldSpareRaiseAndStay)
{
  for (u32 seed = 0; seed < 4; ++seed)
  {
    SCOPED_TRACE(::testing::Message() << "draw " << seed);
    RelayMatch::Side mac = CleanMac();
    mac.spike_gap_ms = {2100, 2100};
    mac.spike_len_ms = {130, 150};
    RelayMatch match(Config{}, mac, CleanPc(), std::nullopt, seed);
    match.PlaySeconds(300);
    match.Print("dropouts every 2.1 s");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const Stats& stats = match.SessionOf(i).GetStats();
      EXPECT_GE(match.SessionOf(i).Delay(), 3) << "side " << i << Notes(match);
      EXPECT_EQ(stats.delay_lowers + stats.delay_reverts, 0) << "side " << i << Notes(match);
      EXPECT_LE(stats.stalls, 40) << "side " << i;
    }
    EXPECT_GT(match.CompareConfirmed(), 290 * 60);
  }
}

// Long dropouts (150-300 ms) every second for a minute. A raise would spare few of them while
// costing every input a frame, so the delay stays at 2 and the stalls are taken as they come.
TEST(OrcaSessionRelay, LongDropoutsEverySecondRaiseNothing)
{
  for (u32 seed = 0; seed < 4; ++seed)
  {
    SCOPED_TRACE(::testing::Message() << "draw " << seed);
    RelayMatch::Side mac = CleanMac();
    mac.spike_gap_ms = {600, 1200};
    mac.spike_len_ms = {150, 300};
    mac.spike_from_s = 60;
    mac.spike_to_s = 120;
    RelayMatch match(Config{}, mac, CleanPc(), std::nullopt, seed);
    match.PlaySeconds(60);
    match.MarkDelays();
    match.PlaySeconds(75);
    match.Print("a minute of long dropouts");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      EXPECT_GT(match.SessionOf(i).GetStats().counted_stalls, 30) << "side " << i;
      EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
    }
    EXPECT_GT(match.CompareConfirmed(), 120 * 60);
  }
}

// Only the Mac's download stalls, so it stalls on the PC's inputs and the PC's delay rises. The PC
// stalls too, only because the stalled Mac stopped sending: those count for nothing.
TEST(OrcaSessionRelay, StallsSetOffByTheOtherSidesStallCountForNothing)
{
  RelayMatch::Side mac = CleanMac();
  mac.spike_gap_ms = {800, 1200};
  mac.spike_len_ms = {250, 300};
  mac.spike_in_only = true;
  mac.spike_from_s = 20;
  mac.spike_to_s = 80;
  RelayMatch match(Config{}, mac, CleanPc());
  match.PlaySeconds(90);
  match.Print("Mac download spikes");
  match.ExpectClean();
  const Stats& mac_stats = match.SessionOf(0).GetStats();
  const Stats& pc_stats = match.SessionOf(1).GetStats();
  EXPECT_GT(mac_stats.counted_stalls, 20);
  EXPECT_GT(pc_stats.stalls, 5);
  EXPECT_EQ(pc_stats.counted_stalls, 0);
  EXPECT_GE(match.MaxDelayOf(1), 3) << Notes(match);
  EXPECT_EQ(match.MaxDelayOf(0), 2) << Notes(match);
  EXPECT_EQ(mac_stats.delay_raises, 0) << Notes(match);
  // The Mac's delay may rise for its round trips, but never for the PC's stalls.
  for (const std::string& note : match.NotesOf(0))
    EXPECT_EQ(note.find("stalled"), std::string::npos) << note;
  EXPECT_GT(match.CompareConfirmed(), 80 * 60);
}

// Fuzz: random links, clock drift and wildly swinging round trips. Whatever the delays do, both
// machines must run every confirmed frame with identical inputs and states.
namespace
{
void FuzzDelaySwings(int snapshot_every)
{
  for (u32 seed = 1; seed <= 60; ++seed)
  {
    std::mt19937 rng(seed);
    const auto pick = [&rng](int lo, int hi) {
      return Uniform(rng, lo, hi);
    };
    Config config;
    config.snapshot_every = snapshot_every;
    config.input_delay = pick(0, 3);
    config.max_delay = config.input_delay + pick(0, 5);
    Match match(config, pick(0, 8), pick(0, 6));
    match.net.extra = {pick(0, 4), pick(0, 4)};
    match.a_extra_every = pick(0, 1) ? pick(20, 200) : 0;
    match.a->tag_ticks = true;
    match.b->tag_ticks = true;
    for (int chunk = 0; chunk < 60; ++chunk)
    {
      // Round trips from LAN to intercontinental, at random moments.
      for (Player* p : {match.a.get(), match.b.get()})
      {
        if (pick(0, 3) == 0)
          p->session.OnRoundTrip(pick(5, 450));
        // With automatic spacing, swing the save cost too so the spacing changes mid-match.
        if (snapshot_every == 0 && seed % 2 == 0 && pick(0, 3) == 0)
          p->game.save_ms = pick(0, 14);
      }
      match.Play(pick(5, 60));
    }
    match.Play(400);  // settle

    SCOPED_TRACE(::testing::Message() << "seed " << seed << ", snapshot every " << snapshot_every);
    ExpectClean(match);
    for (const Player* p : {match.a.get(), match.b.get()})
    {
      EXPECT_LE(p->session.GetStats().deepest_rollback,
                config.max_rollback +
                    (snapshot_every > 0 ? snapshot_every : MAX_SNAPSHOT_EVERY) - 1);
    }
    const int confirmed = std::min(Settled(*match.a), Settled(*match.b));
    ASSERT_GT(confirmed, 500);
    int checked = 0;
    for (int f = 0; f <= confirmed; ++f)
    {
      const auto ra = match.a->game.m_ran_pads.find(f);
      const auto rb = match.b->game.m_ran_pads.find(f);
      if (ra == match.a->game.m_ran_pads.end() || rb == match.b->game.m_ran_pads.end())
        continue;
      ASSERT_EQ(ra->second, rb->second) << "frame " << f;
      ++checked;
    }
    EXPECT_GT(checked, 300);
    EXPECT_GT(match.CompareConfirmed(), 300);
  }
}
}  // namespace

TEST(OrcaSessionFuzz, BothSidesRunIdenticalInputsUnderDelaySwings)
{
  FuzzDelaySwings(0);
}

// The same with sparse snapshots: rollbacks load a snapshot up to k - 1 frames before the first
// wrong frame, from a ring as small as RingPort's.
TEST(OrcaSessionFuzz, SparseSnapshotsKeepBothSidesIdentical)
{
  for (int k = 2; k <= MAX_SNAPSHOT_EVERY; ++k)
    FuzzDelaySwings(k);
}

// Every frame runs on a guess: k times fewer snapshots, same confirmed states and checksums.
TEST(OrcaSessionSparse, SavesDropByKUnderConstantPrediction)
{
  for (const bool still : {true, false})
  {
    std::array<double, MAX_SNAPSHOT_EVERY + 1> per_frame{};
    std::array<u64, MAX_SNAPSHOT_EVERY + 1> final_state{};
    for (int k = 1; k <= MAX_SNAPSHOT_EVERY; ++k)
    {
      SCOPED_TRACE(::testing::Message() << "still " << still << ", snapshot every " << k);
      Config config;
      config.snapshot_every = k;
      config.max_delay = config.input_delay;
      Match match(config, 5, 0);
      match.a->still = still;
      match.b->still = still;
      match.Play(3000);
      ExpectClean(match);
      EXPECT_GT(match.CompareConfirmed(), 2900);
      const Stats& stats = match.a->session.GetStats();
      EXPECT_GT(stats.checksums_matched, 40);
      EXPECT_EQ(stats.snapshot_every, k);
      EXPECT_LE(stats.deepest_rollback, config.max_rollback + k - 1);
      EXPECT_EQ(stats.rollbacks > 0, !still);
      per_frame[k] = static_cast<double>(stats.saves) / match.a->session.CurrentFrame();
      // The same inputs make the same game whatever the snapshot spacing.
      const auto late = match.a->game.m_history.find(2800);
      ASSERT_NE(late, match.a->game.m_history.end());
      final_state[k] = late->second;
      EXPECT_EQ(final_state[k], final_state[1]);
      std::printf("[ sparse %s k=%d ] %.3f saves per frame, %d rollbacks, %d re-run frames, "
                  "deepest %d\n",
                  still ? "still" : "moving", k, per_frame[k], stats.rollbacks,
                  stats.resimulated_frames, stats.deepest_rollback);
    }
    for (int k = 2; k <= MAX_SNAPSHOT_EVERY; ++k)
    {
      // Constant input: exactly every k-th frame, and a checksum frame now and then. Rollbacks
      // re-save on their re-runs, which k spreads out too.
      const double ratio = per_frame[1] / per_frame[k];
      EXPECT_GT(ratio, still ? 0.9 * k : 0.7 * k) << "k " << k;
    }
  }
}

// The first wrong frame falls between two snapshots: the rollback loads the newest snapshot before
// it and re-runs the frames in between with the inputs they had, which come out the same. Checked
// against a game run straight with the right inputs.
TEST(OrcaSessionSparse, RollbackBetweenSnapshotsReRunsFromTheOlderOne)
{
  // The remote input changes every 17 frames and arrives 4 frames late (input delay 2): each
  // change is a wrong guess at exactly that frame.
  constexpr int CHANGE_EVERY = 17;
  const auto remote = [](int f) { return Pad{static_cast<u8>((f / CHANGE_EVERY) % 4)}; };
  struct Lagging final : Transport
  {
    Lagging(std::function<Pad(int)> remote_pad, int lag_frames)
        : pad(std::move(remote_pad)), lag(lag_frames)
    {
    }
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      for (; next <= session->CurrentFrame() - lag; ++next)
        packet.pads.push_back(pad(next));
      // An honest peer's packet carries its inputs up to its own frame (and its delay): it was at
      // that frame when it sent them, `lag` frames ago.
      packet.current_frame = std::max(0, session->CurrentFrame() - lag);
      return {packet};
    }
    bool Connected() const override { return true; }
    std::function<Pad(int)> pad;
    int lag;
    Session* session = nullptr;
    int next = 0;
  };
  // Records, at every load, the snapshots the game held just before it.
  struct Recording final : Game
  {
    bool Save(int frame) override { return game.Save(frame); }
    bool Load(int frame) override
    {
      std::set<int> held;
      for (const auto& [f, state] : game.m_snapshots)
        held.insert(f);
      held_at_load.push_back(std::move(held));
      return game.Load(frame);
    }
    void SetPads(int frame, const Pads& pads) override { game.SetPads(frame, pads); }
    void SetResimulating(bool resimulating) override { game.SetResimulating(resimulating); }
    std::optional<u64> SnapshotChecksum(int frame) override { return game.SnapshotChecksum(frame); }
    void ResetPacing() override { game.ResetPacing(); }
    FakeGame game;
    std::vector<std::set<int>> held_at_load;
  };

  // 4 frames late; and max_rollback late, where rollbacks reach max_rollback + k - 1 frames back
  // through a ring as small as RingPort's.
  for (const int LAG : {4, Config{}.max_rollback})
  for (int k = 2; k <= MAX_SNAPSHOT_EVERY; ++k)
  {
    SCOPED_TRACE(::testing::Message() << "lag " << LAG << ", snapshot every " << k);
    Recording game;
    Lagging net(remote, LAG);
    Config config;
    config.time_sync = false;
    config.max_delay = config.input_delay;
    config.snapshot_every = k;
    game.game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
    Session session(config, game, net);
    net.session = &session;
    int completed = -1;
    for (int i = 0; i < 1200; ++i)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
      if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
        completed = game.game.Run();
    }
    const Stats& stats = session.GetStats();
    const std::vector<std::pair<int, u64>>& loads = game.game.m_loads;
    ASSERT_EQ(loads.size(), static_cast<size_t>(stats.rollbacks));
    ASSERT_GT(loads.size(), 40u);

    FakeGame straight;
    for (int f = 0; f <= stats.confirmed_frame; ++f)
    {
      straight.SetPads(f, Pads{Pad{}, remote(f)});
      straight.Run();
    }
    const auto state_at = [](const FakeGame& g, int f) {
      const auto it = g.m_history.find(f);
      return it == g.m_history.end() ? std::optional<u64>() : it->second;
    };
    int before_wrong = 0;
    for (size_t i = 0; i < loads.size(); ++i)
    {
      // The i-th rollback is the (i + 1)-th change.
      const int wrong = static_cast<int>(i + 1) * CHANGE_EVERY;
      const int loaded = loads[i].first;
      const std::set<int>& held = game.held_at_load[i];
      EXPECT_LE(loaded, wrong);
      EXPECT_LE(wrong - loaded, k - 1) << "wrong frame " << wrong;
      // The newest snapshot at or before the wrong frame, not just any.
      const auto newer = held.upper_bound(wrong);
      EXPECT_EQ(newer == held.begin() ? -1 : *std::prev(newer), loaded) << "wrong frame " << wrong;
      EXPECT_EQ(loads[i].second, state_at(straight, loaded)) << "load of frame " << loaded;
      before_wrong += loaded < wrong;
    }
    // Most changes land between snapshots (k - 1 of every k frames are not saved).
    EXPECT_GT(before_wrong, static_cast<int>(loads.size()) / 3);
    EXPECT_EQ(stats.deepest_rollback, LAG + k - 1);
    EXPECT_EQ(stats.stalls, 0);
    std::printf("[ between lag %d k=%d ] %zu rollbacks, %d loaded a snapshot before the wrong "
                "frame\n",
                LAG, k, loads.size(), before_wrong);
    EXPECT_EQ(stats.resimulated_frames - stats.rollbacks * LAG, [&] {
      int extra = 0;
      for (size_t i = 0; i < loads.size(); ++i)
        extra += static_cast<int>(i + 1) * CHANGE_EVERY - loads[i].first;
      return extra;
    }());
    int compared = 0;
    for (int f = 0; f <= stats.confirmed_frame; ++f, ++compared)
      EXPECT_EQ(state_at(game.game, f), state_at(straight, f)) << "frame " << f;
    EXPECT_GT(compared, 700);
  }
}

// The spacing follows what a save costs on this machine.
TEST(OrcaSessionSparse, SpacingFollowsTheMeasuredSaveTime)
{
  for (const auto& [save_ms, k] : std::vector<std::pair<double, int>>{
           {0.0, 1}, {1.7, 1}, {2.9, 1}, {3.5, 2}, {6.0, 2}, {6.4, 3}, {9.4, 4}, {40.0, 4}})
  {
    SCOPED_TRACE(::testing::Message() << save_ms << " ms");
    Config config;
    Match match(config, 5, 0);
    match.a->game.save_ms = save_ms;
    match.Play(130);  // two windows: the spacing is re-chosen once a window
    ExpectClean(match);
    EXPECT_EQ(match.a->session.GetStats().snapshot_every, k);
    EXPECT_EQ(match.b->session.GetStats().snapshot_every, 1);
    // A fixed spacing (ORCA_TEST_SNAPSHOT_EVERY) wins over the measurement.
    config.snapshot_every = 2;
    Match fixed(config, 5, 0);
    fixed.a->game.save_ms = save_ms;
    fixed.Play(130);
    EXPECT_EQ(fixed.a->session.GetStats().snapshot_every, 2);
  }
  // A save time that drifts across a step moves the spacing only once it is clear of it.
  Match match(Config{}, 5, 0);
  for (const auto& [save_ms, k] : std::vector<std::pair<double, int>>{
           {6.4, 3}, {6.1, 3}, {5.9, 3}, {5.7, 2}, {6.2, 2}, {6.3, 3}, {0.5, 1}})
  {
    SCOPED_TRACE(::testing::Message() << save_ms << " ms");
    match.a->game.save_ms = save_ms;
    match.Play(60);
    EXPECT_EQ(match.a->session.GetStats().snapshot_every, k);
  }
  ExpectClean(match);
}

// ---- Drop-in ----

namespace
{
// map.at() without exceptions (they're off): a missing frame fails the test instead of aborting.
template <typename T>
T At(const std::map<int, T>& map, int frame)
{
  const auto it = map.find(frame);
  if (it == map.end())
  {
    ADD_FAILURE() << "no frame " << frame;
    return T{};
  }
  return it->second;
}

// A drop-in player: its session over a FakeGame, stepping up to `frames` new frames per tick (a
// joiner catching up runs many).
struct DropInPlayer
{
  DropInPlayer(const Config& config, FakeNet& net, int index, int start)
      : transport(net, index), session(config, game, transport), completed(start - 1)
  {
    game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
  }
  Pad LocalPad(int tick) const
  {
    Pad pad{};
    pad[0] = static_cast<u8>((tick / 5 + seat * 3) % 4);
    pad[2] = static_cast<u8>(tick & 0xff);
    pad[4] = static_cast<u8>(1 + seat);
    return pad;
  }
  int Tick(int tick, int frames)
  {
    int ran = 0;
    for (int guard = 0; guard < 4000 && ran < frames; ++guard)
    {
      const Step step = session.OnBoundary(completed, LocalPad(tick));
      if (step.kind == StepKind::Ended || step.kind == StepKind::Stall ||
          step.kind == StepKind::Wait)
      {
        return ran;
      }
      const bool new_frame = step.kind == StepKind::Run && !session.Resimulating();
      completed = game.Run();
      if (new_frame)
        ++ran;
    }
    return ran;
  }
  FakeGame game;
  FakeNet::End transport;
  Session session;
  int completed;
  int seat = 0;
};

Config DropInConfig(int local_seat, int start_frame)
{
  Config config;
  config.seats = MAX_SEATS;
  config.local_seat = local_seat;
  config.authority_seat = 0;
  config.start_frame = start_frame;
  std::array<SeatPlan, MAX_SEATS> plan{};
  plan[0] = {0, NEVER, 0};
  config.plan = plan;
  return config;
}

// Plays the host's solo frames [from, from + frames) on `game`, appending them to `log`.
std::vector<Pads> PlaySolo(FakeGame* game, int from, int frames, std::vector<Pads> log = {})
{
  for (int f = from; f < from + frames; ++f)
  {
    Pads pads;
    pads.fill(UNPLUGGED_PAD);
    pads[0] = Pad{static_cast<u8>((f / 9) % 4), 0, static_cast<u8>(f & 0xff), 0, 9, 0, 0, 0};
    game->SetPads(f, pads);
    game->Run();
    log.push_back(pads);
  }
  return log;
}
}  // namespace

TEST(OrcaPadCodec, UnpluggedIsNotAButton)
{
  EXPECT_TRUE(IsUnplugged(UNPLUGGED_PAD));
  EXPECT_FALSE(IsUnplugged(Pad{}));
  GCPadStatus held;
  held.button = 0xFFFF;
  EXPECT_FALSE(IsUnplugged(EncodePad(held)));
  EXPECT_FALSE(DecodePad(UNPLUGGED_PAD).isConnected);
  EXPECT_TRUE(DecodePad(Pad{}).isConnected);
}

TEST(OrcaDropIn, PacketFieldsRoundTrip)
{
  Packet sent;
  sent.seat = 0;
  sent.current_frame = 900;
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster[0] = {0, NEVER, 0};
  roster[1] = {950, 1200, 900};
  sent.roster = roster;
  sent.history_ack = 899;
  sent.history_seat = 1;
  sent.history_first = 600;
  Pads a;
  a.fill(UNPLUGGED_PAD);
  a[0] = Pad{1, 2, 3, 4, 5, 6, 7, 8};
  Pads b = a;
  b[0][0] = 9;
  sent.history = {a, a, a, b, a};
  sent.leaving = true;
  Packet got;
  ASSERT_TRUE(DecodePacket(EncodePacket(sent), &got));
  ASSERT_TRUE(got.roster);
  EXPECT_EQ(*got.roster, roster);
  EXPECT_EQ(got.history_ack, 899);
  EXPECT_EQ(got.history_seat, 1);
  EXPECT_EQ(got.history_first, 600);
  EXPECT_EQ(got.history, sent.history);
  EXPECT_TRUE(got.leaving);
  // Runs of equal pads travel once.
  EXPECT_NE(EncodePacket(sent).find("[3,"), std::string::npos);

  // Coalescing keeps history that continues.
  std::optional<Packet> pending;
  Packet first = sent;
  first.history = {a, a};
  Packet second = sent;
  second.history_first = 602;
  second.history = {b};
  CoalescePacket(&pending, first);
  CoalescePacket(&pending, second);
  EXPECT_EQ(pending->history_first, 600);
  EXPECT_EQ(pending->history.size(), 3u);
}

// The host plays solo; a friend loads the keyframe of frame 240, replays the host's frames since,
// plugs in, both run identical frames with matching checksums, the friend leaves and the host plays
// on alone; then a second friend joins from a later keyframe.
TEST(OrcaDropIn, FriendJoinsPlaysLeavesAndAnotherJoins)
{
  for (const auto& [latency, jitter] : std::vector<std::pair<int, int>>{{1, 0}, {4, 2}, {8, 4}})
  {
    SCOPED_TRACE(::testing::Message() << "latency " << latency << " jitter " << jitter);
    FakeNet net;
    net.latency = latency;
    net.jitter = jitter;
    FakeGame solo;
    std::vector<Pads> log = PlaySolo(&solo, 0, 300);
    const int keyframe = 240;

    auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 300), net, 0, 300);
    host->game.m_state = solo.m_state;
    host->game.m_history = solo.m_history;
    host->game.m_ran_pads = solo.m_ran_pads;
    host->session.SetLog(0, log);
    host->session.AddPeer(1, 300, keyframe);
    EXPECT_EQ(host->session.Plan()[0], (SeatPlan{0, NEVER, 0})) << host->session.Describe();
    auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe), net, 1, keyframe);
    joiner->seat = 1;
    joiner->game.m_state = At(solo.m_history, keyframe);

    int t = 0;
    int plugged_at = -1;
    for (; t < 1200; ++t)
    {
      net.now = t;
      // Round trips to the relay every half second, as Match's rtt_samples.
      if (t % 30 == 0)
      {
        for (DropInPlayer* p : {host.get(), joiner.get()})
          p->session.OnRoundTrip((latency + jitter / 2) * 1000 / 60);
      }
      host->Tick(t, 1);
      // Unthrottled while behind, as RingPort::SetCatchingUp makes it.
      const bool behind = joiner->session.CatchingUp() ||
                          joiner->session.AuthorityFrame() - joiner->session.CurrentFrame() > 12;
      joiner->Tick(t, behind ? 40 : 1);
      ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
      ASSERT_TRUE(joiner->session.Error().empty()) << joiner->session.Error();
      if (plugged_at < 0 && !joiner->session.CatchingUp())
        plugged_at = host->session.Plan()[1].plug_from;
    }
    ASSERT_GE(plugged_at, 300);
    // The host's own port stays plugged in throughout.
    for (int f = 300; f < host->session.CurrentFrame(); ++f)
      ASSERT_FALSE(IsUnplugged(At(host->game.m_ran_pads, f)[0])) << "frame " << f;
    // Plugged in on both machines from exactly frame S, unplugged before.
    for (const FakeGame* game : {&host->game, &joiner->game})
    {
      EXPECT_TRUE(IsUnplugged(At(game->m_ran_pads, plugged_at - 1)[1]));
      EXPECT_FALSE(IsUnplugged(At(game->m_ran_pads, plugged_at)[1]));
      EXPECT_TRUE(IsUnplugged(At(game->m_ran_pads, plugged_at)[2]));
    }
    // Every frame both confirmed: the same state and pads on both, from the keyframe on.
    const int confirmed =
        std::min({host->session.GetStats().confirmed_frame, host->session.CurrentFrame() - 1,
                  joiner->session.GetStats().confirmed_frame, joiner->session.CurrentFrame() - 1});
    ASSERT_GT(confirmed, plugged_at + 300);
    for (int f = keyframe; f <= confirmed; ++f)
    {
      ASSERT_EQ(At(host->game.m_history, f), At(joiner->game.m_history, f)) << "frame " << f;
      ASSERT_EQ(At(host->game.m_ran_pads, f), At(joiner->game.m_ran_pads, f)) << "frame " << f;
    }
    EXPECT_GT(host->session.GetStats().checksums_matched, 10);
    EXPECT_GT(joiner->session.GetStats().checksums_matched, 10);
    // The joiner stalls while catching up (it runs only confirmed frames), which is not the
    // link, so neither delay moves.
    if (latency == 1)
    {
      EXPECT_GT(joiner->session.GetStats().stalls, 0);
      EXPECT_EQ(joiner->session.GetStats().counted_stalls, 0);
      for (const DropInPlayer* p : {host.get(), joiner.get()})
        EXPECT_EQ(p->session.GetStats().delay_raises, 0);
    }

    // The friend leaves; its port unplugs at a frame both agree on and the host goes idle.
    joiner->session.RequestLeave();
    for (int end = t + 200; t < end && !(joiner->session.LeftDone() && host->session.Idle()); ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      if (!joiner->session.LeftDone())
        joiner->Tick(t, 1);
    }
    ASSERT_TRUE(joiner->session.LeftDone());
    ASSERT_TRUE(host->session.Idle());
    const int unplug = host->session.Plan()[1].unplug_from;
    ASSERT_NE(unplug, NEVER);
    EXPECT_EQ(joiner->session.Plan()[1].unplug_from, unplug);
    EXPECT_FALSE(IsUnplugged(At(host->game.m_ran_pads, unplug - 1)[1]));
    EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, unplug)[1]));
    for (int f = keyframe; f < unplug; ++f)
      ASSERT_EQ(At(host->game.m_history, f), At(joiner->game.m_history, f)) << "frame " << f;

    // Back to solo: the log holds every frame the host ran, as it ran them.
    int base = -1;
    std::vector<Pads> host_log = host->session.TakeLog(&base);
    const int next = host->session.CurrentFrame();
    ASSERT_EQ(base, 0);
    ASSERT_EQ(static_cast<int>(host_log.size()), next);
    for (int f = 0; f < next; ++f)
      ASSERT_EQ(host_log[f], At(host->game.m_ran_pads, f)) << "frame " << f;
    const std::vector<Pad> pending = host->session.PendingLocal();
    host_log = PlaySolo(&host->game, next, 400, host_log);
    // (A real host plays the pending inputs first; this one's solo pads are its own.)
    (void)pending;

    // A second friend joins from a keyframe of the solo frames after.
    const int keyframe2 = next + 300;
    const int start2 = next + 400;
    FakeNet net2;
    net2.latency = latency;
    net2.jitter = jitter;
    auto host2 = std::make_unique<DropInPlayer>(DropInConfig(0, start2), net2, 0, start2);
    host2->game.m_state = host->game.m_state;
    host2->game.m_history = host->game.m_history;
    host2->game.m_ran_pads = host->game.m_ran_pads;
    host2->session.SetLog(0, host_log);
    host2->session.AddPeer(1, start2, keyframe2);
    auto joiner2 = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe2), net2, 1, keyframe2);
    joiner2->seat = 1;
    joiner2->game.m_state = At(host->game.m_history, keyframe2);
    for (int u = 0; u < 900; ++u)
    {
      net2.now = u;
      if (u % 30 == 0)
      {
        for (DropInPlayer* p : {host2.get(), joiner2.get()})
          p->session.OnRoundTrip((latency + jitter / 2) * 1000 / 60);
      }
      host2->Tick(u, 1);
      const bool behind = joiner2->session.CatchingUp() ||
                          joiner2->session.AuthorityFrame() - joiner2->session.CurrentFrame() > 12;
      joiner2->Tick(u, behind ? 40 : 1);
      ASSERT_TRUE(host2->session.Error().empty()) << host2->session.Error();
      ASSERT_TRUE(joiner2->session.Error().empty()) << joiner2->session.Error();
    }
    EXPECT_FALSE(joiner2->session.CatchingUp());
    EXPECT_GT(host2->session.GetStats().checksums_matched, 5);
    const int confirmed2 =
        std::min({host2->session.GetStats().confirmed_frame, host2->session.CurrentFrame() - 1,
                  joiner2->session.GetStats().confirmed_frame, joiner2->session.CurrentFrame() - 1});
    EXPECT_GT(confirmed2, start2 + 300);
    for (int f = keyframe2; f <= confirmed2; ++f)
      ASSERT_EQ(At(host2->game.m_history, f), At(joiner2->game.m_history, f)) << "frame " << f;
  }
}

// A friend whose connection drops: the host unplugs it after its last input and plays on, rolling
// back the frames that guessed otherwise.
TEST(OrcaDropIn, DroppedFriendUnplugsAfterItsLastInput)
{
  FakeNet net;
  net.latency = 4;
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 120);
  auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 120), net, 0, 120);
  host->game.m_state = solo.m_state;
  host->session.SetLog(0, log);
  host->session.AddPeer(1, 120, 60);
  auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, 60), net, 1, 60);
  joiner->seat = 1;
  joiner->game.m_state = At(solo.m_history, 60);
  int t = 0;
  for (; t < 400; ++t)
  {
    net.now = t;
    host->Tick(t, 1);
    joiner->Tick(t, joiner->session.CatchingUp() ? 40 : 1);
  }
  ASSERT_FALSE(joiner->session.CatchingUp());
  // Gone: nothing more from it.
  host->session.DropSeat(1);
  for (int end = t + 60; t < end; ++t)
  {
    net.now = t;
    host->Tick(t, 1);
  }
  ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
  EXPECT_TRUE(host->session.Idle());
  const int unplug = host->session.Plan()[1].unplug_from;
  EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, unplug)[1]));
  EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, host->session.CurrentFrame() - 1)[1]));
}

// A friend whose room connection drops comes back to the same seat of the same room (bug reports
// 6afb0948, 22ba6446, 96e56887: Orca 0.3.30 and 0.3.34, a ranked set's joiner each time). The
// host's packets reach the friend's new connection before the host takes it in again, and their
// roster still names the first stay for that seat: as it was while the host waited on the friend
// (a drop the room hears only when the friend's next connection replaces the dead one), or with
// the seat unplugged after the drop. Neither is about this stay: the friend plugs in again as any
// newcomer does, and both run the same frames.
TEST(OrcaDropIn, FriendBackInTheSameSeatAfterADrop)
{
  for (const bool heard_late : {false, true})
  {
    SCOPED_TRACE(heard_late ? "the drop heard as the friend comes back" : "the drop heard at once");
    FakeNet net;
    net.latency = 4;
    FakeGame solo;
    std::vector<Pads> log = PlaySolo(&solo, 0, 120);
    auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 120), net, 0, 120);
    host->game.m_state = solo.m_state;
    host->game.m_history = solo.m_history;
    host->game.m_ran_pads = solo.m_ran_pads;
    host->session.SetLog(0, log);
    host->session.AddPeer(1, 120, 60);
    auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, 60), net, 1, 60);
    joiner->seat = 1;
    joiner->game.m_state = At(solo.m_history, 60);
    int t = 0;
    for (; t < 400; ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      joiner->Tick(t, joiner->session.CatchingUp() ? 40 : 1);
    }
    ASSERT_FALSE(joiner->session.CatchingUp());
    const int first_plug = host->session.Plan()[1].plug_from;
    ASSERT_NE(first_plug, NEVER);

    // The connection drops: what was on its way to the friend is lost.
    joiner.reset();
    const auto quiet = [&](int ticks) {
      for (int end = t + ticks; t < end; ++t)
      {
        net.now = t;
        host->Tick(t, 1);
        net.queues[1].clear();
      }
    };
    // The host's packets reach the friend's new connection.
    const auto heard = [&](int ticks) {
      for (int end = t + ticks; t < end; ++t)
      {
        net.now = t;
        host->Tick(t, 1);
      }
    };
    if (heard_late)
    {
      // The host waits on the friend (its last inputs), then the new connection replaces the dead
      // one and the host hears the drop a few frames later.
      quiet(60);
      heard(6);
      ASSERT_NE(host->session.Plan()[1].live_from, NEVER);
      host->session.DropSeat(1);
      heard(30);
    }
    else
    {
      host->session.DropSeat(1);
      quiet(60);
      heard(30);
    }
    ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
    ASSERT_TRUE(host->session.Idle());
    ASSERT_EQ(host->session.Plan()[1].plug_from, first_plug);
    ASSERT_FALSE(net.queues[1].empty());
    // The host takes the friend in again from a keyframe of a final frame; the friend's session
    // starts once its rebuild is done.
    const int keyframe = host->session.CurrentFrame() - 1;
    const int next = host->session.CurrentFrame();
    host->session.AddPeer(1, next, keyframe);
    heard(20);
    auto back = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe), net, 1, keyframe);
    back->seat = 1;
    back->game.m_state = At(host->game.m_history, keyframe);
    for (int end = t + 600; t < end; ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      back->Tick(t, back->session.CatchingUp() ? 40 : 1);
      ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
      ASSERT_TRUE(back->session.Error().empty()) << back->session.Error();
    }
    ASSERT_FALSE(back->session.CatchingUp()) << back->session.Describe();
    const int plugged_again = host->session.Plan()[1].plug_from;
    EXPECT_GE(plugged_again, next);
    EXPECT_EQ(back->session.Plan()[1].plug_from, plugged_again);
    const int confirmed =
        std::min({host->session.GetStats().confirmed_frame, host->session.CurrentFrame() - 1,
                  back->session.GetStats().confirmed_frame, back->session.CurrentFrame() - 1});
    ASSERT_GT(confirmed, plugged_again + 120);
    for (int f = keyframe; f <= confirmed; ++f)
    {
      ASSERT_EQ(At(host->game.m_history, f), At(back->game.m_history, f)) << "frame " << f;
      ASSERT_EQ(At(host->game.m_ran_pads, f), At(back->game.m_ran_pads, f)) << "frame " << f;
    }
    EXPECT_FALSE(IsUnplugged(At(host->game.m_ran_pads, plugged_again)[1]));
    EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, plugged_again - 1)[1]));
    EXPECT_GT(back->session.GetStats().checksums_matched, 2);
  }
}

// Session::RequireValues: a newcomer plugs in only once every player, itself included, has
// acknowledged the port values it plugs in with.
TEST(OrcaDropIn, PlugInWaitsForEveryPlayerToHoldThePortValues)
{
  struct Inject final : Transport
  {
    void Send(const Packet& packet) override { sent.push_back(packet); }
    std::vector<Packet> Receive() override { return std::exchange(queue, {}); }
    bool Connected() const override { return true; }
    std::vector<Packet> queue;
    std::vector<Packet> sent;
  };
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 300);
  Inject net;
  FakeGame game;
  game.m_state = solo.m_state;
  game.m_history = solo.m_history;
  game.m_ran_pads = solo.m_ran_pads;
  Session host(DropInConfig(0, 300), game, net);
  host.SetLog(0, log);
  // Two friends replay from the keyframe of frame 240; seat 3's values came later (version 5).
  host.AddPeer(1, 300, 240);
  host.AddPeer(2, 300, 240);
  host.RequireValues(1, 4);
  host.RequireValues(2, 5);
  int completed = 299;
  std::array<int, MAX_SEATS> sequence{};
  // Both friends caught up (history acknowledged, at the host's frame), holding these versions.
  const auto boundary = [&](int ack1, int ack2) {
    for (const auto& [seat, ack] : {std::pair(1, ack1), std::pair(2, ack2)})
    {
      Packet p;
      p.seat = seat;
      p.sequence = ++sequence[seat];
      p.current_frame = host.CurrentFrame();
      p.history_ack = host.CurrentFrame() - 1;
      p.values_ack = ack;
      net.queue.push_back(p);
    }
    const Step step = host.OnBoundary(completed, Pad{});
    EXPECT_EQ(step.kind, StepKind::Run) << host.Error();
    completed = game.Run();
  };
  // Seat 2 has nobody's acknowledgement of version 5 yet; seat 1 has everyone's version 4.
  for (int i = 0; i < 3; ++i)
    boundary(4, 4);
  EXPECT_NE(host.Plan()[1].plug_from, NEVER) << host.Describe();
  EXPECT_EQ(host.Plan()[2].plug_from, NEVER) << host.Describe();
  // The newcomer holds them, the friend already in doesn't: still not.
  for (int i = 0; i < 3; ++i)
    boundary(4, 5);
  EXPECT_EQ(host.Plan()[2].plug_from, NEVER) << host.Describe();
  // An acknowledgement never goes back: a stale packet saying 4 changes nothing once 5 is in.
  boundary(5, 4);
  const int plugged = host.Plan()[2].plug_from;
  EXPECT_NE(plugged, NEVER) << host.Describe();
  EXPECT_GT(plugged, host.CurrentFrame());
  // The roster the host sends carries it from then on.
  ASSERT_FALSE(net.sent.empty());
  ASSERT_TRUE(net.sent.back().roster);
  EXPECT_EQ((*net.sent.back().roster)[2].plug_from, plugged);
  EXPECT_TRUE(host.Error().empty()) << host.Error();
}

// A player's packets acknowledge the newest values version it holds, and only ever more.
TEST(OrcaDropIn, PlayersAcknowledgeTheValuesTheyHold)
{
  FakeNet net;
  DropInPlayer joiner(DropInConfig(1, 40), net, 1, 40);
  joiner.seat = 1;
  FakeNet::End host_end(net, 0);
  joiner.session.SetValuesHeld(3);
  joiner.session.SetValuesHeld(2);
  joiner.Tick(0, 1);
  net.now = 1000;
  const std::vector<Packet> got = host_end.Receive();
  ASSERT_FALSE(got.empty());
  EXPECT_EQ(got.back().values_ack, 3);
  // It survives the wire.
  Packet decoded;
  ASSERT_TRUE(DecodePacket(EncodePacket(got.back()), &decoded));
  EXPECT_EQ(decoded.values_ack, 3);
  Packet none;
  ASSERT_TRUE(DecodePacket(EncodePacket(Packet{}), &none));
  EXPECT_EQ(none.values_ack, -1);
}

// The session may run on the CPU thread in the guest's non-IEEE float mode (flush-to-zero); the
// roster must still hold exactly what was set.
TEST(OrcaDropIn, PlanSurvivesTheGuestFloatMode)
{
  Common::FPU::SetSIMDMode(Common::FPU::ROUND_NEAR, true);
  FakeNet net;
  DropInPlayer host(DropInConfig(0, 100), net, 0, 100);
  host.session.AddPeer(1, 100, 40);
  const std::array<SeatPlan, MAX_SEATS> plan = host.session.Plan();
  Common::FPU::LoadDefaultSIMDState();
  EXPECT_EQ(plan[0], (SeatPlan{0, NEVER, 0}));
  EXPECT_EQ(plan[1], (SeatPlan{NEVER, NEVER, 100}));
  EXPECT_EQ(plan[2], SeatPlan{});
}

// ---- Rosters and history outside the drop-in rules ----

namespace
{
// A pad no player in these tests holds otherwise.
constexpr Pad SCRIPTED_PAD{0x03, 0x00, 0xEE, 0x00, 0x7F, 0x00, 0x00, 0x00};

// A drop-in up to the joiner playing live, after which the test writes the host's (seat 0) packets
// itself.
struct ScriptedHost
{
  ScriptedHost()
  {
    net.latency = 4;
    FakeGame solo;
    std::vector<Pads> log = PlaySolo(&solo, 0, 300);
    host = std::make_unique<DropInPlayer>(DropInConfig(0, 300), net, 0, 300);
    host->game.m_state = solo.m_state;
    host->game.m_history = solo.m_history;
    host->game.m_ran_pads = solo.m_ran_pads;
    host->session.SetLog(0, log);
    host->session.AddPeer(1, 300, KEYFRAME);
    joiner = std::make_unique<DropInPlayer>(DropInConfig(1, KEYFRAME), net, 1, KEYFRAME);
    joiner->seat = 1;
    joiner->game.m_state = At(solo.m_history, KEYFRAME);
  }
  // Both play until the joiner has played `live` frames of its own.
  void PlayUntilLive(int live)
  {
    for (; t < 3000; ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      const bool behind = joiner->session.CatchingUp() ||
                          joiner->session.AuthorityFrame() - joiner->session.CurrentFrame() > 12;
      joiner->Tick(t, behind ? 40 : 1);
      ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
      ASSERT_TRUE(joiner->session.Error().empty()) << joiner->session.Error();
      if (!joiner->session.CatchingUp() &&
          joiner->session.CurrentFrame() > joiner->session.Plan()[1].plug_from + live)
      {
        break;
      }
    }
    ASSERT_FALSE(joiner->session.CatchingUp());
    roster = host->session.Plan();
    from_frame = joiner->session.CurrentFrame();
    // From here on the host's own session is silent; nothing it sent is still on the way.
    net.queues[1].clear();
  }
  Packet Scripted(std::optional<std::array<SeatPlan, MAX_SEATS>> with_roster) const
  {
    Packet p;
    p.seat = 0;
    p.first_frame = 0;
    p.ack = {-1, joiner->session.CurrentFrame() - 1, -1, -1};
    p.current_frame = joiner->session.CurrentFrame();
    p.roster = with_roster;
    return p;
  }
  // History for the joiner from its log's end on, with the scripted pad in the joiner's seat.
  Packet ScriptedHistory(std::optional<std::array<SeatPlan, MAX_SEATS>> with_roster,
                       int first = -1) const
  {
    Packet p = Scripted(with_roster);
    p.history_seat = 1;
    p.history_first = first >= 0 ? first : joiner->session.LogEnd();
    for (int i = 0; i < MAX_HISTORY_IN_PACKET; ++i)
    {
      Pads pads;
      pads.fill(UNPLUGGED_PAD);
      pads[0] = Pad{0, 0, 0, 0, 9, 0, 0, 0};
      pads[1] = SCRIPTED_PAD;
      p.history.push_back(pads);
    }
    return p;
  }
  // Delivers `packet` to the joiner and lets it step once.
  void Send(Packet packet)
  {
    packet.sequence = ++sequence;
    net.now = t;
    net.queues[1].push_back({t, std::move(packet)});
    joiner->Tick(t, 1);
    net.queues[0].clear();
    ++t;
  }
  // Frames from from_frame on that the joiner ran with the scripted pad in its own seat.
  int ScriptedFrames() const
  {
    int n = 0;
    for (int f = from_frame; f < joiner->session.CurrentFrame(); ++f)
    {
      const auto it = joiner->game.m_ran_pads.find(f);
      if (it != joiner->game.m_ran_pads.end() && it->second[1] == SCRIPTED_PAD)
        ++n;
    }
    return n;
  }

  static constexpr int KEYFRAME = 240;
  FakeNet net;
  std::unique_ptr<DropInPlayer> host;
  std::unique_ptr<DropInPlayer> joiner;
  std::array<SeatPlan, MAX_SEATS> roster{};
  int t = 0;
  int sequence = 1 << 20;
  int from_frame = 0;
};
}  // namespace

// History is the host's game before the joiner plays in it, so it ends before the joiner's live
// frame; history past it ends the session.
TEST(OrcaDropIn, HistoryPastTheJoinersLiveFrameEndsTheSession)
{
  ScriptedHost h;
  h.PlayUntilLive(60);
  if (::testing::Test::HasFatalFailure())
    return;
  for (int i = 0; i < 3 && h.joiner->session.Error().empty(); ++i)
    h.Send(h.ScriptedHistory(h.roster));
  EXPECT_NE(h.joiner->session.Error().find("past this player's live frame"), std::string::npos)
      << h.joiner->session.Error();
  // The first such packet ends it: no frame ran (or ran again) with the scripted pad.
  EXPECT_EQ(h.ScriptedFrames(), 0);
}

// The host plugs this player's controller in once and names its live frame once; a roster that
// changes either ends the session.
TEST(OrcaDropIn, RosterThatTakesBackTheJoinersSeatEndsTheSession)
{
  {
    SCOPED_TRACE("plug and live frame reset");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto reset = h.roster;
    reset[1] = {NEVER, NEVER, NEVER};
    h.Send(h.Scripted(reset));
    EXPECT_NE(h.joiner->session.Error().find("plug-in"), std::string::npos)
        << h.joiner->session.Error();
    EXPECT_FALSE(h.joiner->session.CatchingUp());
  }
  {
    SCOPED_TRACE("plug moved later");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto moved = h.roster;
    moved[1].plug_from = h.joiner->session.CurrentFrame() + 4096;
    h.Send(h.Scripted(moved));
    EXPECT_NE(h.joiner->session.Error().find("plug-in"), std::string::npos)
        << h.joiner->session.Error();
  }
  {
    SCOPED_TRACE("live frame moved");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto moved = h.roster;
    moved[1].live_from = h.joiner->session.CurrentFrame() + 4096;
    h.Send(h.Scripted(moved));
    EXPECT_NE(h.joiner->session.Error().find("live frame"), std::string::npos)
        << h.joiner->session.Error();
  }
  {
    // A reset, history ahead, a plug-in again just past the joiner's frame: it ends at the first
    // step.
    SCOPED_TRACE("reset, history, plug again");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto reset = h.roster;
    reset[1] = {NEVER, NEVER, NEVER};
    h.Send(h.Scripted(reset));
    for (int i = 0; i < 10; ++i)
      h.Send(h.ScriptedHistory(reset));
    auto again = h.roster;
    again[1].plug_from = h.joiner->session.CurrentFrame() + 1;
    for (int i = 0; i < 300; ++i)
      h.Send(h.Scripted(again));
    EXPECT_FALSE(h.joiner->session.Error().empty());
    EXPECT_EQ(h.ScriptedFrames(), 0);
  }
  {
    // Dropping the joiner (its live frame back to NEVER, the plug-in kept) is what a host does when
    // it loses a friend: not an error.
    SCOPED_TRACE("dropped");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto dropped = h.roster;
    dropped[1].live_from = NEVER;
    dropped[1].unplug_from = h.joiner->session.CurrentFrame() + 40;
    h.Send(h.Scripted(dropped));
    EXPECT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
  }
}

// A host roster or history that takes back the joiner's seat is the host's doing: the joiner's game
// goes on (ReportSessionEnd says it can), it is never this machine's own failure.
TEST(OrcaDropIn, HostRosterThatEndsTheSessionIsThePeersFault)
{
  const auto ended_by = [](const char* what, const std::function<void(ScriptedHost&)>& send) {
    SCOPED_TRACE(what);
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    send(h);
    const std::string error = h.joiner->session.Error();
    ASSERT_FALSE(error.empty());
    EXPECT_TRUE(h.joiner->session.ErrorFromPeer()) << error;
    EXPECT_FALSE(Orca::Online::IsDesync(error)) << error;
    EXPECT_TRUE(Orca::Online::ReportSessionEnd(error, h.joiner->session.ErrorFromPeer())) << error;
  };
  ended_by("plug-in reset", [](ScriptedHost& h) {
    auto reset = h.roster;
    reset[1] = {NEVER, NEVER, NEVER};
    h.Send(h.Scripted(reset));
  });
  ended_by("live frame moved", [](ScriptedHost& h) {
    auto moved = h.roster;
    moved[1].live_from = h.joiner->session.CurrentFrame() + 4096;
    h.Send(h.Scripted(moved));
  });
  ended_by("unplug moved later", [](ScriptedHost& h) {
    auto later = h.roster;
    later[1].unplug_from = NEVER;
    auto earlier = h.roster;
    earlier[1].unplug_from = h.joiner->session.CurrentFrame() + 600;
    h.Send(h.Scripted(earlier));
    h.Send(h.Scripted(later));
  });
  ended_by("history past the live frame", [](ScriptedHost& h) {
    for (int i = 0; i < 3 && h.joiner->session.Error().empty(); ++i)
      h.Send(h.ScriptedHistory(h.roster));
  });
  ended_by("an invalid packet", [](ScriptedHost& h) {
    Packet p = h.Scripted(h.roster);
    p.current_frame = -5;
    h.Send(std::move(p));
  });
}

// The joiner is plugged in only once it holds the history up to its live frame: a plug-in roster
// before that ends the session, whatever follows it.
TEST(OrcaDropIn, PlugInBeforeTheJoinerHoldsItsHistoryEndsTheSession)
{
  ScriptedHost h;
  h.from_frame = ScriptedHost::KEYFRAME;
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster[0] = {0, NEVER, 0};
  roster[1] = {NEVER, NEVER, 1 << 29};
  // A little of the host's game as history, the joiner's seat unplugged.
  Packet first = h.ScriptedHistory(roster);
  first.history.resize(20);
  for (Pads& pads : first.history)
    pads[1] = UNPLUGGED_PAD;
  h.Send(std::move(first));
  ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
  ASSERT_EQ(h.joiner->session.LogEnd(), ScriptedHost::KEYFRAME + 20);

  auto plugged = roster;
  plugged[1].plug_from =
      std::max(h.joiner->session.CurrentFrame(), h.joiner->session.LogEnd()) + 10;
  h.Send(h.Scripted(plugged));
  EXPECT_NE(h.joiner->session.Error().find("plug-in"), std::string::npos)
      << h.joiner->session.Error();
  // Rosters and history after it change nothing.
  auto unplugged = plugged;
  unplugged[1].unplug_from = h.joiner->session.LogEnd();
  for (int i = 0; i < 4; ++i)
    h.Send(h.ScriptedHistory(unplugged));
  for (int i = 0; i < 300; ++i)
    h.Send(h.Scripted(plugged));
  EXPECT_FALSE(h.joiner->session.Error().empty());
  EXPECT_EQ(h.joiner->session.LogEnd(), ScriptedHost::KEYFRAME + 20);
  EXPECT_EQ(h.ScriptedFrames(), 0);
}

// The host only ever brings this player's unplug frame earlier (it leaves, or the host drops it); a
// roster that moves it later, or back to never, ends the session.
TEST(OrcaDropIn, JoinersUnplugOnlyComesEarlier)
{
  for (const bool back_to_never : {true, false})
  {
    SCOPED_TRACE(back_to_never ? "back to never" : "later");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto leaving = h.roster;
    leaving[1].unplug_from = h.joiner->session.CurrentFrame() + 40;
    h.Send(h.Scripted(leaving));
    ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
    auto earlier = leaving;
    earlier[1].unplug_from -= 10;
    h.Send(h.Scripted(earlier));
    ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
    auto moved = earlier;
    moved[1].unplug_from = back_to_never ? NEVER : earlier[1].unplug_from + 400;
    h.Send(h.Scripted(moved));
    EXPECT_NE(h.joiner->session.Error().find("unplug"), std::string::npos)
        << h.joiner->session.Error();
  }
  {
    // Plugged in and unplugged at the same frame, then history from the live frame on: refused like
    // any other history past the live frame.
    SCOPED_TRACE("plugged and unplugged at once");
    ScriptedHost h;
    h.from_frame = ScriptedHost::KEYFRAME;
    std::array<SeatPlan, MAX_SEATS> roster{};
    roster[0] = {0, NEVER, 0};
    roster[1] = {NEVER, NEVER, ScriptedHost::KEYFRAME};
    h.Send(h.Scripted(roster));
    const int plug = h.joiner->session.CurrentFrame() + 10;
    roster[1].plug_from = plug;
    roster[1].unplug_from = plug;
    h.Send(h.Scripted(roster));
    ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
    h.Send(h.ScriptedHistory(roster));
    EXPECT_NE(h.joiner->session.Error().find("past this player's live frame"), std::string::npos)
        << h.joiner->session.Error();
    roster[1].unplug_from = NEVER;
    for (int i = 0; i < 60; ++i)
      h.Send(h.Scripted(roster));
    EXPECT_EQ(h.ScriptedFrames(), 0);
  }
}

// A host whose plug-in roster is the first one the joiner hears (a live frame at the keyframe, so no
// history; the host's earlier packets lost): the joiner holds everything before its live frame, takes
// the plug-in and plays.
TEST(OrcaDropIn, PlugInAsTheFirstRosterTheJoinerHears)
{
  FakeNet net;
  net.latency = 4;
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 300);
  DropInPlayer host(DropInConfig(0, 300), net, 0, 300);
  host.game.m_state = solo.m_state;
  host.game.m_history = solo.m_history;
  host.game.m_ran_pads = solo.m_ran_pads;
  host.session.SetLog(0, log);
  host.session.AddPeer(1, 300, 300);
  DropInPlayer joiner(DropInConfig(1, 300), net, 1, 300);
  joiner.seat = 1;
  joiner.game.m_state = solo.m_state;
  bool heard_plug = false;
  int t = 0;
  for (; t < 2000; ++t)
  {
    net.now = t;
    host.Tick(t, 1);
    // Everything the host sends before the plug-in is lost on the way.
    if (host.session.Plan()[1].plug_from == NEVER)
      net.queues[1].clear();
    const bool behind = joiner.session.CatchingUp() ||
                        joiner.session.AuthorityFrame() - joiner.session.CurrentFrame() > 12;
    joiner.Tick(t, behind ? 40 : 1);
    ASSERT_TRUE(host.session.Error().empty()) << host.session.Error();
    ASSERT_TRUE(joiner.session.Error().empty()) << joiner.session.Error();
    if (!heard_plug && joiner.session.Plan()[1].plug_from != NEVER)
    {
      heard_plug = true;
      EXPECT_EQ(joiner.session.Plan()[1].plug_from, host.session.Plan()[1].plug_from);
    }
    if (heard_plug && joiner.session.CurrentFrame() > joiner.session.Plan()[1].plug_from + 120)
      break;
  }
  EXPECT_TRUE(heard_plug);
  EXPECT_FALSE(joiner.session.CatchingUp());
  EXPECT_GT(joiner.session.CurrentFrame(), joiner.session.Plan()[1].plug_from + 120);
}

// While catching up, the joiner keeps at most HISTORY_AHEAD (4096) frames of history past the frame
// it is at, whatever live frame the roster names.
TEST(OrcaDropIn, HistoryAheadOfTheJoinerIsBounded)
{
  FakeNet net;
  net.latency = 4;
  constexpr int KEYFRAME = 240;
  DropInPlayer joiner(DropInConfig(1, KEYFRAME), net, 1, KEYFRAME);
  joiner.seat = 1;
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster[0] = {0, NEVER, 0};
  roster[1] = {NEVER, NEVER, 1 << 29};
  int sequence = 0;
  int t = 0;
  for (int i = 0; i < 2000 && joiner.session.Error().empty(); ++i, ++t)
  {
    Packet p;
    p.seat = 0;
    p.sequence = ++sequence;
    p.ack = {-1, -1, -1, -1};
    p.current_frame = joiner.session.CurrentFrame();
    p.roster = roster;
    p.history_seat = 1;
    p.history_first = joiner.session.LogEnd();
    for (int k = 0; k < MAX_HISTORY_IN_PACKET; ++k)
    {
      Pads pads;
      pads.fill(UNPLUGGED_PAD);
      pads[0] = Pad{static_cast<u8>(k % 4), 0, 0, 0, 9, 0, 0, 0};
      p.history.push_back(pads);
    }
    net.now = t;
    net.queues[1].push_back({t, std::move(p)});
    joiner.Tick(t, 1);
    net.queues[0].clear();
  }
  EXPECT_TRUE(joiner.session.Error().empty()) << joiner.session.Error();
  EXPECT_TRUE(joiner.session.CatchingUp());
  EXPECT_LE(joiner.session.LogEnd(), joiner.session.CurrentFrame() + 4096 + 1);
  EXPECT_GT(joiner.session.LogEnd(), KEYFRAME + 4096 - MAX_HISTORY_IN_PACKET);
}

// ---- Direct links (YouGameRoom's DirectLink beside the relay) ----

// Packets arrive twice and out of order (direct copy and coalesced relay copy). Any copy adds
// inputs, acks and uncompared checksums, but only the newest moves the roster or the frame.
TEST(OrcaSessionDirect, StaleCopiesNeverPutTheRosterOrTheClockBack)
{
  struct Scripted final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override { return std::exchange(queue, {}); }
    bool Connected() const override { return true; }
    std::vector<Packet> queue;
  };
  // Host (seat 0) and this player (seat 1), both plugged in from frame 0.
  Config config;
  config.seats = 2;
  config.local_seat = 1;
  config.authority_seat = 0;
  config.time_sync = false;
  config.max_delay = config.input_delay;
  std::array<SeatPlan, MAX_SEATS> plan{};
  plan[0] = {0, NEVER, 0};
  plan[1] = {0, NEVER, 0};
  config.plan = plan;
  FakeGame game;
  Scripted net;
  Session session(config, game, net);
  int completed = -1;
  const auto run_to = [&](int frame) {
    for (int guard = 0; guard < 4000 && session.CurrentFrame() < frame; ++guard)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      if (step.kind == StepKind::Ended || step.kind == StepKind::Stall)
        return;
      if (step.kind != StepKind::Wait)
        completed = game.Run();
    }
  };
  const auto host_pad = [](int f) { return Pad{static_cast<u8>(f % 4), 0, static_cast<u8>(f), 0, 7, 0, 0, 0}; };
  // The host's roster as of its newest packet.
  std::array<SeatPlan, MAX_SEATS> roster = plan;
  const auto host_packet = [&](int sequence, int first, int count, int current) {
    Packet p;
    p.seat = 0;
    p.sequence = sequence;
    p.first_frame = first;
    for (int f = first; f < first + count; ++f)
      p.pads.push_back(host_pad(f));
    p.ack = {-1, -1, -1, -1};
    p.current_frame = current;
    p.roster = roster;
    return p;
  };

  // The newer packet unplugs this player at 400; the older one, arriving after, must not undo it.
  Packet older = host_packet(1, 0, 6, 4);
  roster[1].unplug_from = 400;
  Packet newer = host_packet(2, 0, 12, 10);
  net.queue = {newer, older};
  run_to(1);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.Plan()[1].unplug_from, 400);
  EXPECT_EQ(session.AuthorityFrame(), 10);
  // The same packet twice changes nothing.
  net.queue = {newer, newer};
  run_to(2);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.Plan()[1].unplug_from, 400);

  // Lost packets: each repeats every unacknowledged input, so the next arrival fills the gap.
  net.queue = {host_packet(4, 8, 16, 14)};  // 3 lost
  run_to(10);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.GetStats().confirmed_frame, 23);
  net.queue = {host_packet(10, 16, 40, 30)};  // 5 to 9 lost
  run_to(40);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.GetStats().confirmed_frame, 55);
  EXPECT_EQ(session.AuthorityFrame(), 30);
  // An older copy with an older frame and every older input: nothing moves back.
  Packet stale = host_packet(5, 0, 30, 12);
  stale.roster = plan;
  net.queue = {stale};
  run_to(41);
  EXPECT_EQ(session.AuthorityFrame(), 30);
  EXPECT_EQ(session.Plan()[1].unplug_from, 400);

  // Up to frame 130 with every input in, so frames 60 and 120 have local checksums.
  net.queue = {host_packet(11, 56, 64, 100), host_packet(12, 100, 60, 150)};
  run_to(140);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  ASSERT_TRUE(game.SnapshotChecksum(60).has_value());
  ASSERT_TRUE(game.SnapshotChecksum(120).has_value());
  const int matched = session.GetStats().checksums_matched;
  // The checksum of frame 60 over both paths: compared once.
  Packet with_checksum = host_packet(13, 150, 4, 152);
  with_checksum.checksum_frame = 60;
  with_checksum.checksum = *game.SnapshotChecksum(60);
  net.queue = {with_checksum, with_checksum};
  run_to(142);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.GetStats().checksums_matched, matched + 1);
  // The relay coalesced packet 14's checksum into packet 15, whose direct copy came first without
  // it: the relay copy is not newer, but its checksum is new and must be compared.
  Packet direct_copy = host_packet(15, 150, 8, 156);
  Packet relay_copy = direct_copy;
  relay_copy.checksum_frame = 120;
  relay_copy.checksum = *game.SnapshotChecksum(120) ^ 1;  // a desync, to show it was compared
  net.queue = {direct_copy, relay_copy};
  run_to(144);
  EXPECT_NE(session.Error().find("Desync at frame 120"), std::string::npos) << session.Error();
}

// A drop-in over a lossy, unordered direct link beside the relay (only the relay carries the
// history): the joiner catches up, plugs in at the same frame on both machines, stays identical
// with each checksum compared once, and leaves cleanly.
TEST(OrcaSessionDirect, FriendJoinsOverADirectLink)
{
  FakeNet net;
  net.latency = 6;
  net.jitter = 2;
  net.direct_latency = 0;
  net.direct_jitter = 2;
  net.direct_loss_percent = 20;
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 300);
  const int keyframe = 240;
  auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 300), net, 0, 300);
  host->game.m_state = solo.m_state;
  host->game.m_history = solo.m_history;
  host->game.m_ran_pads = solo.m_ran_pads;
  host->session.SetLog(0, log);
  host->session.AddPeer(1, 300, keyframe);
  auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe), net, 1, keyframe);
  joiner->seat = 1;
  joiner->game.m_state = At(solo.m_history, keyframe);
  int t = 0;
  for (; t < 1500; ++t)
  {
    net.now = t;
    if (t % 30 == 0)
    {
      for (DropInPlayer* p : {host.get(), joiner.get()})
        p->session.OnRoundTrip(7 * 1000 / 60);
    }
    host->Tick(t, 1);
    const bool behind = joiner->session.CatchingUp() ||
                        joiner->session.AuthorityFrame() - joiner->session.CurrentFrame() > 12;
    joiner->Tick(t, behind ? 40 : 1);
    ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
    ASSERT_TRUE(joiner->session.Error().empty()) << joiner->session.Error();
  }
  ASSERT_FALSE(joiner->session.CatchingUp());
  EXPECT_GT(net.direct_delivered, 1000);
  const int plugged_at = host->session.Plan()[1].plug_from;
  ASSERT_NE(plugged_at, NEVER);
  EXPECT_EQ(joiner->session.Plan()[1].plug_from, plugged_at);
  const int confirmed =
      std::min({host->session.GetStats().confirmed_frame, host->session.CurrentFrame() - 1,
                joiner->session.GetStats().confirmed_frame, joiner->session.CurrentFrame() - 1});
  ASSERT_GT(confirmed, plugged_at + 600);
  for (int f = keyframe; f <= confirmed; ++f)
  {
    ASSERT_EQ(At(host->game.m_history, f), At(joiner->game.m_history, f)) << "frame " << f;
    ASSERT_EQ(At(host->game.m_ran_pads, f), At(joiner->game.m_ran_pads, f)) << "frame " << f;
  }
  // One comparison per checksum frame, though most come twice.
  for (const DropInPlayer* p : {host.get(), joiner.get()})
  {
    EXPECT_GT(p->session.GetStats().checksums_matched, 10);
    EXPECT_LE(p->session.GetStats().checksums_matched, (confirmed - keyframe) / 60 + 1);
  }

  joiner->session.RequestLeave();
  for (int end = t + 200; t < end && !(joiner->session.LeftDone() && host->session.Idle()); ++t)
  {
    net.now = t;
    host->Tick(t, 1);
    if (!joiner->session.LeftDone())
      joiner->Tick(t, 1);
  }
  ASSERT_TRUE(joiner->session.LeftDone());
  ASSERT_TRUE(host->session.Idle());
  EXPECT_EQ(joiner->session.Plan()[1].unplug_from, host->session.Plan()[1].unplug_from);
}

// A fast, slightly lossy direct link beside a 48 ms relay path: both sides stay identical, each
// checksum is compared once, time sync stays balanced, and most rollbacks go away.
TEST(OrcaSessionDirect, DirectLinkBesideTheRelay)
{
  const auto side = [](int relay_ms, bool direct) {
    RelayMatch::Side s;
    s.relay_ms = relay_ms;
    s.input_every = 5;
    if (direct)
    {
      s.direct_ms = 1;
      s.direct_jitter_ms = 3;
      s.direct_loss_percent = 10;
    }
    return s;
  };
  RelayMatch relay(Config{}, side(8, false), side(40, false));
  relay.Play(3600);
  relay.Print("relay only");
  relay.ExpectClean();
  RelayMatch both(Config{}, side(8, true), side(40, true));
  both.Play(3600);
  both.Print("direct + relay");
  both.ExpectClean();
  EXPECT_GT(both.CompareConfirmed(), 3400);
  for (int i = 0; i < 2; ++i)
  {
    const Stats& s = both.SessionOf(i).GetStats();
    EXPECT_GT(both.DirectReceivedBy(i), 3000);
    EXPECT_GE(s.checksums_matched, 55);
    EXPECT_LE(s.checksums_matched, 60);
    EXPECT_LT(std::abs(s.frame_advantage), 1.0);
    EXPECT_LE(s.waits, 3);
    EXPECT_EQ(s.delay, 2);
    EXPECT_LT(s.rollbacks * 4, relay.SessionOf(i).GetStats().rollbacks) << "side " << i;
  }
}

TEST(OrcaSessionDirect, RoomBlipWithLiveInputsKeepsEveryFrameHash)
{
  using namespace std::chrono_literals;
  Config config;
  Match control(config, 0, 0);
  Match recovery(config, 0, 0);
  for (Match* match : {&control, &recovery})
  {
    match->net.direct_latency = 0;
    match->rtt_samples = false;
  }
  PeerRoomPresence peer;
  RoomReconnect room;
  for (int tick = 0; tick < 1200; ++tick)
  {
    const auto now = RoomClock::time_point{} + std::chrono::milliseconds(tick * 1000 / 60);
    if (tick == 200)
    {
      peer.Away(now);
      ASSERT_TRUE(room.Start(now, 0));
      recovery.net.relay_open = false;
    }
    if (tick >= 200 && tick < 1000)
    {
      EXPECT_EQ(peer.Check(now, now), RoomLoss::None);
      EXPECT_EQ(room.Check(now), RoomLoss::None);
    }
    if (tick == 1000)
    {
      ASSERT_TRUE(peer.Back(now, true));
      room.since.reset();
      recovery.net.relay_open = true;
    }
    control.Play(1);
    recovery.Play(1);
  }
  EXPECT_GT(recovery.CompareConfirmed(), 1100);
  for (const auto& pair : {std::pair{control.a.get(), recovery.a.get()},
                          std::pair{control.b.get(), recovery.b.get()}})
  {
    EXPECT_TRUE(pair.second->session.Error().empty());
    EXPECT_EQ(pair.first->game.m_history, pair.second->game.m_history);
    EXPECT_EQ(pair.first->game.m_ran_pads, pair.second->game.m_ran_pads);
    EXPECT_EQ(pair.first->session.CurrentFrame(), pair.second->session.CurrentFrame());
    EXPECT_EQ(pair.first->session.GetStats().checksums_matched,
              pair.second->session.GetStats().checksums_matched);
  }
}
