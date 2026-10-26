// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/BootAudit.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Core.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"

namespace Orca::BootAudit
{
namespace
{
std::optional<CacheRecord> Load(const std::string& path)
{
  // A local cache is only a hint, and must be cheap to reject even when damaged.
  if (!File::IsFile(path) || File::GetSize(path) > 16384)
    return std::nullopt;
  Common::IniFile ini;
  if (!ini.Load(path))
    return std::nullopt;
  const auto* section = ini.GetSection("Verified");
  CacheRecord record;
  int version = 0;
  if (!section || !section->Get("Version", &version) || version != 1 ||
      !section->Get("Path", &record.identity.path) ||
      !section->Get("Size", &record.identity.size) ||
      !section->Get("Modified", &record.identity.modified) ||
      !section->Get("Changed", &record.identity.changed) ||
      !section->Get("Volume", &record.identity.volume) ||
      !section->Get("FileId", &record.identity.file_id) ||
      !section->Get("Hash", &record.verified_hash) || !section->Get("Engine", &record.engine))
  {
    return std::nullopt;
  }
  return record;
}

void Save(const std::string& path, const Identity& identity, const std::string& hash)
{
  if (!File::CreateFullPath(path))
    return;
  Common::IniFile ini;
  auto* section = ini.GetOrCreateSection("Verified");
  section->Set("Version", 1);
  section->Set("Path", identity.path);
  section->Set("Size", identity.size);
  section->Set("Modified", identity.modified);
  section->Set("Changed", identity.changed);
  section->Set("Volume", identity.volume);
  section->Set("FileId", identity.file_id);
  section->Set("Hash", hash);
  section->Set("Engine", Common::GetScmRevGitStr());
  ini.Save(path);
}

void Drop(const std::string& path)
{
  if (File::Exists(path))
    File::Delete(path);
}
}  // namespace

std::optional<Identity> Identify(const std::string& path)
{
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(StringToPath(path), ec);
  if (ec)
    return std::nullopt;
  Identity identity;
  identity.path = PathToString(canonical);
#ifdef _WIN32
  const HANDLE file = CreateFileW(canonical.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return std::nullopt;
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO id{};
  const bool ok =
      GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
      GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) &&
      GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id));
  CloseHandle(file);
  if (!ok || standard.Directory || standard.EndOfFile.QuadPart < 0)
    return std::nullopt;
  identity.size = static_cast<u64>(standard.EndOfFile.QuadPart);
  identity.modified = fmt::format("{}", basic.LastWriteTime.QuadPart);
  identity.changed = fmt::format("{}", basic.ChangeTime.QuadPart);
  identity.volume = id.VolumeSerialNumber;
  for (const auto byte : id.FileId.Identifier)
    identity.file_id += fmt::format("{:02x}", byte);
#else
  struct stat info{};
  if (stat(identity.path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
    return std::nullopt;
  identity.size = static_cast<u64>(info.st_size);
#ifdef __APPLE__
  const auto modified = info.st_mtimespec;
  const auto changed = info.st_ctimespec;
#else
  const auto modified = info.st_mtim;
  const auto changed = info.st_ctim;
#endif
  identity.modified = fmt::format("{}:{}", modified.tv_sec, modified.tv_nsec);
  identity.changed = fmt::format("{}:{}", changed.tv_sec, changed.tv_nsec);
  identity.volume = static_cast<u64>(info.st_dev);
  identity.file_id = fmt::format("{}", info.st_ino);
#endif
  return identity;
}

bool CacheRecord::Matches(const Identity& current, std::string_view expected) const
{
  return identity == current && !expected.empty() && verified_hash == expected &&
         engine == Common::GetScmRevGitStr();
}

Audit::Audit(std::vector<FileToVerify> files, std::string cache_dir, Hasher hasher)
    : m_files(std::move(files)),
      m_hasher(hasher ? std::move(hasher) :
                        [](const std::string& path, const std::atomic<bool>& stop) {
                          return HashFile(path, &stop);
                        })
{
  for (const auto& file : m_files)
  {
    auto identity = Identify(file.path);
    const std::string key = (identity ? identity->path : file.path) + '\n' + file.expected;
    const std::string record =
        cache_dir + "/" + fmt::format("{:016x}.ini", XXH3_64bits(key.data(), key.size()));
    const auto cached = identity ? Load(record) : std::nullopt;
    m_cache_hit &= cached && cached->Matches(*identity, file.expected);
    m_identities.push_back(std::move(identity));
    m_records.push_back(record);
  }
  m_worker = std::thread([this] { Run(); });
}

std::shared_ptr<Audit> Audit::Start(std::vector<FileToVerify> files, std::string cache_dir,
                                   Hasher hasher)
{
  return std::shared_ptr<Audit>(
      new Audit(std::move(files), std::move(cache_dir), std::move(hasher)));
}

Audit::~Audit()
{
  Cancel();
  m_worker.join();
}

void Audit::Run()
{
  for (std::size_t i = 0; i < m_files.size(); ++i)
  {
    if (m_stop.load(std::memory_order_relaxed))
    {
      Complete(false);
      return;
    }
    const auto& file = m_files[i];
    const std::string hash = m_hasher(file.path, m_stop);
    const auto after = Identify(file.path);
    // A file replaced or edited during verification never supplies a cache record or goes online.
    const bool changed = m_identities[i] && after != m_identities[i];
    if (hash.empty() || hash != file.expected || changed)
    {
      Complete(false, file.refusal, i);
      return;
    }
  }
  Complete(true);
}

void Audit::Complete(bool passed, std::string error, std::optional<std::size_t> failed)
{
  std::function<void()> failure;
  {
    std::lock_guard lock(m_mutex);
    // Publish cache changes only after the entire audit, serialized with Cancel. In particular,
    // cancelling the SD read must leave the launcher's record unchanged too.
    if (m_stop.load(std::memory_order_relaxed))
    {
      passed = false;
      error.clear();
    }
    else if (passed)
    {
      for (std::size_t i = 0; i < m_files.size(); ++i)
      {
        if (m_identities[i])
          Save(m_records[i], *m_identities[i], m_files[i].expected);
      }
    }
    else if (failed)
    {
      Drop(m_records[*failed]);
    }
    m_error = std::move(error);
    m_passed = passed;
    m_finished = true;
    if (!m_passed && !m_error.empty())
      failure = std::exchange(m_failure, {});
  }
  m_done.notify_all();
  if (failure)
    failure();
}

bool Audit::Wait(const std::atomic<bool>* stop)
{
  std::unique_lock lock(m_mutex);
  if (stop)
  {
    // A watch worker can be destroyed independently of the session's audit.
    while (!m_finished && !stop->load(std::memory_order_relaxed))
      m_done.wait_for(lock, std::chrono::milliseconds(10));
    if (stop->load(std::memory_order_relaxed))
      return false;
  }
  else
  {
    m_done.wait(lock, [this] { return m_finished; });
  }
  return m_passed;
}

void Audit::Cancel()
{
  std::lock_guard lock(m_mutex);
  if (!m_finished)
    m_stop.store(true, std::memory_order_relaxed);
}

std::string Audit::Error() const
{
  std::lock_guard lock(m_mutex);
  return m_error;
}

void Audit::ReportFailure()
{
  std::string error;
  {
    std::lock_guard lock(m_mutex);
    if (!m_finished || m_passed || m_error.empty() || m_reported)
      return;
    m_reported = true;
    error = m_error;
  }
  Status::Error("profile", error);
}

void Audit::OnFailure(std::function<void()> handler)
{
  std::function<void()> failure;
  {
    std::lock_guard lock(m_mutex);
    m_failure = std::move(handler);
    if (m_finished && !m_passed && !m_error.empty())
      failure = std::exchange(m_failure, {});
  }
  if (failure)
    failure();
}

std::shared_ptr<Audit> SessionAudit()
{
  const Profile* profile = ActiveProfile();
  return profile ? profile->boot_audit : nullptr;
}

bool WaitForOnline(const std::shared_ptr<Audit>& audit, const std::atomic<bool>* stop)
{
  if (!audit || audit->Wait(stop))
    return !stop || !stop->load(std::memory_order_relaxed);
  audit->ReportFailure();
  return false;
}

void CancelSession()
{
  if (const auto audit = SessionAudit())
  {
    audit->Cancel();
    audit->ReportFailure();
  }
}

void WatchSession(const std::shared_ptr<Audit>& audit)
{
  const std::weak_ptr<Audit> weak = audit;
  audit->OnFailure([weak] {
    Core::QueueHostJob(
        [weak](Core::System& system) {
          const auto failed = weak.lock();
          const Profile* active = ActiveProfile();
          if (failed && active && active->boot_audit == failed)
          {
            failed->ReportFailure();
            Core::Stop(system);
          }
        },
        true);
  });
}
}  // namespace Orca::BootAudit
