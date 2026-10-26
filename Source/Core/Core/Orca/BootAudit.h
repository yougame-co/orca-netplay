// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Orca::BootAudit
{
struct Identity
{
  std::string path;
  u64 size = 0;
  std::string modified;
  std::string changed;
  u64 volume = 0;
  std::string file_id;
  bool operator==(const Identity&) const = default;
};

std::optional<Identity> Identify(const std::string& path, std::string* reason = nullptr);

struct CacheRecord
{
  Identity identity;
  std::string verified_hash;
  std::string engine;
  bool Matches(const Identity& current, std::string_view expected) const;
};

struct FileToVerify
{
  std::string path;
  std::string expected;
  std::string refusal;
  std::optional<u64> expected_size;
};

// A cache hit permits offline boot only. Every start hashes every file on this worker, including
// hits. Online callers must Wait successfully before doing any room, queue or ticket work.
class Audit final
{
public:
  struct HashResult
  {
    std::string hash;
    std::string reason;
    HashResult(std::string value, std::string why = {})
        : hash(std::move(value)), reason(std::move(why))
    {
    }
    HashResult(const char* value) : hash(value) {}
  };
  using Hasher = std::function<HashResult(const std::string&, const std::atomic<bool>&)>;
  static std::shared_ptr<Audit> Start(std::vector<FileToVerify> files, std::string cache_dir,
                                     Hasher hasher = {});
  ~Audit();
  bool CacheHit() const { return m_cache_hit; }
  bool Wait(const std::atomic<bool>* stop = nullptr);
  // Cancellation never passes, reports a mismatch or changes cache records.
  void Cancel();
  std::string Error() const;
  void ReportFailure();
  // Install after BootCore returns. A failure that already completed is delivered immediately.
  // The handler must schedule host work, never stop the CPU from the hash worker.
  void OnFailure(std::function<void()> handler);

private:
  Audit(std::vector<FileToVerify> files, std::string cache_dir, Hasher hasher);
  void Run();
  void Complete(bool passed, std::string error = {}, std::optional<std::size_t> failed = {});
  std::vector<FileToVerify> m_files;
  std::vector<std::optional<Identity>> m_identities;
  std::vector<std::string> m_identity_errors;
  std::vector<std::string> m_records;
  Hasher m_hasher;
  std::atomic<bool> m_stop{false};
  bool m_cache_hit = true;
  mutable std::mutex m_mutex;
  std::condition_variable m_done;
  bool m_finished = false;
  bool m_passed = false;
  bool m_reported = false;
  std::string m_error;
  std::function<void()> m_failure;
  std::thread m_worker;
};

// Capture the session audit on the calling thread, before starting background work.
std::shared_ptr<Audit> SessionAudit();
bool WaitForOnline(const std::shared_ptr<Audit>& audit, const std::atomic<bool>* stop = nullptr);
// Request cancellation before joining the CPU or restoring configuration.
void CancelSession();

// Called after Core::Init succeeds, for every frontend. Failed warm audits end this session on
// the host thread, including failures completed during core initialization.
void WatchSession(const std::shared_ptr<Audit>& audit);
}  // namespace Orca::BootAudit
