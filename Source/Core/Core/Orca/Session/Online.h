// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/YouGameRoom.h"

// Online play over YouGame: the room this process plays in, used by the rollback core at each frame
// boundary. One room per process; its I/O runs on its own thread.
namespace Orca::Online
{
// Online play is on: ORCA_SESSION=1 plus the desktop app's bridge (YOUGAME_STDIO=1 with Orca's
// stdout carrying it, or YOUGAME_BRIDGE / YOUGAME_TOKEN), or ORCA_TEST_DEV_GAME for tests.
bool Enabled();

// Starts the room at the first frame boundary. It runs on its own thread, so the host keeps playing
// while friends come and go.
void Start();
// Switches to a new room after Shutdown: a friend's (`code`, joining) or this player's own (empty
// code). Never asks the app for an invite. A joiner sends the host its own controls
// (Orca::Events::OwnControls).
void StartRoom(const std::string& code, bool joining, bool friends_room = true,
               bool join_if_taken = false);
// The room's code once known; "" before that or with no room.
std::string Code();
// The roster's name for a seat; "" when unknown or with no room.
std::string SeatName(int seat);
// A joiner's own controls and queue identity as its hello sent them (YouGameRoom::HelloControls).
std::vector<u8> HelloControls();
std::vector<u8> HelloQueue();

// The app's "join <code>" and "leave" (embed mode). Any thread, never blocks; taken at the next
// frame boundary.
void RequestJoin(const std::string& code);
std::optional<std::string> TakeJoinRequest();
void RequestLeave();
bool TakeLeaveRequest();
// The app's "host <code>": host the room the queue matched this game into. Any thread, never
// blocks; taken at a frame boundary.
void RequestHost(const std::string& code);
std::optional<std::string> TakeHostRequest();

// ---- Matchmade rooms ----
// The room's queue: "private" (a friends room, or no room), "casual" or "ranked".
std::string RoomQueue();
bool RoomDoubles();
// A room match is running and its result isn't in yet.
bool MatchLive();
// A ranked set's room after its verdict, still open: the players stay together on the results
// screen until each leaves.
bool SetOver();
// Host: the opponent is playing in this game (past catch-up), or no longer is.
void SetOpponentPlugged(bool plugged);
// Reports a game's result for the room's match.
void ReportGame(const Net::GameReport& report);
// A ranked set's game began (its fight's first frame, the id its report will carry): tells a ranked
// queue room's match, so leaving from then on loses the set.
void ReportGameStart(int start);
// The games of the room's ranked match this player knows of (YouGameRoom::KnownSetGames).
int KnownSetGames();
// The games desynced: the room's match in progress is void.
void ReportDesync();
// The session ended for the opponent's doing (Session::ErrorFromPeer), or a ranked set's wait for
// its verdict ran out: the room's match in progress is void (YouGameRoom::ReportFault).
void ReportPeerFault();
// Waiting in a ranked set's room for the verdict, with no session: this game still runs
// (YouGameRoom::KeepAlive). Emulation thread.
void KeepAlive();
// Sends a replay stream's next bytes to the room for its watchers (YouGameRoom::SendReplayStream).
void SendReplayStream(const std::vector<u8>& bytes);
// Queues a saved copy of the match (Rollback/ReplayStream.h RepackedPieces) for the room, if it is
// still the room of `generation` (RoomGeneration()). Any thread, never blocks; nothing without it.
void SendReplayPacked(const std::vector<std::vector<u8>>& pieces, int frames, u64 generation);
// The current room/socket's process-wide generation (0 with no room). Changes on a reopen,
// including one with the same code; room readiness is still checked separately.
u64 RoomGeneration();
// In a ranked set whose opponent stopped sending inputs (15 s stalled, or silent), this player
// claims the set. Does nothing outside a ranked room's live match.
void ReportStall();
// The app's "delay auto|1..6": this player's input delay in frames, or nullopt for adaptive. Any
// thread, never blocks. Persists across sessions, and applies only while the app's caps include
// "delay".
void SetChosenDelay(std::optional<int> frames);
std::optional<int> ChosenDelay();
// The app's "direct on|off": whether this player allows direct links to other players. On by
// default; persists across rooms and applies only while the app's caps include "direct". The room
// applies it at its next tick. Any thread, never blocks.
void SetDirectWanted(bool on);
bool DirectWanted();
// The app's "record on|off|auto" (with its "record" cap): whether this player's Orca streams its
// match to the room, for watchers and the saved replay the site keeps in the players' accounts.
// "auto", the default, records ranked rooms only; a tournament or friends room records after the
// page's "record on". Persists across rooms. Any thread, never blocks.
enum class RecordChoice
{
  Auto,
  On,
  Off,
};
void SetRecordChoice(RecordChoice choice);
// Whether to record in the current room now: the app's "record" cap, a room, and the choice above.
bool RecordWanted();
// Whether this Orca joins someone else's game (ORCA_JOIN=1, or the app opened an invite); nullopt
// until known, or if the room ended first.
std::optional<bool> Joining();
// The room's transport (valid from Start until Shutdown, which must run on the emulator thread
// after the session stops using it), this Orca's seat (-1 until it's in), other players' events,
// and whether the room is gone.
Net::Transport* Transport();
int Seat();
std::vector<int> CoSeats();
int LocalIndex(int seat);
std::vector<int> GroupOf(int seat);
std::vector<Net::PeerEvent> TakePeerEvents();
// Whether the untaken events include the game's host leaving the room. Any thread.
bool HostLeftPending();
// Host: seats of players whose leaving is among the untaken events. Any thread.
std::vector<int> LeftSeatsPending();
std::optional<std::chrono::steady_clock::time_point> HostAwayDeadline();
void RetryPeer(int seat);
bool HoldInputSilence(std::chrono::steady_clock::time_point since,
                       std::chrono::steady_clock::time_point now,
                       std::optional<std::vector<int>> missing = std::nullopt);
bool RoomRecovery();
bool RoomRecovering(std::chrono::steady_clock::time_point now);
bool RoomEnded();
// This Orca is in its room: the welcome arrived and the room hasn't ended.
bool InRoom();
// A fresh ticket for the room and the keyframe store's URL (YouGameRoom::FreshTicket).
bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error);
// Host: unplug a friend and tell them why ("desync", "stalled", "network").
void DropPeer(int seat, const std::string& reason);
// Host: whether to hold joins that are waiting for a keyframe (YouGameRoom::HoldJoins).
void HoldJoins(bool holding);
// Whether a session error means the games desynced (disagreeing inputs or checksums, or a
// correction that arrived too late to apply).
bool IsDesync(const std::string& session_error);
// Whether a session error is a rollback this machine couldn't make ("No snapshot to roll back",
// "exceeds the limit"): an engine fault the player survives, reported as such.
bool IsRollbackFailure(const std::string& session_error);
// Host: send every port's values so far, with their version, to every player in the game.
void BroadcastNames(const std::vector<Net::KeyframeInfo::Name>& names, int version);
// Leaves the room like Shutdown, but on its own thread: the goodbye and socket close can take a
// second or two, which the emulator thread must not wait for mid-game.
void ShutdownInBackground();
// Rooms ShutdownInBackground is still leaving (their page mirror may still post until done).
bool RoomsLeaving();
// Host: offer the keyframe to a joining seat.
void OfferKeyframe(int seat, const Net::KeyframeInfo& info);

// The app's "prepare-join": the host invited a friend who will drop into the running game, so start
// a keyframe at the next frame boundary while the friend accepts. Any thread; never blocks.
void PrepareJoin();
// True once after a PrepareJoin; the emulator thread takes it at a frame boundary.
bool TakePrepareJoin();

// Drop-in work waiting for the next frame boundary: a friend arriving, a PrepareJoin while the room
// is up, or the app's join or leave once its caps allow them. Ends a solo pause
// (Rollback::OnlineMatch::SoloPauseAllowed). Any thread; never blocks for long.
bool DropInPending();
// Host: only the arrival half of DropInPending: a friend has arrived in the room and isn't taken
// yet, while the room is up. Any thread.
bool ArrivalPending();

// Latest round trip to the room server in ms (-1 before the first). `sequence`, if given, changes
// with each new measurement: feed Session::OnRoundTrip only when it changed.
int RoundTripMs(u32* sequence = nullptr);
// The room's recent round trips, oldest first, for a session starting now; `sequence` as above, for
// the newest (unchanged with no room).
std::vector<int> RecentRoundTrips(u32* sequence);
// How other players' inputs reach this one: the relay or direct links (Net::DirectLink).
Net::DirectSummary Direct();
// Why the room ended, as an Orca::Status code ("" while running, or if none was given).
std::string RoomErrorCode();
// The room ended because someone else holds it: this Orca wasn't its host, or the server replaced
// its socket with another run's. RoomErrorCode says "network".
bool RoomTaken();
// What the room is doing or why it ended, for the window title or log.
std::string StatusLine();

// Reports why the session ended to the desktop app (Orca::Status): a desync, the room's reason, or
// the session's own error; nothing for a normal leave. False when the game can't continue solo
// (this machine's emulation failed; "internal" is always terminal). `from_peer`: the error was
// another player's doing (Session::ErrorFromPeer), never this machine's: it plays on.
bool ReportSessionEnd(const std::string& session_error, bool from_peer = false);

// Why this Orca leaves its room, sent in its goodbye: "mismatch" (a queue room's header it wouldn't
// play) or "no-show" (it didn't pick in time). Any thread.
void SetLeaveReason(const std::string& reason);

// The friend stopped answering mid-match: reports peer_left with `sentence`, and tells the friend
// on the way out that the connection stalled, in case its game recovers.
void ReportPeerStalled(std::string_view sentence);

// Tests only ("test-drop-room"): drops the room's connection as a network failure would.
void TestDropRoom();
// Tests only ("test-direct off|on|in|out|rebuild"): direct links drop all, incoming or outgoing
// datagrams, or none again; "rebuild" rebuilds every link.
void TestDirect(const std::string& command);

// Leaves the room (voiding a running match) and stops its thread, then waits up to 2 s for rooms
// ShutdownInBackground is still leaving. Call when emulation stops.
void Shutdown();

// The lobby compatibility key: players with different keys can't share a room. Built from the Orca
// build, game profile and data files, every forced setting both machines must share, the disc and
// the seeded save. Not the host CPU, so Mac and PC players can meet. "orca2:<xxh3>" in both modes;
// in mod mode (Core/Orca/Mod.h) over the engine, the bundle's tree and its SD image instead of the
// profile files.
std::string CompatibilityKey();
// What the key hashes (tests pin its legacy form).
std::string CompatibilityKeyParts();
// The key's last part, the in-game UX's version (S3, WP19 a): "|ux=<engine>" (UX::kCompatVersion),
// then ".<game>" when the running game's module gives that game a revision of its own
// (Engine/Module.h Manifest::compat), so a change to one game's steps splits only its rooms.
std::string UxKeyPart(int engine, int game);
}  // namespace Orca::Online
