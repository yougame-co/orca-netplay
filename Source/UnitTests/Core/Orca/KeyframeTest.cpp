// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <zstd.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Orca/Session/Keyframe.h"

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
