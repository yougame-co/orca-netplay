// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include <array>
#include <climits>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

// Orca's rollback session: the input exchange and the decision of which frame the emulator runs
// next. It knows nothing about Dolphin; the emulator implements Game and the network side
// Transport. The emulator calls OnBoundary after every frame and does what the returned Step says.
namespace Orca::Net
{
// One player's controller for one frame, as the SI device reports it (opaque to the session).
using Pad = std::array<u8, 8>;
constexpr int MAX_SEATS = 4;
// Marks a port with no controller: the top bit of the first byte, outside PAD_WIRE_BUTTONS so a
// real pad never has it. Ports without a player stay unplugged; a joining friend plugs in at a
// frame every machine agrees on.
constexpr u8 PAD_WIRE_UNPLUGGED = 0x80;
constexpr u8 PAD_WIRE_START_HIGH = 0x10;
constexpr Pad UNPLUGGED_PAD{PAD_WIRE_UNPLUGGED, 0, 0, 0, 0, 0, 0, 0};
inline bool IsUnplugged(const Pad& pad)
{
  return (pad[0] & PAD_WIRE_UNPLUGGED) != 0;
}
// A frame that never comes (about 200 days of play). Not INT_MAX: on Apple Silicon the JIT leaves
// FPCR.AH set on the CPU thread, and clang builds a {0, INT_MAX} pair with movi + FNEG, which then
// yields {0, -1}. 0x3fffffff uses plain MOV immediates.
constexpr int NEVER = 0x3fffffff;
// Bounds on what a peer may send: inputs per packet, and how far ahead its frames may be (input
// delay plus the rollback window, generously).
constexpr int MAX_PADS_IN_PACKET = 64;
constexpr int MAX_FRAMES_AHEAD = 240;
// Drop-in history frames per packet (senders send half, so two coalesced packets still fit).
constexpr int MAX_HISTORY_IN_PACKET = 512;
// Most predicted frames between two snapshots (Config::snapshot_every). A rollback may load a
// snapshot up to MAX_SNAPSHOT_EVERY - 1 frames before the first wrong frame and re-run from there.
constexpr int MAX_SNAPSHOT_EVERY = 4;
// Relay round trips whose median the adaptive delay covers (one per second, so the last 15 s). A
// new session can take this many at once (Session::OnRoundTrip).
constexpr int RTT_SAMPLES = 15;
// Delay-change notes kept for the log until taken (Session::TakeDelayNotes).
constexpr size_t MAX_DELAY_NOTES = 16;
// How many extra frames of the others' delay a stall's length is judged against (Packet::spared).
// A longer stall couldn't be spared by any raise within max_delay anyway.
constexpr int SPARED_FRAMES = 4;
// Windows of the adaptive delay's history (Session::AdaptDelay): the last 45 seconds.
constexpr int NEED_WINDOWS = 45;
using Pads = std::array<Pad, MAX_SEATS>;

// What the session needs from the emulator. Frames count from 0. "The state at the start of frame
// f" is what the emulator holds at the boundary after frame f-1 completes.
class Game
{
public:
  virtual ~Game() = default;
  // Snapshot the current state as the start of `frame`. Only frames the session may roll back to or
  // checksum are saved; Load is never asked for an unsaved frame.
  virtual bool Save(int frame) = 0;
  // Restore the state at the start of `frame`; the next boundary reached is the end of `frame`.
  virtual bool Load(int frame) = 0;
  // The pads every port reports while `frame` runs.
  virtual void SetPads(int frame, const Pads& pads) = 0;
  // True while already-shown frames are re-run (the emulator skips rendering).
  virtual void SetResimulating(bool resimulating) = 0;
  // A hash of the saved state at the start of `frame`, for desync detection (nullopt if not saved).
  // Only asked every Config::checksum_every frames; it may be computed at Save time.
  virtual std::optional<u64> SnapshotChecksum(int frame) = 0;
  // After a stall: restart frame pacing from now, so the emulator doesn't race to make up the time.
  virtual void ResetPacing() = 0;
  // What one Save costs on this machine, smoothed, in ms (0: not measured). Picks
  // Config::snapshot_every: a full RAM copy is cheap on some machines and costly on others, where
  // it also evicts the cache the next frame needs.
  virtual double SaveMs() const { return 0; }
  // Steady-clock time in ms from any origin, for measuring stalls (Packet::spared). Tests with a
  // simulated game may supply their own clock.
  virtual double NowMs() const;
};

// Drop-in: a seat's controller is plugged in for frames [plug_from, unplug_from), and it exchanges
// live inputs from live_from on (before that, its player replays the host's history). NEVER for an
// undecided frame.
struct SeatPlan
{
  int plug_from = NEVER;
  int unplug_from = NEVER;
  int live_from = NEVER;
  bool operator==(const SeatPlan&) const = default;
};

// Host-owned inputs through completed stays, including unplugged gaps between rebuilds.
struct DepartedInputs
{
  int from = NEVER;
  int until = NEVER;
  int first_frame = 0;
  std::vector<Pad> pads;
};

// One message from one player: its recent inputs, what it has received, and a checksum.
struct Packet
{
  int seat = 0;
  // The sender's packet count, from 1 (0: unsequenced). A packet can arrive twice (direct link and
  // relay) and out of order; only one newer than all before it from that seat says where the sender
  // is now (Session::ReceivePackets).
  int sequence = 0;
  // pads[i] is the input for frame first_frame + i.
  int first_frame = 0;
  std::vector<Pad> pads;
  // Other seats on this connection, with exactly the same frame range as pads.
  std::vector<std::pair<int, std::vector<Pad>>> co;
  // Highest contiguous input received for each seat (-1: none). The sender's own seat stays -1,
  // except for completed-seat history during a doubles rebuild.
  std::array<int, MAX_SEATS> ack{-1, -1, -1, -1};
  // The frame this sender is at, for time sync.
  int current_frame = 0;
  // Frames the sender re-ran in its last one-second window, counted from each rollback's first
  // wrong frame (-1: no window yet). Telemetry only; the delay doesn't adapt to rollbacks.
  int resimulated = -1;
  // The sender's stalls that count for the delay, over the whole match (-1: not reported): only
  // once every peer's inputs were arriving, not while catching up, and not coinciding with a hitch
  // on either machine. The others raise their own delay on this count. A running total can't be
  // missed or double counted.
  int stalls = -1;
  // The sender's hitches (Session::OnLocalHitch) over the whole match (-1: not reported).
  int hitches = -1;
  // Of those counted stalls, how many ended within k + 1 frames, for k = 0 .. SPARED_FRAMES - 1: k
  // + 1 more frames of the others' delay would have prevented them. A running total per k, like
  // `stalls` (nullopt: not reported).
  std::optional<std::array<int, SPARED_FRAMES>> spared;
  // The sender's smoothed lead over the slowest other player, in tenths of a frame (nullopt:
  // nothing received yet). Both leads include the same transit time, so time sync halves their
  // difference and transit cancels out.
  std::optional<int> advantage;
  // The sender's typical round trip to the relay: the median of its latest ones, in ms (nullopt:
  // too few measured). The one-way time between two players is half of each one's round trip.
  std::optional<int> rtt;
  std::optional<int> checksum_frame;
  u64 checksum = 0;

  // Drop-in (see SeatPlan): which seats are plugged in when. Only the host's roster counts.
  std::optional<std::array<SeatPlan, MAX_SEATS>> roster;
  // Host-owned machine groups beside the roster: each seat's lead, or -1 for no seat.
  std::optional<std::array<int, MAX_SEATS>> groups;
  // Multi-peer pre-match batch: suppress Start during [first, second).
  std::optional<std::pair<int, int>> start_gate;
  // The host's final seat history through completed stays (opted-in friends rooms only).
  std::map<int, DepartedInputs> departed;
  // The highest frame of shared history this sender holds contiguously from frame 0 (-1: none). The
  // host sends a joining player the history it lacks.
  int history_ack = -1;
  // History for `history_seat` (-1: none): every seat's pads from history_first on, from the host's
  // log of confirmed frames.
  int history_seat = -1;
  int history_first = 0;
  std::vector<Pads> history;
  // This player asks to unplug (leave); the host picks the frame.
  bool leaving = false;
  // The newest version of the host's port values (Session::RequireValues) this sender holds (-1:
  // none or unknown).
  int values_ack = -1;
};

class Transport
{
public:
  virtual ~Transport() = default;
  virtual void Send(const Packet& packet) = 0;
  // Opted-in multi-peer rooms: one input range per receiving seat, one relay envelope.
  virtual void SendToSeats(const std::map<int, Packet>& packets)
  {
    for (const auto& [seat, packet] : packets)
      Send(packet);
  }
  // Packets received since the last call, in arrival order.
  virtual std::vector<Packet> Receive() = 0;
  virtual bool Connected() const = 0;
};

struct Config
{
  int seats = 2;
  bool per_peer_inputs = false;
  int local_seat = 0;
  // Fixed local group, dormant unless explicitly configured.
  std::vector<int> co_seats;
  // Frames between sampling a local input and the frame it applies to: the starting delay and the
  // adaptive floor. Two, like Slippi: rollback hides the rest of a normal link.
  int input_delay = 2;
  // Ceiling of the adaptive or chosen delay (100 ms at 60 Hz). At or below input_delay the adaptive
  // delay never changes. Each player's delay is its own: every input travels explicitly, so peers
  // with different delays still agree on every frame.
  int max_delay = 6;
  // The delay this player chose (the app's "delay N"), used exactly from the first frame (clamped
  // to 1..max_delay). nullopt: adaptive. SetFixedDelay changes it.
  std::optional<int> fixed_delay;
  // Game frames per second. Frames are the session's only clock: "a second" for delay and time-sync
  // windows is this many frames, so sessions fed the same packets decide alike. Also converts round
  // trips from ms to frames.
  int frame_rate = 60;
  // Request a one-frame wait (StepKind::Wait) while this player runs ahead of the others.
  bool time_sync = true;
  // Deepest rollback allowed; beyond it the session stalls until inputs arrive.
  int max_rollback = 7;
  // Players exchange a state checksum every this many confirmed frames (a full RAM hash costs a few
  // ms, so not every frame).
  int checksum_every = 60;
  // Most inputs repeated in one packet.
  int max_pads_per_packet = 16;
  // Predicted frames per snapshot, 1 to MAX_SNAPSHOT_EVERY. 0 picks from Game::SaveMs each
  // "second": 1 while a save costs under about 3 ms, then one more per 3 ms. Checksum frames are
  // always saved. A rollback loads the newest snapshot at or before the first wrong frame, trading
  // up to k - 1 extra re-run frames for k times fewer snapshots.
  int snapshot_every = 0;

  // Drop-in play. Defaults: every seat below `seats` plugged in and live from frame 0 (a match both
  // players start together), with nothing logged.
  //
  // The frame the session starts at: the host's next frame when a friend arrives, or 0 for a friend
  // who replays the host's game from boot.
  int start_frame = 0;
  // The local pad the host held when the session started; it fills the frames the input delay
  // skips, so a held button stays held.
  Pad start_pad{};
  // Held pads for co-seats; the lead continues to use start_pad.
  std::array<Pad, MAX_SEATS> start_pads{};
  // Each seat's plan at the start (default: seats below `seats` plugged in from frame 0).
  std::optional<std::array<SeatPlan, MAX_SEATS>> plan;
  // The seat whose roster decides who plugs in and when (the host), or -1 for a fixed roster.
  int authority_seat = -1;
};

enum class StepKind
{
  // Run the next frame (pads have been set).
  Run,
  // A rollback started: the state was loaded and the next frame's pads are set.
  Rollback,
  // Waiting for remote inputs: after a short wait, call OnBoundary again with the same frame
  // without running anything (give up if the emulator is stopping).
  Stall,
  // Time sync: this player is ahead, so it gives one frame back. Hold for one frame period, then
  // call OnBoundary again with the same frame; pacing resets so the frame isn't made up. At most
  // one per 30 frames.
  Wait,
  // The session ended (see Session::Error).
  Ended,
};

struct Step
{
  StepKind kind = StepKind::Run;
  int frame = 0;  // the frame that runs next (Run, Rollback), or the stalled frame
};

struct Stats
{
  int rollbacks = 0;
  // Frames re-run, and the most in one rollback, counted from the loaded snapshot, so sparse
  // snapshots' extra frames are included.
  int resimulated_frames = 0;
  int deepest_rollback = 0;
  // Snapshots taken, and the current predicted frames per snapshot (Config::snapshot_every).
  int saves = 0;
  int snapshot_every = 1;
  int stalls = 0;
  int checksums_matched = 0;
  int confirmed_frame = -1;
  // The current input delay, whether the player chose it (Config::fixed_delay), and how the
  // adaptive one got there: raises (for the link or the others' stalls), lowers, and raises that
  // undid a lower.
  int delay = 0;
  bool delay_fixed = false;
  int delay_raises = 0;
  int delay_lowers = 0;
  int delay_reverts = 0;
  // Time-sync waits, and the last averaged (our lead - their lead) / 2, in frames.
  int waits = 0;
  double frame_advantage = 0;
  // Slow frames this machine reported (OnLocalHitch), and the most any other player reported.
  int hitches = 0;
  int peer_hitches = 0;
  // Slow frames a game load explained (OnLoadHitch), not in `hitches`.
  int load_hitches = 0;
  // Stalls that count toward the others' delay (no hitch nearby on either machine, not caused by
  // the other side's own stall, not while catching up or settling), as reported (Packet::stalls),
  // and how many a frame more of the others' delay would have spared (Packet::spared).
  int counted_stalls = 0;
  int spared_stalls = 0;
  // Typical relay round trip in ms: this player's median, and the slowest other player's as
  // reported (-1: not known yet). The link part of the delay follows these; single samples are too
  // noisy.
  int rtt_median = -1;
  int peer_rtt_median = -1;
  // Players whose inputs kept coming late against their own frame (Session::LateSeat), counted
  // each time one was found so.
  int late_seats = 0;
};

class Session
{
public:
  Session(const Config& config, Game& game, Transport& transport);

  // Called at every boundary. `completed_frame` is the frame that just finished (-1 at the very
  // first boundary). `local_pad` is the local controller now; it applies to frame (next frame +
  // Delay()). Re-run boundaries also call this; the pad is ignored there.
  Step OnBoundary(int completed_frame, const Pad& local_pad);
  Step OnBoundary(int completed_frame, const Pad& local_pad, const Pads* co_pads);
  // A measured relay round trip, from the transport's pings. Call on the OnBoundary thread once per
  // new measurement (YouGameRoom::RoundTripMs gives a sequence number; feed only when it changes,
  // or one ping counts many times). A new session may take the room's recent ones at once, oldest
  // first, so it covers the link from its first frame.
  void OnRoundTrip(int rtt_ms);
  // This player's chosen delay (Config::fixed_delay), or nullopt for adaptive, from the next sample
  // on. Call on the OnBoundary thread. A mid-match change moves where the next local sample lands:
  // frames a longer delay skips repeat the held input, and samples a shorter one maps onto taken
  // frames are dropped. Every input still travels explicitly, so peers need nothing new. Returning
  // to adaptive starts over from what the link needs.
  void SetFixedDelay(std::optional<int> frames);
  std::optional<int> FixedDelay() const { return m_fixed_delay; }
  // This machine just had a slow frame (a disc read, a shader compile: over two frame periods
  // outside the session's own stalls and waits). Only the emulator can tell, since the session has
  // no wall clock; peers hear of it and exclude the stalls it caused from the delay.
  void OnLocalHitch();
  // A frame that was slow only because the game itself held it (a load, the same on every machine).
  // Counted apart and not reported to peers; stalls around it still don't count.
  void OnLoadHitch();

  // ---- Drop-in ---- The confirmed history: every seat's pads for frames [base, base + size). The
  // host starts with the log of its solo frames since its keyframe and sends a joiner what it
  // lacks; TakeLog returns it, with every frame run so far, when the session ends.
  void SetLog(int base, std::vector<Pads> log);
  std::vector<Pads> TakeLog(int* base);
  int LogEnd() const { return m_log_base + static_cast<int>(m_log.size()); }
  // Host: a player at `seat` arrived and loads the keyframe of frame `history_from`; it replays the
  // log up to live_from - 1 and exchanges live inputs from live_from on. Its controller plugs in
  // once it has caught up.
  void AddPeer(int seat, int live_from, int history_from);
  // Host: all members plug in together after every member catches up.
  void AddPeerBatch(const std::vector<int>& seats, int live_from, int history_from);
  // Host: declare a fixed machine group before admitting it with AddPeerBatch.
  void SetGroup(int lead, const std::vector<int>& co_seats);
  bool IsLocal(int seat) const;
  int LocalSeat() const { return m_config.local_seat; }
  int LeadOf(int seat) const;
  bool SameMachine(int a, int b) const;
  int PeersInPlay() const;
  // Host: the player at `seat` is gone (left or stopped answering); its controller unplugs after
  // its last received input.
  void DropSeat(int seat);
  // Host: schedule a co-seat's unplug ahead of the rollback window; a lead takes its group.
  // Retire a scheduled co-seat while preserving its agreed input prefix and unplug frame.
  void PrepareAdmission(int seat);
  void UnplugSeat(int seat);
  // A joining player: ask the host to unplug this controller. LeftDone() once it has.
  void RequestLeave();
  bool LeftDone() const;
  // Host: no other player is plugged in or joining, so the session can end and the host play solo.
  bool Idle() const;
  // This player and every other one in play.
  int PlayersInPlay() const;
  // ---- Port values (who plays a port, and with which controls) ---- The host sets each port's
  // values from the frame its controller plugs in, and every machine must hold them before running
  // that frame. So `seat` plugs in only once every player (newcomer included) acknowledges holding
  // version `version` or newer. On a player, this records the newest version it holds, which its
  // packets acknowledge. Versions only grow.
  void RequireValues(int seat, int version);
  void SetValuesHeld(int version);
  // Local inputs already sampled for frames from CurrentFrame() on (the host keeps them when it
  // goes back to solo play).
  std::vector<Pad> PendingLocal() const;
  std::vector<Pad> PendingLocal(int seat) const;
  // Whether this seat's controller is plugged in at `frame`, including completed stays.
  bool Plugged(int seat, int frame) const;
  // The newest frame the host reported (a joining player catches up to it).
  int AuthorityFrame() const;
  int AuthoritySeat() const { return m_config.authority_seat; }
  // This player is still replaying the host's game; its own controller isn't plugged in yet.
  bool CatchingUp() const;
  // Something besides inputs that frames from `frame` on depend on changed (a port's name arrived
  // late): re-run them as for a wrong guess. Only within the rollback window and from a snapshot
  // still held; otherwise returns false and nothing happens.
  bool RerunFrom(int frame);
  // Host: drop every other player (they stopped answering, or the session failed on them).
  void DropAllPeers();
  // Host: a keyframe of the state at the start of CurrentFrame() would be final: every earlier
  // frame is confirmed and nothing is being re-run.
  bool Settled() const;
  const std::array<SeatPlan, MAX_SEATS>& Plan() const { return m_plan; }
  // Drop-in bookkeeping in one line, for logs: per seat its plan, received, acknowledged and
  // reported frames and history acknowledgement; the log and local input ranges.
  std::string Describe() const;

  // Each change of this player's delay since the last call, one line each: frame, from, to, and
  // why, with the numbers the rule used. For the log; only the newest MAX_DELAY_NOTES are kept.
  std::vector<std::string> TakeDelayNotes() { return std::exchange(m_delay_notes, {}); }

  const std::string& Error() const { return m_error; }
  // The error was another player's doing (a packet or a host roster no honest Orca sends), not
  // this machine's: this player can play on, and the match it was in doesn't count.
  bool ErrorFromPeer() const { return m_error_from_peer; }
  // Whether `seat`'s inputs keep coming late against its own frame (a player holding them back):
  // this machine then runs fewer frames on guesses for it.
  bool LateSeat(int seat) const;
  // Ends the session for something another player sent that this machine caught outside it (a
  // host's values for this player's own port): the next step ends, ErrorFromPeer.
  void EndForPeer(std::string error) { FailPeer(std::move(error)); }
  const Stats& GetStats() const { return m_stats; }
  int CurrentFrame() const { return m_frame; }
  bool Resimulating() const { return m_resim_until >= 0; }
  // Rollback can return from a boundary while its original input stall is still ongoing.
  bool InputStalled() const { return m_stalling && m_stall_ended_ms < 0; }
  std::vector<int> MissingInputSeats() const;
  int Delay() const { return m_delay; }
  // The newest frame whose inputs are known for every seat (every frame through it ran, or will
  // re-run, with real inputs).
  int ConfirmedFrame() const;
  // Frames before this one are final: confirmed for every seat, run with those inputs, and not
  // waiting to re-run (as CheckChecksums takes its checksums). A replay stream records only these.
  int FinalFrame() const { return std::min({m_frame, ConfirmedFrame() + 1, m_dirty}); }
  // This machine's RAM checksums of final frames (every Config::checksum_every), the newest 600
  // frames' worth: (frame, RamChecksum of the state at that frame's start).
  const std::map<int, u64>& LocalChecksums() const { return m_local_checksums; }

private:
  void AdmitSeat(int seat, int live_from, int history_from);
  void RetireSeat(int seat, bool preserve_unplug = false);
  void CloseEmptyBatch();
  void ReceiveCoPrefixReplies(const Packet& packet);
  void PrepareCoPrefixReplies(const Packet& packet);
  void ReceivePackets();
  void ReceiveOne(const Packet& packet, bool input_only = false);
  bool ApplyGroups(const Packet& packet);
  void AddCoPads(Packet* packet) const;
  void AddPeerChecksum(int seat, const Packet& packet);
  void SendPacket();
  Pads PadsFor(int frame);
  bool NeedsSnapshot(int frame) const;
  void ChooseSnapshotEvery();
  void CheckChecksums();
  void UpdateLateness();
  void ResetLateness(int seat);
  Step Fail(std::string error);
  // Fail, for what another player sent (ErrorFromPeer).
  Step FailPeer(std::string error);
  bool SaveOnce(int frame);
  void SampleLocal(const Pad& local_pad, const Pads* co_pads);
  void AdaptDelay();
  void RaiseToLink();
  void NoteDelay(int from, std::string why);
  int DelayOfInput(int frame) const;
  std::optional<int> LatencyDelay(double margin_ms) const;
  int RoundTripFrames(int seat) const;
  double FramesFromMs(double ms) const;
  void TrackAdvantage();
  int KnownThrough(int seat) const;
  int DepartedThrough(int seat) const;
  const Pad* DepartedPad(int seat, int frame) const;
  void AddDeparted(Packet* packet, int recipient) const;
  bool InPlay(int seat) const;
  int SettleFrom() const;
  int StallLimit() const;
  void AdvanceReceived(int seat);
  void ApplyRoster(const std::array<SeatPlan, MAX_SEATS>& roster);
  void ReceiveHistory(const Packet& packet);
  void AppendConfirmedToLog();
  void PlugCaughtUpPeers();
  void AddHistory(Packet* packet);
  std::set<int> m_plug_batch;
  int m_history_next = 0;
  std::optional<std::pair<int, int>> m_start_gate;
  std::vector<std::pair<int, int>> m_previous_start_gates;
  std::array<int, MAX_SEATS> m_authority_ack{-1, -1, -1, -1};
  std::array<std::array<int, MAX_SEATS>, MAX_SEATS> m_peer_ack{};
  std::map<int, DepartedInputs> m_departed;

  Config m_config;
  Game& m_game;
  Transport& m_transport;

  // The next frame to run in normal flow.
  int m_frame = 0;
  // While re-running: the last frame of the re-run (the newest frame run before it).
  int m_resim_until = -1;
  // Earliest frame that ran with a wrong guess (INT_MAX: none).
  int m_dirty;

  std::map<int, Pad> m_local;
  std::array<std::map<int, Pad>, MAX_SEATS> m_co_local;
  std::array<int, MAX_SEATS> m_lead{0, 1, 2, 3};
  std::array<bool, MAX_SEATS> m_group_known{};
  // A removed initial local co-seat may subsequently belong to another machine.
  std::array<bool, MAX_SEATS> m_local_retired{};
  // Former co-seat leads can still have aligned copies in flight, only during that lead's stay.
  std::array<unsigned, MAX_SEATS> m_retired_co_leads{};
  // A scheduled co-seat stay can outlive room ownership while its slot is being replaced.
  std::array<int, MAX_SEATS> m_departed_owner{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_local_co_until{NEVER, NEVER, NEVER, NEVER};
  std::map<int, DepartedInputs> m_co_prefix_replies;
  std::array<std::map<int, Pad>, MAX_SEATS> m_remote;
  // Highest frame of each seat's input received contiguously.
  std::array<int, MAX_SEATS> m_received{-1, -1, -1, -1};
  // The remote pads each frame actually ran with.
  std::map<int, Pads> m_used;
  // Highest of our own frames each seat has acknowledged.
  std::array<int, MAX_SEATS> m_acked{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_remote_frame{0, 0, 0, 0};

  std::map<int, u64> m_local_checksums;
  std::map<std::pair<int, int>, u64> m_peer_checksums;
  int m_last_checksum_frame = 0;
  std::optional<int> m_pending_checksum;
  int m_last_sent_newest = -1;
  bool m_stalling = false;
  // The frame last saved since the last load (-1: none), so a repeated boundary on a stalled or
  // waiting frame doesn't snapshot it again.
  int m_last_saved = -1;
  // Frames whose snapshot matches this session's history. A rollback forgets those after the frame
  // it loads until the re-run saves them again.
  std::set<int> m_saved;
  // Predicted frames per snapshot (Config::snapshot_every, or chosen from Game::SaveMs).
  int m_snapshot_every = 1;

  // Input delay: adapted once per window of frame_rate frames, unless the player chose one.
  int m_delay;
  std::optional<int> m_fixed_delay;
  int m_window_start = 0;
  // Frames re-run from the first wrong one, over the match and as of the last window, and the last
  // window's count (sent as telemetry).
  int m_guessed_frames = 0;
  int m_window_resimulated = 0;
  int m_last_resimulated = -1;
  // Stalls that count for the delay (sent), and how many ended within k + 1 frames (sent as
  // Packet::spared). The current stall's start and end times (Game::NowMs; -1 until judged, when
  // the frame after it runs). Each peer's reported counts, whether it reports lengths, and how much
  // the last window already took; the least delay carried by inputs its newest stalls waited on
  // (INT_MAX: none this window).
  int m_link_stalls = 0;
  std::array<int, SPARED_FRAMES> m_spared{};
  double m_stall_began_ms = 0;
  double m_stall_ended_ms = -1;
  std::array<int, MAX_SEATS> m_peer_stalls{0, 0, 0, 0};
  std::array<int, MAX_SEATS> m_peer_stalls_seen{0, 0, 0, 0};
  std::array<std::array<int, SPARED_FRAMES>, MAX_SEATS> m_peer_spared{};
  std::array<std::array<int, SPARED_FRAMES>, MAX_SEATS> m_peer_spared_seen{};
  std::array<bool, MAX_SEATS> m_peer_reports_spared{};
  std::array<int, MAX_SEATS> m_peer_stall_delay{INT_MAX, INT_MAX, INT_MAX, INT_MAX};
  // The delay this player's inputs carry from each frame on, as (first frame, delay), oldest first.
  std::deque<std::pair<int, int>> m_delay_marks;
  // The furthest the others ran ahead of this player's sampling this window (their lag on its
  // inputs plus the delay those carried). Then, for each of the last NEED_WINDOWS windows, the
  // least delay at which the others wouldn't have stalled on this player's inputs, and how many of
  // the newest count toward a raise (none from before the last step down).
  int m_window_peer_reach = 0;
  std::array<int, NEED_WINDOWS> m_need_history{};
  int m_evidence_windows = 0;
  // Per player, the frame at which its lag on our inputs last reached max_rollback (it was waiting
  // on them): only then are its stalls ours to answer.
  std::array<int, MAX_SEATS> m_edge_frame{INT_MIN / 2, INT_MIN / 2, INT_MIN / 2, INT_MIN / 2};
  // The frame at which another player's counted stall was last heard of: a stall here around then
  // was caused by theirs (they stopped sending new inputs while stalled).
  int m_last_peer_stall_frame = INT_MIN / 2;
  // Whether the link raised the delay to its current value (so it may follow the link back down),
  // and consecutive windows in which the link wanted less.
  bool m_delay_from_link = false;
  int m_link_below_windows = 0;
  // Consecutive quiet windows (none where a frame less would have left under two frames of room),
  // how many a step down needs (doubled by each one undone), and windows since the last step down.
  int m_quiet_windows = 0;
  int m_hold_windows;
  int m_windows_since_lower = INT_MAX / 2;
  // The frame the current stall began at, and whether it may count: every peer's inputs had started
  // arriving, this player's controller is plugged in, and the newest plug-in has settled
  // (SettleFrom). Judged again when it ends.
  int m_stall_frame = 0;
  bool m_stall_after_input = false;
  // This machine's hitches (sent), each peer's, and the frame the latest was heard of.
  int m_hitches = 0;
  std::array<int, MAX_SEATS> m_peer_hitches{0, 0, 0, 0};
  int m_last_hitch_frame = INT_MIN / 2;

  // This player's latest relay round trips (oldest first) and their median in ms (nullopt: too
  // few), and each peer's reported median.
  std::deque<int> m_rtts;
  std::optional<int> m_typical_rtt;
  std::array<std::optional<int>, MAX_SEATS> m_peer_rtt;

  // Time sync per peer, in frames: our latest lead over it, that lead smoothed, and its own
  // smoothed lead over its slowest peer as reported. The balance (ours - theirs) / 2 is averaged
  // over a window.
  std::array<std::optional<double>, MAX_SEATS> m_advantage;
  // Input lateness per seat (UpdateLateness): this window's sum and samples, late and on-time
  // windows in a row, and whether the seat counts as late.
  std::array<int, MAX_SEATS> m_late_sum{};
  std::array<int, MAX_SEATS> m_late_samples{};
  std::array<int, MAX_SEATS> m_late_windows{};
  std::array<int, MAX_SEATS> m_clean_windows{};
  std::array<bool, MAX_SEATS> m_late{};
  int m_late_window_start = 0;
  std::array<std::optional<double>, MAX_SEATS> m_smoothed_advantage;
  std::array<std::optional<double>, MAX_SEATS> m_remote_advantage;
  double m_balance_sum = 0;
  int m_balance_samples = 0;
  int m_sync_start = 0;
  bool m_wait_pending = false;
  bool m_waiting = false;

  // Packets: the last sequence number sent, and per seat the newest received and the newest
  // checksum frame taken (a duplicate packet carries the same one).
  int m_send_sequence = 0;
  std::array<int, MAX_SEATS> m_peer_sequence{0, 0, 0, 0};
  std::array<int, MAX_SEATS> m_peer_checksum_frame{-1, -1, -1, -1};

  // Drop-in.
  std::array<SeatPlan, MAX_SEATS> m_plan{};
  int m_log_base = 0;
  std::vector<Pads> m_log;
  // Host: per seat, the history frames sent (and when the peer's acknowledgement last moved), and
  // the newest history frame the peer acknowledged.
  std::array<int, MAX_SEATS> m_history_sent{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_history_acked{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_history_stuck{0, 0, 0, 0};
  // A joining player: its own live frame as the host first named it (NEVER: not yet). History is
  // accepted only for frames before it.
  std::array<int, MAX_SEATS> m_own_live_from{NEVER, NEVER, NEVER, NEVER};
  bool m_leaving = false;
  // Port values: per seat, the version every player must hold before it plugs in (host), and the
  // newest each player acknowledged (host); then the newest this machine holds.
  std::array<int, MAX_SEATS> m_values_required{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_values_acked{-1, -1, -1, -1};
  int m_values_held = -1;

  std::vector<std::string> m_delay_notes;

  Stats m_stats;
  std::string m_error;
  bool m_error_from_peer = false;
};
}  // namespace Orca::Net
