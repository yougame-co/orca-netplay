// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

// Machine-readable status lines for the YouGame desktop app, printed on stdout in a session:
//   orca state booting|lobby|playing|ended, plus drop-in states: joining <percent>, friend-joining,
//     friend-joined, friend-left <why>, host-left, left, left lobby, no-room, friend-holding,
//     friend-waiting ("left lobby": the host left its friends for a With Anyone queue pick)
//   orca error <code> <sentence for the player>
//   orca menu <event>       a pick in the game's own Online menu
//   orca result <json>      a matchmade room's verdict (with the app's "results")
//   orca queue ready <casual|ranked> <char> <costume> | unready | skip | timeout me|them
//   orca shaders <compiled> <total>   while the boot compiles shaders
//   orca active <interval_s> <active_s> <inputs> <mode> <pads>   how much the player played, every
//     60 s and when the game stops; embedded, never before "ready" (Orca/Activity.h)
//   orca perf <json>        this machine's speed, versioned ("v":1): "k":"info" once after "ready",
//     "k":"win" every 15 s of running time, "k":"auto" when the run switched its graphics to
//     Performance; embedded only, each line at most 300 bytes (Orca/Perf.h)
// Error codes: boot, cancelled, disc_missing, disc_unreadable, disc_revision, profile, settings,
// room_mismatch, room_full, kicked, network, peer_left, desync, signed_out (the keyframe store
// refused a signed-out player's game; the host says "friend-left signed_out"), internal. Only the
// first error is printed, since it explains the exit. Outside a session nothing is printed.
namespace Orca::Status
{
void State(std::string_view state);
// Prints a state even when it repeats the current one ("left" answers every leave command).
void Event(std::string_view state);
void Error(std::string_view code, std::string_view sentence);
// "orca menu <event>": "online casual", "online ranked" (with "local" appended when the app lacks
// "host", or "kept" when a host keeps its friends because the app can't search that queue:
// PICK_CAPS), "online friends", or "cancel". An event, not a state: each pick prints once (a kept
// pick prints again, armed, once the friends are gone).
void Menu(std::string_view event);
// "orca result <json>" (with the "results" capability): a matchmade room's verdict on a game or a
// ranked set. The whole line must fit in 300 bytes, the app's limit.
void Result(std::string_view json);
// Prints an embed-protocol line as is (e.g. "unsupported host").
void Line(std::string_view line);
// An error the session survives. With the app's "caps join", it is printed and Orca plays on solo
// with exit code 0; otherwise it is the same as Error().
void Report(std::string_view code, std::string_view sentence);

// Capabilities: this build's list is printed once as "orca caps <list>" before "ready", and the app
// sends its own as "caps <list>" on stdin. A later "caps" line replaces the whole answer, and each
// feature is read where it acts, so the app may answer again mid-run. A feature is on only when
// both list it. See ORCA.md, "Embedding".
//   pause    Orca pauses a session while the player's game is alone in it
//   delay    app may send "delay auto|1..6"
//   perf     app may send "perf off|fps|detailed" (the frame meter)
//   direct   app may send "direct on|off"
//   host     app may send "host <code>" and "queue-cancel"
//   chat     app may send "chat <name> <text>", shown as a toast (never logged)
//   yougame  app may send "orb ..." and "notice ..." for the YouGame button over the game; a press
//            on it prints "orca orb" (Windows)
//   music    app may send "music on|off", the player's Music switch (Orca/Music.h)
//   results  "orca result" lines, only for a profile with a verified result reader
//   gfx      app may send "gfx <key> <value>": a live change to one of the player's drawing-only
//            graphics settings (Orca/Perf.h ParseGfx), applied at the next frame boundary
inline constexpr const char* CAPS =
    "join leave stats pause delay perf direct host chat yougame gfx";
// "music": offered where Orca can tell the game's music apart (Orca/Music.h Supported).
inline constexpr const char* MUSIC_CAP = "music";
inline constexpr const char* RESULTS_CAP = "results";
// "locks": a queue room's game enforces the online rules (UX/OnlineRules.h). Offered only for a
// profile with a ruleset; informational, so Orca doesn't wait for the app's answer.
inline constexpr const char* LOCKS_CAP = "locks";
// "queue2": the queue's own character select (UX/Queue.h), with `orca queue ...` lines, the app's
// `queue rating <n|->` and a ready timer. Offered with "locks"; when the app accepts it, Casual and
// Ranked wait for `orca queue ready` before searching.
inline constexpr const char* QUEUE2_CAP = "queue2";
// "pick-casual", "pick-ranked": the app can search that queue right now, and says so again on a
// later "caps" line when that changes. A host with friends in its game leaves them for a With
// Anyone pick only with that pick's cap; without it the pick prints in place, unarmed ("orca menu
// online <queue> kept"), and arms once the friends are gone (Rollback/OnlineMatch.h
// DecideLobbyPick, DecideKeptPick). Offered with "locks". A ruleset profile then offers 17 caps;
// the app reads up to 24 (desktop 0.12.39 and later), and Orca needs a newer app than that anyway.
inline constexpr const char* PICK_CAPS = "pick-casual pick-ranked";
// This build's caps for the running profile: CAPS, plus "music", "results", "locks", "queue2" and
// the pick caps where supported.
std::string OfferedCaps();
void PrintCaps();
void SetAppCaps(std::string_view list);
bool Cap(std::string_view name);
// "orca stats <json>" (with the "stats" capability).
void Stats(std::string_view json);
// "orca shaders <compiled> <total>", about once a second while the boot compiles shaders before the
// first frame, so the app can tell a slow boot from a hung one; once more when it ends. Only before
// "ready", and only if the wait lasts a second or more.
void Shaders(std::size_t compiled, std::size_t total);
// 0 after a normal leave; 1 once an error was reported.
int ExitCode();
// The game reached its first frame. Called every frame boundary; cheap after the first.
void GameStarted();
// Called once, on the CPU thread, at the first GameStarted. Set it before the boot.
void SetFirstFrameListener(void (*listener)());
// Where lines go (default stdout). In embed mode stdout is redirected to stderr, so stray log lines
// can't corrupt the protocol stream.
void SetOutput(std::FILE* out);
// Also notified of the printed error (embed mode repeats it in the app's "error" form).
void SetErrorListener(void (*listener)(std::string_view code, std::string_view sentence));
// The app asked Orca to stop ("quit", or its stdin closed). A game stopped before its first frame
// was not a boot that failed: Finish() then reports no boot error.
void Stopping();
// At exit: reports a boot error if the game never reached a frame, no reason was given and the app
// didn't stop it, then "ended". Returns ExitCode().
int Finish();
}  // namespace Orca::Status
