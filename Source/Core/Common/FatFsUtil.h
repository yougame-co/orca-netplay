// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <span>
#include <string>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

namespace Common
{
static constexpr auto SD_PACK_TEXT = _trans("Pack SD Card Now");
static constexpr auto SD_UNPACK_TEXT = _trans("Unpack SD Card Now");

bool SyncSDFolderToSDImage(const std::function<bool()>& cancelled, bool deterministic);
bool SyncSDImageToSDFolder(const std::function<bool()>& cancelled);

// Orca mods (Core/Orca/Mod.h): an SD image built from exactly these files, never a directory scan.
struct SDImageFile
{
  std::string image_path;   // in the image, '/'-separated, no leading '/'
  std::string source_path;  // on the host
  u64 size = 0;             // the file must still be this long
};
// A new image of `size` bytes (whole 32 KiB clusters) at `image_path`, written to a temporary file
// and renamed into place: FAT32 as SyncSDFolderToSDImage formats it, each folder's files before its
// folders, each by upper-cased name (Project+'s official image's order), and every FAT time zero,
// so the same files and size give the same bytes on every machine.
// `cluster_bytes` (a power of two, 512 to 32768; 0: FatFs's choice for the size) and `fats` (1 or 2;
// 0: one) pin the volume's geometry.
bool BuildSDImageFromFiles(std::span<const SDImageFile> files, u64 size,
                           const std::string& image_path, std::string* error,
                           u32 cluster_bytes = 0, u8 fats = 0);

class FatFsCallbacks
{
public:
  FatFsCallbacks();
  virtual ~FatFsCallbacks();

  virtual u8 DiskInitialize(u8 pdrv);
  virtual u8 DiskStatus(u8 pdrv);
  virtual int DiskRead(u8 pdrv, u8* buff, u32 sector, unsigned int count) = 0;
  virtual int DiskWrite(u8 pdrv, const u8* buff, u32 sector, unsigned int count) = 0;
  virtual int DiskIOCtl(u8 pdrv, u8 cmd, void* buff) = 0;
  virtual u32 GetCurrentTimeFAT();
};

void RunInFatFsContext(FatFsCallbacks& callbacks, const std::function<void()>& function);
}  // namespace Common
