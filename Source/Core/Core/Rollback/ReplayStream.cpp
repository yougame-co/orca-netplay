// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ReplayStream.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/Version.h"
#include "Common/WebSocket.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/SafeJson.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Chat.h"
#include "Core/System.h"

namespace Rollback::ReplayStream
{
namespace
{
using Clock = std::chrono::steady_clock;

std::mutex s_control_mutex;
std::optional<bool> s_pause;
std::optional<int> s_seek;
std::optional<NameMap> s_names;

double MsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

u64 MemoryChecksum(Core::System& system)
{
  auto& memory = system.GetMemory();
  return RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                     {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
}
}  // namespace

bool ProductMode()
{
  return !Orca::GetEnv("ORCA_REPLAY_SOURCE").empty();
}

namespace
{
struct WatchOrigin
{
  std::string scheme;
  std::string host;
  unsigned port;
};

// Deliberately accept only the DNS/IPv4 authorities used by rooms. Reject ambiguous encodings
// before passing the original URL to the socket library.
std::optional<WatchOrigin> ParseWatchOrigin(std::string_view url, bool origin_only)
{
  if (url.find_first_of("\\@#") != std::string_view::npos ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
    return std::nullopt;
  const size_t split = url.find("://");
  if (split == std::string_view::npos)
    return std::nullopt;
  const std::string scheme(url.substr(0, split));
  if (scheme != "wss" && scheme != "ws" && scheme != "https" && scheme != "http")
    return std::nullopt;
  url.remove_prefix(split + 3);
  const size_t end = url.find_first_of("/?");
  if (origin_only && end != std::string_view::npos && url.substr(end) != "/")
    return std::nullopt;
  const auto authority = url.substr(0, end);
  const size_t colon = authority.find(':');
  std::string host(authority.substr(0, colon));
  if (host.empty() || host.front() == '.' || host.back() == '.' ||
      std::any_of(host.begin(), host.end(), [](char c) {
        return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-');
      }))
    return std::nullopt;
  for (char& c : host)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  unsigned port = scheme == "wss" || scheme == "https" ? 443 : 80;
  if (colon != std::string_view::npos)
  {
    const auto field = authority.substr(colon + 1);
    const auto [last, ec] = std::from_chars(field.data(), field.data() + field.size(), port);
    if (ec != std::errc{} || last != field.data() + field.size() || port == 0 || port > 65535)
      return std::nullopt;
  }
  return WatchOrigin{scheme, std::move(host), port};
}
}  // namespace

bool WatchSourceAllowed(std::string_view source, std::string_view rooms_origin, bool test_knobs,
                        std::string* error)
{
  const auto target = ParseWatchOrigin(source, false);
  const auto rooms = ParseWatchOrigin(rooms_origin, true);
  const auto local_test = [test_knobs](const WatchOrigin& origin) {
    return test_knobs && (origin.host == "localhost" || origin.host == "127.0.0.1");
  };
  if (!target || (target->scheme != "wss" &&
                  !(target->scheme == "ws" && local_test(*target))))
    *error = "the replay watch source must use an approved wss URL";
  else if (!rooms || (rooms->scheme != "https" && rooms->scheme != "wss" &&
                     !((rooms->scheme == "http" || rooms->scheme == "ws") && local_test(*rooms))))
    *error = "the replay rooms origin is missing or invalid";
  else if (target->host != rooms->host)
    *error = "the replay watch host does not match the rooms origin";
  else if (target->port != (target->scheme == "wss" ? 443u : 80u) &&
           target->port != rooms->port)
    *error = "the replay watch port does not match the rooms origin";
  else
    return true;
  return false;
}

std::unique_ptr<File::IOFile> OpenReplayFile(const std::string& path, std::string* error)
{
#ifdef _WIN32
  auto file = std::make_unique<File::IOFile>(path, "rb", File::SharedAccess::ReadWrite);
  if (!*file)
  {
    *error = "the replay file can't be opened";
    return nullptr;
  }
  struct _stat64 info;
  if (_fstat64(_fileno(file->GetHandle()), &info) != 0 ||
      (info.st_mode & _S_IFMT) != _S_IFREG)
#else
  // A FIFO must not block before its descriptor can be checked.
  const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
  {
    *error = "the replay file can't be opened";
    return nullptr;
  }
  Common::ScopeGuard close_fd([fd] { close(fd); });
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
#endif
  {
    *error = "the replay source must be a regular file";
    return nullptr;
  }
#ifndef _WIN32
  std::FILE* handle = fdopen(fd, "rb");
  if (!handle)
  {
    *error = "the replay file can't be opened";
    return nullptr;
  }
  close_fd.Dismiss();
  auto file = std::make_unique<File::IOFile>(handle);
#endif
  return file;
}

bool WatchPending::Append(std::span<const u8> incoming)
{
  if (incoming.size() > WATCH_PENDING_MAX - bytes.size() - resets.size() * sizeof(size_t))
    return false;
  bytes.insert(bytes.end(), incoming.begin(), incoming.end());
  return true;
}

bool WatchPending::Reset()
{
  if (WATCH_PENDING_MAX - bytes.size() - resets.size() * sizeof(size_t) < sizeof(size_t))
    return false;
  resets.push_back(bytes.size());
  return true;
}

std::optional<NameMap> ParseNames(std::string_view fields)
{
  if (fields.size() > 4 * (Orca::UX::Chat::NAME_ENCODED + 3))
    return std::nullopt;
  std::istringstream in{std::string(fields)};
  NameMap names;
  for (std::string port, encoded; in >> port;)
  {
    if (port.size() != 1 || port[0] < '0' || port[0] > '3' || !(in >> encoded))
      return std::nullopt;
    auto name = Orca::UX::Chat::DecodeField(encoded, Orca::UX::Chat::NAME_ENCODED,
                                          Orca::UX::Chat::NAME_POINTS);
    if (!name || !names.emplace(port[0] - '0', std::move(*name)).second)
      return std::nullopt;
  }
  return names;
}

std::vector<Orca::Events::PortInfo> ViewPorts(const std::vector<Orca::Events::PortInfo>& ports,
                                          const NameMap& names)
{
  auto view = ports;
  for (auto& port : view)
  {
    const auto it = names.find(port.port);
    port.name = it != names.end() ? it->second : fmt::format("Player {}", port.port + 1);
  }
  return view;
}

bool Command(std::string_view line)
{
  if (!ProductMode())
    return false;
  std::istringstream in{std::string(line)};
  std::string prefix, verb, value, more;
  in >> prefix >> verb;
  if (prefix != "replay")
    return false;
  if ((verb == "pause" || verb == "resume") && !(in >> more))
  {
    std::lock_guard lock(s_control_mutex);
    s_pause = verb == "pause";
    return true;
  }
  if (verb == "seek" && (in >> value) && !(in >> more))
  {
    int frame = -1;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frame);
    if (error != std::errc{} || end != value.data() + value.size() || frame < 0)
      return false;
    std::lock_guard lock(s_control_mutex);
    s_seek = frame;
    return true;
  }
  if (verb == "names")
  {
    std::string fields;
    std::getline(in, fields);
    auto names = ParseNames(fields);
    if (!names)
      return false;
    std::lock_guard lock(s_control_mutex);
    s_names = std::move(*names);
    return true;
  }
  return false;
}

std::vector<std::vector<u8>> RepackedPieces(const std::vector<u8>& written, int* frames,
                                            size_t max_piece, bool* too_big)
{
  *frames = 0;
  if (too_big)
    *too_big = false;
  if (max_piece == 0 || written.empty())
    return {};
  // Our own bytes, but read back with the untrusted reader's bounds all the same.
  Orca::Net::ReplayStreamReader reader;
  std::string error;
  if (!reader.Feed(written.data(), written.size(), &error) || !reader.Info() ||
      reader.Archive().frames.empty())
  {
    WARN_LOG_FMT(ROLLBACK, "Replay: the written history doesn't read back ({}); no saved copy",
                 error.empty() ? "no frames" : error);
    return {};
  }
  const std::vector<u8> bytes =
      Orca::Net::PackReplayStream(*reader.Info(), reader.Archive(), reader.Checksums());
  if (bytes.empty() || bytes.size() > Orca::Net::REPLAY_PACKED_MAX)
  {
    if (too_big)
      *too_big = bytes.size() > Orca::Net::REPLAY_PACKED_MAX;
    WARN_LOG_FMT(ROLLBACK, "Replay: the saved copy packs to {} bytes, over 1 MiB or empty: skipped",
                 bytes.size());
    return {};
  }
  max_piece = std::min(max_piece, Orca::Net::REPLAY_PACKED_PIECE_MAX);
  std::vector<std::vector<u8>> pieces;
  pieces.reserve((bytes.size() + max_piece - 1) / max_piece);
  for (size_t at = 0; at < bytes.size(); at += max_piece)
  {
    const size_t last = std::min(at + max_piece, bytes.size());
    pieces.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                        bytes.begin() + static_cast<std::ptrdiff_t>(last));
  }
  *frames = static_cast<int>(reader.Archive().frames.size());
  return pieces;
}

// ---- PackedSender ----

PackedSender::PackedSender(Send send) : m_send(std::move(send))
{
}

PackedSender::~PackedSender()
{
  m_thread.Shutdown();
}

bool PackedSender::TooBig(u32 history) const
{
  return m_too_big.load() == history;
}

void PackedSender::Submit(std::vector<u8> written, u64 generation, u32 history)
{
  if (!m_started)
  {
    m_started = true;
    m_thread.Reset("Replay packer", [this](Job job) {
      // A newer copy is waiting: it covers this one.
      if (job.sequence != m_latest.load())
        return;
      int frames = 0;
      bool too_big = false;
      const auto pieces =
          RepackedPieces(job.written, &frames, Orca::Net::REPLAY_PACKED_PIECE_MAX, &too_big);
      if (too_big)
        m_too_big = job.history;
      if (!pieces.empty())
        m_send(pieces, frames, job.generation);
    });
  }
  const u64 sequence = m_latest.fetch_add(1) + 1;
  m_thread.Push(Job{std::move(written), generation, history, sequence});
}

// ---- Writer ----

struct Writer::Timing
{
  std::vector<double> calls;
  std::vector<double> chunks;
  std::vector<double> early;
  int next_log = 3600;
  int live = -1;
};

std::unique_ptr<Writer> Writer::ForMatch()
{
  std::string base;
  bool to_room = false;
  if (Orca::UX::TestKnobsAllowed())
  {
    base = Orca::GetEnv("ORCA_TEST_REPLAY_OUT");
    to_room = Orca::GetEnv("ORCA_TEST_REPLAY_ROOM") == "1";
  }
  // The product's writer: room only, while the room records (Online::RecordWanted).
  if (base.empty() && !to_room)
  {
    if (!Orca::SessionActive())
      return nullptr;
    return std::make_unique<Writer>(std::string(), true, true);
  }
  auto writer = std::make_unique<Writer>(std::move(base), to_room);
  if (Orca::GetEnv("ORCA_TEST_REPLAY_TIMING") == "1")
    writer->m_timing = std::make_unique<Timing>();
  return writer;
}

Writer::Writer(std::string base, bool to_room, bool gated)
    : m_base(std::move(base)), m_to_room(to_room), m_gated(gated)
{
}

// The next whole unit to the file and, once its stream has started there, to the room.
bool Writer::Emit(const std::vector<u8>& bytes)
{
  if (m_file && (!m_file->WriteBytes(bytes.data(), bytes.size()) || !m_file->Flush()))
    return false;
  if (m_to_room && m_room_started)
    Orca::Online::SendReplayStream(bytes);
  if (m_to_room && !m_written_over)
  {
    // ~60 KB a minute: past this, about 6 hours in, the copy wouldn't pack into 1 MiB anyway.
    constexpr size_t MAX_WRITTEN = 24u << 20;
    if (m_written.size() + bytes.size() > MAX_WRITTEN)
    {
      WARN_LOG_FMT(ROLLBACK, "Replay: history {} is past {} MiB: no more saved copies of it",
                   m_segment, MAX_WRITTEN >> 20);
      m_written_over = true;
      m_written = {};
    }
    else
    {
      m_written.insert(m_written.end(), bytes.begin(), bytes.end());
    }
  }
  return true;
}

// The room's stream starts: the header, then the frames written so far, a few hundred a message
// (a room message holds at most 64 KiB).
bool Writer::StartRoom(const Orca::Net::ReplayArchive& replay, int live)
{
  bool emitted = false;
  m_room_started = true;
  Orca::Online::SendReplayStream(m_header);
  constexpr int PER_MESSAGE = 600;
  for (int first = m_first; first < m_written_end; first += PER_MESSAGE)
  {
    Orca::Net::ReplayChunk chunk;
    chunk.first = first;
    chunk.live = live;
    const int last = std::min(first + PER_MESSAGE, m_written_end);
    for (int f = first; f < last; ++f)
    {
      Orca::Net::ReplayFrame frame = *replay.At(f);
      if (!frame.ports)
      {
        frame.ports = chunk.frames.empty() ?
                          std::make_shared<const std::vector<Orca::Events::PortInfo>>() :
                          chunk.frames.back().ports;
      }
      frame.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
          Orca::Net::RecordingPorts(*frame.ports));
      chunk.frames.push_back(std::move(frame));
    }
    for (auto it = m_room_checksums.upper_bound(first);
         it != m_room_checksums.end() && it->first <= last; ++it)
    {
      chunk.checksums.emplace_back(it->first, it->second);
    }
    const std::vector<u8> bytes = Orca::Net::PackReplayChunk(chunk, first == m_first);
    if (bytes.empty())
    {
      ERROR_LOG_FMT(ROLLBACK, "Replay: couldn't pack frames {}-{} for the room", first, last);
      m_to_room = false;
      return emitted;
    }
    Orca::Online::SendReplayStream(bytes);
    emitted = true;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Replay: streaming history {} to the room from frame {} ({} frames "
                           "already written)",
                 m_segment, m_first, m_written_end - m_first);
  return emitted;
}

void Writer::SendPacked()
{
  if (!m_to_room || !m_room_started || m_written_over || m_written_end <= m_first ||
      Orca::Online::RoomGeneration() != m_room_generation)
  {
    return;
  }
  if (m_gated && !Orca::Online::RecordWanted())
    return;
  // This history's copy already packed past 1 MiB: later ones only grow.
  if (m_packer && m_packer->TooBig(m_segment))
    return;
  if (!m_packer)
  {
    // A room reopened while this packed: its stream starts again and the next result's copy goes
    // there.
    m_packer = std::make_unique<PackedSender>(&Orca::Online::SendReplayPacked);
  }
  // A copy of what was written (a memcpy of ~60 KB a minute of play); the packing happens there.
  m_packer->Submit(m_written, m_room_generation, m_segment);
}

void Writer::LogTiming(bool final) const
{
  if (!m_timing)
    return;
  const auto describe = [](std::vector<double> calls) {
    std::sort(calls.begin(), calls.end());
    double total = 0;
    for (double us : calls)
      total += us;
    // Nearest-rank percentiles; an empty group has no cost.
    const auto percentile = [&calls](size_t percent) {
      return calls.empty() ? 0.0 : calls[(calls.size() * percent + 99) / 100 - 1];
    };
    return fmt::format("count={} total={:.3f} mean={:.3f} p50={:.3f} p99={:.3f} max={:.3f}",
                       calls.size(), total, calls.empty() ? 0.0 : total / calls.size(),
                       percentile(50), percentile(99), calls.empty() ? 0.0 : calls.back());
  };
  NOTICE_LOG_FMT(ROLLBACK,
                 "Replay: writer timing {} frame={} us: all [{}], chunk [{}], early [{}]",
                 final ? "final" : "periodic", m_timing->live, describe(m_timing->calls),
                 describe(m_timing->chunks), describe(m_timing->early));
}

Writer::~Writer()
{
  LogTiming(true);
}

bool Writer::Pump(const Orca::Net::ReplayArchive& replay, int end, int live,
                  const std::map<int, u64>* checksums, bool room_ready)
{
  // The product writer does nothing while this room doesn't record. The answer (the app's caps,
  // the player's choice, the room) changes rarely and takes three locks: asked twice a second.
  if (m_gated)
  {
    if (!room_ready)
      return true;
    if (m_record_checked < 0 || live < m_record_checked || live - m_record_checked >= 30)
    {
      m_record_wanted = Orca::Online::RecordWanted();
      m_record_checked = live;
    }
    if (!m_record_wanted)
      return true;
  }
  // The test timer covers every return, including calls with nothing settled yet. With it off,
  // no timing samples or clock reads are added to the writer's normal flush cadence.
  const auto started = m_timing ? Clock::now() : Clock::time_point{};
  bool emitted = false;
  Common::ScopeGuard timing([&] {
    if (!m_timing)
      return;
    const double us = std::chrono::duration<double, std::micro>(Clock::now() - started).count();
    m_timing->calls.push_back(us);
    (emitted ? m_timing->chunks : m_timing->early).push_back(us);
    m_timing->live = live;
    if (live >= m_timing->next_log)
    {
      LogTiming(false);
      m_timing->next_log = (live / 3600 + 1) * 3600;
    }
  });
  if (replay.first_frame < 0 || replay.frames.empty())
    return true;
  end = std::min(end, replay.EndFrame());
  if (m_to_room)
  {
    const u64 generation = Orca::Online::RoomGeneration();
    if (generation != m_room_generation)
    {
      m_room_generation = generation;
      m_room_started = false;
    }
  }
  const u32 seed = replay.frames[0].seed;
  // Another history (a fresh start or a join: a new first frame, origin or seed, or an archive that
  // starts over shorter than what was written). Not a rollback: the session loads the newest
  // snapshot at or before the frame it guessed wrong, which on a machine that snapshots every few
  // frames (Windows: 3-11 ms each) can be before frames already written; they re-run with the same
  // inputs and `end` dips under m_written_end until the re-run passes it, with nothing new to write.
  if (!m_started || replay.first_frame != m_first || replay.origin_hash != m_origin ||
      seed != m_seed || replay.EndFrame() < m_written_end)
  {
    // Another history: a file of its own, from its first frame.
    ++m_segment;
    const std::string path =
        m_base.empty() ? std::string() : fmt::format("{}-{}.ygr", m_base, m_segment);
    // Others may read it as it grows (a live watcher of the file, on Windows too).
    m_file = path.empty() ? nullptr :
                            std::make_unique<File::IOFile>(path, "wb", File::SharedAccess::Read);
    Orca::Net::ReplayStreamInfo info;
    info.compat = Orca::Online::CompatibilityKey();
    info.build = Common::GetScmRevGitStr();
    if (const Orca::Profile* profile = Orca::ActiveProfile())
      info.game_id = profile->game_id;
    info.origin_hash = replay.origin_hash;
    info.first_frame = replay.first_frame;
    info.segment = m_segment;
    info.seat_labels = true;
    info.room = Orca::Online::Code();
    const std::vector<u8> header = Orca::Net::PackReplayStreamHeader(info);
    m_header = header;
    m_room_started = false;
    m_room_checksums.clear();
    m_written.clear();
    m_written_over = false;
    if ((m_file && !*m_file) || !Emit(header))
    {
      ERROR_LOG_FMT(ROLLBACK, "Replay: couldn't write {}", path);
      return false;
    }
    m_started = true;
    m_first = replay.first_frame;
    m_origin = replay.origin_hash;
    m_seed = seed;
    m_written_end = m_first;
    NOTICE_LOG_FMT(ROLLBACK, "Replay: writing history {} (from frame {}, origin {:016x}) to {}",
                   m_segment, m_first, m_origin, path);
    if (!m_gated)
      Orca::Status::Line(fmt::format("orca replay-out {}", path.empty() ? "room" : path));
  }
  if (m_to_room && room_ready && m_room_generation != 0 && !m_room_started)
    emitted = StartRoom(replay, live);
  const int pending = end - m_written_end;
  if (pending <= 0 || (pending < 6 && Clock::now() - m_flushed < std::chrono::milliseconds(100)))
    return true;
  Orca::Net::ReplayChunk chunk;
  chunk.first = m_written_end;
  chunk.live = live;
  const int count = std::min(pending, 3600);
  chunk.frames.reserve(static_cast<size_t>(count));
  for (int f = chunk.first; f < chunk.first + count; ++f)
  {
    Orca::Net::ReplayFrame frame = *replay.At(f);
    // A frame recorded before its hook ran (none should be final) takes the last ports.
    if (!frame.ports)
    {
      frame.ports = chunk.frames.empty() ?
                        std::make_shared<const std::vector<Orca::Events::PortInfo>>() :
                        chunk.frames.back().ports;
    }
    frame.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
        Orca::Net::RecordingPorts(*frame.ports));
    chunk.frames.push_back(std::move(frame));
  }
  if (checksums)
  {
    for (auto it = checksums->upper_bound(chunk.first);
         it != checksums->end() && it->first <= chunk.first + count; ++it)
    {
      chunk.checksums.emplace_back(it->first, it->second);
    }
  }
  const std::vector<u8> bytes = Orca::Net::PackReplayChunk(chunk, chunk.first == replay.first_frame);
  if (bytes.empty() || !Emit(bytes))
  {
    ERROR_LOG_FMT(ROLLBACK, "Replay: couldn't write frames {}-{}", chunk.first,
                  chunk.first + count);
    return false;
  }
  emitted = true;
  if (m_to_room)
  {
    for (const auto& [frame, hash] : chunk.checksums)
      m_room_checksums[frame] = hash;
  }
  m_written_end = chunk.first + count;
  m_flushed = Clock::now();
  return true;
}

// ---- Player ----

// A watch URL's stream, handed from its socket's thread to the player.
struct Player::Watch
{
  std::thread thread;
  std::atomic<bool> stop{false};
  std::mutex mutex;
  WatchPending pending;
  bool ended = false;
  bool fatal = false;
  std::string error;
};

Player::Player() = default;

Player::~Player()
{
  if (m_watch)
  {
    m_watch->stop = true;
    if (m_watch->thread.joinable())
      m_watch->thread.join();
  }
}

// The URL to watch on, with a ticket good now. A `watch:<room>` source gets a watch ticket from the
// site for the dev game, as the app's page would get one for a real listing, so every reconnect
// has a fresh one; a ws(s):// URL is used as given (its ticket expires).
static std::string WatchTarget(const std::string& source, std::string* error)
{
  if (!source.starts_with("watch:"))
    return source;
  std::string site = Orca::GetEnv("ORCA_SITE");
  if (site.empty())
    site = "https://yougame.co";
  while (site.ends_with('/'))
    site.pop_back();
  picojson::object request;
  request["dev"] = picojson::value(Orca::GetEnv("ORCA_TEST_DEV_GAME"));
  request["watch"] = picojson::value(source.substr(6));
  Common::HttpRequest http(std::chrono::seconds(10));
  const Common::HttpRequest::Response response =
      http.Post(site + "/api/multiplayer/ticket", picojson::value(request).serialize(),
                {{"Content-Type", "application/json"}}, Common::HttpRequest::AllowedReturnCodes::All);
  picojson::value json;
  if (!response ||
      !Orca::Json::ParseJson(std::string(response->begin(), response->end()), &json) ||
      !json.is<picojson::object>())
  {
    *error = fmt::format("no watch ticket (HTTP {})", http.GetLastResponseCode());
    return {};
  }
  const picojson::object& reply = json.get<picojson::object>();
  const auto str = [&reply](const char* key) {
    const auto it = reply.find(key);
    return it != reply.end() && it->second.is<std::string>() ? it->second.get<std::string>() :
                                                                std::string();
  };
  std::string url = str("url");
  const std::string ticket = str("ticket");
  const std::string path = str("watch");
  if (url.empty() || ticket.empty() || !path.starts_with("/watch/"))
  {
    *error = str("error").empty() ? std::string("no watch ticket") : str("error");
    return {};
  }
  if (url.starts_with("https://"))
    url = "wss://" + url.substr(8);
  else if (url.starts_with("http://"))
    url = "ws://" + url.substr(7);
  else
  {
    *error = "the watch ticket's room URL isn't http(s)";
    return {};
  }
  while (url.ends_with('/'))
    url.pop_back();
  return url + path + "?ticket=" + ticket;
}

void Player::WatchThread(std::string source)
{
  Watch& watch = *m_watch;
  const auto fail = [&watch](std::string why, bool fatal = false) {
    std::lock_guard lock(watch.mutex);
    if (watch.error.empty())
      watch.error = std::move(why);
    watch.ended = true;
    watch.fatal = fatal;
    if (fatal)
      watch.pending = {};
  };
  // ORCA_TEST_REPLAY_WATCH_DROP=<s>: the socket drops once, that long after it first connected, as
  // a network failure would (the reconnect test).
  const int drop_after = !m_product && Orca::UX::TestKnobsAllowed() ?
                            std::max(0, std::atoi(Orca::GetEnv("ORCA_TEST_REPLAY_WATCH_DROP").c_str())) :
                            0;
  bool dropped = false;
  // This segment's bytes taken so far. A connection starts the newest segment from its header,
  // so after a reconnect the first `received` bytes are ones already played: they are skipped,
  // and the first of them (`head`) must be the same, or it is another stream.
  u64 received = 0;
  std::vector<u8> head;
  int connections = 0;
  // Connections in a row that closed with nothing new (a finished stream sends its backlog and
  // closes): the stream is over after a few, rather than reconnecting forever.
  int fruitless = 0;
  while (!watch.stop)
  {
    // A busy room may take a while to answer: a few tries, a moment apart, before giving up; a
    // dropped connection gets about half a minute to come back.
    Common::WebSocket socket;
    std::string error;
    bool connected = false;
    const int tries = connections == 0 ? 5 : 15;
    for (int attempt = 0; attempt < tries && !watch.stop && !connected; ++attempt)
    {
      if (attempt > 0)
      {
        WARN_LOG_FMT(ROLLBACK, "Replay: the match's stream didn't answer ({}); trying again", error);
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      const std::string url = WatchTarget(source, &error);
      if (m_product && !WatchSourceAllowed(url, Orca::GetEnv("ORCA_REPLAY_ROOMS_ORIGIN"),
                                           Orca::UX::TestKnobsAllowed(), &error))
      {
        fail(error, true);
        return;
      }
      connected = !url.empty() && socket.Connect(url, std::chrono::seconds(10), &error);
    }
    if (watch.stop)
      return;
    if (!connected)
    {
      fail(connections == 0 ? "couldn't connect to the match: " + error :
                              "lost the match's stream: " + error);
      return;
    }
    if (connections++ > 0)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Replay: back on the match's stream; skipping the {} bytes already "
                               "played",
                     received);
      Orca::Status::Line(fmt::format("orca replay reconnected {}", connections - 1));
    }
    const auto connected_at = Clock::now();
    u64 received_before = received;
    u64 seen = 0;  // this connection's bytes
    auto last_ping = Clock::now();
    bool lost = false;
    while (!watch.stop)
    {
      if (drop_after > 0 && !dropped &&
          Clock::now() - connected_at > std::chrono::seconds(drop_after))
      {
        dropped = true;
        WARN_LOG_FMT(ROLLBACK, "Replay: test: the watch connection drops");
        socket.Close(4000);
        lost = true;
        break;
      }
      if (Clock::now() - last_ping > std::chrono::seconds(15))
      {
        socket.SendText(R"({"t":"ping"})", std::chrono::seconds(2));
        last_ping = Clock::now();
      }
      const std::optional<std::string> text = socket.ReceiveText(std::chrono::milliseconds(100));
      if (!text)
      {
        if (!socket.IsOpen())
        {
          WARN_LOG_FMT(ROLLBACK, "Replay: the watch connection closed ({}, code {})",
                       socket.Error(), socket.CloseCode());
          lost = true;
          break;
        }
        continue;
      }
      picojson::value value;
      if (!Orca::Json::ParseJson(*text, &value) || !value.is<picojson::object>())
        continue;
      const picojson::object& message = value.get<picojson::object>();
      const auto type = message.find("t");
      if (type == message.end() || !type->second.is<std::string>())
        continue;
      const std::string& t = type->second.get<std::string>();
      if (t == "rs")
      {
        const auto b = message.find("b");
        if (b == message.end() || !b->second.is<std::string>())
          continue;
        const std::string& text64 = b->second.get<std::string>();
        size_t pad = 0;
        while (pad < 2 && pad < text64.size() && text64[text64.size() - 1 - pad] == '=')
          ++pad;
        const std::optional<std::vector<u8>> bytes =
            text64.size() % 4 == 0 ?
                Orca::Net::DecodeInlineKeyframe(text64, text64.size() / 4 * 3 - pad) :
                std::nullopt;
        if (!bytes)
        {
          fail("the match's stream is damaged");
          return;
        }
        // What this connection repeats of the stream already taken: compared where it is the
        // stream's head, then skipped.
        size_t from = 0;
        if (seen < received)
        {
          from = static_cast<size_t>(std::min<u64>(received - seen, bytes->size()));
          for (size_t i = 0; i < from; ++i)
          {
            if (seen + i < head.size() && head[seen + i] != (*bytes)[i])
            {
              fail("the match's stream changed while reconnecting");
              return;
            }
          }
        }
        seen += bytes->size();
        if (from == bytes->size())
          continue;
        received += bytes->size() - from;
        if (head.size() < 65536)
        {
          head.insert(head.end(), bytes->begin() + from,
                      bytes->begin() + std::min(bytes->size(), from + (65536 - head.size())));
        }
        bool queued;
        {
          std::lock_guard lock(watch.mutex);
          queued = watch.pending.Append(std::span(*bytes).subspan(from));
        }
        if (!queued)
        {
          fail("the replay watch queue is over 8 MiB", true);
          return;
        }
      }
      else if (t == "reset")
      {
        // The room starts another segment, possibly a repack of this same history. The player
        // checks its header and overlap, after every byte queued before this marker.
        bool queued;
        {
          std::lock_guard lock(watch.mutex);
          queued = watch.pending.Reset();
        }
        if (!queued)
        {
          fail("the replay watch queue is over 8 MiB", true);
          return;
        }
        received = 0;
        received_before = 0;
        head.clear();
        seen = 0;
        fruitless = 0;
      }
      else if (t == "end")
      {
        {
          std::lock_guard lock(watch.mutex);
          watch.ended = true;
        }
        socket.SendText(R"({"t":"leave"})", std::chrono::seconds(1));
        socket.Close();
        return;
      }
    }
    if (!lost)
    {
      socket.Close();
      return;
    }
    fruitless = received == received_before ? fruitless + 1 : 0;
    if (fruitless >= 3)
    {
      WARN_LOG_FMT(ROLLBACK, "Replay: the match's stream keeps closing with nothing new; it's over");
      std::lock_guard lock(watch.mutex);
      watch.ended = true;
      return;
    }
  }
}

std::unique_ptr<Player> Player::FromEnv()
{
  const bool product = ProductMode();
  std::string path = Orca::GetEnv("ORCA_REPLAY_SOURCE");
  if (!product && Orca::UX::TestKnobsAllowed())
    path = Orca::GetEnv("ORCA_TEST_REPLAY_IN");
  if (path.empty())
    return nullptr;
  auto play = std::make_unique<Player>();
  play->m_path = std::move(path);
  play->m_product = product;
  if (product)
  {
    // Names only when the app gives them (a public replay's view-time names); a player's own replay
    // keeps the recording's labels. A malformed map shows every port as "Player N".
    if (const std::string names = Orca::GetEnv("ORCA_REPLAY_NAMES"); !names.empty())
      play->m_names = ParseNames(names).value_or(NameMap{});
    play->m_follow = play->m_path.starts_with("ws://") || play->m_path.starts_with("wss://");
    play->m_realtime = !play->m_follow;
    play->m_started = Clock::now();
    play->m_last_data = play->m_started;
    return play;
  }
  play->m_follow = Orca::GetEnv("ORCA_TEST_REPLAY_FOLLOW") == "1";
  play->m_realtime = Orca::GetEnv("ORCA_TEST_REPLAY_REALTIME") == "1";
  if (const std::string delay = Orca::GetEnv("ORCA_TEST_REPLAY_DELAY"); !delay.empty())
    play->m_delay = std::clamp(std::atoi(delay.c_str()), 0, 600);
  if (const std::string seek = Orca::GetEnv("ORCA_TEST_REPLAY_SEEK"); !seek.empty())
  {
    const size_t colon = seek.find(':');
    if (colon != std::string::npos)
    {
      const int a = std::atoi(seek.substr(0, colon).c_str());
      const int b = std::atoi(seek.substr(colon + 1).c_str());
      if (a > 0 && b > a)
        play->m_seek = std::make_pair(a, b);
    }
  }
  play->m_repack = Orca::GetEnv("ORCA_TEST_REPLAY_REPACK");
  play->m_started = Clock::now();
  play->m_last_data = play->m_started;
  return play;
}

// Feeds the reader whatever the file has gained. False, with `error`, if it isn't a valid stream
// for this build.
bool Player::Poll(std::string* error)
{
  if (!m_reader)
    m_reader = std::make_unique<Orca::Net::ReplayStreamReader>(Orca::Online::CompatibilityKey());
  if (m_path.starts_with("ws://") || m_path.starts_with("wss://") ||
      (!m_product && m_path.starts_with("watch:")))
  {
    if (m_product && !WatchSourceAllowed(m_path, Orca::GetEnv("ORCA_REPLAY_ROOMS_ORIGIN"),
                                        Orca::UX::TestKnobsAllowed(), error))
      return false;
    if (!m_watch)
    {
      m_watch = std::make_unique<Watch>();
      m_watch->thread = std::thread(&Player::WatchThread, this, m_path);
    }
    std::vector<u8> bytes;
    std::vector<size_t> resets;
    {
      std::lock_guard lock(m_watch->mutex);
      bytes.swap(m_watch->pending.bytes);
      resets.swap(m_watch->pending.resets);
      if (m_watch->ended)
        m_source_ended = true;
      if (!m_watch->error.empty() && (m_watch->fatal || !m_reader->Info()))
      {
        *error = m_watch->error;
        return false;
      }
    }
    if (bytes.empty() && resets.empty())
      return true;
    m_last_data = Clock::now();
    const auto feed = [&](size_t from, size_t to) {
      if (from == to)
        return true;
      const u32 resumes = m_reader->Resumes();
      if (!m_reader->Feed(bytes.data() + from, to - from, error))
        return false;
      if (m_reader->Resumes() != resumes)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Replay: resumed the match's history at frame {} after a reset",
                       m_reader->Archive().EndFrame());
        Orca::Status::Line(fmt::format("orca replay resumed {} end={}", m_reader->Resumes(),
                                     m_reader->Archive().EndFrame()));
      }
      return true;
    };
    size_t from = 0;
    for (const size_t reset : resets)
    {
      if (!feed(from, reset))
        return false;
      m_reader->Resume();
      from = reset;
    }
    return feed(from, bytes.size());
  }
  if (!m_file)
  {
    // The writer may still have it open for writing (a live watcher of a growing file).
    m_file = OpenReplayFile(m_path, error);
    if (!m_file)
    {
      if (!m_follow || *error != "the replay file can't be opened")
        return false;
      error->clear();
      return true;  // a live stream not started yet
    }
  }
  const u64 size = m_file->GetSize();
  if (m_product && (size == 0 || size > Orca::Net::REPLAY_PACKED_MAX))
  {
    *error = size == 0 ? "the saved replay is empty" : "the saved replay is over 1 MiB";
    return false;
  }
  if (size <= m_offset)
    return true;
  std::vector<u8> bytes(static_cast<size_t>(size - m_offset));
  if (!m_file->Seek(static_cast<s64>(m_offset), File::SeekOrigin::Begin) ||
      !m_file->ReadBytes(bytes.data(), bytes.size()))
  {
    *error = "the replay file can't be read";
    return false;
  }
  m_offset = size;
  m_last_data = Clock::now();
  if (!m_reader->Feed(bytes.data(), bytes.size(), error))
    return false;
  if (m_product && (!m_reader->Complete() || m_reader->Archive().frames.empty()))
  {
    *error = "the saved replay is incomplete";
    return false;
  }
  return true;
}

void Player::PrintTime(int frame, bool force)
{
  if (!m_product || !m_loaded || !m_reader->Info())
    return;
  if (!force && Clock::now() - m_time_printed < std::chrono::milliseconds(250))
    return;
  m_time_printed = Clock::now();
  const s64 elapsed = std::max<s64>(0, static_cast<s64>(frame) - m_reader->Info()->first_frame);
  Orca::Status::Line(fmt::format("orca replay time {} {}", frame, elapsed * 1000 / 60));
}

void Player::Fail(const PlayerHost& host, const std::string& why)
{
  if (!m_over)
    Finish(host, false, why);
}

void Player::Finish(const PlayerHost& host, bool ok, const std::string& why)
{
  if (ok && m_product && (!m_reader || !m_reader->Complete() || m_reader->Archive().frames.empty()))
  {
    Finish(host, false, "the replay stream ended incomplete");
    return;
  }
  PrintTime(m_position, true);
  const auto ms = [](double frames) { return frames * 1000.0 / 60.0; };
  const double avg = m_behind_samples ? static_cast<double>(m_behind_sum) / m_behind_samples : 0.0;
  const double live_avg =
      m_behind_samples ? static_cast<double>(m_live_sum) / m_behind_samples : 0.0;
  std::string_view comparison = "checksums compared";
  if (m_labeled_recording)
    comparison = "checksums not compared (labeled recording)";
  else if (m_product && m_names)
    comparison = "checksums not compared (view names)";
  Orca::Status::Line(fmt::format(
      "orca replay done {} frames={} checked={} mismatched={} first_mismatch={} seek={} "
      "behind_avg_ms={:.0f} behind_max_ms={:.0f} live_avg_ms={:.0f} live_max_ms={:.0f} "
      "caught_up_ms={:.0f} waits={} waited_ms={:.0f} wall_ms={:.0f} verification={} why={}",
      ok && m_mismatched == 0 ? "ok" : "fail", m_played, m_checked, m_mismatched, m_first_mismatch,
      m_seek_done ? "done" : (m_seek ? "missed" : "none"), ms(avg), ms(m_behind_max), ms(live_avg),
      ms(m_live_max), m_caught_up_ms, m_waits, m_waited_ms, MsSince(m_started), comparison,
      why.empty() ? "-" : why));
  NOTICE_LOG_FMT(ROLLBACK, "Replay: {} after {} frames, {} checksums checked, {} differ ({})",
                 ok ? "over" : "failed", m_played, m_checked, m_mismatched, why);
  m_over = true;
  host.stop(ok ? "The replay is over; " + std::string(comparison) :
                 "The replay couldn't be played: " + why);
}

std::optional<int> Player::Step(Core::System& system, const PlayerHost& host, int* running,
                                u64 origin_hash, int origin_frame,
                                const OnlineMatch::FrameHook& on_frame)
{
  if (m_over)
    return std::nullopt;
  std::optional<int> rewound;
  std::string error;
  if (!Poll(&error))
  {
    Finish(host, false, error);
    return std::nullopt;
  }
  if (!m_loaded)
  {
    // A live stream may not have its header yet.
    while (!m_reader->Info())
    {
      if (host.stopping())
        return std::nullopt;
      // A watch URL may take several tries to connect (WatchThread).
      const auto limit = m_watch ? std::chrono::seconds(75) : std::chrono::seconds(30);
      if (m_source_ended || Clock::now() - m_last_data > limit)
      {
        Finish(host, false, "the replay never started");
        return std::nullopt;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!Poll(&error))
      {
        Finish(host, false, error);
        return std::nullopt;
      }
    }
    const Orca::Net::ReplayStreamInfo& info = *m_reader->Info();
    if (info.origin_hash != origin_hash || info.first_frame != origin_frame)
    {
      Finish(host, false, "the replay starts from another origin");
      return std::nullopt;
    }
    if (!host.restore_origin())
    {
      Finish(host, false, "couldn't restore this game's local boot");
      return std::nullopt;
    }
    *running = origin_frame - 1;
    m_loaded = true;
    m_labeled_recording = info.seat_labels;
    m_position = origin_frame;
    m_catching = !m_realtime && !m_follow;
    host.set_catching_up(m_catching);
    rewound = origin_frame;
    Orca::Status::Line(fmt::format("orca replay start build={} game={} first={} origin={:016x}",
                                   info.build, info.game_id, info.first_frame, info.origin_hash));
    PrintTime(m_position, true);
  }
  // The window thread never touches the player or machine state. Apply its mailbox here, even
  // while paused or waiting on a live chunk, so stdin and the window heartbeat keep working.
  const auto controls = [&] {
    if (!m_product)
      return true;
    std::optional<bool> pause;
    std::optional<int> seek;
    std::optional<NameMap> names;
    {
      std::lock_guard lock(s_control_mutex);
      pause.swap(s_pause);
      seek.swap(s_seek);
      names.swap(s_names);
    }
    if (names)
      m_names = std::move(names);
    if (pause)
    {
      m_paused = *pause;
      Orca::Status::Line(m_paused ? "state paused" : "state running");
    }
    if (m_seek_target && *running >= *m_seek_target)
      m_seek_target.reset();
    if (seek)
    {
      const auto& archive = m_reader->Archive();
      // Live playback can seek only within the history already received. An invalid target
      // leaves the run and its pause alone, like any unsupported embed command.
      if (*seek < archive.first_frame || *seek >= archive.EndFrame())
      {
        Orca::Status::Line("unsupported replay");
      }
      else
      {
        if (!host.restore_origin())
        {
          Finish(host, false, "couldn't seek in the replay");
          return false;
        }
        Orca::Status::Line(fmt::format("orca replay seek {} -> {}", *running + 1, *seek));
        *running = origin_frame - 1;
        m_position = origin_frame;
        rewound = origin_frame;
        m_seek_target = *seek;
        m_catching = true;
        host.set_catching_up(true);
      }
    }
    return true;
  };
  const auto wait_paused = [&] {
    if (!controls())
      return false;
    // A pause queued during boot still presents the first replay frame before holding it.
    while (m_paused && !m_seek_target && *running >= origin_frame)
    {
      // Keep the last picture visible while the CPU thread waits at its next boundary.
      host.set_catching_up(false);
      m_catching = false;
      PrintTime(m_position);
      if (host.stopping())
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!Poll(&error))
      {
        Finish(host, false, error);
        return false;
      }
      if (!controls())
        return false;
    }
    return true;
  };
  if (!wait_paused())
    return std::nullopt;
  int frame = *running + 1;
  if (m_seek && m_seek_taken && !m_seek_done && frame == m_seek->second)
  {
    const int to = m_seek->first;
    if (!host.restore(std::move(m_seek_image), m_seek_nand, to))
    {
      Finish(host, false, "couldn't seek in the replay");
      return std::nullopt;
    }
    *running = to - 1;
    host.set_catching_up(m_catching);
    m_seek_done = true;
    m_seek_nand.clear();
    Orca::Status::Line(fmt::format("orca replay seek {} -> {}", frame, to));
    frame = to;
    rewound = to;
  }
  Orca::Net::ReplayArchive& archive = m_reader->Archive();
  if (m_follow && !m_seek_target)
  {
    // A live watcher stays `delay` frames behind the newest final frame, so a late chunk doesn't
    // freeze the picture; it waits when closer, and plays out the tail once the stream goes quiet.
    if (archive.EndFrame() - frame <= m_delay)
    {
      const auto wait_start = Clock::now();
      ++m_waits;
      while (archive.EndFrame() - frame <= m_delay)
      {
        if (!wait_paused())
          return std::nullopt;
        frame = *running + 1;
        if (m_seek_target || archive.EndFrame() - frame > m_delay)
          break;
        PrintTime(m_position);
        // A file is over once it stops growing; a watch socket says so ({"t":"end"}, or it is lost
        // for good), and its silence is a network hiccup or a reconnect, not the end.
        const auto quiet_after = m_watch ? std::chrono::seconds(150) : std::chrono::seconds(5);
        const bool quiet = m_source_ended || Clock::now() - m_last_data > quiet_after;
        if (quiet && frame < archive.EndFrame())
          break;
        if (quiet)
        {
          m_waited_ms += MsSince(wait_start);
          // A watch socket lost for good ends the watching early: not a whole match.
          std::string lost;
          if (m_watch)
          {
            std::lock_guard lock(m_watch->mutex);
            lost = m_watch->error;
          }
          Finish(host, lost.empty(), lost.empty() ? "the stream ended" : lost);
          return std::nullopt;
        }
        if (host.stopping())
          return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!Poll(&error))
        {
          Finish(host, false, error);
          return std::nullopt;
        }
      }
      m_waited_ms += MsSince(wait_start);
    }
  }
  else if (frame >= archive.EndFrame())
  {
    if (!m_repack.empty() && m_mismatched == 0)
    {
      const std::vector<u8> bytes =
          Orca::Net::PackReplayStream(*m_reader->Info(), archive, m_reader->Checksums());
      File::IOFile out(m_repack, "wb");
      const bool written = !bytes.empty() && out && out.WriteBytes(bytes.data(), bytes.size());
      Orca::Status::Line(fmt::format("orca replay repack {} {} bytes", written ? "ok" : "fail",
                                     bytes.size()));
    }
    Finish(host, true, "");
    return std::nullopt;
  }
  const int behind = archive.EndFrame() - frame;
  // A saved replay plays flat out unless asked for real time. A watcher plays flat out while far
  // behind (joining late, or after a stall) and in real time once back near its delay; the gap
  // between the two thresholds keeps it from flapping.
  // Render the requested frame before a paused seek waits at the next boundary. Earlier seek
  // frames are hidden and unthrottled, as the canonical boot is.
  const bool seeking = m_seek_target.has_value();
  bool catching = seeking ? frame < *m_seek_target : !m_realtime;
  if (m_follow && !m_realtime && !seeking)
    catching = m_catching ? behind > m_delay + 2 : behind > m_delay + 30;
  if (m_paused && !seeking)
    catching = false;
  if (catching != m_catching)
  {
    host.set_catching_up(catching);
    m_catching = catching;
    if (!catching && m_caught_up_ms < 0)
      m_caught_up_ms = MsSince(m_started);
  }
  if (m_follow && !catching)
  {
    const int behind_live = std::max(behind, m_reader->LiveFrame() - frame);
    m_behind_sum += behind;
    m_live_sum += behind_live;
    ++m_behind_samples;
    m_behind_max = std::max(m_behind_max, behind);
    m_live_max = std::max(m_live_max, behind_live);
  }
  // A seek point: the state at this frame's start, before its hook, as an origin is taken.
  if (m_seek && !m_seek_taken && frame == m_seek->first)
  {
    m_seek_taken =
        host.read_nand(&m_seek_nand) && SnapshotRing::Capture(system, &m_seek_image, false);
    Orca::Status::Line(fmt::format("orca replay seek-point {} {}", frame,
                                   m_seek_taken ? "taken" : "failed"));
  }
  Orca::Net::ReplayFrame* const record = archive.At(frame);
  if (!record)
  {
    Finish(host, false, "the replay has no frame at this position");
    return std::nullopt;
  }
  // Earlier labeled recordings predate the header flag. Recognize their seat metadata
  // too; empty startup frames have no names yet and are not compared below.
  if (!m_labeled_recording && !record->ports->empty() &&
      std::ranges::all_of(*record->ports, [](const auto& port) {
        return port.name == fmt::format("P{}", port.port + 1);
      }))
    m_labeled_recording = true;
  {
    Orca::Net::ReplayFrame view = *record;
    if (m_product && m_names)
    {
      view.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
          ViewPorts(*record->ports, *m_names));
    }
    Orca::Net::ReplayScope scope(&view, true, true, m_product && m_names.has_value(), true);
    on_frame(frame, true, *view.ports, false);
  }
  // Names are also copied/rendered by the game outside Orca's writes. Labeled recordings
  // and view-time names cannot reproduce the live run's full RAM.
  if (const auto it = m_reader->Checksums().find(frame);
      it != m_reader->Checksums().end() && !m_labeled_recording && !record->ports->empty() &&
      !(m_product && m_names))
  {
    const u64 hash = MemoryChecksum(system);
    ++m_checked;
    if (hash != it->second)
    {
      ++m_mismatched;
      if (m_first_mismatch < 0)
      {
        m_first_mismatch = frame;
        WARN_LOG_FMT(ROLLBACK, "Replay: frame {} is {:016x} here, {:016x} in the recording", frame,
                     hash, it->second);
      }
    }
  }
  host.set_pads(frame, record->pads);
  *running = frame;
  m_position = frame;
  PrintTime(frame);
  ++m_played;
  if (frame - m_last_progress >= 600)
  {
    m_last_progress = frame;
    Orca::Status::Line(fmt::format("orca replay at {} of {} behind {} checked {} mismatched {}",
                                   frame, archive.EndFrame(), behind, m_checked, m_mismatched));
  }
  return rewound;
}
}  // namespace Rollback::ReplayStream
