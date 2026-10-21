// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

// What the online session tells the rest of Orca (overlay, name tags, the app's netlog). The
// session owns the data; readers get copies and callbacks.
namespace Orca::Events
{
// A controller port in play: its player's YouGame username and controls. These are the host's
// values for the port at that frame, so they are identical on every machine.
struct PortInfo
{
  int port = 0;  // 0-based GameCube port
  std::string name;
  // Whether another machine holds this port. Differs per machine: UI only, never memory writes.
  bool remote = false;
  // The player's controls as their own game stored them (UX/NameTags.h); empty if none were sent.
  std::vector<u8> controls;
  // The player's queue identity (UX/Queue.h); empty if none. The host's value, so a joiner's pick
  // is written at the same frame on every machine.
  std::vector<u8> queue;
  bool operator==(const PortInfo&) const = default;
};

// This player's controls, read from their game while playing solo, to carry into a game they join
// or host. Any thread.
void SetOwnControls(std::vector<u8> controls);
std::vector<u8> OwnControls();
// This player's queue identity (UX/Queue.h), carried like the controls. Any thread.
void SetOwnQueue(std::vector<u8> queue);
std::vector<u8> OwnQueue();

// Plugged-in ports changed, at the boundary of `frame`, on the CPU thread. First runs only, so it's
// for UI; memory writes must use FrameCallback's `ports`. Setting it again replaces it; an empty
// function clears it.
using PlugInCallback = std::function<void(int frame, const std::vector<PortInfo>& ports)>;
void SetPlugInCallback(PlugInCallback callback);

// Link health, refreshed about once a second. Any thread.
struct LinkStats
{
  bool online = false;  // a friend is plugged in
  int delay = 0;        // input delay, in frames
  double rollbacks_per_second = 0;
  int deepest_rollback = 0;  // frames, in the last second
  int stalls = 0;            // frames the game waited for a friend's input, in total
  int hitches = 0;           // this machine's slow frames, in total
  int peer_hitches = 0;
  int round_trip_ms = -1;  // to the room server; -1 before the first
  // How friends' inputs arrive (worst of them): 0 relay, 1 direct link, 2 direct link through TURN;
  // and that link's round trip (-1: none).
  int transport = 0;
  int link_rtt_ms = -1;
  int confirmed_frame = -1;
  // The game is stalled on a friend's input: for how long, and whose seat (-1: nobody).
  int stalled_ms = 0;
  int waiting_seat = -1;
};
LinkStats GetLinkStats();

// The local controller, when something other than Dolphin's pad input supplies it (the app's
// controller stream). Called on the CPU thread once per frame boundary and must not block; nullopt
// means use Dolphin's pad. The session records the result, so re-runs replay it. `for_frame` is
// true only for a first run's own read, which is the one the source times (InputAge).
using LocalPadSource = std::function<std::optional<GCPadStatus>(bool for_frame)>;
void SetLocalPadSource(LocalPadSource source);
std::optional<GCPadStatus> LocalPad(bool for_frame = false);

// Age of the local controller's input when frames read it, as running totals. Only devices whose
// reports carry an arrival time (a GameCube adapter via the app's helper) count. Readers take
// differences; totals reset when the app's stream restarts. Any thread.
struct InputAge
{
  u64 frames = 0;     // frames that read such a device
  u64 reports = 0;    // reports that came in from it across those frames
  u64 changes = 0;    // frames whose pad differed from the frame before's: the age samples
  double age_ms = 0;  // summed input age over the changes (ControllerSource.h)
};
using InputAgeSource = std::function<InputAge()>;
void SetInputAgeSource(InputAgeSource source);
InputAge GetInputAge();

// Called at every frame boundary, on every machine, first runs and re-runs alike, before any
// snapshot or keyframe is saved there. `frame` is the frame about to run; `resimulating` is true
// during a rollback. Any write to emulated memory from here must be idempotent and depend only on
// that memory and synced data.
// `ports`: the controllers plugged in at `frame`, identical on every machine. Use these for memory
// writes, never NotifyPlugIn's.
// `alone`: this machine's player is playing solo with no drop-in pending (OnlineMatch AloneAt).
// Always false on re-runs. It differs between machines, so it may shape what a reader prints, never
// what it writes to memory.
using FrameCallback =
    std::function<void(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                       const std::vector<PortInfo>& ports, bool alone)>;
void SetFrameCallback(FrameCallback callback);

// The game no longer follows on from the frames readers saw: it went solo after a session, or
// loaded a friend's keyframe. Readers that track the game across boundaries (UX/OnlineMenu.h)
// restart from the next one. CPU thread; Resyncs() from any thread.
void NoteResync();
u64 Resyncs();

// During a stall, every few milliseconds on the CPU thread: how long so far. The overlay redraws
// the last frame from here, since nothing is presented during a stall.
using StallCallback = std::function<void(int stalled_ms)>;
void SetStallCallback(StallCallback callback);

// Every frame boundary, on the CPU thread: whether the frame that just ended was shown (a normal
// first run, not a re-run or catch-up), and how long the game itself held it past one video frame
// (GameHeldMs), in ms and in video frames (GameHeldFrames). Drives the frame meter. Must not touch
// emulation.
using BoundaryCallback = std::function<void(bool shown, double game_held_ms, int game_held_frames)>;
void SetBoundaryCallback(BoundaryCallback callback);

// How much longer than one video frame (`frame_ms`) the game took over a frame in emulated time:
// a load, the same on every machine and on a Wii. 0 unless it took 1.5 frames or more, and never
// more than this machine took past one frame (`wall_ms`), so a jump in emulated time adds nothing.
double GameHeldMs(double emulated_ms, double wall_ms, double frame_ms);
// Those milliseconds as whole video frames. `carry` keeps the fraction for the next hold, so a run
// of loads adds up to the frames they held.
int GameHeldFrames(double game_held_ms, double frame_ms, double& carry);

// Version of what the in-game UX writes to emulated memory; part of the lobby compatibility key.
// Set before boot.
void SetUXCompatVersion(int version);
int UXCompatVersion();

// Called by the session: publish the latest stats and fire the plug-in callback.
void PublishLinkStats(const LinkStats& stats);
void NotifyPlugIn(int frame, const std::vector<PortInfo>& ports);
void NotifyFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                 const std::vector<PortInfo>& ports, bool alone);
void NotifyStall(int stalled_ms);
void NotifyBoundary(bool shown, double game_held_ms, int game_held_frames);
}  // namespace Orca::Events
