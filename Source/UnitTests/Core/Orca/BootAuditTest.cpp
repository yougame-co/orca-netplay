// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IniFile.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Orca/BootAudit.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/System.h"

using namespace std::chrono_literals;
using Orca::BootAudit::Audit;
using Orca::BootAudit::CacheRecord;
using Orca::BootAudit::FileToVerify;
using Orca::BootAudit::Identify;

namespace
{
void SetEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}

struct HeldHash
{
  std::promise<void> entered;
  std::promise<bool> release;
  std::shared_future<bool> result = release.get_future().share();
  std::atomic<int> calls{0};
  static Audit::Hasher Function(const std::shared_ptr<HeldHash>& held)
  {
    return [held](const std::string& path, const std::atomic<bool>& stop) {
      if (held->calls.fetch_add(1) == 0)
        held->entered.set_value();
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!stop && held->result.wait_for(10ms) != std::future_status::ready &&
             std::chrono::steady_clock::now() < deadline)
      {
      }
      if (stop || held->result.wait_for(0ms) != std::future_status::ready || !held->result.get())
        return std::string("different");
      return Orca::HashFile(path, &stop);
    };
  }
};

class ScopedDeathStyle
{
public:
  ScopedDeathStyle() : m_old(GTEST_FLAG_GET(death_test_style))
  {
    // SConfig starts threads; a fresh process also permits macOS networking in the child.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
  ~ScopedDeathStyle() { GTEST_FLAG_SET(death_test_style, m_old); }

private:
  std::string m_old;
};

class OrcaBootAudit : public testing::Test
{
protected:
  void SetUp() override
  {
    old_user = File::GetUserPath(D_USER_IDX);
    old_session = Orca::GetEnv("ORCA_SESSION");
    dir = File::CreateTempDir();
    ASSERT_FALSE(dir.empty());
    File::SetUserPath(D_USER_IDX, dir + "/user/");
    Config::Init();
    SConfig::Init();
    SetEnv("ORCA_SESSION", "0");
    path = dir + "/loader.dol";
    ASSERT_TRUE(File::WriteStringToFile(path, "fixed launcher fixture"));
    expected = Orca::HashFile(path);
    cache = dir + "/cache";
  }
  void TearDown() override
  {
    Orca::Online::Shutdown();
    Orca::SetActiveProfile(std::nullopt);
    SConfig::Shutdown();
    Config::Shutdown();
    File::SetUserPath(D_USER_IDX, old_user);
    SetEnv("ORCA_SESSION", old_session);
    File::DeleteDirRecursively(dir);
  }
  FileToVerify Entry() const
  {
    return {path, expected, "Orca: " + path + " is not the loader profile PPLUS32 names"};
  }
  std::string CachePath() const
  {
    return PathToString(std::filesystem::directory_iterator(StringToPath(cache))->path());
  }
  void MatchCacheToCurrentIdentity()
  {
    // Model identical cached/current metadata while the bytes no longer match the expected hash.
    // Changing ctime back is not portable, so write the current identity into the fixture record.
    const auto identity = Identify(path);
    ASSERT_TRUE(identity);
    Common::IniFile ini;
    ASSERT_TRUE(ini.Load(CachePath()));
    auto* section = ini.GetOrCreateSection("Verified");
    section->Set("Path", identity->path);
    section->Set("Size", identity->size);
    section->Set("Modified", identity->modified);
    section->Set("Changed", identity->changed);
    section->Set("Volume", identity->volume);
    section->Set("FileId", identity->file_id);
    ASSERT_TRUE(ini.Save(CachePath()));
  }
  std::shared_ptr<Audit> Start(Audit::Hasher hasher = {})
  {
    return Audit::Start({Entry()}, cache, std::move(hasher));
  }
  CacheRecord Record() const { return {*Identify(path), expected, Common::GetScmRevGitStr()}; }
  void Activate(const std::shared_ptr<Audit>& audit)
  {
    Orca::Profile profile;
    profile.game_id = "PPLUS32";
    profile.launcher_hash = expected;
    profile.disc = "RSBE01";
    profile.sd_image_hash = "expected-card-hash";
    profile.boot_audit = audit;
    Orca::SetActiveProfile(profile);
  }
  std::string dir, path, expected, cache, old_user, old_session;
};
}  // namespace

TEST_F(OrcaBootAudit, CacheRecordMatchesOnlyTheVerifiedIdentity)
{
  const auto record = Record();
  EXPECT_TRUE(record.Matches(*Identify(path), expected));
  EXPECT_FALSE(record.Matches(*Identify(path), "another-profile-hash"));
  auto different_engine = record;
  different_engine.engine = "another-engine";
  EXPECT_FALSE(different_engine.Matches(*Identify(path), expected));
}

TEST_F(OrcaBootAudit, SizeChangeIsAMiss)
{
  const auto record = Record();
  ASSERT_TRUE(File::WriteStringToFile(path, "different size"));
  EXPECT_FALSE(record.Matches(*Identify(path), expected));
  auto size_only = record.identity;
  ++size_only.size;
  EXPECT_FALSE(record.Matches(size_only, expected));
}

TEST_F(OrcaBootAudit, ModificationTimeChangeIsAMiss)
{
  const auto record = Record();
  auto modified = record.identity;
  modified.modified += "1";
  EXPECT_FALSE(record.Matches(modified, expected));
}

TEST_F(OrcaBootAudit, ChangeTimeChangeIsAMiss)
{
  const auto record = Record();
  auto changed = record.identity;
  changed.changed += "1";
  EXPECT_FALSE(record.Matches(changed, expected));
}

TEST_F(OrcaBootAudit, ReplacedFileIsAMissEvenWithTheSameSizeAndMtime)
{
  const auto record = Record();
  const auto original_time = std::filesystem::last_write_time(StringToPath(path));
  const std::string replacement = dir + "/replacement";
  ASSERT_TRUE(File::WriteStringToFile(replacement, "fixed launcher fixture"));
  std::filesystem::last_write_time(StringToPath(replacement), original_time);
  ASSERT_TRUE(File::Rename(replacement, path));
  const auto current = Identify(path);
  ASSERT_TRUE(current);
  EXPECT_EQ(current->size, record.identity.size);
  EXPECT_EQ(current->modified, record.identity.modified);
  EXPECT_NE(current->file_id, record.identity.file_id);
  EXPECT_FALSE(record.Matches(*current, expected));
  auto id_only = record.identity;
  id_only.file_id += "1";
  EXPECT_FALSE(record.Matches(id_only, expected));
}

TEST_F(OrcaBootAudit, PathAndVolumeChangesAreMisses)
{
  const auto record = Record();
  auto identity = record.identity;
  identity.path += "other";
  EXPECT_FALSE(record.Matches(identity, expected));
  identity = record.identity;
  ++identity.volume;
  EXPECT_FALSE(record.Matches(identity, expected));
}

TEST_F(OrcaBootAudit, ColdVerificationPopulatesAWarmRecordAndStillAudits)
{
  const auto cold = Start();
  EXPECT_FALSE(cold->CacheHit());
  ASSERT_TRUE(cold->Wait());
  const auto held = std::make_shared<HeldHash>();
  const auto warm = Start(HeldHash::Function(held));
  EXPECT_TRUE(warm->CacheHit());
  const auto entered = held->entered.get_future().wait_for(2s);
  auto online_gate = std::async(std::launch::async, [&] { return warm->Wait(); });
  EXPECT_EQ(entered, std::future_status::ready);
  EXPECT_EQ(online_gate.wait_for(30ms), std::future_status::timeout);
  held->release.set_value(true);
  EXPECT_TRUE(online_gate.get());
  EXPECT_EQ(held->calls, 1);
}

TEST_F(OrcaBootAudit, CancelledHashNeverReturnsADigest)
{
  std::atomic<bool> stop{true};
  EXPECT_TRUE(Orca::HashFile(path, &stop).empty());
  stop = false;
  EXPECT_EQ(Orca::HashFile(path, &stop), expected);
  ASSERT_TRUE(File::WriteStringToFile(path, ""));
  stop = true;
  EXPECT_TRUE(Orca::HashFile(path, &stop).empty());
}

TEST_F(OrcaBootAudit, CancellationFinishesAtTheReadBoundaryAndPreservesAllRecords)
{
  const std::string card = dir + "/sd.raw";
  ASSERT_TRUE(File::WriteStringToFile(card, "card fixture"));
  const std::vector<FileToVerify> files{Entry(), {card, Orca::HashFile(card), "card refusal"}};
  for (const bool warm : {false, true})
  {
    if (warm)
      ASSERT_TRUE(Audit::Start(files, cache)->Wait());
    std::vector<std::pair<std::string, std::string>> records;
    std::vector<std::filesystem::file_time_type> times;
    if (warm)
    {
      for (const auto& entry : std::filesystem::directory_iterator(StringToPath(cache)))
      {
        std::string contents;
        ASSERT_TRUE(File::ReadFileToString(PathToString(entry.path()), contents));
        records.emplace_back(PathToString(entry.path()), contents);
        times.push_back(entry.last_write_time());
      }
      ASSERT_EQ(records.size(), 2u);
    }
    std::promise<void> reading;
    std::promise<void> finish_chunk;
    auto chunk = finish_chunk.get_future();
    std::atomic<int> reads{0};
    std::atomic<int> failures{0};
    const auto audit = Audit::Start(files, cache, [&](const std::string& file,
                                                     const std::atomic<bool>& stop) {
      if (file == card)
      {
        // Hold one in-flight read; cancellation must take effect before another chunk begins.
        ++reads;
        reading.set_value();
        if (chunk.wait_for(2s) != std::future_status::ready)
          return std::string{};
        if (!stop)
          ++reads;
      }
      return Orca::HashFile(file, &stop);
    });
    EXPECT_EQ(audit->CacheHit(), warm);
    audit->OnFailure([&] { ++failures; });
    EXPECT_EQ(reading.get_future().wait_for(2s), std::future_status::ready);
    audit->Cancel();
    auto stopped = std::async(std::launch::async, [&] { return audit->Wait(); });
    finish_chunk.set_value();
    EXPECT_EQ(stopped.wait_for(500ms), std::future_status::ready);
    EXPECT_FALSE(stopped.get());
    EXPECT_EQ(reads, 1);
    EXPECT_TRUE(audit->Error().empty());
    audit->OnFailure([&] { ++failures; });
    EXPECT_EQ(failures, 0);
    if (!warm)
    {
      EXPECT_FALSE(File::Exists(cache));
    }
    for (std::size_t i = 0; i < records.size(); ++i)
    {
      std::string contents;
      EXPECT_TRUE(File::ReadFileToString(records[i].first, contents));
      EXPECT_EQ(contents, records[i].second);
      EXPECT_TRUE(std::filesystem::last_write_time(StringToPath(records[i].first)) == times[i]);
    }
    std::atomic<int> calls{0};
    const auto next = Audit::Start(files, cache, [&](const std::string& file,
                                                   const std::atomic<bool>& stop) {
      ++calls;
      return Orca::HashFile(file, &stop);
    });
    EXPECT_EQ(next->CacheHit(), warm);
    EXPECT_TRUE(next->Wait());
    EXPECT_EQ(calls, 2);
  }
}

TEST_F(OrcaBootAudit, DestructionCancelsTheWorker)
{
  const auto held = std::make_shared<HeldHash>();
  auto audit = Start(HeldHash::Function(held));
  ASSERT_EQ(held->entered.get_future().wait_for(2s), std::future_status::ready);
  auto destroyed = std::async(std::launch::async, [&] { audit.reset(); });
  EXPECT_EQ(destroyed.wait_for(500ms), std::future_status::ready);
  destroyed.get();
  EXPECT_FALSE(File::Exists(cache));
}

TEST_F(OrcaBootAudit, SessionCancellationReleasesOnlineStartWithoutARoom)
{
  const auto held = std::make_shared<HeldHash>();
  const auto audit = Start(HeldHash::Function(held));
  Activate(audit);
  auto online = std::async(std::launch::async, [] { Orca::Online::StartRoom("cancelled", false); });
  EXPECT_EQ(online.wait_for(30ms), std::future_status::timeout);
  Orca::BootAudit::CancelSession();
  EXPECT_EQ(online.wait_for(500ms), std::future_status::ready);
  online.get();
  EXPECT_FALSE(audit->Wait());
  EXPECT_TRUE(audit->Error().empty());
  EXPECT_EQ(Orca::Online::Transport(), nullptr);
}

TEST_F(OrcaBootAudit, FailedWarmAuditDropsTheRecord)
{
  ASSERT_TRUE(Start()->Wait());
  const auto warm = Start([](const std::string&, const std::atomic<bool>&) { return "mismatch"; });
  EXPECT_TRUE(warm->CacheHit());
  EXPECT_FALSE(warm->Wait());
  EXPECT_EQ(warm->Error(), Entry().refusal);
  const auto next = Start();
  EXPECT_FALSE(next->CacheHit());
  EXPECT_TRUE(next->Wait());
}

TEST_F(OrcaBootAudit, CorruptCacheRecordsAreMisses)
{
  ASSERT_TRUE(Start()->Wait());
  const std::string record = CachePath();
  for (const std::string& damaged :
       std::vector<std::string>{"not a cache", "[Verified]\nVersion = 1\n",
                                "[Verified]\nVersion = 999\n", std::string(16385, 'x')})
  {
    ASSERT_TRUE(File::WriteStringToFile(record, damaged));
    const auto audit = Start();
    EXPECT_FALSE(audit->CacheHit());
    EXPECT_TRUE(audit->Wait());
  }
}

TEST_F(OrcaBootAudit, UnreadableCacheRecordIsAMiss)
{
  ASSERT_TRUE(Start()->Wait());
  const std::string record = CachePath();
  ASSERT_TRUE(File::Delete(record));
  // A non-file at the record path is unopenable as a cache on every supported platform.
  ASSERT_TRUE(File::CreateDir(record));
  const auto audit = Start();
  EXPECT_FALSE(audit->CacheHit());
  EXPECT_TRUE(audit->Wait());
}

#ifndef _WIN32
TEST_F(OrcaBootAudit, PermissionDeniedCacheFileIsAMiss)
{
  ASSERT_TRUE(Start()->Wait());
  const std::string record = CachePath();
  const auto file = StringToPath(record);
  const auto permissions = std::filesystem::status(file).permissions();
  const Common::ScopeGuard restore([&] { std::filesystem::permissions(file, permissions); });
  std::filesystem::permissions(file, std::filesystem::perms::none);
  ASSERT_FALSE(File::IOFile(record, "rb"));
  const auto audit = Start();
  EXPECT_FALSE(audit->CacheHit());
  EXPECT_TRUE(audit->Wait());
}
#endif

TEST_F(OrcaBootAudit, ChangingAFileDuringTheAuditRefusesIt)
{
  const auto audit = Start([&](const std::string&, const std::atomic<bool>&) {
    EXPECT_TRUE(File::WriteStringToFile(path, "changed file"));
    return expected;
  });
  EXPECT_FALSE(audit->Wait());
  EXPECT_EQ(audit->Error(), Entry().refusal);
}

using OrcaBootAuditDeathTest = OrcaBootAudit;

TEST_F(OrcaBootAuditDeathTest, FailureCompletedDuringInitializationStillEndsTheSession)
{
  const ScopedDeathStyle death_style;
  ASSERT_EXIT(
      {
        SetEnv("ORCA_SESSION", "1");
        const auto failed = Start([](const std::string&, const std::atomic<bool>&) { return "mismatch"; });
        Activate(failed);
        if (failed->Wait())
          std::_Exit(1);
        // Registration after completion is the Core::Init race. No online start is needed.
        Orca::BootAudit::WatchSession(failed);
        Core::HostDispatchJobs(Core::System::GetInstance());
        std::_Exit(Orca::Status::ExitCode() == 1 && !Orca::Online::Transport() ? 0 : 1);
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(OrcaBootAudit, AllLauncherFilesAreAuditedOnEveryStart)
{
  const std::string card = dir + "/sd.raw";
  ASSERT_TRUE(File::WriteStringToFile(card, "card fixture"));
  const std::vector<FileToVerify> files{Entry(), {card, Orca::HashFile(card), "card refusal"}};
  EXPECT_TRUE(Audit::Start(files, cache)->Wait());
  std::atomic<int> calls = 0;
  const auto warm = Audit::Start(files, cache, [&](const std::string& file, const std::atomic<bool>& stop) {
    ++calls;
    return Orca::HashFile(file, &stop);
  });
  EXPECT_TRUE(warm->CacheHit());
  EXPECT_TRUE(warm->Wait());
  EXPECT_EQ(calls, 2);
}

TEST_F(OrcaBootAudit, CompatibilityKeyUsesExpectedHashesWithOrWithoutTheCache)
{
  Activate(nullptr);
  const auto parts = Orca::Online::CompatibilityKeyParts();
  EXPECT_NE(parts.find("launcher=" + expected + "|disc=RSBE01|sd=expected-card-hash|"),
            std::string::npos);
  const auto key = Orca::Online::CompatibilityKey();
  const auto cold = Start();
  Activate(cold);
  ASSERT_TRUE(cold->Wait());
  EXPECT_EQ(Orca::Online::CompatibilityKeyParts(), parts);
  EXPECT_EQ(Orca::Online::CompatibilityKey(), key);
  const auto held = std::make_shared<HeldHash>();
  const auto warm = Start(HeldHash::Function(held));
  Activate(warm);
  EXPECT_TRUE(warm->CacheHit());
  EXPECT_EQ(Orca::Online::CompatibilityKeyParts(), parts);
  EXPECT_EQ(Orca::Online::CompatibilityKey(), key);
  held->release.set_value(true);
  EXPECT_TRUE(warm->Wait());
  EXPECT_EQ(Orca::Online::CompatibilityKey(), key);
}

TEST_F(OrcaBootAuditDeathTest, FailedAuditRefusesStartAndStartRoomWithoutARoom)
{
  const ScopedDeathStyle death_style;
  // Isolate Status::Error's process-wide exit status from other tests.
  ASSERT_EXIT(
      {
        SetEnv("ORCA_SESSION", "1");
        std::FILE* const output = std::tmpfile();
        if (!output)
          std::_Exit(1);
        Orca::Status::SetOutput(output);
        const auto failed = Start([](const std::string&, const std::atomic<bool>&) { return "mismatch"; });
        Activate(failed);
        Orca::BootAudit::WatchSession(failed);
        Orca::Online::Start();
        Orca::Online::StartRoom("auditbad", true);
        Core::HostDispatchJobs(Core::System::GetInstance());
        Orca::BootAudit::CancelSession();
        failed->ReportFailure();
        std::rewind(output);
        std::string lines;
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), output))
          lines += buffer;
        std::_Exit(!Orca::Online::Transport() && Orca::Status::ExitCode() == 1 &&
                          failed->Error() == Entry().refusal &&
                          lines == "orca error profile " + Entry().refusal + "\n"
                      ? 0
                      : 1);
      },
      testing::ExitedWithCode(0), "");
}

namespace
{
class OrcaBootAuditRoom : public OrcaBootAudit
{
protected:
  void SetUp() override
  {
    OrcaBootAudit::SetUp();
    url = Orca::GetEnv("ORCA_TEST_ROOM_SERVER");
    if (!url.starts_with("http://127.0.0.1:"))
      GTEST_SKIP() << "run through Tools/orca/room-fix-tests.py";
    old_site = Orca::GetEnv("ORCA_SITE");
    old_game = Orca::GetEnv("ORCA_TEST_DEV_GAME");
    old_room = Orca::GetEnv("ORCA_ROOM");
    old_direct = Orca::GetEnv("ORCA_DIRECT");
    configured = true;
    SetEnv("ORCA_SITE", url);
    SetEnv("ORCA_TEST_DEV_GAME", "room-fixture");
    SetEnv("ORCA_ROOM", "auditpass");
    SetEnv("ORCA_DIRECT", "0");
  }
  void TearDown() override
  {
    OrcaBootAudit::TearDown();
    if (configured)
    {
      SetEnv("ORCA_SITE", old_site);
      SetEnv("ORCA_TEST_DEV_GAME", old_game);
      SetEnv("ORCA_ROOM", old_room);
      SetEnv("ORCA_DIRECT", old_direct);
    }
  }
  picojson::value Request(const std::string& resource, bool post = false)
  {
    Common::HttpRequest http(2s);
    const auto response = post ? http.Post(url + resource, "{}") : http.Get(url + resource);
    EXPECT_TRUE(response);
    picojson::value value;
    if (response)
      EXPECT_TRUE(picojson::parse(value, std::string(response->begin(), response->end())).empty());
    return value;
  }
  void CheckStart(bool by_code, bool pass = true)
  {
    Request("/configure/auditpass", true);
    ASSERT_TRUE(Start()->Wait());
    const auto held = std::make_shared<HeldHash>();
    const auto audit = Start(HeldHash::Function(held));
    Activate(audit);
    ASSERT_TRUE(audit->CacheHit());
    std::promise<void> starting;
    auto has_started = starting.get_future();
    auto start = std::async(std::launch::async, [&starting, by_code] {
      starting.set_value();
      if (by_code)
        Orca::Online::StartRoom("auditpass", false);
      else
        Orca::Online::Start();
    });
    EXPECT_EQ(has_started.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(start.wait_for(50ms), std::future_status::timeout);
    auto accessors = std::async(std::launch::async, [] {
      return Orca::Online::Transport() == nullptr && Orca::Online::Seat() == -1 &&
             Orca::Online::SeatName(0).empty() && Orca::Online::HelloControls().empty();
    });
    const bool responsive = accessors.wait_for(100ms) == std::future_status::ready;
    EXPECT_TRUE(responsive);
    if (responsive)
      EXPECT_TRUE(accessors.get());
    const auto pending = Request("/state/auditpass");
    EXPECT_EQ(pending.get("tickets").get<double>(), 0);
    EXPECT_EQ(pending.get("connects").get<double>(), 0);
    held->release.set_value(pass);
    start.get();
    if (!responsive)
      accessors.get();
    if (!pass)
    {
      EXPECT_EQ(Orca::Online::Transport(), nullptr);
      const auto refused = Request("/state/auditpass");
      EXPECT_EQ(refused.get("tickets").get<double>(), 0);
      EXPECT_EQ(refused.get("connects").get<double>(), 0);
      EXPECT_EQ(audit->Error(), Entry().refusal);
      return;
    }
    EXPECT_NE(Orca::Online::Transport(), nullptr);
    double tickets = 0;
    for (int attempt = 0; attempt < 100 && tickets == 0; ++attempt)
    {
      tickets = Request("/state/auditpass").get("tickets").get<double>();
      if (tickets == 0)
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_GT(tickets, 0);
  }
  void CheckWatch(bool by_ticket, bool pass, bool abandon = false)
  {
    const auto old_source = Orca::GetEnv("ORCA_REPLAY_SOURCE");
    const auto old_input = Orca::GetEnv("ORCA_TEST_REPLAY_IN");
    const auto old_origin = Orca::GetEnv("ORCA_REPLAY_ROOMS_ORIGIN");
    const Common::ScopeGuard restore([&] {
      SetEnv("ORCA_REPLAY_SOURCE", old_source);
      SetEnv("ORCA_TEST_REPLAY_IN", old_input);
      SetEnv("ORCA_REPLAY_ROOMS_ORIGIN", old_origin);
    });
    SetEnv("ORCA_REPLAY_SOURCE", by_ticket ? "" : "ws://" + url.substr(7) + "/watch/auditpass");
    SetEnv("ORCA_TEST_REPLAY_IN", by_ticket ? "watch:auditpass" : "");
    SetEnv("ORCA_REPLAY_ROOMS_ORIGIN", url);
    Request("/configure/auditpass", true);
    ASSERT_TRUE(Start()->Wait());
    const auto held = std::make_shared<HeldHash>();
    const auto audit = Start(HeldHash::Function(held));
    Activate(audit);
    ASSERT_TRUE(audit->CacheHit());
    auto player = Rollback::ReplayStream::Player::FromEnv();
    ASSERT_NE(player, nullptr);
    std::atomic<bool> stop{false};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::string stopped;
    Rollback::ReplayStream::PlayerHost host;
    host.stop = [&](const std::string& why) { stopped = why; };
    host.stopping = [&] { return stop || std::chrono::steady_clock::now() >= deadline; };
    auto playing = std::async(std::launch::async, [&] {
      int running = -1;
      player->Step(Core::System::GetInstance(), host, &running, 0, 0, {});
    });
    EXPECT_EQ(playing.wait_for(100ms), std::future_status::timeout);
    const auto pending = Request("/state/auditpass");
    EXPECT_EQ(pending.get("tickets").get<double>(), 0);
    EXPECT_EQ(pending.get("connects").get<double>(), 0);
    if (abandon)
    {
      stop = true;
      playing.get();
      auto destroyed = std::async(std::launch::async, [&] { player.reset(); });
      EXPECT_EQ(destroyed.wait_for(500ms), std::future_status::ready);
      // Always release the fixture, so a regression fails without hanging the suite.
      held->release.set_value(true);
      destroyed.get();
      EXPECT_TRUE(audit->Wait());
      const auto abandoned = Request("/state/auditpass");
      EXPECT_EQ(abandoned.get("tickets").get<double>(), 0);
      EXPECT_EQ(abandoned.get("connects").get<double>(), 0);
      return;
    }
    held->release.set_value(pass);
    if (pass)
    {
      double connects = 0;
      for (int attempt = 0; attempt < 100 && connects == 0; ++attempt)
      {
        connects = Request("/state/auditpass").get("connects").get<double>();
        if (connects == 0)
          std::this_thread::sleep_for(10ms);
      }
      EXPECT_GT(connects, 0);
      EXPECT_EQ(Request("/state/auditpass").get("tickets").get<double>(), by_ticket ? 1 : 0);
    }
    else
    {
      EXPECT_EQ(playing.wait_for(2s), std::future_status::ready);
    }
    stop = true;
    playing.get();
    if (!pass)
    {
      EXPECT_TRUE(player->Over());
      EXPECT_EQ(stopped, "The replay couldn't be played: " + Entry().refusal);
      const auto refused = Request("/state/auditpass");
      EXPECT_EQ(refused.get("tickets").get<double>(), 0);
      EXPECT_EQ(refused.get("connects").get<double>(), 0);
    }
  }
  std::string url, old_site, old_game, old_room, old_direct;
  bool configured = false;
};
}  // namespace

TEST_F(OrcaBootAuditRoom, StartCannotRequestATicketUntilTheAuditPasses)
{
  CheckStart(false);
}

TEST_F(OrcaBootAuditRoom, StartRoomCannotOpenARoomUntilTheAuditPasses)
{
  CheckStart(true);
}

TEST_F(OrcaBootAuditRoom, MismatchCannotRequestATicketOrOpenARoom)
{
  CheckStart(false, false);
  CheckStart(true, false);
}

TEST_F(OrcaBootAuditRoom, ProductWatchWaitsBeforeConnecting)
{
  CheckWatch(false, true);
}

TEST_F(OrcaBootAuditRoom, DevWatchWaitsBeforeRequestingATicket)
{
  CheckWatch(true, true);
}

TEST_F(OrcaBootAuditRoom, FailedAuditNeverRequestsAWatchTicketOrConnects)
{
  CheckWatch(false, false);
  CheckWatch(true, false);
}

TEST_F(OrcaBootAuditRoom, WatchDestructionDoesNotWaitForTheAudit)
{
  CheckWatch(false, true, true);
  CheckWatch(true, true, true);
}

using OrcaBootAuditRoomDeathTest = OrcaBootAuditRoom;

TEST_F(OrcaBootAuditRoomDeathTest, FailedWatchAuditReportsTheProfileRefusalOnce)
{
  const ScopedDeathStyle death_style;
  ASSERT_EXIT(
      {
        SetEnv("ORCA_SESSION", "1");
        std::FILE* const output = std::tmpfile();
        if (!output)
          std::_Exit(1);
        Orca::Status::SetOutput(output);
        CheckWatch(false, false);
        CheckWatch(true, false);
        Orca::BootAudit::CancelSession();
        std::rewind(output);
        std::string lines;
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), output))
          lines += buffer;
        const std::string refusal = "orca error profile " + Entry().refusal + "\n";
        const auto first = lines.find(refusal);
        std::_Exit(!HasFailure() && Orca::Status::ExitCode() == 1 &&
                          first != std::string::npos &&
                          lines.find("orca error ", first + refusal.size()) == std::string::npos
                      ? 0
                      : 1);
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(OrcaBootAuditRoomDeathTest, ChangedBytesWithMatchingCacheIdentityRefuseBeforeOnline)
{
  const ScopedDeathStyle death_style;
  ASSERT_EXIT(
      {
        SetEnv("ORCA_SESSION", "1");
        Request("/configure/auditpass", true);
        if (!Start()->Wait() || !File::WriteStringToFile(path, "other launcher fixture"))
          std::_Exit(1);
        MatchCacheToCurrentIdentity();
        const std::string record = CachePath();
        const auto held = std::make_shared<HeldHash>();
        const auto audit = Start(HeldHash::Function(held));
        Activate(audit);
        Orca::BootAudit::WatchSession(audit);
        if (!audit->CacheHit())
          std::_Exit(1);
        auto start = std::async(std::launch::async, [] { Orca::Online::Start(); });
        if (held->entered.get_future().wait_for(2s) != std::future_status::ready ||
            start.wait_for(50ms) != std::future_status::timeout)
          std::_Exit(1);
        const auto pending = Request("/state/auditpass");
        if (pending.get("tickets").get<double>() != 0 || pending.get("connects").get<double>() != 0)
          std::_Exit(1);
        held->release.set_value(true);  // Full HashFile over the actual changed bytes.
        start.get();
        Orca::Online::StartRoom("auditpass", false);
        Core::HostDispatchJobs(Core::System::GetInstance());
        const auto refused = Request("/state/auditpass");
        const bool dropped = !File::Exists(record);
        std::_Exit(!audit->Wait() && audit->Error() == Entry().refusal &&
                          Orca::Status::ExitCode() == 1 && !Orca::Online::Transport() && dropped &&
                          refused.get("tickets").get<double>() == 0 &&
                          refused.get("connects").get<double>() == 0
                      ? 0
                      : 1);
      },
      testing::ExitedWithCode(0), "");
}
