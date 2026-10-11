// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

// The matchmaking queue's character select, Slippi-style. See ORCA.md, "The queue's character
// select".
//
// While searching, the player waits on the local Versus character select where only their own
// panel works. Picking and pressing Start makes them ready (`orca queue ready <casual|ranked>
// <char> <costume>`), and B un-readies (`orca queue unready`). Once matched, the joiner's pick is
// steered onto port 2, and a ready timer runs before the stage select. Casual players can hold Z to
// skip (`orca queue skip`). Ranked has no skip.
//
// Every in-game decision is a pure function of emulated memory, the frame number and the session's
// synced values, written to the MatchBlock::QUEUE region only where it differs, so rollback re-runs
// compute the same result. Masked presses reach the logic through the input gate's latch. Stdout
// lines and the overlay are local and come from first runs only.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::Queue
{
// ---- The region (MatchBlock::QUEUE, 0x30 bytes) ----
constexpr u32 READY = MatchBlock::QUEUE + 0x00;        // u8: bit p set when port p is ready
constexpr u32 FLAGS = MatchBlock::QUEUE + 0x01;        // u8: FLAG_*
constexpr u32 TIMEOUT_WHO = MatchBlock::QUEUE + 0x02;  // u8: bit p set when port p timed out
constexpr u32 LOCKED = MatchBlock::QUEUE + 0x03;       // u8: bit p set when port p is locked in
// u32: frame + 1 when the ready timer started (0 = not running).
constexpr u32 TIMER_START = MatchBlock::QUEUE + 0x04;
constexpr u32 RAW = MatchBlock::QUEUE + 0x08;       // u16[2]: ports 1, 2's raw buttons, last frame
constexpr u32 RAW_PREV = MatchBlock::QUEUE + 0x0C;  // u16[2]: the frame before
constexpr u32 TIMER = MatchBlock::QUEUE + 0x10;     // u16: frames into the ready timer (display)
constexpr u32 STEER = MatchBlock::QUEUE + 0x12;     // u16: frames port 2's pick has been steered
// Port 2's pick: u8 1, u8 character, u8 costume, u8 0, f32 x, f32 y (the token's place).
constexpr u32 PICK = MatchBlock::QUEUE + 0x14;
// u32: frame + 1 when steering port 2 began (0 = not steering).
constexpr u32 STEER_START = MatchBlock::QUEUE + 0x20;
// u32: frame + 1 when port 2's token was placed, starting the costume step (0 = not yet).
constexpr u32 COSTUME_START = MatchBlock::QUEUE + 0x24;
// f32[2]: port 2's hand at the start of the last frame. A is only pressed once the hand is still,
// because it keeps sliding briefly after the stick is released.
constexpr u32 HAND_PREV = MatchBlock::QUEUE + 0x28;
constexpr u32 REGION_END = MatchBlock::QUEUE + 0x30;
static_assert(REGION_END <= MatchBlock::QUEUE + MatchBlock::QUEUE_SIZE);

constexpr u8 FLAG_GO = 0x01;         // both ready (or ranked timer expired): Orca presses Start
constexpr u8 FLAG_TIMED_OUT = 0x02;  // casual timer expired: frozen until the players leave
// The B that cancelled the search is still held. Hide it from the game until released, or the
// token would be picked back up.
constexpr u8 FLAG_SWALLOW_B = 0x04;
// After returning from a room not ready, the Start or A that left the results screen may still be
// held. It must not start a search until released.
constexpr u8 FLAG_SWALLOW_START = 0x08;
// A on BACK while ready un-readies at this frame, then the game sees the same A on the next frame
// and backs out, so one press does both. Idempotent if the frame is re-run.
constexpr u8 FLAG_BACK_A = 0x10;

// 30 s to ready up, 1.5 s of Z to skip, at 60 frames per second.
constexpr int READY_FRAMES = 30 * 60;
constexpr int SKIP_FRAMES = 90;
// Give up steering port 2's hand after this long; the player then picks by hand.
constexpr int STEER_LIMIT_FRAMES = 10 * 60;
// Keep port 2's stick centred this long first. The game calibrates a newly plugged pad's stick
// centre from its first reading, so steering too early would leave the stick reading as pushed.
constexpr int STEER_SETTLE_FRAMES = 20;
// Give up turning port 2's costume (X) after this long; the pick stands in any colour.
constexpr int COSTUME_LIMIT_FRAMES = 3 * 60;

// The character select's id for "no character" (muSelCharPlayerArea +0x1B8, Selch_SelectNone).
constexpr int NO_CHARACTER = 0x28;
// The id for Random (Selch_Random, same in Project+). The game resolves it from its own emulated
// RNG, so it is the same on every machine. X does not change Random's colour, so any costume is
// accepted.
constexpr int RANDOM_CHARACTER = 0x29;

// ---- The queue identity (Events::PortInfo::queue), 13 bytes, big-endian ----
//   +0 version 1   +1 u16 rating (0xFFFF none)   +3 u8 character (0xFF none)   +4 u8 costume
//   +5 f32 x   +9 f32 y   (where the player's token was placed in their own game)
struct Identity
{
  int rating = -1;  // -1: none ("queue rating -")
  int character = -1;  // the character select's id (+0x1B8); -1 none
  int costume = 0;     // the colour number (+0x1BC)
  float x = 0;
  float y = 0;
  bool HasPick() const { return character >= 0 && character != NO_CHARACTER; }
  bool operator==(const Identity&) const = default;
};
std::vector<u8> EncodeIdentity(const Identity& identity);
std::optional<Identity> DecodeIdentity(const std::vector<u8>& bytes);

// ---- What a frame sees ----
struct CssPort
{
  bool readable = false;  // the area could be read
  bool human = false;
  int character = NO_CHARACTER;  // under the hand while holding the token, else the token's
  bool placed = false;           // the token is down on a character
  int costume = 0;
  bool name_list = false;  // its name list is open (Rules::ReadCssNameList)
  // The hand (OnlineRules.h CssHand).
  u32 hand_target = 0;
  u32 hand_button = 0;
  float hand_x = 0;
  float hand_y = 0;
  bool operator==(const CssPort&) const = default;
};

struct View
{
  bool queue2 = false;  // the header is the queue's (FLAG_QUEUE2) and locks the rules
  bool solo = false;    // the queue's own character select (FLAG_SOLO)
  bool ranked = false;
  bool css = false;      // on the character select
  bool game1 = true;     // no fight since the header was written
  bool order_done = true;  // ranked later games: the character order is done (CharOrder.h)
  // Ranked later games: both picks are locked in, so the stage select comes at once. And which
  // ports have locked in so far (bit p, for the overlay's badges).
  bool order_locked_in = false;
  u8 order_locked = 0;
  u8 plugged = 0;        // the ports in play (MatchBlock::PLUGGED)
  std::array<CssPort, 2> ports{};
  std::array<u16, 2> raw{};
  std::array<u16, 2> raw_prev{};
  // Port 2's hand at the start of the last frame (HAND_PREV).
  float hand2_prev_x = 0;
  float hand2_prev_y = 0;
  std::optional<Identity> pick2;  // port 2's queued pick, from its plug frame on
  bool operator==(const View&) const = default;
};

struct State
{
  u8 ready = 0;
  u8 flags = 0;
  u8 timeout_who = 0;
  u8 locked = 0;
  u32 timer_start = 0;
  u16 timer = 0;
  u16 steer = 0;
  u32 steer_start = 0;
  u32 costume_start = 0;
  bool pick_set = false;
  u8 pick_character = 0;
  u8 pick_costume = 0;
  float pick_x = 0;
  float pick_y = 0;
  bool operator==(const State&) const = default;
};

// The room's character select with both players in, at the ready step.
bool ReadyPhase(const View& view);
// Ranked later games once both character-order picks are locked in; no ready step.
bool OrderLockedIn(const View& view);

// Whether the ready step has a 30 s timer: every ranked game, and only casual's first game. Later
// casual games wait for both Starts with no clock, to give players time to talk.
bool Timed(const View& view);

// Whether holding Z may ask to skip this opponent: casual only, both players in, before the go or a
// timeout. Ranked matches cannot be skipped; leaving any other way is a forfeit.
bool MaySkip(const View& view, const State& state);

// The state after this frame. Pure and idempotent: feeding the result back in changes nothing.
State Advance(const View& view, const State& state, int frame);

// Why the own select's ready (port 1) dropped from `before` to `after`, for the log.
std::string_view UnreadyReason(const View& view, const State& before, const State& after);

// The gate's masks and presses for the frame about to run. Pure.
Rollback::InputGate::Masks Gate(const View& view, const State& state);

// Z held for SKIP_FRAMES in a row fires one skip; Z must be released before the next. Local only.
struct SkipHold
{
  int frames = 0;
  bool sent = false;
  // Returns whether a skip fires on this frame.
  bool Step(bool may_skip, bool z_held);
  // 0..1 progress toward a skip, for the overlay.
  float Progress() const;
};

// The overlay's queue text: the top line, a hint under it, and each port's name label and LOCKED IN
// badge. `local` is this machine's port. Pure, display only.
struct Text
{
  std::string line;
  std::string note;
  std::array<std::string, 4> labels;
  std::array<bool, 4> locked{};
};
Text TextFor(const View& view, const State& state, const std::vector<Events::PortInfo>& ports,
             int local, float skip_progress);

// ---- Fixed point (the steering's arithmetic) ----
// Steering is integer-only because host floating point differs between compilers (for example
// ARM64 clang fuses a multiply-add that x86-64 MSVC does not), which would desync the machines.
// See ORCA.md, "Determinism across hosts". Positions are in FIXED_ONEs, 1/65536 of a unit.
constexpr int FIXED_BITS = 16;
constexpr s64 FIXED_ONE = s64{1} << FIXED_BITS;
// Positions this far out are invalid (the character select spans about -30..30, -20..20).
constexpr s64 FIXED_LIMIT = s64{4096} << FIXED_BITS;
// A float in FIXED_ONEs, truncated toward zero, decoded from its bits with no float instructions.
// nullopt for NaN, infinity, or a magnitude of FIXED_LIMIT or more.
std::optional<s64> ToFixed(float value);
// floor(sqrt(n)), exactly.
u64 IntSqrt(u64 n);

// Port 2's stick to move its hand from (x, y) to (tx, ty): full tilt when far, gentler when near,
// centred when close enough or on an invalid position. Integer only.
std::pair<u8, u8> SteerToward(float x, float y, float tx, float ty);
// Whether port 2's hand has nearly stopped: under 0.05 a frame on both axes (Brawl's hand creeps
// about 0.03 a frame after the stick is released). Integer only.
bool HandStill(float x, float y, float prev_x, float prev_y);

// ---- The host's own pick after a fresh start (Rollback/OnlineMatch.cpp) ----
// A fresh start takes a queue room's host to a new character select, so its token is no longer on
// the character it readied with on its own select. Its own inputs put it back, from its queue
// identity: the hand goes to where the token was placed, A once the hand is still over that
// character, B if the token landed on another, X until the costume matches (any colour for
// Random). These are this player's inputs like any other, recorded and sent as such, so the
// opponent's game computes nothing for them. Local only.
struct OwnSteer
{
  int frames = 0;  // frames steered so far
  // The frame count when the token first lay on the pick in the wrong colour (-1: not yet).
  int costume_from = -1;
  bool has_prev = false;
  float prev_x = 0;
  float prev_y = 0;
};
struct OwnStep
{
  bool done = false;
  // Port 1's stick and buttons for this frame, while not done.
  u8 stick_x = static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_X);
  u8 stick_y = static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_Y);
  u16 buttons = 0;
  bool operator==(const OwnStep&) const = default;
};
// One frame: port 1 as the character select shows it, and the pick to put back. Advances `steer`.
// Done once the token is down on the pick in its colour, after STEER_LIMIT_FRAMES, after
// COSTUME_LIMIT_FRAMES of turning the colour, or at once with no pick. Pure, integer steering.
OwnStep StepOwnPick(const CssPort& port, const Identity& pick, OwnSteer* steer);
// This player's own pick, as its queue identity carries it (empty when it never readied one).
Identity OwnIdentity();
// Steers port 1 onto `pick` from the next room character select the frame hook sees (game 1),
// giving up after STEER_LIMIT_FRAMES. CPU thread.
void ArmOwnPick(const Identity& pick);
void DisarmOwnPick();
bool OwnPickArmed();
// The pad the last first-run frame hook chose for port 1 while armed; nullopt when it didn't steer
// at that frame (not on the room's character select yet). CPU thread.
std::optional<GCPadStatus> OwnPickPad();

// ---- Where a pick's square is (local, first runs only) ----
// The steering aims a hand at a place inside the pick's square, learned from the hands: while a
// hand holds its token over a character, the first and last places it was seen there; when that
// token goes down on the same character, their midpoint is that character's place. A token that
// was already down when the select came up (the queue image after a ranked set, or a select the
// game kept from an earlier visit) teaches nothing: the hand was never over it. Places are kept per
// character for the whole run, from either panel, since every select has the same grid.
class PickPlaces
{
public:
  struct Placed
  {
    int character = NO_CHARACTER;
    int costume = 0;
    bool learned = false;  // the hand was over this character, so its place is now known
    float last_x = 0, last_y = 0;    // where the hand last held the token over a character
    float first_x = 0, first_y = 0;  // where it first came over that character
  };
  // Port `port` (0 or 1) on a queue character select this frame. Returns the token going down.
  std::optional<Placed> Frame(int port, const CssPort& css);
  // The place learned for `character`, if any.
  std::optional<std::pair<float, float>> Find(int character) const;

private:
  struct Hover
  {
    int character = -1;
    float first_x = 0, first_y = 0, last_x = 0, last_y = 0;
  };
  std::array<Hover, 2> m_hover{};
  std::array<bool, 2> m_was_placed{};
  std::map<int, std::pair<float, float>> m_places;
};

// The identity this player publishes for `pick`, with its place when known. With no known place it
// carries no pick, so nobody steers it and the player picks by hand: steering to an unknown place
// aimed the hand at (0, 0), another character's square, where it never pressed A and held the
// player's controller for STEER_LIMIT_FRAMES. Pure.
Identity Publishable(Identity pick, const std::optional<std::pair<float, float>>& place);

// ---- Memory ----
// Whether a character select's task and its first two panels can be read (scSelctCharacter), the
// condition for View::css, under any header or none.
bool CssTaskReadable(const GuestMemory& memory);
View ReadView(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
State ReadState(const GuestMemory& memory);
// Writes the state fields (not RAW/RAW_PREV, which the latch owns), only where they differ.
int WriteState(GuestMemory& memory, const State& state);
// The input gate's latch: ports 1 and 2's raw buttons into RAW, the old RAW into RAW_PREV, and
// port 2's hand into HAND_PREV, while the header locks the rules. Returns bytes changed.
int Latch(GuestMemory& memory, const std::array<std::optional<GCPadStatus>, 4>& raw);

// ---- The frame hook's part (UX.cpp), after the character order ----
// Reads, advances and writes the state on every run. On first runs it also prints lines, tracks Z
// for a skip and updates the overlay.
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone);
// The input gate's masks, from memory alone.
Rollback::InputGate::Masks GateFrame(const Core::CPUThreadGuard& guard);
// Whether the queue owns the character select's Start (in a queue2 room). The character order's
// own Start is then left to the ready timer.
bool OwnsStart(const Core::CPUThreadGuard& guard);
// The input gate's latch.
void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw);

// ---- The app's side (any thread) ----
// The player picked Casual or Ranked and waits on the queue's own character select.
void Begin(bool ranked);
// Backed out to the menus (`orca menu cancel`).
void End();
bool Active();
bool Ranked();
// `queue rating <n|->`: this player's rating in the current queue (-1 none).
void SetOwnRating(int rating);
int OwnRating();
// The last solo first run saw the queue's own character select with the player ready.
bool SoloReady();
// The last solo first run saw the queue's own character select.
bool OnSoloCss();
// The last first run saw a queue character select (solo or in a room). The overlay then docks its
// lines on Project+'s rules bar.
bool OnQueueCss();
// The session restored the player's own game after a room. With `keep_ready` false (their own
// timer ran out, or a ranked set was played), ready is cleared so the search does not restart by
// itself. `why` is for the log.
void AfterRestore(bool keep_ready, const char* why = "");
// The app's `queue-cancel`: the search ended, so the player is no longer ready
// (`orca queue unready` follows).
void ClearReady();
// The casual ready timer ran out and its frame is confirmed. Prints `orca queue timeout me|them`
// once.
void ConfirmTimeout(int confirmed, int local_port);
// Discard notifications from the state that was replaced by a local snapshot.
void ResetTimeouts();
// Whether the last `orca queue timeout` said `me`. Cleared when read.
bool TakeTimeoutWasMine();
}  // namespace Orca::UX::Queue
