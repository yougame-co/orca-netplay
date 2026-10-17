// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/WorkQueueThread.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/Rollback.h"

namespace Core
{
class System;
}
namespace File
{
class IOFile;
}

// Saved matches and live watching (Session/Keyframe.h "Replay streams"). A player's Orca writes its
// history's final frames to a replay stream as they settle; a watcher or a saved replay plays one
// back from this machine's own origin, checking every checksum it carries. A watcher is a player
// of a stream that is still growing: it is no seat in the match, so it can't slow or change it.
//
// The product switch: with the app's "record" cap (Orca/Status.h), every match runs a room-only
// writer that records only while Online::RecordWanted() (ranked rooms by default, any room after the
// app's "record on"); otherwise it costs one check a frame. It streams to the room for watchers as
// {"t":"rs","b":<base64>} (Online::SendReplayStream), and at each game's result it also sends the
// history written so far as a compact saved replay (Online::SendReplayPacked):
// {"t":"rp","b":<base64>,"i":<piece index>,"n":<piece count>,"f":<frames>,"ms":<duration at
// 60 fps>}, zero-based pieces of at most 32 KiB decoded, at most 1 MiB for the whole file. The
// emulation thread only copies the bytes already written; a worker thread repacks them
// (PackedSender), so neither the frame nor the result report waits on it. The room keeps the
// latest complete copy for the site when the stream ends (the last game covers the set).
// Test knobs (UX/Probe.h TestKnobsAllowed):
//  - ORCA_TEST_REPLAY_OUT=<base>: write this machine's history to <base>-<n>.ygr (a fresh start or
//    a join begins another history, so another file). ORCA_TEST_REPLAY_ROOM=1 also streams it to
//    the room, with saved copies, whatever the room or the app's caps; <base> may then be empty.
//    ORCA_TEST_REPLAY_TIMING=1 logs Pump's cost in microseconds every 3600 frames and at shutdown.
//  - ORCA_TEST_REPLAY_IN=<file>: instead of a game, boot to the origin and play <file>; or a
//    ws:// or wss:// watch URL (the rooms Worker's /watch/<code> with a watch ticket), whose
//    {"t":"rs","b":<base64>} messages carry the stream, {"t":"end"} its end; or watch:<room>, which
//    gets its watch ticket from ORCA_SITE for the dev game, a fresh one for every connection. A
//    dropped connection comes back (about 30 s of tries) and skips the bytes already played;
//    {"t":"reset"} starts a new segment, whose header and overlapping frames must match.
//    ORCA_TEST_REPLAY_WATCH_DROP=<s> drops it once, that long after it connected.
//    ORCA_TEST_REPLAY_FOLLOW=1 keeps reading as the file grows, ORCA_TEST_REPLAY_DELAY (12) frames
//    behind its newest frame (a live watcher); ORCA_TEST_REPLAY_REALTIME=1 plays a finished file at
//    real speed rather than flat out; ORCA_TEST_REPLAY_SEEK=<a>:<b> goes back from frame b to a
//    snapshot taken at frame a, once. ORCA_TEST_REPLAY_REPACK=<file> writes a clean replay there
//    again as one compact stream.
// Stdout (session only): "orca replay-out <path>" per file written; a player prints "orca replay
// start ...", "orca replay at ..." every 600 frames, "orca replay seek ...", "orca replay
// reconnected <n>", "orca replay resumed ..." after a reset, and one
// "orca replay done ok|fail <key=value>... why=<text>" line.
namespace Rollback::ReplayStream
{
// A history's saved copy from the bytes its writer wrote (a header and live chunks, ~60 KB a
// minute): read back, packed compactly (Orca::Net::PackReplayStream, ~3 KB a minute) and split for
// the room. Empty if the bytes aren't one whole history with frames, the packed copy exceeds 1 MiB
// (`too_big` then says so) or max_piece is 0; max_piece is capped at 32 KiB. `frames` gets the
// copy's frame count.
std::vector<std::vector<u8>> RepackedPieces(const std::vector<u8>& written, int* frames,
                                            size_t max_piece = Orca::Net::REPLAY_PACKED_PIECE_MAX,
                                            bool* too_big = nullptr);

// Repacks saved copies on its own thread and hands the pieces to `send` there, so the caller (the
// emulation thread, at a game's result) only gives up a copy of the written bytes. Copies are
// cumulative: one still waiting when a newer one arrives is skipped. The thread starts with the
// first copy; destruction finishes the copies already handed over (milliseconds each).
class PackedSender
{
public:
  using Send =
      std::function<void(const std::vector<std::vector<u8>>& pieces, int frames, u64 generation)>;
  explicit PackedSender(Send send);
  ~PackedSender();
  // `history`: the writer's history count, for TooBig.
  void Submit(std::vector<u8> written, u64 generation, u32 history);
  // A copy of this history already packed past 1 MiB (its later copies would too).
  bool TooBig(u32 history) const;

private:
  struct Job
  {
    std::vector<u8> written;
    u64 generation = 0;
    u32 history = 0;
    u64 sequence = 0;
  };
  Send m_send;
  std::atomic<u64> m_latest{0};
  // The history whose copy packed past 1 MiB (histories count from 1).
  std::atomic<u32> m_too_big{0};
  Common::WorkQueueThread<Job> m_thread;
  bool m_started = false;
};

class Writer
{
public:
  // The test knobs' writer, else the product's room-only one when this build offers "record".
  static std::unique_ptr<Writer> ForMatch();
  // `gated`: records only while Online::RecordWanted() and the session runs (the product switch).
  explicit Writer(std::string base, bool to_room = false, bool gated = false);
  ~Writer();
  // Appends `replay`'s frames before `end` (all final) with the checksums among `checksums`
  // (frame: RAM checksum at its start) that they cover, at most every 6 frames or 100 ms. `live`
  // is this machine's own newest frame, for watchers. A history other than the one being written
  // begins another file. False once writing failed.
  // With the room on (ORCA_TEST_REPLAY_ROOM), the room takes a stream only from a seated player
  // in a match, so it starts once `room_ready` (a session runs): the header, then everything
  // written so far in compact chunks, then each chunk as it is written. A reopened room starts
  // again with the same header and backlog.
  bool Pump(const Orca::Net::ReplayArchive& replay, int end, int live,
            const std::map<int, u64>* checksums, bool room_ready = false);
  // At a game's result: hands a copy of this history's written bytes to the packer thread, which
  // sends the room a saved copy of the match so far. Quietly does nothing until this history's room
  // stream has started, or if this room no longer records.
  void SendPacked();

private:
  bool Emit(const std::vector<u8>& bytes);
  bool StartRoom(const Orca::Net::ReplayArchive& replay, int live);
  void LogTiming(bool final) const;

  struct Timing;
  std::unique_ptr<Timing> m_timing;

  std::string m_base;
  bool m_to_room = false;
  bool m_started = false;
  bool m_room_started = false;
  u64 m_room_generation = 0;
  // The checksums already written: sessions only retain their last few seconds, but a reopened
  // room needs the whole backlog's checksums too.
  std::map<int, u64> m_room_checksums;
  std::vector<u8> m_header;
  // This history's bytes as written (the file's content), for its saved copies; dropped past
  // MAX_WRITTEN (hours of play, far past what packs into 1 MiB).
  std::vector<u8> m_written;
  bool m_written_over = false;
  std::unique_ptr<PackedSender> m_packer;
  bool m_gated = false;
  // The gated writer's last Online::RecordWanted() answer, and the live frame it was asked at.
  bool m_record_wanted = false;
  int m_record_checked = -1;
  u32 m_segment = 0;
  std::unique_ptr<File::IOFile> m_file;
  int m_first = -1;
  u64 m_origin = 0;
  u32 m_seed = 0;
  int m_written_end = -1;
  std::chrono::steady_clock::time_point m_flushed{};
};

// What a player needs from the match it runs in: the restores it may do, and how it drives frames.
struct PlayerHost
{
  // Puts this machine's own origin back; its frame starts next. False: the run can't go on.
  std::function<bool()> restore_origin;
  // Puts back a seek point this player took, as `frame`.
  std::function<bool(MachineImage image, const std::vector<Orca::Net::NandEntry>& nand, int frame)>
      restore;
  // This machine's session NAND, for a seek point.
  std::function<bool(std::vector<Orca::Net::NandEntry>* nand)> read_nand;
  std::function<void(bool catching_up)> set_catching_up;
  std::function<void(int frame, const Orca::Net::Pads& pads)> set_pads;
  std::function<bool()> stopping;
  // Ends the run, with a message for the player.
  std::function<void(const std::string& message)> stop;
};

class Player
{
public:
  Player();
  ~Player();
  static std::unique_ptr<Player> FromEnv();
  // Plays the next recorded frame instead of a game, once this machine's origin is in. `running`
  // is the last frame run. Returns the frame the state now starts at when it was rewound (the
  // replay loaded, or a seek). Over() once it ended, either way.
  std::optional<int> Step(Core::System& system, const PlayerHost& host, int* running,
                          u64 origin_hash, int origin_frame, const OnlineMatch::FrameHook& on_frame);
  bool Over() const { return m_over; }

private:
  bool Poll(std::string* error);
  void WatchThread(std::string source);
  void Finish(const PlayerHost& host, bool ok, const std::string& why);

  std::string m_path;
  bool m_follow = false;
  bool m_realtime = false;
  int m_delay = 12;
  std::optional<std::pair<int, int>> m_seek;
  // ORCA_TEST_REPLAY_REPACK=<file>: after a whole replay played clean, write it there as one
  // compact stream (PackReplayStream).
  std::string m_repack;
  std::unique_ptr<File::IOFile> m_file;
  u64 m_offset = 0;
  // A watch URL's socket runs on its own thread, handing bytes over here.
  struct Watch;
  std::unique_ptr<Watch> m_watch;
  bool m_source_ended = false;
  std::unique_ptr<Orca::Net::ReplayStreamReader> m_reader;
  bool m_loaded = false;
  bool m_over = false;
  bool m_catching = true;
  int m_played = 0;
  int m_checked = 0;
  int m_mismatched = 0;
  int m_first_mismatch = -1;
  std::chrono::steady_clock::time_point m_started{}, m_last_data{};
  // While watching in real time: frames behind the stream's newest frame, and behind the
  // recording machine's own live frame (the stream's lag plus this one's).
  s64 m_behind_sum = 0;
  int m_behind_samples = 0;
  int m_behind_max = 0;
  s64 m_live_sum = 0;
  int m_live_max = 0;
  // When a watcher first reached real time (its catch-up time), or -1.
  double m_caught_up_ms = -1;
  int m_waits = 0;
  double m_waited_ms = 0;
  int m_last_progress = 0;
  MachineImage m_seek_image;
  std::vector<Orca::Net::NandEntry> m_seek_nand;
  bool m_seek_taken = false;
  bool m_seek_done = false;
};
}  // namespace Rollback::ReplayStream
