// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <string_view>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/LinearDiskCache.h"
#include "VideoCommon/ShaderCacheData.h"

namespace
{
namespace Cache = VideoCommon::ShaderCacheData;

TEST(MetalShaderCache, EveryIdentityFieldInvalidates)
{
  std::array<std::string_view, 6> fields = {"commit", "version", "os-build",
                                            "metal",  "gpu",     "options"};
  const auto identity = Cache::Identity(fields);
  EXPECT_EQ(identity, Cache::Identity(fields));
  for (size_t i = 0; i < fields.size(); ++i)
  {
    auto changed = fields;
    changed[i] = "changed";
    EXPECT_NE(identity, Cache::Identity(changed)) << i;
  }
  const std::array<std::string_view, 2> ab_c = {"ab", "c"};
  const std::array<std::string_view, 2> a_bc = {"a", "bc"};
  EXPECT_NE(Cache::Identity(ab_c), Cache::Identity(a_bc));
}

TEST(MetalShaderCache, IdentityStageAndUidMustMatch)
{
  const std::array<u8, 3> identity_stage = {1, 2, 3};
  const std::array<u8, 2> uid = {4, 5};
  const std::array<u8, 3> msl = {'m', 's', 'l'};
  const auto inner = Cache::Seal(identity_stage, msl);
  const auto record = Cache::Seal(uid, inner);
  const auto checked = Cache::Open(uid, record);
  ASSERT_TRUE(checked);
  const auto source = Cache::Open(identity_stage, *checked);
  ASSERT_TRUE(source);
  EXPECT_TRUE(std::equal(source->begin(), source->end(), msl.begin(), msl.end()));
  const std::array<u8, 2> wrong_uid = {4, 6};
  EXPECT_FALSE(Cache::Open(wrong_uid, record));
  const std::array<u8, 3> wrong_identity_stage = {1, 2, 4};
  EXPECT_FALSE(Cache::Open(wrong_identity_stage, *checked));
}

TEST(MetalShaderCache, CorruptionAndTruncationAreMisses)
{
  const std::array<u8, 1> context = {1};
  const std::array<u8, 3> msl = {'m', 's', 'l'};
  const auto record = Cache::Seal(context, msl);
  for (size_t i = 0; i < record.size(); ++i)
  {
    auto damaged = record;
    damaged[i] ^= 1;
    EXPECT_FALSE(Cache::Open(context, damaged)) << i;
    EXPECT_FALSE(Cache::Open(context, std::span(record).first(i))) << i;
  }
  EXPECT_TRUE(Cache::Seal(context, {}).empty());
  EXPECT_FALSE(Cache::Open(context, msl));  // old raw-MSL format
  const std::vector<u8> oversized(Cache::MAX_PAYLOAD_SIZE + 1);
  EXPECT_TRUE(Cache::Seal(context, oversized).empty());
  EXPECT_FALSE(Cache::Open(context, oversized));
}

struct Reader : Common::LinearDiskCacheReader<u32, u8>
{
  u32 reads = 0;
  void Read(const u32&, const u8*, u32) override { ++reads; }
};

TEST(MetalShaderCache, DiskLimitsAndMalformedRecordFallBack)
{
  const auto dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir + "/shader.cache";
  Common::LinearDiskCache<u32, u8> disk;
  Reader reader;
  EXPECT_EQ(disk.OpenAndRead(path, reader), 0u);
  constexpr std::array<u8, 4> data = {1, 2, 3, 4};
  static_assert(data.size() <= std::numeric_limits<u32>::max());
  constexpr u32 data_size = static_cast<u32>(data.size());
  disk.Append(7, data.data(), data_size);
  disk.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader, 3), 0u);  // excessive record size, no reader call
  EXPECT_EQ(reader.reads, 0u);
  disk.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader, 16, 1), 0u);  // excessive file size recreates header
  disk.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader, 16, 1024), 0u);
  ASSERT_TRUE(disk.Append(7, data.data(), data_size));
  disk.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader, 16, 1024), 1u);
  EXPECT_EQ(reader.reads, 1u);
  disk.Close();
  // A length extending past EOF is rejected before allocation.
  File::IOFile file(path, "r+b");
  const u64 size = file.GetSize();
  ASSERT_TRUE(file.Seek(size - data.size() - sizeof(u32) - sizeof(u32) - sizeof(u32),
                        File::SeekOrigin::Begin));
  const u32 bad_size = 0xffffffff;
  ASSERT_TRUE(file.WriteArray(&bad_size, 1));
  file.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader, 16, 1024), 0u);
  EXPECT_EQ(reader.reads, 1u);
  disk.Close();
  File::DeleteDirRecursively(dir);
}
// Use real file I/O except for a deterministic one-shot append/flush failure.
struct FailingCacheFile : File::IOFile
{
  static inline int writes_until_failure = -1;
  static inline bool fail_flush = false;
  static inline u32 write_calls = 0;
  static inline u32 flush_calls = 0;

  template <typename T>
  bool WriteArray(const T* values, size_t count)
  {
    ++write_calls;
    if (writes_until_failure == 0)
    {
      writes_until_failure = -1;
      // Leave a partial record, just as a short write would.
      File::IOFile::WriteArray(values, count / 2);
      return false;
    }
    if (writes_until_failure > 0)
      --writes_until_failure;
    return File::IOFile::WriteArray(values, count);
  }

  bool Flush()
  {
    ++flush_calls;
    if (fail_flush)
    {
      fail_flush = false;
      return false;
    }
    return File::IOFile::Flush();
  }
};

TEST(MetalShaderCache, CapReachedMidSessionDisablesEvenSmallerAppends)
{
  const auto dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir + "/shader.cache";
  Common::LinearDiskCache<u32, u8> disk;
  Reader reader;
  disk.OpenAndRead(path, reader);
  ASSERT_TRUE(disk.Sync());
  const auto header_size = File::GetSize(path);
  disk.Close();
  constexpr std::array<u8, 4> data = {1, 2, 3, 4};
  static_assert(data.size() <= std::numeric_limits<u32>::max());
  constexpr u32 data_size = static_cast<u32>(data.size());
  const u64 record_size = 3 * sizeof(u32) + data.size();
  const u64 cap = header_size + 2 * record_size + 3 * sizeof(u32);
  disk.OpenAndRead(path, reader, 16, cap);
  ASSERT_TRUE(disk.Append(1, data.data(), data_size));
  ASSERT_TRUE(disk.Append(2, data.data(), data_size));
  EXPECT_FALSE(disk.Append(3, data.data(), data_size));
  EXPECT_FALSE(disk.Append(4, data.data(), 0));  // would fit, but persistence stays disabled
  disk.Close();
  EXPECT_EQ(File::GetSize(path), header_size + 2 * record_size);
  EXPECT_EQ(disk.OpenAndRead(path, reader, 16, cap), 2u);
  EXPECT_EQ(reader.reads, 2u);
  disk.Close();
  File::DeleteDirRecursively(dir);
}

TEST(MetalShaderCache, WriteFailureStopsWithoutRetryAndKeepsLoadedEntries)
{
  const auto dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir + "/shader.cache";
  Common::LinearDiskCache<u32, u8, FailingCacheFile> disk;
  Reader reader;
  constexpr std::array<u8, 4> data = {1, 2, 3, 4};
  static_assert(data.size() <= std::numeric_limits<u32>::max());
  constexpr u32 data_size = static_cast<u32>(data.size());
  disk.OpenAndRead(path, reader);
  ASSERT_TRUE(disk.Append(1, data.data(), data_size));
  disk.Close();
  ASSERT_EQ(disk.OpenAndRead(path, reader), 1u);
  ASSERT_EQ(reader.reads, 1u);
  FailingCacheFile::writes_until_failure = 2;  // length and key succeed, payload is short
  EXPECT_FALSE(disk.Append(2, data.data(), data_size));
  const u32 calls = FailingCacheFile::write_calls;
  const u32 flushes = FailingCacheFile::flush_calls;
  EXPECT_FALSE(disk.Append(3, data.data(), data_size));
  EXPECT_FALSE(disk.Sync());
  EXPECT_EQ(FailingCacheFile::write_calls, calls);
  EXPECT_EQ(FailingCacheFile::flush_calls, flushes);
  EXPECT_EQ(reader.reads, 1u);
  disk.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader), 1u);  // partial tail was never extended
  disk.Close();
  File::DeleteDirRecursively(dir);
}

TEST(MetalShaderCache, FlushFailureStopsWithoutRetry)
{
  const auto dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir + "/shader.cache";
  Common::LinearDiskCache<u32, u8, FailingCacheFile> disk;
  Reader reader;
  disk.OpenAndRead(path, reader);
  constexpr std::array<u8, 1> data = {1};
  static_assert(data.size() <= std::numeric_limits<u32>::max());
  constexpr u32 data_size = static_cast<u32>(data.size());
  ASSERT_TRUE(disk.Append(1, data.data(), data_size));
  FailingCacheFile::fail_flush = true;
  EXPECT_FALSE(disk.Sync());
  const u32 calls = FailingCacheFile::write_calls;
  const u32 flushes = FailingCacheFile::flush_calls;
  EXPECT_FALSE(disk.Append(2, data.data(), data_size));
  EXPECT_FALSE(disk.Sync());
  EXPECT_EQ(FailingCacheFile::write_calls, calls);
  EXPECT_EQ(FailingCacheFile::flush_calls, flushes);
  disk.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader), 1u);
  EXPECT_EQ(reader.reads, 1u);
  disk.Close();
  File::DeleteDirRecursively(dir);
}

TEST(MetalShaderCache, DiskHeaderMismatchAndTruncatedWideValueAreMisses)
{
  const auto dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir + "/shader.cache";
  Common::LinearDiskCache<u32, u8> disk;
  Reader reader;
  disk.OpenAndRead(path, reader);
  constexpr std::array<u8, 1> data = {1};
  static_assert(data.size() <= std::numeric_limits<u32>::max());
  constexpr u32 data_size = static_cast<u32>(data.size());
  disk.Append(7, data.data(), data_size);
  disk.Close();
  File::IOFile file(path, "r+b");
  // LinearDiskCache's header is magic, key/value sizes, then the 40-byte SCM revision.
  ASSERT_TRUE(file.Seek(8, File::SeekOrigin::Begin));
  const u8 changed_revision = 0xff;
  ASSERT_TRUE(file.WriteBytes(&changed_revision, 1));
  file.Close();
  EXPECT_EQ(disk.OpenAndRead(path, reader), 0u);
  EXPECT_EQ(reader.reads, 0u);
  disk.Close();

  struct WideReader : Common::LinearDiskCacheReader<u64, u64>
  {
    u32 reads = 0;
    void Read(const u64&, const u64*, u32) override { ++reads; }
  } wide_reader;
  Common::LinearDiskCache<u64, u64> wide_disk;
  wide_disk.OpenAndRead(path, wide_reader);
  wide_disk.Close();
  File::IOFile wide_file(path, "ab");
  const u32 two_values = 2;
  const u64 key = 7;
  ASSERT_TRUE(wide_file.WriteArray(&two_values, 1));
  ASSERT_TRUE(wide_file.WriteArray(&key, 1));
  ASSERT_TRUE(wide_file.WriteArray(&key, 1));  // only one of two u64 values, and no footer
  wide_file.Close();
  EXPECT_EQ(wide_disk.OpenAndRead(path, wide_reader), 0u);
  EXPECT_EQ(wide_reader.reads, 0u);
  wide_disk.Close();
  File::DeleteDirRecursively(dir);
}

}  // namespace
