// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoaderManager.h"

// Orca: the GPU's guest-chosen writes and reads into guest RAM stay inside it, and a copy or draw
// that fits is checked exactly as before.

TEST(OrcaEfbCopyBounds, AStrideUnderARowCoversTheLastRow)
{
  // A normal copy: rows `stride` apart, the stride at least a row.
  EXPECT_EQ(EFBCopyCheckedRange(640, 120, 640), 640u * 120);
  EXPECT_EQ(EFBCopyCheckedRange(640, 120, 1024), 1024u * 120);
  // A smaller stride: the last row starts at 119 * stride and is a full row long.
  EXPECT_EQ(EFBCopyCheckedRange(640, 120, 32), 119u * 32 + 640);
  EXPECT_EQ(EFBCopyCheckedRange(16384, 256, 0), 16384u);
  EXPECT_EQ(EFBCopyCheckedRange(640, 1, 0), 640u);
  EXPECT_EQ(EFBCopyCheckedRange(640, 0, 32), 0u);
  // The largest the registers allow: no overflow.
  EXPECT_EQ(EFBCopyCheckedRange(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
            u64{0xFFFFFFFF} * 0xFFFFFFFF);
}

namespace
{
using VertexLoaderManager::IndexedRead;
using VertexLoaderManager::INDEXED_OVERREAD;
using VertexLoaderManager::MAX_INDEXED_READS;

struct Arrays
{
  Common::EnumMap<u32, CPArray::XF_D> strides{};
  Common::EnumMap<u32, CPArray::TexCoord7> room{};
  Common::EnumMap<u32, CPArray::TexCoord7> before{};
};

bool Fit(std::span<const IndexedRead> reads, const std::vector<u8>& vertices, u32 vertex_size,
         const Arrays& arrays)
{
  return VertexLoaderManager::IndexedReadsFit(reads, vertices.data(),
                                              static_cast<u32>(vertices.size() / vertex_size),
                                              vertex_size, arrays.strides, arrays.room,
                                              arrays.before);
}
}  // namespace

TEST(OrcaVertexBounds, ReadsFollowTheVertexLayout)
{
  TVtxDesc desc{};
  VAT vat{};
  desc.low.PosMatIdx = true;
  desc.low.Position = VertexComponentFormat::Index16;
  vat.g0.PosFormat = ComponentFormat::Float;
  vat.g0.PosElements = CoordComponentCount::XYZ;
  desc.low.Normal = VertexComponentFormat::Index8;
  vat.g0.NormalFormat = ComponentFormat::Short;
  vat.g0.NormalElements = NormalComponentCount::N;
  desc.low.Color0 = VertexComponentFormat::Direct;
  vat.g0.Color0Comp = ColorFormat::RGBA8888;
  desc.low.Color1 = VertexComponentFormat::Index16;
  vat.g0.Color1Comp = ColorFormat::RGBA6666;
  desc.high.Tex0Coord = VertexComponentFormat::Index8;
  vat.g0.Tex0CoordFormat = ComponentFormat::Float;
  vat.g0.Tex0CoordElements = TexComponentCount::ST;
  desc.high.Tex1Coord = VertexComponentFormat::Direct;
  vat.g1.Tex1CoordFormat = ComponentFormat::UByte;
  vat.g1.Tex1CoordElements = TexComponentCount::S;

  std::array<IndexedRead, MAX_INDEXED_READS> reads{};
  ASSERT_EQ(VertexLoaderManager::GetIndexedReads(desc, vat, &reads).value_or(99), 4u);
  // Position matrix index (1), position index (2), normal index (1), color 0 (4), color 1 index
  // (2), coordinate 0 index (1), coordinate 1 (1): 12 bytes, as the loader counts them.
  EXPECT_EQ(VertexLoaderBase::GetVertexSize(desc, vat), 12u);
  EXPECT_EQ(reads[0].array, CPArray::Position);
  EXPECT_EQ(reads[0].src_offset, 1);
  EXPECT_EQ(reads[0].index_bytes, 2);
  EXPECT_EQ(reads[0].extent, 12);
  EXPECT_TRUE(reads[0].skip_max);
  EXPECT_EQ(reads[1].array, CPArray::Normal);
  EXPECT_EQ(reads[1].src_offset, 3);
  EXPECT_EQ(reads[1].index_bytes, 1);
  EXPECT_EQ(reads[1].extent, 6);
  EXPECT_EQ(reads[2].array, CPArray::Color1);
  EXPECT_EQ(reads[2].src_offset, 8);
  EXPECT_EQ(reads[2].extent, 3);
  EXPECT_TRUE(reads[2].reads_before);
  EXPECT_EQ(reads[3].array, CPArray::TexCoord0);
  EXPECT_EQ(reads[3].src_offset, 10);
  EXPECT_EQ(reads[3].extent, 8);

  // A loader made for this layout carries the same reads.
  const std::unique_ptr<VertexLoaderBase> loader = VertexLoaderBase::CreateVertexLoader(desc, vat);
  ASSERT_TRUE(loader->m_indexed_reads_known);
  ASSERT_EQ(loader->m_num_indexed_reads, 4u);
  EXPECT_EQ(loader->m_indexed_reads[2].src_offset, 8);
}

TEST(OrcaVertexBounds, ThreeNormalIndicesReadThreeVectors)
{
  TVtxDesc desc{};
  VAT vat{};
  desc.low.Position = VertexComponentFormat::Direct;
  vat.g0.PosFormat = ComponentFormat::Float;
  vat.g0.PosElements = CoordComponentCount::XYZ;
  desc.low.Normal = VertexComponentFormat::Index16;
  vat.g0.NormalFormat = ComponentFormat::Float;
  vat.g0.NormalElements = NormalComponentCount::NTB;
  vat.g0.NormalIndex3 = true;

  std::array<IndexedRead, MAX_INDEXED_READS> reads{};
  ASSERT_EQ(VertexLoaderManager::GetIndexedReads(desc, vat, &reads).value_or(99), 3u);
  EXPECT_EQ(VertexLoaderBase::GetVertexSize(desc, vat), 12u + 6);
  for (u32 k = 0; k < 3; ++k)
  {
    EXPECT_EQ(reads[k].src_offset, 12 + 2 * k);
    EXPECT_EQ(reads[k].extent, 12 * (k + 1));
  }

  // One index, all nine floats.
  vat.g0.NormalIndex3 = false;
  ASSERT_EQ(VertexLoaderManager::GetIndexedReads(desc, vat, &reads).value_or(99), 1u);
  EXPECT_EQ(reads[0].extent, 36);
  EXPECT_EQ(VertexLoaderBase::GetVertexSize(desc, vat), 12u + 2);
}

// The reads' offsets add up to the raw vertex size the loaders use, for any description.
TEST(OrcaVertexBounds, OffsetsMatchTheVertexSizeForEveryLayout)
{
  std::mt19937 rng(0x0BCA);
  std::array<IndexedRead, MAX_INDEXED_READS> reads{};
  for (int i = 0; i < 200'000; ++i)
  {
    TVtxDesc desc{};
    VAT vat{};
    desc.low.Hex = rng() & 0x1FFFF;
    desc.high.Hex = rng() & 0xFFFF;
    vat.g0.Hex = rng();
    vat.g1.Hex = rng();
    vat.g2.Hex = rng();
    bool invalid_color = false, invalid_format = false;
    for (u32 c = 0; c < 2; ++c)
    {
      const bool invalid = vat.GetColorFormat(c) > ColorFormat::RGBA8888;
      invalid_format |= invalid;
      invalid_color |= invalid && desc.low.Color[c] != VertexComponentFormat::NotPresent;
    }
    u32 bytes = 0;
    const auto count = VertexLoaderManager::GetIndexedReads(desc, vat, &reads, &bytes);
    if (invalid_color)
    {
      EXPECT_FALSE(count.has_value());
      continue;
    }
    ASSERT_TRUE(count.has_value());
    // GetVertexSize raises a panic alert for an invalid format even when its color is absent.
    if (invalid_format)
      continue;
    ASSERT_EQ(bytes, VertexLoaderBase::GetVertexSize(desc, vat))
        << std::hex << desc.low.Hex << " " << desc.high.Hex << " " << vat.g0.Hex << " "
        << vat.g1.Hex << " " << vat.g2.Hex;
    for (std::size_t r = 0; r < *count; ++r)
      ASSERT_LE(u32{reads[r].src_offset} + reads[r].index_bytes, bytes);
  }
}

// A color in an invalid format (6 or 7): the loaders and the vertex size disagree on where later
// attributes are, so its draws can't be checked and are dropped.
TEST(OrcaVertexBounds, AnInvalidColorFormatCantBeChecked)
{
  TVtxDesc desc{};
  VAT vat{};
  desc.low.Position = VertexComponentFormat::Direct;
  desc.low.Color0 = VertexComponentFormat::Index8;
  vat.g0.Color0Comp = static_cast<ColorFormat>(6);
  std::array<IndexedRead, MAX_INDEXED_READS> reads{};
  EXPECT_FALSE(VertexLoaderManager::GetIndexedReads(desc, vat, &reads).has_value());
  desc.low.Color0 = VertexComponentFormat::Direct;
  vat.g0.Color0Comp = static_cast<ColorFormat>(7);
  EXPECT_FALSE(VertexLoaderManager::GetIndexedReads(desc, vat, &reads).has_value());
  // Not present: its format field is ignored.
  desc.low.Color0 = VertexComponentFormat::NotPresent;
  EXPECT_TRUE(VertexLoaderManager::GetIndexedReads(desc, vat, &reads).has_value());
}

TEST(OrcaVertexBounds, AnElementPastTheRoomDropsTheDraw)
{
  IndexedRead position;
  position.array = CPArray::Position;
  position.index_bytes = 2;
  position.extent = 12;
  position.skip_max = true;
  const std::array<IndexedRead, 1> reads{position};
  Arrays arrays;
  arrays.strides[CPArray::Position] = 12;
  arrays.room[CPArray::Position] = 1000;
  arrays.before[CPArray::Position] = 0x1000;

  const auto vertices = [](std::initializer_list<u16> indices) {
    std::vector<u8> bytes;
    for (const u16 index : indices)
    {
      bytes.push_back(static_cast<u8>(index >> 8));
      bytes.push_back(static_cast<u8>(index));
    }
    return bytes;
  };
  // The last whole element (and the loaders' word over-read) ends at the room's end.
  const u16 last = (1000 - 12 - INDEXED_OVERREAD) / 12;
  EXPECT_TRUE(Fit(reads, vertices({0, 5, last}), 2, arrays));
  EXPECT_FALSE(Fit(reads, vertices({0, static_cast<u16>(last + 1), 5}), 2, arrays));
  EXPECT_FALSE(Fit(reads, vertices({0xFFFE}), 2, arrays));
  // The largest index skips the vertex and reads nothing.
  EXPECT_TRUE(Fit(reads, vertices({0xFFFF, 3}), 2, arrays));
  // A base with room for every index never looks at the indices.
  arrays.room[CPArray::Position] = 0xFFFF * 12 + 12 + INDEXED_OVERREAD;
  EXPECT_TRUE(Fit(reads, vertices({0xFFFE, 0}), 2, arrays));
  // A stride of zero reads element 0 only.
  arrays.room[CPArray::Position] = 16;
  arrays.strides[CPArray::Position] = 0;
  EXPECT_TRUE(Fit(reads, vertices({0xFFFE, 7}), 2, arrays));
  // No room at all (a base never set): nothing is read.
  arrays.room[CPArray::Position] = 0;
  EXPECT_FALSE(Fit(reads, vertices({0}), 2, arrays));
}

TEST(OrcaVertexBounds, AByteBeforeRamIsOutsideItToo)
{
  IndexedRead color;
  color.array = CPArray::Color0;
  color.index_bytes = 1;
  color.extent = 3;
  color.reads_before = true;
  const std::array<IndexedRead, 1> reads{color};
  Arrays arrays;
  arrays.strides[CPArray::Color0] = 4;
  arrays.room[CPArray::Color0] = 1 << 20;
  arrays.before[CPArray::Color0] = 0;
  EXPECT_FALSE(Fit(reads, {3, 0, 2}, 1, arrays));
  EXPECT_TRUE(Fit(reads, {3, 1, 2}, 1, arrays));
  arrays.before[CPArray::Color0] = 1;
  EXPECT_TRUE(Fit(reads, {3, 0, 2}, 1, arrays));
}
