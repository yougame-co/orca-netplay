// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// The matchmaking queue's app side (ORCA.md, "The queue's character select"): what the app said
// (`queue rating`, `queue-cancel`, Casual or Ranked picked), this player's queue identity as the
// session carries it, what the last first run published for the overlay and the session, and the
// casual ready timeout until its frame is confirmed. Engine state, moved out of the Brawl queue
// module (Games/Brawl/Queue.h, which reads and publishes it) in S2. Nothing here writes emulated
// memory or decides a write.
namespace Orca::UX::QueueApp
{
// The character select's id for "no character" (Brawl's muSelCharPlayerArea +0x1B8,
// Selch_SelectNone): a queue identity with it carries no pick.
constexpr int NO_CHARACTER = 0x28;

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
// This player's own pick, as its queue identity carries it (empty when it never readied one).
Identity OwnIdentity();
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

// ---- What the queue module's first runs publish (Games/Brawl/Queue.cpp Frame) ----
void SetSoloReady(bool ready);
void SetOnSoloCss(bool on);
void SetOnQueueCss(bool on);
// The pick readied on the queue's own select, with its place when known: kept and published as
// this player's queue identity (Events::SetOwnQueue).
void SetPick(const Identity& pick);
// Whether the one-time hold-B tip is on screen (Queue::Tip). End() takes it down.
void SetTipShown(bool shown);
bool TipShown();
// A clear asked for by AfterRestore or ClearReady, taken once (the queue clears ready at a solo
// first-run frame), and AfterRestore's reason for the log.
bool TakeClearReady();
std::string ClearReadyWhy();
// Every run (first runs and re-runs): the casual timeout at `frame` (bit 7: timed out; bits 0-1:
// who wasn't ready), kept until ConfirmTimeout reaches that frame. A replay played from the network
// records it as already final.
void NoteTimeout(int frame, u8 value);
}  // namespace Orca::UX::QueueApp
