// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Perf.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/RoomResilience.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/MenuEvents.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/Search.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/UX.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/Rollback/Rollback.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Rollback::OnlineMatch
{
namespace
{
using Orca::Net::Pad;
using Orca::Net::Pads;
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The host's keyframe: captured at a boundary, then packed, compressed and stored on a thread.
struct KeyframeJob
{
  std::thread thread;
  std::atomic<bool> done{false};
  bool ok = false;
  int frame = -1;
  Orca::Net::KeyframeInfo info;
  double capture_ms = 0, pack_ms = 0, put_ms = 0;
  u64 raw_size = 0;
  // The header generation wanted at capture (Games/Brawl/OnlineRules.h WantedGeneration).
  u64 generation = 0;
  std::string error;
  // The store refused it for good (Orca::Net::LastRefusal); empty if another try could work.
  std::string refusal;
  ~KeyframeJob()
  {
    if (thread.joinable())
      thread.join();
  }
};

// A joining player's download of the host's keyframe, unpacked on its own thread.
struct Download
{
  std::thread thread;
  std::atomic<bool> done{false};
  std::atomic<int> percent{0};
  std::atomic<bool> cancel{false};
  bool ok = false;
  std::string error;
  // The store refused the download for good (Orca::Net::LastRefusal).
  std::string refusal;
  Orca::Net::KeyframeInfo info;
  int frame = -1;
  Orca::Net::ReplayArchive replay;
  double transfer_ms = 0, unpack_ms = 0;
  ~Download()
  {
    cancel = true;
    if (thread.joinable())
      thread.join();
  }
};

// A player who arrived for a seat (its hello): its name and own controls (empty if none).
struct Arrival
{
  std::string name;
  std::vector<u8> controls;
  std::vector<u8> queue;
  bool recovering = false;
  bool doubles = false;
  Clock::time_point arrived_at = Clock::now();
  u64 arrived_boundary = 0;
  int next_keyframe_notice = 15;
};

struct Match
{
  bool started = false;
  bool finished = false;
  // Emulation stopped since this match started: the next boot plays a new match.
  bool stale = false;
  std::unique_ptr<RingPort> port;
  std::unique_ptr<Orca::Net::Session> session;
  std::unique_ptr<Orca::Net::KeyframeStore> store;
  bool joining = false;
  bool reopen_join = false;
  int local_seat = 0;
  // The frame the current boundary completes (-1 at the first boundary).
  int running = -1;

  // Host: every seat's pads since the newest keyframe. The session owns the log while friends play.
  int log_base = 0;
  std::vector<Pads> log;
  // Local inputs the session had already sampled ahead (its input delay) when it ended.
  std::deque<Pad> queued_local;
  std::unique_ptr<KeyframeJob> job;
  std::optional<Orca::Net::KeyframeInfo> keyframe;
  // The header generation `keyframe` was captured under (KeyframeJob::generation).
  u64 keyframe_generation = 0;
  double keyframe_capture_ms = 0;
  bool keyframe_wanted = false;
  // A returning friend must resume from this match, even if the old session became idle.
  int recovery_keyframe_from = -1;
  // Boundaries a friend has waited for this room's header (StepHeaderWait).
  int header_waited = 0;
  // Players waiting for a keyframe, and seats the session took in (until the room says they left).
  std::map<int, Arrival> waiting;
  std::map<int, Arrival> menu_waiting;
  u64 host_boundaries = 0;
  std::set<int> seated;
  // Seats taken in but not plugged in yet; the app hears "friend-joined" once all have.
  std::set<int> plugging;
  FriendsBatch batch;
  bool doubles = false;
  std::set<int> doubles_seats;
  std::vector<Orca::Net::KeyframeInfo> shared_keyframes;
  // Host: hold waiting friends while in a single-player mode (the module's Read().DropInHeld()).
  bool holding = false;
  // Host: its room ended without this player leaving it. Reported once, then reopened with a
  // growing backoff; `home_code` is the last room that worked.
  bool room_lost = false;
  bool reopen_gave_up = false;
  unsigned reopen_attempts = 0;
  std::optional<Clock::time_point> network_failure_since;
  Clock::time_point room_lost_at{}, reopen_at{};
  std::chrono::seconds reopen_backoff{0};
  std::string home_code;
  // A new room to open shortly after a leave, so a quit right after it never requests a ticket.
  std::optional<Clock::time_point> reopen_pending;
  // A former joiner not on port 1 has no room; the app is told once, at the next boundary.
  bool tell_no_room = false;

  // A joining player.
  std::unique_ptr<Download> download;
  std::unique_ptr<Download> replaying;
  // This machine's own origin (CaptureOrigin): the only state it ever restores for a join or a
  // fresh start. `origin_frame` is -1 until it is captured.
  MachineImage origin;
  std::vector<Orca::Net::NandEntry> origin_nand;
  int origin_frame = -1;
  u64 origin_hash = 0;
  u64 origin_ticks = 0;
  // Frames in a row the frame hook saw the built main menu, before the origin.
  int menu_built_streak = 0;
  int origin_words_streak = 0;
  OriginRule origin_rule = OriginRule::Menu;
  // No origin could be found (the menu never built in time): this game takes and joins nobody.
  bool origin_failed = false;
  Orca::Net::ReplayArchive replay;
  Clock::time_point replay_heartbeat{};
  int host_seat = 0;
  std::string host_name;
  bool host_here = false;
  Orca::Net::HostRoomWait host_room_wait;
  // The host holds this join while its game is in a single-player mode.
  bool host_holding = false;
  Clock::time_point boot_time{}, offered_time{}, loaded_time{};
  double load_ms = 0;
  bool joined = false;
  int leave_at = -1;
  bool leave_requested = false;
  // The opponent didn't pick in time on a ranked set's later game (CharOrder::NoShow): this
  // player's leave reports the room's match void first.
  bool opponent_no_show = false;
  std::string join_status;

  int last_percent = -1;

  u32 round_trip_sequence = 0;
  std::optional<Clock::time_point> last_return;
  Common::EventHook on_state_changed;

  // A join started during play ("join <code>"): this game plays solo until the keyframe is in.
  bool join_in_play = false;
  // Why the join failed (PumpJoin), for the caller to report.
  std::string join_error_code, join_error;

  // For the "orca stats" line: values at the last line, deepest rollback and stall time since.
  Clock::time_point stats_time{};
  int stats_frame = -1;
  // Video frames the game itself held shown frames for since then (LastFrameHeldFrames).
  int stats_held = 0;
  Orca::Net::Stats stats_last{};
  int stats_rbmax = 0;
  double stats_stall_ms = 0;
  // The local controller's input-age totals at the last line (Orca::Events::InputAge).
  Orca::Events::InputAge stats_input{};
  // Stalls on the other players across steps, for the current session.
  StallBudget stall_budget;

  // Each port's player name and own controls, and the frame they apply from. The host decides them
  // and sends them with keyframe offers, so every game writes the same values each frame. A short
  // history per seat lets a re-run frame get the values it had.
  std::map<int, std::vector<Orca::Net::KeyframeInfo::Name>> port_names;
  // Version of port_names (Session::RequireValues). A joiner holds the host's newest, keeping one
  // heard mid-download for after the load.
  int names_version = 0;
  std::optional<Orca::Net::KeyframeInfo> later_names;
  // The ports the UI was last told of.
  std::vector<Orca::Events::PortInfo> told_ports;
  // A joiner's host left during a re-run; handled at the next boundary that isn't one.
  std::optional<std::string> host_gone;
  // Host: frames its session has run with nobody else in it (IDLE_GRACE_FRAMES).
  int idle_frames = 0;

  // A ranked room still deciding the set after the opponent left: stay in it, solo, until the
  // verdict closes it or this time passes. See ORCA.md, "Matchmaking".
  std::optional<Clock::time_point> linger_until;
  // Host: the app was told the opponent left ("friend-left <why>") since they joined.
  bool friend_left_told = false;

  // Queue image: this player's own game (its character select) when the queue matched it, restored
  // once that room is over. Armed once the room was actually entered.
  struct QueueImage
  {
    int frame = 0;
    int local_seat = 0;
    Clock::time_point loaded_time{};
    MachineImage image;
    std::vector<Orca::Net::NandEntry> nand;
    Orca::Net::ReplayArchive replay;
    bool armed = false;
    Clock::time_point taken{};
  };
  std::unique_ptr<QueueImage> queue_image;
  // Trusted local return point while an unverified replay is being rebuilt.
  std::unique_ptr<QueueImage> before_join;
  bool want_queue_image = false;
  // A fight was played in the ranked room this queue image is for: the restored character select
  // comes back not ready, so Start searches again.
  bool ranked_fought = false;

  // Fresh starts (FreshStart). `fresh_pending`: an exit (0 for Reset) due at the next
  // boundary. `fresh_queue_armed`: `host` matched this game, so its queue room fresh-starts once
  // the welcome names it; `fresh_room` is the room the last fresh start was for.
  std::optional<u8> fresh_pending;
  std::optional<Clock::time_point> fresh_join_warned_at;
  bool fresh_queue_armed = false;
  std::string fresh_room;
  // The friends waiting now join in place (DecideFreshStart said None for them).
  bool friends_in_place = false;
  // The fresh start under way: its unseen tail, then (a queue room) neutral pads until the room's
  // header is in, then port 1's own pick put back (Module::Css() ArmOwnPick).
  struct Fresh
  {
    enum class Phase
    {
      Tail,
      Hold,
      Steer,
    };
    Phase phase = Phase::Tail;
    FreshKind kind = FreshKind::Menu;
    u8 exit = 0;
    bool queue = false;
    std::string room;
    // Frames in a row the tail has seen the character select up.
    int css_streak = 0;
    Clock::time_point started{};
    // A queue room's hold and steer run unseen past the tail (FreshUnseen): whether they still do,
    // how many frames have, and when the tail ended.
    bool unseen = false;
    int unseen_frames = 0;
    Clock::time_point tail_end{};
  };
  std::optional<Fresh> fresh;
  // ORCA_TEST_FAST_UNTIL: solo frames run unthrottled, until then or the first fresh start or
  // session (`fast_spent`).
  bool fast = false;
  bool fast_spent = false;

  // A Casual or Ranked pick handed over while this game wasn't alone, at a boundary the app's
  // commands may not take (TakeLobbyPickNow): retried at the next one while it still stands
  // (Games/Brawl/OnlineMenu.h PickStands).
  std::optional<Orca::UX::OnlinePick> lobby_pick;
  // A pick this host announced unarmed to keep its friends, armed once they are gone and it still
  // stands (ArmKeptPick).
  std::optional<Orca::UX::OnlinePick> kept_pick;
  // The pick whose queue this host ended for a friend, and whether a friend came in since: armed
  // again once they are all gone (RearmFriendsPick).
  std::optional<Orca::UX::OnlinePick> friends_pick;
  bool friends_pick_played = false;
  // Set over: after a ranked verdict this game stays in the room until the player leaves
  // (SetOverLeave). Tracks the leave, B held since, and the last buttons (to spot presses).
  bool set_over_left = false;
  bool set_over_hint = false;  // the overlay's leave hint is shown
  std::optional<Clock::time_point> set_over_hold_since;
  u16 set_over_buttons = 0xFFFF;
  // This machine's history as a replay stream, and a replay played instead of a game
  // (ReplayStream.h).
  std::unique_ptr<ReplayStream::Writer> replay_out;
  std::unique_ptr<ReplayStream::Player> replay_play;
};

// Desyncs this process has seen across all matches (the stats line's "ds").
int s_desyncs = 0;

// Runs the trusted frame hook and retains full-RAM hashes for unlabeled histories.
void ReplayFrameHook(Core::System& system, Match& match, int frame, bool resimulating,
                     const std::vector<Orca::Events::PortInfo>& ports, bool read_own,
                     const FrameHook& on_frame, bool restored = false)
{
  auto& replay = match.replay;
  if (restored)
    replay.checksums.erase(replay.checksums.lower_bound(frame), replay.checksums.end());
  on_frame(frame, resimulating, ports, read_own);
  if (frame % 60 == 0 && replay.first_frame >= 0)
  {
    auto& memory = system.GetMemory();
    replay.checksums[frame] = RamChecksum(
        {memory.GetRAM(), memory.GetRamSize()},
        {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
  }
}

// The local pad the last boundary read, for set over's B and Start.
std::optional<Orca::Net::Pad> s_local_pad;

// Solo pause. s_solo_idle: the last boundary left this game alone (false while a boundary runs).
// s_pausing: a pause is in effect, so boundaries take nobody in. Sequentially consistent: either
// the pause sees a running boundary, or the boundary sees the pause.
std::atomic<bool> s_solo_idle{false};
std::atomic<bool> s_pausing{false};
// Paused host time, for the stats line. Pause and resume arrive on the host thread; 0 = not paused.
std::atomic<s64> s_paused_since_ns{0};
std::atomic<s64> s_paused_ns{0};
// All paused host time so far, for this machine's speed (Orca/Perf.h), which never takes it.
std::atomic<s64> s_paused_total_ns{0};

s64 NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

void AddName(Match& match, Orca::Net::KeyframeInfo::Name name);
std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match);
void SetNames(Match& match, const Orca::Net::KeyframeInfo& from_host);
void RefreshOwnValues(Match& match, int frame);
std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame);
void Leave(Match& match);
void JoinInPlay(Match& match, const std::string& code, bool keep_room = false);

Match& TheMatch()
{
  static Match match;
  return match;
}

// This game plays alone with no drop-in under way (SoloPauseAllowed).
bool SoloIdle(const Match& match)
{
  return match.started && !match.finished && match.port && !match.session && !match.joining &&
         !match.download && !match.job && !match.keyframe_wanted && match.waiting.empty() &&
         match.plugging.empty() && !match.port->CatchingUp();
}

// Emulation is stopping, or the host asked the CPU thread to pause and waits for it: a loop holding
// the CPU thread must give up rather than deadlock.
bool Stopping(Core::System& system)
{
  return Core::GetState(system) == Core::State::Stopping ||
         system.GetCPU().GetState() != CPU::State::Running;
}

// Two frame periods at 60 Hz: a frame slower than this was a hitch.
constexpr std::chrono::microseconds HITCH_TIME{33'334};
// A peer frozen with its connection open: after this long the host unplugs it, a joiner stops.
constexpr auto PEER_SILENCE_LIMIT = Orca::Net::PEER_SILENCE_LIMIT;
constexpr char PEER_SILENT_SENTENCE[] = "Your friend's game stopped responding";
// A joiner past its unplug frame waits this long for the host's final acknowledgement
// (Session::LeftDone), then leaves anyway.
constexpr std::chrono::seconds LEAVE_SILENCE_LIMIT{3};
// Host: frames the session keeps running after everyone left, so a leaving friend still gets the
// host's last acknowledgement if a packet is lost.
constexpr int IDLE_GRACE_FRAMES = 12;
// The rollback window (Config::max_rollback); RingPort keeps this many snapshots plus two.
constexpr int MAX_ROLLBACK = 7;
// No keyframe in a boot's first frames: Project+'s frames 3-9 after its launcher run differently
// when the JIT holds other blocks, as a joiner's does. Early friends wait for frame 300 (5 s).
constexpr int FIRST_KEYFRAME_FRAME = 300;
// FIRST_KEYFRAME_FRAME, or a mod's own (mod.json firstKeyframeFrame, Core/Orca/Mod.h).
int FirstKeyframeFrame()
{
  return Orca::Mod::ActiveMod() ? static_cast<int>(Orca::Mod::FirstKeyframeFrame()) :
                                  FIRST_KEYFRAME_FRAME;
}
// A joiner far behind the host runs unthrottled.
constexpr int CATCH_UP_BEHIND = 12;
// How long a joiner waits for the host's keyframe before it gives up.
constexpr std::chrono::seconds JOIN_LIMIT{90};
// How long the rooms Worker holds a disconnected player's seat (AWAY_MS).
constexpr auto ROOM_HOLD = Orca::Net::ROOM_HOLD;
// How long to wait in a ranked room for the verdict after the opponent left (forfeit after 15 s).
constexpr std::chrono::seconds VERDICT_WAIT{30};
// Ranked: the opponent's inputs stopped this long, so this player claims the set (ReportStall).
constexpr std::chrono::seconds RANKED_STALL_LIMIT{15};
// Set over: holding B this long on the results screen leaves the room.
constexpr std::chrono::milliseconds SET_OVER_HOLD{1500};

// Orca's input delay when the page chooses none (no "caps delay", no "delay N" yet, or an old
// page's "delay auto"): a fixed three frames, the page's own default. Never the session's adaptive
// delay: players want a delay that never moves with the link; lag shows as rollbacks or stalls.
constexpr int DEFAULT_DELAY = 3;

// The input delay this player plays at: the page's "delay N" (with "caps delay"), else
// DEFAULT_DELAY. Always fixed.
std::optional<int> ChosenDelay()
{
  if (Orca::Status::Cap("delay"))
  {
    if (const std::optional<int> chosen = Orca::Online::ChosenDelay())
      return chosen;
  }
  return DEFAULT_DELAY;
}

// ---- Test knobs (UX/Probe.h TestKnobsAllowed) ----
// ORCA_TEST_FRESH=off: no fresh starts; every join replays from the host's origin in place.
bool FreshStartsOn()
{
  static const bool off = Orca::GetEnv("ORCA_TEST_FRESH") == "off";
  return !(off && Orca::UX::TestKnobsAllowed());
}
// ORCA_TEST_FRESH_AFTER=<frames>: a friend fresh-starts a host whose history is longer than this.
int FreshFriendsAfter()
{
  static const int after = [] {
    const std::string value = Orca::GetEnv("ORCA_TEST_FRESH_AFTER");
    return value.empty() ? -1 : std::max(0, std::atoi(value.c_str()));
  }();
  return after >= 0 && Orca::UX::TestKnobsAllowed() ? after : -1;
}
// ORCA_TEST_FAST_UNTIL=<frame>: solo frames before it run unthrottled, to age a host quickly; never
// after its first fresh start or session.
int FastUntil()
{
  static const int until = std::atoi(Orca::GetEnv("ORCA_TEST_FAST_UNTIL").c_str());
  return until > 0 && Orca::UX::TestKnobsAllowed() ? until : 0;
}

// A new history's seed for the game's random numbers (ReplayFrame::seed, Games/Brawl/OnlineMenu.h
// FreshMove::ApplySeed): fresh for every boot and every fresh start, so RANDOM on the character
// select, and every draw the game makes, differ from one match to the next. The host draws it and
// the replay carries it, so a joiner's rebuild starts from the same numbers. 0 (the canonical
// boot's own numbers, as every Orca before this) with no YouGame room; offline harness runs have no
// history at all, so they repeat. In a dev game's room ORCA_TEST_RNG_SEED=<n> (a test knob) fixes
// it.
u32 NewHistorySeed()
{
  if (Orca::UX::TestKnobsAllowed())
  {
    static const std::string fixed = Orca::GetEnv("ORCA_TEST_RNG_SEED");
    if (!fixed.empty())
      return static_cast<u32>(std::strtoul(fixed.c_str(), nullptr, 0));
  }
  if (!Orca::Online::Enabled())
    return 0;
  std::random_device device;
  u32 seed = 0;
  while (seed == 0)
    seed = static_cast<u32>(device());
  return seed;
}

// The origin has been captured; frames from it on are recorded.
bool OriginReady(const Match& match)
{
  return match.origin_frame >= 0;
}

// Before the origin, every Orca plays the canonical boot (CanonicalBootPads).
bool CanonicalBoot(const Match& match)
{
  return !OriginReady(match) && !match.origin_failed;
}

// A fresh start's unseen tail is running.
bool InTail(const Match& match)
{
  return match.fresh && match.fresh->phase == Match::Fresh::Phase::Tail;
}

// A queue room's fresh start runs its hold or steer unseen and unthrottled (FreshUnseen).
bool Unseen(const Match& match)
{
  return match.fresh && match.fresh->unseen && !InTail(match) && match.port &&
         match.port->CatchingUp();
}

// The unseen hold or steer is over: from the next frame the game shows and keeps time again.
void ShowFresh(Match& match, const char* why)
{
  if (!match.fresh || !match.fresh->unseen)
    return;
  Match::Fresh& fresh = *match.fresh;
  fresh.unseen = false;
  if (!InTail(match) && match.port)
    match.port->SetCatchingUp(false);
  // The stats line's frame rate starts over: the unseen frames weren't this machine's pace.
  match.stats_frame = -1;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Drop-in: the fresh start shows again at frame {} after {} unseen "
                 "frames past its tail ({:.0f} ms){}{}",
                 match.running + 1, fresh.unseen_frames, MsSince(fresh.tail_end), why ? ": " : "",
                 why ? why : "");
}

// A round-trip sample the session has not seen yet (the room numbers its samples).
std::optional<int> NewRoundTrip(u32* last_sequence)
{
  u32 sequence = 0;
  const int round_trip_ms = Orca::Online::RoundTripMs(&sequence);
  if (round_trip_ms < 0 || sequence == *last_sequence)
    return std::nullopt;
  *last_sequence = sequence;
  return round_trip_ms;
}

// A new session takes the room's recent round trips, so its input delay fits from the first frame.
void SeedRoundTrips(Match& match)
{
  for (const int round_trip_ms : Orca::Online::RecentRoundTrips(&match.round_trip_sequence))
    match.session->OnRoundTrip(round_trip_ms);
}

// A joiner's single status line: each message replaces the last, and the join's end clears it.
void ShowJoinMessage(std::string message, u32 ms)
{
  if (std::ranges::all_of(message, Common::IsPrintableCharacter))
    OSD::AddTypedMessage(OSD::MessageType::OrcaJoin, std::move(message), ms);
}

void StopEmulation(Core::System& system, const std::string& message)
{
  Core::DisplayMessage(message, 10000);
  if (!Stopping(system))
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
}

IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}

Orca::Net::Config SessionConfig()
{
  Orca::Net::Config config;
  config.seats = Orca::Net::MAX_SEATS;
  config.per_peer_inputs = Orca::Online::RoomDoubles();
  config.max_rollback = MAX_ROLLBACK;
  // Snapshot spacing is local only (it changes no frame's result); a test may fix it.
  config.snapshot_every = Orca::TestSnapshotEvery();
  // So is the input delay.
  config.fixed_delay = ChosenDelay();
  return config;
}

// ---- Host ----

// Captures the state at the start of `frame` and stores it on a thread. Only from the boundary
// hook, with every earlier frame final.
void StartKeyframe(Core::System& system, Match& match, int frame,
                    std::optional<u64> final_hash = std::nullopt)
{
  for (auto& [seat, arrival] : match.waiting)
    arrival.next_keyframe_notice = 0;
  const auto capture_start = Clock::now();
  auto job = std::make_unique<KeyframeJob>();
  job->frame = frame;
  // The replay holds this game's frames from its origin (or its last fresh start) on.
  const Orca::Net::ReplayFrame* boundary = match.replay.At(frame);
  if (!boundary || static_cast<size_t>(frame) > Orca::Net::MAX_REPLAY_FRAMES)
  {
    job->error = "restart the game to accept more players";
    job->refusal = "replay_limit";
    job->done = true;
    match.job = std::move(job);
    return;
  }
  Orca::Net::ReplayArchive replay = match.replay;
  replay.boundary = *boundary;
  replay.frames.resize(static_cast<size_t>(frame - replay.first_frame));
  auto& memory = system.GetMemory();
  replay.target_hash = final_hash ? *final_hash :
      RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                  {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
  job->capture_ms = MsSince(capture_start);
  KeyframeJob* const raw = job.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  raw->thread = std::thread([raw, store, captured_replay = std::move(replay)] {
    try
    {
      const auto pack_start = Clock::now();
      std::vector<u8> blob = Orca::Net::PackKeyframe(raw->frame, captured_replay);
      raw->raw_size = captured_replay.frames.size() * sizeof(Orca::Net::ReplayFrame);
      if (blob.empty())
      {
        raw->error = "the game's replay is too large; start a new game to accept more players";
        raw->refusal = "replay_limit";
      }
      if (raw->error.empty() && !Orca::Net::EncryptKeyframe(raw->frame, &blob, &raw->info.key))
        raw->error = "encryption failed";
      raw->pack_ms = MsSince(pack_start);
      if (raw->error.empty())
      {
        raw->info.frame = raw->frame;
        raw->info.size = blob.size();
        raw->info.hash = Orca::Net::KeyframeHash(blob);
        raw->info.id = fmt::format("replay-{}-{}", raw->frame, raw->info.hash.substr(0, 8));
        if (Orca::Net::InlineKeyframe(blob.size()))
        {
          // Small enough to travel in the room's offer itself (Keyframe.h INLINE_KEYFRAME_MAX):
          // nothing goes to the store, so there is nothing to delete later either.
          raw->info.inline_blob = std::move(blob);
          raw->ok = true;
        }
        else
        {
          const auto put_start = Clock::now();
          raw->ok = store->Put(raw->info, blob, &raw->error);
          raw->put_ms = MsSince(put_start);
          if (!raw->ok)
            raw->refusal = Orca::Net::LastRefusal();
        }
      }
    }
    catch (const std::exception&)
    {
      raw->error = "couldn't prepare the game's replay";
    }
    raw->done = true;
  });
  match.job = std::move(job);
}


void EndFresh(Match& match, const char* why);

// Port 1's pad while a fresh start runs (FreshStart): nothing pressed through the tail and, in a
// queue room, until the room's header is in; then the player's own pick put back by its own
// inputs (Module::Css() ArmOwnPick); then the player's controller.
Pad FreshPad(Match& match, const Pad& local, int frame)
{
  if (!match.fresh)
    return local;
  Match::Fresh& fresh = *match.fresh;
  using Phase = Match::Fresh::Phase;
  // The queue room went away while holding: the player plays on.
  if (fresh.phase != Phase::Tail && fresh.queue &&
      (Orca::Online::RoomEnded() || Orca::Online::Code() != fresh.room))
  {
    EndFresh(match, "the room is gone");
    return local;
  }
  if (fresh.phase == Phase::Hold && Orca::UX::Game().Header().HeaderInPlace())
  {
    const Orca::UX::QueueApp::Identity pick = Orca::UX::QueueApp::OwnIdentity();
    if (Orca::Status::Cap("queue2") && pick.HasPick())
    {
      fresh.phase = Phase::Steer;
      Orca::UX::Game().Css()->ArmOwnPick(pick);
    }
    else
    {
      EndFresh(match, "the room's header is in");
      return local;
    }
  }
  if (fresh.phase == Phase::Steer)
  {
    if (!Orca::UX::Game().Css()->OwnPickArmed())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port 1's pick is back at frame {} ({:.0f} ms after the "
                               "fresh start)",
                     frame, MsSince(fresh.started));
      EndFresh(match, nullptr);
      return local;
    }
    const std::optional<GCPadStatus> steer = Orca::UX::Game().Css()->OwnPickPad();
    return steer ? Orca::Net::EncodePad(*steer) : Pad{};
  }
  return Pad{};
}

// The tail is over once the character select has been up FRESH_CSS_SETTLE frames (this frame's
// hook read it), or after FRESH_TAIL_LIMIT frames. Then the game runs and shows as usual.
void StepTail(Match& match, int frame)
{
  if (!InTail(match))
    return;
  Match::Fresh& fresh = *match.fresh;
  fresh.css_streak = Orca::UX::Game().Read().LastSeen().css ? fresh.css_streak + 1 : 0;
  const int length = frame - match.origin_frame + 1;
  if (!FreshTailSettled(fresh.kind, fresh.css_streak, length))
    return;
  if (fresh.kind == FreshKind::Menu && fresh.css_streak < FRESH_CSS_SETTLE)
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: the fresh start's tail found no character select in {} "
                           "frames; it ends at frame {}",
                 FRESH_TAIL_LIMIT, frame);
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the fresh start's tail ran frames {}-{} in {:.0f} ms",
                 match.origin_frame, frame, MsSince(fresh.started));
  // The readers start over on the character select, silently.
  Orca::Events::NoteResync();
  match.stats_frame = -1;
  // A queue room's host holds port 1 until the room's header is in, then puts its own pick back
  // (FreshPad), still unseen and unthrottled (FreshUnseen); a friend's fresh start is over.
  if (fresh.queue && fresh.kind == FreshKind::Menu)
  {
    fresh.phase = Match::Fresh::Phase::Hold;
    fresh.unseen = FreshUnseen(true, 0);
    fresh.unseen_frames = 0;
    fresh.tail_end = Clock::now();
    if (!fresh.unseen)
      match.port->SetCatchingUp(false);
  }
  else
  {
    match.port->SetCatchingUp(false);
    match.fresh.reset();
  }
}

// One frame of the unseen hold or steer ran: past FRESH_UNSEEN_LIMIT the rest shows as before.
void StepUnseen(Match& match)
{
  if (!match.fresh || !match.fresh->unseen || InTail(match))
    return;
  Match::Fresh& fresh = *match.fresh;
  ++fresh.unseen_frames;
  if (!FreshUnseen(fresh.queue, fresh.unseen_frames))
    ShowFresh(match, "its unseen frames ran out");
}

void RunSolo(Match& match, const std::function<Pad(int)>& local_pad)
{
  // The local pad is read every frame, even when the frame doesn't use it, so the app's stream and
  // the input stats see each frame.
  Pad pad;
  if (!match.queued_local.empty())
  {
    pad = match.queued_local.front();
    match.queued_local.pop_front();
  }
  else
  {
    pad = local_pad(match.local_seat);
  }
  if (match.doubles && !match.waiting.empty() && Orca::UX::Game().Read().OnTheMenus())
    pad[0] &= static_cast<u8>(~(PAD_BUTTON_START >> 8));
  const int frame = match.running + 1;
  if (const int until = FastUntil();
      until > 0 && (!match.fast_spent && frame < until) != match.fast)
  {
    match.fast = !match.fast_spent && frame < until;
    Core::SetIsThrottlerTempDisabled(match.fast);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: frames run {} from frame {} (ORCA_TEST_FAST_UNTIL)",
                   match.fast ? "unthrottled" : "at their own pace", frame);
  }
  Pads pads;
  if (CanonicalBoot(match))
  {
    // Every Orca's boot plays the same inputs to its origin, whatever its seat or controller.
    pads = CanonicalBootPads();
  }
  else
  {
    pads.fill(Orca::Net::UNPLUGGED_PAD);
    pads[match.local_seat] = FreshPad(match, pad, frame);
  }
  match.port->SetPads(frame, pads);
  match.log.push_back(pads);
  match.running = frame;
  // Before StepTail: the tail's own last frame isn't one of the unseen hold's.
  StepUnseen(match);
  StepTail(match, frame);
  // No keyframe stored or in progress: only the newest frames can serve the next, so cap the log.
  if (!match.keyframe && !match.job && match.log.size() > 4 * KEYFRAME_FRESH_FRAMES)
  {
    const int drop = static_cast<int>(match.log.size()) - KEYFRAME_FRESH_FRAMES;
    match.log.erase(match.log.begin(), match.log.begin() + drop);
    match.log_base += drop;
  }
}

// Host: unplug every friend still in the session, telling each (and the app) why.
void DropFriends(Match& match, const std::string& reason, bool announce = true)
{
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (seat == match.local_seat || match.session->Plan()[seat].live_from == Orca::Net::NEVER)
      continue;
    Orca::Online::DropPeer(seat, reason);
    match.session->DropSeat(seat);
  }
  match.seated.clear();
  match.plugging.clear();
  Orca::Online::SetOpponentPlugged(false);
  if (reason == "desync")
    Orca::Online::ReportDesync();
  if (announce)
  {
    Orca::Status::State("friend-left " + reason);
    match.friend_left_told = true;
  }
}

void ConfirmResults(Match& match);

void GoSolo(Match& match, const char* why, const char* state = "playing")
{
  // Report results already final; readings after them used predicted inputs and start over.
  ConfirmResults(match);
  Orca::Events::NoteResync();
  match.log = match.session->TakeLog(&match.log_base);
  const std::vector<Pad> pending = match.session->PendingLocal();
  match.queued_local.assign(pending.begin(), pending.end());
  Orca::Net::Stats stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to solo play at frame {} ({}): {} rollbacks, {} checksums matched",
                 match.running + 1, why, stats.rollbacks, stats.checksums_matched);
  match.session.reset();
  match.plugging.clear();
  match.idle_frames = 0;
  match.port->SetCatchingUp(false);
  // A session that ended mid re-run left the port in re-run mode (unthrottled, unrendered, muted).
  match.port->SetResimulating(false);
  if (state)
    Orca::Status::State(state);
}

// With "caps join", a failed join, the host leaving or a desync leaves this player solo.
bool Soft()
{
  return Orca::Status::Cap("join");
}

std::unique_ptr<Orca::Net::KeyframeStore> MakeStore()
{
  return Orca::Net::MakeKeyframeStore(
      [](bool fresh, std::string* ticket, std::string* store_url, std::string* error) {
        return Orca::Online::FreshTicket(fresh, ticket, store_url, error);
      });
}

// Deletes a keyframe from the store, unless it travelled inline in the room's offer and so was
// never stored (Keyframe.h INLINE_KEYFRAME_MAX).
void DeleteStored(Match& match, const Orca::Net::KeyframeInfo& info)
{
  if (info.shared && match.joining)
    return;
  if (match.store && info.inline_blob.empty())
    match.store->Delete(info.id);
}

// The fresh start's hold or steer is over (a tail always runs to its end: StepTail).
void EndFresh(Match& match, const char* why)
{
  if (!match.fresh || InTail(match))
    return;
  if (why)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the fresh start hands port 1 back to the player at frame {} "
                             "({})",
                   match.running + 1, why);
  }
  // An unseen hold or steer ends with it: the player sees and plays from this frame.
  ShowFresh(match, nullptr);
  if (match.fresh->phase == Match::Fresh::Phase::Steer)
    Orca::UX::Game().Css()->DisarmOwnPick();
  match.fresh.reset();
}

// Drops drop-in work for the room being left; the next room gets a new store.
void ResetDropIn(Match& match)
{
  match.opponent_no_show = false;
  match.replaying.reset();
  match.before_join.reset();
  // A fresh start due, armed or holding for this room is over with it.
  match.fresh_pending.reset();
  match.fresh_join_warned_at.reset();
  match.fresh_queue_armed = false;
  EndFresh(match, "the room is left");
  if (match.keyframe)
    DeleteStored(match, *match.keyframe);
  match.keyframe.reset();
  if (match.store)
    match.store->Cancel();
  match.waiting.clear();
  match.menu_waiting.clear();
  match.batch.Clear();
  match.doubles = false;
  match.doubles_seats.clear();
  for (const auto& shared : match.shared_keyframes)
    DeleteStored(match, shared);
  match.shared_keyframes.clear();
  match.seated.clear();
  match.plugging.clear();
  match.keyframe_wanted = false;
  match.header_waited = 0;
  // Wind down the job, the download and the old store (a delete can retry for seconds) on a
  // detached thread; job and download first, since they use the store.
  std::thread([job = std::move(match.job), download = std::move(match.download),
               store = std::move(match.store)]() mutable {
    job.reset();
    download.reset();
    store.reset();
  }).detach();
  match.store = MakeStore();
}

// The online rules' mode for a room's queue (OnlineRules.h): casual or ranked, else none.
Orca::Engine::Mode QueueMode(const std::string& queue)
{
  if (queue == "casual")
    return Orca::Engine::Mode::Casual;
  if (queue == "ranked")
    return Orca::Engine::Mode::Ranked;
  return Orca::Engine::Mode::None;
}

// Queue flags for a queue room's header (FLAG_QUEUE2 with `queue2`); host sets, joiner checks.
u8 RoomFlags()
{
  return Orca::Status::Cap("queue2") ? Orca::Engine::FLAG_QUEUE2 : u8{0};
}

// The header this game should carry, written only where HeaderFreeAt allows: a queue room's while
// hosting it on port 1, or while waiting for or staying after a ranked verdict; the queue's own
// character select's (FLAG_SOLO) while waiting there with `queue2`; otherwise none.
std::pair<Orca::Engine::Mode, u8> WantedRulesMode(const Match& match)
{
  using Orca::Engine::Mode;
  namespace MB = Orca::Engine;
  if (match.joining)
    return {Mode::None, 0};
  const Mode mode =
      Orca::Online::RoomEnded() ? Mode::None : QueueMode(Orca::Online::RoomQueue());
  if (mode != Mode::None &&
      (match.local_seat == 0 ||
       ((match.linger_until || Orca::Online::SetOver()) && mode == Mode::Ranked)))
  {
    return {mode, RoomFlags()};
  }
  if (mode == Mode::None && Orca::UX::QueueApp::Active() && Orca::Status::Cap("queue2") &&
      match.local_seat == 0 && !match.session && !match.linger_until)
  {
    return {Orca::UX::QueueApp::Ranked() ? Mode::Ranked : Mode::Casual,
            static_cast<u8>(MB::FLAG_SOLO | MB::FLAG_QUEUE2)};
  }
  return {Mode::None, 0};
}

// Tells the online rules which header to carry now. Runs before the frame hook and again before a
// keyframe, since a room welcome can arrive in between.
void UpdateWanted(const Match& match)
{
  const auto [mode, flags] = WantedRulesMode(match);
  const bool room = mode != Orca::Engine::Mode::None &&
                    (flags & Orca::Engine::FLAG_SOLO) == 0;
  if (Orca::UX::Game().Header().SetWanted(mode, flags, room ? Orca::Online::Code() : std::string()))
  {
    DEBUG_LOG_FMT(ROLLBACK, "Online rules: header {} flags {:#04x} wanted from frame {}",
                  static_cast<int>(mode), flags, match.running + 1);
  }
}

// A keyframe nobody took, made under another header than wanted now, is for the wrong match.
void DropStaleKeyframe(Match& match)
{
  if (!match.keyframe || match.session ||
      match.keyframe_generation == Orca::UX::Game().Header().WantedGeneration())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} dropped: made for another header",
                 match.keyframe->frame);
  DeleteStored(match, *match.keyframe);
  match.keyframe.reset();
  if (!match.waiting.empty())
    match.keyframe_wanted = true;
}

// Nothing a friend would replay covers this boundary's frame (HeaderFreeAt's `solo_quiet`).
bool SoloQuiet(const Match& match)
{
  return match.started && !match.finished && match.port && !match.session && !match.joining &&
         !match.download && !match.job && match.plugging.empty() &&
         (!match.port->CatchingUp() ||
          HeaderFreeWhileCatchingUp(Unseen(match), InTail(match), match.joining));
}

// Host of a queue room whose game never carried the room's header while the opponent waited: the
// joiner would refuse the keyframe, so report `orca error mismatch` and leave, as a joiner does
// (LeaveOnMismatch). Both players search again.
void FailHeaderWait(Match& match)
{
  WARN_LOG_FMT(ROLLBACK,
               "Online rules: room {}: this game's header isn't the room's after {} boundaries "
               "with the opponent waiting, frame {}: leaving (mismatch)",
               Orca::Online::Code(), HEADER_FAIL_BOUNDARIES, match.running + 1);
  Orca::Online::SetLeaveReason("mismatch");
  Orca::Status::Report("mismatch", "That match couldn't start");
  Leave(match);
}

// Forgets what KeepRoomOpen knew of a room this player left on purpose.
void ForgetRoom(Match& match)
{
  match.reopen_join = false;
  match.room_lost = false;
  match.reopen_gave_up = false;
  match.reopen_attempts = 0;
  match.network_failure_since.reset();
  match.reopen_backoff = {};
  match.home_code.clear();
  match.reopen_pending.reset();
  match.tell_no_room = false;
}

// Stop sends "leave" then "quit" at once; the delay keeps the quit from finding a ticket request.
constexpr std::chrono::seconds REOPEN_DELAY{1};

// Solo again: on port 1, open a room of its own; elsewhere none ("no-room"): hosts play port 1.
void ReopenRoom(Match& match)
{
  ForgetRoom(match);
  Orca::Online::ShutdownInBackground();
  if (match.local_seat == 0)
    match.reopen_pending = Clock::now() + REOPEN_DELAY;
  else
    match.tell_no_room = true;
}

// A former joiner moves to port 1 from the next frame, like a host; only this game changes.
void Reseat(Match& match)
{
  if (match.local_seat == 0)
    return;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} moves to port 1 from frame {}", match.local_seat + 1,
                 match.running + 1);
  match.local_seat = 0;
  // Names came from the old host's roster; this player's own comes from its new room.
  match.port_names.clear();
}

// Host: its friends left. Leave a matchmade room for one of its own, unless a ranked set is still
// being decided: then wait for the room's verdict (up to VERDICT_WAIT).
void AfterFriendsGone(Match& match)
{
  Orca::Online::SetOpponentPlugged(false);
  const std::string queue = Orca::Online::RoomQueue();
  if (queue == "private" || Orca::Online::RoomEnded() || match.linger_until)
    return;
  // Tell the app the opponent left now, since this game won't be in the room to hear it.
  if (!std::exchange(match.friend_left_told, true))
    Orca::Status::State("friend-left left");
  // Set over: stay until the player leaves (SetOverLeave); that leave is not a forfeit.
  if (queue == "ranked" && Orca::Online::SetOver())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the opponent left the set's room (set over): this "
                             "game stays until its player leaves");
    return;
  }
  if (queue == "ranked" && Orca::Online::MatchLive())
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Matchmaking: the opponent went mid-set: waiting for the room's verdict");
    match.linger_until = Clock::now() + VERDICT_WAIT;
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the {} pairing is over: a room of this game's own", queue);
  ResetDropIn(match);
  ReopenRoom(match);
}

// Host on port 1: its room ended without this player leaving, so friends can't drop in. Report
// once, then reopen after 2 s, backing off to 30 s. While the server holds a friends-room seat
// (ROOM_HOLD), reuse the code so pending invites still work; queue rooms need a fresh solo room.
void KeepRoomOpen(Match& match, Clock::time_point now = Clock::now())
{
  if (match.local_seat != 0)
    return;
  // The room ReopenRoom asked for, once its time comes.
  if (match.reopen_pending)
  {
    if (now < *match.reopen_pending)
      return;
    match.reopen_pending.reset();
    Orca::Online::StartRoom("", false);
    return;
  }
  const auto give_up = [&] {
    match.reopen_gave_up = true;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: room {} stopped reopening after {} attempts",
                   Orca::Online::Code(), match.reopen_attempts);
    Orca::Status::Report("network", "The room couldn't reconnect. Your game continues solo; "
                                     "start a new game to open a room.");
    // Both old and new apps know this state and offer a new game, without retrying this one.
    Orca::Status::Event("no-room");
  };
  if (!Orca::Online::RoomEnded())
  {
    if (Orca::Online::InRoom())
    {
      if (Orca::Online::Joining().value_or(false))
      {
        if (Pausing())
          return;
        JoinInPlay(match, Orca::Online::Code(), true);
        match.reopen_join = true;
        return;
      }
      if (match.room_lost)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: hosting room {} again", Orca::Online::Code());
        // Clear the soft error the page keeps showing about the lost room.
        if (Soft())
          Orca::Status::Event("playing");
      }
      match.room_lost = false;
      match.reopen_gave_up = false;
      match.reopen_attempts = 0;
      match.network_failure_since.reset();
      match.reopen_backoff = {};
      match.home_code = Orca::Online::Code();
    }
    else if (!match.reopen_gave_up && !match.session && match.network_failure_since &&
             now - *match.network_failure_since >= std::chrono::minutes(5))
    {
      give_up();
      // A slow ticket or upgrade cannot extend the outage's deadline.
      Orca::Online::ShutdownInBackground();
    }
    return;
  }
  // A session ends with its room: it fails at its next step and the host goes solo.
  if (match.session)
    return;
  // No error code: this player left the room itself (the app's Leave).
  const std::string code = Orca::Online::RoomErrorCode();
  if (code.empty())
    return;
  // A matchmade room closed after its result: never rejoin that code; open a fresh room.
  if (code == "match-over")
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the room closed after its result; a room of its own");
    Orca::Status::Event("friend-left match-over");
    Orca::UX::Search::End();
    match.linger_until.reset();
    ResetDropIn(match);
    ForgetRoom(match);
    Orca::Online::ShutdownInBackground();
    Orca::Online::StartRoom("", false);
    return;
  }
  if (match.reopen_gave_up)
    return;
  if (code == "network")
  {
    if (!match.network_failure_since)
      match.network_failure_since = now;
  }
  else
  {
    match.network_failure_since.reset();
  }
  if (match.reopen_attempts >= 10 ||
      (match.network_failure_since && now - *match.network_failure_since >= std::chrono::minutes(5)))
  {
    give_up();
    return;
  }
  if (!match.room_lost)
  {
    match.room_lost = true;
    match.room_lost_at = now;
    match.reopen_backoff = std::chrono::seconds(2);
    match.reopen_at = now + match.reopen_backoff;
    const std::string why = Orca::Online::StatusLine();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: this game's room ended ({}: {}); another opens in {} s",
                   code, why, match.reopen_backoff.count());
    Core::DisplayMessage(why, 6000);
    if (Soft())
    {
      if (Orca::Online::RoomTaken() || code == "room_full")
        Orca::Status::Event("playing");
      else
        Orca::Status::Report("network", fmt::format("{}: your game opens its room again", why));
    }
    // Whoever waited for a keyframe was in that room.
    ResetDropIn(match);
  }
  // Forget queue codes before trying a private room, including on later retries.
  match.home_code = RoomReopenCode(match.home_code, Orca::Online::RoomQueue(),
                                   now - match.room_lost_at < ROOM_HOLD,
                                   Orca::Online::RoomTaken() || code == "room_full");
  if (now < match.reopen_at)
    return;
  const std::string reopen = match.home_code;
  ++match.reopen_attempts;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: reopening room {} attempt={} (previous room={}, ended: {}: {})",
                 reopen.empty() ? "new" : reopen, match.reopen_attempts, Orca::Online::Code(), code,
                 Orca::Online::StatusLine());
  Orca::Online::ShutdownInBackground();
  // FailJoin keeps an automatic adoption on its own game even without "caps join".
  Orca::Online::StartRoom(reopen, false, true, !reopen.empty());
  match.reopen_backoff = std::min(match.reopen_backoff * 2, std::chrono::seconds(30));
  match.reopen_at = now + match.reopen_backoff;
}

// A joining player on its own again (it left, its host did, the session failed, or the join never
// finished): it plays on with every other controller unplugged. `own_boot`: no keyframe was ever
// loaded, so it plays its own game on port 1.
void BecomeSolo(Match& match, const char* why, const char* state, bool own_boot = false,
                bool keep_room = false)
{
  Orca::Events::NoteResync();
  if (match.session)
  {
    GoSolo(match, why, nullptr);
  }
  else
  {
    match.log.clear();
    match.log_base = match.running + 1;
    match.queued_local.clear();
  }
  if (own_boot)
    Reseat(match);
  match.joining = false;
  match.join_in_play = false;
  match.joined = false;
  match.host_here = false;
  match.host_holding = false;
  match.leave_requested = false;
  match.leave_at = -1;
  match.last_percent = -1;
  match.host_room_wait.Back();
  match.later_names.reset();
  OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
  Orca::UX::Search::JoinOver();
  match.port->SetCatchingUp(false);
  ResetDropIn(match);
  if (!keep_room)
    ReopenRoom(match);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port {} from frame {} ({})",
                 match.local_seat + 1, match.running + 1, why);
  if (state)
    Orca::Status::State(state);
}

// A held host seat can return. Keep the room and play solo until its fresh keyframe arrives.
bool WaitForAwayHost(Match& match)
{
  if (!match.joining || match.leave_requested || Orca::Online::RoomQueue() != "private" ||
      !match.host_room_wait.Start(Clock::now(), Orca::Online::HostAwayDeadline()))
    return false;
  const auto deadline = match.host_room_wait.until;
  BecomeSolo(match, "host room away", nullptr, false, true);
  match.host_room_wait.until = deadline;
  match.joining = true;
  match.join_in_play = true;
  match.host_gone.reset();
  match.boot_time = Clock::now();
  match.offered_time = match.boot_time;
  match.join_status.clear();
  Orca::Online::RetryPeer(match.host_seat);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: host room away; session ended, staying in room {} for back",
                 Orca::Online::Code());
  Orca::Status::Event("room-away host ended waiting-back");
  return true;
}

bool InQueueRoom();

// On the queue's own character select, or searching.
bool QueueOrSearch()
{
  return Orca::UX::QueueApp::Active() ||
         Orca::UX::Search::Current() != Orca::UX::Search::State::None;
}

// Host: a friend arrived or an invite is on its way while this player is on the queue. The queue
// ends as With Friends ends it (`orca menu cancel`), so the friend's keyframe has no queue header.
void EndQueueForFriend(Match& match, bool arrived, bool invited)
{
  FriendQueueInputs in;
  in.friend_coming = arrived || invited;
  in.queue_or_search = QueueOrSearch();
  in.local_seat = match.local_seat;
  in.joining = match.joining;
  in.friends_room = !Orca::Online::RoomEnded() && Orca::Online::Seat() >= 0 &&
                    Orca::Online::RoomQueue() == "private";
  if (!FriendEndsQueue(in))
    return;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Queue: a friend {} room {} at frame {} while this player is on the {} queue: "
                 "the queue is over (orca menu cancel), so the friend's keyframe carries no queue "
                 "header",
                 arrived ? "arrived in" : "is invited to", Orca::Online::Code(),
                 match.running + 1,
                 (Orca::UX::QueueApp::Active() ? Orca::UX::QueueApp::Ranked() :
                                              Orca::UX::Search::Ranked()) ?
                     "ranked" :
                     "casual");
  // Armed again once the friends are gone (RearmFriendsPick).
  if (Orca::UX::QueueApp::Active())
  {
    match.friends_pick =
        Orca::UX::QueueApp::Ranked() ? Orca::UX::OnlinePick::Ranked : Orca::UX::OnlinePick::Casual;
    match.friends_pick_played = false;
  }
  Orca::UX::Search::End();
  Orca::UX::QueueApp::End();
  Orca::Status::Menu("cancel");
}

void HostEvents(Core::System& system, Match& match, Orca::Net::YouGameRoom* test_room = nullptr,
                std::optional<bool> test_menus = std::nullopt)
{
  ++match.host_boundaries;
  match.doubles = test_room ? test_room->Doubles() : Orca::Online::RoomDoubles();
  const bool in_room = test_room ? test_room->Connected() : Orca::Online::InRoom();
  const bool in_queue_room = test_room ? QueueMode(test_room->Queue()) != Orca::Engine::Mode::None :
                                         InQueueRoom();
  // `host` also opens private/tournament rooms. Only a confirmed welcome can disarm the queue
  // start: before it, "private" is also the default for a queue room still connecting.
  if (match.fresh_queue_armed && in_room && !in_queue_room)
    match.fresh_queue_armed = false;
  const bool on_menus = test_menus.value_or(Orca::UX::Game().Read().OnTheMenus());
  if (on_menus)
  {
    match.waiting.merge(match.menu_waiting);
    match.menu_waiting.clear();
  }
  bool arrived = false;
  for (const Orca::Net::PeerEvent& event :
       (test_room ? test_room->TakePeerEvents() : Orca::Online::TakePeerEvents()))
  {
    if (event.kind == Orca::Net::PeerEvent::Kind::Arrived)
    {
      if (event.seat <= 0 || event.seat >= Orca::Net::MAX_SEATS || event.host)
      {
        WARN_LOG_FMT(ROLLBACK, "Drop-in: ignoring a player at seat {}{}", event.seat + 1,
                     event.host ? " that says it hosts" : "");
        continue;
      }
      // Legacy peers retain the one-friend limit. Opted-in peers use independent input ranges
      // and may rebuild together without withholding another player's inputs.
      const bool capable = match.doubles && event.doubles &&
          std::ranges::all_of(match.seated, [&](int seat) {
            return seat == event.seat || match.doubles_seats.contains(seat);
          }) && std::ranges::all_of(match.waiting, [&](const auto& entry) {
            return entry.first == event.seat || entry.second.doubles;
          }) && std::ranges::all_of(match.menu_waiting, [&](const auto& entry) {
            return entry.first == event.seat || entry.second.doubles;
          });
      if ((match.doubles && !event.doubles) || (!capable && (std::any_of(match.seated.begin(), match.seated.end(),
                      [&event](int seat) { return seat != event.seat; }) ||
          std::any_of(match.waiting.begin(), match.waiting.end(),
                      [&event](const auto& waiting) { return waiting.first != event.seat; }) ||
          std::any_of(match.menu_waiting.begin(), match.menu_waiting.end(),
                      [&event](const auto& waiting) { return waiting.first != event.seat; }))))
      {
        NOTICE_LOG_FMT(ROLLBACK,
                       "Drop-in: {} arrived for port {}: room_full",
                       event.name, event.seat + 1);
        if (test_room)
          test_room->DropPeer(event.seat, "room_full");
        else
          Orca::Online::DropPeer(event.seat, "room_full");
        continue;
      }
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} arrived for port {}", event.name, event.seat + 1);
      Core::DisplayMessage(fmt::format("{} is joining on port {}", event.name, event.seat + 1), 4000);
      auto& arrivals = match.doubles && !on_menus && !event.recovering ?
                          match.menu_waiting : match.waiting;
      arrivals[event.seat] = {event.name, event.controls, event.queue, event.recovering,
                              event.doubles, Clock::now(), match.host_boundaries};
      if (event.recovering)
      {
        if (match.doubles && match.session)
          match.session->DropSeat(event.seat);
        const int from = match.doubles && match.session && !match.session->LocalChecksums().empty() ?
                             match.session->LocalChecksums().rbegin()->first : match.running + 1;
        match.recovery_keyframe_from = std::max(match.recovery_keyframe_from, from);
      }
      match.keyframe_wanted = true;
      arrived = true;
      Orca::Status::State("friend-joining");
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::Left)
    {
      const bool was_waiting = (match.waiting.erase(event.seat) + match.menu_waiting.erase(event.seat)) > 0;
      match.plugging.erase(event.seat);
      match.doubles_seats.erase(event.seat);
      // Taken in (even if already unplugged and the host went solo): the app still hears it.
      const bool was_playing = match.seated.erase(event.seat) > 0;
      if (match.session)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room{}{}", event.seat + 1,
                       event.reason.empty() ? "" : ": ", event.reason);
        match.session->DropSeat(event.seat);
      }
      if (was_playing && match.seated.empty())
        Orca::Online::SetOpponentPlugged(false);
      // The app hears why a friend left (its bye: "desync", "stalled").
      if (was_waiting || was_playing)
      {
        Orca::Status::State("friend-left " +
                            (event.reason.empty() ? std::string("left") : event.reason));
        match.friend_left_told = true;
      }
    }
  }
  if (match.plugging.empty())
  {
    for (const auto& shared : match.shared_keyframes)
      DeleteStored(match, shared);
    match.shared_keyframes.clear();
  }
  // An invite on its way ends the queue, but makes no keyframe: the friend's arrival may
  // fresh-start this game first (DecideFreshStart).
  const bool invited = Orca::Online::TakePrepareJoin();
  // A game with no origin can't be rebuilt anywhere: say so now rather than at the join's limit.
  if (match.origin_failed && (!match.waiting.empty() || !match.menu_waiting.empty()))
  {
    for (const auto& [seat, arrival] : match.waiting)
      Orca::Online::DropPeer(seat, "refused");
    for (const auto& [seat, arrival] : match.menu_waiting)
      Orca::Online::DropPeer(seat, "refused");
    match.waiting.clear();
    match.menu_waiting.clear();
    match.keyframe_wanted = false;
    Orca::Status::State("friend-left refused");
    match.friend_left_told = true;
  }
  // A friend on the way ends this player's queue (before UpdateWanted).
  EndQueueForFriend(match, arrived, invited);
  // Refresh the wanted header: the room's welcome may have come after this boundary's hook.
  UpdateWanted(match);

  // Diagnose an arrival that never gets as far as capture, including joins held for the menus.
  const auto now = Clock::now();
  for (auto& [seat, arrival] : match.waiting)
  {
    if (match.job)
      arrival.next_keyframe_notice = 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - arrival.arrived_at);
    if (!arrival.next_keyframe_notice || elapsed.count() < arrival.next_keyframe_notice)
      continue;
    const int next = match.running + 1;
    const bool in_place = Orca::UX::Game().Header().HeaderInPlace();
    int waited = match.header_waited;
    const bool header_ready = HeaderWaitAt(&waited, InTail(match), Unseen(match), in_place,
                                           true, in_queue_room) == HeaderWait::Ready;
    const auto capture = ChooseJoinCapture(match.session.get(), match.doubles, next,
        std::max(match.replay.first_frame + 1, match.recovery_keyframe_from));
    const char* fresh_reason = !match.fresh ? "none" :
        InTail(match) ? "tail" : match.fresh->phase == Match::Fresh::Phase::Hold ?
        "queue header hold" : "restoring own pick";
    NOTICE_LOG_FMT(ROLLBACK,
                   "Drop-in: no keyframe yet {} s after {} arrived (wanted={}, header in place={}, "
                   "running frame {}, boundaries since arrival {}, store={}, job={}, header ready={}, "
                   "next={}, first keyframe frame={} (reached={}), recovery_keyframe_from={} "
                   "(reached={}), origin ready={}, replay.first_frame={} (past={}), fresh={} "
                   "(reason={}), capture present={}, fresh pending={}, queue fresh armed={}, "
                   "room ready={}, queue room={}, room queue={}, holding={})",
                   elapsed.count(), arrival.name, match.keyframe_wanted, in_place, match.running,
                   match.host_boundaries - arrival.arrived_boundary, match.store != nullptr,
                   match.job != nullptr, header_ready, next, FirstKeyframeFrame(),
                   next >= FirstKeyframeFrame(), match.recovery_keyframe_from,
                   next >= match.recovery_keyframe_from, OriginReady(match), match.replay.first_frame,
                   next > match.replay.first_frame, match.fresh.has_value(), fresh_reason,
                   capture.has_value(), match.fresh_pending.has_value(), match.fresh_queue_armed,
                   in_room, in_queue_room,
                   test_room ? test_room->Queue() : Orca::Online::RoomQueue(), match.holding);
    arrival.next_keyframe_notice = arrival.next_keyframe_notice == 15 ? 45 : 0;
  }

  // A finished keyframe: offer it to whoever waits, and drop the solo log before it.
  if (match.job && match.job->done)
  {
    std::unique_ptr<KeyframeJob> job = std::move(match.job);
    if (job->thread.joinable())
      job->thread.join();
    if (!job->ok && !job->refusal.empty())
    {
      // Refused for good (say, a signed-out ticket): another keyframe gets the same answer, so
      // whoever waits hears it now instead of after its join times out.
      const std::string why = job->refusal == "signed_out" || job->refusal == "replay_limit" ?
                                  job->refusal : "refused";
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} refused ({}): {}; {} waiting told",
                    job->frame, job->refusal, job->error, match.waiting.size());
      for (const auto& [seat, arrival] : match.waiting)
        Orca::Online::DropPeer(seat, why);
      if (!match.waiting.empty())
      {
        match.waiting.clear();
        Orca::Status::State("friend-left " + why);
        match.friend_left_told = true;
      }
      match.keyframe_wanted = false;
    }
    else if (!job->ok)
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} failed: {}", job->frame, job->error);
      // Whoever waits for it gets another try.
      match.keyframe_wanted = !match.waiting.empty();
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Drop-in: keyframe of frame {}: {} bytes raw, {} bytes {}; capture {:.1f} ms, "
                     "pack and encrypt {:.1f} ms, store {:.0f} ms",
                     job->frame, job->raw_size, job->info.size,
                     job->info.inline_blob.empty() ? "stored" : "inline in the room's offer",
                     job->capture_ms, job->pack_ms, job->put_ms);
      // An earlier keyframe nobody took (say, an invite not accepted yet) is replaced.
      if (match.keyframe)
        DeleteStored(match, *match.keyframe);
      match.keyframe = job->info;
      match.keyframe_generation = job->generation;
      if (!match.session && job->frame >= match.log_base &&
          job->frame <= match.log_base + static_cast<int>(match.log.size()))
      {
        match.log.erase(match.log.begin(), match.log.begin() + (job->frame - match.log_base));
        match.log_base = job->frame;
      }
    }
  }

  // A host in a single-player mode (Classic, Training, Events...) holds waiting friends until its
  // game is back in the menus (Games/Brawl/OnlineMenu.h SequenceHoldsDropIn).
  const bool recovering = !match.waiting.empty() && std::ranges::all_of(match.waiting, [](const auto& entry) {
    return entry.second.recovering;
  });
  const bool batch_ready = !match.doubles || match.batch.Ready(
      on_menus, static_cast<int>(match.waiting.size()),
      static_cast<int>(match.plugging.size()), recovering, Clock::now());
  const bool hold = !match.waiting.empty() &&
      (Orca::UX::Game().Read().DropInHeld() || !batch_ready);
  const bool notice_hold = hold || !match.menu_waiting.empty();
  if (notice_hold != match.holding)
  {
    match.holding = notice_hold;
    if (test_room)
      test_room->HoldJoins(notice_hold);
    else
      Orca::Online::HoldJoins(notice_hold);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} at frame {}",
                   notice_hold ? "the join waits for the menus or its batch" :
                          "the join goes on",
                   match.running + 1);
    if (notice_hold)
      Orca::Status::State("friend-holding");
    else if (!match.waiting.empty())
      Orca::Status::State("friend-joining");
  }
  if (hold || (match.waiting.empty() && !match.menu_waiting.empty()))
    return;

  // A fresh start first, so the friend or the queue's opponent replays only this match's frames.
  if (match.waiting.empty())
    match.friends_in_place = false;
  FreshInputs start;
  start.kind = FreshKindForOrigin(match.origin_rule);
  start.enabled = FreshStartsOn() && match.origin_frame > 0;
  start.origin_ready = OriginReady(match);
  start.joining = match.joining;
  start.session = match.session != nullptr;
  start.seated_or_plugging = !match.seated.empty() || !match.plugging.empty();
  start.busy = InTail(match) || match.job != nullptr || match.fresh_pending.has_value();
  start.queue_armed = match.fresh_queue_armed;
  start.in_queue_room = in_queue_room;
  start.waiting = !match.waiting.empty();
  start.in_place = match.friends_in_place ||
                   (!start.in_queue_room && std::ranges::any_of(match.waiting, [](const auto& entry) {
                     return entry.second.recovering;
                   }));
  start.single_player = Orca::UX::Game().Read().DropInHeld();
  start.history_frames = match.running + 1 - match.replay.first_frame;
  const u64 ticks = system.GetCoreTiming().GetTicks();
  start.history_time = std::chrono::duration<double>(
      static_cast<double>(ticks >= match.origin_ticks ? ticks - match.origin_ticks : 0) /
      system.GetSystemTimers().GetTicksPerSecond());
  start.fresh_after = FreshFriendsAfter();
  switch (WarnBeforeResetJoin(DecideFreshStart(start), start.kind, start.queue_armed,
                             &match.fresh_join_warned_at, Clock::now()))
  {
  case FreshStep::None:
    // Whoever waits now joins in place, even if the history passes the threshold meanwhile.
    match.friends_in_place = !match.waiting.empty();
    break;
  case FreshStep::Hold:
    return;
  case FreshStep::Warn:
    Orca::UX::ShowRestartForJoin(match.waiting.begin()->second.name);
    return;
  case FreshStep::Now:
    if (start.kind == FreshKind::Reset)
    {
      match.fresh_pending = 0;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a reset start at the next boundary (frame {})",
                     match.running + 2);
    }
    else if (start.queue_armed)
    {
      match.fresh_pending = Orca::Online::RoomQueue() == "ranked" ? Orca::Net::MENU_EXIT_RANKED :
                                                                    Orca::Net::MENU_EXIT_CASUAL;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} room {}: a fresh start at the next boundary (frame {})",
                     Orca::Online::RoomQueue(), Orca::Online::Code(), match.running + 2);
    }
    else
    {
      const std::optional<Orca::UX::OnlinePick> pick = Orca::UX::Game().Read().CssPick();
      match.fresh_pending = FriendsFreshExit(
          pick ? std::optional(*pick == Orca::UX::OnlinePick::Ranked) : std::nullopt);
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a friend arrives after {} frames of this game's own: a "
                               "fresh start at the next boundary (frame {}, exit {})",
                     start.history_frames, match.running + 2, *match.fresh_pending);
    }
    return;
  }

  // A queue room's opponent must replay a keyframe carrying the room's header, so wait for it. If
  // it never lands, fail the join rather than start the match on the wrong header.
  DropStaleKeyframe(match);
  const u64 generation = Orca::UX::Game().Header().WantedGeneration();
  const bool in_place = Orca::UX::Game().Header().HeaderInPlace();
  // A fresh start's tail can't write the header (it runs as a re-run), so its boundaries don't
  // count toward the header's limit; nor do its unseen hold's, which run unthrottled.
  const HeaderWait header = HeaderWaitAt(&match.header_waited, InTail(match), Unseen(match),
                                         in_place, !match.waiting.empty(), in_queue_room);
  if (header == HeaderWait::Fail)
  {
    FailHeaderWait(match);
    return;
  }
  const bool header_ready = header == HeaderWait::Ready;
  if (!in_place && match.keyframe_wanted && match.header_waited % 60 == 1)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online rules: a keyframe waits for the room's header (frame {}{})",
                   match.running + 1,
                   header_ready ? "; a friends room's, made anyway" : "");
  }

  // Capture a keyframe when one is wanted and the newest is stale, missing or for another header.
  // In a session, only once every earlier frame is final.
  const int next = match.running + 1;
  // Made under the current header. In a session an old one stays: it may be on its way to a friend.
  const bool usable = match.keyframe && match.keyframe_generation == generation;
  const bool fresh = usable && match.keyframe->frame >= match.recovery_keyframe_from &&
                     next - match.keyframe->frame <= KEYFRAME_FRESH_FRAMES;
  bool capture_failed = false;
  const auto capture = ChooseJoinCapture(match.session.get(), match.doubles, next,
      std::max(match.replay.first_frame + 1, match.recovery_keyframe_from));
  if (match.keyframe_wanted && !match.job && match.store && header_ready)
  {
    if (fresh)
      match.keyframe_wanted = false;
    // Not while a fresh start runs (its tail, the queue host's hold, its own pick being put back):
    // the session starts as soon as the keyframe is stored, and the player's pad then replaces the
    // steering, so the pick would be left half done.
    else if (next >= FirstKeyframeFrame() && next >= match.recovery_keyframe_from &&
             OriginReady(match) && next > match.replay.first_frame &&
             !match.fresh && capture)
    {
      match.keyframe_wanted = false;
      StartKeyframe(system, match, capture->frame, capture->hash);
      if (match.job)
      {
        match.job->generation = generation;
      }
      else
      {
        capture_failed = true;
        match.keyframe_wanted = !match.waiting.empty();
      }
    }
  }
  if (!match.store && !match.waiting.empty())
    WARN_LOG_FMT(ROLLBACK, "Drop-in: no keyframe store: a friend can't join yet");

  // Take in whoever waits once there is a keyframe for them. Not during a solo pause (BeginPause):
  // the session's first step would give up half done. Not solo after a capture failed here: a
  // friend replaying the stale keyframe would replay frames already announced as this player's
  // alone (AloneAt). The capture is retried next boundary.
  if (!match.keyframe || match.waiting.empty() || match.job || s_pausing ||
      (capture_failed && !match.session) || !header_ready || !usable)
  {
    return;
  }
  if (match.keyframe->frame < match.recovery_keyframe_from)
    return;
  int live_from = next;
  if (match.doubles && match.session)
    for (const auto& [seat, arrival] : match.waiting)
      if (match.session->Plan()[seat].unplug_from != Orca::Net::NEVER)
        live_from = std::max(live_from, match.session->Plan()[seat].unplug_from);
  const bool batch = match.doubles && !recovering &&
                     (match.waiting.size() > 1 || !match.seated.empty());
  std::vector<int> batch_seats;
  if (batch)
  {
    for (const auto& [seat, arrival] : match.waiting)
    {
      batch_seats.push_back(seat);
      AddName(match, {seat, live_from, arrival.name, arrival.controls, arrival.queue});
    }
  }
  if (match.doubles && match.waiting.size() > 1)
  {
    match.keyframe->shared = true;
    match.shared_keyframes.push_back(*match.keyframe);
  }
  for (auto it = match.waiting.begin(); it != match.waiting.end();)
  {
    const int seat = it->first;
    const int keyframe_frame = match.keyframe->frame;
    if (!match.session)
    {
      if (keyframe_frame < match.log_base)
      {
        ++it;
        continue;
      }
      Orca::Net::Config config = SessionConfig();
      config.per_peer_inputs = match.doubles;
      config.local_seat = 0;
      config.authority_seat = 0;
      config.start_frame = next;
      config.start_pad = match.log.empty() ? Pad{} : match.log.back()[0];
      std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
      plan[0] = {0, Orca::Net::NEVER, 0};
      config.plan = plan;
      // Anything that arrived while solo (a departed friend's last packets) belongs to no one.
      auto* transport = test_room ? test_room : Orca::Online::Transport();
      transport->Receive();
      match.session = std::make_unique<Orca::Net::Session>(config, *match.port,
                                                          *transport);
      match.stall_budget.Clear();
      SeedRoundTrips(match);
      match.session->SetLog(match.log_base, std::move(match.log));
      match.log.clear();
      // Solo pads had no delay; now they do, and skipped frames repeat the current pad (start_pad).
      match.queued_local.clear();
      // A fresh start still putting port 1's pick back hands it to the player: in a session the
      // local pad is sampled ahead of the frame it plays.
      EndFresh(match, "a session starts");
      // A session keeps time with its friends (ORCA_TEST_FAST_UNTIL ages a host before one).
      match.fast_spent = true;
      if (std::exchange(match.fast, false))
        Core::SetIsThrottlerTempDisabled(false);
      // The host's own controls apply from the next frame (this frame's hook already ran) and never
      // change mid-session: a friend may already have run that frame.
      RefreshOwnValues(match, next + 1);
    }
    if (batch && !batch_seats.empty())
    {
      match.session->AddPeerBatch(batch_seats, live_from, keyframe_frame);
      batch_seats.clear();
    }
    else if (!batch)
      match.session->AddPeer(seat, live_from, keyframe_frame);
    match.seated.insert(seat);
    if (it->second.doubles)
      match.doubles_seats.insert(seat);
    match.plugging.insert(seat);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", seat + 1, it->second.name,
                   live_from, it->second.controls.empty() ? "" : ", with their own controls");
    if (!batch)
      AddName(match, {seat, live_from, it->second.name, it->second.controls, it->second.queue});
    // The newcomer plugs in only once every player holds its values.
    match.session->RequireValues(seat, match.names_version);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
    Orca::Net::KeyframeInfo offer = *match.keyframe;
    offer.names = AllNames(match);
    offer.names_version = match.names_version;
    // Others acknowledge the newcomer's values in their packets (Session::RequireValues).
    if (test_room)
    {
      test_room->BroadcastNames(offer.names, offer.names_version);
      test_room->OfferKeyframe(seat, offer);
    }
    else
    {
      Orca::Online::BroadcastNames(offer.names, offer.names_version);
      Orca::Online::OfferKeyframe(seat, offer);
    }
    NOTICE_LOG_FMT(ROLLBACK,
                   "Drop-in: port {} replays from keyframe frame {}, live from frame {}", seat + 1,
                   keyframe_frame, live_from);
    it = match.waiting.erase(it);
  }
  // A joiner deletes the keyframe it loaded, so the next arrival gets a fresh one.
  if (match.waiting.empty())
  {
    match.keyframe.reset();
    match.recovery_keyframe_from = -1;
    match.batch.Clear();
  }
}

// ---- A joining player ----

// Joiner: its keyframe loaded into a friends room, so its own queue ends (`orca menu cancel`) and
// the game kept for after a queue room is dropped (kept, it would block going home). A join that
// fails earlier keeps all of it.
void EndQueueForJoin(Match& match)
{
  const bool queue_or_search = QueueOrSearch();
  if (!JoinEndsQueue(InQueueRoom(), queue_or_search, match.queue_image != nullptr))
    return;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Queue: joined {}'s friends room {}: this player's own queue is over{}{}",
                 match.host_name.empty() ? std::string("a friend") : match.host_name,
                 Orca::Online::Code(), queue_or_search ? " (orca menu cancel)" : "",
                 match.queue_image ? "; the game it kept for after a queue's room is dropped" :
                                     "");
  match.queue_image.reset();
  match.want_queue_image = false;
  match.ranked_fought = false;
  if (!queue_or_search)
    return;
  Orca::UX::Search::End();
  Orca::UX::QueueApp::End();
  Orca::Status::Menu("cancel");
}

void StartDownload(Match& match, const Orca::Net::KeyframeInfo& info)
{
  auto download = std::make_unique<Download>();
  download->info = info;
  Download* const raw = download.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  raw->thread = std::thread([raw, store] {
    try
    {
      const auto start = Clock::now();
      std::string error;
      std::optional<std::vector<u8>> blob;
      if (!raw->info.inline_blob.empty())
      {
        // It came inline in the host's offer (Keyframe.h INLINE_KEYFRAME_MAX): the same checks
        // follow as for a download.
        blob = raw->info.inline_blob;
        raw->percent = 100;
      }
      else
      {
        blob = store->Get(
            raw->info,
            [raw](u64 done, u64 total) {
              raw->percent = static_cast<int>(done * 100 / std::max<u64>(total, 1));
              return !raw->cancel.load();
            },
            &error);
      }
      raw->transfer_ms = MsSince(start);
      if (!blob)
      {
        raw->error = error;
        raw->refusal = Orca::Net::LastRefusal();
        raw->done = true;
        return;
      }
      // Downloaded or inline, the same checks: size, hash, decryption, unpacking.
      const auto unpack_start = Clock::now();
      raw->ok = Orca::Net::OpenKeyframe(raw->info, std::move(*blob), &raw->frame, &raw->replay,
                                        &raw->error);
      raw->unpack_ms = MsSince(unpack_start);
    }
    catch (const std::exception&)
    {
      raw->error = "couldn't read the replay";
      raw->ok = false;
    }
    raw->done = true;
  });
  match.download = std::move(download);
}

enum class JoinPump
{
  Waiting,
  Ready,
  Failed,
};

// Polls the room for a joiner (host arrival, keyframe offer, download progress) without blocking.
JoinPump PumpJoin(Match& match, std::string* last_status)
{
  {
    for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
    {
      if (event.kind == Orca::Net::PeerEvent::Kind::Arrived && event.host)
      {
        if (match.host_room_wait.until)
        {
          match.boot_time = Clock::now();
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: host back; resuming by keyframe in room {}",
                         Orca::Online::Code());
          Orca::Status::Event("room-back host ended fresh-drop-in");
          Orca::Status::State("joining 0");
        }
        match.host_here = true;
        match.host_seat = event.seat;
        match.host_name = event.name.empty() ? "your friend" : event.name;
        ShowJoinMessage(fmt::format("Joining {}'s game...", match.host_name),
                        static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
      {
        if (match.host_room_wait.until && event.reason == "room-away-silent")
          continue;
        // The host never carried the room's header (FailHeaderWait): the match can't start.
        if (event.reason == "mismatch")
        {
          WARN_LOG_FMT(ROLLBACK, "Online rules: the host left room {}: its game wasn't set up for "
                       "this match (mismatch)", Orca::Online::Code());
          match.join_error_code = "mismatch";
          match.join_error = "That match couldn't start";
          return JoinPump::Failed;
        }
        match.join_error_code = "peer_left";
        match.join_error = "Your friend left before you could join";
        return JoinPump::Failed;
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Hold && !match.download)
      {
        // The host is in a single-player mode: wait; the time limit restarts after.
        if (event.holding != match.host_holding)
        {
          match.host_holding = event.holding;
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host {}",
                         event.holding ? "is in a single-player mode: the join waits" :
                                         "is back in the menus: the join goes on");
          if (event.holding)
          {
            Orca::Status::State("friend-waiting");
            ShowJoinMessage(
                fmt::format("{} is playing a single-player mode: you'll join when they're back in "
                            "the menus",
                            match.host_name.empty() ? "Your friend" : match.host_name),
                static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
          }
          else
          {
            Orca::Status::State("joining 0");
            match.last_percent = std::max(match.last_percent, 0);
            ShowJoinMessage(fmt::format("Joining {}'s game...", match.host_name.empty() ?
                                                                    "your friend" :
                                                                    match.host_name),
                            static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
          }
        }
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Names)
      {
        // The host took another friend in mid-download; acknowledge these values once loaded.
        if (!match.later_names || event.keyframe.names_version > match.later_names->names_version)
          match.later_names = event.keyframe;
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Keyframe && !match.download)
      {
        match.host_room_wait.Back();
        // Offered: any hold the host has now is for another friend's join.
        match.host_holding = false;
        match.offered_time = Clock::now();
        match.host_seat = event.seat;
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host offers keyframe {} (frame {}, {} bytes, {})",
                       event.keyframe.id, event.keyframe.frame, event.keyframe.size,
                       event.keyframe.inline_blob.empty() ? "from the store" : "inline");
        StartDownload(match, event.keyframe);
      }
    }
    if (Orca::Online::RoomEnded())
    {
      // The caller reports the room's own code (room_full, room_mismatch, kicked, network).
      match.join_error_code.clear();
      match.join_error = fmt::format("Couldn't join: {}", Orca::Online::StatusLine());
      return JoinPump::Failed;
    }
    if (match.host_room_wait.Expired(Clock::now()))
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: host back wait ended: room hold expired");
      Orca::Status::Event("room-away host ended hold-expired");
      match.join_error_code = "peer_left";
      match.join_error = "Your friend did not return to the room";
      return JoinPump::Failed;
    }
    if (std::string status = Orca::Online::StatusLine(); status != *last_status)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca: {}", status);
      *last_status = std::move(status);
    }
    if (match.download)
    {
      // Downloading is the first half of joining; catching up is the second.
      const int percent = match.download->percent / 2;
      if (percent > match.last_percent)
      {
        match.last_percent = percent;
        Orca::Status::State(fmt::format("joining {}", percent));
      }
      if (match.download->done)
        return JoinPump::Ready;
    }
    // A held join has no time limit; the limit counts from when the host lets it go on.
    if (match.host_holding)
      match.boot_time = Clock::now();
    if (!match.host_room_wait.until && Clock::now() - match.boot_time > JOIN_LIMIT)
    {
      match.join_error_code = "network";
      match.join_error = "Your friend's game never sent its state";
      return JoinPump::Failed;
    }
  }
  return JoinPump::Waiting;
}

// A failed automatic reopen or a join with "caps join" plays on solo; other failed joins stop.
void FailJoin(Core::System& system, Match& match)
{
  const std::string sentence =
      match.join_error.empty() ? std::string("Couldn't join your friend's game") : match.join_error;
  // An empty code: the room ended and gives its own reason.
  std::string code = match.join_error_code;
  if (code.empty())
    code = Orca::Online::RoomErrorCode();
  // An automatic friends-room join that failed falls back to a new room without an app error.
  const bool automatic = std::exchange(match.reopen_join, false);
  if (automatic)
    Orca::Status::Event("playing");
  else
    ReportJoinFailure(code, sentence);
  match.join_error_code.clear();
  match.join_error.clear();
  if (automatic || Soft())
  {
    Core::DisplayMessage(sentence, 6000);
    BecomeSolo(match, "the join failed", "playing", match.loaded_time == Clock::time_point{});
    return;
  }
  StopEmulation(system, sentence);
  End("couldn't join");
}

enum class Load
{
  Done,
  // Nothing in this game changed; FailJoin reports it.
  Failed,
  // Half loaded; the game was stopped.
  Broken,
};

// Puts this machine's own image and NAND back as `frame`, on a new ring (the old snapshots belong
// to the game being replaced). Only ever images this process captured itself.
enum class Restore
{
  Done,
  // Nothing changed.
  Failed,
  // Half restored: the caller stops the game.
  Broken,
};
Restore RestoreLocal(Core::System& system, Match& match, MachineImage image,
                     const std::vector<Orca::Net::NandEntry>& nand_entries, int frame)
{
  IOS::HLE::FS::HostFileSystem* nand = system.IsWii() ? HostNand(system) : nullptr;
  if (!LocalNandReady(system.IsWii(), nand != nullptr, Core::WiiRootIsTemporary()))
    return Restore::Failed;
  if (nand)
    nand->CloseHostFiles();
  match.port.reset();
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  if (nand)
  {
    if (!Orca::Net::ReplaceNandTree(nand->HostRoot(), nand_entries))
      return Restore::Broken;
    nand->ReloadFst();
  }
  if (!match.port->LoadImage(std::move(image), frame))
    return Restore::Broken;
  // Frames from here are a new timeline; test runs' harness counts them as first passes.
  Rollback::Harness::Restart();
  return Restore::Done;
}

// Captures this machine's origin at the start of `frame`, before its hook: its own snapshot, NAND
// and RAM checksum. The replay starts here.
bool CaptureOrigin(Core::System& system, Match& match, int frame)
{
  const auto start = Clock::now();
  IOS::HLE::FS::HostFileSystem* nand = system.IsWii() ? HostNand(system) : nullptr;
  if (!system.IsWii())
    match.origin_nand.clear();
  if (nand)
    nand->CloseHostFiles();
  const bool nand_ok = LocalNandReady(system.IsWii(), nand != nullptr,
                                      Core::WiiRootIsTemporary()) &&
                       (!system.IsWii() ||
                        Orca::Net::ReadNandTree(nand->HostRoot(), &match.origin_nand));
  if (nand)
    nand->ReopenHostFiles();
  if (!nand_ok || !SnapshotRing::Capture(system, &match.origin, true))
  {
    StopEmulation(system, "Couldn't prepare this game's local replay origin");
    End("replay origin");
    return false;
  }
  match.origin_frame = frame;
  match.origin_ticks = system.GetCoreTiming().GetTicks();
  match.origin_hash = RamChecksum(match.origin.mem1, match.origin.mem2);
  match.replay = {};
  match.replay.origin_hash = match.origin_hash;
  match.replay.first_frame = frame;
  // The history's seed is its first frame's typed event, applied by that frame's hook (FreshMove),
  // after this capture: the origin stays every Orca's same canonical one.
  if (match.origin_rule == OriginRule::Menu)
  {
    match.replay.frames.resize(1);
    match.replay.frames[0].seed = NewHistorySeed();
  }
  match.log.clear();
  match.log_base = frame;
  // Every way into the origin's frame starts with an empty JIT: this one, a fresh start and a
  // joiner's restore (both clear after RestoreLocal). So a host whose friend joins in place runs
  // from the origin with the same JIT history as that friend's rebuild.
  system.GetJitInterface().ClearSafe();
  u64 nand_bytes = 0;
  for (const Orca::Net::NandEntry& e : match.origin_nand)
    nand_bytes += e.data.size();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: origin at frame {}: RAM {:016x}, NAND {} KB ({:.0f} ms)", frame,
                 match.origin_hash, nand_bytes / 1024, MsSince(start));
  NOTICE_LOG_FMT(ROLLBACK, "Orca origin: rule {} at frame {}: RAM {:016x}",
                 match.origin_rule == OriginRule::Declared ? "declared" :
                 match.origin_rule == OriginRule::Frame0 ? "frame0" : "menu",
                 frame, match.origin_hash);
  return true;
}

// The host goes back to its own origin and leaves the main menu with `fresh_pending`'s exit for
// the match's character select (ORCA.md "Drop-in"). The history starts again there, so whoever
// joins next replays only this match's frames. Solo with no session only. Returns the origin's
// frame, where the state now starts.
std::optional<int> FreshStart(Core::System& system, Match& match)
{
  const FreshKind kind = FreshKindForOrigin(match.origin_rule);
  const u8 exit = std::exchange(match.fresh_pending, std::nullopt).value_or(0);
  if (!OriginReady(match) || match.session || match.joining || match.replaying || match.download ||
      match.job || !match.port || !FreshExitAllowed(kind, exit))
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: a fresh start was due at frame {}, but this game isn't solo "
                           "with an origin; skipped",
                 match.running + 1);
    return std::nullopt;
  }
  const auto start = Clock::now();
  const int from = match.running + 1;
  const int frame = match.origin_frame;
  switch (RestoreLocal(system, match, match.origin, match.origin_nand, frame))
  {
  case Restore::Done:
    break;
  case Restore::Failed:
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: no session NAND for a fresh start at frame {}", from);
    return std::nullopt;
  case Restore::Broken:
    StopEmulation(system, "Couldn't restore this game's local boot");
    Orca::Status::Error("internal", "Couldn't restore this game's local boot");
    End("fresh start");
    return std::nullopt;
  }
  system.GetJitInterface().ClearSafe();
  // A keyframe of the game left behind serves nobody now.
  if (match.keyframe)
    DeleteStored(match, *match.keyframe);
  match.keyframe.reset();
  match.keyframe_wanted = !match.waiting.empty();
  match.header_waited = 0;
  match.running = frame - 1;
  // The exit and a new seed are the first frame's typed events; FreshMove applies them at that
  // frame's hook, here and in every replay of it. Reset games have neither event.
  match.replay = FreshReplayStart(kind, frame, match.origin_hash, exit,
                                  kind == FreshKind::Menu ? NewHistorySeed() : 0);
  match.log.clear();
  match.log_base = frame;
  match.queued_local.clear();
  // This player's own name, controls and queue identity apply from the origin.
  match.port_names.clear();
  RefreshOwnValues(match, frame);
  match.stats_frame = -1;
  Match::Fresh fresh;
  fresh.kind = kind;
  fresh.exit = exit;
  fresh.queue = InQueueRoom();
  fresh.room = Orca::Online::Code();
  fresh.started = start;
  match.fresh = fresh;
  if (fresh.queue)
  {
    match.fresh_queue_armed = false;
    match.fresh_room = fresh.room;
  }
  Orca::UX::Game().Css()->DisarmOwnPick();
  // ORCA_TEST_FAST_UNTIL aged the host before its first fresh start only.
  match.fast_spent = true;
  if (std::exchange(match.fast, false))
    Core::SetIsThrottlerTempDisabled(false);
  // Unthrottled, unseen and muted until the character select is up (StepTail).
  match.port->SetCatchingUp(true);
  Orca::Events::NoteResync();
  Orca::UX::QueueApp::ResetTimeouts();
  Orca::UX::Game().Results().Reset();
  Orca::UX::Game().Header().MemoryReplaced();
  // Friends already saw their warning before fresh_pending was scheduled.
  if (kind == FreshKind::Reset && fresh.queue)
    Orca::UX::ShowRestartForMatch();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: fresh start at frame {}: back to the origin (frame {}) in {:.0f} "
                           "ms, leaving the main menu with exit code {}{}",
                 from, frame, MsSince(start), exit,
                 fresh.queue ? fmt::format(" for {} room {}", Orca::Online::RoomQueue(), fresh.room) :
                               std::string());
  return frame;
}

// Restores only a snapshot and NAND captured by this process at its own origin.
// Peer data is a bounded input replay; it never enters Dolphin's state loader.
Load LoadKeyframe(Core::System& system, Match& match, int* frame_out)
{
  std::unique_ptr<Download> download = std::move(match.download);
  if (download->thread.joinable())
    download->thread.join();
  if (!download->ok)
  {
    match.join_error_code = download->refusal == "signed_out" ? "signed_out" : "network";
    match.join_error = "Couldn't download your friend's replay";
    return Load::Failed;
  }
  if (!OriginReady(match) || match.origin.state.empty())
  {
    match.join_error_code = "internal";
    match.join_error = "This game has no local origin to rebuild your friend's game from";
    return Load::Failed;
  }
  // Both machines boot the same way to the same origin: another frame or another RAM there is
  // another game (or another build of it).
  if (download->replay.origin_hash != match.origin_hash ||
      download->replay.first_frame != match.origin_frame)
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: the host's origin is frame {} ({:016x}), this game's frame {} "
                           "({:016x})",
                 download->replay.first_frame, download->replay.origin_hash, match.origin_frame,
                 match.origin_hash);
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's game data differs from yours";
    return Load::Failed;
  }
  // Every header it writes is this game's ruleset's (or none).
  if (!Orca::Net::HeadersFitRuleset(download->replay,
                                    static_cast<u8>(Orca::UX::Game().ProfileRuleset())))
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: the host's replay sets another game's rules: refused");
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's game data differs from yours";
    return Load::Failed;
  }
  IOS::HLE::FS::HostFileSystem* nand = system.IsWii() ? HostNand(system) : nullptr;
  if (!LocalNandReady(system.IsWii(), nand != nullptr, Core::WiiRootIsTemporary()))
  {
    match.join_error_code = "internal";
    match.join_error = "Couldn't restore this game's local boot";
    return Load::Failed;
  }
  auto before = std::make_unique<Match::QueueImage>();
  before->frame = match.running + 1;
  before->local_seat = match.local_seat;
  before->loaded_time = match.loaded_time;
  before->replay = match.replay;
  if (!SnapshotRing::Capture(system, &before->image, false))
    return Load::Failed;
  if (nand)
  {
    nand->CloseHostFiles();
    if (!Orca::Net::ReadNandTree(nand->HostRoot(), &before->nand))
    {
      nand->ReopenHostFiles();
      return Load::Failed;
    }
  }
  match.before_join = std::move(before);
  if (RestoreLocal(system, match, match.origin, match.origin_nand, match.origin_frame) !=
      Restore::Done)
  {
    StopEmulation(system, "Couldn't restore this game's local boot");
    End("replay origin");
    return Load::Broken;
  }
  system.GetJitInterface().ClearSafe();
  // A fresh start of this game's own is over: the friend's game replaces it.
  match.fresh.reset();
  Orca::UX::Game().Css()->DisarmOwnPick();
  match.replay = std::move(download->replay);
  match.running = match.origin_frame - 1;
  match.join_in_play = false;
  match.loaded_time = Clock::now();
  match.replay_heartbeat = {};
  match.replaying = std::move(download);
  match.port->SetCatchingUp(true);
  Orca::UX::QueueApp::ResetTimeouts();
  Orca::UX::Game().Results().Reset();
  Orca::Events::NoteResync();
  Orca::UX::Game().Header().MemoryReplaced();
  // The host's game is here: the next picture shown is the room's select, not this player's own.
  Orca::UX::Search::JoinOver();
  *frame_out = match.origin_frame;
  return Load::Done;
}

void FinishReplay(Match& match)
{
  const Download& download = *match.replaying;
  const int frame = download.frame;
  if (!download.info.shared)
    DeleteStored(match, download.info);
  match.local_seat = Orca::Online::Seat();
  if (match.later_names && match.later_names->names_version > download.info.names_version)
    SetNames(match, *match.later_names);
  else
    SetNames(match, download.info);
  match.later_names.reset();
  match.log.clear();
  match.log_base = frame;
  match.queued_local.clear();
  Orca::Net::Config config = SessionConfig();
  config.local_seat = match.local_seat;
  config.authority_seat = match.host_seat;
  config.start_frame = frame;
  std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
  plan[match.host_seat] = {0, Orca::Net::NEVER, 0};
  config.plan = plan;
  match.session =
      std::make_unique<Orca::Net::Session>(config, *match.port, *Orca::Online::Transport());
  match.stall_budget.Clear();
  match.session->SetValuesHeld(match.names_version);
  SeedRoundTrips(match);
  match.running = frame - 1;
  match.stats_frame = -1;
  EndQueueForJoin(match);
  Orca::UX::Game().Header().ExpectHeader(QueueMode(Orca::Online::RoomQueue()), Orca::Online::Code(),
                                RoomFlags(), Orca::Online::KnownSetGames());
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: rebuilt replay frame {} locally in {:.0f} ms", frame,
                 MsSince(match.loaded_time));
  match.replaying.reset();
  match.before_join.reset();
}

bool RestoreBeforeJoin(Core::System& system, Match& match)
{
  auto before = std::move(match.before_join);
  if (!before || RestoreLocal(system, match, std::move(before->image), before->nand,
                              before->frame) != Restore::Done)
  {
    return false;
  }
  // As after loading the origin: a rebuild cancelled before its jit_clear_frame leaves blocks
  // compiled from the boot's code.
  system.GetJitInterface().ClearSafe();
  match.running = before->frame - 1;
  match.local_seat = before->local_seat;
  match.loaded_time = before->loaded_time;
  match.replay = std::move(before->replay);
  match.replaying.reset();
  Orca::Events::NoteResync();
  Orca::UX::QueueApp::ResetTimeouts();
  Orca::UX::Game().Results().Reset();
  Orca::UX::Game().Header().MemoryReplaced();
  return true;
}

enum class ReplayStep { Running, Done, Failed };

ReplayStep AdvanceReplay(Core::System& system, Match& match, const FrameHook& on_frame)
{
  const bool leave = Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest();
  const auto join = Orca::Status::Cap("join") ? Orca::Online::TakeJoinRequest() : std::nullopt;
  if (leave || (join && *join != Orca::Online::Code()))
  {
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay cancelled");
    }
    else if (join)
      JoinInPlay(match, *join);
    else
      Leave(match);
    return ReplayStep::Failed;
  }
  const int frame = match.running + 1;
  const int target = match.replaying->frame;
  if (MsSince(match.loaded_time) > 180000 || Orca::Online::RoomEnded() ||
      Orca::Online::HostLeftPending())
  {
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay failed");
      return ReplayStep::Failed;
    }
    // As PumpJoin: a room that ended gives its own code (match-over, kicked, room_full...).
    if (Orca::Online::RoomEnded())
    {
      match.join_error_code.clear();
      match.join_error = fmt::format("Couldn't join: {}", Orca::Online::StatusLine());
    }
    else if (Orca::Online::HostLeftPending())
    {
      match.join_error_code = "peer_left";
      match.join_error = "Your friend left before you could join";
    }
    else
    {
      match.join_error_code = "network";
      match.join_error = "Couldn't finish rebuilding your friend's game";
    }
    FailJoin(system, match);
    return ReplayStep::Failed;
  }
  // Keep the unjoined seat alive without acknowledging live values or claiming any new history.
  if (Clock::now() - match.replay_heartbeat >= std::chrono::seconds(1))
  {
    Orca::Net::Packet heartbeat;
    heartbeat.seat = Orca::Online::Seat();
    heartbeat.first_frame = target;
    heartbeat.current_frame = target;
    heartbeat.history_ack = target - 1;
    if (auto* transport = Orca::Online::Transport())
      transport->Send(heartbeat);
    match.replay_heartbeat = Clock::now();
  }
  // Every machine cleared the JIT once at the profile's jit_clear_frame of its own boot
  // (Rollback.cpp, JitClearDue), and the replay's frames count from that boot's first boundary. A
  // joiner long past that frame never gets JitClearDue again, so the rebuild clears here, at the
  // same frame. A launch joiner's JitClearDue fires at this boundary too; a second clear is free.
  // An origin at the built main menu is past that frame (DecideOrigin), so only a replay from
  // frame 0 (a game without one) gets here.
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && profile->jit_clear_frame && frame == static_cast<int>(*profile->jit_clear_frame))
  {
    system.GetJitInterface().ClearSafe();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: JIT cleared at replay frame {}", frame);
  }
  Orca::Net::ReplayFrame* const at = frame < target ? match.replay.At(frame) : nullptr;
  // The decoder hands over every frame from the origin to the target; a gap is a bug, never a
  // frame to play with the boundary's events.
  if (frame < target && !at)
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: the replay has no frame {} (frames {}-{})", frame,
                  match.replay.first_frame, target);
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay gap");
      return ReplayStep::Failed;
    }
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's replay didn't reproduce the same game";
    FailJoin(system, match);
    return ReplayStep::Failed;
  }
  Orca::Net::ReplayFrame& record = at ? *at : match.replay.boundary;
  {
    Orca::Net::ReplayScope scope(&record, true, true);
    ReplayFrameHook(system, match, frame, true, *record.ports, false, on_frame);
  }
  if (frame < target)
  {
    match.port->SetPads(frame, record.pads);
    match.running = frame;
    const int first = match.replay.first_frame;
    if (frame % 60 == 0)
    {
      Orca::Status::State(fmt::format(
          "joining {}", 50 + (target > first ? (frame - first) * 40 / (target - first) : 40)));
    }
    return ReplayStep::Running;
  }
  auto& memory = system.GetMemory();
  const u64 hash = RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                              {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
  if (hash != match.replay.target_hash)
  {
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay mismatch");
      return ReplayStep::Failed;
    }
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's replay didn't reproduce the same game";
    FailJoin(system, match);
    return ReplayStep::Failed;
  }
  // Preserve the boundary's typed events for future joins from this machine.
  match.replay.frames.push_back(match.replay.boundary);
  FinishReplay(match);
  return ReplayStep::Done;
}

// A replay player's next frame (ReplayStream.h), with this match's own restores: its origin, or a
// seek point it took itself. Like a joiner's rebuild, it never restores anything from elsewhere.
std::optional<int> StepReplayPlayer(Core::System& system, Match& match, const FrameHook& on_frame)
{
  const auto after_restore = [&system] {
    system.GetJitInterface().ClearSafe();
    Orca::Events::NoteResync();
    Orca::UX::QueueApp::ResetTimeouts();
    Orca::UX::Game().Results().Reset();
    Orca::UX::Game().Header().MemoryReplaced();
  };
  ReplayStream::PlayerHost host;
  host.restore_origin = [&] {
    if (RestoreLocal(system, match, match.origin, match.origin_nand, match.origin_frame) !=
        Restore::Done)
      return false;
    after_restore();
    return true;
  };
  host.restore = [&](MachineImage image, const std::vector<Orca::Net::NandEntry>& nand, int frame) {
    if (RestoreLocal(system, match, std::move(image), nand, frame) != Restore::Done)
      return false;
    after_restore();
    return true;
  };
  host.read_nand = [&system](std::vector<Orca::Net::NandEntry>* nand) {
    IOS::HLE::FS::HostFileSystem* fs = HostNand(system);
    if (!fs)
      return false;
    fs->CloseHostFiles();
    const bool ok = Orca::Net::ReadNandTree(fs->HostRoot(), nand);
    fs->ReopenHostFiles();
    return ok;
  };
  host.set_catching_up = [&match](bool catching) { match.port->SetCatchingUp(catching); };
  host.set_pads = [&match](int frame, const Orca::Net::Pads& pads) {
    match.port->SetPads(frame, pads);
  };
  host.stopping = [&system] { return Stopping(system); };
  host.stop = [&system](const std::string& message) { StopEmulation(system, message); };
  const std::optional<int> rewound = match.replay_play->Step(
      system, host, &match.running, match.origin_hash, match.origin_frame, on_frame);
  if (match.replay_play->Over())
  {
    match.replay_play.reset();
    End("replay over");
  }
  return rewound;
}

// A launch join (ORCA_JOIN, or an invite the app opened): blocks until the host's keyframe is in.
// Returns the frame the state starts at, or nullopt (join failed, or stopping).
std::optional<int> JoinHost(Core::System& system, Match& match)
{
  JoinPump pump = JoinPump::Waiting;
  while (!Stopping(system))
  {
    // TakeCommands doesn't run during a launch join, so handle "leave" and "join" here.
    if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left before joining, at the app's request");
      Leave(match);
      return std::nullopt;
    }
    if (Orca::Status::Cap("join"))
    {
      if (const auto code = Orca::Online::TakeJoinRequest(); code && *code != Orca::Online::Code())
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} instead", *code);
        OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
        ResetDropIn(match);
        Orca::Online::ShutdownInBackground();
        Orca::Online::StartRoom(*code, true, !QueueOrSearch());
        match.boot_time = Clock::now();
        match.offered_time = match.boot_time;
        match.host_here = false;
        match.host_holding = false;
        match.last_percent = -1;
        match.join_status.clear();
        match.later_names.reset();
        Orca::Status::State("joining 0");
      }
    }
    pump = PumpJoin(match, &match.join_status);
    if (pump != JoinPump::Waiting)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (pump == JoinPump::Waiting)
    return std::nullopt;
  if (pump == JoinPump::Failed)
  {
    FailJoin(system, match);
    return std::nullopt;
  }
  int frame = -1;
  switch (LoadKeyframe(system, match, &frame))
  {
  case Load::Done:
    return frame;
  case Load::Failed:
    FailJoin(system, match);
    return std::nullopt;
  case Load::Broken:
    return std::nullopt;
  }
  return std::nullopt;
}

// ---- The app's commands (embed mode, with "caps join") ----

// "leave": a joiner asks its host to unplug it at an agreed frame; a host opens a new room.
void Leave(Match& match)
{
  // The opponent didn't pick in time and stayed: the room's match hears it void before this leave.
  if (std::exchange(match.opponent_no_show, false) &&
      VoidFirst({.queue_room = Orca::Online::RoomQueue() != "private",
                 .match_live = Orca::Online::MatchLive(),
                 .set_over = Orca::Online::SetOver(),
                 .room_ended = Orca::Online::RoomEnded(),
                 .from_peer = true}))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: leaving after the opponent's no-show: reporting the "
                             "match void first");
    Orca::Online::ReportPeerFault();
  }
  // Leaving a ranked set forfeits it once its first game began (before that the room voids it), and
  // the screen says which (UX/SetEnd.h).
  if (Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive())
    Orca::UX::SetEnd::Current().SelfLeft(false, Orca::UX::SetEnd::NowMs());
  Orca::UX::Search::End();
  match.linger_until.reset();
  if (match.joining && match.session && match.session->CatchingUp())
  {
    // Still catching up, so never plugged in: there's no unplug frame to agree on. Leaving the room
    // is enough; the host drops the seat.
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left while catching up, frame {}", match.running);
    BecomeSolo(match, "left while catching up", nullptr);
    Orca::Status::Event("left");
    return;
  }
  if (match.joining && match.session)
  {
    if (!match.leave_requested)
    {
      match.leave_requested = true;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at the app's request, frame {}", match.running);
      match.session->RequestLeave();
    }
    return;
  }
  if (match.joining)
  {
    // Still downloading: give up. A launch joiner never loaded a keyframe: its own boot, port 1.
    BecomeSolo(match, "left before joining", nullptr, match.loaded_time == Clock::time_point{});
  }
  else
  {
    // Leave even with nobody in the room: the page expects "left", then the new room.
    if (match.session)
      GoSolo(match, "left", nullptr);
    ResetDropIn(match);
    ReopenRoom(match);
  }
  Orca::Status::Event("left");
}

// "join <code>": join a friend's game during play, playing solo until its keyframe loads.
void JoinInPlay(Match& match, const std::string& code, bool keep_room)
{
  if (code == Orca::Online::Code() && match.joining)
    return;
  if (match.session)
    GoSolo(match, "joining another game", nullptr);
  ResetDropIn(match);
  ForgetRoom(match);
  match.linger_until.reset();
  // A queue match: the join's own messages replace the search's, and the overlay says the opponent
  // was found until the host's game is loaded (queue2 searches from the queue's own select, which
  // Search doesn't track, so the queue's state counts too).
  const bool queued = QueueOrSearch();
  Orca::UX::Search::Matched(false);
  if (queued)
    Orca::UX::Search::JoinStarted();
  if (!keep_room)
  {
    Orca::Online::ShutdownInBackground();
    Orca::Online::StartRoom(code, true, !queued);
  }
  match.reopen_join = false;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} during play, frame {}", code, match.running + 1);
  match.joining = true;
  match.join_in_play = true;
  match.later_names.reset();
  match.boot_time = Clock::now();
  match.offered_time = match.boot_time;
  match.host_here = false;
  match.host_holding = false;
  match.joined = false;
  match.leave_requested = false;
  match.last_percent = -1;
  match.join_status.clear();
  Orca::Status::State("joining 0");
}

// "host <code>": the page selected a room with an opponent who will drop in. Only while alone:
// leave this room and host the selected one, on port 1 (Reseat).
void HostInPlay(Match& match, const std::string& code)
{
  const bool alone = match.port && !match.session && !match.joining && !match.download &&
                     !match.job && !match.keyframe_wanted && match.waiting.empty() &&
                     match.plugging.empty() && match.seated.empty() && !match.port->CatchingUp() &&
                     !Orca::Online::DropInPending();
  if (!alone)
  {
    WARN_LOG_FMT(ROLLBACK, "Matchmaking: can't host room {}: this game isn't alone", code);
    Orca::Status::Line("unsupported host");
    return;
  }
  Reseat(match);
  ResetDropIn(match);
  ForgetRoom(match);
  match.linger_until.reset();
  Orca::Online::ShutdownInBackground();
  Orca::Online::StartRoom(code, false, false);
  Orca::UX::Search::Matched(true);
  // Once the welcome names the queue, this game fresh-starts into the room's character select
  // (DecideFreshStart); the header and rule locks follow at the next alone boundary after it. Once
  // per room.
  match.fresh_queue_armed = code != match.fresh_room;
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: hosting room {} from frame {}", code, match.running + 1);
}

bool MaybeRestoreQueueImage(Core::System& system, Match& match);

// Takes the app's commands where the game may change hands: never mid re-run or in a launch join.
void TakeCommands(Core::System& system, Match& match)
{
  // A command sent before the app's "caps" line stays queued in Online until it arrives.
  if (!Orca::Status::Cap("join") && !Orca::Status::Cap("leave") && !Orca::Status::Cap("host"))
    return;
  // Likewise before the origin (the boot plays nobody's inputs) and through a fresh start's tail.
  if (CanonicalBoot(match) || InTail(match))
    return;
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    return;
  }
  if (match.joining && !match.join_in_play && !match.session)
    return;
  if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    Leave(match);
  // Restore this player's own game once a queue room is over, so the next pairing doesn't start
  // from the last opponent's character select.
  const bool restored = MaybeRestoreQueueImage(system, match);
  // On the queue's own character select, the current game is the one to come back to.
  const bool queue_css =
      Orca::Status::Cap("queue2") && (restored || Orca::UX::QueueApp::OnSoloCss());
  if (Orca::Status::Cap("join"))
  {
    if (const auto code = Orca::Online::TakeJoinRequest())
    {
      match.want_queue_image = queue_css && !match.session && !match.joining;
      JoinInPlay(match, *code);
    }
  }
  if (Orca::Status::Cap("host"))
  {
    if (const auto code = Orca::Online::TakeHostRequest())
    {
      match.want_queue_image = queue_css && !match.session && !match.joining;
      HostInPlay(match, *code);
    }
  }
}

// ---- The queue image (Games/Brawl/Queue.h) ----

// Captures this player's own game and NAND, to restore when the queue's room is over.
void CaptureQueueImage(Core::System& system, Match& match)
{
  match.want_queue_image = false;
  match.ranked_fought = false;
  auto qi = std::make_unique<Match::QueueImage>();
  qi->frame = match.running + 1;
  qi->replay = match.replay;
  const auto start = Clock::now();
  if (!SnapshotRing::Capture(system, &qi->image, false))
  {
    ERROR_LOG_FMT(ROLLBACK, "Queue: capturing this player's own game failed");
    return;
  }
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    nand->CloseHostFiles();
    const bool ok = Orca::Net::ReadNandTree(nand->HostRoot(), &qi->nand);
    nand->ReopenHostFiles();
    if (!ok)
    {
      ERROR_LOG_FMT(ROLLBACK, "Queue: reading the NAND for this player's own game failed");
      return;
    }
  }
  qi->taken = Clock::now();
  u64 nand_bytes = 0;
  for (const Orca::Net::NandEntry& e : qi->nand)
    nand_bytes += e.data.size();
  NOTICE_LOG_FMT(ROLLBACK, "Queue: this player's own game kept at frame {} ({:.0f} ms, NAND {} "
                 "KB)",
                 match.running + 1, MsSince(start), nand_bytes / 1024);
  match.queue_image = std::move(qi);
}

// Whether this game is in a room the queue made (its welcome said casual or ranked).
bool InQueueRoom()
{
  return !Orca::Online::RoomEnded() && QueueMode(Orca::Online::RoomQueue()) !=
                                           Orca::Engine::Mode::None;
}

// Restores the queue image once the queue's room is over: solo, no drop-in under way, no ranked
// verdict pending. The player is ready again unless their own ready timer ran out.
bool MaybeRestoreQueueImage(Core::System& system, Match& match)
{
  if (!match.queue_image)
    return false;
  Match::QueueImage& qi = *match.queue_image;
  // Armed once the room was actually entered: hosting it, or a session in it.
  if (!qi.armed && InQueueRoom() &&
      ((match.local_seat == 0 && !match.joining) || (match.joining && match.session)))
  {
    qi.armed = true;
    NOTICE_LOG_FMT(ROLLBACK, "Queue: in room {} ({})", Orca::Online::Code(),
                   Orca::Online::RoomQueue());
  }
  // Out of the queue (backed out to the menus): nothing to come back to.
  if (!Orca::UX::QueueApp::Active())
  {
    match.queue_image.reset();
    return false;
  }
  if (!qi.armed || match.session || match.joining || match.download || match.job ||
      match.linger_until || !match.waiting.empty() || !match.plugging.empty() || InQueueRoom() ||
      !match.port || match.port->CatchingUp())
  {
    return false;
  }
  std::unique_ptr<Match::QueueImage> image = std::move(match.queue_image);
  const auto start = Clock::now();
  const int frame = image->frame;
  switch (RestoreLocal(system, match, std::move(image->image), image->nand, frame))
  {
  case Restore::Done:
    break;
  case Restore::Failed:
    ERROR_LOG_FMT(ROLLBACK, "Queue: no session NAND to put this player's own game back into");
    return false;
  case Restore::Broken:
    StopEmulation(system, "Couldn't go back to your own game");
    Orca::Status::Error("internal", "Couldn't go back to your own game");
    End("queue image");
    return false;
  }
  // The room's fresh start, if any, was for the game left behind.
  match.fresh.reset();
  Orca::UX::Game().Css()->DisarmOwnPick();
  Orca::Events::NoteResync();
  // The header the frame hook read was the room's game's; this game's own is read at the next hook.
  Orca::UX::Game().Header().MemoryReplaced();
  match.log.clear();
  match.log_base = frame;
  match.running = frame - 1;
  match.replay = std::move(image->replay);
  match.queued_local.clear();
  match.keyframe.reset();
  // This player's own game plays port 1, and a room of its own opens (a former joiner had none).
  Reseat(match);
  if (match.tell_no_room)
  {
    match.tell_no_room = false;
    match.reopen_pending = Clock::now() + REOPEN_DELAY;
  }
  const bool mine = Orca::UX::QueueApp::TakeTimeoutWasMine();
  // After a ranked set (a fight was played in that room) the player presses Start to search again.
  const bool fought = std::exchange(match.ranked_fought, false);
  const char* why = mine ? "the ready timer ran out on us" : fought ? "a ranked set was played" : "";
  Orca::UX::QueueApp::AfterRestore(!mine && !fought, why);
  NOTICE_LOG_FMT(ROLLBACK, "Queue: back on this player's own character select at frame {} in {:.0f} "
                 "ms ({:.1f} s after the room's game was kept){}",
                 frame, MsSince(start),
                 std::chrono::duration<double>(start - image->taken).count(),
                 *why ? fmt::format("; not ready ({})", why) : std::string());
  return true;
}

// ---- The friends lobby and the queue ----

// A Casual or Ranked pick the frame hook saw at this boundary while this game wasn't alone
// (OnlineMenu.h TakeLobbyPick), taken after the app's commands (DecideLobbyPick). A host whose
// app can search that queue now leaves its room as the app's Leave does: its friends play on, hear
// `host-left`, and come home on the character select the pick opened (ComeHome). It prints
// `orca state left lobby`, then the pick as if made alone. Without the pick's cap it keeps its
// friends and prints the pick unarmed (`orca menu online <queue> kept`), so the app says why and
// cancels; ArmKeptPick arms it once they are gone. Only at a boundary the app's commands may take
// (never mid re-run). Local: the session ends here, so only the room's leave reaches the friends.
void TakeLobbyPickNow(Match& match)
{
  auto pick = Orca::UX::Game().Read().TakeLobbyPick();
  // A fresh pick replaces one that waited.
  if (pick)
    match.lobby_pick.reset();
  else if (match.lobby_pick)
  {
    pick = std::exchange(match.lobby_pick, std::nullopt);
    // A pick that waited is stale once the game has moved on from it (main menu, stage select,
    // fight): leaving the friends then would be for nothing.
    if (!Orca::UX::Game().Read().PickStands(*pick))
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick that waited for a boundary is stale by "
                               "frame {} (the game went on from it): dropped",
                     Orca::UX::MenuEventText(*pick, true), match.running + 1);
      return;
    }
  }
  if (!pick)
    return;
  const char* const queue = *pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    // The game already left the menu for the character select: keep the pick, or Start there
    // would do nothing.
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked mid re-run, frame {}: at the next boundary",
                   queue, match.running + 1);
    match.lobby_pick = pick;
    return;
  }
  LobbyPickInputs in;
  in.joining = match.joining;
  in.local_seat = match.local_seat;
  in.queue_room = InQueueRoom();
  in.host_cap = Orca::Status::Cap("host");
  in.pick_cap = Orca::UX::PickSearchable(*pick);
  in.session = match.session != nullptr;
  in.session_idle = match.session && match.session->Idle();
  in.drop_in_friends =
      !match.waiting.empty() || !match.plugging.empty() || !match.seated.empty();
  in.arrival_pending = Orca::Online::ArrivalPending();
  in.keyframe_kept = match.keyframe || match.job || match.keyframe_wanted;
  const LobbyPickPlan plan = DecideLobbyPick(in);
  // A newer pick replaces any kept or friends pick, whatever it comes to.
  match.kept_pick.reset();
  match.friends_pick.reset();
  if (plan.drop_keyframe)
    ResetDropIn(match);
  switch (plan.step)
  {
  case LobbyPickStep::Ignore:
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked in a game that isn't this player's alone "
                             "({}): nothing to do here",
                   queue, match.joining ? "its host's" : "with friends");
    return;
  case LobbyPickStep::Announce:
    if (!plan.arm)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked with friends in room {}, which the page "
                               "can't search now (no pick-{}): staying with them, frame {}",
                     queue, Orca::Online::Code(), queue, match.running + 1);
      match.kept_pick = pick;
    }
    break;
  case LobbyPickStep::Leave:
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked with friends in room {}: leaving it for the "
                             "queue, frame {}{}",
                   queue, Orca::Online::Code(), match.running + 1,
                   in.arrival_pending ? " (a friend arriving)" : "");
    match.linger_until.reset();
    if (match.session)
      GoSolo(match, "left for the queue", nullptr);
    ResetDropIn(match);
    ReopenRoom(match);
    // Either player may have pressed it in a shared menu: say what happened, not who did it.
    Core::DisplayMessage(*pick == Orca::UX::OnlinePick::Ranked ? "Left the lobby for Ranked" :
                                                                 "Left the lobby for Casual",
                         4000);
    Orca::Status::Event("left lobby");
    break;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Online menu: orca menu {}{}",
                 Orca::UX::MenuEventText(*pick, Orca::Status::Cap("host")),
                 plan.arm ? "" : " (unarmed)");
  Orca::UX::AnnounceOnlinePick(*pick, plan.arm);
}

// A former joiner on a port other than 1 with no room (MayComeHome), at its first alone boundary
// on the menus: it moves to port 1 from the next frame and opens a room of its own, as after a
// queue room (MaybeRestoreQueueImage). Solo with no session, so nothing re-runs this boundary and
// only this game changes; the next friend's keyframe carries it.
void ComeHome(Match& match)
{
  // Every boundary: the common case (port 1, or not solo) asks the room nothing.
  if (match.local_seat == 0 || match.joining || match.session)
    return;
  HomeInputs in;
  in.local_seat = match.local_seat;
  in.joining = match.joining;
  in.session = match.session != nullptr;
  in.room_up = !Orca::Online::RoomEnded();
  in.solo_idle = SoloIdle(match);
  in.drop_in_pending = Orca::Online::DropInPending();
  in.on_menus = Orca::UX::Game().Read().OnTheMenus();
  in.queue_image = match.queue_image != nullptr;
  in.lingering = match.linger_until.has_value();
  if (!MayComeHome(in))
    return;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to a game of its own on the menus (frame {}): a room of "
                           "its own opens",
                 match.running + 1);
  Reseat(match);
  // On port 1 now; ReopenRoom opens its own room a moment later.
  ReopenRoom(match);
  // Home on the character select a Casual or Ranked pick opened (its host left for the queue at
  // that pick): announce the same pick, so Start readies it on the queue's own character select.
  // Alone now, the app answers it as any alone pick.
  const std::optional<Orca::UX::OnlinePick> pick = Orca::UX::Game().Read().CssPick();
  if (AnnounceHomePick(pick.has_value(), Orca::Status::Cap("host"),
                       Orca::UX::QueueApp::Active() ||
                           Orca::UX::Search::Current() != Orca::UX::Search::State::None))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: home on the {} character select: orca menu {}",
                   *pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual",
                   Orca::UX::MenuEventText(*pick, true));
    Orca::UX::AnnounceOnlinePick(*pick);
  }
}

// A pick this host announced unarmed to keep its friends (TakeLobbyPickNow). Once the friends are
// gone and the game is alone on the character select that pick opened, it is armed as an alone
// pick (`orca menu online <queue>`). Dropped once the game moves on from it or a queue or search is
// under way. First runs only.
void ArmKeptPick(Match& match)
{
  if (!match.kept_pick || (match.session && match.session->Resimulating()))
    return;
  KeptPickInputs in;
  in.stands = Orca::UX::Game().Read().PickStands(*match.kept_pick);
  in.alone = match.local_seat == 0 && !match.joining && match.seated.empty() &&
             AloneAt(match.running + 1, SoloIdle(match), Orca::Online::DropInPending(),
                     match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt);
  in.queue_or_search = Orca::UX::QueueApp::Active() ||
                       Orca::UX::Search::Current() != Orca::UX::Search::State::None;
  const Orca::UX::OnlinePick pick = *match.kept_pick;
  const char* const queue = pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  switch (DecideKeptPick(in))
  {
  case KeptPickStep::Wait:
    return;
  case KeptPickStep::Drop:
    match.kept_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick kept with friends is over by frame {}: "
                             "dropped",
                   queue, match.running + 1);
    return;
  case KeptPickStep::Arm:
    match.kept_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick kept with friends, alone now (frame {}): "
                             "orca menu {}",
                   queue, match.running + 1,
                   Orca::UX::MenuEventText(pick, Orca::Status::Cap("host")));
    Orca::UX::AnnounceOnlinePick(pick);
    return;
  }
}

// A host whose queue ended for a friend: once a friend came in and all of them left, and it is
// alone again on that pick's character select, the pick is armed again so Start searches. First
// runs only.
void RearmFriendsPick(Match& match)
{
  if (!match.friends_pick || (match.session && match.session->Resimulating()))
    return;
  if (!match.seated.empty())
    match.friends_pick_played = true;
  FriendsPickInputs in;
  in.played = match.friends_pick_played;
  in.alone = match.local_seat == 0 && !match.joining && match.seated.empty() &&
             AloneAt(match.running + 1, SoloIdle(match), Orca::Online::DropInPending(),
                     match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt);
  in.on_pick_select = Orca::UX::Game().Read().CssPick() == match.friends_pick;
  // Only a queue the page can search now: unprompted, a refusal would only confuse.
  in.host_cap = Orca::Status::Cap("host") && Orca::UX::PickSearchable(*match.friends_pick);
  in.queue_or_search = QueueOrSearch();
  in.elsewhere = match.joining || match.local_seat != 0;
  const Orca::UX::OnlinePick pick = *match.friends_pick;
  const char* const queue = pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  switch (DecideFriendsPick(in))
  {
  case FriendsPickStep::Wait:
    return;
  case FriendsPickStep::Drop:
    match.friends_pick.reset();
    return;
  case FriendsPickStep::Arm:
    match.friends_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Queue: the friends are gone and this game is alone again on the {} "
                             "character select (frame {}): orca menu {}",
                   queue, match.running + 1,
                   Orca::UX::MenuEventText(pick, Orca::Status::Cap("host")));
    Orca::UX::AnnounceOnlinePick(pick);
    return;
  }
}

// A player on a port other than 1 that didn't come home at this boundary (say, in a fight) has no
// room of its own: the app hears "no-room" once, after the old room has finished leaving, so it
// never sees that room again. It still comes home once back on the menus.
void TellNoRoom(Match& match)
{
  if (!match.tell_no_room || Orca::Online::RoomsLeaving())
    return;
  match.tell_no_room = false;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no room of its own on port {}", match.local_seat + 1);
  Orca::Status::State("no-room");
}

// A joiner whose host's game carries another header than this room's leaves before playing it.
void LeaveOnMismatch(Match& match)
{
  if (match.session && match.session->Resimulating())
    return;
  const auto why = Orca::UX::Game().Header().TakeMismatch();
  if (!why)
    return;
  WARN_LOG_FMT(ROLLBACK, "Online rules: leaving room {}: {}", Orca::Online::Code(), *why);
  Orca::Online::SetLeaveReason(*why);
  Orca::Status::Report(*why, "That match couldn't start");
  Leave(match);
}

// ---- Ports, for the in-game UI ----
//
// Each port's values (player name and own controls) are decided by the host from a frame it picks;
// every machine holds them before running that frame, so the frame hook writes the same bytes
// everywhere (NameTags.h). The host plugs a newcomer in only once every player acknowledges its
// values, and never changes its own mid-session.

// Host: a seat's new values, from their frame. Keeps a few entries per seat.
void AddName(Match& match, Orca::Net::KeyframeInfo::Name name)
{
  auto& history = match.port_names[name.seat];
  if (!history.empty() && history.back().from >= name.from)
    history.pop_back();
  history.push_back(std::move(name));
  if (history.size() > 4)
    history.erase(history.begin());
  ++match.names_version;
}

std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match)
{
  std::vector<Orca::Net::KeyframeInfo::Name> all;
  for (const auto& [seat, history] : match.port_names)
    all.insert(all.end(), history.begin(), history.end());
  return all;
}

// A joiner takes the host's values and version; its packets acknowledge that version from now on.
void SetNames(Match& match, const Orca::Net::KeyframeInfo& from_host)
{
  match.port_names.clear();
  std::vector<Orca::Net::KeyframeInfo::Name> sorted = from_host.names;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto& a, const auto& b) { return a.from < b.from; });
  for (auto& name : sorted)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", name.seat + 1, name.name,
                   name.from, name.controls.empty() ? "" : ", with their own controls");
    match.port_names[name.seat].push_back(std::move(name));
  }
  match.names_version = from_host.names_version;
  if (match.session)
    match.session->SetValuesHeld(match.names_version);
}

// The values a seat had at `frame` (the newest from before it), or none.
const Orca::Net::KeyframeInfo::Name* EntryAt(const Match& match, int seat, int frame)
{
  const auto it = match.port_names.find(seat);
  if (it == match.port_names.end())
    return nullptr;
  // Entries are in frame order.
  for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry)
  {
    if (frame >= entry->from)
      return &*entry;
  }
  return nullptr;
}

// Host, as a session starts: record its own name and controls from `frame` if they changed.
void RefreshOwnValues(Match& match, int frame)
{
  const Orca::Net::KeyframeInfo::Name* now = EntryAt(match, match.local_seat, frame);
  std::string own = Orca::Online::Seat() == match.local_seat ?
                        Orca::Online::SeatName(match.local_seat) :
                        std::string();
  if (own.empty() && now)
    own = now->name;
  std::vector<u8> controls = Orca::Events::OwnControls();
  std::vector<u8> queue = Orca::Events::OwnQueue();
  if (now ? now->name == own && now->controls == controls && now->queue == queue :
            own.empty() && controls.empty() && queue.empty())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", match.local_seat + 1, own,
                 frame, controls.empty() ? "" : ", with their own controls");
  AddName(match, {match.local_seat, frame, std::move(own), std::move(controls), std::move(queue)});
}

// Controllers plugged in at `frame` (the session's plan, or solo just this player).
std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame)
{
  // The canonical boot: port 1, with no values, on every machine.
  if (CanonicalBoot(match))
    return {Orca::Events::PortInfo{0, "", false, {}, {}}};
  // A host names its own port, with its own controls, only while solo: in a session a friend may
  // already have run this frame. A joiner takes all values from its host. Solo frames are never
  // re-run, and a friend who joins later gets these entries with the keyframe.
  if (!match.joining && !match.session && Orca::Online::Seat() == match.local_seat)
  {
    const Orca::Net::KeyframeInfo::Name* now = EntryAt(match, match.local_seat, frame);
    std::string own = Orca::Online::SeatName(match.local_seat);
    if (own.empty() && now)
      own = now->name;
    std::vector<u8> controls = Orca::Events::OwnControls();
    if (now ? now->name != own || now->controls != controls : !own.empty() || !controls.empty())
    {
      std::vector<u8> queue = now ? now->queue : std::vector<u8>{};
      AddName(match,
              {match.local_seat, frame, std::move(own), std::move(controls), std::move(queue)});
    }
  }
  const auto port = [&](int seat) {
    Orca::Events::PortInfo info{seat, "", seat != match.local_seat, {}};
    if (const Orca::Net::KeyframeInfo::Name* entry = EntryAt(match, seat, frame))
    {
      info.name = entry->name;
      info.controls = entry->controls;
      info.queue = entry->queue;
    }
    return info;
  };
  std::vector<Orca::Events::PortInfo> ports;
  if (!match.session)
  {
    ports.push_back(port(match.local_seat));
    return ports;
  }
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (match.session->Plugged(seat, frame))
      ports.push_back(port(seat));
  }
  return ports;
}

// ---- Stats ----

// This machine's own speed (Orca/Perf.h): what the frame that just ended was, at its boundary,
// before the session steps.
void PerfBoundary(const Match& match)
{
  Orca::Perf::Boundary boundary;
  // Re-runs and a joiner's catch-up run with Rollback::IsResimulating.
  boundary.first_run = !Rollback::IsResimulating();
  // Frames that run unthrottled or unseen are not this machine's frame rate.
  boundary.excluded = !match.started || match.finished || match.stale ||
                      match.replaying != nullptr || CanonicalBoot(match) || InTail(match) ||
                      Unseen(match) || (match.port && match.port->CatchingUp()) ||
                      Core::GetIsThrottlerTempDisabled();
  boundary.hidden = !Orca::UX::HostVisible();
  boundary.held = LastFrameHeldMs() > 0;
  boundary.held_frames = LastFrameHeldFrames();
  if (match.session)
  {
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      if (seat != match.local_seat && match.session->Plugged(seat, match.running))
        ++boundary.peers;
    }
    boundary.rerun_total = static_cast<u64>(match.session->GetStats().resimulated_frames);
  }
  boundary.paused_total = std::chrono::nanoseconds(s_paused_total_ns.load());
  Orca::Perf::OnBoundary(boundary);
}

// Publishes link stats every boundary, and the "orca stats" line about once a second.
void PublishStats(Match& match)
{
  if (match.session && match.session->Resimulating())
    return;
  const Orca::Net::Stats stats = match.session ? match.session->GetStats() : Orca::Net::Stats{};
  int peers = 0;
  if (match.session)
  {
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      if (seat != match.local_seat && match.session->Plugged(seat, match.running))
        ++peers;
    }
  }
  Orca::Events::LinkStats link;
  link.online = peers > 0;
  link.delay = stats.delay;
  link.deepest_rollback = match.stats_rbmax;
  link.stalls = stats.stalls;
  link.hitches = stats.hitches;
  link.peer_hitches = stats.peer_hitches;
  link.round_trip_ms = Orca::Online::RoundTripMs();
  link.confirmed_frame = stats.confirmed_frame;
  const Orca::Net::DirectSummary direct = Orca::Online::Direct();
  link.transport = direct.transport;
  link.link_rtt_ms = direct.rtt_ms;

  const auto now = Clock::now();
  // Paused time is left out of the interval, so fps counts running time.
  match.stats_time += TakePausedTime();
  if (match.stats_frame < 0)
  {
    match.stats_time = now;
    match.stats_frame = match.running;
    match.stats_held = 0;
    match.stats_last = stats;
    match.stats_input = Orca::Events::GetInputAge();
  }
  const double seconds = std::chrono::duration<double>(now - match.stats_time).count();
  // A new session restarts its counters.
  const bool reset = stats.rollbacks < match.stats_last.rollbacks ||
                     stats.stalls < match.stats_last.stalls;
  const Orca::Net::Stats& last = reset ? Orca::Net::Stats{} : match.stats_last;
  if (seconds > 0)
    link.rollbacks_per_second = (stats.rollbacks - last.rollbacks) / seconds;
  Orca::Events::PublishLinkStats(link);
  if (seconds < 1.0)
    return;
  picojson::object o;
  const auto num = [&o](const char* key, double value) { o[key] = picojson::value(value); };
  num("f", match.running);
  num("seat", match.local_seat);
  num("peers", peers);
  num("ping", link.round_trip_ms);
  // Median round trips the delay follows: to the relay, and the slowest friend's (-1 if unknown).
  num("pmed", stats.rtt_median);
  num("fpmed", stats.peer_rtt_median);
  num("delay", stats.delay);
  num("rb", stats.rollbacks - last.rollbacks);
  num("rbf", stats.resimulated_frames - last.resimulated_frames);
  num("rbmax", match.stats_rbmax);
  num("st", stats.stalls - last.stalls);
  num("stms", std::round(match.stats_stall_ms));
  num("ds", s_desyncs);
  // Times another player's inputs were found coming late against its own frame (Session::LateSeat).
  num("late", stats.late_seats);
  // Frames the game itself held (a load) count as shown, so this falls only when this machine does.
  num("fps",
      std::round((match.running - match.stats_frame + match.stats_held) / seconds * 10) / 10);
  num("hi", stats.hitches);
  num("phi", stats.peer_hitches);
  // Slow frames a game load explained; "hi" plus this is what earlier builds counted as "hi".
  num("hil", stats.load_hitches);
  num("tx", link.transport);
  num("lrtt", link.link_rtt_ms);
  // Input age (ControllerSource.h): "pa" is the mean age in ms of the newest adapter report at
  // frames whose pad changed, "rpf" adapter reports per frame; -1 without an adapter.
  const Orca::Events::InputAge input = Orca::Events::GetInputAge();
  const Orca::Events::InputAge& input_last =
      input.frames < match.stats_input.frames ? Orca::Events::InputAge{} : match.stats_input;
  const u64 input_changes = input.changes - input_last.changes;
  const u64 input_frames = input.frames - input_last.frames;
  num("pa", input_changes > 0 ?
                std::round((input.age_ms - input_last.age_ms) / input_changes * 10) / 10 :
                -1);
  num("rpf", input_frames > 0 ?
                 std::round(double(input.reports - input_last.reports) / input_frames * 10) / 10 :
                 -1);
  Orca::Status::Stats(picojson::value(o).serialize());
  match.stats_time = now;
  match.stats_frame = match.running;
  match.stats_held = 0;
  match.stats_last = stats;
  match.stats_input = input;
  match.stats_rbmax = 0;
  match.stats_stall_ms = 0;
}

// Waiting in a ranked room for the verdict after the opponent left: once the room closes or
// VERDICT_WAIT passes, open a room of its own (a former joiner moves to port 1 first).
void Linger(Match& match)
{
  if (!match.linger_until)
    return;
  // The verdict came and the room stays open (set over); SetOverLeave takes this game out of it.
  if (Orca::Online::SetOver())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the verdict came (set over): done waiting");
    match.linger_until.reset();
    return;
  }
  const bool over = Orca::Online::RoomEnded();
  if (!over && Clock::now() < *match.linger_until)
  {
    // Still here and running: the room hears so, so the opponent can't claim this game stalled.
    Orca::Online::KeepAlive();
    return;
  }
  match.linger_until.reset();
  // No verdict in time and the set still open: this player's void goes in before it leaves, so the
  // leave isn't a forfeit (an opponent back in the room after its own drop never settles it).
  if (VoidFirst({.queue_room = Orca::Online::RoomQueue() != "private",
                 .match_live = Orca::Online::MatchLive(),
                 .set_over = Orca::Online::SetOver(),
                 .room_ended = over,
                 .waited_out = true}))
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Matchmaking: no verdict in {} s with the set still open: reporting "
                   "it void before leaving",
                   VERDICT_WAIT.count());
    Orca::Online::ReportPeerFault();
  }
  // A verdict that came already cleared the countdown; none will come now.
  Orca::UX::SetEnd::Current().NoVerdict(Orca::UX::SetEnd::NowMs());
  if (match.local_seat != 0)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: done waiting in the room ({})",
                   over ? Orca::Online::RoomErrorCode() : "no verdict in time");
    if (over && Orca::Online::RoomErrorCode() == "match-over")
      Orca::Status::Event("host-left match-over");
    Reseat(match);
    ReopenRoom(match);
    return;
  }
  if (!over)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: no verdict in {} s: a room of this game's own",
                   VERDICT_WAIT.count());
    ResetDropIn(match);
    ReopenRoom(match);
  }
}

// Reports games whose frames are now final (the module's Verdicts, Engine/Module.h). Every machine reads the same frames, so
// both report the same games. A game counts only once an opponent is plugged in.
void ConfirmResults(Match& match)
{
  if (!match.started || match.finished || !match.port)
    return;
  int confirmed = match.running;
  int plug = INT32_MAX;
  if (match.session)
  {
    if (match.session->Resimulating())
      return;
    confirmed = std::min(confirmed, match.session->ConfirmedFrame() + 1);
    int plugged = 0, latest = -1;
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      const Orca::Net::SeatPlan& plan = match.session->Plan()[seat];
      if (plan.plug_from == Orca::Net::NEVER || plan.unplug_from != Orca::Net::NEVER)
        continue;
      ++plugged;
      latest = std::max(latest, plan.plug_from);
    }
    if (plugged >= 2)
      plug = latest;
  }
  // The casual ready timer ran out (UX/QueueApp.h), at the same final frame on both machines.
  Orca::UX::QueueApp::ConfirmTimeout(confirmed, match.local_seat);
  // Ranked: a player who didn't pick a character in time leaves the room. Before the first game the
  // set ends with no game, so the room voids it and rates nobody; on a later game (a token still in
  // hand after the character order, CharOrder.h) leaving loses the set. The other waits for the
  // verdict, and a leave of its own then reports the match void first.
  if (const auto no_show = Orca::UX::Game().Results().ConfirmNoShow(confirmed))
  {
    const bool mine = *no_show == match.local_seat;
    NOTICE_LOG_FMT(ROLLBACK, "Online rules: port {} didn't pick a character in time{}",
                   *no_show + 1, mine ? ": leaving the set" : "");
    if (mine && Orca::Online::RoomQueue() == "ranked" && plug != INT32_MAX)
    {
      Orca::Online::SetLeaveReason("no-show");
      Orca::UX::SetEnd::Current().SelfLeft(true, Orca::UX::SetEnd::NowMs());
      Orca::Online::RequestLeave();
    }
    else if (!mine)
    {
      match.opponent_no_show = Orca::Online::RoomQueue() == "ranked";
      Core::DisplayMessage("Your opponent didn't pick a character in time", 6000);
    }
  }
  std::vector<int> starts;
  const std::vector<Orca::Engine::GameResult> results =
      Orca::UX::Game().Results().Confirm(confirmed, plug, &starts);
  // A joiner that asked to leave stays plugged in until its host's roster names the unplug frame,
  // so a fight's first frame can turn final in between. The leave press decides: the screen said
  // then whether the set counts (Leave), and the room hears no game began after it.
  if (match.joining && match.leave_requested)
    starts.clear();
  // A ranked game began (its fighters in, after the stage's loading image), at the same final frame
  // on both machines: the room learns the set reached it, so a leave from here on loses the set.
  // Sent ahead of results read from the same frames; the room keeps only this player's first
  // game-start of the set and needs no order between them.
  for (const int start : starts)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: a ranked game began (the fight from frame {})", start);
    Orca::Online::ReportGameStart(start);
  }
  // On screen, a leave from here on loses the set (UX/SetEnd.h); a set that ended a game reached it
  // too. Only a ranked set's model takes it.
  if (!starts.empty() || !results.empty())
    Orca::UX::SetEnd::Current().GameBegan(Orca::UX::SetEnd::NowMs());
  for (const Orca::Engine::GameResult& r : results)
  {
    Orca::Net::GameReport report;
    report.kind = r.kind == Orca::Engine::GameResult::Kind::Win  ? Orca::Net::GameReport::Kind::Win :
                  r.kind == Orca::Engine::GameResult::Kind::Draw ? Orca::Net::GameReport::Kind::Draw :
                                                               Orca::Net::GameReport::Kind::Void;
    report.winner_seat = r.winner_port;
    report.why = r.why;
    if (report.kind != Orca::Net::GameReport::Kind::Void)
      report.detail = r.DetailJson();
    report.start = r.start;
    report.from_set_block = r.from_set_block;
    report.number = r.number;
    report.set_done = r.set_done;
    report.set_winner_seat = r.set_winner;
    if (r.set_done)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the set ended with game {}: {}", r.number,
                     r.set_done == 1 ? fmt::format("port {} won", r.set_winner + 1) :
                                       std::string("void"));
    }
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: a game ended at frame {}: {}", r.frame,
                   report.kind == Orca::Net::GameReport::Kind::Win ?
                       fmt::format("port {} won, {}", r.winner_port + 1, report.detail) :
                   report.kind == Orca::Net::GameReport::Kind::Draw ?
                       fmt::format("a draw, {}", report.detail) :
                       fmt::format("void ({})", r.why));
    Orca::Online::ReportGame(report);
    // The match so far as a saved replay: a copy of the written bytes here, packed and sent on the
    // writer's own thread, so neither this frame nor the report above waits on it.
    if (match.replay_out)
      match.replay_out->SendPacked();
  }
}

// How long the session has waited on a friend inside StepSession, for the overlay.
void PublishStall(Match& match, double stalled_ms)
{
  Orca::Events::LinkStats link = Orca::Events::GetLinkStats();
  link.stalled_ms = static_cast<int>(stalled_ms);
  link.waiting_seat = -1;
  if (stalled_ms > 0)
  {
    const auto missing = match.session && Orca::Online::RoomDoubles() ?
                             match.session->MissingInputSeats() : std::vector<int>{};
    link.waiting_seat = WaitingFriendSeat(match.local_seat, missing, match.joining, match.host_seat,
                                         {match.seated.begin(), match.seated.end()});
  }
  Orca::Events::PublishLinkStats(link);
}
}  // namespace

namespace
{
// A joiner's room events: the host's port names, and the host leaving (HostGone handles it).
void JoinerEvents(Match& match)
{
  for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
  {
    if (event.kind == Orca::Net::PeerEvent::Kind::Names)
    {
      // A name for frames already run (it arrived after its plug frame): re-run them so the frame
      // hook writes it, as the host did. Ignore broadcasts older than what this player holds.
      if (event.keyframe.names_version < match.names_version)
        continue;
      const std::vector<Orca::Net::KeyframeInfo::Name> before = AllNames(match);
      int earliest = INT32_MAX;
      for (const auto& name : event.keyframe.names)
      {
        if (std::find(before.begin(), before.end(), name) != before.end())
          continue;
        const Orca::Net::SeatPlan& plan = match.session->Plan()[name.seat];
        const int from = std::max(name.from, plan.plug_from);
        if (plan.plug_from == Orca::Net::NEVER || from > match.running || from >= plan.unplug_from)
          continue;
        earliest = std::min(earliest, from);
      }
      SetNames(match, event.keyframe);
      if (earliest <= match.running)
      {
        if (match.session->RerunFrom(earliest))
        {
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a name for frame {} arrived at frame {}: re-running",
                         earliest, match.running + 1);
        }
        else
        {
          WARN_LOG_FMT(ROLLBACK,
                       "Drop-in: a name for frame {} arrived at frame {}, too late to re-run",
                       earliest, match.running + 1);
        }
      }
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
      match.host_gone = event.reason;
  }
}

// A joiner: the host's values for this player's own port must be what it sent (OwnValuesKept).
// Anything else is the host's doing: the session ends for it, before the port plays with them.
void CheckOwnValues(Match& match)
{
  if (!match.session || !match.session->Error().empty())
    return;
  OwnValues own;
  if (Orca::Online::Seat() == match.local_seat)
    own.name = Orca::Online::SeatName(match.local_seat);
  own.controls = Orca::Online::HelloControls();
  own.queue = Orca::Online::HelloQueue();
  const int plug_from = match.session->Plan()[match.local_seat].plug_from;
  if (OwnValuesKept(AllNames(match), match.local_seat, plug_from, own))
    return;
  WARN_LOG_FMT(ROLLBACK,
               "Drop-in: the host's values for port {} aren't this player's own: the "
               "session ends",
               match.local_seat + 1);
  match.session->EndForPeer("The host changed this player's own port values");
}

// A joiner's leave is complete: play on solo; the app hears "left" (its own Leave, so no error).
void FinishLeave(Match& match)
{
  LogStats("at leave");
  // A ranked set's leave has its own message on screen (UX/SetEnd.h).
  if (Orca::Online::RoomQueue() != "ranked")
    Core::DisplayMessage(fmt::format("You left {}'s game", match.host_name), 4000);
  BecomeSolo(match, "left", nullptr);
  Orca::Status::Event("left");
}

// A joiner that asked to leave and ran past its unplug frame; it only awaits the host's final ack.
bool LeavingPastUnplug(const Match& match)
{
  if (!match.joining || !match.leave_requested || !match.session)
    return false;
  const int unplug = match.session->Plan()[match.local_seat].unplug_from;
  return unplug != Orca::Net::NEVER && match.running >= unplug;
}

// A joiner whose host left, at a boundary that isn't a re-run: play on solo (with "caps join") or
// stop. Either way the frame gets its pads.
void HostGone(Core::System& system, Match& match, const std::function<Pad(int)>& local_pad)
{
  Orca::Net::PeerEvent event;
  event.reason = *std::exchange(match.host_gone, std::nullopt);
  // This player had asked to leave, so the host leaving completes it, whether or not the host's
  // last acknowledgement arrived first.
  if (Soft() && match.leave_requested && event.reason != "desync")
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host left the room as this player left, frame {}",
                   match.running + 1);
    FinishLeave(match);
    RunSolo(match, local_pad);
    return;
  }
  LogStats("host left");
  if ((event.reason == "room-away-silent" || event.reason == "room-resume") &&
      WaitForAwayHost(match))
  {
    RunSolo(match, local_pad);
    return;
  }
  if (event.reason == "desync")
    ++s_desyncs;
  if (Soft())
  {
    // The host's game is gone and this one goes on: every other controller unplugs.
    if (event.reason == "desync")
    {
      Orca::Online::ReportDesync();
      Orca::Status::Report("desync", "Your games went out of sync, so the match ended");
    }
    // A queue room's screen already says so (UX/SetEnd.h).
    if (Orca::Online::RoomQueue() == "private")
      Core::DisplayMessage(fmt::format("{} left: you play on", match.host_name), 6000);
    // Ranked set undecided: wait for the verdict (only awarded while someone is in the room).
    const bool linger = Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive() &&
                        !Orca::Online::RoomEnded();
    // Set over: stay on the results screen until this player leaves too; not a forfeit.
    const bool stay = Orca::Online::RoomQueue() == "ranked" && Orca::Online::SetOver();
    if (stay)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host left the set's room (set over): this game "
                               "stays until its player leaves");
    }
    BecomeSolo(match, "host left", "host-left", false, linger || stay);
    if (linger)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host went mid-set: waiting for the verdict");
      match.linger_until = Clock::now() + VERDICT_WAIT;
    }
    RunSolo(match, local_pad);
    return;
  }
  if (event.reason == "desync")
    Orca::Status::Error("desync", "Your games went out of sync, so the match ended");
  else
    Orca::Status::Error("peer_left", "Your friend's game ended");
  End("host left");
  StopEmulation(system, event.reason == "desync" ?
                            "Your games went out of sync, so the match ended" :
                            "Your friend's game ended");
}

// Host of a queue room: seats the room says left that the session still has in play. Friends rooms
// rely on the silence limit, since friends may come back.
std::vector<int> LeftSeatsInPlay(const Match& match)
{
  std::vector<int> seats;
  if (!match.session || (Orca::Online::RoomQueue() == "private" && !Orca::Online::RoomDoubles()))
    return seats;
  for (const int seat : Orca::Online::LeftSeatsPending())
  {
    if (seat >= 0 && seat < Orca::Net::MAX_SEATS && seat != match.local_seat &&
        match.session->Plan()[seat].live_from != Orca::Net::NEVER)
    {
      seats.push_back(seat);
    }
  }
  return seats;
}

static std::optional<int> SessionBoundary(
    Core::System& system, Match& match,
    const std::function<Orca::Net::Pad(int)>& local_pad, const FrameHook& on_frame,
    const std::function<void(int)>& local_pad_waiting, std::optional<int> rewound_to,
    bool doubles, const std::function<bool()>& stopping, std::chrono::milliseconds silence_limit,
    const std::function<std::vector<int>()>& left_in_play);

std::optional<int> Boundary(Core::System& system,
                            const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                            const FrameHook& on_frame,
                            const std::function<void(int local_seat)>& local_pad_waiting)
{
  Match& match = TheMatch();
  if (match.stale)
  {
    // A new boot in the same process (the room was left when the last one stopped).
    if (match.store)
      match.store->Cancel();
    match.session.reset();
    match.port.reset();
    match.job.reset();
    match.download.reset();
    match = {};
    Orca::UX::Game().Css()->DisarmOwnPick();
    Orca::Perf::NewBoot();
  }
  if (match.finished)
    return std::nullopt;
  match.stats_held += LastFrameHeldFrames();

  std::optional<int> rewound_to;
  if (!match.started)
  {
    match.started = true;
    // Once emulation has stopped (the CPU thread is gone), leave the room.
    if (!match.on_state_changed)
    {
      match.on_state_changed = Core::AddOnStateChangedCallback([](Core::State state) {
        if (state != Core::State::Uninitialized)
          return;
        End("emulation stopped");
        TheMatch().stale = true;
      });
    }
    match.store = MakeStore();
    if (const std::string leave = Orca::GetEnv("ORCA_TEST_LEAVE_AT"); !leave.empty())
      match.leave_at = std::atoi(leave.c_str());
    // This player's own controls, before a launch join's hello carries them.
    Orca::UX::Game().LoadOwnControls();
    if (ReplayStream::ProductMode())
      match.replay_play = ReplayStream::Player::FromEnv();
    if (!match.replay_play)
      Orca::Online::Start();
    // The role (host plays, joiner waits) is known once the room has its ticket.
    std::optional<bool> joining;
    while (!match.replay_play && !(joining = Orca::Online::Joining()) &&
           !Orca::Online::RoomEnded() && !Stopping(system))
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    match.joining = joining.value_or(false);
    if (match.joining)
    {
      while (Orca::Online::Seat() < 0 && !Orca::Online::RoomEnded() && !Stopping(system))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      match.local_seat = Orca::Online::Seat();
      if (!match.store)
      {
        StopEmulation(system,
                      "Couldn't join: this YG Dolphin has nowhere to get your friend's game from");
        End("no keyframe store");
        return std::nullopt;
      }
      // Until the keyframe is in, run this boot unthrottled and unseen to its origin.
      match.boot_time = Clock::now();
      Orca::Status::State("joining 0");
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      match.port->SetCatchingUp(true);
    }
    else
    {
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      Orca::Status::State("playing");
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port 1; room {}", Orca::Online::StatusLine());
    }
    if (!match.replay_play)
    {
      match.replay_out = ReplayStream::Writer::ForMatch();
      match.replay_play = match.joining ? nullptr : ReplayStream::Player::FromEnv();
    }
    // A replay player boots to its origin unseen and unthrottled, as a launch joiner does.
    if (match.replay_play)
      match.port->SetCatchingUp(true);
    system.GetJitInterface().ClearSafe();
    // A game whose main menu this can't read has its origin at its first boundary instead, and no
    // fresh starts.
    const auto* mod = Orca::Mod::ActiveMod();
    match.origin_rule = SelectOriginRule(mod ? &mod->manifest : nullptr,
                                        Orca::UX::Game().Read().FreshSupported(),
                                        Orca::Mod::GuardsFailed());
    if (match.origin_rule == OriginRule::Frame0)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: this game's main menu isn't read: its origin is its first "
                               "frame, with no fresh starts");
      if (!CaptureOrigin(system, match, 0))
        return std::nullopt;
    }
  }

  // Check each welcome before replay, fresh starts, the frame hook, or peer events can play it.
  if (!Orca::Online::RoomEnded())
  {
    if (const auto reason = RoomRefusal(Orca::Online::RoomQueue(),
                                       Orca::UX::Game().Describe().results);
        !reason.empty())
    {
      Orca::UX::Search::End();
      Orca::UX::QueueApp::End();
      BecomeSolo(match, "ranked unsupported", "playing", true);
      Orca::Status::Report("ranked_unsupported", reason);
      Orca::UX::ShowToast(std::string(reason));
    }
  }

  // Every boot plays the canonical boot to its origin, the first boundary whose main menu has been
  // built for ORIGIN_SETTLE_FRAMES frames, captured before that boundary's hook.
  if (CanonicalBoot(match))
  {
    const int frame = match.running + 1;
    const Orca::Profile* profile = Orca::ActiveProfile();
    const auto* mod = Orca::Mod::ActiveMod();
    // A builtin verdict is known before this boundary's hook, including when a declaration
    // names firstKeyframeFrame itself. Legacy boots do not take this branch.
    if (mod && mod->manifest.builtin != Orca::Mod::Builtin::None)
    {
      const Core::CPUThreadGuard guard(system);
      Orca::Mod::RefreshGuards(guard, frame);
      if (match.origin_rule == OriginRule::Menu && Orca::Mod::GuardsFailed())
        match.origin_rule = SelectOriginRule(&mod->manifest, true, true);
    }
    OriginStep step;
    if (match.origin_rule == OriginRule::Declared && mod && mod->manifest.origin)
    {
      const Core::CPUThreadGuard guard(system);
      const Orca::UX::GuardMemory memory(guard);
      match.origin_words_streak = DeclaredOriginStreak(
          frame, match.origin_words_streak, *mod->manifest.origin, memory);
      step = DecideDeclaredOrigin(frame, match.origin_words_streak, *mod->manifest.origin);
    }
    else if (match.origin_rule == OriginRule::Failed)
      step = OriginStep::Fail;
    else
      step = DecideOrigin(frame, match.menu_built_streak,
                          profile ? profile->jit_clear_frame : std::nullopt);
    switch (step)
    {
    case OriginStep::Wait:
      break;
    case OriginStep::Capture:
      if (!CaptureOrigin(system, match, frame))
        return std::nullopt;
      break;
    case OriginStep::Fail:
      match.origin_failed = true;
      match.origin_rule = OriginRule::Failed;
      auto& memory = system.GetMemory();
      const u64 ram = RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                                  {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
      NOTICE_LOG_FMT(ROLLBACK, "Orca origin: rule failed at frame {}: RAM {:016x}", frame, ram);
      if (const auto sentence = OriginFailureReport(mod ? &mod->manifest : nullptr);
          !sentence.empty())
        Orca::Status::Report("no_origin", sentence);
      Orca::Status::WithdrawOnline();
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: no built main menu by frame {}: this game has no origin, so "
                              "it can't take or join players",
                    frame);
      if (match.replay_play)
      {
        ReplayStream::PlayerHost host;
        host.stop = [&system](const std::string& message) { StopEmulation(system, message); };
        match.replay_play->Fail(host, "this game couldn't produce the replay's local origin");
        match.replay_play.reset();
        End("replay origin failed");
        return std::nullopt;
      }
      break;
    }
  }

  // A replay player plays its file from the origin on, instead of a game.
  if (match.replay_play && OriginReady(match))
    return StepReplayPlayer(system, match, on_frame);
  // This machine's history, as its frames become final.
  if (match.replay_out && OriginReady(match) && !match.replaying)
  {
    const int final_end = match.session ? std::min(match.running + 1, match.session->FinalFrame()) :
                                          match.running + 1;
    if (!match.replay_out->Pump(match.replay, final_end, match.running + 1,
                                &match.replay.checksums,
                                match.session != nullptr))
      match.replay_out.reset();
  }

  Orca::Net::ReplayRecordingScope recording(&match.replay);
  bool replay_boundary = false;
  if (match.replaying)
  {
    switch (AdvanceReplay(system, match, on_frame))
    {
    case ReplayStep::Running:
      return std::nullopt;
    case ReplayStep::Failed:
      if (match.finished)
        return std::nullopt;
      rewound_to = match.running + 1;
      RunSolo(match, local_pad);
      return rewound_to;
    case ReplayStep::Done:
      replay_boundary = true;
      rewound_to = match.running + 1;
      break;
    }
  }

  // A fresh start due (HostEvents): back to the origin and out of the main menu, before this
  // boundary's hook, which then runs the origin's frame.
  if (match.fresh_pending)
  {
    if (const std::optional<int> origin = FreshStart(system, match))
    {
      rewound_to = origin;
    }
    else if (!match.finished)
    {
      // Not tried again every boundary: whoever waits now (a friend, or the queue room's
      // opponent) joins in place instead, from this game's origin.
      match.friends_in_place = !match.waiting.empty();
      if (std::exchange(match.fresh_queue_armed, false))
        match.fresh_room = Orca::Online::Code();
    }
    if (match.finished)
      return std::nullopt;
  }

  // Once a queue room is over, restore this player's own game first, so this boundary's hook
  // already sees its own character select.
  if (MaybeRestoreQueueImage(system, match))
    rewound_to = match.running + 1;

  // The header this game's room calls for, written only where HeaderFreeAt allows (never in the
  // canonical boot).
  UpdateWanted(match);
  DropStaleKeyframe(match);
  Orca::UX::Game().Header().SetHeaderFree(
      !CanonicalBoot(match) &&
      HeaderFreeAt(match.running + 1, SoloQuiet(match),
                   match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt));

  // The in-game UI's frame hook, before the snapshot or keyframe so they capture its writes.
  if (!replay_boundary)
  {
    const int frame = match.running + 1;
    // A fresh start's tail runs as a joiner's rebuild of the same frames would: a re-run that
    // plays the replay's typed events (the main menu exit) and announces nothing.
    const bool resimulating = (match.session && match.session->Resimulating()) || InTail(match);
    const std::vector<Orca::Events::PortInfo> ports = PortsAt(match, frame);
    auto* record = Orca::Net::ReplayRecordingScope::Frame(frame);
    if (record)
    {
      auto canonical = Orca::Net::SyncedPorts(ports);
      const Orca::Net::ReplayFrame* previous = match.replay.At(frame - 1);
      if (previous && previous->ports && *previous->ports == canonical)
        record->ports = previous->ports;
      else
        record->ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(std::move(canonical));
    }
    Orca::Net::ReplayScope replay_scope(record, resimulating);
    // Nothing is this player's alone before its origin: the boot is everyone's.
    ReplayFrameHook(system, match, frame, resimulating, ports,
             !resimulating && !CanonicalBoot(match) &&
                 AloneAt(frame, SoloIdle(match), Orca::Online::DropInPending(),
                         match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt),
             on_frame);
    if (!resimulating && ports != match.told_ports)
    {
      match.told_ports = ports;
      Orca::Events::NotifyPlugIn(frame, ports);
    }
    // Before the origin: frames in a row with the main menu built (DecideOrigin).
    if (CanonicalBoot(match) && match.origin_rule == OriginRule::Menu)
    {
      match.menu_built_streak =
          Orca::UX::Game().Read().LastSeen().menu_built ? match.menu_built_streak + 1 : 0;
    }
  }

  TakeCommands(system, match);
  LeaveOnMismatch(match);
  if (match.want_queue_image)
    CaptureQueueImage(system, match);
  else
  {
    if (MaybeRestoreQueueImage(system, match))
      rewound_to = match.running + 1;
  }
  // After the app's commands and the frame hook: a Casual or Ranked pick made with friends, a kept
  // pick to arm, a former joiner's way home, then "no-room" for one that didn't come home.
  TakeLobbyPickNow(match);
  ArmKeptPick(match);
  RearmFriendsPick(match);
  ComeHome(match);
  TellNoRoom(match);

  if (match.joining && !match.session && match.join_in_play)
  {
    // A join during play: this game plays on solo until the friend's keyframe is in.
    int frame = -1;
    switch (PumpJoin(match, &match.join_status))
    {
    case JoinPump::Waiting:
      RunSolo(match, local_pad);
      PublishStats(match);
      return std::nullopt;
    case JoinPump::Failed:
      FailJoin(system, match);
      break;
    case JoinPump::Ready:
      switch (LoadKeyframe(system, match, &frame))
      {
      case Load::Done:
        rewound_to = frame;
        break;
      case Load::Failed:
        FailJoin(system, match);
        break;
      case Load::Broken:
        return std::nullopt;
      }
      break;
    }
    if (match.finished)
      return std::nullopt;
  }
  else if (match.joining && !match.session && CanonicalBoot(match))
  {
    // A launch joiner first boots on its own to its origin, unseen and unthrottled, taking the
    // host's offer meanwhile; the join goes on from there (JoinHost).
    if (PumpJoin(match, &match.join_status) == JoinPump::Failed)
      FailJoin(system, match);
    if (match.joining)
    {
      RunSolo(match, local_pad);
      return std::nullopt;
    }
    // Failed (with "caps join" this player now plays its own boot on port 1), or stopping.
    if (match.finished || Stopping(system))
      return std::nullopt;
  }
  else if (match.joining && !match.session)
  {
    if (match.origin_failed)
    {
      match.join_error_code = "internal";
      match.join_error = "This game can't rebuild your friend's game";
      FailJoin(system, match);
    }
    else
    {
      rewound_to = JoinHost(system, match);
    }
    // Failed (with "caps join" this player now plays solo), or stopping.
    if (!rewound_to && (match.joining || match.finished || Stopping(system)))
      return std::nullopt;
  }

  if (match.replaying)
  {
    const auto step = AdvanceReplay(system, match, on_frame);
    if (step == ReplayStep::Failed && !match.finished)
    {
      rewound_to = match.running + 1;
      RunSolo(match, local_pad);
    }
    if (step != ReplayStep::Done)
      return rewound_to;
  }

  if (!match.joining)
  {

    Linger(match);
    if (!Stopping(system))
      KeepRoomOpen(match);
    // While waiting for a ranked room's verdict, nobody new is taken in.
    if (!match.joining && !Orca::Online::RoomEnded() && !match.linger_until)
      HostEvents(system, match);
    if (!match.session)
    {
      RunSolo(match, local_pad);
      // A fresh start's tail and unseen hold run unthrottled: their frames aren't this machine's
      // frame rate.
      if (!InTail(match) && !Unseen(match))
        PublishStats(match);
      return rewound_to;
    }
  }
  if (!match.session)
    return std::nullopt;

  // Round trips go to the session's stats (the delay is always fixed: ChosenDelay); each player's
  // delay is its own.
  if (const auto round_trip_ms = NewRoundTrip(&match.round_trip_sequence))
    match.session->OnRoundTrip(*round_trip_ms);
  match.session->SetFixedDelay(ChosenDelay());
  // A frame over two frame periods was a local hitch (disc read, shader compile). Time the game
  // itself held it (a load, the same on every machine) is not this machine's: a frame slow only by
  // that is counted apart, so a gate can still compare the raw count with an older build's.
  const auto now = Clock::now();
  const auto held = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(LastFrameHeldMs()));
  if (match.last_return && now - *match.last_return > HITCH_TIME && !match.port->CatchingUp())
  {
    if (now - *match.last_return - held > HITCH_TIME)
      match.session->OnLocalHitch();
    else
      match.session->OnLoadHitch();
  }

  // Handle a departed host only outside a re-run, or half-replayed frames would remain.
  if (match.joining)
  {
    JoinerEvents(match);
    CheckOwnValues(match);
    if (match.host_gone && !match.session->Resimulating())
    {
      HostGone(system, match, local_pad);
      return rewound_to;
    }
  }

  if (match.joining && match.leave_at >= 0 && match.running >= match.leave_at &&
      !match.leave_requested)
  {
    match.leave_requested = true;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at frame {}", match.running);
    match.session->RequestLeave();
  }

  return SessionBoundary(system, match, local_pad, on_frame, local_pad_waiting, rewound_to,
                         Orca::Online::RoomDoubles(), [&] { return Stopping(system); },
                         LeavingPastUnplug(match) ? LEAVE_SILENCE_LIMIT : PEER_SILENCE_LIMIT,
                         [&] { return LeftSeatsInPlay(match); });
}

static std::optional<int> SessionBoundary(
    Core::System& system, Match& match,
    const std::function<Orca::Net::Pad(int)>& local_pad, const FrameHook& on_frame,
    const std::function<void(int)>& local_pad_waiting, std::optional<int> rewound_to,
    bool doubles, const std::function<bool()>& stopping, std::chrono::milliseconds silence_limit,
    const std::function<std::vector<int>()>& left_in_play)
{
  for (int attempt = 2; attempt > 0;)
  {
    const int players_before = match.session->PlayersInPlay();
    const Common::ScopeGuard spend_attempt([&] {
      if (match.session && match.session->PlayersInPlay() >= players_before)
        --attempt;
    });
    bool peer_silent = false;
    // A joiner stalled on its host stops waiting once the host has left the room.
    bool host_left = false;
    // Likewise a queue room's host: opponents that left but are still in play.
    std::vector<int> left_seats;
    // Show a stall while it lasts; StepSession polls the callback during the wait.
    const auto step_start = Clock::now();
    auto stall_published = step_start;
    bool stall_shown = false;
    bool stall_claimed = false;
    const bool catching_up = match.port->CatchingUp() || match.session->CatchingUp();
    const int stalls_before = match.session->GetStats().stalls;
    const int running_before = match.running;
    // Stalls and time-sync waits are not this machine's work (Orca/Perf.h), and neither are the
    // presents and sleeps inside them.
    std::chrono::nanoseconds waited{};
    std::optional<Orca::Perf::StepScope> perf_scope;
    perf_scope.emplace();
    const Orca::Net::Step step = StepSession(
        *match.session, *match.port, match.running, [&] { return local_pad(match.local_seat); },
        [&] {
          // A talk press still down ends when Start is let go, not when the game moves again.
          // Host only: it reads the pad without recording or sending it (UX/OrbCombo.h).
          if (local_pad_waiting)
            local_pad_waiting(match.local_seat);
          const auto poll_time = Clock::now();
          if (poll_time - stall_published > std::chrono::milliseconds(10))
          {
            stall_published = poll_time;
            stall_shown = true;
            PublishStall(match, MsSince(step_start));
            Orca::Events::NotifyStall(static_cast<int>(MsSince(step_start)));
            // Mid ranked set, a long stall shows as a disconnect with a forfeit countdown.
            // Public recovery shows its hold; friends keep their existing overlay flow.
            if (Orca::Online::RoomQueue() == "private" || !Orca::Online::RoomRecovery())
              Orca::UX::SetEnd::Current().Stall(static_cast<int>(MsSince(step_start)),
                                                Orca::UX::SetEnd::NowMs());
            if (!match.joining)
              left_seats = left_in_play();
            // Opted-in queue recovery waits for the server; never announce a local forfeit.
            if (!stall_claimed && poll_time - step_start >= RANKED_STALL_LIMIT &&
                Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive() &&
                !Orca::Online::RoomRecovery())
            {
              stall_claimed = true;
              WARN_LOG_FMT(ROLLBACK, "Matchmaking: the opponent's inputs stopped for {} s: "
                                     "claiming the set",
                           RANKED_STALL_LIMIT.count());
              Orca::Online::ReportStall();
              Core::DisplayMessage("Your opponent stopped playing: the set is yours", 6000);
            }
          }
          if (match.joining && Orca::Online::HostLeftPending())
            host_left = true;
          return host_left || !left_seats.empty() || stopping();
        },
        std::chrono::milliseconds{1},
        silence_limit, &peer_silent, &waited,
        [&](Clock::time_point poll_time) {
          const auto missing = FriendsMissingInputs(*match.session, doubles);
          return !LeavingPastUnplug(match) &&
                 (WaitForFriendsRoster(*match.session, match.joining, doubles) ||
                  Orca::Online::HoldInputSilence(step_start, poll_time, missing));
        },
        // A joiner catching up waits on history by design, outside the stall budget.
        catching_up ? nullptr : &match.stall_budget,
        [&](Clock::time_point poll_time) {
          return WaitForFriendsRoster(*match.session, match.joining, doubles) ||
                 Orca::Online::RoomRecovering(poll_time);
        });
    perf_scope.reset();
    Orca::Perf::AddWait(waited);
    match.last_return = Clock::now();
    if (stall_shown)
    {
      PublishStall(match, 0);
      Orca::UX::SetEnd::Current().StallOver(step.kind == Orca::Net::StepKind::Run ||
                                                step.kind == Orca::Net::StepKind::Rollback,
                                            Orca::UX::SetEnd::NowMs());
    }
    if (match.session)
    {
      for (const std::string& note : match.session->TakeDelayNotes())
        NOTICE_LOG_FMT(ROLLBACK, "Online match: {}", note);
    }
    if (match.session && match.session->GetStats().stalls > stalls_before)
      match.stats_stall_ms += MsSince(step_start);
    if (step.kind == Orca::Net::StepKind::Rollback)
    {
      match.stats_rbmax = std::max(match.stats_rbmax, running_before - step.frame + 1);
      // The loaded snapshot holds the hook's writes made with the ports known then, and the
      // rollback may be due to those ports changing. Rerun the hook for the frame landed on.
      auto* record = Orca::Net::ReplayRecordingScope::Frame(step.frame);
      auto ports = PortsAt(match, step.frame);
      auto canonical = Orca::Net::SyncedPorts(ports);
      if (record)
        record->ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(std::move(canonical));
      Orca::Net::ReplayScope replay_scope(record, true);
      ReplayFrameHook(system, match, step.frame, true, ports, false, on_frame, true);
    }
    if (step.kind == Orca::Net::StepKind::Run || step.kind == Orca::Net::StepKind::Rollback)
    {
      match.running = step.frame;
      PublishStats(match);
      // Every 30 s of play with friends, log the session's numbers.
      if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
          step.frame % 1800 == 0 && !match.port->CatchingUp())
      {
        LogStats(fmt::format("at frame {}", step.frame).c_str());
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
      }
      if (match.joining)
      {
        // Unthrottled and unrendered while far behind the host.
        const int behind = match.session->AuthorityFrame() - step.frame;
        match.port->SetCatchingUp(match.session->CatchingUp() || behind > CATCH_UP_BEHIND);
        if (match.session->CatchingUp())
        {
          const int percent = 50 + std::clamp(50 - behind / 4, 0, 49);
          // The host keeps running, so `behind` can briefly grow; never go backwards.
          if (percent > match.last_percent)
          {
            match.last_percent = percent;
            Orca::Status::State(fmt::format("joining {}", percent));
          }
        }
        else if (!match.joined && match.session->Plugged(match.local_seat, step.frame))
        {
          match.joined = true;
          match.reopen_join = false;
          Orca::Status::State("playing");
          const double catch_up = MsSince(match.loaded_time);
          NOTICE_LOG_FMT(ROLLBACK,
                         "Drop-in: plugged into port {} at frame {}: catch-up {:.0f} ms, {:.0f} ms "
                         "since the keyframe was offered, {:.0f} ms since this Orca's first frame",
                         match.local_seat + 1, step.frame, catch_up, MsSince(match.offered_time),
                         MsSince(match.boot_time));
          ShowJoinMessage(
              fmt::format("Joined {}'s game on port {}", match.host_name, match.local_seat + 1),
              4000);
        }
        if (match.session->LeftDone() && step.kind == Orca::Net::StepKind::Run &&
            !match.session->Resimulating())
        {
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: unplugged at frame {}",
                         match.session->Plan()[match.local_seat].unplug_from);
          if (Soft())
          {
            FinishLeave(match);
            return rewound_to;
          }
          LogStats("at leave");
          End("left");
          StopEmulation(system, "You left your friend's game");
          return rewound_to;
        }
      }
      else
      {
        // Host: once every friend taken in has plugged in, tell the app they joined.
        if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
            std::erase_if(match.plugging,
                          [&](int seat) { return match.session->Plugged(seat, step.frame); }) > 0 &&
            match.plugging.empty() && match.waiting.empty())
        {
          Orca::Status::State("friend-joined");
          match.friend_left_told = false;
          // A matchmade room's match may begin (YouGameRoom), and the search is over.
          Orca::Online::SetOpponentPlugged(true);
          Orca::UX::Search::End();
          for (const auto& shared : match.shared_keyframes)
            DeleteStored(match, shared);
          match.shared_keyframes.clear();
        }
        // Nobody else in the session: go solo after IDLE_GRACE_FRAMES. Skip "playing" if the app
        // already heard the friend leave, or it would wipe that line.
        if (match.session->Idle() && match.waiting.empty() && match.menu_waiting.empty())
        {
          if (++match.idle_frames >= IDLE_GRACE_FRAMES)
          {
            GoSolo(match, "no friends left", match.friend_left_told ? nullptr : "playing");
            AfterFriendsGone(match);
          }
        }
        else
        {
          match.idle_frames = 0;
        }
      }
      if (step.kind == Orca::Net::StepKind::Rollback)
        return step.frame;
      return rewound_to;
    }
    if (step.kind != Orca::Net::StepKind::Ended)
    {
      // StepSession handles every other step itself; reaching this is a bug.
      ERROR_LOG_FMT(ROLLBACK, "Online: unexpected session step {} at frame {}",
                    static_cast<int>(step.kind), step.frame);
      return rewound_to;
    }
    if (stopping())
      return rewound_to;
    const std::string error = match.session->Error();
    if ((!match.joining || Soft()) && Orca::Online::RoomEnded() &&
        Orca::Online::RoomErrorCode() == "match-over" && !Orca::Online::IsDesync(error))
    {
      // A result can end recovery while the session waits at any frame. Both roles take the
      // normal result path before a stale host-left/silence/session error can report a failure.
      if (match.joining)
        BecomeSolo(match, "match over", "host-left match-over", true);
      else
      {
        DropFriends(match, "network", false);
        GoSolo(match, "match over", nullptr);
      }
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (host_left && error.empty() && !match.session->Resimulating())
    {
      // The host left: the joined game is gone (or this player's own leave is complete).
      JoinerEvents(match);
      if (!match.host_gone)
        match.host_gone = "";
      HostGone(system, match, local_pad);
      return rewound_to;
    }
    if (!left_seats.empty() && error.empty() && !match.session->Resimulating())
    {
      // The room says they left: unplug them now instead of after the silence limit, and play on
      // while the room's forfeit runs.
      for (const int seat : left_seats)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room while this game waited on it: "
                                 "unplugged at once, frame {}",
                       seat + 1, match.running + 1);
        match.session->DropSeat(seat);
      }
      // A shrinking in-play roster bounds the additional passes. The scope guard spends an
      // attempt only when that count did not drop, including duplicate departure notifications.
      continue;
    }
    if (!match.joining && match.session->CurrentFrame() == match.running + 1 &&
        (attempt == 2 || !error.empty()))
    {
      // The host plays on and unplugs whoever stopped answering or broke the session. If the host's
      // own room went away, KeepRoomOpen reports that instead of "your friend left".
      const bool room_gone = Orca::Online::RoomEnded();
      WARN_LOG_FMT(ROLLBACK, "Drop-in: unplugging every friend at frame {}: {}", match.running + 1,
                   peer_silent ? "no word from them" : error);
      // Mid ranked set the screen already counts their forfeit down; after it they're just gone.
      const bool ranked_stall = error.empty() && Orca::Online::RoomQueue() == "ranked" &&
                                Orca::Online::MatchLive();
      const bool set_over = error.empty() && Orca::Online::SetOver();
      if (set_over)
        Orca::UX::SetEnd::Current().OpponentLeftAfterSet(Orca::UX::SetEnd::NowMs());
      if (!room_gone && !ranked_stall && !set_over)
      {
        Core::DisplayMessage(peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                           fmt::format("Your friend was disconnected: {}", error),
                             6000);
      }
      if (error.empty())
      {
        const auto missing = DropMissingFriends(*match.session, doubles);
        if (missing)
        {
          for (const int seat : *missing)
          {
            match.seated.erase(seat);
            match.plugging.erase(seat);
            match.doubles_seats.erase(seat);
            Orca::Online::DropPeer(seat, "stalled");
          }
          match.friend_left_told = true;
          Orca::Status::State("friend-left stalled");
          if (match.seated.empty())
            Orca::Online::SetOpponentPlugged(false);
          continue;
        }
        // Silent friends: unplug them and step again; a silent ranked opponent loses the set.
        if (ranked_stall)
          Orca::UX::SetEnd::Current().OpponentDropped(Orca::UX::SetEnd::NowMs());
        Orca::Online::ReportStall();
        DropFriends(match, "stalled");
        continue;
      }
      if (Orca::Online::IsDesync(error))
        ++s_desyncs;
      // The opponent broke the session: the room's match hears it void before anything else.
      if (VoidFirst({.queue_room = Orca::Online::RoomQueue() != "private",
                     .match_live = Orca::Online::MatchLive(),
                     .set_over = Orca::Online::SetOver(),
                     .room_ended = room_gone,
                     .from_peer = match.session->ErrorFromPeer()}))
      {
        Orca::Online::ReportPeerFault();
      }
      DropFriends(match, Orca::Online::IsDesync(error) ? "desync" : "network",
                  !room_gone && attempt == 2);
      GoSolo(match, "the session failed");
      if (!room_gone)
        AfterFriendsGone(match);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && match.joining && peer_silent && error.empty() && LeavingPastUnplug(match))
    {
      // Only the host's final acknowledgement was missing: the leave is complete anyway.
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no word from the host for {} s after leaving, frame {}",
                     LEAVE_SILENCE_LIMIT.count(), match.running + 1);
      FinishLeave(match);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    WARN_LOG_FMT(ROLLBACK, "Online: session ended at frame {}: {}", match.running + 1,
                 error.empty() ? (peer_silent ? "no word from the host" : "no error") : error);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: session state at the end: {}", match.session->Describe());
    if (match.joining && (error.empty() || error == "Connection lost") && WaitForAwayHost(match))
    {
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (peer_silent)
    {
      WARN_LOG_FMT(ROLLBACK, "Online: no word from the host for {} s at frame {}",
                   PEER_SILENCE_LIMIT.count(), match.running);
      Orca::Online::ReportPeerStalled(PEER_SILENT_SENTENCE);
    }
    const bool survivable = Orca::Online::ReportSessionEnd(error, match.session->ErrorFromPeer());
    if (Orca::Online::IsDesync(error))
      ++s_desyncs;
    if (Soft() && survivable && match.joining && Orca::Online::RoomQueue() == "ranked" &&
        Orca::Online::MatchLive() && !Orca::Online::RoomEnded())
    {
      // Ranked host went quiet: stay (leaving would forfeit) and wait for the verdict.
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host went quiet mid-set: waiting for the verdict");
      if (peer_silent)
        Orca::Online::ReportStall();
      // The screen counts their forfeit down (UX/SetEnd.h).
      Orca::UX::SetEnd::Current().OpponentDropped(Orca::UX::SetEnd::NowMs());
      BecomeSolo(match, "the session ended", "host-left", false, true);
      match.linger_until = Clock::now() + VERDICT_WAIT;
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && survivable && match.joining && Orca::Online::RoomQueue() == "ranked" &&
        Orca::Online::SetOver())
    {
      // Set over: the host is just gone; stay on the results screen until this player leaves too.
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host's game ended after the set (set over): "
                               "this game stays until its player leaves");
      Orca::UX::SetEnd::Current().OpponentLeftAfterSet(Orca::UX::SetEnd::NowMs());
      BecomeSolo(match, "the session ended", "host-left", false, true);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && survivable && match.joining)
    {
      // Unplugged from the host's game; this one goes on.
      Core::DisplayMessage(peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                         "The connection to your friend's game ended: you play on",
                           6000);
      BecomeSolo(match, "the session ended", "host-left");
      RunSolo(match, local_pad);
      return rewound_to;
    }
    End("session ended");
    StopEmulation(system, peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                        fmt::format("Online match ended: {}",
                                                    error.empty() ? "stopped" : error));
    return rewound_to;
  }
  return rewound_to;
}

// Notes a fight in the ranked room this queue image is for (MaybeRestoreQueueImage).
void NoteRankedFight(Match& match)
{
  if (match.ranked_fought || !match.queue_image || Orca::Online::RoomEnded() ||
      Orca::Online::RoomQueue() != "ranked")
  {
    return;
  }
  if (Orca::UX::Game().Results().LatestScene() == Orca::Engine::SceneKind::Fight)
  {
    match.ranked_fought = true;
    NOTICE_LOG_FMT(ROLLBACK, "Queue: a ranked fight in room {} (frame {}): back not ready after it",
                   Orca::Online::Code(), match.running + 1);
  }
}

// Set over: after a ranked set's verdict, both players stay on the results screen and can chat on
// the page. This game leaves the room when its results screen is over (read at a final frame), when
// the player holds B for SET_OVER_HOLD (B is masked from the game there: OnlineRules GateMasks),
// or, once the opponent is gone, on Start or A. Reads only the local pad and never changes the
// game.
void SetOverLeave(Match& match)
{
  Orca::UX::SetEnd::Model& set_end = Orca::UX::SetEnd::Current();
  if (!Orca::Online::SetOver() || match.finished)
  {
    if (std::exchange(match.set_over_hint, false))
      set_end.LeaveHint(false, false, 0);
    match.set_over_left = false;
    match.set_over_hold_since.reset();
    match.set_over_buttons = 0xFFFF;
    return;
  }
  if (match.set_over_left)
    return;
  using Scene = Orca::Engine::SceneKind;
  const std::optional<Scene> final_scene = Orca::UX::Game().Results().FinalScene();
  const bool on_results = Orca::UX::Game().Results().LatestScene() == Scene::Results;
  // The opponent's game is gone: the host's session ended, or the joiner plays solo.
  const bool alone = !match.session;
  const u16 buttons = s_local_pad ? Orca::Net::DecodePad(*s_local_pad).button : u16{0};
  const u16 pressed = static_cast<u16>(buttons & ~match.set_over_buttons);
  match.set_over_buttons = buttons;
  const auto now = Clock::now();
  if (on_results && (buttons & PAD_BUTTON_B))
  {
    if (!match.set_over_hold_since)
      match.set_over_hold_since = now;
  }
  else
  {
    match.set_over_hold_since.reset();
  }
  const float held =
      match.set_over_hold_since ?
          std::min(1.0f, static_cast<float>(std::chrono::duration<double>(
                                                now - *match.set_over_hold_since) /
                                            SET_OVER_HOLD)) :
          0.0f;
  const char* why = nullptr;
  if (final_scene && *final_scene != Scene::Results)
    why = "the game is off the results screen";
  else if (match.set_over_hold_since && now - *match.set_over_hold_since >= SET_OVER_HOLD)
    why = "B held";
  else if (alone && on_results && (pressed & (PAD_BUTTON_START | PAD_BUTTON_A)))
    why = "the opponent is gone and the player pressed on";
  match.set_over_hint = on_results && !why;
  set_end.LeaveHint(match.set_over_hint, alone, held);
  if (!why)
    return;
  match.set_over_left = true;
  match.set_over_hold_since.reset();
  set_end.LeavingSetRoom(Orca::UX::SetEnd::NowMs());
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: set over: leaving room {} ({}), frame {}",
                 Orca::Online::Code(), why, match.running + 1);
  Orca::Online::RequestLeave();
}
}  // namespace

std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame,
                              const std::function<void(int local_seat)>& local_pad_waiting)
{
  s_solo_idle = false;
  PerfBoundary(TheMatch());
  const bool was_replaying = TheMatch().replaying != nullptr;
  // The local pad as this boundary read it for the game (for SetOverLeave).
  const std::function<Orca::Net::Pad(int)> seen = [&local_pad](int local_seat) {
    const Orca::Net::Pad pad = local_pad(local_seat);
    s_local_pad = pad;
    return pad;
  };
  const std::optional<int> rewound_to = Boundary(system, seen, on_frame, local_pad_waiting);
  if (TheMatch().replaying || (was_replaying && !TheMatch().session))
  {
    s_solo_idle = SoloIdle(TheMatch());
    return rewound_to;
  }
  ConfirmResults(TheMatch());
  NoteRankedFight(TheMatch());
  SetOverLeave(TheMatch());
  Orca::UX::Game().SetLocalPort(TheMatch().local_seat);
  s_solo_idle = SoloIdle(TheMatch());
  return rewound_to;
}

void LogStats(const char* when)
{
  const Match& match = TheMatch();
  if (!match.session)
    return;
  const Orca::Net::Stats& stats = match.session->GetStats();
  NOTICE_LOG_FMT(
      ROLLBACK,
      "Online match {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, {} stalls ({} "
      "counted), {} waits, {} hitches here ({} more in game loads), {} there, frame advantage "
      "{:.2f}, input delay {}{} "
      "(+{} -{} reverted {}), {} counted stalls a frame more would have spared, {} checksums "
      "matched, confirmed frame {}, round trip {} ms (median {} ms here, {} ms there), link {}, "
      "{} snapshots (every {}, {:.2f} ms each), {} found late{}{}",
      when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
      stats.deepest_rollback, stats.stalls, stats.counted_stalls, stats.waits, stats.hitches,
      stats.load_hitches, stats.peer_hitches, stats.frame_advantage, stats.delay,
      stats.delay_fixed ? " (chosen)" : "", stats.delay_raises, stats.delay_lowers,
      stats.delay_reverts, stats.spared_stalls, stats.checksums_matched, stats.confirmed_frame,
      Orca::Online::RoundTripMs(), stats.rtt_median, stats.peer_rtt_median,
      Orca::Online::Direct().line, stats.saves, stats.snapshot_every,
      match.port ? match.port->SaveMs() : 0.0, stats.late_seats,
      match.session->Error().empty() ? "" : "; ", match.session->Error());
}

void ReportJoinFailure(std::string_view code, std::string_view sentence)
{
  if (code == "host_gone")
    Orca::Status::Event("host-left host_gone");
  else if (code == "match-over")
    Orca::Status::Event("host-left match-over");
  else
    Orca::Status::Report(code.empty() ? "network" : code, sentence);
}

bool Active()
{
  const Match& match = TheMatch();
  return match.started && !match.finished;
}

void End(const char* reason)
{
  Match& match = TheMatch();
  if (!match.started || match.finished)
    return;
  match.finished = true;
  s_solo_idle = false;
  LogStats(fmt::format("ended ({})", reason).c_str());
  // Transfers in flight give up now; their threads are joined below.
  if (match.store)
    match.store->Cancel();
  match.session.reset();
  match.port.reset();
  match.download.reset();
  match.replaying.reset();
  match.job.reset();
  if (match.keyframe)
    DeleteStored(match, *match.keyframe);
  for (const auto& shared : match.shared_keyframes)
    DeleteStored(match, shared);
  Orca::Online::Shutdown();
}

bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe)
{
  return solo_idle && !drop_in_pending &&
         !(stored_keyframe && frame - *stored_keyframe <= KEYFRAME_FRESH_FRAMES);
}

bool HeaderFreeAt(int frame, bool solo_quiet, std::optional<int> stored_keyframe)
{
  return solo_quiet && !(stored_keyframe && frame - *stored_keyframe <= KEYFRAME_FRESH_FRAMES);
}

bool OwnValuesKept(const std::vector<Orca::Net::KeyframeInfo::Name>& names, int seat, int plug_from,
                   const OwnValues& own)
{
  std::vector<const Orca::Net::KeyframeInfo::Name*> mine;
  for (const auto& name : names)
  {
    if (name.seat == seat)
      mine.push_back(&name);
  }
  std::stable_sort(mine.begin(), mine.end(),
                   [](const auto* a, const auto* b) { return a->from < b->from; });
  const auto kept = [&own](const Orca::Net::KeyframeInfo::Name* entry) {
    if (!entry)
      return own.controls.empty() && own.queue.empty();
    return entry->controls == own.controls && entry->queue == own.queue &&
           (own.name.empty() || entry->name == own.name);
  };
  if (plug_from == Orca::Net::NEVER)
    return kept(mine.empty() ? nullptr : mine.back());
  const Orca::Net::KeyframeInfo::Name* in_force = nullptr;
  for (const auto* entry : mine)
  {
    if (entry->from <= plug_from)
      in_force = entry;
    else if (!kept(entry))
      return false;
  }
  return kept(in_force);
}

void StallBudget::Add(Clock::time_point start, Clock::time_point end)
{
  if (end <= start)
    return;
  std::erase_if(m_stalls, [&](const auto& stall) { return stall.second <= end - STALL_WINDOW; });
  m_stalls.emplace_back(start, end);
}

bool StallBudget::ExcuseRecovered(Clock::time_point now)
{
  std::erase_if(m_recoveries, [&](Clock::time_point at) { return at <= now - RECOVERY_WINDOW; });
  if (static_cast<int>(m_recoveries.size()) >= MAX_RECOVERIES)
    return false;
  m_recoveries.push_back(now);
  return true;
}

bool StallBudget::Poll(Clock::time_point now, Clock::time_point step_start, bool recovering)
{
  ObserveRecovery(recovering);
  auto total = Stalled(now);
  if (!m_recovery_seen)
  {
    for (const auto& [start, end] : m_pending_stalls)
      if (end > now - STALL_WINDOW)
        total += end - std::max(start, now - STALL_WINDOW);
    total += now - std::max(step_start, now - STALL_WINDOW);
  }
  return total >= STALL_BUDGET;
}

void StallBudget::FinishStep(std::optional<Clock::time_point> stall_start, Clock::time_point end,
                              bool still_stalled, bool resumed)
{
  if (stall_start && end > *stall_start)
    m_pending_stalls.emplace_back(*stall_start, end);
  // A partial packet can cause a rollback before it supplies the inputs we stalled on.
  if (still_stalled && resumed)
    return;
  bool excused = false;
  if (!m_pending_stalls.empty() && m_recovery_seen && resumed)
  {
    Clock::duration duration{};
    for (const auto& [start, until] : m_pending_stalls)
      duration += until - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    // A short stall during a recovery (the frames right after a resume, a hiccup before the room
    // notices) is not the outage: it counts as usual and spends no excuse, so one outage costs one.
    if (duration >= MIN_EXCUSED_STALL)
    {
      excused = ExcuseRecovered(end);
      if (excused)
        NOTICE_LOG_FMT(ROLLBACK,
                       "Online: stall of {} ms excused: recovered outage ({} of {} in {} min)",
                       ms, Recoveries(), MAX_RECOVERIES, RECOVERY_WINDOW.count());
      else
        NOTICE_LOG_FMT(ROLLBACK,
                       "Online: stall of {} ms not excused: recovered outage cap reached "
                       "({} of {} in {} min)",
                       ms, Recoveries(), MAX_RECOVERIES, RECOVERY_WINDOW.count());
    }
  }
  if (!excused)
    for (const auto& [start, until] : m_pending_stalls)
      Add(start, until);
  m_pending_stalls.clear();
  m_recovery_seen = false;
}

StallBudget::Clock::duration
StallBudget::Stalled(Clock::time_point now, std::optional<Clock::time_point> ongoing_since) const
{
  const Clock::time_point from = now - STALL_WINDOW;
  Clock::duration total{};
  const auto add = [&](Clock::time_point start, Clock::time_point end) {
    start = std::max(start, from);
    end = std::min(end, now);
    if (end > start)
      total += end - start;
  };
  for (const auto& [start, end] : m_stalls)
    add(start, end);
  if (ongoing_since)
    add(*ongoing_since, now);
  return total;
}

bool VoidFirst(const VerdictWaitInputs& in)
{
  return in.queue_room && in.match_live && !in.set_over && !in.room_ended &&
         (in.from_peer || in.waited_out);
}

bool MayComeHome(const HomeInputs& in)
{
  return in.local_seat != 0 && !in.joining && !in.session && !in.room_up && in.solo_idle &&
         !in.drop_in_pending && in.on_menus && !in.queue_image && !in.lingering;
}

LobbyPickPlan DecideLobbyPick(const LobbyPickInputs& in)
{
  LobbyPickPlan plan;
  if (in.joining || in.queue_room)
    return plan;
  const bool friends =
      (in.session && !in.session_idle) || in.drop_in_friends || in.arrival_pending;
  if (!friends)
  {
    plan.step = LobbyPickStep::Announce;
    plan.drop_keyframe = in.keyframe_kept && !in.session;
    return plan;
  }
  if (!in.host_cap || in.local_seat != 0)
    return plan;
  if (in.pick_cap)
  {
    plan.step = LobbyPickStep::Leave;
    return plan;
  }
  plan.step = LobbyPickStep::Announce;
  plan.arm = false;
  return plan;
}

bool AnnounceHomePick(bool css_pick, bool host_cap, bool queue_or_search)
{
  return css_pick && host_cap && !queue_or_search;
}

bool FriendEndsQueue(const FriendQueueInputs& in)
{
  return in.friend_coming && in.queue_or_search && in.local_seat == 0 && !in.joining &&
         in.friends_room;
}

bool JoinEndsQueue(bool queue_room, bool queue_or_search, bool queue_image)
{
  return !queue_room && (queue_or_search || queue_image);
}

FriendsPickStep DecideFriendsPick(const FriendsPickInputs& in)
{
  if (in.queue_or_search || in.elsewhere || !in.host_cap)
    return FriendsPickStep::Drop;
  return in.played && in.alone && in.on_pick_select ? FriendsPickStep::Arm : FriendsPickStep::Wait;
}

KeptPickStep DecideKeptPick(const KeptPickInputs& in)
{
  if (!in.stands || in.queue_or_search)
    return KeptPickStep::Drop;
  return in.alone ? KeptPickStep::Arm : KeptPickStep::Wait;
}

Orca::Net::Pads CanonicalBootPads()
{
  Orca::Net::Pads pads;
  pads.fill(Orca::Net::UNPLUGGED_PAD);
  pads[0] = Orca::Net::Pad{};
  return pads;
}

OriginStep DecideOrigin(int frame, int menu_built_streak, std::optional<u32> jit_clear_frame)
{
  if (menu_built_streak >= ORIGIN_SETTLE_FRAMES &&
      (!jit_clear_frame || frame > static_cast<int>(*jit_clear_frame)))
  {
    return OriginStep::Capture;
  }
  return frame >= ORIGIN_DEADLINE ? OriginStep::Fail : OriginStep::Wait;
}

OriginRule SelectOriginRule(const Orca::Mod::Manifest* manifest, bool menu_supported,
                            bool guards_failed)
{
  if (!manifest)
    return menu_supported ? OriginRule::Menu : OriginRule::Frame0;
  if (manifest->builtin != Orca::Mod::Builtin::None && !guards_failed)
    return OriginRule::Menu;
  if (manifest->origin)
    return OriginRule::Declared;
  return manifest->builtin == Orca::Mod::Builtin::None ? OriginRule::Frame0 : OriginRule::Failed;
}

OriginStep DecideDeclaredOrigin(int frame, int streak, const Orca::Mod::OriginDecl& decl)
{
  if (frame < static_cast<int>(decl.frame))
    return OriginStep::Wait;
  if (decl.words.empty())
    return frame == static_cast<int>(decl.frame) ? OriginStep::Capture : OriginStep::Fail;
  if (streak >= static_cast<int>(decl.settle))
    return OriginStep::Capture;
  return frame >= static_cast<int>(decl.frame) + ORIGIN_WORDS_WINDOW ? OriginStep::Fail :
                                                                    OriginStep::Wait;
}

std::string_view OriginFailureReport(const Orca::Mod::Manifest* manifest)
{
  return manifest ? "This pack's game didn't reach the start point its pack names, so friends "
                    "can't join." : "";
}

std::string_view RoomRefusal(std::string_view queue, bool results)
{
  return queue == "ranked" && !results ?
             "This game doesn't report match results, so it can't play Ranked. Try Casual or "
             "With Friends." : "";
}

int DeclaredOriginStreak(int frame, int streak, const Orca::Mod::OriginDecl& decl,
                         const Orca::UX::GuestMemory& memory)
{
  if (frame < static_cast<int>(decl.frame) || decl.words.empty())
    return 0;
  for (const auto& word : decl.words)
  {
    if (!memory.Valid(word.address) || memory.Read32(word.address) != word.word)
      return 0;
  }
  return std::min(streak + 1, static_cast<int>(decl.settle));
}

FreshKind FreshKindForOrigin(OriginRule rule)
{
  return rule == OriginRule::Declared ? FreshKind::Reset : FreshKind::Menu;
}

bool FreshExitAllowed(FreshKind kind, u8 exit)
{
  return kind == FreshKind::Reset ? exit == 0 : Orca::Net::ValidMenuExit(exit);
}

bool FreshTailSettled(FreshKind kind, int css_streak, int length)
{
  return kind == FreshKind::Reset || css_streak >= FRESH_CSS_SETTLE || length >= FRESH_TAIL_LIMIT;
}

Orca::Net::ReplayArchive FreshReplayStart(FreshKind kind, int frame, u64 origin_hash, u8 exit,
                                         u32 seed)
{
  Orca::Net::ReplayArchive replay;
  replay.origin_hash = origin_hash;
  replay.first_frame = frame;
  if (kind == FreshKind::Menu)
  {
    replay.frames.resize(1);
    replay.frames[0].menu_exit = exit;
    replay.frames[0].seed = seed;
  }
  return replay;
}

bool LocalNandReady(bool is_wii, bool has_host_nand, bool temporary)
{
  return !is_wii || (has_host_nand && temporary);
}

std::string RoomReopenCode(const std::string& previous_code, const std::string& queue,
                            bool held, bool taken)
{
  return held && !taken && QueueMode(queue) == Orca::Engine::Mode::None ? previous_code :
                                                                       std::string();
}

bool PumpRoomReopenForTest()
{
  Match match;
  KeepRoomOpen(match);
  return match.joining && match.reopen_join;
}

std::vector<unsigned> TraceRoomReopenForTest(const std::vector<std::chrono::seconds>& times,
                                              const std::function<void()>& after_step)
{
  Match match;
  std::vector<unsigned> attempts;
  for (const auto time : times)
  {
    KeepRoomOpen(match, Clock::time_point{} + time);
    after_step();
    attempts.push_back(match.reopen_attempts);
  }
  return attempts;
}

int PumpRoomReopenFailureForTest(const std::function<void()>& wait_for_failure)
{
  Core::System& system = Core::System::GetInstance();
  Match match;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK, true);
  match.running = 0;
  KeepRoomOpen(match);
  if (!match.joining || !match.reopen_join)
    return -1;
  wait_for_failure();
  if (PumpJoin(match, &match.join_status) != JoinPump::Failed)
    return -1;
  FailJoin(system, match);
  if (match.joining || match.finished || !match.reopen_pending)
    return -1;
  RunSolo(match, [](int) { return Pad{}; });
  std::this_thread::sleep_until(*match.reopen_pending);
  KeepRoomOpen(match);
  return match.running;
}

static HostRecoveryResult PumpHostForTest(Core::System& system, Orca::Net::YouGameRoom& room,
                                          Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                          int current_frame, bool session_alive,
                                          const std::vector<int>& seated, std::optional<bool> menus,
                                          std::unique_ptr<Orca::Net::KeyframeStore> store,
                                          bool host_requested = false)
{
  // This adapter supplies a headless game's history to the actual HostEvents/StartKeyframe path.
  // The room runs its real I/O thread; packing, encryption and keyframe offers are unchanged.
  Match match;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  match.store = std::move(store);
  match.origin_frame = replay.first_frame;
  match.origin_rule = OriginRule::Menu;
  match.running = current_frame - 1;
  match.replay = std::move(replay);
  // HostInPlay arms a queue fresh start before this room's welcome arrives.
  match.fresh_queue_armed = host_requested;
  if (!match.replay.frames.empty() && match.replay.frames.back().ports)
  {
    for (const auto& port : *match.replay.frames.back().ports)
      AddName(match, {port.port, match.origin_frame, port.name, port.controls, port.queue});
  }
  match.log_base = match.origin_frame;
  match.log.resize(current_frame - match.log_base);
  match.seated.insert(seated.begin(), seated.end());
  if (room.Doubles())
    match.doubles_seats = match.seated;
  if (session_alive)
  {
    Orca::Net::Config config;
    config.seats = Orca::Net::MAX_SEATS;
    config.per_peer_inputs = room.Doubles();
    config.local_seat = 0;
    config.authority_seat = 0;
    config.start_frame = current_frame;
    std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
    plan[0] = {0, Orca::Net::NEVER, 0};
    for (const int seat : seated)
      plan[seat] = {0, Orca::Net::NEVER, 0};
    config.plan = plan;
    match.session = std::make_unique<Orca::Net::Session>(config, game, room);
    match.session->SetLog(match.log_base, match.log);
  }
  HostEvents(system, match, &room, menus);
  HostRecoveryResult result;
  if (match.job)
  {
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (!match.job->done && Clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (match.job->done && match.job->ok)
      result.keyframe_frame = match.job->info.frame;
    HostEvents(system, match, &room, menus);
  }
  result.fresh_start = match.fresh_pending.has_value();
  result.seated = match.waiting.empty() && match.seated.contains(1);
  result.current_frame = match.running + 1;
  result.seats.assign(match.seated.begin(), match.seated.end());
  return result;
}

HostRecoveryResult PumpRecoveryHostForTest(Core::System& system, Orca::Net::YouGameRoom& room,
                                          Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                          int current_frame, bool session_alive,
                                          std::unique_ptr<Orca::Net::KeyframeStore> store)
{
  return PumpHostForTest(system, room, game, std::move(replay), current_frame, session_alive,
                         {1}, std::nullopt, std::move(store));
}

HostRecoveryResult PumpFriendsHostForTest(Core::System& system, Orca::Net::YouGameRoom& room,
                                         Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                         int current_frame, const std::vector<int>& seated,
                                         bool on_menus,
                                         std::unique_ptr<Orca::Net::KeyframeStore> store,
                                         bool host_requested)
{
  return PumpHostForTest(system, room, game, std::move(replay), current_frame, !seated.empty(),
                         seated, on_menus, std::move(store), host_requested);
}

int PumpFriendsHostBoundaryForTest(std::unique_ptr<Orca::Net::Session>& session, int completed,
                                   const std::vector<int>& seated,
                                   const std::function<void()>& poll,
                                   const std::function<std::vector<int>()>& left_in_play,
                                   bool joining)
{
  Core::System& system = Core::System::GetInstance();
  Match match;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK, true);
  match.joining = joining;
  match.local_seat = joining ? 1 : 0;
  match.running = completed;
  match.seated.insert(seated.begin(), seated.end());
  match.session = std::move(session);
  SessionBoundary(system, match, [](int) { return Pad{}; }, [](int, bool, const auto&, bool) {},
                  [&](int) { poll(); }, std::nullopt, true, [] { return false; },
                  std::chrono::milliseconds(20), left_in_play);
  session = std::move(match.session);
  return match.running;
}

std::optional<std::vector<int>> FriendsMissingInputs(const Orca::Net::Session& session, bool doubles)
{
  if (!doubles)
    return std::nullopt;
  auto seats = session.MissingInputSeats();
  return seats.empty() ? std::nullopt : std::optional(std::move(seats));
}

std::optional<std::vector<int>> DropMissingFriends(Orca::Net::Session& session, bool doubles)
{
  auto seats = FriendsMissingInputs(session, doubles);
  if (seats)
    for (const int seat : *seats)
      session.DropSeat(seat);
  return seats;
}

int WaitingFriendSeat(int local_seat, const std::vector<int>& missing, bool joining,
                       int host_seat, const std::vector<int>& seated)
{
  const auto other = [local_seat](int seat) { return seat >= 0 && seat != local_seat; };
  if (const auto seat = std::ranges::find_if(missing, other); seat != missing.end())
    return *seat;
  if (joining && other(host_seat))
    return host_seat;
  if (const auto seat = std::ranges::find_if(seated, other); seat != seated.end())
    return *seat;
  return -1;
}

bool WaitForFriendsRoster(const Orca::Net::Session& session, bool joining, bool doubles)
{
  const auto seats = FriendsMissingInputs(session, doubles);
  return joining && seats && std::ranges::find(*seats, session.AuthoritySeat()) == seats->end();
}

bool FriendsBatch::Ready(bool on_menus, int waiting, int plugging, bool recovering,
                         std::chrono::steady_clock::time_point now)
{
  if (waiting == 0)
  {
    Clear();
    return true;
  }
  if (recovering)
    return true;
  if (!on_menus || plugging != 0)
  {
    Clear();
    return false;
  }
  if (!m_until)
    m_until = now + std::chrono::seconds(2);
  return waiting >= Orca::Net::MAX_SEATS - 1 || now >= *m_until;
}

std::optional<JoinCapture> ChooseJoinCapture(const Orca::Net::Session* session, bool doubles,
                                            int next, int earliest)
{
  if (next < earliest)
    return std::nullopt;
  if (!session || session->Settled())
    return JoinCapture{next, std::nullopt};
  if (doubles)
  {
    for (auto entry = session->LocalChecksums().rbegin(); entry != session->LocalChecksums().rend();
         ++entry)
    {
      if (entry->first >= earliest && entry->first <= next)
        return JoinCapture{entry->first, entry->second};
    }
  }
  return std::nullopt;
}

FreshStep DecideFreshStart(const FreshInputs& in)
{
  if (!in.enabled || in.joining || in.session || in.seated_or_plugging)
    return FreshStep::None;
  // A queue room's host: once its welcome names the queue. No keyframe before.
  if (in.queue_armed)
  {
    if (!in.origin_ready || in.busy || !in.in_queue_room)
      return FreshStep::Hold;
    return FreshStep::Now;
  }
  // A queue room that had its fresh start (or never needed one) takes its opponent in place.
  if (in.in_queue_room || !in.waiting || in.in_place)
    return FreshStep::None;
  if (!in.origin_ready || in.busy || in.single_player)
    return FreshStep::Hold;
  if (in.fresh_after >= 0)
    return in.history_frames > in.fresh_after ? FreshStep::Now : FreshStep::None;
  const bool due = in.kind == FreshKind::Reset ? in.history_time > FRESH_RESET_FRIENDS_AFTER :
                                                in.history_frames > FRESH_FRIENDS_AFTER;
  return due ? FreshStep::Now : FreshStep::None;
}

FreshStep WarnBeforeResetJoin(FreshStep step, FreshKind kind, bool queue_armed,
                             std::optional<Clock::time_point>* warned_at, Clock::time_point now)
{
  if (step != FreshStep::Now || kind != FreshKind::Reset || queue_armed)
  {
    warned_at->reset();
    return step;
  }
  if (!*warned_at)
  {
    *warned_at = now;
    return FreshStep::Warn;
  }
  return now - **warned_at < FRESH_RESET_JOIN_WARNING ? FreshStep::Hold : FreshStep::Now;
}

u8 FriendsFreshExit(std::optional<bool> css_pick_ranked)
{
  if (!css_pick_ranked)
    return Orca::Net::MENU_EXIT_FRIENDS;
  return *css_pick_ranked ? Orca::Net::MENU_EXIT_RANKED : Orca::Net::MENU_EXIT_CASUAL;
}

bool FreshUnseen(bool queue_room, int frames)
{
  return queue_room && frames < FRESH_UNSEEN_LIMIT;
}

bool HeaderFreeWhileCatchingUp(bool unseen_hold, bool in_tail, bool joining)
{
  return unseen_hold && !in_tail && !joining;
}

HeaderWait HeaderWaitAt(int* waited, bool in_tail, bool unseen, bool in_place, bool someone_waiting,
                        bool queue_room)
{
  if (in_tail || (unseen && !in_place))
    return HeaderWait::Wait;
  return StepHeaderWait(waited, in_place, someone_waiting, queue_room);
}

HeaderWait StepHeaderWait(int* waited, bool in_place, bool someone_waiting, bool queue_room)
{
  if (in_place || !someone_waiting)
  {
    *waited = 0;
    return in_place ? HeaderWait::Ready : HeaderWait::Wait;
  }
  ++*waited;
  if (queue_room)
  {
    if (*waited <= HEADER_FAIL_BOUNDARIES)
      return HeaderWait::Wait;
    *waited = 0;
    return HeaderWait::Fail;
  }
  return *waited > HEADER_WAIT_BOUNDARIES ? HeaderWait::Ready : HeaderWait::Wait;
}

bool SoloPauseAllowed()
{
  // A session with no room (tests): nobody else's game is affected.
  if (!Orca::Online::Enabled())
    return true;
  return s_solo_idle && !Orca::Online::DropInPending();
}

bool BeginPause()
{
  const bool was_pausing = s_pausing.exchange(true);
  if (!SoloPauseAllowed())
  {
    if (!was_pausing)
      s_pausing = false;
    return false;
  }
  s64 none = 0;
  s_paused_since_ns.compare_exchange_strong(none, NowNs());
  return true;
}

void EndPause()
{
  s_pausing = false;
  if (const s64 since = s_paused_since_ns.exchange(0); since != 0)
  {
    const s64 paused = NowNs() - since;
    s_paused_ns += paused;
    s_paused_total_ns += paused;
  }
}

bool Pausing()
{
  return s_pausing;
}

void WatchRoomWhilePaused()
{
  Match& match = TheMatch();
  // A pause only goes ahead solo (SoloPauseAllowed), so no session or join is under way.
  if (!s_pausing || !match.started || match.finished || match.joining || match.session)
    return;
  KeepRoomOpen(match);
}

std::chrono::nanoseconds TakePausedTime()
{
  return std::chrono::nanoseconds(s_paused_ns.exchange(0));
}
}  // namespace Rollback::OnlineMatch
