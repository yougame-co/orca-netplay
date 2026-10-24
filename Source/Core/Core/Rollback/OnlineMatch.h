// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Online drop-in play: friends join this game through a YouGame room. The host plays solo from
// boot; a friend who joins goes back to its own origin, replays the host's inputs since that
// origin, and takes the next free port. From there a rollback session over the room's transport
// runs both games. Before a queue match, and before a friend joins a host that has played on its
// own for a while, the host goes back to its origin too (a fresh start), so a join replays only the
// match's own frames. See ORCA.md, "Drop-in".

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/YouGameRoom.h"

namespace Core
{
class System;
}

namespace Orca::Mod
{
struct Manifest;
struct OriginDecl;
}
namespace Orca::Net
{
struct ReplayArchive;
class KeyframeStore;
}
namespace Orca::UX
{
class GuestMemory;
}

namespace Rollback::OnlineMatch
{
// Runs at each frame boundary. The first call joins the room; a joiner blocks here until it has
// loaded the host's keyframe (or emulation stops). `local_pad(seat)` returns this player's pad.
// Returns the frame the state was rewound to (a rollback, or a joiner's keyframe); the state is
// then at the start of that frame.
// `on_frame` runs first at every boundary, before anything is saved: `ports` are the controllers
// plugged in at that frame, and `alone` is AloneAt for it (false on re-runs).
// `local_pad_waiting(seat)` runs at each tick while the session holds a frame on the other
// players (a stall or a time-sync wait). Host only: it must not touch the pad the frame records
// and sends, or any emulated state.
using FrameHook = std::function<void(int frame, bool resimulating,
                                     const std::vector<Orca::Events::PortInfo>& ports, bool alone)>;
std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame,
                              const std::function<void(int local_seat)>& local_pad_waiting = {});

// A keyframe at most this old (30 s at 60 fps) still serves a new joiner, who replays the frames
// since.
constexpr int KEYFRAME_FRESH_FRAMES = 30 * 60;

// Wall clock only: no batching timer enters the replay or session packets.
class FriendsBatch
{
public:
  bool Ready(bool on_menus, int waiting, int plugging, bool recovering,
             std::chrono::steady_clock::time_point now, int local_seats = 1);
  void Clear() { m_until.reset(); }
private:
  std::optional<std::chrono::steady_clock::time_point> m_until;
};

// Boundary's silence policy: an empty selector retains the ordinary recovery/drop behavior.
std::optional<std::vector<int>> FriendsMissingInputs(const Orca::Net::Session& session, bool doubles);
int WaitingFriendSeat(int local_seat, const std::vector<int>& missing, bool joining,
                       int host_seat, const std::vector<int>& seated);
bool WaitForFriendsRoster(const Orca::Net::Session& session, bool joining, bool doubles);
std::optional<std::vector<int>> DropMissingFriends(Orca::Net::Session& session, bool doubles);

struct JoinCapture
{
  int frame;
  std::optional<u64> hash;
};
// Active multi-peer sessions may have predictions every frame: use a final checkpoint's hash.
std::optional<JoinCapture> ChooseJoinCapture(const Orca::Net::Session* session, bool doubles,
                                            int next, int earliest);

// Reopen a held friends room under its code; a queue, expired or taken room needs a fresh code.
std::string RoomReopenCode(const std::string& previous_code, const std::string& queue,
                            bool held, bool taken);
// Pumps the actual reopen path against Online's room and returns whether it adopted a friend.
bool PumpRoomReopenForTest();
// Pumps the actual automatic reopen loop at supplied times, with a callback after each step.
std::vector<unsigned> TraceRoomReopenForTest(const std::vector<std::chrono::seconds>& times,
                                              const std::function<void()>& after_step);
// Adopts the room, waits for its failure, then pumps the actual fallback and one solo frame.
int PumpRoomReopenFailureForTest(const std::function<void()>& wait_for_failure);

struct LaunchJoinBoundaryResult
{
  bool finished;
  bool joining;
  bool has_port;
  int frame;
};
// Runs the actual boundary before a launch joiner's origin, against Online's room.
LaunchJoinBoundaryResult PumpLaunchJoinBoundaryForTest();

// Refused joins whose host or match ended use the app's existing state events.
void ReportJoinFailure(std::string_view code, std::string_view sentence);

// Headless runtime fixture: pumps the same host event/keyframe path with a real room and a
// supplied game history. The caller initializes Core::System's memory and owns `game`.
// `host_requested` supplies the queue fresh-start arm set by HostInPlay before WELCOME.
struct HostRecoveryResult
{
  bool fresh_start = false;
  bool seated = false;
  int current_frame = -1;
  std::optional<int> keyframe_frame;
  std::vector<int> seats;
  std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
  std::array<int, Orca::Net::MAX_SEATS> groups{};
  std::string error;
};
HostRecoveryResult PumpRecoveryHostForTest(Core::System& system, Orca::Net::YouGameRoom& room,
                                          Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                          int current_frame, bool session_alive,
                                          std::unique_ptr<Orca::Net::KeyframeStore> store);
HostRecoveryResult PumpFriendsHostForTest(Core::System& system, Orca::Net::YouGameRoom& room,
                                         Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                         int current_frame, const std::vector<int>& seated,
                                         bool on_menus,
                                         std::unique_ptr<Orca::Net::KeyframeStore> store,
                                         bool host_requested = false,
                                         const std::function<void(const std::function<void()>&)>&
                                             after_admit = {});

// Runs room events while friends admission is held off the menus.
bool PumpHeldFriendsEventsForTest(Orca::Net::YouGameRoom& room,
    std::unique_ptr<Orca::Net::Session>& session, const std::vector<int>& seated);

// Runs Boundary's session loop with a supplied session and short silence window.
int PumpFriendsHostBoundaryForTest(std::unique_ptr<Orca::Net::Session>& session, int completed,
                                   const std::vector<int>& seated,
                                   const std::function<void()>& poll,
                                   const std::function<std::vector<int>()>& left_in_play,
                                   bool joining = false);

// ---- The origin and fresh starts ----
// Every Orca boots the same way up to its origin: port 1 plugged in with nothing pressed, ports 2-4
// unplugged, whatever the player's seat or controller, and nobody's name or controls. The origin is
// the first boundary at which the main menu has been built for ORIGIN_SETTLE_FRAMES frames
// (UX/OnlineMenu.h FreshMove), captured before that boundary's frame hook: this machine's own
// snapshot, NAND and RAM checksum. A replay starts there, so a join never re-runs the boot, and
// two machines whose RAM differs there never meet (room_mismatch).
constexpr int ORIGIN_SETTLE_FRAMES = 30;
// A game whose main menu isn't built by then has no origin: it can't take or join players.
constexpr int ORIGIN_DEADLINE = 6000;
constexpr int ORIGIN_WORDS_WINDOW = 6000;
// The pads of every frame before the origin.
Orca::Net::Pads CanonicalBootPads();
enum class OriginStep
{
  Wait,
  Capture,
  Fail,
};
// At the boundary of `frame`, before its hook: `menu_built_streak` frames in a row (up to the last)
// showed the built main menu. The origin must come after the profile's JIT clear.
OriginStep DecideOrigin(int frame, int menu_built_streak, std::optional<u32> jit_clear_frame);
enum class OriginRule
{
  Menu,
  Declared,
  Frame0,
  Failed,
};
OriginRule SelectOriginRule(const Orca::Mod::Manifest* manifest, bool menu_supported,
                          bool guards_failed);
OriginStep DecideDeclaredOrigin(int frame, int streak, const Orca::Mod::OriginDecl& decl);
// Legacy menu failures keep their existing log only; packs also explain the failure to the app.
std::string_view OriginFailureReport(const Orca::Mod::Manifest* manifest);
// A ranked room needs game results; friends and casual rooms accept engine-only modules.
std::string_view RoomRefusal(std::string_view queue, bool results);
int DeclaredOriginStreak(int frame, int streak, const Orca::Mod::OriginDecl& decl,
                         const Orca::UX::GuestMemory& memory);

// A fresh start: the host restores its own origin, leaves the main menu for the match's character
// select (With Friends, Casual or Ranked) with a recorded exit, and runs unseen and unthrottled
// until that select is up (the tail). Its history starts again at the origin, so the joiner
// replays only the tail and the frames since. Never while a session runs.
//
// A queue room's host always fresh-starts, once per room, once the room's welcome has come. A
// friend arriving at a host with no session fresh-starts it when the host's history since its last
// start is over FRESH_FRIENDS_AFTER frames; a shorter one joins in place. A host in a single-player
// mode holds the friend first, as before.
constexpr int FRESH_FRIENDS_AFTER = 10 * 60;
// Reset games have no menu shortcut: keep short joins in place and bound longer replays to
// three minutes of emulated play. Seconds work at any frame rate and exclude host pauses.
inline constexpr std::chrono::seconds FRESH_RESET_FRIENDS_AFTER{3 * 60};
// Give a solo host time to read the warning before a friend's join resets unsaved progress.
inline constexpr std::chrono::seconds FRESH_RESET_JOIN_WARNING{3};
// The tail ends once the character select has been up this many frames, or after FRESH_TAIL_LIMIT.
constexpr int FRESH_CSS_SETTLE = 30;
constexpr int FRESH_TAIL_LIMIT = 600;
enum class FreshKind
{
  Menu,
  Reset,
};
FreshKind FreshKindForOrigin(OriginRule rule);
bool FreshExitAllowed(FreshKind kind, u8 exit);
bool FreshTailSettled(FreshKind kind, int css_streak, int length);
// The new history's metadata and typed events. Reset histories have neither an exit nor a seed.
Orca::Net::ReplayArchive FreshReplayStart(FreshKind kind, int frame, u64 origin_hash, u8 exit,
                                        u32 seed);
// GameCube has no NAND component. Wii still requires the temporary session root and its IOS FS.
bool LocalNandReady(bool is_wii, bool has_host_nand, bool temporary);
enum class FreshStep
{
  None,  // nothing to do: no fresh start (a join, if any, goes on in place)
  Warn,  // show the reset warning now; hold the join before scheduling its fresh start
  Now,   // fresh-start at the next boundary
  Hold,  // a fresh start is due later: make no keyframe yet
};
struct FreshInputs
{
  FreshKind kind = FreshKind::Menu;
  // Fresh starts are on (ORCA_TEST_FRESH=off turns them off) and this game's origin is after boot.
  bool enabled = true;
  bool origin_ready = false;
  bool joining = false;
  bool session = false;
  // A friend seated or plugging in.
  bool seated_or_plugging = false;
  // A fresh start's tail is running, or a keyframe is being made.
  bool busy = false;
  // `host <code>` matched this game: its room fresh-starts once the welcome names the queue.
  bool queue_armed = false;
  bool in_queue_room = false;
  // A friend waits for a keyframe.
  bool waiting = false;
  // The friends waiting were already taken in place (None for them earlier): their keyframe may be
  // on its way, so the decision stands until nobody waits.
  bool in_place = false;
  // The host is in a single-player mode (UX/OnlineMenu.h DropInHeld).
  bool single_player = false;
  // Frames since the host's last start (its origin or its last fresh start).
  int history_frames = 0;
  // Emulated time since the origin, restored with the timeline; used by reset games.
  std::chrono::duration<double> history_time{};
  // Negative: the kind's default; nonnegative: an explicit test threshold in frames.
  int fresh_after = -1;
};
FreshStep DecideFreshStart(const FreshInputs& in);
// Host wall time only: warn once, then hold a reset join for three seconds. Other starts and a
// cancelled or held join clear the warning, so a later join gets its own notice.
FreshStep WarnBeforeResetJoin(FreshStep step, FreshKind kind, bool queue_armed,
                             std::optional<std::chrono::steady_clock::time_point>* warned_at,
                             std::chrono::steady_clock::time_point now);
// The main menu exit a friends fresh start takes: the Casual or Ranked select the host is on
// (UX/OnlineMenu.h CssPick), so Play again re-arms there, else With Friends.
u8 FriendsFreshExit(std::optional<bool> css_pick_ranked);

// A queue room's fresh start also runs its hold for the room's header and port 1's own pick being
// put back unseen and unthrottled, as its tail does: the host's picture comes back with its pick
// already in, about a second sooner, and the keyframe follows at once. A friend's fresh start has
// neither. These frames are first runs (the hook writes and records as always), only not shown or
// timed. At most FRESH_UNSEEN_LIMIT frames past the tail; the rest of a hold or steer that takes
// longer shows as before.
constexpr int FRESH_UNSEEN_LIMIT = 120;
// Whether the next frame past the tail still runs unseen: `frames` have run unseen so far.
bool FreshUnseen(bool queue_room, int frames);
// While a port catches up, the frame hook may write the room's header only during that unseen
// hold (never in a tail, which re-runs, nor in a joiner's rebuild).
bool HeaderFreeWhileCatchingUp(bool unseen_hold, bool in_tail, bool joining);

// True when `frame` belongs to this player alone: playing solo, no drop-in pending, and no fresh
// keyframe a joining friend would replay from. Only then may the UI announce things.
bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe);

// Whether the frame hook may write the online rules' header at `frame`: no other game runs this
// frame from an earlier state. Looser than AloneAt: a friend arriving or a keyframe being wanted
// doesn't block it, since that keyframe is made after the hook and so carries the header.
bool HeaderFreeAt(int frame, bool solo_quiet, std::optional<int> stored_keyframe);

// The host's wait for its room's header before it makes or offers a keyframe: the keyframe carries
// the header in game memory, and the joiner replays it.
enum class HeaderWait
{
  Ready,  // make or offer the keyframe
  Wait,   // not yet; the header is written at a later free boundary
  Fail,   // queue room only: the match can't start and the room is left
};
// A friends room whose game never gets its header (say, a session was already running when it
// changed) makes the keyframe anyway after this many boundaries (2 s) with a friend waiting.
constexpr int HEADER_WAIT_BOUNDARIES = 120;
// A queue room never makes a keyframe without the room's header: after this many boundaries (5 s)
// with the opponent waiting, the join fails.
constexpr int HEADER_FAIL_BOUNDARIES = 300;
// One boundary of the wait. `in_place`: the header is in game memory (HeaderInPlace);
// `someone_waiting`: a friend waits for a keyframe; `queue_room`: this game hosts a matchmade room.
// `*waited` counts the boundaries waited so far.
HeaderWait StepHeaderWait(int* waited, bool in_place, bool someone_waiting, bool queue_room);
// StepHeaderWait at a host's boundary: a fresh start's tail never counts, and neither does its
// unseen hold (FreshUnseen) until the header is in, so the limit is never reached sooner in real
// time because those frames run unthrottled.
HeaderWait HeaderWaitAt(int* waited, bool in_tail, bool unseen, bool in_place, bool someone_waiting,
                        bool queue_room);

// Coming home: a player who joined a friend's game and then went solo (the host left, it left, or
// its session ended) still plays on the port it was given, with no room. Nobody can drop in, and
// the queue's own character select wants port 1, so Start would do nothing. At its first alone
// boundary on the menus it moves to port 1 and opens a room of its own. See ORCA.md, "Drop-in".
struct HomeInputs
{
  // The port it plays (0: port 1, nothing to do).
  int local_seat = 0;
  // Joining a friend's game, or in a session: not alone.
  bool joining = false;
  bool session = false;
  // The room it was in is still up (a ranked room waiting for the verdict, or kept after the set):
  // that room's ending takes it home (Linger, SetOverLeave).
  bool room_up = false;
  // SoloIdle, and no drop-in pending for the next boundary (Orca::Online::DropInPending: the app's
  // join wins).
  bool solo_idle = false;
  bool drop_in_pending = false;
  // On the menus at this boundary (UX/OnlineMenu.h OnTheMenus): the main menu or a character
  // select only.
  bool on_menus = false;
  // A queue room left this player's own game to restore (MaybeRestoreQueueImage), which moves it
  // to port 1 by itself.
  bool queue_image = false;
  // Waiting in a ranked room for its verdict (Linger).
  bool lingering = false;
};
bool MayComeHome(const HomeInputs& in);

// Time this game spent held on the other players' inputs, across steps. One stall ends at the
// silence limit, but a player who lets one input through just before each limit would hold the
// game forever; past STALL_BUDGET in the last STALL_WINDOW they count as silent all the same.
class StallBudget
{
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds STALL_WINDOW{60};
  static constexpr std::chrono::seconds STALL_BUDGET{15};
  // A stall from `start` to `end`.
  void Add(Clock::time_point start, Clock::time_point end);
  // Stalled time in the window ending `now`, with a stall still going on since `ongoing_since`.
  Clock::duration Stalled(Clock::time_point now,
                          std::optional<Clock::time_point> ongoing_since = std::nullopt) const;
  bool Spent(Clock::time_point now,
             std::optional<Clock::time_point> ongoing_since = std::nullopt,
             bool ongoing_held = false) const
  {
    // A room hold excuses this outage only; completed stalls still consume the budget.
    return Stalled(now, ongoing_held ? std::nullopt : ongoing_since) >= STALL_BUDGET;
  }
  void Clear()
  {
    m_stalls.clear();
    m_recoveries.clear();
    m_pending_stalls.clear();
    m_recovery_seen = false;
  }
  // A completed stall room recovery covered (a friend's connection blip the room recovered) is not
  // the other player starving us: it doesn't count, up to MAX_RECOVERIES in RECOVERY_WINDOW. Past
  // that a player flapping its connection would hold the game forever, so it counts like any
  // other. Returns whether this one was excused.
  static constexpr std::chrono::minutes RECOVERY_WINDOW{10};
  static constexpr int MAX_RECOVERIES = 3;
  // Only a stall at least this long is a recovered outage worth an excuse.
  static constexpr std::chrono::seconds MIN_EXCUSED_STALL{2};
  bool ExcuseRecovered(Clock::time_point now);
  // Used by StepSession, including rollback returns that split one input stall across steps.
  void ObserveRecovery(bool recovering) { m_recovery_seen |= recovering; }
  bool Poll(Clock::time_point now, Clock::time_point step_start, bool recovering);
  void FinishStep(std::optional<Clock::time_point> stall_start, Clock::time_point end,
                  bool still_stalled, bool resumed);
  int Recoveries() const { return static_cast<int>(m_recoveries.size()); }

private:
  std::vector<std::pair<Clock::time_point, Clock::time_point>> m_stalls;
  std::vector<Clock::time_point> m_recoveries;
  std::vector<std::pair<Clock::time_point, Clock::time_point>> m_pending_stalls;
  bool m_recovery_seen = false;
};

// A joiner's own port values: its name in the room (empty: unknown), and its controls and queue
// identity as its hello sent them (Online::HelloControls, HelloQueue).
struct OwnValues
{
  std::string name;
  std::vector<u8> controls;
  std::vector<u8> queue;
};
// Whether the host's port values (KeyframeInfo::names) keep this joiner's own `seat` as it sent it.
// The host decides every port's values so every machine writes the same bytes, but a player's own
// controls and pick are never the host's to choose: the entry in force at its plug-in frame (the
// newest one while that isn't known, `plug_from` NEVER) and every later one must hold them. No
// entry at all is fine only for a player who sent none.
bool OwnValuesKept(const std::vector<Orca::Net::KeyframeInfo::Name>& names, int seat, int plug_from,
                   const OwnValues& own);

// A queue room's match with no verdict yet, when this game's session with the opponent ends or its
// wait for the verdict (Linger) runs out.
struct VerdictWaitInputs
{
  // A casual or ranked room (not a friends room), its match still being decided (MatchLive), the
  // ranked set not over, and the room still up.
  bool queue_room = false;
  bool match_live = false;
  bool set_over = false;
  bool room_ended = false;
  // The session ended for the opponent's doing (Session::ErrorFromPeer), or the wait for the
  // verdict ran out with the room still open.
  bool from_peer = false;
  bool waited_out = false;
};
// Whether this game reports the room's match void before anything else (Online::ReportPeerFault),
// so that its leaving, or its silence while it waits, can't settle the match for the opponent.
bool VoidFirst(const VerdictWaitInputs& in);

// What the session does, at the same boundary, with a With Anyone pick (Casual or Ranked) the frame
// hook saw while the game wasn't alone (UX/OnlineMenu.h TakeLobbyPick). Casual and Ranked leave the
// friends lobby for a 2-player room the matchmaker fills, but only when the page can search that
// queue now (the app's "pick-casual" / "pick-ranked" caps); otherwise the friends would be dropped
// for nothing. In a shared menu either player's press makes the pick, and every game sees it at
// the same frame.
enum class LobbyPickStep
{
  // Nothing. A joiner's game follows its host (it comes home on that pick's character select and
  // announces it there: AnnounceHomePick); a queue room's menus are locked; and without the app's
  // "host" cap nothing would search, so the friends keep their game.
  Ignore,
  // Only announce the pick. Either nobody is left to leave (the app's leave or join went first, or
  // the session only idles out its last frames), or a host with friends whose page can't search
  // the picked queue keeps them and announces the pick unarmed (`orca menu online <queue> kept`),
  // so its page says why and cancels. Once the friends are gone the pick is armed (DecideKeptPick).
  Announce,
  // A host with friends in its game or on their way, whose page can search the picked queue: it
  // leaves the room (the friends play on: `orca state host-left`), prints `orca state left lobby`,
  // and announces the pick.
  Leave,
};
struct LobbyPickInputs
{
  // Joining; the port it plays (a former joiner hosts nobody); in a matchmade room; the app's
  // "host" cap; the app's cap for the picked queue (UX/MenuEvents.h PickSearchable).
  bool joining = false;
  int local_seat = 0;
  bool queue_room = false;
  bool host_cap = false;
  bool pick_cap = false;
  // A session, and whether it only idles out its last frames with nobody in it or on the way
  // (Session::Idle, IDLE_GRACE_FRAMES).
  bool session = false;
  bool session_idle = false;
  // A friend waiting for its keyframe, plugging in or seated.
  bool drop_in_friends = false;
  // A friend's arrival the host hasn't taken yet (Online::ArrivalPending). Taken in later, that
  // friend would get a game under the queue's header and refuse it (`orca error mismatch`).
  bool arrival_pending = false;
  // A keyframe stored, being made, or wanted (a prepare-join) for an invite nobody has taken yet.
  bool keyframe_kept = false;
};
struct LobbyPickPlan
{
  LobbyPickStep step = LobbyPickStep::Ignore;
  // Drop the invite's keyframe (made before the pick); a friend who comes later ends the queue
  // first (FriendEndsQueue). Never while a session runs or friends are kept.
  bool drop_keyframe = false;
  // The announcement also starts the pick (UX/MenuEvents.h AnnounceOnlinePick: the queue's own
  // character select, or the search). False for a host that keeps its friends: the queue's header
  // would reach a friend on the way, who would refuse it, and the page cancels anyway.
  bool arm = true;
};
LobbyPickPlan DecideLobbyPick(const LobbyPickInputs& in);

// Whether a former joiner coming home (ComeHome) on a character select that a With Anyone pick
// opened (UX/OnlineMenu.h CssPick) announces that pick there, so its Start readies the queue as its
// host's did. Only with the app's "host" cap, and never over a queue or search under way. The pick
// caps don't apply: it is alone, and its page answers as for any alone pick.
bool AnnounceHomePick(bool css_pick, bool host_cap, bool queue_or_search);

// A pick a host announced unarmed to keep its friends (LobbyPickPlan::arm false), checked at each
// later boundary (ArmKeptPick). It is armed once the game is alone on that pick's character select,
// so Start there readies it, and dropped once the game leaves that select (UX/OnlineMenu.h
// PickStands) or a queue or search is under way anyway.
enum class KeptPickStep
{
  Wait,
  Arm,
  Drop,
};
struct KeptPickInputs
{
  // The pick still stands where the frame hook last read the game (PickStands).
  bool stands = false;
  // This game plays alone on port 1 (AloneAt, not joining).
  bool alone = false;
  bool queue_or_search = false;
};
KeptPickStep DecideKeptPick(const KeptPickInputs& in);

// A friend on the way while the host is on the queue (its own character select, or searching).
// The friend wins: the queue ends, so the header goes back to none at the next boundary and the
// friend's keyframe has no queue header (a friends room's joiner refuses one).
struct FriendQueueInputs
{
  // A friend arrived, or the app's prepare-join came, at this boundary.
  bool friend_coming = false;
  bool queue_or_search = false;
  // The port it plays (a host plays port 1), and whether it is joining a friend's game.
  int local_seat = 0;
  bool joining = false;
  // The room's welcome came and said "private" (never a matched room, even before its welcome).
  bool friends_room = false;
};
bool FriendEndsQueue(const FriendQueueInputs& in);

// A joiner whose keyframe just loaded into a friends room (not `queue_room`): its own queue ends
// and the game kept for after a queue room (`queue_image`) is dropped, since only a queue room's
// end puts it back and, kept, it blocks going home (MayComeHome).
bool JoinEndsQueue(bool queue_room, bool queue_or_search, bool queue_image);

// A host whose queue ended for a friend (FriendEndsQueue), at each later boundary: once a friend
// came in and all of them left, and the game is alone again on that pick's character select, the
// pick is armed again (`orca menu online <queue>`) so Start searches. Never for an invite nobody
// took yet. Dropped once a queue or search is under way, without the cap, or when joining
// elsewhere.
enum class FriendsPickStep
{
  Wait,
  Arm,
  Drop,
};
struct FriendsPickInputs
{
  // A friend came in since the queue ended.
  bool played = false;
  // AloneAt, on port 1, not joining, nobody seated.
  bool alone = false;
  // The last character select read was opened for that pick (UX/OnlineMenu.h CssPick).
  bool on_pick_select = false;
  // The app's "host" cap and the pick's cap.
  bool host_cap = false;
  bool queue_or_search = false;
  // Joining a friend's game, or on a port other than 1.
  bool elsewhere = false;
};
FriendsPickStep DecideFriendsPick(const FriendsPickInputs& in);

// True while this process plays online (solo in its room, or with friends).
bool Active();
// CPU thread: the lead's game port, including solo play after leaving a room.
int LocalSeat();
int LocalOrdinal(int seat);
int LocalSeatCount();
std::vector<Orca::Net::Pads> PumpSoloSeatsForTest(Core::System& system,
    const std::vector<int>& co_seats, int frames, bool fresh_tail,
    const std::function<Orca::Net::Pad(int)>& sample,
    std::vector<Orca::Events::PortInfo>* ports);
Orca::Net::Pads PumpSoloRoomForTest(Core::System& system, Orca::Net::YouGameRoom& room);
std::optional<Orca::Net::Pad> PumpSeenLocalPadForTest(int lead,
    const std::vector<int>& co_seats, const std::function<Orca::Net::Pad(int)>& sample);
int PumpLeadSeatForTest(std::unique_ptr<Orca::Net::Session> session, int solo_seat);
// Exercise the match-over handoff, including the old seat's delayed samples.
std::vector<Orca::Net::Pads> PumpJoinerSoloForTest(
    std::unique_ptr<Orca::Net::Session> session, int frames,
    const std::function<Orca::Net::Pad(int)>& sample);
// Logs the session's stats.
void LogStats(const char* when);
// Logs the stats and the reason, ends the session and leaves the room. Safe to call more than once;
// also runs when emulation stops or the session ends.
void End(const char* reason);

// Whether the player's pause may go ahead. Allowed in a session without a room (tests), or online
// while this game plays alone with no drop-in under way or pending, so the pause stops only this
// player's game. Any thread.
bool SoloPauseAllowed();
// Core::SetState(Paused) in a session: whether the pause may go ahead. From a yes until EndPause,
// boundaries take no friend in, so a boundary that runs while the CPU thread winds down never
// starts a session it can't step.
bool BeginPause();
// Core::SetState(Running): the pause, if any, is over.
void EndPause();
// True between a successful BeginPause and EndPause: the core is paused by the player, not by boot
// stepping, an HLE reload or a panic.
bool Pausing();
// During the player's pause, on the CPU thread (Core::RunOnCPUThread): what a boundary does for
// this game's room, so a room lost while paused is reported and opened again.
void WatchRoomWhilePaused();
// Host time spent paused since the last call. The stats line leaves it out, so its fps counts
// running time only.
std::chrono::nanoseconds TakePausedTime();
}  // namespace Rollback::OnlineMatch
