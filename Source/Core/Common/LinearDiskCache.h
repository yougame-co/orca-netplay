// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"

// On disk format:
// header{
// u32 'DCAC';
// u32 version;  // svn_rev
// u16 sizeof(key_type);
// u16 sizeof(value_type);
//}

// key_value_pair{
// u32 value_size;
// key_type   key;
// value_type[value_size]   value;
//}

namespace Common
{
template <typename K, typename V>
class LinearDiskCacheReader
{
public:
  virtual void Read(const K& key, const V* value, u32 value_size) = 0;
};

// Dead simple unsorted key-value store with append functionality.
// No random read functionality, all reading is done in OpenAndRead.
// Keys and values can contain any characters, including \0.
//
// Suitable for caching generated shader bytecode between executions.
// Not tuned for extreme performance but should be reasonably fast.
// Does not support keys or values larger than 2GB, which should be reasonable.
// Keys must have non-zero length; values can have zero length.

// K and V are some POD type
// K : the key type
// V : value array type
template <typename K, typename V, typename FileType = File::IOFile>
class LinearDiskCache
{
public:
  // return number of read entries
  u32 OpenAndRead(const std::string& filename, LinearDiskCacheReader<K, V>& reader,
                  u32 max_value_size = std::numeric_limits<u32>::max(),
                  u64 max_file_size = std::numeric_limits<u64>::max())
  {
    // Since we're reading/writing directly to the storage of K instances,
    // K must be trivially copyable.
    static_assert(std::is_trivially_copyable<K>::value, "K must be a trivially copyable type");

    // close any currently opened file
    Close();
    m_num_entries = 0;
    m_max_file_size = max_file_size;
    m_writes_disabled = false;

    // try opening for reading/writing
    m_file.Open(filename, "r+b");

    const u64 file_size = m_file.GetSize();
    m_file_size = file_size;

    m_header.Init();
    if (m_file.IsOpen() && file_size <= max_file_size && ValidateHeader())
    {
      // good header, read some key/value pairs
      K key;

      std::unique_ptr<V[]> value = nullptr;
      u32 value_size = 0;
      u32 entry_number = 0;
      u64 last_valid_value_start = m_file.Tell();

      while (m_file.ReadArray(&value_size, 1))
      {
        // Validate the complete record before allocating: a damaged length must not request an
        // unbounded allocation, or wrap the extent calculation for value types larger than u8.
        const u64 bytes = sizeof(K) + u64{value_size} * sizeof(V) + sizeof(entry_number);
        const u64 value_start = m_file.Tell();
        if (value_size > max_value_size || value_start > file_size ||
            bytes > file_size - value_start)
          break;

        value = std::make_unique_for_overwrite<V[]>(value_size);

        // read key/value and pass to reader
        if (m_file.ReadArray(&key, 1) && m_file.ReadArray(value.get(), value_size) &&
            m_file.ReadArray(&entry_number, 1) && entry_number == m_num_entries + 1)
        {
          last_valid_value_start = m_file.Tell();
          reader.Read(key, value.get(), value_size);
        }
        else
        {
          break;
        }

        m_num_entries++;
      }
      m_file.ClearError();
      m_file.Seek(last_valid_value_start, File::SeekOrigin::Begin);

      return m_num_entries;
    }

    // failed to open file for reading or bad header
    // close and recreate file
    Close();
    m_file.Open(filename, "wb");
    m_file_size = sizeof(Header);
    if (!m_file.WriteArray(&m_header, 1))
      DisableWrites("header write failed");
    return 0;
  }

  bool Sync()
  {
    if (m_writes_disabled)
      return false;
    if (!m_file.Flush())
    {
      DisableWrites("flush failed");
      return false;
    }
    return true;
  }
  void Close()
  {
    if (m_file.IsOpen())
    {
      // Close also reports prior read errors; only a new close/flush failure disables writes.
      const bool was_good = m_file.IsGood();
      if (!m_file.Close() && was_good)
        DisableWrites("close/flush failed");
    }
  }

  // A failed append leaves any already loaded entries usable, but persistence stays disabled
  // until the next OpenAndRead. Never append behind a partial record or retry a failed write.
  bool Append(const K& key, const V* value, u32 value_size)
  {
    if (m_writes_disabled)
      return false;

    const u64 record_size = sizeof(value_size) + sizeof(K) + u64{value_size} * sizeof(V) +
                            sizeof(m_num_entries);
    if (m_file_size > m_max_file_size || record_size > m_max_file_size - m_file_size)
    {
      DisableWrites("file size limit reached");
      return false;
    }

    const u32 next_entry = m_num_entries + 1;
    if (!m_file.WriteArray(&value_size, 1) || !m_file.WriteArray(&key, 1) ||
        !m_file.WriteArray(value, value_size) || !m_file.WriteArray(&next_entry, 1))
    {
      DisableWrites("append failed");
      return false;
    }
    m_num_entries = next_entry;
    m_file_size += record_size;
    return true;
  }

private:
  void DisableWrites(const char* reason)
  {
    if (!m_writes_disabled)
      WARN_LOG_FMT(COMMON, "Disk cache: {}; further writes disabled for this session", reason);
    m_writes_disabled = true;
  }

  bool ValidateHeader()
  {
    char file_header[sizeof(Header)];

    return (m_file.ReadArray(file_header, sizeof(Header)) &&
            !memcmp((const char*)&m_header, file_header, sizeof(Header)));
  }

  struct Header
  {
    void Init()
    {
      // Null-terminator is intentionally not copied.
      std::memcpy(&id, "DCAC", sizeof(u32));
      std::memcpy(ver, Common::GetScmRevGitStr().c_str(),
                  std::min(Common::GetScmRevGitStr().size(), sizeof(ver)));
    }

    u32 id = 0;
    const u16 key_t_size = sizeof(K);
    const u16 value_t_size = sizeof(V);
    char ver[40] = {};

  } m_header;

  FileType m_file;
  u64 m_file_size = 0;
  u64 m_max_file_size = std::numeric_limits<u64>::max();
  bool m_writes_disabled = false;
  u32 m_num_entries = 0;
};
}  // namespace Common
