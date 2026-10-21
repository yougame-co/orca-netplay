// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Online.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Orca/Bridge.h"
#include "Core/Orca/Engine/Module.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Rollback/ReplayStream.h"

namespace Orca::Online
{
namespace
{
// How long Shutdown waits for rooms still being left in the background (goodbye, close handshake
// and emptying the page's room take about a second).
constexpr auto TEARDOWN_WAIT = std::chrono::seconds(2);
std::mutex s_mutex;
// Shared so a caller still holding the room (WaitForMatch, a status poll) keeps it alive while
// Shutdown runs on another thread.
std::shared_ptr<Net::YouGameRoom> s_room;

std::string Env(const char* name)
{
  return GetEnv(name);
}

std::shared_ptr<Net::YouGameRoom> Room()
{
  std::lock_guard lock(s_mutex);
  return s_room;
}
}  // namespace

bool Enabled()
{
  if (!SessionActive())
    return false;
  // Replay boots use the same four-port profile and boundary dispatcher as online matches,
  // while StartLocked refuses to create a room for them.
  if (Rollback::ReplayStream::ProductMode())
    return true;
  return Bridge::Ready() || (!Env("YOUGAME_BRIDGE").empty() && !Env("YOUGAME_TOKEN").empty()) ||
         !Env("ORCA_TEST_DEV_GAME").empty();
}

namespace
{
// The UX version for the running game: the engine's, and the game module's own revision.
std::string RunningUxKeyPart()
{
  Engine::Module* module = Engine::ActiveModule();
  return UxKeyPart(Events::UXCompatVersion(), module ? module->Describe().compat : 0);
}

// The universal forced settings that change what the game computes, the seeded save and whether test
// overrides are on: the tail of both key versions.
std::string UniversalKeyParts()
{
  return fmt::format(
      "thread={}|fma={}|rtc={}:{}|si={},{},{},{}|efb={}|xfb={}|access={}|bbox={}|save={:016x}|test={}"
      "|sdcard={}:{}",
      Config::Get(Config::MAIN_CPU_THREAD), Config::Get(Config::SESSION_USE_FMA),
      Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE), Config::Get(Config::MAIN_CUSTOM_RTC_VALUE),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(0))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(1))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(2))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(3))),
      Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE), Config::Get(Config::GFX_HACK_BBOX_ENABLE),
      GetSeedSaveHash(), TestOverridesActive(), Config::Get(Config::MAIN_WII_SD_CARD),
      Config::Get(Config::MAIN_ALLOW_SD_WRITES));
}

// Mod mode (Core/Orca/Mod.h, YouGame docs/ORCA_MODS.md "The key in mod mode"): the engine's commit
// and API version, the bundle's tree hash (mod.json, the patches, textures, launcher and SD files:
// everything but orca/jitwarm and orca/test/), the SD image's hash, the disc, the Sys/GameSettings
// INIs Dolphin loads for the game (as the legacy key hashes them: an install whose copy was edited
// meets nobody rather than desyncing), then the forced settings as the legacy key adds them. The
// player's own User/GameSettings INIs are ignored in a mod session
// (ConfigLoaders/GameConfigLoader.cpp), so they are not in it.
std::string ModKeyParts(const Profile& profile)
{
  const Mod::ModInfo& mod = *profile.mod;
  const std::string disc = Env("ORCA_DISC_SHA1");
  std::string parts = fmt::format("orca2|{}|api={}|tree={}|sd={}|disc={}|sys=",
                                  Common::GetScmRevGitStr(), Mod::ENGINE_API, mod.tree.hash,
                                  mod.sd_image_hash.empty() ? "-" : mod.sd_image_hash,
                                  disc.empty() ? "-" : disc);
  // A launcher profile runs under two game IDs, its loader's and the disc's (as below).
  std::vector<std::pair<std::string, u16>> ini_games{{profile.game_id, profile.revision}};
  if (profile.IsLauncher())
  {
    const SConfig& running = SConfig::GetInstance();
    ini_games = {{profile.boot_game_id, 0},
                 {profile.disc, profile.revision},
                 {running.GetGameID(), running.GetRevision()}};
  }
  for (const auto& [ini_game, ini_revision] : ini_games)
  {
    for (const std::string& name : ConfigLoaders::GetGameIniFilenames(ini_game, ini_revision))
    {
      std::string contents;
      if (File::ReadFileToString(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name,
                                 contents))
      {
        parts += fmt::format("{:016x},", XXH3_64bits(contents.data(), contents.size()));
      }
      else
      {
        parts += "-,";
      }
    }
  }
  parts += '|';
  for (const auto& [location, value] : profile.forced)
    parts += fmt::format("{}.{}.{}={}|", static_cast<int>(location.system), location.section,
                         location.key, value);
  parts += UniversalKeyParts();
  parts += RunningUxKeyPart();
  return parts;
}
}  // namespace

std::string UxKeyPart(int engine, int game)
{
  return game > 0 ? fmt::format("|ux={}.{}", engine, game) : fmt::format("|ux={}", engine);
}

std::string CompatibilityKeyParts()
{
  if (const Profile* profile = ActiveProfile(); profile && profile->mod)
    return ModKeyParts(*profile);
  std::string parts = fmt::format("orca|{}|", Common::GetScmRevGitStr());
  if (const Profile* profile = ActiveProfile())
  {
    parts += fmt::format("{}|{}|", profile->game_id, profile->revision);
    // Data files that affect emulation besides code: the profile and every game settings INI
    // Dolphin loads for this disc, shipped or the player's own (a local edit changes what the game
    // runs with).
    std::vector<std::string> files{File::GetSysDirectory() + "Orca/" + profile->game_id + ".ini",
                                   // Orca's forced game patches (UX/GamePatches.h)
                                   File::GetSysDirectory() + "Orca/" + profile->game_id + ".patches"};
    // A launcher profile runs under two game IDs: its loader's, then the disc's.
    std::vector<std::pair<std::string, u16>> ini_games{{profile->game_id, profile->revision}};
    if (profile->IsLauncher())
    {
      // The running metadata comes from the disc's TMD, whose version can differ from the header's
      // revision: hash the INIs for both.
      const SConfig& running = SConfig::GetInstance();
      ini_games = {{profile->boot_game_id, 0},
                   {profile->disc, profile->revision},
                   {running.GetGameID(), running.GetRevision()}};
    }
    for (const auto& [ini_game, ini_revision] : ini_games)
    {
      for (const std::string& name : ConfigLoaders::GetGameIniFilenames(ini_game, ini_revision))
      {
        files.push_back(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name);
        files.push_back(File::GetUserPath(D_GAMESETTINGS_IDX) + name);
      }
    }
    // The loader and SD card the boot verified. Their hashes are in the profile too, but stating
    // them here keeps the key independent of the profile file.
    if (profile->IsLauncher())
    {
      parts += fmt::format("launcher={}|disc={}|sd={}|", profile->launcher_hash, profile->disc,
                           profile->sd_image_hash);
    }
    for (const std::string& path : files)
    {
      std::string contents;
      if (File::ReadFileToString(path, contents))
        parts += fmt::format("{:016x}|", XXH3_64bits(contents.data(), contents.size()));
      else
        parts += "-|";
    }
    for (const auto& [location, value] : profile->forced)
      parts += fmt::format("{}.{}.{}={}|", static_cast<int>(location.system), location.section,
                           location.key, value);
  }
  // The host CPU is not in the key: JitArm64 and Jit64 produce identical results, so Mac and PC
  // players meet. FMA is in the key (below).
  //
  // Nor is whether an ARM CPU has FEAT_AFP: sessions force accurate NaNs and keep denormal single
  // inputs exact on CPUs that flush them (Profile.cpp MAIN_ACCURATE_NANS,
  // JitArm64::DenormalInputsFlushed), so every Mac and PC computes the same. ORCA_TEST_NO_AFP
  // simulates an M1 to M3 on a newer Mac.
  //
  // The universal forced settings that change what the game computes.
  parts += UniversalKeyParts();
  // The disc image's hash, when the app computed it: a modified disc with the right ID and revision
  // would otherwise desync instead of being refused. For a dump AliasRevision plays as another
  // (Brawl Rev 1), the app passes the hash of the dump it plays as, or its players meet no one.
  parts += "|disc=" + Env("ORCA_DISC_SHA1");
  // Version of what the in-game UX writes to emulated memory (name tags, mode locks).
  parts += RunningUxKeyPart();
  return parts;
}

std::string CompatibilityKey()
{
  const std::string parts = CompatibilityKeyParts();
  // "orca2:" is the room protocol's prefix, in legacy and mod mode alike; the two modes never share
  // a hash: legacy parts start "orca|", mod parts "orca2|" (ModKeyParts). The room allows 1-64
  // characters of [a-zA-Z0-9._:-].
  const std::string key = fmt::format("orca2:{:016x}", XXH3_64bits(parts.data(), parts.size()));
  // What went into it, when that changes: a player alone in a queue shows which input split them.
  {
    static std::mutex logged_mutex;
    static std::string logged;
    std::lock_guard lock(logged_mutex);
    if (parts != logged)
    {
      logged = parts;
      NOTICE_LOG_FMT(NETPLAY, "Orca: compatibility {} from {}", key, parts);
    }
  }
  return key;
}

namespace
{
void StartLocked(Net::RoomOptions options)
{
  if (Rollback::ReplayStream::ProductMode())
    return;
  options.compatibility = CompatibilityKey();
  if (const Profile* profile = ActiveProfile())
  {
    std::string game = profile->game_id;
    Common::ToLower(&game);
    options.mode = "orca-" + game;
  }
  if (!Env("ORCA_NAME").empty())
    options.player_name = Env("ORCA_NAME").substr(0, 32);
  // A joiner's own controls, for its port in the host's game (none for a launch join, whose game
  // hasn't run yet).
  if (options.joining)
  {
    options.controls = Events::OwnControls();
    options.queue = Events::OwnQueue();
  }
  NOTICE_LOG_FMT(NETPLAY, "Orca: joining a YouGame room ({}), compatibility {}",
                 options.joining ? "to join a friend's game" : "as the host",
                 options.compatibility);
  s_room = std::make_shared<Net::YouGameRoom>(std::move(options));
}
}  // namespace

void Start()
{
  std::lock_guard lock(s_mutex);
  if (s_room)
    return;
  Net::RoomOptions options;
  options.room_code = Env("ORCA_ROOM");
  options.joining = Env("ORCA_JOIN") == "1";
  StartLocked(std::move(options));
}

void StartRoom(const std::string& code, bool joining, bool friends_room)
{
  std::lock_guard lock(s_mutex);
  if (s_room)
    return;
  Net::RoomOptions options;
  options.room_code = code;
  options.joining = joining;
  options.friends_room = friends_room;
  options.ask_app_for_room = false;
  StartLocked(std::move(options));
}

std::vector<u8> HelloControls()
{
  const auto room = Room();
  return room ? room->HelloControls() : std::vector<u8>{};
}

std::vector<u8> HelloQueue()
{
  const auto room = Room();
  return room ? room->HelloQueue() : std::vector<u8>{};
}

std::string SeatName(int seat)
{
  const auto room = Room();
  return room ? room->SeatName(seat) : "";
}

std::string Code()
{
  const auto room = Room();
  return room ? room->Code() : "";
}

namespace
{
std::mutex s_request_mutex;
std::optional<std::string> s_join_request;
std::optional<std::string> s_host_request;
bool s_leave_request = false;
std::optional<int> s_chosen_delay;
std::atomic<bool> s_direct_wanted{true};
std::atomic<RecordChoice> s_record_choice{RecordChoice::Auto};
}  // namespace

void RequestJoin(const std::string& code)
{
  std::lock_guard lock(s_request_mutex);
  s_join_request = code;
}

std::optional<std::string> TakeJoinRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_join_request, std::nullopt);
}

void RequestHost(const std::string& code)
{
  std::lock_guard lock(s_request_mutex);
  s_host_request = code;
}

std::optional<std::string> TakeHostRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_host_request, std::nullopt);
}

std::string RoomQueue()
{
  const auto room = Room();
  return room ? room->Queue() : "private";
}

bool RoomDoubles()
{
  const auto room = Room();
  return room && room->Doubles();
}

bool MatchLive()
{
  const auto room = Room();
  return room && room->MatchLive();
}

bool SetOver()
{
  const auto room = Room();
  return room && room->GetState() != Net::RoomState::Ended && room->SetOver();
}

void SetOpponentPlugged(bool plugged)
{
  if (const auto room = Room())
    room->SetOpponentPlugged(plugged);
}

void ReportGame(const Net::GameReport& report)
{
  if (const auto room = Room())
    room->ReportGame(report);
}

void ReportGameStart(int start)
{
  if (const auto room = Room())
    room->ReportGameStart(start);
}

int KnownSetGames()
{
  const auto room = Room();
  return room ? room->KnownSetGames() : 0;
}

void ReportPeerFault()
{
  if (const auto room = Room())
    room->ReportFault();
}

void KeepAlive()
{
  if (const auto room = Room())
    room->KeepAlive();
}

void SendReplayStream(const std::vector<u8>& bytes)
{
  if (const auto room = Room())
    room->SendReplayStream(bytes);
}

void SendReplayPacked(const std::vector<std::vector<u8>>& pieces, int frames, u64 generation)
{
  if (const auto room = Room(); room && room->Generation() == generation)
    room->SendReplayPacked(pieces, frames);
}

u64 RoomGeneration()
{
  const auto room = Room();
  return room ? room->Generation() : 0;
}

void ReportDesync()
{
  if (const auto room = Room())
    room->ReportDesync();
}

void ReportStall()
{
  if (const auto room = Room(); room && room->Queue() == "ranked" && room->MatchLive())
    room->ReportStall();
}

void RequestLeave()
{
  std::lock_guard lock(s_request_mutex);
  s_leave_request = true;
}

bool TakeLeaveRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_leave_request, false);
}

void SetDirectWanted(bool on)
{
  s_direct_wanted = on;
}

bool DirectWanted()
{
  return s_direct_wanted;
}

void SetRecordChoice(RecordChoice choice)
{
  s_record_choice = choice;
}

bool RecordWanted()
{
  const RecordChoice choice = s_record_choice;
  if (choice == RecordChoice::Off || !Status::Cap(Status::RECORD_CAP))
    return false;
  const auto room = Room();
  if (!room)
    return false;
  return choice == RecordChoice::On || room->Queue() == "ranked";
}

void SetChosenDelay(std::optional<int> frames)
{
  std::lock_guard lock(s_request_mutex);
  s_chosen_delay = frames;
}

std::optional<int> ChosenDelay()
{
  std::lock_guard lock(s_request_mutex);
  return s_chosen_delay;
}

std::optional<bool> Joining()
{
  const auto room = Room();
  return room ? room->Joining() : std::nullopt;
}

Net::Transport* Transport()
{
  const auto room = Room();
  return room.get();
}

int Seat()
{
  const auto room = Room();
  return room ? room->Seat() : -1;
}

std::vector<Net::PeerEvent> TakePeerEvents()
{
  const auto room = Room();
  return room ? room->TakePeerEvents() : std::vector<Net::PeerEvent>{};
}

bool HostLeftPending()
{
  const auto room = Room();
  return room && room->HostLeftPending();
}

std::vector<int> LeftSeatsPending()
{
  const auto room = Room();
  return room ? room->LeftSeatsPending() : std::vector<int>{};
}

std::optional<std::chrono::steady_clock::time_point> HostAwayDeadline()
{
  const auto room = Room();
  return room ? room->HostAwayDeadline() : std::nullopt;
}

void RetryPeer(int seat)
{
  if (const auto room = Room())
    room->RetryPeer(seat);
}

bool HoldInputSilence(std::chrono::steady_clock::time_point since,
                       std::chrono::steady_clock::time_point now,
                       std::optional<std::vector<int>> missing)
{
  const auto room = Room();
  return room && room->HoldInputSilence(since, now, std::move(missing));
}

bool RoomRecovery()
{
  const auto room = Room();
  return room && room->RoomRecovery();
}

bool RoomRecovering(std::chrono::steady_clock::time_point now)
{
  const auto room = Room();
  return room && room->RoomRecovering(now);
}

bool RoomEnded()
{
  const auto room = Room();
  return !room || room->GetState() == Net::RoomState::Ended;
}

bool InRoom()
{
  const auto room = Room();
  return room && room->Connected();
}

bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error)
{
  const auto room = Room();
  if (!room)
  {
    *error = "not in a room";
    return false;
  }
  return room->FreshTicket(fresh, ticket, store_url, error);
}

void DropPeer(int seat, const std::string& reason)
{
  if (const auto room = Room())
    room->DropPeer(seat, reason);
}

void HoldJoins(bool holding)
{
  if (const auto room = Room())
    room->HoldJoins(holding);
}

bool IsDesync(const std::string& session_error)
{
  return session_error.starts_with("Desync") ||
         session_error.find("changed its input") != std::string::npos ||
         session_error.find("arrived after") != std::string::npos ||
         session_error.find("history changed") != std::string::npos ||
         session_error.starts_with("No local input");
}

bool IsRollbackFailure(const std::string& session_error)
{
  return session_error.starts_with("No snapshot to roll back") ||
         session_error.find("exceeds the limit") != std::string::npos;
}

void BroadcastNames(const std::vector<Net::KeyframeInfo::Name>& names, int version)
{
  if (const auto room = Room())
    room->BroadcastNames(names, version);
}

namespace
{
// Rooms being left in the background, until their goodbyes are sent. Never destroyed, so a teardown
// still running at process exit can't find them gone.
struct Teardowns
{
  std::mutex mutex;
  std::condition_variable done;
  int running = 0;
};
Teardowns& TheTeardowns()
{
  static Teardowns* const teardowns = new Teardowns;
  return *teardowns;
}
}  // namespace

bool RoomsLeaving()
{
  Teardowns& teardowns = TheTeardowns();
  std::lock_guard lock(teardowns.mutex);
  return teardowns.running > 0;
}

void ShutdownInBackground()
{
  std::shared_ptr<Net::YouGameRoom> room;
  {
    std::lock_guard lock(s_mutex);
    room = std::move(s_room);
  }
  if (!room)
    return;
  Teardowns& teardowns = TheTeardowns();
  {
    std::lock_guard lock(teardowns.mutex);
    ++teardowns.running;
  }
  std::thread([leaving_room = std::move(room), &teardowns]() mutable {
    leaving_room->Leave();
    leaving_room.reset();
    std::lock_guard lock(teardowns.mutex);
    --teardowns.running;
    teardowns.done.notify_all();
  }).detach();
}

void OfferKeyframe(int seat, const Net::KeyframeInfo& info)
{
  if (const auto room = Room())
    room->OfferKeyframe(seat, info);
}

namespace
{
std::atomic<bool> s_prepare_join{false};
}

void PrepareJoin()
{
  s_prepare_join.store(true, std::memory_order_relaxed);
}

bool TakePrepareJoin()
{
  return s_prepare_join.exchange(false, std::memory_order_relaxed);
}

bool DropInPending()
{
  // Mirrors what the boundary will take: join or leave once the app's caps allow them (OnlineMatch
  // TakeCommands), an invite or arrival only while the room is up (HostEvents).
  {
    std::lock_guard lock(s_request_mutex);
    if ((s_join_request && Status::Cap("join")) || (s_leave_request && Status::Cap("leave")) ||
        (s_host_request && Status::Cap("host")))
    {
      return true;
    }
  }
  const auto room = Room();
  if (!room || room->GetState() == Net::RoomState::Ended)
    return false;
  return s_prepare_join.load(std::memory_order_relaxed) || room->ArrivalPending();
}

bool ArrivalPending()
{
  const auto room = Room();
  return room && room->GetState() != Net::RoomState::Ended && room->ArrivalPending();
}

int RoundTripMs(u32* sequence)
{
  const auto room = Room();
  return room ? room->RoundTripMs(sequence) : -1;
}

Net::DirectSummary Direct()
{
  const auto room = Room();
  return room ? room->Direct() : Net::DirectSummary{};
}

std::vector<int> RecentRoundTrips(u32* sequence)
{
  const auto room = Room();
  return room ? room->RecentRoundTrips(sequence) : std::vector<int>{};
}

namespace
{
// The room's code as the app knows it: "taken" is the game's (it opens another room).
std::string AppCode(std::string code)
{
  return code == "taken" ? "network" : code;
}
}  // namespace

std::string RoomErrorCode()
{
  const auto room = Room();
  return room ? AppCode(room->ErrorCode()) : "";
}

bool RoomTaken()
{
  const auto room = Room();
  return room && room->ErrorCode() == "taken";
}

std::string StatusLine()
{
  const auto room = Room();
  return room ? room->Status() : "";
}

bool ReportSessionEnd(const std::string& session_error, bool from_peer)
{
  const auto room = Room();
  // Disagreeing inputs or checksums: the games desynced. Tell the friend before leaving, so both
  // report the same thing.
  if (IsDesync(session_error))
  {
    if (room)
    {
      room->ReportDesync();
      room->SetLeaveReason("desync");
    }
    Status::Report("desync", "Your games went out of sync, so the match ended");
    return true;
  }
  // A rollback this machine couldn't make (an engine fault, not a desync): leave the match and play
  // on from here. Worded distinctly so logs tell it from a desync.
  if (IsRollbackFailure(session_error))
  {
    Status::Report(
        "network",
        fmt::format("Your game couldn't roll back ({}), so you left the match", session_error));
    return true;
  }
  // The session ends when the room does, and the room says why unless this player left. Check state
  // first: once Ended, the code is final.
  if (room && room->GetState() == Net::RoomState::Ended)
  {
    // A matchmade room closed after its result: a clean end, reported by the caller.
    if (const std::string code = AppCode(room->ErrorCode()); !code.empty() && code != "match-over")
      Status::Report(code, room->Status());
    return true;
  }
  if (session_error.empty())
    return true;
  // The emulator side failed (a snapshot that wouldn't save or load, lost frame alignment), or the
  // friend sent something an honest Orca never sends (a bad packet, or a host's roster or history
  // that takes back this player's seat): that one is the friend's doing, and this game goes on.
  if (from_peer || session_error.find("sent an invalid packet") != std::string::npos)
  {
    // The room's match hears it void first, so this player's going can't hand it to the friend.
    if (room)
      room->ReportFault();
    Status::Report("network", session_error);
    return true;
  }
  Status::Error("internal", session_error);
  return false;
}

void SetLeaveReason(const std::string& reason)
{
  if (const auto room = Room())
    room->SetLeaveReason(reason);
}

void ReportPeerStalled(std::string_view sentence)
{
  if (const auto room = Room())
    room->SetLeaveReason("stalled");
  Status::Report("peer_left", sentence);
}

void TestDropRoom()
{
  if (const auto room = Room())
  {
    // All recovery rooms retain the same room object; legacy silent drops keep the socket.
    if (!room->RoomRecovery() && Env("ORCA_TEST_DROP_SILENT") == "1")
    {
      static std::vector<std::shared_ptr<Net::YouGameRoom>> s_kept;
      s_kept.push_back(room);
    }
    room->TestDropConnection();
  }
}

void TestDirect(const std::string& command)
{
  if (const auto room = Room())
    room->TestDirect(command);
}

void Shutdown()
{
  std::shared_ptr<Net::YouGameRoom> room;
  {
    std::lock_guard lock(s_mutex);
    room = std::move(s_room);
  }
  if (room)
    room->Leave();
  // Rooms just left in the background (a leave, then an immediate quit) still send their goodbyes,
  // so friends learn right away rather than after a timeout. Bounded, so a hung teardown can't hold
  // the exit.
  Teardowns& teardowns = TheTeardowns();
  std::unique_lock lock(teardowns.mutex);
  if (!teardowns.done.wait_for(lock, TEARDOWN_WAIT, [&] { return teardowns.running == 0; }))
    WARN_LOG_FMT(NETPLAY, "Orca: {} room(s) still leaving at shutdown", teardowns.running);
}
}  // namespace Orca::Online
