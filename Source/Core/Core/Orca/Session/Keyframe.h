// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/Rollback.h"

// Network drop-in transfers are bounded controller-input replays with typed UI events. Each
// machine restores its own local boot snapshot and executes the replay before joining live play.
// NAND helpers below serve local snapshots only; no NAND or Dolphin state is accepted from peers.
namespace Orca::Net
{
// One file or folder of the session NAND, by its '/'-separated path under the NAND root.
struct NandEntry
{
  std::string path;
  bool directory = false;
  std::vector<u8> data;
  // A file the joiner's own boot also writes (Profile::boot_nand_frame): the keyframe carries only
  // its XXH3-64, and the joiner uses its own copy if the hash matches.
  bool reference = false;
  u64 hash = 0;
};

// A file's hash as a reference carries it.
u64 NandHash(const std::vector<u8>& data);
// The NAND's files under `root` as {path: hash} (no folders), for references.
std::map<std::string, u64> HashNandTree(const std::string& root);
// Replaces each reference in `entries` with the joiner's own file at that path under `root`. False,
// with the path in `error`, if one is missing or differs.
bool ResolveNandReferences(const std::string& root, std::vector<NandEntry>* entries,
                           std::string* error);

// The NAND's files under `root`, in name order. False if the tree can't be read.
bool ReadNandTree(const std::string& root, std::vector<NandEntry>* entries);
// Replaces everything under `root` with `entries`. `root` must be a temporary session NAND; the
// caller checks.
bool ReplaceNandTree(const std::string& root, const std::vector<NandEntry>& entries);

// Packs and unpacks a keyframe. `hash` is the hex XXH3 of the compressed bytes.
std::vector<u8> PackKeyframe(int frame, const ReplayArchive& replay);
bool UnpackKeyframe(const std::vector<u8>& blob, int* frame, ReplayArchive* replay);
std::string KeyframeHash(const std::vector<u8>& blob);

// ---- Replay streams: saved matches and live watching ----
// A replay stream is a file, or bytes on the wire, that grows while a match is played: a header,
// then chunks of the match's final frames (every seat's confirmed inputs and the typed UI events,
// the same records a drop-in join replays), each carrying the session's RAM checksums for the
// frames it covers. Any prefix that ends on a chunk is a complete replay up to that frame, so the
// same bytes serve a saved replay and a live watcher a few frames behind. Inputs only: never RAM
// images, NAND files or addresses, as for a join.
struct ReplayStreamInfo
{
  // Online::CompatibilityKey() of the recording build: a replay plays back only where it matches.
  std::string compat;
  // The recording build's git revision, so a player can fetch that build.
  std::string build;
  std::string game_id;
  u64 origin_hash = 0;
  int first_frame = -1;
  // This process's history count: a fresh start or a join starts another.
  u32 segment = 0;
  std::string room;
  // Seat labels differ from the live names in RAM, including game-owned copies.
  bool seat_labels = false;
};

struct ReplayChunk
{
  int first = 0;
  // The recording machine's own newest frame when it wrote this chunk (its live frame), so a
  // watcher knows how far behind the match itself it is, not only behind the stream.
  int live = 0;
  std::vector<ReplayFrame> frames;
  // (frame, RamChecksum of the state at that frame's start, after its hook), from the session.
  std::vector<std::pair<int, u64>> checksums;
};

std::vector<u8> PackReplayStreamHeader(const ReplayStreamInfo& info);
// Empty if the frames can't be written (too large, a seed or menu exit past the history's start).
std::vector<u8> PackReplayChunk(const ReplayChunk& chunk, bool history_start);

// A whole history as one compact stream: the header, then chunks of up to a minute of frames with
// every checksum, for keeping a finished match (a live stream's small chunks compress poorly).
// Empty if a frame can't be written.
std::vector<u8> PackReplayStream(const ReplayStreamInfo& info, const ReplayArchive& replay,
                                 const std::map<int, u64>& checksums);
// A saved copy (PackReplayStream) goes to the room in `rp` pieces (Online::SendReplayPacked): at
// most 1 MiB in all, at most 32 KiB a piece before base64.
constexpr size_t REPLAY_PACKED_MAX = 1u << 20;
constexpr size_t REPLAY_PACKED_PIECE_MAX = 32u << 10;

// Decodes a replay stream fed in pieces (a growing file, or network messages). Feed takes the
// next bytes; a partial chunk waits for the rest. Frames must arrive in order with no gap. Resume
// begins another segment of the same history: its header (all but the segment count the same),
// then frames from its start, comparing the overlap and appending only what wasn't already read.
//
// The bytes come from other players (a file they made, later the room), so this is an untrusted
// parser: every length is bounded before it is used, the stream's compatibility key is checked
// before any frame is decoded, and frames, checksums and port metadata are capped in total.
class ReplayStreamReader
{
public:
  // `compat`: the only compatibility key accepted (this build's Online::CompatibilityKey()); empty
  // accepts any (tests and tools that only read).
  explicit ReplayStreamReader(std::string compat = {}) : m_compat(std::move(compat)) {}
  // False, with `error`, on bytes that aren't a valid stream; the reader is then spent.
  bool Feed(const u8* data, size_t size, std::string* error);
  // A room reset: discard an incomplete tail, keeping the archive, checksums and metadata budget.
  // The next header must be the same history (all but its segment count); the overlap is then
  // compared frame by frame. A failed reader stays failed.
  void Resume();
  const std::optional<ReplayStreamInfo>& Info() const { return m_info; }
  // Every frame decoded so far, from Info()->first_frame.
  ReplayArchive& Archive() { return m_archive; }
  const std::map<int, u64>& Checksums() const { return m_checksums; }
  // The newest live frame a chunk named (ReplayChunk::live), or -1.
  int LiveFrame() const { return m_live; }
  u64 BytesRead() const { return m_bytes; }
  u32 Chunks() const { return m_chunks; }
  // Resumed segments whose identical header has been accepted (not just a reset received).
  u32 Resumes() const { return m_resumes; }
  // At saved-file EOF (or live end): a complete header and no partial chunk/next segment.
  bool Complete() const { return m_info.has_value() && m_pending.empty() && !m_resuming && !m_failed; }

private:
  bool Parse(std::string* error);
  std::string m_compat;
  std::vector<u8> m_pending;
  std::optional<ReplayStreamInfo> m_info;
  bool m_resuming = false;
  int m_segment_end = -1;
  // Port metadata still allowed (as PackKeyframe's budget): names and controls that change every
  // frame can't grow memory past it.
  size_t m_metadata_budget = 64ull << 20;
  ReplayArchive m_archive;
  std::map<int, u64> m_checksums;
  u64 m_bytes = 0;
  u32 m_chunks = 0;
  u32 m_resumes = 0;
  int m_live = -1;
  bool m_failed = false;
};

// AES-256-GCM over a packed keyframe, so YouGame's store only holds ciphertext. The key is random
// per keyframe and reaches the joiner only in the room's `kf` message (KeyframeInfo::key, 64 hex).
// Body: IV (12) | ciphertext | tag (16); the frame number is authenticated too.
bool EncryptKeyframe(int frame, std::vector<u8>* blob, std::string* key_hex);
bool DecryptKeyframe(int frame, const std::string& key_hex, std::vector<u8>* blob);

// A joiner's checks on the host's encrypted replay, whether downloaded from the store or inline in
// the offer (KeyframeInfo::inline_blob): the size and hash the offer named, decryption with its
// key, and unpacking at its frame. False, with `error` saying which, on any mismatch.
bool OpenKeyframe(const KeyframeInfo& info, std::vector<u8> blob, int* frame, ReplayArchive* replay,
                  std::string* error);

// Where keyframes travel: the host puts one, the joiner gets it, and it's deleted once loaded. Put
// and Get block, so call them on their own thread; the progress callback can return false to abort.
// Delete never blocks.
class KeyframeStore
{
public:
  virtual ~KeyframeStore() = default;
  virtual bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) = 0;
  virtual std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                             const std::function<bool(u64, u64)>& progress,
                                             std::string* error) = 0;
  virtual void Delete(const std::string& id) = 0;
  // Makes Put and Get give up as soon as they can (emulation is stopping).
  virtual void Cancel() {}
};

// Supplies a room ticket (a newly minted one when `fresh`) and the store's URL for the room.
using TicketSource = std::function<bool(bool fresh, std::string* ticket, std::string* store_url,
                                        std::string* error)>;

// ORCA_TEST_KEYFRAME_DIR: a folder shared by two processes on one machine (tests). Otherwise
// YouGame's keyframe endpoint, authorized with room tickets.
std::unique_ptr<KeyframeStore> MakeKeyframeStore(TicketSource tickets);

// The HTTP store alone (tests point it at a local server).
std::unique_ptr<KeyframeStore> MakeHttpKeyframeStore(TicketSource tickets);

// After a Put or Get on this thread failed: why, when YouGame's store refused for good (its code,
// such as "signed_out"). Empty when another try could work (a network error, 429, 5xx, a 4xx
// without YouGame's code) or the call was cancelled.
std::string LastRefusal();
}  // namespace Orca::Net
