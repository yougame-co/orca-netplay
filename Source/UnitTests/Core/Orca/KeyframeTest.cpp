// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <gtest/gtest.h>
#include <picojson.h>
#include <zstd.h>

#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/SafeJson.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/System.h"

using namespace Orca::Net;

namespace
{
ReplayArchive Replay(size_t count, int first_frame = 0)
{
  ReplayArchive replay;
  replay.first_frame = first_frame;
  replay.origin_hash = 0x123456789abcdef0;
  replay.target_hash = 0xfedcba9876543210;
  auto ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "player", false, {1, 2, 3}, {4, 5}},
                                        {2, "friend", true, {6, 7}, {8}}});
  std::mt19937 rng(1);
  replay.frames.resize(count);
  for (auto& frame : replay.frames)
  {
    for (auto& pad : frame.pads)
      for (auto& byte : pad)
        byte = static_cast<u8>(rng());
    frame.ports = ports;
  }
  replay.boundary.ports = ports;
  replay.boundary.header = ReplayHeader{2, 1, 1, 2, 0x11223344};
  replay.boundary.clear_ready = true;
  return replay;
}

std::vector<u8> Raw(const std::vector<u8>& blob)
{
  std::vector<u8> raw(ZSTD_getFrameContentSize(blob.data(), blob.size()));
  EXPECT_EQ(ZSTD_decompress(raw.data(), raw.size(), blob.data(), blob.size()), raw.size());
  return raw;
}

std::vector<u8> Compressed(const std::vector<u8>& raw)
{
  std::vector<u8> blob(ZSTD_compressBound(raw.size()));
  const auto size = ZSTD_compress(blob.data(), blob.size(), raw.data(), raw.size(), 1);
  EXPECT_FALSE(ZSTD_isError(size));
  blob.resize(size);
  return blob;
}

void U32(std::vector<u8>& raw, size_t offset, u32 value)
{
  std::memcpy(raw.data() + offset, &value, sizeof(value));
}
}  // namespace

TEST(OrcaKeyframe, ReplayRoundTripsInputsAndTypedEvents)
{
  auto original = Replay(4321);
  original.frames[42].header = ReplayHeader{1, 2, 0, 3, 55};
  original.frames[99].clear_ready = true;
  const auto blob = PackKeyframe(4321, original);
  ASSERT_FALSE(blob.empty());
  int frame = -1;
  ReplayArchive decoded;
  ASSERT_TRUE(UnpackKeyframe(blob, &frame, &decoded));
  EXPECT_EQ(frame, 4321);
  EXPECT_EQ(decoded.origin_hash, original.origin_hash);
  EXPECT_EQ(decoded.target_hash, original.target_hash);
  ASSERT_EQ(decoded.frames.size(), original.frames.size());
  for (size_t i = 0; i < decoded.frames.size(); ++i)
  {
    EXPECT_EQ(decoded.frames[i].pads, original.frames[i].pads);
    EXPECT_EQ(decoded.frames[i].header, original.frames[i].header);
    EXPECT_EQ(decoded.frames[i].clear_ready, original.frames[i].clear_ready);
    EXPECT_EQ(decoded.frames[i].ports->at(0).controls, original.frames[i].ports->at(0).controls);
    EXPECT_EQ(decoded.frames[i].ports->at(1).queue, original.frames[i].ports->at(1).queue);
  }
  EXPECT_EQ(decoded.frames[0].ports, decoded.frames[1].ports);
  EXPECT_EQ(decoded.boundary.header, original.boundary.header);
  EXPECT_TRUE(decoded.boundary.clear_ready);
}

// A replay whose history writes another game's ruleset into the match block, even on frames in the
// middle with the room's header back by its end, is refused before it plays: it would select the
// other game's memory writes here.
TEST(OrcaKeyframe, AReplayNeverSetsAnotherGamesRules)
{
  constexpr u8 BRAWL = 1, PPLUS = 2;
  ReplayArchive replay;
  replay.first_frame = 100;
  replay.frames.resize(40);
  replay.frames[5].header = ReplayHeader{2, BRAWL, 0, 0, 1234};
  replay.boundary.header = ReplayHeader{2, BRAWL, 0, 0, 1234};
  EXPECT_TRUE(HeadersFitRuleset(replay, BRAWL));
  // A header that clears the block is fine anywhere.
  replay.frames[9].header = ReplayHeader{};
  EXPECT_TRUE(HeadersFitRuleset(replay, BRAWL));
  // Project+ on frames 10-19 of a vanilla Brawl game, Brawl's again by the end.
  for (int f = 10; f < 20; ++f)
    replay.frames[static_cast<size_t>(f)].header = ReplayHeader{2, PPLUS, 0, 0, 1234};
  EXPECT_FALSE(HeadersFitRuleset(replay, BRAWL));
  EXPECT_FALSE(HeadersFitRuleset(replay, PPLUS));
  // Only the boundary's.
  ReplayArchive boundary_only;
  boundary_only.first_frame = 100;
  boundary_only.boundary.header = ReplayHeader{1, PPLUS, 0, 0, 0};
  EXPECT_FALSE(HeadersFitRuleset(boundary_only, BRAWL));
  EXPECT_TRUE(HeadersFitRuleset(boundary_only, PPLUS));
  // A game with no ruleset takes no ruleset's header.
  EXPECT_FALSE(HeadersFitRuleset(boundary_only, 0));
}

TEST(OrcaKeyframe, RejectsLegacyStatesTruncationAndTrailingData)
{
  const auto blob = PackKeyframe(2, Replay(2));
  auto raw = Raw(blob);
  int frame = 77;
  ReplayArchive decoded;
  decoded.origin_hash = 99;
  for (size_t n = 0; n < raw.size(); ++n)
  {
    const std::vector<u8> truncated(raw.begin(), raw.begin() + n);
    EXPECT_FALSE(UnpackKeyframe(Compressed(truncated), &frame, &decoded));
  }
  EXPECT_EQ(frame, 77);
  EXPECT_EQ(decoded.origin_hash, 99);
  std::memcpy(raw.data(), "OKF2", 4);
  EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
  raw = Raw(blob);
  raw.push_back(0);
  EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
}

TEST(OrcaKeyframe, BoundsCountsSeatsAndPayloadLengthsBeforeAllocation)
{
  const auto blob = PackKeyframe(1, Replay(1));
  const auto original = Raw(blob);
  int frame;
  ReplayArchive decoded;
  // Archive count, first frame, port count, first seat, name length, controls length, second
  // seat.
  for (const auto [offset, value] : std::vector<std::pair<size_t, u32>>{{4, 0xffffffff},
                                                                        {8, 2},
                                                                        {60, 0xffffffff},
                                                                        {64, 4},
                                                                        {68, 129},
                                                                        {78, 65},
                                                                        {99, 0}})
  {
    auto raw = original;
    U32(raw, offset, value);
    EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded)) << offset;
  }
}

TEST(OrcaKeyframe, BoundsDecompressionAndTypedHeaderValues)
{
  int frame;
  ReplayArchive decoded;
  std::vector<u8> oversized((64 << 20) + 1, 0);
  EXPECT_FALSE(UnpackKeyframe(Compressed(oversized), &frame, &decoded));
  auto replay = Replay(0);
  replay.boundary.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>();
  const auto original = Raw(PackKeyframe(0, replay));
  // Header presence, mode, ruleset, coin, flags, clear-ready presence, and the menu exit.
  for (const auto [offset, value] : std::vector<std::pair<size_t, u32>>{
           {64, 2}, {68, 3}, {72, 3}, {76, 2}, {80, 4}, {88, 2}, {92, 24}, {92, 26}, {92, 255}})
  {
    auto raw = original;
    U32(raw, offset, value);
    EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded)) << offset;
  }
}

TEST(OrcaKeyframe, ReplayRecordingRetainsWinningInputsAndEventsAcrossRollback)
{
  ReplayArchive archive;
  archive.first_frame = 0;
  ReplayRecordingScope recording(&archive);
  auto* frame = ReplayRecordingScope::Frame(0);
  frame->header = ReplayHeader{1, 1, 0, 0, 7};
  frame->clear_ready = true;
  Pads predicted{};
  ReplayRecordingScope::RecordPads(0, predicted);
  Pads corrected{};
  corrected[2][3] = 99;
  ReplayRecordingScope::RecordPads(0, corrected);
  ASSERT_EQ(archive.frames.size(), 1u);
  EXPECT_EQ(archive.frames[0].pads, corrected);
  EXPECT_TRUE(archive.frames[0].clear_ready);
  EXPECT_EQ(archive.frames[0].header->room, 7u);
  EXPECT_EQ(ReplayRecordingScope::Frame(MAX_REPLAY_FRAMES + 1), nullptr);
}

TEST(OrcaKeyframe, AReplayFromAFreshStartHoldsOnlyItsOwnFrames)
{
  // A host that fresh-started at its origin frame 1480 offers frame 1700: 220 frames travel,
  // and the first one leaves the main menu for Casual.
  auto original = Replay(220, 1480);
  original.frames[0].menu_exit = MENU_EXIT_CASUAL;
  original.frames[17].header = ReplayHeader{1, 1, 0, 2, 9};
  const auto blob = PackKeyframe(1700, original);
  ASSERT_FALSE(blob.empty());
  int frame = -1;
  ReplayArchive decoded;
  ASSERT_TRUE(UnpackKeyframe(blob, &frame, &decoded));
  EXPECT_EQ(frame, 1700);
  EXPECT_EQ(decoded.first_frame, 1480);
  ASSERT_EQ(decoded.frames.size(), 220u);
  EXPECT_EQ(decoded.EndFrame(), 1700);
  EXPECT_EQ(decoded.frames[0].menu_exit, MENU_EXIT_CASUAL);
  EXPECT_EQ(decoded.At(1480)->menu_exit, MENU_EXIT_CASUAL);
  EXPECT_EQ(decoded.At(1497)->header, original.frames[17].header);
  EXPECT_EQ(decoded.At(1479), nullptr);
  EXPECT_EQ(decoded.At(1700), nullptr);
  for (size_t i = 0; i < decoded.frames.size(); ++i)
    EXPECT_EQ(decoded.frames[i].pads, original.frames[i].pads) << i;
  // Each exit a fresh start may take.
  for (const u8 exit : {MENU_EXIT_FRIENDS, MENU_EXIT_CASUAL, MENU_EXIT_RANKED})
  {
    auto replay = Replay(3, 200);
    replay.frames[0].menu_exit = exit;
    ASSERT_TRUE(UnpackKeyframe(PackKeyframe(203, replay), &frame, &decoded)) << int{exit};
    EXPECT_EQ(decoded.frames[0].menu_exit, exit);
  }
  // A keyframe at the origin itself: the boundary is the first frame, so it may carry the exit.
  auto at_origin = Replay(0, 230);
  at_origin.boundary.menu_exit = MENU_EXIT_RANKED;
  ASSERT_TRUE(UnpackKeyframe(PackKeyframe(230, at_origin), &frame, &decoded));
  EXPECT_EQ(frame, 230);
  EXPECT_TRUE(decoded.frames.empty());
  EXPECT_EQ(decoded.boundary.menu_exit, MENU_EXIT_RANKED);
}

TEST(OrcaKeyframe, AHistorysSeedTravelsOnItsFirstFrameOnly)
{
  // A fresh start's first frame: the exit and the seed the host drew for this history.
  auto original = Replay(40, 230);
  original.frames[0].menu_exit = MENU_EXIT_RANKED;
  original.frames[0].seed = 0x9E3779B9;
  int frame = -1;
  ReplayArchive decoded;
  ASSERT_TRUE(UnpackKeyframe(PackKeyframe(270, original), &frame, &decoded));
  EXPECT_EQ(decoded.frames[0].seed, 0x9E3779B9u);
  EXPECT_EQ(decoded.frames[0].menu_exit, MENU_EXIT_RANKED);
  for (size_t i = 1; i < decoded.frames.size(); ++i)
    EXPECT_EQ(decoded.frames[i].seed, 0u) << i;
  EXPECT_EQ(decoded.boundary.seed, 0u);
  // A boot's own origin: a seed with no exit.
  auto boot = Replay(5, 230);
  boot.frames[0].seed = 1;
  ASSERT_TRUE(UnpackKeyframe(PackKeyframe(235, boot), &frame, &decoded));
  EXPECT_EQ(decoded.frames[0].seed, 1u);
  EXPECT_EQ(decoded.frames[0].menu_exit, 0);
  // A keyframe at the origin itself: the boundary is the first frame, so it may carry the seed.
  auto at_origin = Replay(0, 230);
  at_origin.boundary.seed = 77;
  ASSERT_TRUE(UnpackKeyframe(PackKeyframe(230, at_origin), &frame, &decoded));
  EXPECT_EQ(decoded.boundary.seed, 77u);
  // Anywhere else, never: a later frame's, or a later boundary's.
  auto late = Replay(10, 100);
  late.frames[3].seed = 5;
  EXPECT_TRUE(PackKeyframe(110, late).empty());
  auto boundary = Replay(10, 100);
  boundary.boundary.seed = 5;
  EXPECT_TRUE(PackKeyframe(110, boundary).empty());
}

TEST(OrcaKeyframe, PackingRefusesAReplayThatDoesntFitItsOrigin)
{
  // No origin yet, a frame before the origin, too few frames held.
  EXPECT_TRUE(PackKeyframe(10, Replay(10, -1)).empty());
  EXPECT_TRUE(PackKeyframe(99, Replay(10, 100)).empty());
  EXPECT_TRUE(PackKeyframe(111, Replay(10, 100)).empty());
  EXPECT_FALSE(PackKeyframe(110, Replay(10, 100)).empty());
  // The count limit is the frame's, not the number of frames held.
  EXPECT_FALSE(PackKeyframe(static_cast<int>(MAX_REPLAY_FRAMES),
                            Replay(10, static_cast<int>(MAX_REPLAY_FRAMES) - 10))
                   .empty());
  EXPECT_TRUE(PackKeyframe(static_cast<int>(MAX_REPLAY_FRAMES) + 1,
                           Replay(10, static_cast<int>(MAX_REPLAY_FRAMES) - 9))
                  .empty());
  // A menu exit anywhere but the first frame, or one that isn't an exit a fresh start takes.
  auto late = Replay(10, 100);
  late.frames[1].menu_exit = MENU_EXIT_FRIENDS;
  EXPECT_TRUE(PackKeyframe(110, late).empty());
  auto boundary = Replay(10, 100);
  boundary.boundary.menu_exit = MENU_EXIT_CASUAL;
  EXPECT_TRUE(PackKeyframe(110, boundary).empty());
  for (const u8 exit : {u8{1}, u8{24}, u8{26}, u8{29}, u8{32}, u8{255}})
  {
    auto bad = Replay(10, 100);
    bad.frames[0].menu_exit = exit;
    EXPECT_TRUE(PackKeyframe(110, bad).empty()) << int{exit};
  }
}

TEST(OrcaKeyframe, UnpackingRefusesAMenuExitOffTheFirstFrame)
{
  // Two frames whose records are the same size, so the second's exit is at a known offset.
  auto replay = Replay(2, 50);
  replay.boundary.header.reset();
  replay.boundary.clear_ready = false;
  const auto original = Raw(PackKeyframe(52, replay));
  const size_t record = (original.size() - 28) / 3;
  ASSERT_EQ(28 + 3 * record, original.size());
  int frame = -1;
  ReplayArchive decoded;
  ASSERT_TRUE(UnpackKeyframe(Compressed(original), &frame, &decoded));
  // Each record ends with the menu exit, then the seed.
  // The first frame's exit is fine; the second's, or the boundary's, is refused.
  auto raw = original;
  U32(raw, 28 + record - 8, MENU_EXIT_CASUAL);
  EXPECT_TRUE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
  EXPECT_EQ(decoded.frames[0].menu_exit, MENU_EXIT_CASUAL);
  for (const size_t at : {28 + 2 * record - 8, 28 + 3 * record - 8})
  {
    raw = original;
    U32(raw, at, MENU_EXIT_FRIENDS);
    EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded)) << at;
  }
  // The same for the seed: any value on the first frame, none anywhere else.
  raw = original;
  U32(raw, 28 + record - 4, 0xFFFFFFFF);
  EXPECT_TRUE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
  EXPECT_EQ(decoded.frames[0].seed, 0xFFFFFFFFu);
  for (const size_t at : {28 + 2 * record - 4, 28 + 3 * record - 4})
  {
    raw = original;
    U32(raw, at, 1);
    EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded)) << at;
  }
  // A first frame past the archive's frame.
  raw = original;
  U32(raw, 8, 53);
  EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
}

TEST(OrcaKeyframe, RecordingStartsAtTheOrigin)
{
  ReplayArchive archive;
  ReplayRecordingScope recording(&archive);
  // No origin yet: nothing is recorded.
  EXPECT_EQ(ReplayRecordingScope::Frame(0), nullptr);
  ReplayRecordingScope::RecordPads(5, Pads{});
  EXPECT_TRUE(archive.frames.empty());
  archive.first_frame = 1480;
  EXPECT_EQ(ReplayRecordingScope::Frame(1479), nullptr);
  Pads pads{};
  pads[0][1] = 0x10;
  ReplayRecordingScope::RecordPads(1482, pads);
  ASSERT_EQ(archive.frames.size(), 3u);
  EXPECT_EQ(archive.frames[2].pads, pads);
  EXPECT_EQ(archive.At(1482), &archive.frames[2]);
  EXPECT_EQ(archive.EndFrame(), 1483);
  EXPECT_EQ(ReplayRecordingScope::Frame(1480), &archive.frames[0]);
  EXPECT_EQ(ReplayRecordingScope::Frame(static_cast<int>(MAX_REPLAY_FRAMES) + 1), nullptr);
}

TEST(OrcaKeyframe, ReplacingTheNandWritesOnlyWhatDiffers)
{
  const std::string root = File::CreateTempDir() + "/nand";
  const auto write = [&](const std::string& path, const std::string& text) {
    ASSERT_TRUE(File::CreateFullPath(root + "/" + path));
    ASSERT_TRUE(File::WriteStringToFile(root + "/" + path, text));
  };
  write("tmp/boot.bin", std::string(4096, 'b'));
  write("title/save.dat", "old save");
  write("title/extra.dat", "gone after");
  write("stray/inner/file", "gone after");
  write("kind", "a file where a folder belongs");
  // The unchanged file keeps its old time, so a rewrite would show.
  const auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
  std::filesystem::last_write_time(std::filesystem::path(root + "/tmp/boot.bin"), old_time);
  std::vector<NandEntry> entries;
  const auto dir = [&](const std::string& path) {
    NandEntry e;
    e.path = path;
    e.directory = true;
    entries.push_back(e);
  };
  const auto file = [&](const std::string& path, const std::string& text) {
    NandEntry e;
    e.path = path;
    e.data.assign(text.begin(), text.end());
    entries.push_back(e);
  };
  dir("kind");
  file("kind/inside", "now a folder");
  dir("title");
  file("title/new.dat", "");
  file("title/save.dat", "new save!");
  dir("tmp");
  file("tmp/boot.bin", std::string(4096, 'b'));
  ASSERT_TRUE(ReplaceNandTree(root, entries));
  std::vector<NandEntry> read;
  ASSERT_TRUE(ReadNandTree(root, &read));
  ASSERT_EQ(read.size(), entries.size());
  for (size_t i = 0; i < read.size(); ++i)
  {
    EXPECT_EQ(read[i].path, entries[i].path);
    EXPECT_EQ(read[i].directory, entries[i].directory) << read[i].path;
    EXPECT_EQ(read[i].data, entries[i].data) << read[i].path;
  }
  EXPECT_TRUE(std::filesystem::last_write_time(std::filesystem::path(root + "/tmp/boot.bin")) ==
              old_time);
  // An unsafe path changes nothing.
  NandEntry escape;
  escape.path = "../outside";
  EXPECT_FALSE(ReplaceNandTree(root, {escape}));
  ASSERT_TRUE(ReadNandTree(root, &read));
  EXPECT_EQ(read.size(), entries.size());
  File::DeleteDirRecursively(root);
}

TEST(OrcaKeyframe, BoundsDecodedMetadataWhenCompressedDataIsSmall)
{
  ReplayArchive replay;
  replay.boundary.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "a", false, {}, {}}, {1, "b", false, {}, {}},
                                        {2, "c", false, {}, {}}, {3, "d", false, {}, {}}});
  replay.first_frame = 0;
  const auto single = Raw(PackKeyframe(0, replay));
  constexpr u32 count = 80000;
  constexpr size_t preamble = 28;
  const size_t record_size = single.size() - preamble;
  std::vector<u8> raw(preamble + (count + 1) * record_size);
  std::copy_n(single.begin(), preamble, raw.begin());
  U32(raw, 4, count);
  for (u32 i = 0; i <= count; ++i)
  {
    std::copy(single.begin() + preamble, single.end(), raw.begin() + preamble + i * record_size);
    raw[preamble + i * record_size + 44] = static_cast<u8>('a' + i % 26);
  }
  const auto blob = Compressed(raw);
  EXPECT_LT(blob.size(), 1u << 20);
  int frame = -1;
  ReplayArchive decoded;
  EXPECT_FALSE(UnpackKeyframe(blob, &frame, &decoded));
  EXPECT_EQ(frame, -1);
}

TEST(OrcaKeyframe, RejectsLegacyAndRedirectingDownloadIds)
{
  EXPECT_TRUE(ValidReplayId("replay-0-01234567"));
  EXPECT_TRUE(ValidReplayId("replay-216000-abcdef01"));
  for (const std::string id : {"kf-1-01234567", "../thumb?u=host", "replay-01-01234567",
                               "replay-216001-01234567", "replay-1-0123456g",
                               "replay-1-01234567?u=host", "replay-1-01234567/../x"})
    EXPECT_FALSE(ValidReplayId(id)) << id;
}

TEST(OrcaKeyframe, EncryptionRoundTripsAndRefusesTampering)
{
  const std::vector<u8> packed = PackKeyframe(77, Replay(77));
  std::vector<u8> sealed = packed;
  std::string key;
  ASSERT_TRUE(EncryptKeyframe(77, &sealed, &key));
  EXPECT_EQ(key.size(), 64u);
  EXPECT_EQ(sealed.size(), packed.size() + 28);
  std::string other_key;
  std::vector<u8> twice = packed;
  ASSERT_TRUE(EncryptKeyframe(77, &twice, &other_key));
  EXPECT_NE(key, other_key);
  EXPECT_NE(twice, sealed);
  std::vector<u8> opened = sealed;
  ASSERT_TRUE(DecryptKeyframe(77, key, &opened));
  EXPECT_EQ(opened, packed);
  opened = sealed;
  EXPECT_FALSE(DecryptKeyframe(77, other_key, &opened));
  opened = sealed;
  EXPECT_FALSE(DecryptKeyframe(78, key, &opened));
  opened = sealed;
  opened[opened.size() / 2] ^= 1;
  EXPECT_FALSE(DecryptKeyframe(77, key, &opened));
  opened = sealed;
  EXPECT_FALSE(DecryptKeyframe(77, key.substr(0, 63) + "g", &opened));
}

namespace
{
// A sealed replay of `frames` frames as the host offers it: packed, encrypted and named.
KeyframeInfo Sealed(int frames, std::vector<u8>* blob_out = nullptr)
{
  std::vector<u8> blob = PackKeyframe(frames, Replay(static_cast<size_t>(frames)));
  KeyframeInfo info;
  EXPECT_TRUE(EncryptKeyframe(frames, &blob, &info.key));
  info.frame = frames;
  info.size = blob.size();
  info.hash = KeyframeHash(blob);
  info.id = "replay-" + std::to_string(frames) + "-" + info.hash.substr(0, 8);
  info.names = {{0, 12, "cy", {}}, {1, 140, "di", {1, 2, 3}}};
  info.names_version = 2;
  if (blob_out)
    *blob_out = blob;
  return info;
}

// The offer as the joiner's room reads it back off the wire.
std::optional<KeyframeInfo> OverTheWire(const picojson::object& offer)
{
  const std::string text = picojson::value(offer).serialize();
  picojson::value parsed;
  EXPECT_TRUE(picojson::parse(parsed, text).empty());
  return KeyframeOfferFromJson(parsed.get<picojson::object>());
}
}  // namespace

// Base64 as the offer carries it: standard alphabet, padded, and only the canonical text of the
// stated size decodes.
TEST(OrcaKeyframe, InlineBase64IsStrict)
{
  const std::string foobar = "foobar";
  for (size_t n = 1; n <= foobar.size(); ++n)
  {
    const std::vector<u8> bytes(foobar.begin(), foobar.begin() + n);
    const std::string text = EncodeInlineKeyframe(bytes);
    static const char* const want[] = {"Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    EXPECT_EQ(text, want[n - 1]);
    const auto decoded = DecodeInlineKeyframe(text, n);
    ASSERT_TRUE(decoded) << text;
    EXPECT_EQ(*decoded, bytes);
    // The wrong size, even by one, decodes nothing.
    EXPECT_FALSE(DecodeInlineKeyframe(text, n + 1));
    if (n > 1)
      EXPECT_FALSE(DecodeInlineKeyframe(text, n - 1));
  }
  std::mt19937 rng(7);
  for (const size_t n :
       {size_t{2}, size_t{3}, size_t{1000}, INLINE_KEYFRAME_MAX - 1, INLINE_KEYFRAME_MAX})
  {
    std::vector<u8> bytes(n);
    for (u8& b : bytes)
      b = static_cast<u8>(rng());
    const std::string text = EncodeInlineKeyframe(bytes);
    EXPECT_EQ(text.size(), (n + 2) / 3 * 4);
    EXPECT_LE(text.size(), INLINE_KEYFRAME_MAX_TEXT);
    const auto decoded = DecodeInlineKeyframe(text, n);
    ASSERT_TRUE(decoded) << n;
    EXPECT_EQ(*decoded, bytes) << n;
  }
  // Not base64, padding inside or misplaced, padding bits set, URL-safe letters, whitespace.
  for (const char* bad :
       {"Zm9v!g==", "Zg=v", "Z===", "Zh==", "Zm9=", "Zm-v", "Zm_v", "Zm9 ", "Zm9v\n"})
  {
    EXPECT_FALSE(DecodeInlineKeyframe(bad, 3)) << bad;
    EXPECT_FALSE(DecodeInlineKeyframe(bad, 1)) << bad;
  }
  EXPECT_FALSE(DecodeInlineKeyframe("", 0));
}

// The host sends a replay of at most INLINE_KEYFRAME_MAX bytes in the offer itself, and a larger
// one through the store; the offer with it fits the rooms Worker's 64 KB Orca message cap.
TEST(OrcaKeyframe, ASmallReplayTravelsInTheOffer)
{
  EXPECT_FALSE(InlineKeyframe(0));
  EXPECT_TRUE(InlineKeyframe(1));
  EXPECT_TRUE(InlineKeyframe(INLINE_KEYFRAME_MAX));
  EXPECT_FALSE(InlineKeyframe(INLINE_KEYFRAME_MAX + 1));
  EXPECT_FALSE(InlineKeyframe(u64{128} << 20));

  // A queue match's replay: a fresh start's ~140 frames are a few KB sealed.
  std::vector<u8> blob;
  KeyframeInfo info = Sealed(140, &blob);
  EXPECT_LT(blob.size(), INLINE_KEYFRAME_MAX / 2) << blob.size();
  info.inline_blob = blob;
  std::optional<KeyframeInfo> got = OverTheWire(KeyframeOfferToJson(info));
  ASSERT_TRUE(got);
  EXPECT_EQ(got->frame, info.frame);
  EXPECT_EQ(got->id, info.id);
  EXPECT_EQ(got->size, info.size);
  EXPECT_EQ(got->hash, info.hash);
  EXPECT_EQ(got->key, info.key);
  EXPECT_EQ(got->names, info.names);
  EXPECT_EQ(got->names_version, info.names_version);
  EXPECT_EQ(got->inline_blob, blob);
  int frame = -1;
  ReplayArchive replay;
  std::string error;
  ASSERT_TRUE(OpenKeyframe(*got, got->inline_blob, &frame, &replay, &error)) << error;
  EXPECT_EQ(frame, 140);
  EXPECT_EQ(replay.frames.size(), 140u);

  // Without `b` the offer is the store's, as before.
  info.inline_blob.clear();
  got = OverTheWire(KeyframeOfferToJson(info));
  ASSERT_TRUE(got);
  EXPECT_TRUE(got->inline_blob.empty());
  EXPECT_EQ(picojson::value(KeyframeOfferToJson(info)).serialize().find("\"b\""),
            std::string::npos);

  // The largest inline offer, with every port named, stays well under 64 KB.
  KeyframeInfo big = info;
  big.size = INLINE_KEYFRAME_MAX;
  big.inline_blob.assign(INLINE_KEYFRAME_MAX, 0xAB);
  for (int seat = 0; seat < 4; ++seat)
    for (int k = 0; k < 4; ++k)
      big.names.push_back({seat, k, std::string(64, 'n'), std::vector<u8>(MAX_CONTROLS, 1),
                           std::vector<u8>(MAX_QUEUE, 2)});
  picojson::object wire;
  wire["t"] = picojson::value("msg");
  wire["to"] = picojson::value(std::string(64, 'p'));
  wire["d"] = picojson::value(KeyframeOfferToJson(big));
  EXPECT_LT(picojson::value(wire).serialize().size(), 56u * 1024);
}

// The joiner refuses an inline replay before decoding it when it is longer than the cap allows or
// than the size the offer states, and refuses the whole offer then (so it never joins on it); a
// tampered one fails the same checks as a damaged download.
TEST(OrcaKeyframe, AnInlineReplayIsBoundedAndChecked)
{
  std::vector<u8> blob;
  KeyframeInfo info = Sealed(100, &blob);
  info.inline_blob = blob;
  const picojson::object good = KeyframeOfferToJson(info);
  ASSERT_TRUE(OverTheWire(good));

  // Longer than any inline replay may be, whatever `n` says: refused unread.
  picojson::object offer = good;
  offer["b"] = picojson::value(std::string(INLINE_KEYFRAME_MAX_TEXT + 4, 'A'));
  EXPECT_FALSE(OverTheWire(offer));
  offer["n"] = picojson::value(static_cast<double>(INLINE_KEYFRAME_MAX + 3));
  EXPECT_FALSE(OverTheWire(offer));
  // `n` over the cap with `b`: refused, even with text of the right length for it.
  offer = good;
  offer["n"] = picojson::value(static_cast<double>(INLINE_KEYFRAME_MAX + 1));
  offer["b"] = picojson::value(std::string((INLINE_KEYFRAME_MAX + 1 + 2) / 3 * 4, 'A'));
  EXPECT_FALSE(OverTheWire(offer));
  // Text that isn't the stated size, isn't base64, or isn't a string.
  offer = good;
  offer["b"] = picojson::value(EncodeInlineKeyframe(std::vector<u8>(blob.size() + 3, 1)));
  EXPECT_FALSE(OverTheWire(offer));
  offer["b"] = picojson::value(std::string((blob.size() + 2) / 3 * 4, '*'));
  EXPECT_FALSE(OverTheWire(offer));
  offer["b"] = picojson::value(1.0);
  EXPECT_FALSE(OverTheWire(offer));

  // One flipped bit: the offer reads (it is well formed), and the joiner's checks refuse it as a
  // damaged download.
  std::vector<u8> tampered = blob;
  tampered[tampered.size() / 2] ^= 1;
  offer = good;
  offer["b"] = picojson::value(EncodeInlineKeyframe(tampered));
  std::optional<KeyframeInfo> got = OverTheWire(offer);
  ASSERT_TRUE(got);
  int frame = -1;
  ReplayArchive replay;
  std::string error;
  EXPECT_FALSE(OpenKeyframe(*got, got->inline_blob, &frame, &replay, &error));
  EXPECT_EQ(error, "the keyframe arrived damaged");
  // The right bytes under another key or frame: refused too.
  got = OverTheWire(good);
  ASSERT_TRUE(got);
  got->key[0] = got->key[0] == 'a' ? 'b' : 'a';
  EXPECT_FALSE(OpenKeyframe(*got, got->inline_blob, &frame, &replay, &error));
  got = OverTheWire(good);
  ASSERT_TRUE(got);
  EXPECT_FALSE(
      OpenKeyframe(*got, std::vector<u8>(blob.begin(), blob.end() - 1), &frame, &replay, &error));
  EXPECT_TRUE(OpenKeyframe(*got, got->inline_blob, &frame, &replay, &error)) << error;
}

// The HTTP store against Tools/orca/fake_keyframes.py (set ORCA_TEST_KEYFRAME_SERVER to its URL):
// put, get with progress, delete, retries past 503s. A stale ticket is replaced once, a second
// refusal is final (LastRefusal names it), and a repeated PUT of the same id counts as done.
TEST(OrcaKeyframe, HttpStoreAgainstAFakeServer)
{
  const char* server = std::getenv("ORCA_TEST_KEYFRAME_SERVER");
  if (!server || !*server)
    GTEST_SKIP() << "start Tools/orca/fake_keyframes.py and set ORCA_TEST_KEYFRAME_SERVER";
  int minted = 0;
  const auto store_for = [&](const std::string& room, const std::string& cached,
                             const std::string& fresh_ticket) {
    return MakeHttpKeyframeStore(
        [=, &minted](bool fresh, std::string* t, std::string* url, std::string*) {
          // Like YouGameRoom::FreshTicket: a minted ticket is the cached one from then on.
          minted += fresh;
          *t = minted ? fresh_ticket : cached;
          *url = std::string(server) + "/api/orca/keyframes/" + room;
          return true;
        });
  };
  for (const auto& [room, size] : std::vector<std::pair<std::string, size_t>>{
           {"room1", 300000}, {"room1", 3 << 20}, {"flaky", 200000}})
  {
    SCOPED_TRACE(room + " " + std::to_string(size));
    // The cached ticket went stale: one refusal, then a new one.
    minted = 0;
    auto store = store_for(room, "stale", "good");
    std::vector<u8> blob(size);
    std::mt19937 rng(static_cast<u32>(size));
    for (u8& b : blob)
      b = static_cast<u8>(rng());
    const std::string hash = KeyframeHash(blob);
    KeyframeInfo info{1200, "replay-1200-" + hash.substr(0, 8), blob.size(), hash, ""};
    std::string error;
    ASSERT_TRUE(store->Put(info, blob, &error)) << error;
    EXPECT_EQ(minted, 1);
    EXPECT_EQ(LastRefusal(), "") << "503s and a stale ticket aren't refusals";
    auto good = store_for(room, "good", "good");
    ASSERT_TRUE(good->Put(info, blob, &error)) << "409 exists is done: " << error;
    int calls = 0;
    u64 last = 0;
    const auto got = good->Get(
        info,
        [&](u64 done, u64 total) {
          ++calls;
          EXPECT_EQ(total, info.size);
          EXPECT_GE(done, last);
          last = done;
          return true;
        },
        &error);
    ASSERT_TRUE(got) << error;
    EXPECT_EQ(*got, blob);
    EXPECT_GT(calls, 0);
    EXPECT_EQ(last, info.size);
    good->Delete(info.id);
    good.reset();  // waits for the delete
    auto again = store_for(room, "good", "good");
    EXPECT_FALSE(again->Get(info, [](u64, u64) { return true; }, &error));
    EXPECT_NE(error.find("404"), std::string::npos) << error;
    EXPECT_NE(error.find("gone"), std::string::npos) << "the store's sentence: " << error;
    EXPECT_EQ(LastRefusal(), "gone");
  }
  // A GET with a stale ticket gets a new one too, and the refusal's body never counts as progress.
  {
    minted = 0;
    auto putter = store_for("room2", "good", "good");
    const std::vector<u8> blob(50000, 3);
    const std::string hash = KeyframeHash(blob);
    const KeyframeInfo info{7, "replay-7-" + hash.substr(0, 8), blob.size(), hash, ""};
    std::string error;
    ASSERT_TRUE(putter->Put(info, blob, &error)) << error;
    minted = 0;
    auto getter = store_for("room2", "stale", "good");
    u64 most = 0;
    const auto got =
        getter->Get(info, [&](u64 done, u64) { most = std::max(most, done); return true; }, &error);
    ASSERT_TRUE(got) << error;
    EXPECT_EQ(*got, blob);
    EXPECT_EQ(minted, 1);
    EXPECT_EQ(most, blob.size());
    getter->Delete(info.id);
  }
  // Cancelled (emulation stopping): no more tries.
  {
    auto store = store_for("flaky", "good", "good");
    store->Cancel();
    const std::vector<u8> blob(1000, 2);
    const std::string hash = KeyframeHash(blob);
    std::string error;
    EXPECT_FALSE(store->Put({3, "replay-3-" + hash.substr(0, 8), blob.size(), hash, ""}, blob, &error));
    EXPECT_EQ(error, "cancelled");
    EXPECT_EQ(LastRefusal(), "");
  }
  // A ticket minted while signed out: one new ticket (the player may have signed in since), then
  // the refusal is final.
  {
    const std::vector<u8> blob(2000, 4);
    const std::string hash = KeyframeHash(blob);
    const KeyframeInfo info{9, "replay-9-" + hash.substr(0, 8), blob.size(), hash, ""};
    std::string error;
    minted = 0;
    ASSERT_TRUE(store_for("room3", "guest", "good")->Put(info, blob, &error)) << error;
    EXPECT_EQ(minted, 1);
    minted = 0;
    EXPECT_FALSE(store_for("room4", "guest", "guest")->Put(info, blob, &error));
    EXPECT_EQ(minted, 1);
    EXPECT_NE(error.find("403"), std::string::npos) << error;
    EXPECT_EQ(LastRefusal(), "signed_out");
    // A 4xx without YouGame's code (an edge page) fails this try but isn't a refusal.
    EXPECT_FALSE(store_for("room4", "edge", "edge")->Put(info, blob, &error));
    EXPECT_NE(error.find("403"), std::string::npos) << error;
    EXPECT_EQ(LastRefusal(), "");
  }
  // A ticket the store refuses even when new: final.
  auto refused = store_for("room1", "bad", "bad");
  std::string error;
  const std::vector<u8> blob(1000, 1);
  const std::string hash = KeyframeHash(blob);
  EXPECT_FALSE(refused->Put({1, "replay-1-" + hash.substr(0, 8), blob.size(), hash, ""}, blob, &error));
  EXPECT_NE(error.find("401"), std::string::npos) << error;
  EXPECT_EQ(LastRefusal(), "ticket");
}

// ---- Replay streams (Keyframe.h): saved matches and live watching ----

namespace
{
ReplayStreamInfo StreamInfo()
{
  ReplayStreamInfo info;
  info.compat = "orca2:test";
  info.build = "0123456789abcdef";
  info.game_id = "RSBE01";
  info.origin_hash = 0x123456789abcdef0;
  info.first_frame = 500;
  info.segment = 2;
  info.room = "abc123";
  return info;
}

// The whole replay, cut into chunks of `per_chunk` frames, each with a checksum every 60 frames.
std::vector<u8> Stream(const ReplayArchive& replay, size_t per_chunk)
{
  ReplayStreamInfo info = StreamInfo();
  info.first_frame = replay.first_frame;
  std::vector<u8> out = PackReplayStreamHeader(info);
  for (size_t start = 0; start < replay.frames.size(); start += per_chunk)
  {
    ReplayChunk chunk;
    chunk.first = replay.first_frame + static_cast<int>(start);
    const size_t end = std::min(start + per_chunk, replay.frames.size());
    chunk.frames.assign(replay.frames.begin() + start, replay.frames.begin() + end);
    for (int f = chunk.first + 1; f <= replay.first_frame + static_cast<int>(end); ++f)
    {
      if (f % 60 == 0)
        chunk.checksums.emplace_back(f, 0xabc000u + static_cast<u64>(f));
    }
    const auto bytes = PackReplayChunk(chunk, start == 0);
    EXPECT_FALSE(bytes.empty());
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out;
}
}  // namespace

namespace
{
std::vector<u8> StreamHeaderJson(std::string_view json)
{
  auto bytes = PackReplayStreamHeader(StreamInfo());
  bytes.resize(12);
  U32(bytes, 8, static_cast<u32>(json.size()));
  bytes.insert(bytes.end(), json.begin(), json.end());
  return bytes;
}

std::string HeaderWithSegment(std::string_view segment, const std::string& compat = "orca2:test")
{
  auto info = StreamInfo();
  info.compat = compat;
  const auto bytes = PackReplayStreamHeader(info);
  std::string json(bytes.begin() + 12, bytes.end());
  const auto at = json.find("\"segment\":2");
  EXPECT_NE(at, std::string::npos);
  json.replace(at + 10, 1, segment);
  return json;
}

void ExpectHeaderRefused(std::string_view json)
{
  const auto bytes = StreamHeaderJson(json);
  ReplayStreamReader reader("orca2:test");
  std::string error;
  EXPECT_FALSE(reader.Feed(bytes.data(), bytes.size(), &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(reader.Info());
  EXPECT_TRUE(reader.Archive().frames.empty());
  EXPECT_FALSE(reader.Complete());
  const auto good = PackReplayStreamHeader(StreamInfo());
  EXPECT_FALSE(reader.Feed(good.data(), good.size(), &error));
  EXPECT_EQ(error, "the replay stream already failed");
}

void ExpectWatchRefused(
    const std::string& message, const std::string& why = "the replay's header is incomplete")
{
  sf::TcpListener listener;
  ASSERT_EQ(listener.listen(0, sf::IpAddress::LocalHost), sf::Socket::Status::Done);
  listener.setBlocking(false);
  const std::string origin = "127.0.0.1:" + std::to_string(listener.getLocalPort());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::atomic<bool> stop{false};
  std::thread server([&] {
    sf::TcpSocket socket;
    while (!stop && std::chrono::steady_clock::now() < deadline)
    {
      if (listener.accept(socket) == sf::Socket::Status::Done)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    socket.setBlocking(false);
    std::string request;
    while (!stop && std::chrono::steady_clock::now() < deadline &&
           request.find("\r\n\r\n") == std::string::npos)
    {
      std::array<char, 4096> buffer;
      size_t received = 0;
      const auto status = socket.receive(buffer.data(), buffer.size(), received);
      if (status != sf::Socket::Status::Done && status != sf::Socket::Status::NotReady)
        return;
      request.append(buffer.data(), received);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::string field = "Sec-WebSocket-Key: ";
    const auto key_at = request.find(field);
    if (key_at == std::string::npos)
      return;
    const auto start = key_at + field.size();
    const auto end = request.find("\r\n", start);
    const auto digest = Common::SHA1::CalculateDigest(
        request.substr(start, end - start) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const std::string accept = EncodeInlineKeyframe(std::vector<u8>(digest.begin(), digest.end()));
    const auto send = [&](const std::string& bytes) {
      size_t sent = 0;
      while (sent < bytes.size() && !stop && std::chrono::steady_clock::now() < deadline)
      {
        size_t count = 0;
        const auto status = socket.send(bytes.data() + sent, bytes.size() - sent, count);
        sent += count;
        if (status != sf::Socket::Status::Done && status != sf::Socket::Status::Partial &&
            status != sf::Socket::Status::NotReady)
          return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return sent == bytes.size();
    };
    const auto frame = [](const std::string& text) {
      std::string bytes(1, static_cast<char>(0x81));
      if (text.size() < 126)
        bytes += static_cast<char>(text.size());
      else if (text.size() <= 0xffff)
      {
        bytes += static_cast<char>(126);
        bytes += static_cast<char>(text.size() >> 8);
        bytes += static_cast<char>(text.size() & 0xff);
      }
      else
      {
        bytes += static_cast<char>(127);
        for (int shift = 56; shift >= 0; shift -= 8)
          bytes += static_cast<char>((static_cast<u64>(text.size()) >> shift) & 0xff);
      }
      return bytes + text;
    };
    if (!send("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
              "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n") ||
        !send(frame(message)) || !send(frame(R"({"t":"end"})")))
      return;
    while (!stop && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  const Common::ScopeGuard join([&] {
    stop = true;
    server.join();
  });
  const auto set = [](const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    if (value.empty())
      unsetenv(key);
    else
      setenv(key, value.c_str(), 1);
#endif
  };
  const auto old_source = Orca::GetEnv("ORCA_REPLAY_SOURCE");
  const auto old_origin = Orca::GetEnv("ORCA_REPLAY_ROOMS_ORIGIN");
  const Common::ScopeGuard restore([&] {
    set("ORCA_REPLAY_SOURCE", old_source);
    set("ORCA_REPLAY_ROOMS_ORIGIN", old_origin);
  });
  set("ORCA_REPLAY_SOURCE", "ws://" + origin + "/watch/test");
  set("ORCA_REPLAY_ROOMS_ORIGIN", "http://" + origin);
  auto player = Rollback::ReplayStream::Player::FromEnv();
  ASSERT_NE(player, nullptr);
  std::string stopped;
  Rollback::ReplayStream::PlayerHost host;
  host.stop = [&](const std::string& why) { stopped = why; };
  host.stopping = [&] { return std::chrono::steady_clock::now() >= deadline; };
  int running = -1;
  EXPECT_FALSE(player->Step(Core::System::GetInstance(), host, &running, 0, 0, {}));
  EXPECT_TRUE(player->Over());
  EXPECT_EQ(stopped, "The replay couldn't be played: " + why);
  EXPECT_EQ(running, -1);
}

std::string WatchWithSegment(std::string_view segment)
{
  return R"({"t":"rs","b":")" +
         EncodeInlineKeyframe(
             StreamHeaderJson(HeaderWithSegment(segment, Orca::Online::CompatibilityKey()))) +
         R"(","extra":)" + std::string(segment) + "}";
}
}  // namespace

TEST(OrcaSafeJson, PreservesFiniteControllerNumbers)
{
  const std::string json = "[0.00003051850947599719,1e-9,1e999," + std::string(400, '9') + "]";
  picojson::value value;
  ASSERT_TRUE(Orca::Json::ParseJson(json, &value, Orca::Json::NumberPolicy::Finite));
  ASSERT_TRUE(value.is<picojson::array>());
  const auto& numbers = value.get<picojson::array>();
  ASSERT_EQ(numbers.size(), 4u);
  ASSERT_TRUE(numbers[0].is<double>());
  ASSERT_TRUE(numbers[1].is<double>());
  EXPECT_DOUBLE_EQ(numbers[0].get<double>(), 0.00003051850947599719);
  EXPECT_DOUBLE_EQ(numbers[1].get<double>(), 1e-9);
  EXPECT_TRUE(numbers[2].is<picojson::null>());
  EXPECT_TRUE(numbers[3].is<picojson::null>());
  ASSERT_TRUE(Orca::Json::ParseJson(json, &value));
  for (const auto& number : value.get<picojson::array>())
    EXPECT_TRUE(number.is<picojson::null>());
}

TEST(OrcaReplayStream, HeaderWithHugeNumberIsRefused)
{
  for (const auto& number : {std::string("1e999"), std::string(400, '9')})
  {
    SCOPED_TRACE(number.size());
    ExpectHeaderRefused(HeaderWithSegment(number));
  }
}

TEST(OrcaReplayStream, HeaderNestedPastLimitIsRefused)
{
  const std::string brackets(16 * 1024, '[');
  ExpectHeaderRefused(brackets);
  ExpectHeaderRefused(HeaderWithSegment(std::string(100, '[') + "0" + std::string(100, ']')));
}

TEST(OrcaReplayStream, WatchMessageWithHugeNumberIsRefused)
{
  for (const auto& number : {std::string("1e999"), std::string(400, '9')})
  {
    SCOPED_TRACE(number.size());
    ExpectWatchRefused(WatchWithSegment(number));
  }
}

TEST(OrcaReplayStream, WatchMessageNestedPastLimitIsRefused)
{
  const std::string brackets(16 * 1024, '[');
  ExpectWatchRefused(
      R"({"t":"rs","b":")" + EncodeInlineKeyframe(StreamHeaderJson(brackets)) +
          R"(","extra":)" + brackets + "0" + std::string(brackets.size(), ']') + "}",
      "the replay's header isn't valid");
  ExpectWatchRefused(WatchWithSegment(std::string(100, '[') + "0" + std::string(100, ']')));
}

TEST(OrcaReplayStream, ValidHeaderParsesByteIdentically)
{
  const std::string json =
      R"({"build":"0123456789abcdef","compat":"orca2:test","first_frame":500,"game":"RSBE01",)"
      R"("origin_hash":"123456789abcdef0","room":"abc123","seat_labels":false,"segment":2,"v":2})";
  ASSERT_EQ(json.size(), 171u);
  std::vector<u8> golden{'Y', 'G', 'R', 'S', 1, 0, 0, 0, 171, 0, 0, 0};
  golden.insert(golden.end(), json.begin(), json.end());
  ReplayStreamReader reader("orca2:test");
  std::string error;
  for (const u8 byte : golden)
    ASSERT_TRUE(reader.Feed(&byte, 1, &error)) << error;
  ASSERT_TRUE(reader.Info());
  EXPECT_EQ(PackReplayStreamHeader(*reader.Info()), golden);
  EXPECT_TRUE(reader.Archive().frames.empty());
}

// Changing this header also requires updating rooms/test/fixtures/yg-dolphin-stream-header.json;
// its "about" field describes the refresh steps.
TEST(OrcaReplayStream, HeaderBytesMatchRoomsGoldenFixture)
{
  auto info = StreamInfo();
  EXPECT_EQ(Common::BytesToHexString(PackReplayStreamHeader(info)),
            "5947525301000000ab0000007b226275696c64223a2230313233343536373839616263646566222c22636f6d70617422"
            "3a226f726361323a74657374222c2266697273745f6672616d65223a3530302c2267616d65223a22525342453031222c"
            "226f726967696e5f68617368223a2231323334353637383961626364656630222c22726f6f6d223a2261626331323322"
            "2c22736561745f6c6162656c73223a66616c73652c227365676d656e74223a322c2276223a327d");
  info.seat_labels = true;
  EXPECT_EQ(Common::BytesToHexString(PackReplayStreamHeader(info)),
            "5947525301000000aa0000007b226275696c64223a2230313233343536373839616263646566222c22636f6d70617422"
            "3a226f726361323a74657374222c2266697273745f6672616d65223a3530302c2267616d65223a22525342453031222c"
            "226f726967696e5f68617368223a2231323334353637383961626364656630222c22726f6f6d223a2261626331323322"
            "2c22736561745f6c6162656c73223a747275652c227365676d656e74223a322c2276223a327d");
  // A change here means updating rooms/test/fixtures/yg-dolphin-stream-header.json streams.chunked
  // too. The XML report carries the sample used in notes/train-0348-replay-fixture.md.
  const auto chunked = Stream(Replay(30, 0), 10);
  EXPECT_EQ(chunked.size(), 1516u);
  EXPECT_EQ(Orca::Mod::Sha256Hex(
                std::string_view(reinterpret_cast<const char*>(chunked.data()), chunked.size())),
            "36d2a4e821fd27739e20fcd4726b42be65d1c3420b2e7a47bc47ea79f2222c7e");
  RecordProperty("chunked_fixture_hex", Common::BytesToHexString(chunked));
}

// A game's saved copy is the written live stream (small chunks, every checksum) read back and packed
// compactly. The room's pieces can split a compressed chunk; joined, they read as the same match.
TEST(OrcaReplayStream, RepackedPiecesKeepTheWholeHistory)
{
  auto original = Replay(8200, 500);
  original.frames[0].seed = 77;
  original.frames[0].menu_exit = MENU_EXIT_RANKED;
  original.frames[42].header = ReplayHeader{1, 2, 0, 3, 55};
  original.frames[99].clear_ready = true;
  auto ports = std::make_shared<std::vector<Orca::Events::PortInfo>>(*original.frames[3600].ports);
  ports->at(0).name = "new name";
  for (size_t i = 3600; i < original.frames.size(); ++i)
    original.frames[i].ports = ports;
  const std::vector<u8> live = Stream(original, 7);
  ReplayStreamReader written;
  std::string error;
  ASSERT_TRUE(written.Feed(live.data(), live.size(), &error)) << error;
  const auto expected = PackReplayStream(*written.Info(), written.Archive(), written.Checksums());
  ASSERT_FALSE(expected.empty());
  // Random test pads barely compress; real play packs ~20x smaller than its live stream.
  ASSERT_LT(expected.size(), live.size());
  for (const size_t max_piece : {size_t{13}, REPLAY_PACKED_PIECE_MAX, REPLAY_PACKED_MAX})
  {
    SCOPED_TRACE(max_piece);
    int frames = 0;
    const auto pieces = Rollback::ReplayStream::RepackedPieces(live, &frames, max_piece);
    ASSERT_FALSE(pieces.empty());
    EXPECT_EQ(frames, 8200);
    std::vector<u8> bytes;
    ReplayStreamReader reader("orca2:test");
    for (size_t i = 0; i < pieces.size(); ++i)
    {
      const auto& piece = pieces[i];
      ASSERT_FALSE(piece.empty());
      const size_t limit = std::min(max_piece, REPLAY_PACKED_PIECE_MAX);
      EXPECT_LE(piece.size(), limit);
      if (i + 1 < pieces.size())
        EXPECT_EQ(piece.size(), limit);
      bytes.insert(bytes.end(), piece.begin(), piece.end());
      ASSERT_TRUE(reader.Feed(piece.data(), piece.size(), &error)) << error;
    }
    EXPECT_EQ(bytes, expected);
    ASSERT_TRUE(reader.Info());
    EXPECT_EQ(reader.Info()->build, StreamInfo().build);
    EXPECT_EQ(reader.Info()->game_id, StreamInfo().game_id);
    EXPECT_EQ(reader.Info()->first_frame, original.first_frame);
    EXPECT_EQ(reader.Info()->room, StreamInfo().room);
    EXPECT_EQ(reader.Archive().origin_hash, original.origin_hash);
    ASSERT_EQ(reader.Archive().frames.size(), original.frames.size());
    EXPECT_EQ(reader.Chunks(), 3u);
    EXPECT_EQ(reader.Checksums(), written.Checksums());
    EXPECT_FALSE(reader.Checksums().empty());
    // As the live stream reads (a stream marks every port remote: a viewer holds none).
    for (size_t i = 0; i < original.frames.size(); ++i)
    {
      const auto& got = reader.Archive().frames[i];
      const auto& want = written.Archive().frames[i];
      EXPECT_EQ(got.pads, original.frames[i].pads);
      EXPECT_EQ(got.pads, want.pads);
      EXPECT_EQ(got.header, want.header);
      EXPECT_EQ(got.clear_ready, want.clear_ready);
      EXPECT_EQ(got.seed, want.seed);
      EXPECT_EQ(got.menu_exit, want.menu_exit);
      ASSERT_TRUE(got.ports);
      EXPECT_EQ(*got.ports, *want.ports);
    }
  }
}

// Past 1 MiB packed, no copy at all (the room would refuse it); a shorter history of the same
// play still gets one.
TEST(OrcaReplayStream, RepackedPiecesRefuseCopiesOverOneMiB)
{
  const auto original = Replay(40000, 500);
  ASSERT_GT(PackReplayStream(StreamInfo(), original, {}).size(), REPLAY_PACKED_MAX);
  int frames = -1;
  bool too_big = false;
  EXPECT_TRUE(Rollback::ReplayStream::RepackedPieces(Stream(original, 600), &frames,
                                                     REPLAY_PACKED_PIECE_MAX, &too_big)
                  .empty());
  EXPECT_EQ(frames, 0);
  EXPECT_TRUE(too_big);
  auto shorter = original;
  shorter.frames.resize(18500);
  const auto pieces = Rollback::ReplayStream::RepackedPieces(Stream(shorter, 600), &frames);
  ASSERT_FALSE(pieces.empty());
  EXPECT_EQ(frames, 18500);
  size_t bytes = 0;
  for (const auto& piece : pieces)
  {
    EXPECT_LE(piece.size(), REPLAY_PACKED_PIECE_MAX);
    bytes += piece.size();
  }
  EXPECT_LE(bytes, REPLAY_PACKED_MAX);
}

// Nothing to send from bytes that aren't a history with frames; a cut tail is left out.
TEST(OrcaReplayStream, RepackedPiecesRefuseBadBytes)
{
  const auto original = Replay(300, 500);
  const std::vector<u8> live = Stream(original, 100);
  int frames = -1;
  EXPECT_TRUE(Rollback::ReplayStream::RepackedPieces({}, &frames).empty());
  EXPECT_TRUE(Rollback::ReplayStream::RepackedPieces(live, &frames, 0).empty());
  EXPECT_TRUE(
      Rollback::ReplayStream::RepackedPieces(PackReplayStreamHeader(StreamInfo()), &frames).empty());
  EXPECT_EQ(frames, 0);
  auto damaged = live;
  damaged[damaged.size() / 2] ^= 0x40;
  bool too_big = true;
  EXPECT_TRUE(
      Rollback::ReplayStream::RepackedPieces(damaged, &frames, REPLAY_PACKED_PIECE_MAX, &too_big)
          .empty());
  EXPECT_FALSE(too_big);
  // Two whole chunks and part of the third: a copy of the two.
  const std::vector<u8> two = Stream(Replay(200, 500), 100);
  auto cut = live;
  cut.resize(two.size() + 10);
  ASSERT_EQ(std::vector<u8>(cut.begin(), cut.begin() + static_cast<std::ptrdiff_t>(two.size())),
            two);
  EXPECT_FALSE(Rollback::ReplayStream::RepackedPieces(cut, &frames).empty());
  EXPECT_EQ(frames, 200);
}

// The emulation thread hands a copy over and goes on: a send that hangs (a slow room) never holds
// up the next Submit, a copy overtaken while waiting is skipped, and the newest one still goes out.
TEST(OrcaReplayStream, PackedSenderNeverBlocksTheCaller)
{
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::pair<int, u64>> sent;
  bool release = false;
  {
    Rollback::ReplayStream::PackedSender sender(
        [&](const std::vector<std::vector<u8>>& pieces, int frames, u64 generation) {
          EXPECT_FALSE(pieces.empty());
          std::unique_lock lock(mutex);
          sent.emplace_back(frames, generation);
          changed.notify_all();
          changed.wait(lock, [&] { return release; });
        });
    sender.Submit(Stream(Replay(120, 500), 30), 1, 1);
    {
      std::unique_lock lock(mutex);
      ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return !sent.empty(); }));
    }
    // The first send hangs; these return at once (a blocked caller would never reach the release).
    auto submits = std::async(std::launch::async, [&] {
      sender.Submit(Stream(Replay(240, 500), 30), 1, 1);
      sender.Submit(Stream(Replay(360, 500), 30), 2, 1);
    });
    ASSERT_EQ(submits.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    changed.notify_all();
  }
  // Destroyed: the copy in hand finished first.
  const std::vector<std::pair<int, u64>> expected{{120, 1}, {360, 2}};
  EXPECT_EQ(sent, expected);
}

// A copy that doesn't pack sends nothing, and the next one still goes.
TEST(OrcaReplayStream, PackedSenderSendsNothingForABadCopy)
{
  std::vector<int> sent;
  {
    Rollback::ReplayStream::PackedSender sender(
        [&](const std::vector<std::vector<u8>>&, int frames, u64) { sent.push_back(frames); });
    sender.Submit({1, 2, 3}, 1, 1);
  }
  EXPECT_TRUE(sent.empty());
  {
    Rollback::ReplayStream::PackedSender sender(
        [&](const std::vector<std::vector<u8>>&, int frames, u64) { sent.push_back(frames); });
    sender.Submit({1, 2, 3}, 1, 1);
    sender.Submit(Stream(Replay(90, 500), 30), 1, 1);
  }
  EXPECT_EQ(sent, std::vector<int>{90});
}

// A copy past 1 MiB marks its history, so the writer stops copying it; bad bytes or another
// history don't.
TEST(OrcaReplayStream, PackedSenderMarksAHistoryTooBig)
{
  std::vector<int> sent;
  Rollback::ReplayStream::PackedSender sender(
      [&](const std::vector<std::vector<u8>>&, int frames, u64) { sent.push_back(frames); });
  EXPECT_FALSE(sender.TooBig(3));
  sender.Submit({1, 2, 3}, 1, 2);
  sender.Submit(Stream(Replay(40000, 500), 600), 1, 3);
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!sender.TooBig(3) && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(sender.TooBig(3));
  EXPECT_FALSE(sender.TooBig(2));
  EXPECT_FALSE(sender.TooBig(4));
  EXPECT_TRUE(sent.empty());
}

// A stream holds every frame's inputs and typed events and the checksums, and reads the same
// whether it arrives whole or a few bytes at a time (a growing file, a network stream).
TEST(OrcaReplayStream, RoundTripsWholeOrInPieces)
{
  auto original = Replay(1000, 500);
  original.frames[0].seed = 77;
  original.frames[0].menu_exit = MENU_EXIT_RANKED;
  original.frames[42].header = ReplayHeader{1, 2, 0, 3, 55};
  original.frames[99].clear_ready = true;
  const std::vector<u8> bytes = Stream(original, 7);
  for (const size_t piece : {bytes.size(), size_t{1}, size_t{13}, size_t{4096}})
  {
    ReplayStreamReader reader;
    std::string error;
    for (size_t at = 0; at < bytes.size(); at += piece)
    {
      const size_t n = std::min(piece, bytes.size() - at);
      ASSERT_TRUE(reader.Feed(bytes.data() + at, n, &error)) << error;
    }
    ASSERT_TRUE(reader.Info());
    EXPECT_EQ(reader.Info()->compat, "orca2:test");
    EXPECT_EQ(reader.Info()->build, "0123456789abcdef");
    EXPECT_EQ(reader.Info()->game_id, "RSBE01");
    EXPECT_EQ(reader.Info()->origin_hash, original.origin_hash);
    EXPECT_EQ(reader.Info()->first_frame, 500);
    EXPECT_EQ(reader.Info()->segment, 2u);
    EXPECT_EQ(reader.Info()->room, "abc123");
    ReplayArchive& decoded = reader.Archive();
    EXPECT_EQ(decoded.first_frame, 500);
    EXPECT_EQ(decoded.origin_hash, original.origin_hash);
    ASSERT_EQ(decoded.frames.size(), original.frames.size());
    for (size_t i = 0; i < decoded.frames.size(); ++i)
    {
      EXPECT_EQ(decoded.frames[i].pads, original.frames[i].pads);
      EXPECT_EQ(decoded.frames[i].header, original.frames[i].header);
      EXPECT_EQ(decoded.frames[i].clear_ready, original.frames[i].clear_ready);
      EXPECT_EQ(decoded.frames[i].seed, original.frames[i].seed);
      EXPECT_EQ(decoded.frames[i].menu_exit, original.frames[i].menu_exit);
      ASSERT_EQ(decoded.frames[i].ports->size(), original.frames[i].ports->size());
      for (size_t p = 0; p < decoded.frames[i].ports->size(); ++p)
      {
        const auto& got = decoded.frames[i].ports->at(p);
        const auto& want = original.frames[i].ports->at(p);
        EXPECT_EQ(got.port, want.port);
        EXPECT_EQ(got.name, want.name);
        EXPECT_EQ(got.controls, want.controls);
        EXPECT_EQ(got.queue, want.queue);
      }
    }
    // Equal ports are shared, across chunks too.
    EXPECT_EQ(decoded.frames[6].ports, decoded.frames[7].ports);
    EXPECT_EQ(reader.Checksums().size(), 1000u / 60u + 1);
    EXPECT_EQ(reader.Checksums().at(540), 0xabc000u + 540);
    EXPECT_EQ(reader.Chunks(), (1000u + 6) / 7);
  }
}

// A live watcher reads a chunk only once all of it has arrived: a partial tail is held back.
TEST(OrcaReplayStream, HoldsBackAPartialChunk)
{
  const auto replay = Replay(20, 0);
  const std::vector<u8> bytes = Stream(replay, 10);
  ReplayStreamReader reader;
  std::string error;
  ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size() - 5, &error)) << error;
  EXPECT_EQ(reader.Archive().frames.size(), 10u);
  ASSERT_TRUE(reader.Feed(bytes.data() + bytes.size() - 5, 5, &error)) << error;
  EXPECT_EQ(reader.Archive().frames.size(), 20u);
}

// A reopened recorder repacks its small live chunks into a backlog with different boundaries.
// Whole overlapping chunks and the overlap in a partly new chunk must agree, without replaying
// or charging metadata for them again; an incomplete old tail is discarded at the reset.
TEST(OrcaReplayStream, ResumesWithARepackedBacklog)
{
  auto original = Replay(1407, 500);
  original.frames[0].seed = 77;
  original.frames[0].menu_exit = MENU_EXIT_RANKED;
  original.frames[42].header = ReplayHeader{1, 2, 0, 3, 55};
  original.frames[99].clear_ready = true;
  auto ports = std::make_shared<std::vector<Orca::Events::PortInfo>>(*original.frames[735].ports);
  ports->at(0).name = "new name";
  for (size_t i = 735; i < original.frames.size(); ++i)
    original.frames[i].ports = ports;
  auto prefix = original;
  prefix.frames.resize(738);
  const auto live = Stream(prefix, 6);
  const auto backlog = Stream(original, 600);
  const auto uninterrupted = Stream(original, 6);
  ReplayStreamReader expected;
  std::string error;
  ASSERT_TRUE(expected.Feed(uninterrupted.data(), uninterrupted.size(), &error)) << error;
  for (const size_t piece : {backlog.size(), size_t{1}, size_t{13}})
  {
    ReplayStreamReader reader;
    ASSERT_TRUE(reader.Feed(live.data(), live.size() - 5, &error)) << error;
    EXPECT_EQ(reader.Archive().frames.size(), 732u);
    reader.Resume();
    // A second reset while the new header is incomplete still resumes this same history.
    ASSERT_TRUE(reader.Feed(backlog.data(), 5, &error)) << error;
    reader.Resume();
    for (size_t at = 0; at < backlog.size(); at += piece)
    {
      ASSERT_TRUE(reader.Feed(backlog.data() + at, std::min(piece, backlog.size() - at), &error))
          << error;
    }
    EXPECT_EQ(reader.Resumes(), 1u);
    EXPECT_EQ(reader.Archive().first_frame, expected.Archive().first_frame);
    EXPECT_EQ(reader.Archive().origin_hash, expected.Archive().origin_hash);
    ASSERT_EQ(reader.Archive().frames.size(), expected.Archive().frames.size());
    for (size_t i = 0; i < original.frames.size(); ++i)
    {
      const auto& got = reader.Archive().frames[i];
      const auto& want = expected.Archive().frames[i];
      EXPECT_EQ(got.pads, want.pads);
      EXPECT_EQ(*got.ports, *want.ports);
      EXPECT_EQ(got.header, want.header);
      EXPECT_EQ(got.clear_ready, want.clear_ready);
      EXPECT_EQ(got.menu_exit, want.menu_exit);
      EXPECT_EQ(got.seed, want.seed);
    }
    EXPECT_EQ(reader.Checksums(), expected.Checksums());
    EXPECT_EQ(reader.Archive().frames[731].ports, reader.Archive().frames[732].ports);
    // Repeating an entirely overlapping segment must leave the archive and checksums intact.
    reader.Resume();
    ASSERT_TRUE(reader.Feed(backlog.data(), backlog.size(), &error)) << error;
    EXPECT_EQ(reader.Resumes(), 2u);
    EXPECT_EQ(reader.Archive().frames.size(), original.frames.size());
    EXPECT_EQ(reader.Checksums(), expected.Checksums());
  }
}

TEST(OrcaReplayStream, RefusesAnotherHeaderOnResume)
{
  const auto replay = Replay(20, 500);
  const auto live = Stream(replay, 6);
  std::vector<ReplayStreamInfo> other(7, StreamInfo());
  other[0].compat += "other";
  other[1].build += "other";
  other[2].game_id = "RSBP01";
  other[3].origin_hash ^= 1;
  ++other[4].first_frame;
  other[5].room += "other";
  other[6].seat_labels = true;
  std::vector<std::vector<u8>> headers;
  for (const auto& info : other)
    headers.push_back(PackReplayStreamHeader(info));
  // A header that isn't valid JSON is another stream too.
  auto broken = PackReplayStreamHeader(StreamInfo());
  broken[broken.size() - 1] = '!';
  headers.push_back(std::move(broken));
  for (const auto& header : headers)
  {
    ReplayStreamReader reader("orca2:test");
    std::string error;
    ASSERT_TRUE(reader.Feed(live.data(), live.size(), &error)) << error;
    reader.Resume();
    EXPECT_FALSE(reader.Feed(header.data(), header.size(), &error));
    EXPECT_EQ(error, "the match's stream moved to another history");
    EXPECT_EQ(reader.Archive().frames.size(), replay.frames.size());
    reader.Resume();
    EXPECT_FALSE(reader.Feed(live.data(), live.size(), &error));
  }
}

TEST(OrcaReplayStream, ResumesAcrossTheRecordersSegmentNumber)
{
  // A recorder that drops back into its match through a keyframe starts its own next segment of
  // the same history: only the header's segment count differs, and the frames carry on.
  const auto replay = Replay(900, 500);
  auto prefix = replay;
  prefix.frames.resize(300);
  const auto live = Stream(prefix, 6);
  auto later = StreamInfo();
  later.segment += 2;
  auto backlog = PackReplayStreamHeader(later);
  const auto chunks = Stream(replay, 600);
  const size_t header_size = PackReplayStreamHeader(StreamInfo()).size();
  backlog.insert(backlog.end(), chunks.begin() + static_cast<std::ptrdiff_t>(header_size),
                 chunks.end());
  ReplayStreamReader reader("orca2:test");
  std::string error;
  ASSERT_TRUE(reader.Feed(live.data(), live.size(), &error)) << error;
  reader.Resume();
  ASSERT_TRUE(reader.Feed(backlog.data(), backlog.size(), &error)) << error;
  EXPECT_EQ(reader.Resumes(), 1u);
  EXPECT_EQ(reader.Archive().frames.size(), replay.frames.size());
}

TEST(OrcaReplayStream, RefusesAChangedOverlappingFrame)
{
  const auto replay = Replay(70, 0);
  const auto live = Stream(replay, 6);
  for (int field = 0; field < 9; ++field)
  {
    SCOPED_TRACE(field);
    auto changed = replay;
    auto& frame = changed.frames[0];
    auto ports = std::make_shared<std::vector<Orca::Events::PortInfo>>(*frame.ports);
    frame.ports = ports;
    switch (field)
    {
    case 0:
      frame.pads[0][0] ^= 1;
      break;
    case 1:
      ports->at(0).name += "other";
      break;
    case 2:
      ports->at(0).controls.push_back(4);
      break;
    case 3:
      ports->at(0).queue.push_back(9);
      break;
    case 4:
      ports->at(0).port = 1;
      break;
    case 5:
      frame.header = ReplayHeader{1, 2, 0, 3, 55};
      break;
    case 6:
      frame.clear_ready = true;
      break;
    case 7:
      frame.menu_exit = MENU_EXIT_FRIENDS;
      break;
    case 8:
      frame.seed = 77;
      break;
    }
    const auto backlog = Stream(changed, 600);
    ReplayStreamReader reader;
    std::string error;
    ASSERT_TRUE(reader.Feed(live.data(), live.size(), &error)) << error;
    reader.Resume();
    EXPECT_FALSE(reader.Feed(backlog.data(), backlog.size(), &error));
    EXPECT_EQ(error, "the match's stream changed");
  }
}

TEST(OrcaReplayStream, RefusesAChangedOverlappingChecksum)
{
  const auto replay = Replay(70, 0);
  const auto live = Stream(replay, 6);
  ReplayChunk chunk;
  chunk.first = 0;
  chunk.frames = replay.frames;
  chunk.checksums.emplace_back(60, 0xabc000u + 61);
  const auto backlog = PackReplayChunk(chunk, true);
  ASSERT_FALSE(backlog.empty());
  ReplayStreamReader reader;
  std::string error;
  ASSERT_TRUE(reader.Feed(live.data(), live.size(), &error)) << error;
  reader.Resume();
  const auto header = PackReplayStreamHeader(*reader.Info());
  ASSERT_TRUE(reader.Feed(header.data(), header.size(), &error)) << error;
  EXPECT_FALSE(reader.Feed(backlog.data(), backlog.size(), &error));
  EXPECT_EQ(error, "the match's stream changed");
  EXPECT_EQ(reader.Checksums().at(60), 0xabc000u + 60);
}

// Damage anywhere is refused, never played: a flipped byte in a chunk, chunks out of order, a
// seed after a history's first frame, another stream version, a header that isn't the stream's.
TEST(OrcaReplayStream, RefusesDamageGapsAndOtherVersions)
{
  const auto replay = Replay(30, 0);
  const std::vector<u8> good = Stream(replay, 10);
  std::string error;
  {
    auto bad = good;
    bad[bad.size() - 20] ^= 0x40;
    ReplayStreamReader reader;
    EXPECT_FALSE(reader.Feed(bad.data(), bad.size(), &error));
    EXPECT_NE(error.find("damaged"), std::string::npos) << error;
    // A failed reader stays failed.
    EXPECT_FALSE(reader.Feed(good.data(), 1, &error));
  }
  {
    std::vector<u8> bytes = PackReplayStreamHeader(StreamInfo());
    ReplayChunk chunk;
    chunk.first = 510;  // the stream starts at 500
    chunk.frames.assign(replay.frames.begin(), replay.frames.begin() + 5);
    const auto packed = PackReplayChunk(chunk, false);
    bytes.insert(bytes.end(), packed.begin(), packed.end());
    ReplayStreamReader reader;
    EXPECT_FALSE(reader.Feed(bytes.data(), bytes.size(), &error));
    EXPECT_NE(error.find("follow on"), std::string::npos) << error;
  }
  {
    ReplayChunk chunk;
    chunk.first = 3;
    chunk.frames.assign(replay.frames.begin(), replay.frames.begin() + 5);
    chunk.frames[2].seed = 9;
    EXPECT_TRUE(PackReplayChunk(chunk, false).empty());
    chunk.frames[2].seed = 0;
    chunk.frames[0].seed = 9;
    EXPECT_TRUE(PackReplayChunk(chunk, false).empty());
    EXPECT_FALSE(PackReplayChunk(chunk, true).empty());
  }
  {
    auto bad = good;
    bad[4] = 3;  // Another stream version.
    ReplayStreamReader reader;
    EXPECT_FALSE(reader.Feed(bad.data(), bad.size(), &error));
  }
  {
    std::vector<u8> bad = {'O', 'R', 'P', '2', 0, 0, 0, 0, 0, 0, 0, 0};
    ReplayStreamReader reader;
    EXPECT_FALSE(reader.Feed(bad.data(), bad.size(), &error));
    EXPECT_NE(error.find("not a replay stream"), std::string::npos) << error;
  }
}

// The servers parse the container and refuse any version but 1 (rooms/src/replay-stream.ts, the
// site's api/replays/orca and tournament-replay-body.ts). 0.3.46 wrote 2 and no recording was
// ever handed off; raise this only together with all three.
TEST(OrcaReplayStream, ContainerVersionIsTheOneTheServersAccept)
{
  const std::vector<u8> header = PackReplayStreamHeader(StreamInfo());
  ASSERT_GE(header.size(), 12u);
  EXPECT_EQ(std::string(header.begin(), header.begin() + 4), "YGRS");
  EXPECT_EQ(header[4] | header[5] << 8 | header[6] << 16 | header[7] << 24, 1);
}

// The site rebuilds a tournament download's header with "v":1; the same build's stream still plays.
TEST(OrcaReplayStream, ReadsAHeaderTheSiteRebuilt)
{
  const auto replay = Replay(30, 0);
  const std::vector<u8> good = Stream(replay, 10);
  std::string bytes(good.begin(), good.end());
  const size_t at = bytes.find("\"v\":2");
  ASSERT_NE(at, std::string::npos);
  bytes[at + 4] = '1';
  std::string error;
  ReplayStreamReader reader("orca2:test");
  EXPECT_TRUE(reader.Feed(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), &error)) << error;
  EXPECT_EQ(reader.Archive().frames.size(), 30u);
}

// A player only reads a stream of its own build: another build's is refused at the header, before
// any frame of it is decoded.
TEST(OrcaReplayStream, RefusesAnotherBuildBeforeItsFrames)
{
  const auto replay = Replay(30, 0);
  const std::vector<u8> bytes = Stream(replay, 10);
  std::string error;
  ReplayStreamReader same("orca2:test");
  EXPECT_TRUE(same.Feed(bytes.data(), bytes.size(), &error)) << error;
  EXPECT_EQ(same.Archive().frames.size(), 30u);
  ReplayStreamReader other("orca2:other");
  EXPECT_FALSE(other.Feed(bytes.data(), bytes.size(), &error));
  EXPECT_NE(error.find("another build"), std::string::npos) << error;
  EXPECT_TRUE(other.Archive().frames.empty());
}

// Bytes from another player are untrusted: random damage, cuts and splices never crash or hang the
// reader, and whatever it accepts is a whole, in-order history.
TEST(OrcaReplayStream, SurvivesRandomDamage)
{
  const auto replay = Replay(200, 100);
  const std::vector<u8> good = Stream(replay, 9);
  std::mt19937 rng(7);
  for (int round = 0; round < 3000; ++round)
  {
    std::vector<u8> bytes = good;
    const int edits = 1 + static_cast<int>(rng() % 4);
    for (int e = 0; e < edits; ++e)
    {
      const size_t at = rng() % bytes.size();
      switch (rng() % 4)
      {
      case 0:
        bytes[at] ^= static_cast<u8>(1u << (rng() % 8));
        break;
      case 1:
        bytes.resize(at);
        break;
      case 2:
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), static_cast<u8>(rng()));
        break;
      default:
        // A large length where a chunk's fields are.
        if (at + 4 <= bytes.size())
          U32(bytes, at, 0xffffffffu - static_cast<u32>(rng() % 16));
        break;
      }
      if (bytes.empty())
        break;
    }
    ReplayStreamReader reader;
    std::string error;
    const size_t piece = 1 + rng() % 64;
    for (size_t at = 0; at < bytes.size(); at += piece)
    {
      if (!reader.Feed(bytes.data() + at, std::min(piece, bytes.size() - at), &error))
        break;
    }
    const ReplayArchive& got = reader.Archive();
    ASSERT_LE(got.frames.size(), replay.frames.size() + 3600);
    for (const auto& frame : got.frames)
      ASSERT_TRUE(frame.ports);
  }
}

// Every byte of a chunk is covered by its hash, the preamble's fields too: a damaged live frame
// (which nothing else checks) is refused like a damaged payload.
TEST(OrcaReplayStream, RefusesADamagedPreamble)
{
  const auto replay = Replay(30, 0);
  const std::vector<u8> good = Stream(replay, 10);
  ReplayStreamInfo info = StreamInfo();
  info.first_frame = 0;
  const size_t header = PackReplayStreamHeader(info).size();
  // The first chunk's live frame: magic (4), first (4), count (4), then live.
  for (const size_t field : {size_t{12}, size_t{16}})
  {
    auto bad = good;
    bad[header + field] ^= 0x01;
    ReplayStreamReader reader;
    std::string error;
    EXPECT_FALSE(reader.Feed(bad.data(), bad.size(), &error)) << field;
    EXPECT_TRUE(reader.Archive().frames.empty()) << field;
  }
}

TEST(OrcaReplayStream, ViewTimeNamesReplaceEveryFrameWithoutChangingSourceMetadata)
{
  auto original = Replay(30, 500);
  original.frames[15].ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "stored rename", false, {9}, {10}},
                                        {1, "stored newcomer", true, {11}, {12}}});
  const auto bytes = Stream(original, 7);
  ReplayStreamReader reader;
  std::string error;
  ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size(), &error)) << error;
  const auto names = Rollback::ReplayStream::ParseNames("0 Alice%20Smith 2 Ren%C3%A9");
  ASSERT_TRUE(names);
  for (const auto& frame : reader.Archive().frames)
  {
    const auto before = *frame.ports;
    const auto view = Rollback::ReplayStream::ViewPorts(before, *names);
    ASSERT_EQ(view.size(), before.size());
    for (size_t i = 0; i < view.size(); ++i)
    {
      EXPECT_EQ(view[i].name, view[i].port == 0 ? "Alice Smith" :
                              view[i].port == 2 ? "Ren\xc3\xa9" : "Player 2");
      EXPECT_EQ(view[i].port, before[i].port);
      EXPECT_EQ(view[i].remote, before[i].remote);
      EXPECT_EQ(view[i].controls, before[i].controls);
      EXPECT_EQ(view[i].queue, before[i].queue);
    }
    EXPECT_EQ(*frame.ports, before);
    const auto fallback = Rollback::ReplayStream::ViewPorts(before, {});
    EXPECT_EQ(fallback[0].name, "Player 1");
  }
  // A reconnect still compares the recording's names, even after views were made with new ones.
  reader.Resume();
  ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size(), &error)) << error;
  EXPECT_EQ(reader.Archive().frames.size(), original.frames.size());
  const auto replacement = Rollback::ReplayStream::ParseNames("0 New%20name");
  ASSERT_TRUE(replacement);
  const auto view = Rollback::ReplayStream::ViewPorts(*reader.Archive().frames[0].ports, *replacement);
  EXPECT_EQ(view[0].name, "New name");
  EXPECT_EQ(view[1].name, "Player 3");
}

TEST(OrcaReplayStream, ViewTimeNamesUseChatUnicodeAndPortLimits)
{
  using Rollback::ReplayStream::ParseNames;
  ASSERT_TRUE(ParseNames(""));
  EXPECT_TRUE(ParseNames("0 O'Neil 1 Player%20Two 2 %F0%9F%90%AC 3 Ada"));
  std::string points;
  for (int i = 0; i < 24; ++i)
    points += "%F0%9F%90%AC";
  EXPECT_TRUE(ParseNames("0 " + points));
  EXPECT_FALSE(ParseNames("0 " + points + "A"));
  for (const char* invalid : {"0 Alice 0 Bob", "4 Alice", "-1 Alice", "00 Alice", "0",
                              "0 %00", "0 %0A", "0 %C2%80", "0 %E2%80%A8", "0 %ED%A0%80",
                              "0 %C0%AF", "0 %FF", "0 %", "0 Alice extra"})
    EXPECT_FALSE(ParseNames(invalid)) << invalid;
}

TEST(OrcaReplayStream, CompletionRejectsAnIncompleteSavedTail)
{
  const auto bytes = Stream(Replay(30, 500), 7);
  ReplayStreamReader whole;
  std::string error;
  ASSERT_TRUE(whole.Feed(bytes.data(), bytes.size(), &error)) << error;
  EXPECT_TRUE(whole.Complete());
  ReplayStreamReader partial;
  ASSERT_TRUE(partial.Feed(bytes.data(), bytes.size() - 1, &error)) << error;
  EXPECT_FALSE(partial.Archive().frames.empty());
  EXPECT_FALSE(partial.Complete());
  ASSERT_TRUE(partial.Feed(bytes.data() + bytes.size() - 1, 1, &error)) << error;
  EXPECT_TRUE(partial.Complete());
  whole.Resume();
  EXPECT_FALSE(whole.Complete());
}

TEST(OrcaReplayStream, WatchSourcesMatchTheExplicitRoomsOrigin)
{
  using Rollback::ReplayStream::WatchSourceAllowed;
  std::string error;
  for (const auto* origin : {"https://rooms.example", "https://staging-rooms.example:8443/"})
  {
    const std::string host = std::string(origin).substr(8);
    const std::string authority = host.substr(0, host.find('/'));
    EXPECT_TRUE(WatchSourceAllowed("wss://" + authority + "/watch/test", origin, false, &error))
        << error;
  }
  EXPECT_TRUE(WatchSourceAllowed("wss://ROOMS.example:443/watch/test", "https://rooms.example",
                                false, &error));
  EXPECT_TRUE(WatchSourceAllowed("wss://rooms.example/watch/test", "https://rooms.example:8443",
                                false, &error));
  for (const auto* source : {"wss://other.example/watch/test",
                             "wss://rooms.example.other.example/watch/test",
                             "wss://user@rooms.example/watch/test",
                             "wss://user:pass@rooms.example/watch/test",
                             "ws://rooms.example/watch/test",
                             "wss://rooms.example:8443/watch/test",
                             "wss://rooms.example:0/watch/test",
                             "wss://rooms.example:65536/watch/test",
                             "wss://rooms.example:/watch/test",
                             "wss://rooms.example:443:8443/watch/test",
                             "wss://rooms%2eexample/watch/test",
                             "wss://rooms.example\\other.example/watch/test",
                             "wss://rooms.example/watch/test#fragment",
                             "wss://rooms.example\n/watch/test",
                             "https://rooms.example/watch/test"})
  {
    error.clear();
    EXPECT_FALSE(WatchSourceAllowed(source, "https://rooms.example", false, &error)) << source;
    EXPECT_FALSE(error.empty());
  }
  for (const auto* origin : {"", "https://", "https://rooms.example/path",
                             "https://user@rooms.example", "http://rooms.example"})
    EXPECT_FALSE(WatchSourceAllowed("wss://rooms.example/watch/test", origin, false, &error));
}

TEST(OrcaReplayStream, PlainWatchSourcesRequireBothLoopbackAndTestKnobs)
{
  using Rollback::ReplayStream::WatchSourceAllowed;
  std::string error;
  for (const auto* host : {"localhost", "127.0.0.1"})
  {
    const std::string origin = std::string("http://") + host + ":45678";
    const std::string source = std::string("ws://") + host + ":45678/watch/test";
    EXPECT_TRUE(WatchSourceAllowed(source, origin, true, &error)) << error;
    EXPECT_FALSE(WatchSourceAllowed(source, origin, false, &error));
    EXPECT_FALSE(WatchSourceAllowed(source, "", true, &error));
    EXPECT_FALSE(WatchSourceAllowed(std::string("ws://") + host + ":45679/watch/test", origin,
                                   true, &error));
  }
  EXPECT_FALSE(WatchSourceAllowed("ws://rooms.example/watch/test", "https://rooms.example",
                                 true, &error));
  EXPECT_FALSE(WatchSourceAllowed("ws://127.1/watch/test", "https://127.1", true, &error));
}

TEST(OrcaReplayStream, PendingWatchBytesAndResetsShareOneLimit)
{
  using namespace Rollback::ReplayStream;
  WatchPending pending;
  const std::vector<u8> bytes(WATCH_PENDING_MAX - sizeof(size_t), 7);
  ASSERT_TRUE(pending.Append(bytes));
  ASSERT_TRUE(pending.Reset());
  EXPECT_EQ(pending.resets, std::vector<size_t>{bytes.size()});
  EXPECT_FALSE(pending.Append(std::array<u8, 1>{8}));
  EXPECT_FALSE(pending.Reset());
  EXPECT_EQ(pending.bytes.size(), bytes.size());
  EXPECT_EQ(pending.resets.size(), 1u);
  pending = {};
  ASSERT_TRUE(pending.Append(std::vector<u8>(WATCH_PENDING_MAX, 9)));
  EXPECT_FALSE(pending.Reset());
  EXPECT_FALSE(pending.Append(std::array<u8, 1>{8}));
}

TEST(OrcaReplayStream, PendingWatchResetsAloneReachTheLimit)
{
  using namespace Rollback::ReplayStream;
  WatchPending pending;
  for (size_t i = 0; i < WATCH_PENDING_MAX / sizeof(size_t); ++i)
    ASSERT_TRUE(pending.Reset()) << i;
  EXPECT_FALSE(pending.Reset());
  EXPECT_FALSE(pending.Append(std::array<u8, 1>{8}));
  EXPECT_TRUE(pending.bytes.empty());
  EXPECT_EQ(pending.resets.size(), WATCH_PENDING_MAX / sizeof(size_t));
}

TEST(OrcaReplayStream, LocalSourcesMustBeRegularFiles)
{
  const std::string directory = File::CreateTempDir();
  ASSERT_FALSE(directory.empty());
  const Common::ScopeGuard cleanup([&] { File::DeleteDirRecursively(directory); });
  const std::string path = directory + "/replay.ygr";
  ASSERT_TRUE(File::WriteStringToFile(path, "replay"));
  std::string error;
  auto file = Rollback::ReplayStream::OpenReplayFile(path, &error);
  ASSERT_NE(file, nullptr) << error;
  EXPECT_EQ(file->GetSize(), 6u);
  EXPECT_EQ(Rollback::ReplayStream::OpenReplayFile(directory, &error), nullptr);
#ifndef _WIN32
  const std::string fifo = directory + "/fifo";
  ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(Rollback::ReplayStream::OpenReplayFile(fifo, &error), nullptr);
  EXPECT_EQ(error, "the replay source must be a regular file");
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
#endif
}

TEST(OrcaReplayStream, ProductWatchRefusalsEndOnTheFirstStep)
{
  const auto set = [](const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    if (value.empty())
      unsetenv(key);
    else
      setenv(key, value.c_str(), 1);
#endif
  };
  const std::string old_source = Orca::GetEnv("ORCA_REPLAY_SOURCE");
  const std::string old_origin = Orca::GetEnv("ORCA_REPLAY_ROOMS_ORIGIN");
  const Common::ScopeGuard restore([&] {
    set("ORCA_REPLAY_SOURCE", old_source);
    set("ORCA_REPLAY_ROOMS_ORIGIN", old_origin);
  });
  // These are parsed and refused before any socket is created.
  set("ORCA_REPLAY_ROOMS_ORIGIN", "https://127.0.0.1:45678");
  for (const auto* source : {"wss://localhost:45678/watch/test",
                             "wss://user@127.0.0.1:45678/watch/test",
                             "wss://127.0.0.1:45679/watch/test"})
  {
    set("ORCA_REPLAY_SOURCE", source);
    auto player = Rollback::ReplayStream::Player::FromEnv();
    ASSERT_NE(player, nullptr);
    std::string stopped;
    Rollback::ReplayStream::PlayerHost host;
    host.stop = [&](const std::string& message) { stopped = message; };
    int running = -1;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(player->Step(Core::System::GetInstance(), host, &running, 0, 0, {}));
    EXPECT_TRUE(player->Over());
    EXPECT_NE(stopped.find("The replay couldn't be played:"), std::string::npos);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  }
}

TEST(OrcaKeyframe, TagAssignmentsAreBoundedAndRoundTripWithoutNames)
{
  auto replay = Replay(0);
  replay.boundary.tag_assignments = {{0, 119, true, true}};
  const auto packed = PackKeyframe(0, replay);
  ASSERT_FALSE(packed.empty());
  int frame = -1;
  ReplayArchive decoded;
  ASSERT_TRUE(UnpackKeyframe(packed, &frame, &decoded));
  EXPECT_EQ(decoded.boundary.tag_assignments, replay.boundary.tag_assignments);
  const auto original = Raw(packed);
  for (const u32 value : {120u << 2, 1u << 11})
  {
    auto raw = original;
    U32(raw, raw.size() - 12, value);
    EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
  }
  replay.boundary.tag_assignments = {{0, 1, false, false}, {0, 2, false, false}};
  EXPECT_TRUE(PackKeyframe(0, replay).empty());
  replay.boundary.tag_assignments = {{0, 120, false, false}};
  EXPECT_TRUE(PackKeyframe(0, replay).empty());
}

TEST(OrcaReplayStream, WriterStripsRealNamesAndPreservesTagDecisions)
{
  const std::string directory = File::CreateTempDir();
  const Common::ScopeGuard cleanup([&] { File::DeleteDirRecursively(directory); });
  const std::string base = directory + "/recording";
  auto replay = Replay(120);
  const auto real_ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "Alice", false}, {1, "Bob", true}});
  for (auto& frame : replay.frames)
  {
    frame.ports = real_ports;
    frame.tag_assignments.emplace();
  }
  replay.frames[0].tag_assignments = {{0, 2, false, false}, {1, 3, true, false}};
  {
    Rollback::ReplayStream::Writer writer(base);
    const std::map<int, u64> checksums{{60, 123}, {120, 456}};
    ASSERT_TRUE(writer.Pump(replay, 120, 120, &checksums));
  }
  File::IOFile file(base + "-1.ygr", "rb");
  ASSERT_TRUE(file);
  std::vector<u8> bytes(file.GetSize());
  ASSERT_TRUE(file.ReadBytes(bytes.data(), bytes.size()));
  ReplayStreamReader reader;
  std::string error;
  ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size(), &error)) << error;
  ASSERT_TRUE(reader.Complete());
  ASSERT_TRUE(reader.Info());
  EXPECT_TRUE(reader.Info()->seat_labels);
  for (const auto& frame : reader.Archive().frames)
    for (const auto& port : *frame.ports)
      EXPECT_EQ(port.name, "P" + std::to_string(port.port + 1));
  EXPECT_EQ(reader.Archive().frames[0].tag_assignments, replay.frames[0].tag_assignments);
  EXPECT_EQ(reader.Checksums().size(), 2u);
  EXPECT_EQ(real_ports->at(0).name, "Alice");
  // The live join history keeps real names and all non-text decisions.
  ReplayArchive joined;
  int frame = -1;
  replay.boundary.ports = real_ports;
  ASSERT_TRUE(UnpackKeyframe(PackKeyframe(120, replay), &frame, &joined));
  EXPECT_EQ(joined.frames[0].ports->at(0).name, "Alice");
  EXPECT_EQ(joined.frames[0].tag_assignments, replay.frames[0].tag_assignments);
}

TEST(OrcaReplayStream, RecordingMetadataUsesSeatLabelsAcrossRenamesAndRepacking)
{
  auto original = Replay(120);
  std::vector<Orca::Events::PortInfo> live{{0, "Alice", false, {1}, {2}},
                                          {1, "Bob", true, {3}, {4}},
                                          {2, "Carol", true, {5}, {6}},
                                          {3, "Dan", true, {7}, {8}}};
  for (auto& frame : original.frames)
  {
    frame.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
        RecordingPorts(live));
    live[0].name += "x";
  }
  original.boundary.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      RecordingPorts(live));
  auto info = StreamInfo();
  info.first_frame = original.first_frame;
  info.seat_labels = true;
  auto bytes = PackReplayStream(info, original, {{60, 123}, {120, 456}});
  for (int pass = 0; pass < 2; ++pass)
  {
    ReplayStreamReader reader;
    std::string error;
    ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size(), &error)) << error;
    ASSERT_TRUE(reader.Complete());
    EXPECT_TRUE(reader.Info()->seat_labels);
    EXPECT_EQ(reader.Checksums().at(60), 123u);
    EXPECT_EQ(reader.Checksums().at(120), 456u);
    for (const auto& frame : reader.Archive().frames)
      for (const auto& port : *frame.ports)
      {
        EXPECT_EQ(port.name, "P" + std::to_string(port.port + 1));
        EXPECT_TRUE(port.remote);  // Playback has no local player.
        EXPECT_EQ(port.controls, live[port.port].controls);
        EXPECT_EQ(port.queue, live[port.port].queue);
      }
    bytes = PackReplayStream(*reader.Info(), reader.Archive(), reader.Checksums());
  }
  EXPECT_EQ(live[1].name, "Bob");
}

TEST(OrcaReplayStream, SeatLabelFlagIsOptionalAndTyped)
{
  const auto header = PackReplayStreamHeader(StreamInfo());
  const std::string json(header.begin() + 12, header.end());
  const std::string field = "\"seat_labels\":false,";
  const auto at = json.find(field);
  ASSERT_NE(at, std::string::npos);
  for (const std::string replacement : {"", "\"seat_labels\":true,", "\"seat_labels\":\"false\","})
  {
    SCOPED_TRACE(replacement);
    auto changed = json;
    changed.replace(at, field.size(), replacement);
    auto bytes = header;
    bytes.resize(12);
    U32(bytes, 8, static_cast<u32>(changed.size()));
    bytes.insert(bytes.end(), changed.begin(), changed.end());
    ReplayStreamReader reader;
    std::string error;
    if (replacement.find("\"false\"") != std::string::npos)
      EXPECT_FALSE(reader.Feed(bytes.data(), bytes.size(), &error));
    else
    {
      ASSERT_TRUE(reader.Feed(bytes.data(), bytes.size(), &error)) << error;
      ASSERT_TRUE(reader.Info());
      EXPECT_EQ(reader.Info()->seat_labels, !replacement.empty());
    }
  }
}
