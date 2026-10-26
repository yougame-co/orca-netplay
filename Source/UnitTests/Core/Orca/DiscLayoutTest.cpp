// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/Config/Config.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/DiscLayout.h"
#include "Core/Orca/DiscReport.h"
#include "Core/Orca/RebuildStandard.h"
#include "Core/Orca/StandardBlob.h"
#include "DiscIO/Blob.h"
#include "DiscIO/CISOBlob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWad.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WIABlob.h"
#include "DiscIO/WbfsBlob.h"
#include "OrcaDiscTool/OutputFile.h"

namespace
{
using Wii = DiscIO::VolumeWii;
namespace Layout = Orca::DiscLayout;
namespace Report = Orca::DiscReport;
namespace Rebuild = Orca::RebuildStandard;

void Write32(std::vector<u8>* bytes, size_t offset, u64 value)
{
  const u32 be = Common::swap32(static_cast<u32>(value));
  std::memcpy(bytes->data() + offset, &be, sizeof(be));
}

void Write16(std::vector<u8>* bytes, size_t offset, u16 value)
{
  const u16 be = Common::swap16(value);
  std::memcpy(bytes->data() + offset, &be, sizeof(be));
}

void Text(std::vector<u8>* bytes, size_t offset, const std::string& value)
{
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

// A single hash group, invented files, and an invented encrypted ticket field. The normal
// ticket reader uses default IOSC entries in memory; no dumped keys or Nintendo data.
class FixtureBlob final : public DiscIO::BlobReader
{
public:
  explicit FixtureBlob(std::vector<u8> data,
                       DiscIO::DataSizeType size_type = DiscIO::DataSizeType::Accurate)
      : m_data(std::move(data)), m_size_type(size_type)
  {
  }
  DiscIO::BlobType GetBlobType() const override { return DiscIO::BlobType::PLAIN; }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override
  {
    return std::make_unique<FixtureBlob>(m_data, m_size_type);
  }
  u64 GetRawSize() const override { return m_data.size(); }
  u64 GetDataSize() const override { return m_data.size(); }
  DiscIO::DataSizeType GetDataSizeType() const override { return m_size_type; }
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return {}; }
  bool Read(u64 offset, u64 size, u8* out) override
  {
    if (m_size_type == DiscIO::DataSizeType::LowerBound)
    {
      // NFS semantics: unstored blocks, including the tail, read as zeros.
      if (size > std::numeric_limits<u64>::max() - offset)
        return false;
      const u64 stored = offset < m_data.size() ? std::min(size, m_data.size() - offset) : 0;
      if (stored != 0)
        std::memcpy(out, m_data.data() + offset, stored);
      std::fill_n(out + stored, size - stored, 0);
      return true;
    }
    if (offset > m_data.size() || size > m_data.size() - offset)
      return false;
    std::memcpy(out, m_data.data() + offset, size);
    return true;
  }
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* out, u64) override
  {
    return Read(offset, size, out);
  }

private:
  std::vector<u8> m_data;
  DiscIO::DataSizeType m_size_type;
};

std::vector<u8> BlankTailImage(bool hashes)
{
  std::vector<u8> image(0x68000);
  Write32(&image, 0x18, DiscIO::WII_DISC_MAGIC);
  image[0x60] = hashes ? 0 : 1;
  image[0x61] = 1;  // NFS exposes unencrypted sectors to VolumeWii.
  Write32(&image, 0x40000, 1);
  Write32(&image, 0x40004, 0x40020 / 4);
  Write32(&image, 0x40020, 0x50000 / 4);
  Write32(&image, 0x50000 + 0x2b8, 0x10000 / 4);
  Write32(&image, 0x50000 + 0x2bc, Wii::BLOCK_TOTAL_SIZE / 4);
  for (size_t i = 0x60000; i < image.size(); ++i)
    image[i] = static_cast<u8>(i * 7 + 3);
  return image;
}

TEST(VolumeWiiBlob, LowerBoundBlankTailMatchesPaddedImage)
{
  for (const bool hashes : {false, true})
  {
    SCOPED_TRACE(hashes);
    const auto stored = BlankTailImage(hashes);
    auto padded = stored;
    padded.resize(stored.size() + 4 * Wii::BLOCK_TOTAL_SIZE);
    Wii expected(std::make_unique<FixtureBlob>(padded), false);
    // Deliberately retain BlobType::PLAIN: the guard must use size semantics, not the type name.
    Wii actual(std::make_unique<FixtureBlob>(stored, DiscIO::DataSizeType::LowerBound), false);
    const auto partition = expected.GetGamePartition();
    ASSERT_EQ(actual.GetGamePartition(), partition);
    const u64 block_size = hashes ? Wii::BLOCK_DATA_SIZE : Wii::BLOCK_TOTAL_SIZE;
    for (const u64 offset : {block_size - 16, block_size, 3 * block_size + 3})
    {
      std::array<u8, Wii::BLOCK_DATA_SIZE + 32> a{}, b{};
      b.fill(0xff);
      ASSERT_TRUE(expected.Read(offset, a.size(), a.data(), partition));
      ASSERT_TRUE(actual.Read(offset, b.size(), b.data(), partition));
      EXPECT_EQ(a, b);
      if (offset >= block_size)
        EXPECT_TRUE(std::ranges::all_of(b, [](u8 byte) { return byte == 0; }));
    }
    std::array<u8, 32> bytes{};
    EXPECT_FALSE(actual.Read(std::numeric_limits<u64>::max(), 1, bytes.data(), partition));
    EXPECT_FALSE(actual.Read(16, std::numeric_limits<u64>::max(), bytes.data(), partition));
  }
}

TEST(VolumeWiiBlob, FiniteContainerBlankTailsMatchPaddedImage)
{
  const std::string directory = File::CreateTempDir();
  ASSERT_FALSE(directory.empty());
  struct Cleanup
  {
    std::string path;
    ~Cleanup() { File::DeleteDirRecursively(path); }
  } cleanup{directory};
  const auto write = [](const std::string& path, const std::vector<u8>& bytes) {
    return File::WriteStringToFile(
        path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  };
  const auto callback = [](const std::string&, float) { return true; };
  for (const bool hashes : {false, true})
  {
    const auto stored = BlankTailImage(hashes);
    auto padded = stored;
    padded.resize(0x100000);
    FixtureBlob source(padded);
    Wii expected(source.CopyReader(), false);
    for (const auto type : {DiscIO::BlobType::PLAIN, DiscIO::BlobType::SPLIT_PLAIN,
                            DiscIO::BlobType::GCZ, DiscIO::BlobType::WIA, DiscIO::BlobType::RVZ,
                            DiscIO::BlobType::CISO, DiscIO::BlobType::WBFS})
    {
      SCOPED_TRACE(static_cast<int>(type));
      SCOPED_TRACE(hashes);
      std::string path = directory + "/tail";
      if (type == DiscIO::BlobType::CISO)
      {
        DiscIO::CISOHeader header{};
        header.magic = DiscIO::CISO_MAGIC;
        header.block_size = Wii::BLOCK_TOTAL_SIZE;
        std::fill_n(header.map, stored.size() / header.block_size, 1);
        std::vector<u8> container(sizeof(header) + stored.size());
        std::memcpy(container.data(), &header, sizeof(header));
        std::copy(stored.begin(), stored.end(), container.begin() + sizeof(header));
        ASSERT_TRUE(write(path, container));
      }
      else if (type == DiscIO::BlobType::WBFS)
      {
        // One stored cluster, followed by unmapped clusters that resolve into cluster zero.
        std::vector<u8> container(0x200000);
        std::memcpy(container.data(), "WBFS", 4);
        Write32(&container, 4, container.size() / 512);
        container[8] = 9;
        container[9] = 20;
        container[12] = 1;
        Write16(&container, 512 + 256, 1);
        std::copy(stored.begin(), stored.end(), container.begin() + 0x100000);
        ASSERT_TRUE(write(path, container));
      }
      else if (type == DiscIO::BlobType::GCZ)
      {
        ASSERT_TRUE(DiscIO::ConvertToGCZ(&source, "", path, 0, Wii::BLOCK_TOTAL_SIZE, callback));
      }
      else if (type == DiscIO::BlobType::WIA || type == DiscIO::BlobType::RVZ)
      {
        ASSERT_TRUE(DiscIO::ConvertToWIAOrRVZ(&source, "", path, type == DiscIO::BlobType::RVZ,
                                            DiscIO::WIARVZCompressionType::None, 0,
                                            Wii::GROUP_TOTAL_SIZE, callback));
      }
      else if (type == DiscIO::BlobType::SPLIT_PLAIN)
      {
        path += ".part0.iso";
        ASSERT_TRUE(write(path, stored));
        ASSERT_TRUE(write(directory + "/tail.part1.iso",
                          std::vector<u8>(padded.size() - stored.size())));
      }
      else
      {
        ASSERT_TRUE(write(path, padded));
      }
      auto actual = DiscIO::CreateDisc(path, false);
      ASSERT_TRUE(actual);
      ASSERT_EQ(actual->GetBlobType(), type);
      const auto partition = actual->GetGamePartition();
      const u64 block_size = hashes ? Wii::BLOCK_DATA_SIZE : Wii::BLOCK_TOTAL_SIZE;
      for (const u64 offset : {block_size - 16, block_size, 3 * block_size + 3})
      {
        std::array<u8, Wii::BLOCK_DATA_SIZE + 32> a{}, b{};
        b.fill(0xff);
        ASSERT_TRUE(expected.Read(offset, a.size(), a.data(), partition));
        ASSERT_TRUE(actual->Read(offset, b.size(), b.data(), partition));
        EXPECT_EQ(a, b);
      }
      // WBFS also serves unmapped logical clusters beyond the stored cluster.
      if (type == DiscIO::BlobType::WBFS)
      {
        std::array<u8, 32> bytes{};
        bytes.fill(0xff);
        const u64 raw_offset = 0x210000;
        const u64 offset = hashes ? (raw_offset - 0x60000) / Wii::BLOCK_TOTAL_SIZE *
                                       Wii::BLOCK_DATA_SIZE :
                                   raw_offset - 0x60000;
        ASSERT_TRUE(actual->Read(offset, bytes.size(), bytes.data(), partition));
        EXPECT_TRUE(std::ranges::all_of(bytes, [](u8 byte) { return byte == 0; }));
      }
      std::array<u8, 32> bytes{};
      const u64 raw_size = actual->GetDataSize() - 0x60000;
      const u64 logical_size = hashes ? raw_size / Wii::BLOCK_TOTAL_SIZE * Wii::BLOCK_DATA_SIZE :
                                       raw_size;
      EXPECT_FALSE(actual->Read(logical_size, 1, bytes.data(), partition));
      EXPECT_FALSE(actual->Read(logical_size - 1, 2, bytes.data(), partition));
      actual.reset();
      ASSERT_TRUE(File::Delete(path));
    }
  }
}

TEST(VolumeWiiBlob, DirectoryBlankTailReadsRemainBounded)
{
  const std::string directory = File::CreateTempDir();
  ASSERT_FALSE(directory.empty());
  struct Cleanup
  {
    std::string path;
    ~Cleanup() { File::DeleteDirRecursively(path); }
  } cleanup{directory};
  ASSERT_TRUE(File::CreateDir(directory + "/sys"));
  ASSERT_TRUE(File::CreateDir(directory + "/files"));
  ASSERT_TRUE(File::CreateDir(directory + "/disc"));
  for (const bool hashes : {false, true})
  {
    auto boot = BlankTailImage(hashes);
    boot.resize(0x440);
    ASSERT_TRUE(File::WriteStringToFile(
        directory + "/sys/boot.bin",
        std::string_view(reinterpret_cast<const char*>(boot.data()), boot.size())));
    ASSERT_TRUE(File::WriteStringToFile(
        directory + "/disc/header.bin",
        std::string_view(reinterpret_cast<const char*>(boot.data()), 0x100)));
    ASSERT_TRUE(File::WriteStringToFile(directory + "/sys/main.dol", std::string(0x100, '\0')));
    ASSERT_TRUE(
        File::WriteStringToFile(directory + "/sys/apploader.img", std::string(0x20, '\0')));
    auto actual = DiscIO::CreateDisc(directory + "/sys/main.dol", false);
    ASSERT_TRUE(actual);
    ASSERT_EQ(actual->GetBlobType(), DiscIO::BlobType::DIRECTORY);
    const auto partition = actual->GetGamePartition();
    const auto data_offset = actual->ReadSwappedAndShifted(partition.offset + 0x2b8,
                                                         DiscIO::PARTITION_NONE);
    ASSERT_TRUE(data_offset);
    const u64 raw_size = actual->GetDataSize() - partition.offset - *data_offset;
    const u64 logical_size = hashes ? raw_size / Wii::BLOCK_TOTAL_SIZE * Wii::BLOCK_DATA_SIZE :
                                     raw_size;
    std::array<u8, 32> bytes{};
    bytes.fill(0xff);
    ASSERT_TRUE(actual->Read(logical_size - bytes.size(), bytes.size(), bytes.data(), partition));
    EXPECT_TRUE(std::ranges::all_of(bytes, [](u8 byte) { return byte == 0; }));
    EXPECT_FALSE(actual->Read(logical_size, 1, bytes.data(), partition));
  }
}

enum class GapData
{
  Zero,
  Formula,
  Seeded,
  Random,
  PaddedSeeded,
  PhysicalZeroFormula,
  PhysicalZeroSeeded,
  PhysicalZeroTree,
};

enum class SourceMetadata
{
  Normal,
  DeepFST,
  BadName,
  HugeFile,
  HugeFST,
};

std::vector<u8> WiiImage(bool moved, bool changed = false, bool other_gap = false,
                         bool wrong_id = false, bool odd_strings = false, bool opaque_fst = false,
                         GapData gap_data = GapData::Zero, u8 disc_number = 0,
                         size_t block_count = Wii::BLOCKS_PER_GROUP,
                         SourceMetadata metadata = SourceMetadata::Normal,
                         bool alias_fixture = false)
{
  const u64 partition = moved ? 0x80000 : 0x50000;
  const u64 data_offset = 0x20000;
  const u64 dol = moved ? 0x8000 : 0x3000;
  const u64 fst = moved ? 0x9000 : 0x4000;
  const u64 file_a = moved ? 0xa000 : 0x5000;
  const u64 file_b = moved ? 0xb000 : 0x6000;
  std::vector<u8> data(block_count * Wii::BLOCK_DATA_SIZE);
  Text(&data, 0, wrong_id ? "RWRN01" : "RTEE01");
  data[6] = disc_number;
  Write32(&data, 0x18, DiscIO::WII_DISC_MAGIC);
  Text(&data, 0x20, "Invented fixture");
  Write32(&data, 0x420, dol / 4);
  Write32(&data, 0x424, fst / 4);
  Write32(&data, 0x428, 0x80 / 4);
  Write32(&data, 0x42c, 0x80 / 4);
  Write32(&data, 0x2440 + 0x14, 0x10);
  Text(&data, 0x2460, "invented loader!");
  Write32(&data, dol, 0x100);
  Write32(&data, dol + 0x90, 4);
  Text(&data, dol + 0x100, "code");
  // Root, hello.txt, folder, nested.bin, empty. String offsets intentionally need not
  // follow table order, and suffix-sharing is supported by the layout representation.
  const u32 hello = odd_strings ? 24 : 0;
  const u32 folder = odd_strings ? 11 : 10;
  const u32 nested = odd_strings ? 0 : 17;
  const u32 empty = odd_strings ? 18 : 28;
  Write32(&data, fst, 0x01000000);
  Write32(&data, fst + 8, 5);
  Write32(&data, fst + 12, hello);
  Write32(&data, fst + 16, file_a / 4);
  Write32(&data, fst + 20, 5);
  Write32(&data, fst + 24, 0x01000000 | folder);
  Write32(&data, fst + 32, 5);
  Write32(&data, fst + 36, nested);
  Write32(&data, fst + 40, file_b / 4);
  Write32(&data, fst + 44, 3);
  Write32(&data, fst + 48, empty);
  // Zero-length files can point inside another file and do not consume a gap.
  Write32(&data, fst + 52, 0);
  Text(&data, fst + 60 + hello, "hello.txt");
  Text(&data, fst + 60 + folder, "folder");
  Text(&data, fst + 60 + nested, "nested.bin");
  Text(&data, fst + 60 + empty, "empty");
  if (opaque_fst)
    data[fst + 0x7e] = 0x91;
  Text(&data, file_a, "hello");
  Text(&data, file_b, changed ? "abX" : "abc");
  if (gap_data != GapData::Zero)
  {
    const std::array<Layout::Range, 5> gaps{{{0x2470, dol - 0x2470},
                                             {dol + 0x104, fst - dol - 0x104},
                                             {fst + 0x80, file_a - fst - 0x80},
                                             {file_a + 5, file_b - file_a - 5},
                                             {file_b + 3, data.size() - file_b - 3}}};
    // Disc mastering and RVZ use 0x8000 decrypted bytes, independently of the
    // physical sectors' 0x7c00-byte payloads and the layout implementation.
    constexpr u64 junk_unit_size = 0x8000;
    std::vector<u8> junk(junk_unit_size);
    std::mt19937 random(0x12345678);
    for (const auto& gap : gaps)
    {
      for (u64 at = gap.offset; at < gap.offset + gap.size;)
      {
        const u64 unit = at / junk_unit_size;
        auto seed = Layout::FormulaSeed("RTEE01", disc_number, at);
        if (gap_data != GapData::Formula && gap_data != GapData::PhysicalZeroFormula)
        {
          for (size_t i = 0; i < seed.size(); ++i)
            seed[i] = Common::swap32(static_cast<u32>(0x12345678 + i * 0x1020304 + unit));
        }
        DiscIO::LaggedFibonacciGenerator lfg;
        lfg.SetSeed(seed.data());
        lfg.GetBytes(junk.size(), junk.data());
        const u64 within = at % junk_unit_size;
        const u64 length = std::min(gap.offset + gap.size - at, junk.size() - within);
        for (u64 i = 0; i < length; ++i)
          data[at + i] = gap_data == GapData::Random ? static_cast<u8>(random()) : junk[within + i];
        at += length;
      }
      if (gap_data == GapData::PaddedSeeded)
        std::fill_n(data.begin() + gap.offset, gap.offset == 0x2470 ? gap.size - 97 : 32, 0);
    }
  }
  if (other_gap)
    data[0x2900] = 0x81;
  if (metadata == SourceMetadata::DeepFST)
  {
    constexpr u32 count = 201;
    std::fill_n(data.begin() + fst, 0x1000, 0);
    Write32(&data, 0x428, 0x1000 / 4);
    Write32(&data, 0x42c, 0x1000 / 4);
    for (u32 i = 0; i < count; ++i)
    {
      Write32(&data, fst + i * 12, 0x01000000);
      Write32(&data, fst + i * 12 + 4, i == 0 ? 0 : i - 1);
      Write32(&data, fst + i * 12 + 8, count);
    }
    Text(&data, fst + count * 12, "d");
  }
  if (metadata == SourceMetadata::BadName)
    Write32(&data, fst + 12, 0x00ffffff);
  if (metadata == SourceMetadata::HugeFile)
    Write32(&data, fst + 16, 0xffffffff);
  if (metadata == SourceMetadata::HugeFST)
    Write32(&data, 0x424, 0xffffffff);
  if (alias_fixture)
  {
    Text(&data, 0, "RSBE01");
    data[7] = 1;
    data[0x3dd00 + 0x11a1f] = 1;
  }

  std::vector<u8> ticket(DiscIO::WII_PARTITION_TICKET_SIZE);
  Write32(&ticket, 0, 0x10001);
  Text(&ticket, 0x140, "Invented-XS");
  std::fill_n(ticket.begin() + offsetof(IOS::ES::Ticket, title_key), 16, 0x6b);
  // These signature bytes are invented markers, never valid signatures.
  std::fill(ticket.begin() + 4, ticket.begin() + 0x104, moved ? 0x41 : 0x42);
  const IOS::ES::TicketReader reader(ticket);
  const auto key = reader.GetTitleKey(IOS::HLE::IOSC{reader.GetConsoleType(), false});
  FixtureBlob blob(data);
  std::vector<std::array<u8, Wii::BLOCK_DATA_SIZE>> blocks(Wii::BLOCKS_PER_GROUP);
  std::vector<Wii::HashBlock> hashes(Wii::BLOCKS_PER_GROUP);
  std::vector<u8> h3(DiscIO::WII_PARTITION_H3_SIZE);
  const u64 physical_size = block_count * Wii::BLOCK_TOTAL_SIZE;
  std::vector<u8> encrypted(physical_size);
  for (size_t group = 0; group * Wii::BLOCKS_PER_GROUP < block_count; ++group)
  {
    for (size_t i = 0; i < Wii::BLOCKS_PER_GROUP; ++i)
    {
      const size_t block = group * Wii::BLOCKS_PER_GROUP + i;
      blocks[i].fill(0);
      if (block < block_count)
        std::copy_n(data.begin() + block * Wii::BLOCK_DATA_SIZE, Wii::BLOCK_DATA_SIZE,
                    blocks[i].begin());
    }
    EXPECT_TRUE(Wii::HashGroup(blocks.data(), hashes.data()));
    const auto group_hash = Common::SHA1::CalculateDigest(hashes[0].h2);
    std::copy(group_hash.begin(), group_hash.end(), h3.begin() + group * group_hash.size());
    auto group_bytes = std::make_unique<std::array<u8, Wii::GROUP_TOTAL_SIZE>>();
    EXPECT_TRUE(Wii::EncryptGroup(group * Wii::GROUP_DATA_SIZE, 0, data.size(), key, &blob,
                                  group_bytes.get()));
    const size_t offset = group * Wii::GROUP_TOTAL_SIZE;
    std::copy_n(group_bytes->begin(), std::min<u64>(Wii::GROUP_TOTAL_SIZE, physical_size - offset),
                encrypted.begin() + offset);
  }
  std::vector<u8> tmd(sizeof(IOS::ES::TMDHeader) + sizeof(IOS::ES::Content));
  Write32(&tmd, 0, 0x10001);
  std::fill(tmd.begin() + 4, tmd.begin() + 0x104, moved ? 0x71 : 0x72);
  Text(&tmd, 0x140, "Invented-CP");
  Write16(&tmd, offsetof(IOS::ES::TMDHeader, num_contents), 1);
  auto h3_hash = Common::SHA1::CalculateDigest(h3);
  if (alias_fixture)
    h3_hash = {0xab, 0x93, 0xef, 0x28, 0x0b, 0x83, 0x45, 0x72, 0x52, 0x3f,
               0x64, 0xc5, 0x5c, 0x6a, 0x32, 0x05, 0xea, 0xd6, 0x6d, 0x04};
  std::copy(h3_hash.begin(), h3_hash.end(),
            tmd.begin() + sizeof(IOS::ES::TMDHeader) + offsetof(IOS::ES::Content, sha1));
  std::vector<u8> image(partition + data_offset + physical_size);
  std::copy_n(data.begin(), 0x100, image.begin());
  Write32(&image, 0x40000, 2);
  Write32(&image, 0x40004, 0x40020 / 4);
  Write32(&image, 0x40020, partition / 4);
  // A non-game table entry is enough to ensure rebuilding drops it; never read its data.
  Write32(&image, 0x40028, (partition + data_offset + physical_size - 0x8000) / 4);
  Write32(&image, 0x4002c, 1);
  Write32(&image, 0x4e000, 1);
  std::copy(ticket.begin(), ticket.end(), image.begin() + partition);
  Write32(&image, partition + 0x2a4, tmd.size());
  Write32(&image, partition + 0x2a8, 0x2c0 / 4);
  Write32(&image, partition + 0x2ac, 0x20);
  Write32(&image, partition + 0x2b0, 0x500 / 4);
  Write32(&image, partition + 0x2b4, 0x4000 / 4);
  Write32(&image, partition + 0x2b8, data_offset / 4);
  Write32(&image, partition + 0x2bc, physical_size / 4);
  std::copy(tmd.begin(), tmd.end(), image.begin() + partition + 0x2c0);
  Text(&image, partition + 0x500, "invented certificate bytes");
  std::copy(h3.begin(), h3.end(), image.begin() + partition + 0x4000);
  std::copy_n(encrypted.begin(), physical_size, image.begin() + partition + data_offset);
  if (gap_data == GapData::PhysicalZeroFormula || gap_data == GapData::PhysicalZeroSeeded)
  {
    // Like a scrubbed ISO, erase the complete encrypted sectors (including hash/IV
    // headers), not their plaintext. All affected payloads are outside the files.
    for (size_t block : {size_t{1}, size_t{2}})
      std::fill_n(image.begin() + partition + data_offset + block * 0x8000, 0x8000, 0);
    if (gap_data == GapData::PhysicalZeroFormula)
      std::fill_n(image.begin() + partition + data_offset + (block_count - 1) * 0x8000, 0x8000, 0);
  }
  if (gap_data == GapData::PhysicalZeroTree)
  {
    // Retain the tree generated before erasure: individual leaves, an entire
    // subgroup, a full group, and a final group containing only three sectors.
    for (size_t block = 0; block < block_count; ++block)
    {
      if (block == 1 || block == 2 || (block >= 8 && block < 16) || (block >= 64 && block < 128) ||
          block >= 192)
        std::fill_n(image.begin() + partition + data_offset + block * Wii::BLOCK_TOTAL_SIZE,
                    Wii::BLOCK_TOTAL_SIZE, 0);
    }
  }
  return image;
}

bool WriteBytes(const std::string& path, const std::vector<u8>& bytes)
{
  return File::WriteStringToFile(
      path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

class DiscLayoutTest : public testing::Test
{
protected:
  void SetUp() override
  {
    Common::RegisterMsgAlertHandler(
        [](const char*, const char*, bool, Common::MsgType) { return false; });
    directory = File::CreateTempDir();
    ASSERT_FALSE(directory.empty());
    old_wii_root = File::GetUserPath(D_WIIROOT_IDX);
    File::SetUserPath(D_WIIROOT_IDX, directory + "/empty-wii/");
    original = directory + "/original.iso";
    variant = directory + "/variant.iso";
    layout_path = directory + "/layout.json";
    reference_path = directory + "/reference.json";
    output = directory + "/out.rvz";
    ASSERT_TRUE(WriteBytes(original, WiiImage(false)));
    ASSERT_TRUE(WriteBytes(variant, WiiImage(true)));
    RefreshLayout();
  }
  void RefreshLayout()
  {
    auto volume = DiscIO::CreateVolume(original, false);
    ASSERT_TRUE(volume);
    std::string error;
    const auto generated = Layout::Generate(*volume, &error);
    ASSERT_TRUE(generated) << error;
    layout = *generated;
    const auto generated_report = Report::Generate(original, &error);
    ASSERT_TRUE(generated_report) << error;
    report = *generated_report;
    picojson::array files;
    for (const auto& file : report.files)
    {
      picojson::value value;
      ASSERT_TRUE(picojson::parse(value, Report::FileLine(file)).empty());
      files.push_back(value);
    }
    ASSERT_TRUE(File::WriteStringToFile(
        reference_path,
        picojson::value(picojson::object{{"name", picojson::value("invented standard")},
                                         {"files", picojson::value(files)},
                                         {"partitionOffset", picojson::value(static_cast<double>(
                                                                 layout.partition_offset))}})
            .serialize()));
    ASSERT_TRUE(File::WriteStringToFile(layout_path, Layout::Serialize(layout)));
  }
  void TearDown() override
  {
    File::SetUserPath(D_WIIROOT_IDX, old_wii_root);
    File::DeleteDirRecursively(directory);
  }
  std::pair<int, std::string> Run(bool against = true)
  {
    std::FILE* out = std::tmpfile();
    EXPECT_NE(out, nullptr);
    if (!out)
      return {-1, {}};
    const int code = Rebuild::Run(
        variant, layout_path, against ? std::optional<std::string>(reference_path) : std::nullopt,
        output, out);
    std::rewind(out);
    std::string text;
    char buffer[1024];
    while (const size_t n = std::fread(buffer, 1, sizeof(buffer), out))
      text.append(buffer, n);
    std::fclose(out);
    return {code, text};
  }
  void NoOutput()
  {
    EXPECT_FALSE(File::Exists(output));
    EXPECT_FALSE(File::Exists(output + ".partial"));
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      EXPECT_EQ(entry.path().filename().string().find(".partial."), std::string::npos);
  }
  std::string old_wii_root;
  std::string directory, original, variant, layout_path, reference_path, output;
  Layout::Layout layout;
  Report::Report report;
};

TEST_F(DiscLayoutTest, ExportsOnlyMetadataAndRegeneratesFST)
{
  auto volume = DiscIO::CreateVolume(original, false);
  ASSERT_TRUE(volume);
  const auto* fst = layout.FindSystemFile("sys/fst.bin");
  ASSERT_NE(fst, nullptr);
  std::vector<u8> expected(static_cast<size_t>(fst->size));
  ASSERT_TRUE(volume->Read(fst->offset, fst->size, expected.data(), volume->GetGamePartition()));
  EXPECT_EQ(Layout::BuildFST(layout), expected);
  const std::string json = Layout::Serialize(layout);
  EXPECT_EQ(json.find("invented certificate"), std::string::npos);
  EXPECT_EQ(json.find("sha1\""), std::string::npos);
  std::string error;
  const auto parsed = Layout::Parse(json, &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(Layout::Serialize(*parsed), json);
  EXPECT_EQ(parsed->partitions.size(), 2);
  ASSERT_FALSE(parsed->gaps.empty());
  for (const auto& gap : parsed->gaps)
    EXPECT_EQ(gap.kind, "zero");
  File::Delete(layout_path);
  std::FILE* out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(Layout::Run(original, layout_path, out), 0);
  std::fclose(out);
  EXPECT_FALSE(File::Exists(layout_path + ".partial"));
}

TEST(DiscLayoutJunkTest, FormulaMatchesIndependentNKitVectors)
{
  // SHA-1 vectors from the supplied MIT gc-junk.ts algorithm, using an invented ID.
  const std::array<std::tuple<u8, u64, const char*>, 3> vectors{{
      {0, 0, "CECDCC2B36FFF5F14AC7580B56C6A149478878A5"},
      {1, 0x8000, "0BD58B6BB1F3C7E95400F72B7EC3B1B168A6D25C"},
      {255, 0x68000, "2748ABCDA5FE0632371B5F6A1569E8016B769F31"},
  }};
  for (const auto& [disc, offset, hash] : vectors)
  {
    const auto seed = Layout::FormulaSeed("RTEE01", disc, offset);
    DiscIO::LaggedFibonacciGenerator lfg;
    lfg.SetSeed(seed.data());
    std::vector<u8> bytes(Layout::JUNK_UNIT_SIZE);
    lfg.GetBytes(bytes.size(), bytes.data());
    EXPECT_EQ(Common::SHA1::DigestToString(Common::SHA1::CalculateDigest(bytes)), hash);
  }
}

class DiscLayoutJunkTest : public DiscLayoutTest, public testing::WithParamInterface<GapData>
{
};

TEST_P(DiscLayoutJunkTest, ClassifiesMetadataAndRebuildsMovedFiles)
{
  const GapData mode = GetParam();
  ASSERT_TRUE(WriteBytes(original, WiiImage(false, false, false, false, false, false, mode, 1)));
  // The moved copy has different, scrubbed gaps. Only its files can contribute to the rebuild.
  ASSERT_TRUE(
      WriteBytes(variant, WiiImage(true, false, false, false, false, false, GapData::Zero, 1)));
  ASSERT_NO_FATAL_FAILURE(RefreshLayout());
  ASSERT_TRUE(File::Delete(layout_path));
  std::FILE* out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  testing::internal::CaptureStderr();
  const int code = Layout::Run(original, layout_path, out);
  const std::string summary = testing::internal::GetCapturedStderr();
  std::fclose(out);
  ASSERT_EQ(code, 0);
  std::string json, error;
  ASSERT_TRUE(File::ReadFileToString(layout_path, json));
  const auto parsed = Layout::Parse(json, &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(Layout::Serialize(*parsed), json);
  EXPECT_EQ(parsed->disc_number, 1);
  const std::string kind = mode == GapData::Formula ? "junk" :
                           mode == GapData::Seeded  ? "seeded" :
                           mode == GapData::Zero    ? "zero" :
                                                      "other";
  u64 bytes = 0;
  size_t seeds = 0;
  for (const auto& gap : parsed->gaps)
  {
    if (mode == GapData::Random)
      EXPECT_TRUE(gap.kind == "other" || gap.kind == "zero");
    else
      EXPECT_EQ(gap.kind, kind);
    if (gap.kind == kind)
      bytes += gap.size;
    seeds += gap.seeds.size();
    EXPECT_EQ(gap.seeds.empty(), mode != GapData::Seeded);
  }
  EXPECT_GT(bytes, Layout::JUNK_UNIT_SIZE);
  EXPECT_TRUE(std::any_of(parsed->gaps.begin(), parsed->gaps.end(), [&](const auto& gap) {
    return gap.kind == kind && gap.size > Layout::JUNK_UNIT_SIZE;
  }));
  EXPECT_NE(summary.find(kind + "=" + std::to_string(bytes)), std::string::npos);
  EXPECT_NE(summary.find(std::to_string(seeds) + " seeds stored"), std::string::npos);
  EXPECT_EQ(std::count(summary.begin(), summary.end(), '\n'), 1);

  // An explicit key allowlist prevents adding payload fields to the metadata schema.
  picojson::value root;
  ASSERT_TRUE(picojson::parse(root, json).empty());
  const std::set<std::string> allowed{
      "version",         "id",  "discNumber", "h3Sha1", "tmdContentSha1", "partitions",
      "partitionOffset", "tmd", "cert",       "h3",     "data",           "fstMaxSize",
      "systemFiles",     "fst", "gaps"};
  for (const auto& [key, value] : root.get<picojson::object>())
    EXPECT_EQ(allowed.count(key), 1) << key;
  const std::set<std::string> metadata_fields{"offset",    "size",       "type",   "path", "name",
                                              "directory", "nameOffset", "parent", "next"};
  for (const char* section : {"partitions", "systemFiles", "fst"})
  {
    for (const auto& entry : root.get(section).get<picojson::array>())
    {
      for (const auto& [key, value] : entry.get<picojson::object>())
        EXPECT_TRUE(metadata_fields.contains(key)) << key;
    }
  }
  for (const char* section : {"tmd", "cert", "h3", "data"})
  {
    const auto& range = root.get(section).get<picojson::object>();
    EXPECT_EQ(range.size(), 2);
    EXPECT_TRUE(range.contains("offset") && range.contains("size"));
  }
  for (const auto& value : root.get("gaps").get<picojson::array>())
  {
    const auto& object = value.get<picojson::object>();
    EXPECT_EQ(object.size(), mode == GapData::Seeded ? 4 : 3);
    EXPECT_TRUE(object.contains("offset") && object.contains("size") && object.contains("kind"));
    if (mode == GapData::Seeded)
    {
      ASSERT_TRUE(object.contains("seeds"));
      for (const auto& seed : value.get("seeds").get<picojson::array>())
      {
        ASSERT_TRUE(seed.is<std::string>());
        const auto& hex = seed.get<std::string>();
        EXPECT_EQ(hex.size(), 17 * 8);
        EXPECT_TRUE(std::all_of(hex.begin(), hex.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }));
      }
    }
  }
  for (const char* payload : {"invented certificate bytes", "Invented-XS", "Invented-CP"})
    EXPECT_EQ(json.find(payload), std::string::npos);

  const auto [result, text] = Run(mode != GapData::Seeded);
  if (mode == GapData::Random)
  {
    EXPECT_EQ(result, 1) << text;
    EXPECT_NE(text.find("padding bytes at offset " + std::to_string(layout.gaps[0].offset)),
              std::string::npos);
    EXPECT_NE(text.find("cannot be reconstructed"), std::string::npos);
    NoOutput();
    return;
  }
  ASSERT_EQ(result, 0) << text;
  const auto rebuilt = Report::Generate(output, &error);
  ASSERT_TRUE(rebuilt) << error;
  EXPECT_TRUE(
      Report::Compare(*rebuilt, {"standard", report.files, layout.partition_offset}).empty());
  EXPECT_EQ(rebuilt->header.h3_sha1, report.header.h3_sha1);
  EXPECT_EQ(rebuilt->header.tmd_content_sha1, report.header.tmd_content_sha1);
  EXPECT_EQ(rebuilt->header.partition_offset, report.header.partition_offset);
  EXPECT_FALSE(File::Exists(output + ".partial"));
}

INSTANTIATE_TEST_SUITE_P(InventedGaps, DiscLayoutJunkTest,
                         testing::Values(GapData::Formula, GapData::Seeded, GapData::Zero,
                                         GapData::Random));

TEST_F(DiscLayoutTest, PaddedSeededGapsIncludeTinyUnalignedFragmentsAndRandomReads)
{
  ASSERT_TRUE(WriteBytes(
      original, WiiImage(false, false, false, false, false, false, GapData::PaddedSeeded)));
  ASSERT_NO_FATAL_FAILURE(RefreshLayout());
  EXPECT_TRUE(std::any_of(layout.gaps.begin(), layout.gaps.end(), [](const auto& gap) {
    return gap.kind == "seeded" && gap.size == 97 && gap.offset % 4 != 0;
  }));
  EXPECT_TRUE(std::any_of(layout.gaps.begin(), layout.gaps.end(),
                          [](const auto& gap) { return gap.kind == "zero"; }));
  std::string error;
  auto blob = Orca::StandardBlob::Create(variant, layout, &error);
  ASSERT_TRUE(blob) << error;
  const auto volume = DiscIO::CreateVolume(original, false);
  ASSERT_TRUE(volume);
  // These reads cross both 0x7c00 physical data boundaries and 0x8000 junk boundaries.
  for (u64 offset : {0x2471, 0x7bff, 0x7fff, 0x12345, 0x1efff, 0x1e7ffe})
  {
    const u64 size = std::min<u64>(0x9876, layout.DecryptedSize() - offset);
    std::vector<u8> actual(size), expected(size);
    ASSERT_TRUE(blob->ReadWiiDecrypted(offset, size, actual.data(),
                                       layout.partition_offset + layout.data.offset));
    ASSERT_TRUE(volume->Read(offset, size, expected.data(), volume->GetGamePartition()));
    EXPECT_EQ(actual, expected) << offset;
  }
  const auto [code, text] = Run();
  EXPECT_EQ(code, 0) << text;
}

TEST_F(DiscLayoutTest, InvalidSeedsAreRefusedAndChangedSeedsFailVerification)
{
  ASSERT_TRUE(
      WriteBytes(original, WiiImage(false, false, false, false, false, false, GapData::Seeded)));
  ASSERT_NO_FATAL_FAILURE(RefreshLayout());
  std::string error;
  auto bad = layout;
  bad.gaps[0].seeds.clear();
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.gaps[0].seeds.push_back(bad.gaps[0].seeds[0]);
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.gaps[0].kind = "junk";
  EXPECT_FALSE(Layout::Validate(bad, &error));
  picojson::value with_seeds;
  ASSERT_TRUE(picojson::parse(with_seeds, Layout::Serialize(layout)).empty());
  with_seeds.get("gaps").get<picojson::array>()[0].get<picojson::object>()["kind"] =
      picojson::value("junk");
  EXPECT_FALSE(Layout::Parse(with_seeds.serialize(), &error));
  for (const std::string& hex : {std::string(135, '0'), std::string(136, 'g')})
  {
    picojson::value root;
    ASSERT_TRUE(picojson::parse(root, Layout::Serialize(layout)).empty());
    root.get("gaps").get<picojson::array>()[0].get("seeds").get<picojson::array>()[0] =
        picojson::value(hex);
    EXPECT_FALSE(Layout::Parse(root.serialize(), &error));
  }
  layout.gaps[0].seeds[0][1] ^= Common::swap32(u32{1});
  ASSERT_TRUE(File::WriteStringToFile(layout_path, Layout::Serialize(layout)));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_EQ(text, "FAIL: rebuilt disc H3 SHA-1 does not match the standard layout\n");
  NoOutput();
}

TEST_F(DiscLayoutTest, PhysicalZeroSectorsRemainDistinctFromDecryptedZeros)
{
  for (const auto mode : {GapData::PhysicalZeroFormula, GapData::PhysicalZeroSeeded})
  {
    ASSERT_TRUE(WriteBytes(original, WiiImage(false, false, false, false, false, false, mode)));
    ASSERT_NO_FATAL_FAILURE(RefreshLayout());
    std::string error;
    const auto parsed = Layout::Parse(Layout::Serialize(layout), &error);
    ASSERT_TRUE(parsed) << error;
    u64 physical_zero_bytes = 0;
    for (const auto& gap : parsed->gaps)
    {
      EXPECT_NE(gap.kind, "other");
      if (gap.physical_zero)
      {
        EXPECT_EQ(gap.kind, "zero");
        EXPECT_TRUE(gap.seeds.empty());
        physical_zero_bytes += gap.size;
      }
    }
    EXPECT_EQ(physical_zero_bytes, (mode == GapData::PhysicalZeroFormula ? 3 : 2) * 0x7c00);
    auto blob = Orca::StandardBlob::Create(variant, *parsed, &error);
    ASSERT_TRUE(blob) << error;
    const auto volume = DiscIO::CreateVolume(original, false);
    ASSERT_TRUE(volume);
    // Every gap, including unaligned reads and the 0x8000 unit which is interrupted
    // by a zero sector at 0x7c00, must match without using the source's gap bytes.
    std::vector<u8> expected(0x8000), actual(0x8000);
    for (const auto& gap : parsed->gaps)
    {
      for (u64 offset = gap.offset; offset < gap.offset + gap.size;)
      {
        const u64 size = std::min<u64>(0x8000, gap.offset + gap.size - offset);
        ASSERT_TRUE(volume->Read(offset, size, expected.data(), volume->GetGamePartition()));
        ASSERT_TRUE(blob->ReadWiiDecrypted(offset, size, actual.data(),
                                           layout.partition_offset + layout.data.offset));
        EXPECT_TRUE(std::equal(expected.begin(), expected.begin() + size, actual.begin()))
            << offset;
        offset += size;
      }
    }
    actual.resize(0x8000 + 14);
    ASSERT_TRUE(blob->Read(layout.partition_offset + layout.data.offset + 0x8000 - 7, 0x8000 + 14,
                           actual.data()));
    EXPECT_TRUE(std::all_of(actual.begin() + 7, actual.end(), [](u8 b) { return b == 0; }));
    // Use a bounded raw read entirely inside an erased sector, including its hash header.
    actual.resize(0x8000);
    ASSERT_TRUE(blob->Read(layout.partition_offset + layout.data.offset + 0x8000, actual.size(),
                           actual.data()));
    EXPECT_TRUE(std::all_of(actual.begin(), actual.end(), [](u8 b) { return b == 0; }));
    auto bad = *parsed;
    auto zero = std::find_if(bad.gaps.begin(), bad.gaps.end(),
                             [](const auto& gap) { return gap.physical_zero; });
    ASSERT_NE(zero, bad.gaps.end());
    zero->kind = "junk";
    EXPECT_FALSE(Layout::Validate(bad, &error));
    picojson::value invalid;
    ASSERT_TRUE(picojson::parse(invalid, Layout::Serialize(*parsed)).empty());
    EXPECT_EQ(invalid.get("version").get<double>(), 3);
    invalid.get<picojson::object>()["version"] = picojson::value(1.0);
    EXPECT_FALSE(Layout::Parse(invalid.serialize(), &error));
    invalid.get<picojson::object>()["version"] = picojson::value(3.0);
    invalid.get("gaps").get<picojson::array>()[0].get<picojson::object>()["physicalZero"] =
        picojson::value("true");
    EXPECT_FALSE(Layout::Parse(invalid.serialize(), &error));
  }
}

TEST_F(DiscLayoutTest, ErasedSubtreesRebuildExactlyFromIdentityAndMovedFiles)
{
  constexpr size_t blocks = 3 * Wii::BLOCKS_PER_GROUP + 3;
  ASSERT_TRUE(WriteBytes(original, WiiImage(false, false, false, false, false, false,
                                            GapData::PhysicalZeroTree, 0, blocks)));
  ASSERT_TRUE(WriteBytes(
      variant, WiiImage(true, false, false, false, false, false, GapData::Zero, 0, blocks)));
  ASSERT_NO_FATAL_FAILURE(RefreshLayout());
  std::string error;
  const auto parsed = Layout::Parse(Layout::Serialize(layout), &error);
  ASSERT_TRUE(parsed) << error;
  ASSERT_EQ(parsed->erased_hashes.size(), 5);
  EXPECT_EQ(std::count_if(parsed->erased_hashes.begin(), parsed->erased_hashes.end(),
                          [](const auto& hash) { return hash.level == 1; }),
            2);
  EXPECT_EQ(std::count_if(parsed->erased_hashes.begin(), parsed->erased_hashes.end(),
                          [](const auto& hash) { return hash.level == 2; }),
            1);
  EXPECT_EQ(std::count_if(parsed->erased_hashes.begin(), parsed->erased_hashes.end(),
                          [](const auto& hash) { return hash.level == 3; }),
            2);
  const auto source = DiscIO::CreateVolume(original, false);
  ASSERT_TRUE(source);
  std::vector<u8> expected(Wii::BLOCK_TOTAL_SIZE), actual(Wii::BLOCK_TOTAL_SIZE);
  for (const bool identity : {true, false})
  {
    std::FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    const int code =
        Rebuild::Run(identity ? original : variant, layout_path, reference_path, output, out);
    std::fclose(out);
    ASSERT_EQ(code, 0);
    auto rebuilt = DiscIO::CreateVolume(output, false);
    ASSERT_TRUE(rebuilt);
    // A moved source supplies its own signatures. Identity checks the whole header too.
    const u64 first = identity ? 0 : layout.h3.offset;
    for (u64 at = first; at < layout.data.offset + layout.data.size;)
    {
      const u64 size = std::min<u64>(expected.size(), layout.data.offset + layout.data.size - at);
      ASSERT_TRUE(source->Read(layout.partition_offset + at, size, expected.data(),
                               DiscIO::PARTITION_NONE));
      ASSERT_TRUE(
          rebuilt->Read(layout.partition_offset + at, size, actual.data(), DiscIO::PARTITION_NONE));
      EXPECT_TRUE(std::equal(expected.begin(), expected.begin() + size, actual.begin())) << at;
      at += size;
    }
    ASSERT_TRUE(File::Delete(output));
  }
  // Legacy layouts can recover retained metadata from an input with the same H3.
  auto legacy = *parsed;
  legacy.erased_hashes.clear();
  auto blob = Orca::StandardBlob::Create(original, legacy, &error);
  ASSERT_TRUE(blob) << error;
  ASSERT_TRUE(blob->ComputeH3(&error)) << error;
  auto view = DiscIO::CreateDisc(blob->CopyReader(), false);
  ASSERT_TRUE(view);
  const auto rebuilt_report = Report::Generate(*view, &error);
  ASSERT_TRUE(rebuilt_report) << error;
  EXPECT_EQ(rebuilt_report->header.h3_sha1, layout.h3_sha1);
  EXPECT_FALSE(Orca::StandardBlob::Create(variant, legacy, &error));
  EXPECT_NE(error.find("layout lacks erased hash metadata"), std::string::npos);
}

TEST_F(DiscLayoutTest, ErasedHashMetadataIsValidatedAndCannotHideChangedData)
{
  ASSERT_TRUE(WriteBytes(
      original, WiiImage(false, false, false, false, false, false, GapData::PhysicalZeroFormula)));
  ASSERT_NO_FATAL_FAILURE(RefreshLayout());
  ASSERT_FALSE(layout.erased_hashes.empty());
  std::string error;
  auto bad = layout;
  bad.erased_hashes.push_back(bad.erased_hashes.front());
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.erased_hashes.pop_back();
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  for (u64 level : {0, 2, 3, 4})
  {
    bad = layout;
    bad.erased_hashes.front().level = level;
    EXPECT_FALSE(Layout::Validate(bad, &error));
  }
  bad = layout;
  bad.erased_hashes.front().index = 0;
  EXPECT_FALSE(Layout::Validate(bad, &error));
  picojson::value root;
  ASSERT_TRUE(picojson::parse(root, Layout::Serialize(layout)).empty());
  root.get<picojson::object>()["version"] = picojson::value(2.0);
  EXPECT_FALSE(Layout::Parse(root.serialize(), &error));
  root.get<picojson::object>()["version"] = picojson::value(3.0);
  root.get("erasedHashes").get<picojson::array>()[0].get<picojson::object>()["digest"] =
      picojson::value(std::string(40, 'g'));
  EXPECT_FALSE(Layout::Parse(root.serialize(), &error));
  layout.erased_hashes.front().digest[0] ^= 1;
  ASSERT_TRUE(File::WriteStringToFile(layout_path, Layout::Serialize(layout)));
  auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_EQ(text, "FAIL: rebuilt disc H3 SHA-1 does not match the standard layout\n");
  NoOutput();
  ASSERT_NO_FATAL_FAILURE(RefreshLayout());
  ASSERT_TRUE(WriteBytes(variant, WiiImage(true, true)));
  std::tie(code, text) = Run(false);
  EXPECT_EQ(code, 2);
  EXPECT_EQ(text, "DIFF: H3 SHA-1 differs from standard layout\n");
  NoOutput();
}

TEST_F(DiscLayoutTest, TMDContentHashMismatchNamesTheFailedCheck)
{
  layout.tmd_content_sha1 = std::string(40, '1');
  ASSERT_TRUE(File::WriteStringToFile(layout_path, Layout::Serialize(layout)));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_EQ(text, "FAIL: rebuilt disc TMD content hash does not match the standard layout\n");
  NoOutput();
}

TEST_F(DiscLayoutTest, PartialJunkUnitAtPartitionEndRebuildsExactly)
{
  ASSERT_TRUE(WriteBytes(
      original, WiiImage(false, false, false, false, false, false, GapData::Seeded, 0, 3)));
  ASSERT_TRUE(
      WriteBytes(variant, WiiImage(true, false, false, false, false, false, GapData::Zero, 0, 3)));
  ASSERT_NO_FATAL_FAILURE(RefreshLayout());
  EXPECT_EQ(layout.DecryptedSize(), 3 * Wii::BLOCK_DATA_SIZE);
  EXPECT_NE(layout.DecryptedSize() % Layout::JUNK_UNIT_SIZE, 0);
  const auto [code, text] = Run();
  ASSERT_EQ(code, 0) << text;
  std::string error;
  const auto rebuilt = Report::Generate(output, &error);
  ASSERT_TRUE(rebuilt) << error;
  EXPECT_EQ(rebuilt->header.h3_sha1, report.header.h3_sha1);
  EXPECT_EQ(rebuilt->header.tmd_content_sha1, report.header.tmd_content_sha1);
  EXPECT_TRUE(
      Report::Compare(*rebuilt, {"partial", report.files, layout.partition_offset}).empty());
}

TEST_F(DiscLayoutTest, StringTableOrderCanDifferFromEntryOrder)
{
  ASSERT_TRUE(WriteBytes(original, WiiImage(false, false, false, false, true)));
  const auto volume = DiscIO::CreateVolume(original, false);
  ASSERT_TRUE(volume);
  std::string error;
  const auto odd = Layout::Generate(*volume, &error);
  ASSERT_TRUE(odd) << error;
  EXPECT_GT(odd->entries[1].name_offset, odd->entries[3].name_offset);
  const auto* fst = odd->FindSystemFile("sys/fst.bin");
  std::vector<u8> expected(static_cast<size_t>(fst->size));
  ASSERT_TRUE(volume->Read(fst->offset, fst->size, expected.data(), volume->GetGamePartition()));
  EXPECT_EQ(Layout::BuildFST(*odd), expected);
}

TEST_F(DiscLayoutTest, RebuildRestoresHashesFilesOffsetAndPreservesSignatures)
{
  const auto [code, text] = Run();
  ASSERT_EQ(code, 0) << text;
  EXPECT_EQ(text, "PASS: rebuilt RTEE01 to the standard layout\n");
  std::string error;
  const auto rebuilt = Report::Generate(output, &error);
  ASSERT_TRUE(rebuilt) << error;
  EXPECT_TRUE(
      Report::Compare(*rebuilt, {"original", report.files, layout.partition_offset}).empty());
  EXPECT_EQ(rebuilt->header.h3_sha1, report.header.h3_sha1);
  EXPECT_EQ(rebuilt->header.tmd_content_sha1, report.header.h3_sha1);
  const auto volume = DiscIO::CreateVolume(output, false);
  const auto source = DiscIO::CreateVolume(variant, false);
  ASSERT_TRUE(volume);
  ASSERT_TRUE(source);
  EXPECT_EQ(volume->GetPartitions(), std::vector<DiscIO::Partition>{volume->GetGamePartition()});
  EXPECT_EQ(volume->GetTMD(volume->GetGamePartition()).GetSignatureData(),
            source->GetTMD(source->GetGamePartition()).GetSignatureData());
  EXPECT_EQ(volume->GetTicket(volume->GetGamePartition()).GetBytes(),
            source->GetTicket(source->GetGamePartition()).GetBytes());
  EXPECT_EQ(volume->GetCertificateChain(volume->GetGamePartition()),
            source->GetCertificateChain(source->GetGamePartition()));
  std::vector<u8> omitted(0x1000, 1);
  ASSERT_TRUE(volume->Read(0x48000, omitted.size(), omitted.data(), DiscIO::PARTITION_NONE));
  EXPECT_TRUE(std::all_of(omitted.begin(), omitted.end(), [](u8 c) { return c == 0; }));
  EXPECT_FALSE(File::Exists(output + ".partial"));
}

TEST_F(DiscLayoutTest, RebuildWithoutReferenceChecksH3)
{
  const auto [code, text] = Run(false);
  EXPECT_EQ(code, 0) << text;
}

TEST_F(DiscLayoutTest, RebuildChangedFileIsDiffAndWritesNothing)
{
  ASSERT_TRUE(WriteBytes(variant, WiiImage(true, true)));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 2);
  EXPECT_TRUE(text.starts_with("DIFF:"));
  EXPECT_NE(text.find("folder/nested.bin"), std::string::npos);
  NoOutput();
}

TEST_F(DiscLayoutTest, RebuildWrongGameIsFail)
{
  ASSERT_TRUE(WriteBytes(variant, WiiImage(true, false, false, true)));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_TRUE(text.starts_with("FAIL:"));
  NoOutput();
}

TEST_F(DiscLayoutTest, RebuildOtherGapIsRefused)
{
  layout.gaps[0].kind = "other";
  ASSERT_TRUE(File::WriteStringToFile(layout_path, Layout::Serialize(layout)));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_NE(text.find("padding bytes at offset " + std::to_string(layout.gaps[0].offset)),
            std::string::npos);
  EXPECT_NE(text.find("cannot be reconstructed"), std::string::npos);
  NoOutput();
}

TEST_F(DiscLayoutTest, IncorrectFormulaJunkFailsHashVerification)
{
  layout.gaps[0].kind = "junk";
  ASSERT_TRUE(File::WriteStringToFile(layout_path, Layout::Serialize(layout)));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_EQ(text, "FAIL: rebuilt disc H3 SHA-1 does not match the standard layout\n");
  NoOutput();
}

TEST_F(DiscLayoutTest, RebuildTruncatedInputIsFail)
{
  auto bytes = WiiImage(true);
  bytes.resize(0xa0100);
  ASSERT_TRUE(WriteBytes(variant, bytes));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_TRUE(text.starts_with("FAIL:"));
  NoOutput();
}

TEST_F(DiscLayoutTest, RebuildOverlappingFilesIsRefused)
{
  layout.entries[3].offset = layout.entries[1].offset;
  ASSERT_TRUE(File::WriteStringToFile(layout_path, Layout::Serialize(layout)));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_NE(text.find("overlapping"), std::string::npos);
  NoOutput();
}

TEST_F(DiscLayoutTest, RebuildVerificationMismatchDeletesPartial)
{
  // An incorrect reference FST is excluded during preflight but checked on the reopened RVZ.
  std::string json;
  ASSERT_TRUE(File::ReadFileToString(reference_path, json));
  picojson::value root;
  ASSERT_TRUE(picojson::parse(root, json).empty());
  for (auto& entry : root.get("files").get<picojson::array>())
  {
    if (entry.get("path").get<std::string>() == "sys/fst.bin")
      entry.get<picojson::object>()["sha1"] = picojson::value(std::string(40, '1'));
  }
  ASSERT_TRUE(File::WriteStringToFile(reference_path, root.serialize()));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_EQ(text, "FAIL: rebuilt disc file hash/size/set mismatch: sys/fst.bin\n");
  NoOutput();
}

TEST_F(DiscLayoutTest, NonzeroGapsAreConservativelyOther)
{
  ASSERT_TRUE(WriteBytes(original, WiiImage(false, false, true)));
  const auto volume = DiscIO::CreateVolume(original, false);
  ASSERT_TRUE(volume);
  std::string error;
  const auto other = Layout::Generate(*volume, &error);
  ASSERT_TRUE(other) << error;
  EXPECT_TRUE(std::any_of(other->gaps.begin(), other->gaps.end(),
                          [](auto& gap) { return gap.kind == "other"; }));
}

TEST_F(DiscLayoutTest, UnknownFSTBytesCannotBeExported)
{
  ASSERT_TRUE(WriteBytes(original, WiiImage(false, false, false, false, false, true)));
  const auto volume = DiscIO::CreateVolume(original, false);
  ASSERT_TRUE(volume);
  std::string error;
  EXPECT_FALSE(Layout::Generate(*volume, &error));
  EXPECT_EQ(error, "FST has bytes not representable as names and structure");
}

TEST_F(DiscLayoutTest, RebuildSigtermDuringConversionDeletesPartial)
{
  std::FILE* out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  bool signalled = false;
  const int code = Rebuild::Run(variant, layout_path, reference_path, output, out, [&](u64, u64) {
    if (!signalled)
    {
      signalled = true;
      std::raise(SIGTERM);
    }
  });
  std::fclose(out);
  EXPECT_TRUE(signalled);
  EXPECT_EQ(code, 1);
  NoOutput();
}

TEST_F(DiscLayoutTest, StandaloneCommandDispatchAndStrictOptions)
{
  std::vector<std::string> args{"orca-disc-tool", "--disc-layout", original,
                                "--out=" + directory + "/cli-layout.json"};
  const auto run = [&](std::vector<std::string> values) {
    std::vector<char*> argv;
    for (auto& value : values)
      argv.push_back(value.data());
    testing::internal::CaptureStdout();
    const auto code = Rebuild::RunCommandLine(static_cast<int>(argv.size()), argv.data());
    const auto text = testing::internal::GetCapturedStdout();
    return std::make_pair(code, text);
  };
  auto [code, text] = run(args);
  ASSERT_EQ(code, 0) << text;
  EXPECT_EQ(text, "PASS: wrote RTEE01 layout\n");
  std::tie(code, text) = run({"orca-disc-tool", "--disc-report=" + original,
                             "--against", reference_path});
  ASSERT_EQ(code, 0) << text;
  EXPECT_NE(text.find("PASS: files match"), std::string::npos);
  std::tie(code, text) = run({"orca-disc-tool", "--rebuild-standard=" + variant, "--layout",
                              layout_path, "--out", output, "--against", reference_path});
  ASSERT_EQ(code, 0) << text;
  EXPECT_EQ(text, "PASS: rebuilt RTEE01 to the standard layout\n");
  for (const auto& bad : std::vector<std::vector<std::string>>{
           {"orca-disc-tool", "--disc-layout", original},
           {"orca-disc-tool", "--disc-layout", original, "--out", output, "--boot"},
           {"orca-disc-tool", "--rebuild-standard", variant, "--out", output},
           {"orca-disc-tool", "--disc-layout", original, "--out", output, "--out", output},
           {"orca-disc-tool", "--disc-report", original, "--out", output},
           {"orca-disc-tool", "--disc-report", original, "--disc-layout", original},
           {"orca-disc-tool", "--out="}})
  {
    std::tie(code, text) = run(bad);
    EXPECT_EQ(code, 1);
    EXPECT_TRUE(text.starts_with("FAIL:"));
  }
  std::tie(code, text) = run({"orca-disc-tool", "--batch"});
  EXPECT_FALSE(code);
  EXPECT_TRUE(text.empty());
}

TEST_F(DiscLayoutTest, InvalidLayoutRangesAndTreesAreRefused)
{
  std::string error;
  auto bad = layout;
  bad.partition_offset = 0x400000000ULL;
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.entries[2].parent = 2;
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.entries[3].name = "../bad";
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.gaps.erase(bad.gaps.begin());
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.h3.offset = bad.tmd.offset;
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  bad = layout;
  bad.entries[3].name_offset = 0xffffff;
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(bad), &error));
  EXPECT_FALSE(Layout::Parse(Layout::Serialize(layout) + " {}", &error));
}

TEST_F(DiscLayoutTest, RebuildExistingOutputIsUntouched)
{
  ASSERT_TRUE(File::WriteStringToFile(output, "existing"));
  const auto [code, text] = Run();
  EXPECT_EQ(code, 1);
  EXPECT_EQ(text, "FAIL: output already exists\n");
  std::string bytes;
  ASSERT_TRUE(File::ReadFileToString(output, bytes));
  EXPECT_EQ(bytes, "existing");
}
TEST_F(DiscLayoutTest, HostilePartitionTablesAreRejectedBeforeTraversal)
{
  const auto baseline = WiiImage(true);
  const std::vector<std::pair<u32, u32>> tables = {
      {0xffffffff, 0x40020 / 4},
      {0x20000001, 0x40020 / 4},
      {257, 0x40020 / 4},
      {2, 0xffffffff},
      {2, static_cast<u32>((baseline.size() - 4) / 4)}};
  for (const auto& [count, table] : tables)
  {
    auto bytes = baseline;
    Write32(&bytes, 0x40000, count);
    Write32(&bytes, 0x40004, table);
    ASSERT_TRUE(WriteBytes(variant, bytes));
#ifndef _WIN32
    // A regression must fail in bounded time, rather than stall the test runner.
    EXPECT_EXIT(
        {
          alarm(3);
          const auto disc = DiscIO::CreateDisc(variant, false);
          std::_Exit(disc ? 1 : 0);
        },
        testing::ExitedWithCode(0), "");
#else
    EXPECT_FALSE(DiscIO::CreateDisc(variant, false));
#endif
    Wii direct(std::make_unique<FixtureBlob>(bytes), false);
    EXPECT_EQ(direct.GetGamePartition(), DiscIO::PARTITION_NONE);
    const auto [code, text] = Run();
    EXPECT_EQ(code, 1) << text;
    NoOutput();
  }
}

TEST_F(DiscLayoutTest, HugeSourceOffsetsAndLengthsAreRejected)
{
  const auto baseline = WiiImage(true);
  for (const u64 field : {u64{0x40020}, u64{0x80000 + 0x2a8}, u64{0x80000 + 0x2b0},
                          u64{0x80000 + 0x2b4}, u64{0x80000 + 0x2b8}, u64{0x80000 + 0x2bc}})
  {
    auto bytes = baseline;
    Write32(&bytes, field, 0xffffffff);
    ASSERT_TRUE(WriteBytes(variant, bytes));
    const auto [code, text] = Run();
    EXPECT_EQ(code, 1) << text;
    NoOutput();
  }
  const auto disc = DiscIO::CreateDisc(original, false);
  ASSERT_TRUE(disc);
  std::array<u8, 32> bytes{};
  const auto partition = disc->GetGamePartition();
  EXPECT_FALSE(disc->Read(std::numeric_limits<u64>::max(), 1, bytes.data(), partition));
  EXPECT_FALSE(disc->Read(16, std::numeric_limits<u64>::max(), bytes.data(), partition));
  EXPECT_FALSE(disc->Read(0xffffffffULL * Wii::BLOCK_DATA_SIZE, 1, bytes.data(), partition));
}

TEST_F(DiscLayoutTest, SourceMetadataIsCheckedBeforeRecursiveFilesystemReads)
{
  for (const auto metadata : {SourceMetadata::DeepFST, SourceMetadata::BadName,
                              SourceMetadata::HugeFile, SourceMetadata::HugeFST})
  {
    ASSERT_TRUE(WriteBytes(variant, WiiImage(true, false, false, false, false, false, GapData::Zero,
                                             0, Wii::BLOCKS_PER_GROUP, metadata)));
    const auto [code, text] = Run();
    EXPECT_EQ(code, 1) << text;
    EXPECT_NE(text.find("source FST"), std::string::npos) << text;
    NoOutput();
    std::FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(Layout::Run(variant, output, out), 1);
    std::fclose(out);
    NoOutput();
  }
}

TEST_F(DiscLayoutTest, WholeDiscVerificationRejectsWrittenRVZCorruption)
{
  std::string error;
  auto expected = Orca::StandardBlob::Create(variant, layout, &error);
  ASSERT_TRUE(expected) << error;
  ASSERT_TRUE(expected->ComputeH3(&error)) << error;
  std::vector<u8> intended(expected->GetDataSize());
  ASSERT_TRUE(expected->Read(0, intended.size(), intended.data()));
  // These are actual written RVZs with altered logical bytes, not changes to the reference.
  for (const u64 offset : {u64{0x20}, u64{0x40024}, u64{0x4e000}, u64{0x49000},
                           layout.partition_offset + layout.h3.offset})
  {
    auto corrupt = intended;
    corrupt[offset] ^= 1;
    FixtureBlob reader(std::move(corrupt));
    const std::string path = directory + "/corrupt.rvz";
    auto volume = DiscIO::CreateDisc(reader.CopyReader(), false);
    ASSERT_TRUE(volume);
    ASSERT_TRUE(DiscIO::ConvertToWIAOrRVZ(
        &reader, variant, path, true, DiscIO::WIARVZCompressionType::Zstd, 5, 128 * 1024,
        [](const std::string&, float) { return true; }, volume.get()));
    auto actual = DiscIO::CreateDisc(path, false);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->GetBlobType(), DiscIO::BlobType::RVZ);
    EXPECT_FALSE(Rebuild::VerifyRaw(*expected, *actual, &error));
    EXPECT_EQ(error, "rebuilt disc byte mismatch at offset " + std::to_string(offset));
    actual.reset();
    ASSERT_TRUE(File::Delete(path));
  }
  for (const int delta :
       {-static_cast<int>(Wii::BLOCK_TOTAL_SIZE), static_cast<int>(Wii::BLOCK_TOTAL_SIZE)})
  {
    auto corrupt = intended;
    corrupt.resize(corrupt.size() + delta);
    Write32(&corrupt, layout.partition_offset + 0x2bc, (layout.data.size + delta) / 4);
    FixtureBlob reader(std::move(corrupt));
    const std::string path = directory + "/length.rvz";
    auto volume = DiscIO::CreateDisc(reader.CopyReader(), false);
    ASSERT_TRUE(volume);
    ASSERT_TRUE(DiscIO::ConvertToWIAOrRVZ(
        &reader, variant, path, true, DiscIO::WIARVZCompressionType::Zstd, 5, 128 * 1024,
        [](const std::string&, float) { return true; }, volume.get()));
    const auto actual = DiscIO::CreateDisc(path, false);
    ASSERT_TRUE(actual);
    EXPECT_FALSE(Rebuild::VerifyRaw(*expected, *actual, &error));
    EXPECT_EQ(error, "rebuilt disc logical size mismatch");
  }
}

TEST_F(DiscLayoutTest, TruncatedContainerHeadersAreRefused)
{
  for (const std::array<u8, 4> magic :
       {std::array<u8, 4>{'R', 'V', 'Z', 1}, std::array<u8, 4>{'W', 'I', 'A', 1}})
  {
    for (const size_t length : {size_t{4}, size_t{16}, size_t{64}})
    {
      std::vector<u8> bytes(length);
      std::copy(magic.begin(), magic.end(), bytes.begin());
      ASSERT_TRUE(WriteBytes(variant, bytes));
      const auto [code, text] = Run();
      EXPECT_EQ(code, 1) << text;
      NoOutput();
    }
  }
}

TEST_F(DiscLayoutTest, DestinationCreatedDuringConversionIsPreserved)
{
  std::FILE* out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  bool created = false;
  const int code = Rebuild::Run(variant, layout_path, reference_path, output, out, [&](u64, u64) {
    if (!created)
    {
      created = true;
      EXPECT_TRUE(File::WriteStringToFile(output, "concurrent output"));
    }
  });
  std::fclose(out);
  EXPECT_TRUE(created);
  EXPECT_EQ(code, 1);
  std::string bytes;
  ASSERT_TRUE(File::ReadFileToString(output, bytes));
  EXPECT_EQ(bytes, "concurrent output");
  ASSERT_TRUE(File::Delete(output));
  NoOutput();
}

TEST_F(DiscLayoutTest, OutputReservationsAreUniqueAndPublicationIsExclusive)
{
  auto first = Orca::DiscTool::OutputFile::Create(output);
  auto second = Orca::DiscTool::OutputFile::Create(output);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->Path(), second->Path());
  ASSERT_TRUE(File::WriteStringToFile(first->Path(), "first"));
  ASSERT_TRUE(File::WriteStringToFile(second->Path(), "second"));
  auto publisher = std::async(std::launch::async, [&] { return first->Publish(output); });
  const bool published_second = second->Publish(output);
  const bool published_first = publisher.get();
  EXPECT_NE(published_first, published_second);
  first.reset();
  second.reset();
  std::string bytes;
  ASSERT_TRUE(File::ReadFileToString(output, bytes));
  EXPECT_EQ(bytes, published_first ? "first" : "second");
  EXPECT_EQ(Layout::Run(original, output, stdout), 1);
  ASSERT_TRUE(File::Delete(output));
  NoOutput();
}

#ifndef _WIN32
TEST_F(DiscLayoutTest, SimultaneousRebuildsDoNotReplaceOrDeleteEachOthersFiles)
{
  int ready[2], go[2];
  ASSERT_EQ(pipe(ready), 0);
  ASSERT_EQ(pipe(go), 0);
  std::array<pid_t, 2> children{};
  for (auto& pid : children)
  {
    pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0)
    {
      alarm(20);
      close(ready[0]);
      close(go[1]);
      std::FILE* out = std::tmpfile();
      if (!out)
        std::_Exit(3);
      bool waiting = false;
      const int code =
          Rebuild::Run(variant, layout_path, reference_path, output, out, [&](u64, u64) {
            if (!waiting)
            {
              waiting = true;
              char token = 'x';
              if (write(ready[1], &token, 1) != 1 || read(go[0], &token, 1) != 1)
                std::_Exit(3);
            }
          });
      std::fclose(out);
      std::_Exit(code);
    }
  }
  close(ready[1]);
  close(go[0]);
  std::array<char, 2> tokens{};
  size_t received = 0;
  while (received < tokens.size())
  {
    const auto n = read(ready[0], tokens.data() + received, tokens.size() - received);
    if (n <= 0)
      break;
    received += n;
  }
  EXPECT_EQ(received, 2);
  EXPECT_EQ(write(go[1], "xx", 2), 2);
  close(ready[0]);
  close(go[1]);
  std::array<int, 2> codes{};
  for (size_t i = 0; i < children.size(); ++i)
  {
    int status;
    ASSERT_EQ(waitpid(children[i], &status, 0), children[i]);
    ASSERT_TRUE(WIFEXITED(status));
    codes[i] = WEXITSTATUS(status);
  }
  std::sort(codes.begin(), codes.end());
  EXPECT_EQ(codes, (std::array<int, 2>{0, 1}));
  std::string error;
  const auto verified = Report::Generate(output, &error);
  ASSERT_TRUE(verified) << error;
  EXPECT_TRUE(
      Report::Compare(*verified, {"original", report.files, layout.partition_offset}).empty());
  ASSERT_TRUE(File::Delete(output));
  NoOutput();
}
#endif

TEST_F(DiscLayoutTest, DefaultAndCachedFactoriesPreserveSourceAndRebuiltReads)
{
  std::string error;
  const auto [code, text] = Run();
  ASSERT_EQ(code, 0) << text;
  Config::Init();
  struct ConfigScope
  {
    ~ConfigScope() { Config::Shutdown(); }
  } config_scope;
  for (const auto& path : {original, output})
  {
    const auto baseline = DiscIO::CreateDisc(path, false);
    ASSERT_TRUE(baseline);
    for (const bool cached : {false, true})
    {
      Config::SetCurrent(Config::MAIN_LOAD_GAME_INTO_MEMORY, cached);
      auto core = DiscIO::CreateDiscForCore(path);
      auto ordinary = DiscIO::CreateDisc(path);
      auto volume = DiscIO::CreateVolume(path);
      ASSERT_TRUE(core);
      EXPECT_EQ(core->GetBlobReader().IsCached(), cached);
      ASSERT_TRUE(ordinary);
      ASSERT_TRUE(volume);
      const auto partition = baseline->GetGamePartition();
      for (const auto& candidate : {core.get(), ordinary.get()})
      {
        EXPECT_FALSE(Orca::AliasRevision(*candidate));
        EXPECT_TRUE(Orca::SessionDiscReady(*candidate));
      }
      for (const DiscIO::Volume* candidate :
           {static_cast<DiscIO::Volume*>(core.get()), static_cast<DiscIO::Volume*>(ordinary.get()),
            volume.get()})
      {
        const auto expected = Report::Generate(*baseline, &error);
        const auto actual = Report::Generate(*candidate, &error);
        ASSERT_TRUE(expected) << error;
        ASSERT_TRUE(actual) << error;
        EXPECT_TRUE(
            Report::Compare(*actual, {"baseline", expected->files, partition.offset}).empty());
        std::array<u8, 0x100> a{}, b{};
        ASSERT_TRUE(baseline->Read(0, a.size(), a.data(), DiscIO::PARTITION_NONE));
        ASSERT_TRUE(candidate->Read(0, b.size(), b.data(), DiscIO::PARTITION_NONE));
        EXPECT_EQ(a, b);
      }
    }
  }
}

TEST_F(DiscLayoutTest, DefaultAndCachedFactoriesKeepRevisionAliasChecks)
{
  ASSERT_TRUE(
      WriteBytes(variant, WiiImage(true, false, false, false, false, false, GapData::Zero, 0,
                                   Wii::BLOCKS_PER_GROUP, SourceMetadata::Normal, true)));
  Config::Init();
  struct ConfigScope
  {
    ~ConfigScope() { Config::Shutdown(); }
  } config_scope;
  for (const bool cached : {false, true})
  {
    Config::SetCurrent(Config::MAIN_LOAD_GAME_INTO_MEMORY, cached);
    auto disc = DiscIO::CreateDiscForCore(variant);
    ASSERT_TRUE(disc);
    EXPECT_EQ(disc->GetBlobReader().IsCached(), cached);
    EXPECT_FALSE(Orca::SessionDiscReady(*disc));
    ASSERT_TRUE(Orca::AliasRevision(*disc));
    EXPECT_TRUE(Orca::SessionDiscReady(*disc));
    EXPECT_EQ(disc->GetRevision(), 2);
    EXPECT_EQ(disc->GetRevision(disc->GetGamePartition()), 2);
    EXPECT_EQ(disc->ReadSwapped<u8>(0x3dd00 + 0x11a1f, disc->GetGamePartition()), 2);
    EXPECT_FALSE(Orca::AliasRevision(*disc));
  }
  auto bytes = WiiImage(true, false, false, false, false, false, GapData::Zero, 0,
                        Wii::BLOCKS_PER_GROUP, SourceMetadata::Normal, true);
  bytes[0x80000 + 0x2c0 + sizeof(IOS::ES::TMDHeader) + offsetof(IOS::ES::Content, sha1)] ^= 1;
  ASSERT_TRUE(WriteBytes(variant, bytes));
  const auto refused = DiscIO::CreateDisc(variant);
  ASSERT_TRUE(refused);
  EXPECT_FALSE(Orca::AliasRevision(*refused));
  EXPECT_EQ(refused->GetRevision(refused->GetGamePartition()), 1);
}

TEST_F(DiscLayoutTest, DirectoryFactoryKeepsDefaultReads)
{
  const auto source = DiscIO::CreateDisc(original, false);
  ASSERT_TRUE(source);
  const auto partition = source->GetGamePartition();
  const std::string extracted = directory + "/extracted";
  ASSERT_TRUE(DiscIO::ExportSystemData(*source, partition, extracted));
  File::CreateFullPath(extracted + "/files/folder/");
  ASSERT_TRUE(DiscIO::ExportFile(*source, partition, "hello.txt", extracted + "/files/hello.txt"));
  ASSERT_TRUE(DiscIO::ExportFile(*source, partition, "folder/nested.bin",
                                 extracted + "/files/folder/nested.bin"));
  ASSERT_TRUE(File::WriteStringToFile(extracted + "/files/folder/empty", ""));
  const auto baseline = DiscIO::CreateDisc(extracted + "/sys/main.dol", false);
  const auto ordinary = DiscIO::CreateDisc(extracted + "/sys/main.dol");
  ASSERT_TRUE(baseline);
  ASSERT_TRUE(ordinary);
  EXPECT_EQ(baseline->GetBlobType(), DiscIO::BlobType::DIRECTORY);
  for (const auto& path : {"hello.txt", "folder/nested.bin"})
  {
    std::array<u8, 5> expected{}, actual{};
    const auto a = DiscIO::ReadFile(*baseline, baseline->GetGamePartition(), path, expected.data(),
                                    expected.size());
    const auto b = DiscIO::ReadFile(*ordinary, ordinary->GetGamePartition(), path, actual.data(),
                                    actual.size());
    EXPECT_EQ(a, b);
    EXPECT_GT(a, 0);
    EXPECT_EQ(expected, actual);
  }
  std::array<u8, 0x8000> a{}, b{};
  const u64 raw = baseline->GetGamePartition().offset + 0x20000;
  ASSERT_TRUE(baseline->Read(raw, a.size(), a.data(), DiscIO::PARTITION_NONE));
  ASSERT_TRUE(ordinary->Read(raw, b.size(), b.data(), DiscIO::PARTITION_NONE));
  EXPECT_EQ(a, b);
}

TEST_F(DiscLayoutTest, WADFactoriesKeepContentIntegrity)
{
  const auto source = DiscIO::CreateDisc(original, false);
  ASSERT_TRUE(source);
  const auto& ticket = source->GetTicket(source->GetGamePartition());
  std::vector<u8> plain(0x40, 0x5a), encrypted(plain.size());
  const auto key = source->GetTitleKey(source->GetGamePartition());
  const auto aes = Common::AES::CreateContextEncrypt(key.data());
  const std::array<u8, 16> iv{};
  ASSERT_TRUE(aes->Crypt(iv.data(), plain.data(), encrypted.data(), plain.size()));
  auto tmd = source->GetTMD(source->GetGamePartition()).GetBytes();
  Write32(&tmd, sizeof(IOS::ES::TMDHeader) + offsetof(IOS::ES::Content, size) + 4, plain.size());
  const auto hash = Common::SHA1::CalculateDigest(plain);
  std::copy(hash.begin(), hash.end(),
            tmd.begin() + sizeof(IOS::ES::TMDHeader) + offsetof(IOS::ES::Content, sha1));
  std::vector<u8> wad(0x40 + 0x2c0 + 0x240 + encrypted.size());
  Write32(&wad, 0, 0x20);
  wad[4] = 'I';
  wad[5] = 's';
  Write32(&wad, 0x10, ticket.GetBytes().size());
  Write32(&wad, 0x14, tmd.size());
  Write32(&wad, 0x18, encrypted.size());
  std::copy(ticket.GetBytes().begin(), ticket.GetBytes().end(), wad.begin() + 0x40);
  std::copy(tmd.begin(), tmd.end(), wad.begin() + 0x300);
  std::copy(encrypted.begin(), encrypted.end(), wad.begin() + 0x540);
  const std::string path = directory + "/fixture.wad";
  ASSERT_TRUE(WriteBytes(path, wad));
  const auto direct = DiscIO::CreateWAD(path);
  const auto volume = DiscIO::CreateVolume(path);
  const auto baseline = DiscIO::CreateVolume(path, false);
  ASSERT_TRUE(direct);
  ASSERT_TRUE(volume);
  ASSERT_TRUE(baseline);
  ASSERT_EQ(volume->GetVolumeType(), DiscIO::Platform::WiiWAD);
  ASSERT_EQ(baseline->GetVolumeType(), DiscIO::Platform::WiiWAD);
  const auto* normal = static_cast<const DiscIO::VolumeWAD*>(volume.get());
  const auto* opted_out = static_cast<const DiscIO::VolumeWAD*>(baseline.get());
  ASSERT_NE(normal, nullptr);
  ASSERT_NE(opted_out, nullptr);
  const auto content = normal->GetTMD().GetContents().at(0);
  for (const auto* candidate :
       std::array<const DiscIO::VolumeWAD*, 3>{direct.get(), normal, opted_out})
  {
    EXPECT_TRUE(candidate->CheckContentIntegrity(content, candidate->GetContent(0), ticket));
    auto changed = candidate->GetContent(0);
    changed[0] ^= 1;
    EXPECT_FALSE(candidate->CheckContentIntegrity(content, changed, ticket));
  }
}

}  // namespace
