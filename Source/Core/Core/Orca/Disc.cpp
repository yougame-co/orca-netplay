// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Disc.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/Orca/Branding.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Sandbox.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

namespace Orca
{
namespace
{
// Super Smash Bros. Brawl (USA) (Rev 1), read as (USA, Canada) (Rev 2). The dumps differ in the
// revision byte of both disc headers and in main.dol at 0x8001BC9C (`li r7,1`; Rev 2 `li r7,2`).
// Their TMDs and tickets differ only in hashes, signatures and the ticket ID, which the game
// doesn't read (ORCA.md, "Orca.app and the disc").
constexpr std::string_view BRAWL_USA_ID = "RSBE01";
constexpr u16 BRAWL_REV1 = 1;
constexpr u16 BRAWL_REV2 = 2;
// The game partition's content SHA-1 in the Rev 1 TMD: only this exact dump is aliased.
constexpr std::array<u8, 20> BRAWL_REV1_CONTENT_SHA1 = {0xab, 0x93, 0xef, 0x28, 0x0b, 0x83, 0x45,
                                                        0x72, 0x52, 0x3f, 0x64, 0xc5, 0x5c, 0x6a,
                                                        0x32, 0x05, 0xea, 0xd6, 0x6d, 0x04};
// main.dol starts at 0x3DD00 in the partition; 0x8001BC9C's low byte is 0x11A1F into it.
constexpr u64 BRAWL_REV1_DOL_VERSION = 0x3DD00 + 0x11A1F;
}  // namespace

bool AliasRevision(DiscIO::VolumeDisc& volume, std::string* reason)
{
  if (reason)
    reason->clear();
  const auto refuse = [reason](std::string why) {
    if (reason)
      *reason = std::move(why);
    return false;
  };
  const auto platform = volume.GetVolumeType();
  if (platform != DiscIO::Platform::WiiDisc)
    return refuse(fmt::format("platform={}; expected WiiDisc", static_cast<int>(platform)));
  const DiscIO::Partition game = volume.GetGamePartition();
  const auto id = volume.GetGameID(game);
  if (id != BRAWL_USA_ID)
    return refuse(fmt::format("GetGameID(game)={}; expected RSBE01", id));
  const auto revision = volume.GetRevision(game);
  if (revision != BRAWL_REV1)
    return refuse(fmt::format("GetRevision(game)={}; expected 1",
                             revision ? std::to_string(*revision) : "missing"));
  const auto& tmd = volume.GetTMD(game);
  if (!tmd.IsValid())
    return refuse("TMD invalid/unparsed");
  const std::vector<IOS::ES::Content> contents = tmd.GetContents();
  if (contents.size() != 1)
    return refuse(fmt::format("contents.size()={}; expected 1", contents.size()));
  if (contents[0].sha1 != BRAWL_REV1_CONTENT_SHA1)
  {
    const auto& sha = contents[0].sha1;
    return refuse(fmt::format("content[0] SHA-1={:02x}{:02x}{:02x}{:02x}; expected ab93ef28",
                             sha[0], sha[1], sha[2], sha[3]));
  }
  const std::vector<DiscIO::VolumeDisc::ReadPatch> patches = {
      {DiscIO::PARTITION_NONE, 7, 1, 2}, {game, 7, 1, 2}, {game, BRAWL_REV1_DOL_VERSION, 1, 2}};
  DiscIO::VolumeDisc::ReadPatchFailure failure;
  if (volume.SetReadPatches(patches, &failure))
    return true;
  if (failure.unsupported)
    return refuse("SetReadPatches unsupported");
  const auto& patch = patches.at(failure.index);
  return refuse(fmt::format(
      "SetReadPatches[{}]: partition={} offset=0x{:x} byte={}; expected 0x{:02x}", failure.index,
      patch.partition == DiscIO::PARTITION_NONE ? "none" : fmt::format("0x{:x}", patch.partition.offset),
      patch.offset, failure.actual ? fmt::format("0x{:02x}", *failure.actual) : "unreadable",
      patch.original));
}

std::string DiscRevisionRefusal(std::string prefix, std::string_view reason)
{
  if (!reason.empty())
    prefix += fmt::format(" ({})", reason);
  return prefix;
}

bool SessionDiscReady(const DiscIO::VolumeDisc& volume)
{
  const DiscIO::Partition game = volume.GetGamePartition();
  return volume.GetGameID(game) != BRAWL_USA_ID || volume.GetRevision(game) == BRAWL_REV2;
}

DiscCheck CheckDisc(const std::string& path, std::string_view game_id)
{
  const auto refuse = [](std::string sentence) {
    return DiscCheck{false, "disc_revision", std::move(sentence)};
  };
  if (path.empty() || !File::Exists(path) || File::IsDirectory(path))
    return refuse("That disc image can't be found.");

  const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(path);
  if (!volume)
    return refuse("That file isn't a disc image YG Dolphin can read.");

  std::string alias_reason;
  AliasRevision(*volume, &alias_reason);
  const DiscIO::Partition partition = volume->GetGamePartition();
  const std::string id = volume->GetGameID(partition);
  const u16 revision = volume->GetRevision(partition).value_or(0);
  if (id != game_id)
  {
    return refuse(fmt::format("That disc{} isn't Super Smash Bros. Brawl for the USA ({}).",
                              id.empty() ? "" : fmt::format(" ({})", id), game_id));
  }

  std::string error;
  const std::optional<Profile> profile = LoadProfile(id, revision, &error);
  if (!profile)
  {
    if (error.find("revision") == std::string::npos)
      return DiscCheck{false, "profile", BrandWords(error)};
    if (revision == BRAWL_REV1)
    {
      return refuse(DiscRevisionRefusal(
          "That Brawl disc is Rev 1 but not an unmodified copy of it. YG Dolphin plays "
          "unmodified USA Rev 1 and Rev 2 discs, so both players have the same game.", alias_reason));
    }
    return refuse(
        fmt::format("That Brawl disc is revision {}; YG Dolphin plays the USA Rev 1 and Rev 2 "
                    "discs, so both players have the same game.",
                    revision));
  }
  return DiscCheck{true, {}, {}};
}

DiscCheck CheckBootFile(const std::string& path)
{
  // Inside the app's sandbox, a file it wasn't given looks missing or unreadable: say it's the
  // sandbox, so the app can tell the player to move the disc (Orca/Sandbox.h).
  const auto refuse = [&path](const char* code) {
    if (!path.empty() && Sandbox::Jailed() && Sandbox::ReadDenied(path))
      return DiscCheck{false, Sandbox::DISC_ERROR, Sandbox::DISC_SENTENCE};
    return DiscCheck{false, code, path};
  };
  if (path.empty() || !File::Exists(path))
    return refuse("disc_missing");
  // A folder (an extracted disc) is left to the boot's own checks.
  if (!File::IsDirectory(path) && !File::IOFile(path, "rb").IsOpen())
    return refuse("disc_unreadable");
  return DiscCheck{true, {}, {}};
}
}  // namespace Orca
