// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3DCommon/FxcCache.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoConfig.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3dcompiler.h>

#include "Common/StringUtil.h"
#endif

namespace D3DCommon::FxcCache
{
namespace
{
// File layout: MAGIC, FORMAT, then one record per compile: bytecode size (u32), key, SHA-1 of key
// plus bytecode, and the bytecode. A record that fails its check (torn write, or two processes
// appending at once) ends the read, and the file is rewritten from the good records before it, so
// bytecode is never read back under the wrong key.
constexpr u32 MAGIC = 0x4358464f;  // "OFXC"
// Changing FORMAT must also change what RecordCheck hashes, since an older Orca sharing the user
// folder may keep appending old-format records after a newer one rewrote the header.
constexpr u32 FORMAT = 1;
// A larger file starts over. Shaders from older builds pile up as the generators change, and the
// whole file is held in memory (Brawl's and P+'s specialized shaders are about 8 MB).
constexpr u64 MAX_FILE_SIZE = u64{64} << 20;
constexpr u32 MAX_RECORD_SIZE = u32{16} << 20;
// The file stays open for the whole run. A plain open on Windows locks out other processes, so
// every open here shares read and write access. Two instances writing at once can tear a record;
// its check stops it being read, so later records are lost rather than misread.
constexpr File::SharedAccess SHARED = File::SharedAccess::ReadWrite;

using Digest = Common::SHA1::Digest;

struct State
{
  std::mutex mutex;
  bool opened = false;
  File::IOFile file;
  std::map<Key, std::vector<u8>> entries;
  size_t read = 0;
  size_t found = 0;
  size_t added = 0;
};

State& GetState()
{
  static State state;
  return state;
}

Digest RecordCheck(const Key& key, std::span<const u8> bytecode)
{
  auto context = Common::SHA1::CreateContext();
  context->Update(key.data(), key.size());
  context->Update(bytecode.data(), bytecode.size());
  return context->Finish();
}

// Unbuffered, so each record goes out in one write and can't interleave with another process's.
bool OpenToWrite(File::IOFile& file, const std::string& path, const char* mode)
{
  if (!file.Open(path, mode, SHARED))
    return false;
  std::setvbuf(file.GetHandle(), nullptr, _IONBF, 0);
  return true;
}

void WriteRecord(File::IOFile& file, const Key& key, std::span<const u8> bytecode)
{
  const u32 size = static_cast<u32>(bytecode.size());
  const Digest check = RecordCheck(key, bytecode);
  std::vector<u8> record(sizeof(size) + key.size() + check.size() + bytecode.size());
  u8* out = record.data();
  std::memcpy(out, &size, sizeof(size));
  out += sizeof(size);
  std::memcpy(out, key.data(), key.size());
  out += key.size();
  std::memcpy(out, check.data(), check.size());
  out += check.size();
  std::memcpy(out, bytecode.data(), bytecode.size());
  file.WriteBytes(record.data(), record.size());
}

std::string FilePath()
{
  // Creates the folder if needed. No host config in the name: the key already covers it.
  return GetDiskShaderCacheFileName(APIType::D3D, "fxc", false, false);
}

// Reads every valid record. Returns false unless the file ends right after the last one.
bool ReadRecords(const std::string& path, std::map<Key, std::vector<u8>>* entries)
{
  File::IOFile in(path, "rb", SHARED);
  if (!in)
    return false;
  const u64 file_size = in.GetSize();
  if (file_size > MAX_FILE_SIZE)
  {
    NOTICE_LOG_FMT(VIDEO, "Orca: {} is {} MB, past {} MB: it starts over", path, file_size >> 20,
                   MAX_FILE_SIZE >> 20);
    return false;
  }
  u32 header[2] = {};
  if (!in.ReadArray(header, 2) || header[0] != MAGIC || header[1] != FORMAT)
    return false;
  for (;;)
  {
    // May be past the size measured at open if another process was appending; that record was
    // still read whole and checked.
    if (in.Tell() >= file_size)
      return true;
    u32 size = 0;
    Key key{};
    Digest check{};
    if (!in.ReadArray(&size, 1) || size == 0 || size > MAX_RECORD_SIZE || !in.ReadArray(&key) ||
        !in.ReadArray(&check))
    {
      return false;
    }
    std::vector<u8> bytecode(size);
    if (!in.ReadBytes(bytecode.data(), bytecode.size()) || RecordCheck(key, bytecode) != check)
      return false;
    entries->insert_or_assign(key, std::move(bytecode));
  }
}

void OpenLocked(State& state)
{
  if (state.opened)
    return;
  state.opened = true;

  const std::string path = FilePath();
  const bool whole = ReadRecords(path, &state.entries);
  state.read = state.entries.size();
  if (whole)
  {
    OpenToWrite(state.file, path, "ab");
    return;
  }

  // Missing, old format, too big, or a bad record: rewrite it from what was read.
  if (File::Exists(path))
  {
    WARN_LOG_FMT(VIDEO, "Orca: {} didn't read to its end; writing it again with {} shaders", path,
                 state.entries.size());
  }
  if (!OpenToWrite(state.file, path, "wb"))
    return;
  const u32 header[2] = {MAGIC, FORMAT};
  state.file.WriteArray(header, 2);
  for (const auto& [key, bytecode] : state.entries)
    WriteRecord(state.file, key, bytecode);
  // Reopen to append: "wb" would overwrite anything another process appended meanwhile.
  OpenToWrite(state.file, path, "ab");
}

std::optional<Digest> ReadCompilerDigest()
{
#ifdef _WIN32
  const HMODULE dll = GetModuleHandleW(D3DCOMPILER_DLL_W);
  if (!dll)
    return std::nullopt;
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(dll, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    return std::nullopt;
  path.resize(length);
  std::string bytes;
  if (!File::ReadFileToString(WStringToUTF8(path), bytes) || bytes.empty())
    return std::nullopt;
  return Common::SHA1::CalculateDigest(bytes);
#else
  return std::nullopt;
#endif
}

// The compiler DLL is loaded before the first compile and doesn't change during the process.
const std::optional<Digest>& CompilerDigest()
{
  static const std::optional<Digest> digest = ReadCompilerDigest();
  return digest;
}
}  // namespace

std::optional<Key> MakeKey(std::string_view hlsl, std::string_view entry, std::string_view target,
                           u32 flags, std::string_view macros)
{
  if (!g_ActiveConfig.bShaderCache)
    return std::nullopt;
  const std::optional<Digest>& compiler = CompilerDigest();
  if (!compiler)
    return std::nullopt;
  return KeyFor(*compiler, hlsl, entry, target, flags, macros);
}

Key KeyFor(const Digest& compiler, std::string_view hlsl, std::string_view entry,
           std::string_view target, u32 flags, std::string_view macros)
{
  // Prefix each part with its length so different inputs can't hash the same bytes.
  auto context = Common::SHA1::CreateContext();
  const auto part = [&context](const void* data, size_t size) {
    const u64 size64 = size;
    context->Update(reinterpret_cast<const u8*>(&size64), sizeof(size64));
    context->Update(static_cast<const u8*>(data), size);
  };
  part(compiler.data(), compiler.size());
  part(entry.data(), entry.size());
  part(target.data(), target.size());
  part(&flags, sizeof(flags));
  part(macros.data(), macros.size());
  part(hlsl.data(), hlsl.size());
  return context->Finish();
}

std::optional<std::vector<u8>> Find(const Key& key)
{
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  OpenLocked(state);
  const auto it = state.entries.find(key);
  if (it == state.entries.end())
    return std::nullopt;
  state.found++;
  return it->second;
}

void Store(const Key& key, std::span<const u8> bytecode)
{
  if (bytecode.empty() || bytecode.size() > MAX_RECORD_SIZE)
    return;
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  OpenLocked(state);
  if (!state.entries.try_emplace(key, bytecode.begin(), bytecode.end()).second)
    return;
  state.added++;
  if (state.file.IsOpen())
  {
    WriteRecord(state.file, key, bytecode);
    // Flush so a killed process keeps what it compiled.
    state.file.Flush();
  }
}

void Close()
{
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  if (!state.opened)
    return;
  NOTICE_LOG_FMT(VIDEO,
                 "Orca: FXC cache: {} shaders read from {}, {} compiles found there, {} compiled "
                 "and added",
                 state.read, FilePath(), state.found, state.added);
  state.file.Close();
  state.entries.clear();
  state.opened = false;
  state.read = state.found = state.added = 0;
}
}  // namespace D3DCommon::FxcCache
