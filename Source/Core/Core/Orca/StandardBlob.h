// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "Core/Orca/DiscLayout.h"
#include "DiscIO/Blob.h"
#include "DiscIO/WiiEncryptionCache.h"

namespace DiscIO
{
class VolumeDisc;
}

namespace Orca
{
// A standalone, read-only view. All decrypted buffers remain in memory.
class StandardBlob final : public DiscIO::BlobReader
{
public:
  static std::unique_ptr<StandardBlob> Create(const std::string& image,
                                              const DiscLayout::Layout& layout, std::string* error);
  ~StandardBlob() override;
  bool ComputeH3(std::string* error);

  DiscIO::BlobType GetBlobType() const override { return DiscIO::BlobType::DIRECTORY; }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override;
  u64 GetRawSize() const override { return GetDataSize(); }
  u64 GetDataSize() const override;
  DiscIO::DataSizeType GetDataSizeType() const override { return DiscIO::DataSizeType::Accurate; }
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }
  bool Read(u64 offset, u64 size, u8* buffer) override;
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override;
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* buffer, u64 partition_data_offset) override;

private:
  struct State;
  void RestoreErasedHashes(DiscIO::VolumeWii::HashBlock* hashes, u64 group) const;
  StandardBlob(std::unique_ptr<DiscIO::VolumeDisc> source, std::shared_ptr<State> state);
  std::unique_ptr<DiscIO::VolumeDisc> m_source;
  std::shared_ptr<State> m_state;
  DiscIO::WiiEncryptionCache m_cache;
};
}  // namespace Orca
