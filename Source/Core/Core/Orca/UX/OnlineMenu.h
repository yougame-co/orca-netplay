// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// The game's own Online menu. Patches (Data/Sys/Orca/<profile>.patches) make Brawl's Wi-Fi button
// and Project+'s Play Online open an offline ONLINE page. Each choice leaves the main menu for the
// local Versus character select with an exit code: 30 Casual, 31 Ranked, 25 With Friends. This
// file reads that pick and prints it on stdout ("orca menu online casual local", ...) for the app.
//
// Picks are read only on first runs, never on rollback re-runs, and announced only while this game
// plays alone. A Casual or Ranked pick made while it isn't alone (friends in it or about to land)
// goes to the session instead (TakeLobbyPick): a host whose app can search that queue leaves its
// friends and announces it, one that can't keeps them and announces it unarmed ("kept"), and a
// joiner lets it go; once its host has left, it comes home on the character select that pick
// opened and announces the pick that select holds (CssPick). Acting on a pick while inputs may
// still be guessed is safe: the session ends at that boundary. After a resync (Events NoteResync)
// the reader starts over silently, so nothing stale is printed.
//
// Scene manager at 0x805A0060: +4 the scene, whose name is at +0 ("muMenuMain", "scMemoryChange"
// between scenes); +0x284 the main menu's exit code, written as the menu leaves.
namespace Orca::UX
{
class GuestMemory;

enum class OnlinePick
{
  Casual,
  Ranked,
  Friends,
};

// What one frame shows of the main menu.
struct MenuState
{
  enum class Where
  {
    Other,
    Menu,     // muMenuMain
    Leaving,  // scMemoryChange: between two scenes
  };
  Where where = Where::Other;
  u32 exit_code = 0;
};

MenuState ReadMenuState(const GuestMemory& memory);

// The pick an exit code stands for, if any.
std::optional<OnlinePick> PickForExitCode(u32 exit_code);

// The words after "orca menu" on stdout: "online casual" or "online ranked" when the app's queue
// searches ("host" capability), with " local" appended when nothing searches; "online friends".
std::string_view MenuEventText(OnlinePick pick, bool queue = false);

// The current scene's name ("" when unreadable).
std::string ReadSceneName(const GuestMemory& memory);

// Matchmaking search state after a Casual or Ranked pick, for the overlay. The search ends when the
// app matches this game (`host <code>` or `join <code>`), the app cancels (`queue-cancel`), or the
// player backs out to the main menu, which prints "orca menu cancel" once. Any thread.
namespace Search
{
enum class State
{
  None,
  Searching,
  Found,
};
void Begin(bool ranked);
// The app matched this game: Found when hosting, None when joining. A joiner that was searching
// shows "Opponent found · joining <host>…" (Joining) until JoinOver, so its own select's search
// lines don't stay up while the host's game comes over.
void Matched(bool hosting);
// The opponent plugged in, the app ended the search, or the room went away. Ends Joining too.
void End();
State Current();
bool Ranked();
// A matched join began while this player was searching or on the queue's own select: the overlay
// says the opponent was found until JoinOver (or End).
void JoinStarted();
// A joiner's "Opponent found" lines are up: from a matched join until the host's game is loaded
// (JoinOver) or the join ends. Display only: Current() stays None meanwhile.
bool Joining();
// The joiner loaded the host's game, or its join ended.
void JoinOver();
// The overlay's two lines (empty: nothing to show).
std::pair<std::string, std::string> Lines();
// Lines() for the joining state, given the host's name (empty while the room hasn't named it).
std::pair<std::string, std::string> JoiningLines(const std::string& host, bool ranked);
}  // namespace Search

class OnlineMenuReader
{
public:
  // One frame. `resyncs` is Events::Resyncs(). Returns the pick to announce, if any.
  std::optional<OnlinePick> Boundary(const MenuState& state, bool resimulating, bool alone,
                                     u64 resyncs);
  // The pick the last Boundary saw made while this game was not alone, which it never announces
  // (the session decides what it means: TakeLobbyPick below). Once; the next Boundary forgets it.
  std::optional<OnlinePick> TakeBusyPick();

private:
  bool m_started = false;
  u64 m_resyncs = 0;
  // The previous scene (or this one) was the main menu.
  bool m_from_menu = false;
  // This exit's code was already seen.
  bool m_seen = false;
  std::optional<OnlinePick> m_busy;
};

// ---- Leaving the friends lobby for the queue, and coming home (ORCA.md "Drop-in", "Online
// menu"). Casual and Ranked under With Anyone leave the friends lobby for a 2-player room the
// matchmaker fills; a guest whose host left goes back to its own lobby. ----

// Prints "orca menu online ..." for a pick ("... local" without the app's "host") and starts what
// it means: the queue's own character select (with "queue2"), the search, or neither (Friends).
// `arm` false prints the line alone, marked "kept": a host that keeps its friends because the app
// can't search that queue now (PickSearchable), so the app says why and cancels, and nothing of the
// queue reaches the friends' game.
void AnnounceOnlinePick(OnlinePick pick, bool arm = true);

// Whether the app can search this pick's queue right now ("pick-casual", "pick-ranked":
// Orca::Status PICK_CAPS); never Friends. Any thread.
bool PickSearchable(OnlinePick pick);

// A Casual or Ranked pick the frame hook saw at this boundary while the game was not alone, which
// nothing printed. The session takes it at the same boundary, after the hook: a host leaves its
// friends for the queue and announces it (or, without the pick's cap, stays and announces it
// unarmed); a joiner lets it go. Once. CPU thread.
std::optional<OnlinePick> TakeLobbyPick();

// Whether a scene counts as "the menus", where a former joiner moves to port 1 (Rollback/
// OnlineMatch.cpp ComeHome): the main menu or a character select; never a fight, a stage select, a
// results screen, a scene change or a single-player mode. Pure.
bool MenusScene(std::string_view scene, std::string_view sequence);
// MenusScene as the frame hook read it at this boundary (first runs; false before the first, and
// for games other than Brawl rev 2 and Project+). CPU thread.
bool OnTheMenus();

// The Casual or Ranked pick the character select read at this boundary was opened for (the main
// menu's exit code kept in sqVsMelee, CssTitle::ReadPick: 30, 31); nullopt anywhere else. A former
// joiner that comes home there picks it up. First runs, as OnTheMenus. CPU thread.
std::optional<OnlinePick> CssPick();
// Whether a Casual or Ranked pick the session couldn't take when it was seen (Rollback/
// OnlineMatch.cpp TakeLobbyPickNow) still stands: on the way out of the main menu with that pick's
// exit code, or on the character select it opened. Anywhere else it is stale. CPU thread.
bool PickStands(OnlinePick pick);
// The Casual or Ranked pick (30, 31) a main menu exit code stands for. Pure.
std::optional<OnlinePick> QueuePickForCssCode(u32 code);

// Frame hook: reads the menu and prints "orca menu ...". Reads emulated memory only.
void ReadOnlineMenu(const Core::CPUThreadGuard& guard, bool resimulating, bool alone);

// ---- Friends drop in from the menus ----
// A friend who joins while the host is in the menus moves both games to With Friends' character
// select, as if the host had picked it. A friend who joins during a single-player mode waits until
// the host is back (`orca state friend-holding` / `friend-waiting`).
//
// gfSceneManager (0x805A0060): +0x4 scene, +0x10 sequence (names at +0), +0x284 exit code, +0x288
// process step. Every menu exit writes the code, then step 2.

// The current sequence's name ("sqMenuMain", "sqVsMelee", ...; "" when unreadable).
std::string ReadSequenceName(const GuestMemory& memory);

// Whether a friend must wait in this sequence: single-player modes and the Vault. Everything else,
// including the boot, takes a friend in.
bool SequenceHoldsDropIn(std::string_view sequence);

// Whether the host's game was in such a sequence at its last first-run frame. Any thread.
bool DropInHeld();
// Whether the game was in its Training mode (sqTraining) at its last first-run frame, for the
// activity lines (Orca/Activity.h). Any thread.
bool InTraining();

namespace FriendsMove
{
// The scene manager's process step while a scene runs, and the step that ends it.
constexpr u32 STEP_RUNNING = 1;
constexpr u32 STEP_EXIT = 2;
// With Friends: goes to sqVsMelee, and B from its character select returns to the ONLINE page.
constexpr u32 EXIT_WITH_FRIENDS = 25;
// No friend plugs in before this frame, because a host never makes a keyframe in its first 300
// frames (Rollback/OnlineMatch.cpp FIRST_KEYFRAME_FRAME).
constexpr int FIRST_FRIEND_FRAME = 300;
// The friend sees the host's built menu for half a second before both move.
constexpr u32 MENU_SETTLE_FRAMES = 30;
// muMenuMain's "pages built" byte (same in both games). Its files load over many frames, then all
// pages are built in one frame and this becomes 1. Leaving the menu before then calls through an
// unfilled proc table and Brawl hangs, so never exit until it is set. Meaningless at step 0.
constexpr u32 MENU_BUILT = 0xAC8;
// Options > Controls (its name list and the name keypad) is a gfTask of its own, "MuName"
// (muNameTask, sora_menu_name), linked in the same process list as the menu's task (muMenuMain
// +0x7B4, "MainMenuTask"; gfTask +0x0 its name, +0x4 the previous task, +0x8 the next). The game
// never leaves the menu while it runs (B backs out of it first); leaving then runs it once more on
// what the exit tore down: on the list both games freeze, on the keypad both crash (0x80677f54,
// Brawl and Project+, Orca 0.3.38). Never move while it runs: the friend waits on the host's menu
// until the host leaves Controls.
constexpr u32 MENU_TASK = 0x7B4;
constexpr u32 TASK_PREV = 0x4;
constexpr u32 TASK_NEXT = 0x8;
constexpr std::string_view KEYPAD_TASK = "MuName";
// How many tasks the walk reads each way (a menu has about six).
constexpr int TASK_WALK_MAX = 64;
// Whether a task named KEYPAD_TASK is in the process list of the main menu `menu` (the scene).
// Reads only; a pure function of memory.
bool KeypadOpen(const GuestMemory& m, u32 menu);

// What one frame reads.
struct Inputs
{
  // The block's memory is present (MatchBlock::Present and the friends bytes mapped).
  bool present = false;
  // Ports plugged in this frame (bit n: port n+1).
  u8 plugged = 0;
  // A casual or ranked header is present: queue matches never go to With Friends.
  bool queue = false;
  std::string scene;
  std::string sequence;
  u32 step = 0;
  u32 exit_code = 0;
  // On muMenuMain with MENU_BUILT set (only then can it be left).
  bool menu_built = false;
  // On muMenuMain with Options > Controls open (KeypadOpen): the menu can't be left.
  bool keypad = false;
  int frame = 0;
  // The block's friends bytes (MatchBlock FRIENDS_*).
  u8 tag = 0;
  u8 seen = 0;
  u8 pending = 0;
  // Frame + 1 since which the built main menu has run with a friend pending (0: not yet).
  u32 menu_since = 0;
};

// What that frame writes.
struct Outputs
{
  // Write the friends bytes. False when nothing was kept and no friend is plugged in, so solo play
  // never writes them.
  bool keep = false;
  u8 seen = 0;
  u8 pending = 0;
  u32 menu_since = 0;
  // Leave the main menu for With Friends (exit code plus STEP_EXIT, like the menu's own exit).
  bool move = false;
};

// Pure function of its inputs, so a rollback re-run at the same frame writes the same thing.
Outputs Decide(const Inputs& in);

// Frame hook, first runs and re-runs, Brawl rev 2 and Project+ only. Returns whether the host left
// the main menu for With Friends this frame.
bool Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports);
}  // namespace FriendsMove

// ---- A fresh start (Rollback/OnlineMatch.cpp, ORCA.md "Drop-in") ----
// Every Orca boots the same way up to its origin: the first frame its main menu has been built for
// MENU_SETTLE_FRAMES frames, with nobody's controller in it. Before a match, the host goes back to
// that origin and leaves the main menu for the match's character select the way the player's own
// press would: the exit code (Orca::Net::MENU_EXIT_*: With Friends, Casual or Ranked) is a typed
// event on the replay's first frame, written like FriendsMove's move. A joiner replays the same
// event from its own origin, so the match's history starts there on both machines.
namespace FreshMove
{
// What one frame shows, for the session's origin and the fresh start's tail.
struct Seen
{
  // The main menu runs with its pages built and no exit chosen (it can be left).
  bool menu_built = false;
  // A character select is up: its task and panels can be read.
  bool css = false;
  bool operator==(const Seen&) const = default;
};
Seen Read(const GuestMemory& memory);

// Leaves a built main menu with `exit` (25, 30 or 31): the code, then STEP_EXIT. Writes nothing
// anywhere else or for any other code, so a re-run of the same frame writes nothing more. Returns
// whether it wrote.
bool Apply(GuestMemory& memory, u32 exit);

// The game's two random number generators, mtRand objects (a vtable word, then a 31-bit LCG seed:
// seed = (seed * 0x41C64E6D + 12345) & 0x7FFFFFFF). Brawl rev 2 and Project+ alike.
// - RNG_DEFAULT, g_mtRandDefault: randi/randf, everything a fight draws.
// - RNG_MENU: the menus' own. The character select draws every panel's RANDOM from it as it
//   starts (0x806857F0: three draws per panel), and the pick under the token is that draw.
// Orca pins the clock and boots the same scripted way every time, so both start every boot (and
// every fresh start's origin) from the same numbers: without a seed, RANDOM resolved to the same
// character in every match (ORCA.md "Random").
constexpr u32 RNG_DEFAULT = 0x805A00B8;
constexpr u32 RNG_MENU = 0x805A0420;
constexpr u32 RNG_VTABLE = 0x8042AE50;  // mtRand's vtable, the first word of both
constexpr u32 RNG_SEED = 0x04;          // the seed's offset in an mtRand
// The seed word `seed` gives each generator: different for the two, never above 31 bits.
u32 RngSeed(u32 seed, u32 rng);
// Starts both generators from `seed` (a replay's first frame, ReplayFrame::seed): writes the two
// seed words, and only where both generators are what this expects. 0 writes nothing (the
// canonical boot's own numbers). The same seed on the same state writes the same words, so a
// re-run of the frame ends as the first run did. Returns whether it wrote.
bool ApplySeed(GuestMemory& memory, u32 seed);

// Frame hook, first runs and re-runs: applies the replay's recorded exit and seed on its first
// frame, then reads the frame for LastSeen.
void Frame(const Core::CPUThreadGuard& guard, int frame);

// What the last call of the frame hook read (nothing for other games). CPU thread.
Seen LastSeen();

// Whether this game's main menu can be read and left: Brawl rev 2, and Project+.
bool Supported();
}  // namespace FreshMove
}  // namespace Orca::UX
