// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Common/Crypto/SHA1.h"

// Orca: integrity envelopes for Metal's cached MSL. Context binds a payload to its identity,
// stage or UID. Length-prefix fields so different identities cannot alias by concatenation.
namespace VideoCommon::ShaderCacheData
{
inline constexpr u32 VERSION = 1;
inline constexpr size_t MAX_PAYLOAD_SIZE = 16 * 1024 * 1024;
using Digest = Common::SHA1::Digest;

inline Digest Identity(std::span<const std::string_view> fields)
{
  auto hash = Common::SHA1::CreateContext();
  hash->Update(reinterpret_cast<const u8*>(&VERSION), sizeof(VERSION));
  for (const auto field : fields)
  {
    const u64 size = field.size();
    hash->Update(reinterpret_cast<const u8*>(&size), sizeof(size));
    hash->Update(field);
  }
  return hash->Finish();
}

inline Digest Check(std::span<const u8> context, std::span<const u8> payload)
{
  auto hash = Common::SHA1::CreateContext();
  hash->Update(reinterpret_cast<const u8*>(&VERSION), sizeof(VERSION));
  const u64 size = context.size();
  hash->Update(reinterpret_cast<const u8*>(&size), sizeof(size));
  hash->Update(context);
  hash->Update(payload);
  return hash->Finish();
}

inline std::vector<u8> Seal(std::span<const u8> context, std::span<const u8> payload)
{
  if (payload.empty() || payload.size() > MAX_PAYLOAD_SIZE)
    return {};
  const auto check = Check(context, payload);
  std::vector<u8> result(check.begin(), check.end());
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

inline std::optional<std::span<const u8>> Open(std::span<const u8> context,
                                               std::span<const u8> record)
{
  if (record.size() <= Digest{}.size() || record.size() > MAX_PAYLOAD_SIZE + Digest{}.size())
    return std::nullopt;
  const auto payload = record.subspan(Digest{}.size());
  const auto check = Check(context, payload);
  if (!std::equal(check.begin(), check.end(), record.begin()))
    return std::nullopt;
  return payload;
}
}  // namespace VideoCommon::ShaderCacheData
