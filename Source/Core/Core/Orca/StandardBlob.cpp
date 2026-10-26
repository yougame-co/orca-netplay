// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/StandardBlob.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/Orca/DiscReport.h"
#include "Core/Orca/RebuildStandard.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/VolumeDisc.h"

namespace Orca
{
namespace
{
using Wii = DiscIO::VolumeWii;
struct Segment
{
  u64 offset;
  u64 size;
  u64 source_offset = 0;
  std::vector<u8> memory;
};

void Write32(std::vector<u8>* bytes, size_t offset, u64 n)
{
  const u32 be = Common::swap32(static_cast<u32>(n));
  std::memcpy(bytes->data() + offset, &be, sizeof(be));
}

bool Within(u64 offset, u64 size, u64 end)
{
  return offset <= end && size <= end - offset;
}

void CollectOffsets(const DiscIO::FileInfo& directory, const std::string& parent,
                    std::map<std::string, u64>* offsets)
{
  for (const auto& child : directory)
  {
    const std::string path = parent + child.GetName();
    if (child.IsDirectory())
      CollectOffsets(child, path + '/', offsets);
    else
      offsets->emplace(path, child.GetOffset());
  }
}
}  // namespace

struct StandardBlob::State
{
  DiscLayout::Layout layout;
  std::vector<Segment> files;
  std::vector<Segment> raw;
  std::vector<u64> zero_blocks;
  std::array<std::map<u64, Common::SHA1::Digest>, 3> erased_hashes;
  std::array<u8, Wii::AES_KEY_SIZE> key;
  std::array<u8, Common::AES::Context::BLOCK_SIZE> decrypted_zero;
};

StandardBlob::StandardBlob(std::unique_ptr<DiscIO::VolumeDisc> source, std::shared_ptr<State> state)
    : m_source(std::move(source)), m_state(std::move(state)), m_cache(this)
{
}

StandardBlob::~StandardBlob() = default;

std::unique_ptr<StandardBlob>
StandardBlob::Create(const std::string& image, const DiscLayout::Layout& layout, std::string* error)
{
  const auto fail = [&](std::string reason) -> std::unique_ptr<StandardBlob> {
    *error = std::move(reason);
    return nullptr;
  };
  if (!DiscLayout::Validate(layout, error))
    return nullptr;
  for (const auto& gap : layout.gaps)
  {
    if (gap.kind == "other")
      return fail(fmt::format("padding bytes at offset {} cannot be reconstructed; export a "
                              "layout with reproducible padding",
                              gap.offset));
  }
  auto source = DiscIO::CreateDisc(image, false);
  if (!source || source->GetVolumeType() != DiscIO::Platform::WiiDisc)
    return fail("cannot open Wii disc image");
  if (!DiscLayout::ValidateSource(*source, error))
    return nullptr;
  const auto partition = source->GetGamePartition();
  if (partition == DiscIO::PARTITION_NONE || source->GetGameID(partition) != layout.id)
    return fail("input game ID does not match layout");
  const auto report = DiscReport::Generate(*source, error, RebuildStandard::IsCancelled);
  if (!report)
    return nullptr;
  const auto* fs = source->GetFileSystem(partition);
  if (!fs)
    return fail("cannot read input filesystem");
  std::map<std::string, u64> source_offsets;
  CollectOffsets(fs->GetRoot(), "", &source_offsets);
  std::map<std::string, DiscReport::FileEntry> actual;
  for (const auto& file : report->files)
    actual.emplace(file.path, file);
  auto state = std::make_shared<State>();
  state->layout = layout;
  std::sort(state->layout.gaps.begin(), state->layout.gaps.end(),
            [](const auto& a, const auto& b) { return a.offset < b.offset; });
  for (const auto& gap : state->layout.gaps)
  {
    if (gap.physical_zero)
    {
      for (u64 block = gap.offset / Wii::BLOCK_DATA_SIZE;
           block <= (gap.offset + gap.size - 1) / Wii::BLOCK_DATA_SIZE; ++block)
        state->zero_blocks.push_back(block);
    }
  }
  std::sort(state->zero_blocks.begin(), state->zero_blocks.end());
  state->zero_blocks.erase(std::unique(state->zero_blocks.begin(), state->zero_blocks.end()),
                           state->zero_blocks.end());
  if (!state->zero_blocks.empty() && state->layout.erased_hashes.empty())
  {
    // Version 2 layouts did not retain missing subtrees. An input with the same H3
    // can supply them, including for an identity rebuild of an older layout.
    const auto hashes = DiscLayout::ReadErasedHashes(*source, layout, error);
    if (!hashes)
      return nullptr;
    state->layout.erased_hashes = *hashes;
  }
  for (const auto& hash : state->layout.erased_hashes)
    state->erased_hashes[hash.level - 1].emplace(hash.index, hash.digest);
  for (const auto& file : DiscLayout::Files(layout))
  {
    const auto it = actual.find(file.path);
    if (it == actual.end() || (file.path != "sys/fst.bin" && it->second.size != file.size))
      return fail("DIFF: file set or size differs from layout");
    actual.erase(it);
    Segment segment{file.offset, file.size};
    if (file.path == "sys/fst.bin")
    {
      segment.memory = *DiscLayout::BuildFST(layout);
    }
    else if (file.path == "sys/boot.bin")
    {
      segment.memory.resize(DiscIO::DISCHEADER_SIZE);
      if (!source->Read(0, segment.memory.size(), segment.memory.data(), partition))
        return fail("cannot read input boot.bin");
      const auto* dol = layout.FindSystemFile("sys/main.dol");
      const auto* fst = layout.FindSystemFile("sys/fst.bin");
      Write32(&segment.memory, 0x420, dol->offset / 4);
      Write32(&segment.memory, 0x424, fst->offset / 4);
      Write32(&segment.memory, 0x428, fst->size / 4);
      Write32(&segment.memory, 0x42c, layout.fst_max_size / 4);
    }
    else if (file.path == "sys/bi2.bin" || file.path == "sys/apploader.img")
    {
      segment.source_offset = file.offset;
    }
    else if (file.path == "sys/main.dol")
    {
      const auto dol = DiscIO::GetBootDOLOffset(*source, partition);
      if (!dol)
        return fail("cannot read input DOL offset");
      segment.source_offset = *dol;
    }
    else
    {
      const auto info = source_offsets.find(file.path);
      if (info == source_offsets.end())
        return fail("cannot locate input file");
      segment.source_offset = info->second;
    }
    if (segment.size)
      state->files.push_back(std::move(segment));
  }
  if (!actual.empty())
    return fail("DIFF: input contains extra files");
  std::sort(state->files.begin(), state->files.end(),
            [](const auto& a, const auto& b) { return a.offset < b.offset; });
  const auto& ticket = source->GetTicket(partition);
  const auto& tmd = source->GetTMD(partition);
  if (!ticket.IsValid() || !tmd.IsValid() || tmd.GetNumContents() != 1 ||
      ticket.GetBytes().size() != DiscIO::WII_PARTITION_TICKET_SIZE ||
      tmd.GetBytes().size() != layout.tmd.size)
    return fail("input ticket or TMD does not fit layout");
  state->key = source->GetTitleKey(partition);
  const std::array<u8, Common::AES::Context::BLOCK_SIZE> zero{};
  const auto aes = Common::AES::CreateContextDecrypt(state->key.data());
  if (!aes->CryptIvZero(zero.data(), state->decrypted_zero.data(), zero.size()))
    return fail("cannot initialize physical-zero gap regeneration");
  // Read the certificate range explicitly: a failed cached certificate read looks like an empty
  // chain.
  const auto cert_size = source->ReadSwapped<u32>(partition.offset + 0x2ac, DiscIO::PARTITION_NONE);
  const auto cert_offset =
      source->ReadSwappedAndShifted(partition.offset + 0x2b0, DiscIO::PARTITION_NONE);
  if (!cert_size || !cert_offset || *cert_size != layout.cert.size)
    return fail("input certificate chain does not fit layout");
  std::vector<u8> header(static_cast<size_t>(layout.data.offset));
  std::copy(ticket.GetBytes().begin(), ticket.GetBytes().end(), header.begin());
  std::copy(tmd.GetBytes().begin(), tmd.GetBytes().end(), header.begin() + layout.tmd.offset);
  if (!source->Read(partition.offset + *cert_offset, *cert_size, header.data() + layout.cert.offset,
                    DiscIO::PARTITION_NONE))
    return fail("cannot read input certificate chain");
  Write32(&header, 0x2a4, layout.tmd.size);
  Write32(&header, 0x2a8, layout.tmd.offset / 4);
  Write32(&header, 0x2ac, layout.cert.size);
  Write32(&header, 0x2b0, layout.cert.offset / 4);
  Write32(&header, 0x2b4, layout.h3.offset / 4);
  Write32(&header, 0x2b8, layout.data.offset / 4);
  Write32(&header, 0x2bc, layout.data.size / 4);
  std::vector<u8> disc_header(DiscIO::WII_NONPARTITION_DISCHEADER_SIZE);
  std::vector<u8> region(DiscIO::WII_REGION_DATA_SIZE);
  if (!source->Read(0, disc_header.size(), disc_header.data(), DiscIO::PARTITION_NONE) ||
      !source->Read(DiscIO::WII_REGION_DATA_ADDRESS, region.size(), region.data(),
                    DiscIO::PARTITION_NONE))
    return fail("cannot read input disc header or region data");
  if (disc_header[0x60] != 0 || disc_header[0x61] != 0)
    return fail("input must have Wii hashes and encryption");
  std::vector<u8> table(0x28);
  Write32(&table, 0, 1);
  Write32(&table, 4, 0x40020 / 4);
  Write32(&table, 0x20, layout.partition_offset / 4);
  state->raw.push_back({0, disc_header.size(), 0, std::move(disc_header)});
  state->raw.push_back({0x40000, table.size(), 0, std::move(table)});
  state->raw.push_back({DiscIO::WII_REGION_DATA_ADDRESS, region.size(), 0, std::move(region)});
  state->raw.push_back({layout.partition_offset, header.size(), 0, std::move(header)});
  return std::unique_ptr<StandardBlob>(new StandardBlob(std::move(source), std::move(state)));
}

void StandardBlob::RestoreErasedHashes(Wii::HashBlock* hashes, u64 group) const
{
  for (u64 base = 0; base < Wii::BLOCKS_PER_GROUP; base += 8)
  {
    for (u64 block = base; block < base + 8; ++block)
    {
      const auto it = m_state->erased_hashes[0].find(group * Wii::BLOCKS_PER_GROUP + block);
      if (it != m_state->erased_hashes[0].end())
        hashes[base].h1[block - base] = it->second;
    }
    for (u64 block = base + 1; block < base + 8; ++block)
      hashes[block].h1 = hashes[base].h1;
    const auto it = m_state->erased_hashes[1].find(group * 8 + base / 8);
    hashes[0].h2[base / 8] = it != m_state->erased_hashes[1].end() ?
                                 it->second :
                                 Common::SHA1::CalculateDigest(hashes[base].h1);
  }
  for (size_t block = 1; block < Wii::BLOCKS_PER_GROUP; ++block)
    hashes[block].h2 = hashes[0].h2;
}

bool StandardBlob::ComputeH3(std::string* error)
{
  auto& layout = m_state->layout;
  auto& header = m_state->raw.back().memory;
  std::vector<std::array<u8, Wii::BLOCK_DATA_SIZE>> data(Wii::BLOCKS_PER_GROUP);
  std::vector<Wii::HashBlock> hashes(Wii::BLOCKS_PER_GROUP);
  for (u64 offset = 0, group = 0; offset < layout.DecryptedSize();
       offset += Wii::GROUP_DATA_SIZE, ++group)
  {
    if (RebuildStandard::IsCancelled())
    {
      *error = "cancelled";
      return false;
    }
    if (group % 32 == 0)
      fmt::print(stderr, "PROGRESS {} 1000\n", 100 + offset * 150 / layout.DecryptedSize());
    if (!Wii::HashGroup(data.data(), hashes.data(), [&](size_t block) {
          const u64 position = offset + block * Wii::BLOCK_DATA_SIZE;
          data[block].fill(0);
          return position >= layout.DecryptedSize() ||
                 ReadWiiDecrypted(position, Wii::BLOCK_DATA_SIZE, data[block].data(),
                                  layout.partition_offset + layout.data.offset);
        }))
    {
      *error = "cannot hash rebuilt partition";
      return false;
    }
    RestoreErasedHashes(hashes.data(), group);
    const auto it = m_state->erased_hashes[2].find(group);
    const auto hash = it != m_state->erased_hashes[2].end() ?
                          it->second :
                          Common::SHA1::CalculateDigest(hashes[0].h2);
    std::copy(hash.begin(), hash.end(), header.begin() + layout.h3.offset + group * hash.size());
  }
  const auto hash = Common::SHA1::CalculateDigest(header.data() + layout.h3.offset, layout.h3.size);
  const size_t content_hash =
      layout.tmd.offset + sizeof(IOS::ES::TMDHeader) + offsetof(IOS::ES::Content, sha1);
  std::copy(hash.begin(), hash.end(), header.begin() + content_hash);
  return true;
}

std::unique_ptr<DiscIO::BlobReader> StandardBlob::CopyReader() const
{
  auto source = DiscIO::CreateDisc(m_source->GetBlobReader().CopyReader(), false);
  if (!source)
    return nullptr;
  return std::unique_ptr<StandardBlob>(new StandardBlob(std::move(source), m_state));
}

u64 StandardBlob::GetDataSize() const
{
  return m_state->layout.partition_offset + m_state->layout.data.offset + m_state->layout.data.size;
}

bool StandardBlob::SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const
{
  const auto& layout = m_state->layout;
  return partition_data_offset == layout.partition_offset + layout.data.offset &&
         Within(offset, size, layout.DecryptedSize());
}

bool StandardBlob::ReadWiiDecrypted(u64 offset, u64 size, u8* buffer, u64 partition_data_offset)
{
  if (RebuildStandard::IsCancelled() ||
      !SupportsReadWiiDecrypted(offset, size, partition_data_offset))
    return false;
  std::fill_n(buffer, static_cast<size_t>(size), 0);
  const auto& layout = m_state->layout;
  auto gap = std::lower_bound(layout.gaps.begin(), layout.gaps.end(), offset,
                              [](const auto& g, u64 p) { return g.offset + g.size <= p; });
  for (; gap != layout.gaps.end() && gap->offset < offset + size; ++gap)
  {
    const u64 end = std::min(offset + size, gap->offset + gap->size);
    if (gap->kind == "zero")
    {
      if (gap->physical_zero)
      {
        // Zero ciphertext and a zero header IV produce the same AES block throughout
        // every 0x7c00-byte payload. The pattern stays in memory and is never serialized.
        for (u64 start = std::max(offset, gap->offset); start < end; ++start)
          buffer[start - offset] = m_state->decrypted_zero[start % m_state->decrypted_zero.size()];
      }
      continue;
    }
    for (u64 start = std::max(offset, gap->offset); start < end;)
    {
      const u64 unit = start / DiscLayout::JUNK_UNIT_SIZE;
      const u64 length =
          std::min(end - start, DiscLayout::JUNK_UNIT_SIZE - start % DiscLayout::JUNK_UNIT_SIZE);
      const auto seed = gap->kind == "junk" ?
                            DiscLayout::FormulaSeed(layout.id, layout.disc_number, start) :
                            gap->seeds[unit - gap->offset / DiscLayout::JUNK_UNIT_SIZE];
      DiscIO::LaggedFibonacciGenerator lfg;
      lfg.SetSeed(seed.data());
      lfg.Forward(start % DiscLayout::JUNK_UNIT_SIZE);
      lfg.GetBytes(length, buffer + start - offset);
      start += length;
    }
  }
  auto it = std::lower_bound(m_state->files.begin(), m_state->files.end(), offset,
                             [](const Segment& s, u64 p) { return s.offset + s.size <= p; });
  for (; it != m_state->files.end() && it->offset < offset + size; ++it)
  {
    const u64 start = std::max(offset, it->offset);
    const u64 length = std::min(offset + size, it->offset + it->size) - start;
    if (!it->memory.empty())
      std::memcpy(buffer + start - offset, it->memory.data() + start - it->offset, length);
    else if (!m_source->Read(it->source_offset + start - it->offset, length,
                             buffer + start - offset, m_source->GetGamePartition()))
      return false;
  }
  return true;
}

bool StandardBlob::Read(u64 offset, u64 size, u8* buffer)
{
  if (!Within(offset, size, GetDataSize()))
    return false;
  std::fill_n(buffer, static_cast<size_t>(size), 0);
  for (const auto& segment : m_state->raw)
  {
    const u64 start = std::max(offset, segment.offset);
    const u64 end = std::min(offset + size, segment.offset + segment.size);
    if (start < end)
      std::memcpy(buffer + start - offset, segment.memory.data() + start - segment.offset,
                  end - start);
  }
  const auto& layout = m_state->layout;
  const u64 data_offset = layout.partition_offset + layout.data.offset;
  const u64 start = std::max(offset, data_offset);
  const u64 end = std::min(offset + size, GetDataSize());
  if (start < end && !m_cache.EncryptGroups(
                         start - data_offset, end - start, buffer + start - offset, data_offset,
                         layout.DecryptedSize(), m_state->key,
                         [this](Wii::HashBlock* hashes, u64 group_offset) {
                           RestoreErasedHashes(hashes, group_offset / Wii::GROUP_TOTAL_SIZE);
                         }))
    return false;
  if (start < end)
  {
    auto block = std::lower_bound(m_state->zero_blocks.begin(), m_state->zero_blocks.end(),
                                  (start - data_offset) / Wii::BLOCK_TOTAL_SIZE);
    for (; block != m_state->zero_blocks.end(); ++block)
    {
      const u64 block_start = data_offset + *block * Wii::BLOCK_TOTAL_SIZE;
      if (block_start >= end)
        break;
      const u64 zero_start = std::max(start, block_start);
      const u64 zero_end = std::min(end, block_start + Wii::BLOCK_TOTAL_SIZE);
      std::fill(buffer + zero_start - offset, buffer + zero_end - offset, 0);
    }
  }
  return true;
}
}  // namespace Orca
