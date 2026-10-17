// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Keyframe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <chrono>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>

#include <atomic>

#include <curl/curl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <fmt/format.h>
#include <picojson.h>
#include <xxh3.h>
#include <zstd.h>

#include "Common/CommonFuncs.h"
#include "Common/CurlTLS.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"

namespace Orca::Net
{
namespace
{
// ORP2 contains controller inputs and typed UI events; legacy machine images are rejected. Builds
// with another layout of it have another UX compat version, so they never meet. ORP2 added each
// frame's seed (ReplayFrame::seed).
constexpr char MAGIC[4] = {'O', 'R', 'P', '2'};
constexpr int WINDOW_LOG = 26;
constexpr int LEVEL = 3;
// Sanity limit per section (MEM2 is 64 MB; a Wii NAND is at most 512 MB).
constexpr u64 MAX_SECTION = 512ull << 20;
constexpr u64 MAX_RAW = 64ull << 20;
// The site accepts at most 128 MB.
constexpr u64 MAX_KEYFRAME_BYTES = 128ull << 20;

class Writer
{
public:
  void U32(u32 v) { Bytes(&v, sizeof(v)); }
  void U64(u64 v) { Bytes(&v, sizeof(v)); }
  void Bytes(const void* data, size_t size)
  {
    if (size > MAX_RAW - out.size())
      throw std::length_error("Replay too large");
    if (size == 0)
      return;
    const auto* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + size);
  }
  void Blob(const std::vector<u8>& data)
  {
    U64(data.size());
    Bytes(data.data(), data.size());
  }
  std::vector<u8> out;
};

class Reader
{
public:
  explicit Reader(const std::vector<u8>& data) : m_data(data) {}
  bool Bytes(void* out, size_t size)
  {
    if (size > m_data.size() - m_pos)
      return false;
    std::memcpy(out, m_data.data() + m_pos, size);
    m_pos += size;
    return true;
  }
  bool U32(u32* v) { return Bytes(v, sizeof(*v)); }
  bool U64(u64* v) { return Bytes(v, sizeof(*v)); }
  bool Blob(std::vector<u8>* out, u64 limit = MAX_SECTION)
  {
    u64 size;
    if (!U64(&size) || size > limit || size > m_data.size() - m_pos)
      return false;
    out->assign(m_data.begin() + m_pos, m_data.begin() + m_pos + size);
    m_pos += size;
    return true;
  }
  bool AtEnd() const { return m_pos == m_data.size(); }

private:
  const std::vector<u8>& m_data;
  size_t m_pos = 0;
};

void Collect(const File::FSTEntry& entry, const std::string& prefix,
             std::vector<NandEntry>* entries, bool* ok)
{
  std::vector<const File::FSTEntry*> children;
  for (const auto& child : entry.children)
    children.push_back(&child);
  std::sort(children.begin(), children.end(),
            [](const auto* a, const auto* b) { return a->virtualName < b->virtualName; });
  for (const File::FSTEntry* child : children)
  {
    NandEntry out;
    out.path = prefix.empty() ? child->virtualName : prefix + "/" + child->virtualName;
    out.directory = child->isDirectory;
    if (!out.directory)
    {
      std::string data;
      if (!File::ReadFileToString(child->physicalName, data))
        *ok = false;
      out.data.assign(data.begin(), data.end());
    }
    const std::string path = out.path;
    entries->push_back(std::move(out));
    if (child->isDirectory)
      Collect(*child, path, entries, ok);
  }
}

bool SafeRelative(const std::string& path)
{
  if (path.empty() || path.size() > 1024 || path.front() == '/' || path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos)
  {
    return false;
  }
  size_t start = 0;
  while (start <= path.size())
  {
    const size_t end = std::min(path.find('/', start), path.size());
    const std::string_view part(path.data() + start, end - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    start = end + 1;
  }
  return true;
}
}  // namespace

bool ReadNandTree(const std::string& root, std::vector<NandEntry>* entries)
{
  entries->clear();
  if (!File::IsDirectory(root))
    return false;
  bool ok = true;
  Collect(File::ScanDirectoryTree(root, true), "", entries, &ok);
  return ok;
}

namespace
{
// The OS's error text, for logging a failed NAND replacement step.
std::string LastOsError()
{
#ifdef _WIN32
  return Common::GetLastErrorString();
#else
  return Common::LastStrerrorString();
#endif
}
}  // namespace

namespace
{
// Whether the file at `path` already holds exactly `data`.
bool SameFile(const std::string& path, const std::vector<u8>& data)
{
  if (!File::IsFile(path) || File::GetSize(path) != data.size())
    return false;
  File::IOFile file(path, "rb");
  std::vector<u8> have(data.size());
  return file && file.ReadBytes(have.data(), have.size()) && have == data;
}

// Removes what `entries` doesn't hold under `tree`, and anything held there as the other kind (a
// file where a folder belongs). A name differing only in case is removed too, before the right one
// is written, so a case-insensitive disk ends with the replacement's spelling.
bool RemoveExtras(const File::FSTEntry& tree, const std::string& prefix,
                  const std::map<std::string, bool>& wanted)
{
  for (const File::FSTEntry& child : tree.children)
  {
    const std::string path = prefix.empty() ? child.virtualName : prefix + "/" + child.virtualName;
    const auto it = wanted.find(path);
    if (it != wanted.end() && it->second == child.isDirectory)
    {
      if (child.isDirectory && !RemoveExtras(child, path, wanted))
        return false;
      continue;
    }
    const bool removed = child.isDirectory ? File::DeleteDirRecursively(child.physicalName) :
                                             File::Delete(child.physicalName);
    if (!removed)
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't remove {}: {}", child.physicalName,
                    LastOsError());
      return false;
    }
  }
  return true;
}
}  // namespace

bool ReplaceNandTree(const std::string& root, const std::vector<NandEntry>& entries)
{
  if (root.empty())
    return false;
  std::map<std::string, bool> wanted;
  for (const NandEntry& entry : entries)
  {
    if (!SafeRelative(entry.path))
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: the keyframe's NAND has an unsafe path");
      return false;
    }
    wanted[entry.path] = entry.directory;
  }
  if (!File::CreateFullPath(root + "/"))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't create {}: {}", root, LastOsError());
    return false;
  }
  // Only what differs is written. A restore usually changes a few save files of a tree that holds
  // Brawl's 39 MB of boot files, and on Windows every file written is scanned again.
  if (!RemoveExtras(File::ScanDirectoryTree(root, true), "", wanted))
    return false;
  for (const NandEntry& entry : entries)
  {
    const std::string path = root + "/" + entry.path;
    if (entry.directory)
    {
      if (!File::CreateFullPath(path + "/"))
      {
        ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't create {}: {}", path, LastOsError());
        return false;
      }
      continue;
    }
    if (SameFile(path, entry.data))
      continue;
    File::CreateFullPath(path);
    File::IOFile file(path, "wb");
    if (!file || !file.WriteBytes(entry.data.data(), entry.data.size()))
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't write {}: {}", path, LastOsError());
      return false;
    }
  }
  return true;
}

namespace
{
size_t ReplayMetadataBytes(const ReplayFrame& frame)
{
  size_t bytes = sizeof(std::vector<Events::PortInfo>) + 96;
  for (const auto& port : *frame.ports)
    bytes += sizeof(Events::PortInfo) + 96 + port.name.size() + port.controls.size() + port.queue.size();
  return bytes;
}

bool ChargeMetadata(const ReplayFrame& frame, const ReplayFrame* previous, size_t* budget)
{
  if (previous && *previous->ports == *frame.ports)
    return true;
  const size_t bytes = ReplayMetadataBytes(frame);
  if (bytes > *budget)
    return false;
  *budget -= bytes;
  return true;
}

bool WriteReplayFrame(Writer& w, const ReplayFrame& frame)
{
  if (!frame.ports || frame.ports->size() > MAX_SEATS)
    return false;
  u32 seen = 0;
  for (const auto& port : *frame.ports)
  {
    if (port.port < 0 || port.port >= MAX_SEATS || (seen & (1u << port.port)) ||
        port.name.size() > 128 || port.controls.size() > MAX_CONTROLS || port.queue.size() > MAX_QUEUE)
      return false;
    seen |= 1u << port.port;
  }
  if (frame.header && (frame.header->mode > 2 || frame.header->ruleset > 2 ||
                       frame.header->coin > 1 || (frame.header->flags & ~3u)))
    return false;
  if (frame.menu_exit != 0 && !ValidMenuExit(frame.menu_exit))
    return false;
  w.Bytes(frame.pads.data(), sizeof(frame.pads));
  w.U32(frame.ports ? static_cast<u32>(frame.ports->size()) : 0);
  if (frame.ports)
  {
    for (const auto& port : *frame.ports)
    {
      w.U32(port.port);
      w.U32(static_cast<u32>(port.name.size()));
      w.Bytes(port.name.data(), port.name.size());
      w.Blob(port.controls);
      w.Blob(port.queue);
    }
  }
  w.U32(frame.header.has_value());
  if (frame.header)
  {
    w.U32(frame.header->mode);
    w.U32(frame.header->ruleset);
    w.U32(frame.header->coin);
    w.U32(frame.header->flags);
    w.U32(frame.header->room);
  }
  w.U32(frame.clear_ready);
  w.U32(frame.menu_exit);
  w.U32(frame.seed);
  return true;
}

bool ReadReplayFrame(Reader& r, ReplayFrame* frame)
{
  u32 count;
  if (!r.Bytes(frame->pads.data(), sizeof(frame->pads)) || !r.U32(&count) || count > MAX_SEATS)
    return false;
  auto ports = std::make_shared<std::vector<Events::PortInfo>>();
  ports->reserve(count);
  u32 seen = 0;
  for (u32 i = 0; i < count; ++i)
  {
    Events::PortInfo port;
    u32 seat, length;
    if (!r.U32(&seat) || seat >= MAX_SEATS || (seen & (1u << seat)) || !r.U32(&length) || length > 128)
      return false;
    seen |= 1u << seat;
    port.port = static_cast<int>(seat);
    port.remote = true;
    port.name.resize(length);
    if (!r.Bytes(port.name.data(), length) || !r.Blob(&port.controls, MAX_CONTROLS) ||
        !r.Blob(&port.queue, MAX_QUEUE))
      return false;
    ports->push_back(std::move(port));
  }
  frame->ports = std::move(ports);
  u32 header, clear;
  if (!r.U32(&header) || header > 1)
    return false;
  if (header)
  {
    u32 mode, ruleset, coin, flags, room;
    if (!r.U32(&mode) || mode > 2 || !r.U32(&ruleset) || ruleset > 2 ||
        !r.U32(&coin) || coin > 1 || !r.U32(&flags) || (flags & ~3u) || !r.U32(&room))
      return false;
    frame->header = ReplayHeader{static_cast<u8>(mode), static_cast<u8>(ruleset),
                                static_cast<u8>(coin), static_cast<u8>(flags), room};
  }
  if (!r.U32(&clear) || clear > 1)
    return false;
  frame->clear_ready = clear != 0;
  u32 menu_exit;
  if (!r.U32(&menu_exit) || (menu_exit != 0 && !ValidMenuExit(menu_exit)))
    return false;
  frame->menu_exit = static_cast<u8>(menu_exit);
  u32 seed;
  if (!r.U32(&seed))
    return false;
  frame->seed = seed;
  return true;
}

// The smallest a frame's record can be: pads, a port count, the header and clear-ready flags, the
// menu exit, the seed.
constexpr size_t MIN_FRAME_BYTES = sizeof(Pads) + 5 * 4;
// Magic, the frame count, the first frame, the origin and target hashes.
constexpr size_t PREAMBLE_BYTES = 4 + 4 + 4 + 8 + 8;
}  // namespace

std::vector<u8> PackKeyframe(int frame, const ReplayArchive& replay)
{
  try
  {
    // The replay holds frames [first_frame, frame); only its first frame may leave the main menu.
    const int first = replay.first_frame;
    if (first < 0 || frame < first || static_cast<size_t>(frame) > MAX_REPLAY_FRAMES ||
        replay.frames.size() < static_cast<size_t>(frame - first))
      return {};
    const size_t count = static_cast<size_t>(frame - first);
    Writer w;
    w.Bytes(MAGIC, sizeof(MAGIC));
    w.U32(static_cast<u32>(frame));
    w.U32(static_cast<u32>(first));
    w.U64(replay.origin_hash);
    w.U64(replay.target_hash);
    size_t budget = MAX_RAW - (count + 1) * sizeof(ReplayFrame);
    const ReplayFrame* previous = nullptr;
    for (size_t i = 0; i < count; ++i)
    {
      if ((i > 0 && (replay.frames[i].menu_exit != 0 || replay.frames[i].seed != 0)) ||
          !WriteReplayFrame(w, replay.frames[i]))
        return {};
      if (!ChargeMetadata(replay.frames[i], previous, &budget))
        return {};
      previous = &replay.frames[i];
    }
    if ((count > 0 && (replay.boundary.menu_exit != 0 || replay.boundary.seed != 0)) ||
        !WriteReplayFrame(w, replay.boundary) ||
        !ChargeMetadata(replay.boundary, previous, &budget))
      return {};
    if (w.out.size() > MAX_RAW)
      return {};
    std::vector<u8> out(ZSTD_compressBound(w.out.size()));
    const size_t size = ZSTD_compress(out.data(), out.size(), w.out.data(), w.out.size(), LEVEL);
    if (ZSTD_isError(size))
      return {};
    out.resize(size);
    return out;
  }
  catch (const std::exception&)
  {
    return {};
  }
}

bool UnpackKeyframe(const std::vector<u8>& blob, int* frame, ReplayArchive* replay)
{
  try
  {
    const auto raw_size = ZSTD_getFrameContentSize(blob.data(), blob.size());
    if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN ||
        raw_size > MAX_RAW)
      return false;
    std::vector<u8> raw(raw_size);
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx)
      return false;
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, WINDOW_LOG);
    const size_t size = ZSTD_decompressDCtx(dctx, raw.data(), raw.size(), blob.data(), blob.size());
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(size) || size != raw.size())
      return false;
    Reader r(raw);
    char magic[4];
    u32 end, first;
    ReplayArchive decoded;
    if (raw.size() < PREAMBLE_BYTES || !r.Bytes(magic, sizeof(magic)) ||
        std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0 || !r.U32(&end) ||
        end > MAX_REPLAY_FRAMES || !r.U32(&first) || first > end ||
        !r.U64(&decoded.origin_hash) || !r.U64(&decoded.target_hash))
      return false;
    const u32 count = end - first;
    if (static_cast<u64>(count + 1) * MIN_FRAME_BYTES > raw.size() - PREAMBLE_BYTES)
      return false;
    decoded.first_frame = static_cast<int>(first);
    decoded.frames.reserve(count);
    size_t budget = MAX_RAW - (static_cast<size_t>(count) + 1) * sizeof(ReplayFrame);
    for (u32 i = 0; i < count; ++i)
    {
      ReplayFrame next;
      if (!ReadReplayFrame(r, &next) || (i > 0 && (next.menu_exit != 0 || next.seed != 0)))
        return false;
      if (!ChargeMetadata(next, decoded.frames.empty() ? nullptr : &decoded.frames.back(), &budget))
        return false;
      if (!decoded.frames.empty() && *decoded.frames.back().ports == *next.ports)
        next.ports = decoded.frames.back().ports;
      decoded.frames.push_back(std::move(next));
    }
    if (!ReadReplayFrame(r, &decoded.boundary) ||
        (count > 0 && (decoded.boundary.menu_exit != 0 || decoded.boundary.seed != 0)) ||
        !ChargeMetadata(decoded.boundary, decoded.frames.empty() ? nullptr : &decoded.frames.back(),
                        &budget) ||
        !r.AtEnd())
      return false;
    *frame = static_cast<int>(end);
    *replay = std::move(decoded);
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}

namespace
{
constexpr char STREAM_MAGIC[4] = {'Y', 'G', 'R', 'S'};
constexpr char CHUNK_MAGIC[4] = {'Y', 'G', 'R', 'C'};
constexpr u32 STREAM_VERSION = 1;
constexpr u32 MAX_STREAM_HEADER = 16u << 10;
// A chunk holds at most a minute of frames and 8 MB compressed.
constexpr u32 MAX_CHUNK_FRAMES = 3600;
constexpr u32 MAX_CHUNK_BYTES = 8u << 20;
// Magic, first frame, frame count, live frame, checksum count, payload size, then the XXH3 of all
// of those and the payload: every byte of a chunk is covered.
constexpr size_t CHUNK_FIELDS = 4 + 4 + 4 + 4 + 4 + 4;
constexpr size_t CHUNK_PREAMBLE = CHUNK_FIELDS + 8;

u64 ChunkHash(const u8* fields, const u8* payload, size_t size)
{
  XXH3_state_t* state = XXH3_createState();
  if (!state)
    return 0;
  XXH3_64bits_reset(state);
  XXH3_64bits_update(state, fields, CHUNK_FIELDS);
  XXH3_64bits_update(state, payload, size);
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}
constexpr size_t STREAM_PREAMBLE = 4 + 4 + 4;

void PutU32(std::vector<u8>* out, u32 v)
{
  const auto* p = reinterpret_cast<const u8*>(&v);
  out->insert(out->end(), p, p + sizeof(v));
}
void PutU64(std::vector<u8>* out, u64 v)
{
  const auto* p = reinterpret_cast<const u8*>(&v);
  out->insert(out->end(), p, p + sizeof(v));
}
u32 GetU32(const u8* p)
{
  u32 v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
u64 GetU64(const u8* p)
{
  u64 v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
}  // namespace

std::vector<u8> PackReplayStreamHeader(const ReplayStreamInfo& info)
{
  picojson::object o;
  o["v"] = picojson::value(static_cast<double>(STREAM_VERSION));
  o["compat"] = picojson::value(info.compat);
  o["build"] = picojson::value(info.build);
  o["game"] = picojson::value(info.game_id);
  o["origin_hash"] = picojson::value(fmt::format("{:016x}", info.origin_hash));
  o["first_frame"] = picojson::value(static_cast<double>(info.first_frame));
  o["segment"] = picojson::value(static_cast<double>(info.segment));
  o["room"] = picojson::value(info.room);
  const std::string json = picojson::value(o).serialize();
  std::vector<u8> out(STREAM_MAGIC, STREAM_MAGIC + sizeof(STREAM_MAGIC));
  PutU32(&out, STREAM_VERSION);
  PutU32(&out, static_cast<u32>(json.size()));
  out.insert(out.end(), json.begin(), json.end());
  return out;
}

std::vector<u8> PackReplayChunk(const ReplayChunk& chunk, bool history_start)
{
  try
  {
    if (chunk.first < 0 || chunk.frames.empty() || chunk.frames.size() > MAX_CHUNK_FRAMES ||
        static_cast<size_t>(chunk.first) + chunk.frames.size() > MAX_REPLAY_FRAMES)
      return {};
    Writer w;
    for (size_t i = 0; i < chunk.frames.size(); ++i)
    {
      const ReplayFrame& frame = chunk.frames[i];
      // A seed or a menu exit belongs only to a history's first frame (as in PackKeyframe).
      if (!(history_start && i == 0) && (frame.menu_exit != 0 || frame.seed != 0))
        return {};
      if (!WriteReplayFrame(w, frame))
        return {};
    }
    for (const auto& [frame, hash] : chunk.checksums)
    {
      w.U32(static_cast<u32>(frame));
      w.U64(hash);
    }
    std::vector<u8> payload(ZSTD_compressBound(w.out.size()));
    const size_t size =
        ZSTD_compress(payload.data(), payload.size(), w.out.data(), w.out.size(), LEVEL);
    if (ZSTD_isError(size) || size > MAX_CHUNK_BYTES)
      return {};
    payload.resize(size);
    std::vector<u8> out(CHUNK_MAGIC, CHUNK_MAGIC + sizeof(CHUNK_MAGIC));
    PutU32(&out, static_cast<u32>(chunk.first));
    PutU32(&out, static_cast<u32>(chunk.frames.size()));
    PutU32(&out, static_cast<u32>(std::max(chunk.live, 0)));
    PutU32(&out, static_cast<u32>(chunk.checksums.size()));
    PutU32(&out, static_cast<u32>(payload.size()));
    PutU64(&out, ChunkHash(out.data(), payload.data(), payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
  }
  catch (const std::exception&)
  {
    return {};
  }
}

std::vector<u8> PackReplayStream(const ReplayStreamInfo& info, const ReplayArchive& replay,
                                 const std::map<int, u64>& checksums)
{
  std::vector<u8> out = PackReplayStreamHeader(info);
  const int last = replay.EndFrame();
  for (size_t start = 0; start < replay.frames.size(); start += MAX_CHUNK_FRAMES)
  {
    ReplayChunk chunk;
    chunk.first = replay.first_frame + static_cast<int>(start);
    chunk.live = last;
    const size_t end = std::min(start + MAX_CHUNK_FRAMES, replay.frames.size());
    chunk.frames.assign(replay.frames.begin() + static_cast<std::ptrdiff_t>(start),
                        replay.frames.begin() + static_cast<std::ptrdiff_t>(end));
    const int chunk_end = replay.first_frame + static_cast<int>(end);
    for (auto it = checksums.upper_bound(chunk.first); it != checksums.end() && it->first <= chunk_end;
         ++it)
    {
      chunk.checksums.emplace_back(it->first, it->second);
    }
    const std::vector<u8> bytes = PackReplayChunk(chunk, start == 0);
    if (bytes.empty())
      return {};
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out;
}

void ReplayStreamReader::Resume()
{
  m_pending.clear();
  m_resuming = true;
}

bool ReplayStreamReader::Feed(const u8* data, size_t size, std::string* error)
{
  if (m_failed)
  {
    *error = "the replay stream already failed";
    return false;
  }
  m_pending.insert(m_pending.end(), data, data + size);
  m_bytes += size;
  if (!Parse(error))
  {
    m_failed = true;
    return false;
  }
  return true;
}

bool ReplayStreamReader::Parse(std::string* error)
{
  size_t pos = 0;
  const auto fail = [error](const char* why) {
    *error = why;
    return false;
  };
  try
  {
    if (!m_info || m_resuming)
    {
      if (m_pending.size() < STREAM_PREAMBLE)
        return true;
      if (std::memcmp(m_pending.data(), STREAM_MAGIC, sizeof(STREAM_MAGIC)) != 0)
        return fail(m_info ? "the match's stream moved to another history" : "not a replay stream");
      const u32 version = GetU32(m_pending.data() + 4);
      const u32 size = GetU32(m_pending.data() + 8);
      if (version != STREAM_VERSION)
        return fail(m_info ? "the match's stream moved to another history" :
                             "a replay stream of another version");
      if (size > MAX_STREAM_HEADER)
        return fail("the replay's header is too large");
      if (m_pending.size() < STREAM_PREAMBLE + size)
        return true;
      pos = STREAM_PREAMBLE + size;
      picojson::value value;
      const std::string json(m_pending.begin() + STREAM_PREAMBLE,
                             m_pending.begin() + STREAM_PREAMBLE + size);
      if (!picojson::parse(value, json).empty() || !value.is<picojson::object>())
        return fail(m_info ? "the match's stream moved to another history" :
                             "the replay's header isn't valid");
      const picojson::object& o = value.get<picojson::object>();
      const auto text = [&o](const char* key) -> std::optional<std::string> {
        const auto it = o.find(key);
        if (it == o.end() || !it->second.is<std::string>())
          return std::nullopt;
        return it->second.get<std::string>();
      };
      const auto number = [&o](const char* key) -> std::optional<double> {
        const auto it = o.find(key);
        if (it == o.end() || !it->second.is<double>())
          return std::nullopt;
        return it->second.get<double>();
      };
      // Another build's stream is refused before anything else of it is read.
      if (!m_info && !m_compat.empty() && text("compat") != m_compat)
        return fail("the replay was recorded by another build");
      ReplayStreamInfo info;
      const auto compat = text("compat"), build = text("build"), game = text("game"),
                 origin = text("origin_hash"), room = text("room");
      const auto first = number("first_frame"), segment = number("segment");
      if (!compat || !build || !game || !origin || origin->size() != 16 || !first || *first < 0 ||
          *first > MAX_REPLAY_FRAMES || !segment || *segment < 0)
      {
        return fail(m_info ? "the match's stream moved to another history" :
                             "the replay's header is incomplete");
      }
      info.compat = *compat;
      info.build = *build;
      info.game_id = *game;
      info.room = room.value_or("");
      info.origin_hash = std::strtoull(origin->c_str(), nullptr, 16);
      info.first_frame = static_cast<int>(*first);
      info.segment = static_cast<u32>(*segment);
      if (m_info)
      {
        // The same history: everything but the writer's own segment count, which moves on when
        // a recorder drops back into its match through a keyframe (its history rebuilt from the
        // host's). That it is the same match is then proven frame by frame on the overlap.
        if (info.compat != m_info->compat || info.build != m_info->build ||
            info.game_id != m_info->game_id || info.origin_hash != m_info->origin_hash ||
            info.first_frame != m_info->first_frame || info.room != m_info->room)
        {
          return fail("the match's stream moved to another history");
        }
        ++m_resumes;
      }
      else
      {
        m_archive = {};
        m_archive.origin_hash = info.origin_hash;
        m_archive.first_frame = info.first_frame;
        m_info = std::move(info);
      }
      m_resuming = false;
      m_segment_end = m_archive.first_frame;
    }
    while (m_pending.size() - pos >= CHUNK_PREAMBLE)
    {
      const u8* p = m_pending.data() + pos;
      if (std::memcmp(p, CHUNK_MAGIC, sizeof(CHUNK_MAGIC)) != 0)
        return fail("the replay has a damaged chunk");
      const u32 first = GetU32(p + 4), count = GetU32(p + 8), live = GetU32(p + 12),
                checks = GetU32(p + 16), size = GetU32(p + 20);
      const u64 hash = GetU64(p + 24);
      if (count == 0 || count > MAX_CHUNK_FRAMES || size > MAX_CHUNK_BYTES ||
          checks > count + 1 || static_cast<u64>(first) + count > MAX_REPLAY_FRAMES)
        return fail("the replay has a damaged chunk");
      if (static_cast<int>(first) != m_segment_end)
        return fail("the replay's frames don't follow on");
      if (m_pending.size() - pos - CHUNK_PREAMBLE < size)
        break;  // the rest of this chunk hasn't arrived yet
      const u8* payload = p + CHUNK_PREAMBLE;
      if (ChunkHash(p, payload, size) != hash)
        return fail("the replay has a damaged chunk");
      const auto raw_size = ZSTD_getFrameContentSize(payload, size);
      if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN ||
          raw_size > MAX_RAW)
        return fail("the replay has a damaged chunk");
      std::vector<u8> raw(raw_size);
      ZSTD_DCtx* dctx = ZSTD_createDCtx();
      if (!dctx)
        return fail("out of memory");
      ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, WINDOW_LOG);
      const size_t got = ZSTD_decompressDCtx(dctx, raw.data(), raw.size(), payload, size);
      ZSTD_freeDCtx(dctx);
      if (ZSTD_isError(got) || got != raw.size())
        return fail("the replay has a damaged chunk");
      Reader r(raw);
      for (u32 i = 0; i < count; ++i)
      {
        ReplayFrame next;
        if (!ReadReplayFrame(r, &next))
          return fail("the replay has a damaged frame");
        const int frame = static_cast<int>(first + i);
        if (const ReplayFrame* previous = m_archive.At(frame))
        {
          if (next.pads != previous->pads || *next.ports != *previous->ports ||
              next.header != previous->header || next.clear_ready != previous->clear_ready ||
              next.menu_exit != previous->menu_exit || next.seed != previous->seed)
            return fail("the match's stream changed");
          continue;
        }
        if (frame != m_archive.first_frame && (next.menu_exit != 0 || next.seed != 0))
          return fail("the replay has a damaged frame");
        if (!ChargeMetadata(next, m_archive.frames.empty() ? nullptr : &m_archive.frames.back(),
                            &m_metadata_budget))
          return fail("the replay's player names and controls are too large");
        if (!m_archive.frames.empty() && *m_archive.frames.back().ports == *next.ports)
          next.ports = m_archive.frames.back().ports;
        m_archive.frames.push_back(std::move(next));
      }
      for (u32 i = 0; i < checks; ++i)
      {
        u32 frame;
        u64 value;
        if (!r.U32(&frame) || !r.U64(&value) || frame < first || frame > first + count)
          return fail("the replay has a damaged checksum");
        const auto [it, inserted] = m_checksums.emplace(static_cast<int>(frame), value);
        if (!inserted && it->second != value)
          return fail("the match's stream changed");
      }
      if (!r.AtEnd())
        return fail("the replay has a damaged chunk");
      pos += CHUNK_PREAMBLE + size;
      m_segment_end = static_cast<int>(first + count);
      ++m_chunks;
      if (live <= MAX_REPLAY_FRAMES)
        m_live = std::max(m_live, static_cast<int>(live));
    }
  }
  catch (const std::exception&)
  {
    return fail("the replay couldn't be read");
  }
  m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(pos));
  return true;
}


namespace
{
constexpr size_t GCM_IV = 12;
constexpr size_t GCM_TAG = 16;

std::string Aad(int frame)
{
  return fmt::format("orca-keyframe-{}", frame);
}

bool RandomBytes(u8* out, size_t size)
{
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  static constexpr char personal[] = "orca-keyframe";
  const bool ok =
      mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                            reinterpret_cast<const unsigned char*>(personal), sizeof(personal)) ==
          0 &&
      mbedtls_ctr_drbg_random(&drbg, out, size) == 0;
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}

bool KeyFromHex(const std::string& hex, u8 key[32])
{
  if (hex.size() != 64)
    return false;
  for (size_t i = 0; i < 32; ++i)
  {
    unsigned value = 0;
    for (size_t k = 0; k < 2; ++k)
    {
      const char c = hex[i * 2 + k];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0)
        return false;
      value = value * 16 + static_cast<unsigned>(digit);
    }
    key[i] = static_cast<u8>(value);
  }
  return true;
}
}  // namespace

bool EncryptKeyframe(int frame, std::vector<u8>* blob, std::string* key_hex)
{
  u8 key[32], iv[GCM_IV];
  if (!RandomBytes(key, sizeof(key)) || !RandomBytes(iv, sizeof(iv)))
    return false;
  std::vector<u8> out(GCM_IV + blob->size() + GCM_TAG);
  std::copy(iv, iv + GCM_IV, out.begin());
  const std::string aad = Aad(frame);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok =
      mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
      mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, blob->size(), iv, GCM_IV,
                                reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
                                blob->data(), out.data() + GCM_IV, GCM_TAG,
                                out.data() + GCM_IV + blob->size()) == 0;
  mbedtls_gcm_free(&gcm);
  if (!ok)
    return false;
  key_hex->clear();
  for (u8 b : key)
    *key_hex += fmt::format("{:02x}", b);
  *blob = std::move(out);
  return true;
}

bool DecryptKeyframe(int frame, const std::string& key_hex, std::vector<u8>* blob)
{
  u8 key[32];
  if (!KeyFromHex(key_hex, key) || blob->size() < GCM_IV + GCM_TAG)
    return false;
  const size_t length = blob->size() - GCM_IV - GCM_TAG;
  std::vector<u8> out(length);
  const std::string aad = Aad(frame);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok =
      mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
      mbedtls_gcm_auth_decrypt(&gcm, length, blob->data(), GCM_IV,
                               reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
                               blob->data() + GCM_IV + length, GCM_TAG, blob->data() + GCM_IV,
                               out.data()) == 0;
  mbedtls_gcm_free(&gcm);
  if (!ok)
    return false;
  *blob = std::move(out);
  return true;
}

u64 NandHash(const std::vector<u8>& data)
{
  return XXH3_64bits(data.data(), data.size());
}

std::map<std::string, u64> HashNandTree(const std::string& root)
{
  std::map<std::string, u64> hashes;
  std::vector<NandEntry> entries;
  if (!ReadNandTree(root, &entries))
    return hashes;
  for (const NandEntry& entry : entries)
  {
    if (!entry.directory)
      hashes[entry.path] = NandHash(entry.data);
  }
  return hashes;
}

bool ResolveNandReferences(const std::string& root, std::vector<NandEntry>* entries,
                           std::string* error)
{
  for (NandEntry& entry : *entries)
  {
    if (!entry.reference)
      continue;
    std::string data;
    if (!File::ReadFileToString(root + "/" + entry.path, data))
    {
      *error = entry.path + " is missing";
      return false;
    }
    entry.data.assign(data.begin(), data.end());
    if (NandHash(entry.data) != entry.hash)
    {
      *error = entry.path + " differs";
      return false;
    }
    entry.reference = false;
  }
  return true;
}

std::string KeyframeHash(const std::vector<u8>& blob)
{
  return fmt::format("{:016x}", XXH3_64bits(blob.data(), blob.size()));
}

bool OpenKeyframe(const KeyframeInfo& info, std::vector<u8> blob, int* frame, ReplayArchive* replay,
                  std::string* error)
{
  if (blob.size() != info.size || KeyframeHash(blob) != info.hash ||
      !DecryptKeyframe(info.frame, info.key, &blob))
  {
    *error = "the keyframe arrived damaged";
    return false;
  }
  if (!UnpackKeyframe(blob, frame, replay) || *frame != info.frame)
  {
    *error = "the keyframe doesn't unpack";
    return false;
  }
  return true;
}

namespace
{
// LastRefusal, per thread: the job and the download threads each read their own.
thread_local std::string t_last_refusal;

// A folder shared by two Orcas on one machine (tests): the host writes <id>.okf, the joiner reads
// it.
class LocalKeyframeStore final : public KeyframeStore
{
public:
  explicit LocalKeyframeStore(std::string dir) : m_dir(std::move(dir))
  {
    File::CreateFullPath(m_dir + "/");
  }
  bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) override
  {
    if (!ValidReplayId(info.id) || data.size() > MAX_KEYFRAME_BYTES || data.size() != info.size)
    {
      *error = "invalid replay offer";
      return false;
    }
    const std::string path = Path(info.id);
    const std::string temp = path + ".part";
    File::IOFile file(temp, "wb");
    if (!file || !file.WriteBytes(data.data(), data.size()) || !file.Close() ||
        !File::Rename(temp, path))
    {
      *error = "could not write " + path;
      return false;
    }
    return true;
  }
  std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                     const std::function<bool(u64, u64)>& progress,
                                     std::string* error) override
  {
    if (!ValidReplayId(info.id) || info.size == 0 || info.size > MAX_KEYFRAME_BYTES)
    {
      *error = "invalid replay offer";
      return std::nullopt;
    }
    File::IOFile file(Path(info.id), "rb");
    if (!file || file.GetSize() != info.size)
    {
      *error = "no keyframe " + info.id;
      return std::nullopt;
    }
    std::vector<u8> data(info.size);
    constexpr u64 CHUNK = 4 << 20;
    for (u64 done = 0; done < info.size;)
    {
      const u64 n = std::min(CHUNK, info.size - done);
      if (!file.ReadBytes(data.data() + done, n))
      {
        *error = "could not read keyframe " + info.id;
        return std::nullopt;
      }
      done += n;
      if (!progress(done, info.size))
      {
        *error = "cancelled";
        return std::nullopt;
      }
    }
    return data;
  }
  void Delete(const std::string& id) override
  {
    if (ValidReplayId(id) && Orca::GetEnv("ORCA_TEST_KEEP_KEYFRAMES") != "1")
      File::Delete(Path(id));
  }

private:
  std::string Path(const std::string& id) const { return m_dir + "/" + id + ".okf"; }
  std::string m_dir;
};

// YouGame's keyframe endpoint: PUT, GET and DELETE on <store_url>/<id> with "Authorization: Ticket
// <room ticket>". A 401 with code "ticket", or a 403 "signed_out" (the player may have signed in
// since), retries with a new ticket; network errors, 429 and 5xx retry with backoff; other refusals
// are final. A 409 "exists" on PUT means an earlier attempt landed but its reply was lost.
class HttpKeyframeStore final : public KeyframeStore
{
public:
  explicit HttpKeyframeStore(TicketSource tickets) : m_tickets(std::move(tickets)) {}
  ~HttpKeyframeStore() override
  {
    std::lock_guard lock(m_deletes_mutex);
    for (std::thread& t : m_deletes)
      t.join();
  }

  bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) override
  {
    if (!ValidReplayId(info.id) || data.size() > MAX_KEYFRAME_BYTES || data.size() != info.size)
    {
      *error = "invalid replay offer";
      return false;
    }
    return WithRetries(error, [&](const std::string& ticket, const std::string& base,
                                  Result* result) {
      *result = Request("PUT", base + "/" + info.id,
                        {"Authorization: Ticket " + ticket,
                         "Content-Type: application/octet-stream",
                         fmt::format("X-Orca-Frame: {}", info.frame), "X-Orca-Hash: " + info.hash},
                        &data, nullptr, nullptr);
      if (result->status == 409 && result->code == "exists")
        result->status = 200;
    });
  }

  std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                     const std::function<bool(u64, u64)>& progress,
                                     std::string* error) override
  {
    if (!ValidReplayId(info.id) || info.size == 0 || info.size > MAX_KEYFRAME_BYTES)
    {
      *error = fmt::format("a keyframe of {} bytes", info.size);
      return std::nullopt;
    }
    std::vector<u8> body;
    const bool ok = WithRetries(error, [&](const std::string& ticket, const std::string& base,
                                           Result* result) {
      body.clear();
      body.reserve(info.size);
      *result = Request("GET", base + "/" + info.id, {"Authorization: Ticket " + ticket}, nullptr,
                        &body, [&](u64 done, u64) { return progress(done, info.size); });
    });
    if (!ok)
      return std::nullopt;
    if (body.size() != info.size)
    {
      *error = fmt::format("the keyframe arrived with {} bytes of {}", body.size(), info.size);
      return std::nullopt;
    }
    return body;
  }

  void Delete(const std::string& id) override
  {
    if (!ValidReplayId(id))
      return;
    // Best effort, off the caller's thread (the CPU thread calls this right after a load); the
    // site's cleanup catches anything missed. Copy the ticket now, since the room may be gone when
    // the thread runs.
    std::string ticket, base, error;
    if (!m_tickets || !m_tickets(false, &ticket, &base, &error))
    {
      WARN_LOG_FMT(NETPLAY, "Orca keyframe: can't delete {}: {}", id, error);
      return;
    }
    std::lock_guard lock(m_deletes_mutex);
    m_deletes.emplace_back([ticket, base, id] {
      for (int attempt = 0; attempt < 3; ++attempt)
      {
        const Result result = Request("DELETE", base + "/" + id, {"Authorization: Ticket " + ticket},
                                      nullptr, nullptr, nullptr, nullptr);
        if ((result.status >= 200 && result.status < 300) || result.status == 404)
          return;
        if (result.status != 0 && result.status != 429 && result.status < 500)
        {
          WARN_LOG_FMT(NETPLAY, "Orca keyframe: deleting {} refused (HTTP {})", id, result.status);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500 << attempt));
      }
    });
  }

private:
  void Cancel() override { m_cancel = true; }

  struct Result
  {
    long status = 0;  // 0: no HTTP response
    std::string error;
    std::string code;  // YouGame's {"code"} in a refusal
    bool final = false;  // not worth retrying
  };

  static constexpr int ATTEMPTS = 4;

  bool WithRetries(std::string* error,
                   const std::function<void(const std::string&, const std::string&, Result*)>& op)
  {
    t_last_refusal.clear();
    bool fresh = false;
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt)
    {
      if (attempt > 0 && !fresh)
      {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500 << (attempt - 1));
        while (!m_cancel && std::chrono::steady_clock::now() < until)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (m_cancel)
      {
        *error = "cancelled";
        return false;
      }
      std::string ticket, base, ticket_error;
      if (!m_tickets || !m_tickets(fresh, &ticket, &base, &ticket_error))
      {
        *error = "no ticket: " + ticket_error;
        return false;
      }
      Result result;
      op(ticket, base, &result);
      if (result.status >= 200 && result.status < 300)
      {
        error->clear();
        return true;
      }
      *error = result.status ? fmt::format("HTTP {}{}{}", result.status,
                                           result.error.empty() ? "" : ": ", result.error) :
                               result.error;
      // Expired or unknown ticket, or one minted before the player signed in: retry once with a
      // new one.
      if (((result.status == 401 && result.code == "ticket") ||
           (result.status == 403 && result.code == "signed_out")) &&
          !fresh)
      {
        fresh = true;
        continue;
      }
      fresh = false;
      // Refusals (wrong room, too large, gone, room full) are final; 429, 5xx and network errors
      // retry.
      const bool retry = !result.final && (result.status == 0 || result.status == 429 ||
                                           result.status >= 500);
      if (!retry)
      {
        // Only YouGame's own refusals carry a code; a bare 4xx (an edge page) may pass next time.
        if (result.status >= 400)
          t_last_refusal = result.code;
        return false;
      }
      WARN_LOG_FMT(NETPLAY, "Orca keyframe: {}; trying again", *error);
    }
    return false;
  }

  struct Transfer
  {
    CURL* curl = nullptr;
    const std::vector<u8>* upload = nullptr;
    size_t sent = 0;
    std::vector<u8>* download = nullptr;
    std::vector<u8>* error_body = nullptr;
    u64 limit = 0;
    std::function<bool(u64, u64)> progress;
    const std::atomic<bool>* cancel = nullptr;
  };

  static int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
  {
    const auto* t = static_cast<Transfer*>(user);
    return t->cancel && t->cancel->load() ? 1 : 0;
  }

  static size_t OnRead(char* buffer, size_t size, size_t count, void* user)
  {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = std::min(size * count, t->upload->size() - t->sent);
    std::memcpy(buffer, t->upload->data() + t->sent, n);
    t->sent += n;
    return n;
  }

  static size_t OnWrite(char* data, size_t size, size_t count, void* user)
  {
    try
    {
      auto* t = static_cast<Transfer*>(user);
      const size_t n = size * count;
      // A refusal's body is YouGame's {error, code}, not the keyframe.
      long status = 0;
      curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
      if (status < 200 || status >= 300 || !t->download)
      {
        if (t->error_body->size() + n > 64 * 1024)
          return 0;
        t->error_body->insert(t->error_body->end(), data, data + n);
        return n;
    }
    if (t->download->size() + n > t->limit)
      return 0;  // abort: larger than any keyframe
    t->download->insert(t->download->end(), data, data + n);
    if (t->progress && !t->progress(t->download->size(), 0))
      return 0;
    return n;
    }
    catch (const std::exception&)
    {
      return 0;
    }
  }

  Result Request(const char* method, const std::string& url,
                 const std::vector<std::string>& headers, const std::vector<u8>* upload,
                 std::vector<u8>* download, std::function<bool(u64, u64)> progress) const
  {
    return Request(method, url, headers, upload, download, std::move(progress), &m_cancel);
  }

  static Result Request(const char* method, const std::string& url,
                        const std::vector<std::string>& headers, const std::vector<u8>* upload,
                        std::vector<u8>* download, std::function<bool(u64, u64)> progress,
                        const std::atomic<bool>* cancel)
  {
    Result result;
    CURL* curl = curl_easy_init();
    if (!curl)
    {
      result.error = "no HTTP client";
      return result;
    }
    curl_slist* list = nullptr;
    for (const std::string& h : headers)
      list = curl_slist_append(list, h.c_str());
    std::vector<u8> error_body;
    Transfer t;
    t.curl = curl;
    t.upload = upload;
    t.download = download;
    t.error_body = &error_body;
    t.limit = MAX_KEYFRAME_BYTES;
    t.progress = std::move(progress);
    t.cancel = cancel;
    Common::ConfigureCurlTLS(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    // A transfer may take minutes on a slow link, but must not stall for 20 s.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    if (std::string_view(method) == "PUT")
    {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(curl, CURLOPT_READFUNCTION, &OnRead);
      curl_easy_setopt(curl, CURLOPT_READDATA, &t);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                       static_cast<curl_off_t>(upload ? upload->size() : 0));
    }
    else if (std::string_view(method) != "GET")
    {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK)
    {
      result.error = curl_easy_strerror(code);
      // Aborts (the player gave up, emulation stopping, an oversized body) are not worth retrying.
      result.final = code == CURLE_WRITE_ERROR || code == CURLE_ABORTED_BY_CALLBACK ||
                     (cancel && cancel->load());
      if (result.status >= 200 && result.status < 300)
        result.status = 0;
    }
    else if (!(result.status >= 200 && result.status < 300) && !error_body.empty())
    {
      // YouGame's refusals are {"error": "...", "code": "..."}.
      picojson::value json;
      const std::string text(error_body.begin(), error_body.end());
      if (picojson::parse(json, text).empty() && json.is<picojson::object>())
      {
        const auto& o = json.get<picojson::object>();
        if (const auto it = o.find("error"); it != o.end() && it->second.is<std::string>())
          result.error = it->second.get<std::string>().substr(0, 200);
        if (const auto it = o.find("code"); it != o.end() && it->second.is<std::string>())
          result.code = it->second.get<std::string>().substr(0, 32);
      }
    }
    return result;
  }

  TicketSource m_tickets;
  std::atomic<bool> m_cancel{false};
  std::mutex m_deletes_mutex;
  std::vector<std::thread> m_deletes;
};
}  // namespace

std::unique_ptr<KeyframeStore> MakeHttpKeyframeStore(TicketSource tickets)
{
  return std::make_unique<HttpKeyframeStore>(std::move(tickets));
}

std::unique_ptr<KeyframeStore> MakeKeyframeStore(TicketSource tickets)
{
  const std::string dir = Orca::GetEnv("ORCA_TEST_KEYFRAME_DIR");
  if (!dir.empty())
    return std::make_unique<LocalKeyframeStore>(dir);
  return MakeHttpKeyframeStore(std::move(tickets));
}

std::string LastRefusal()
{
  return t_last_refusal;
}
}  // namespace Orca::Net
