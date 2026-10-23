// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// libFuzzer target for the replay stream reader (Session/Keyframe.h "Replay streams"): the bytes
// come from other players, so the parser must survive anything. The first byte picks how the rest
// is cut into Feed calls, as a growing file or a network stream would deliver it. Wire it beside
// the WP07 targets (their CMake and corpus); build with -fsanitize=fuzzer,address.
#include <cstddef>
#include <cstdint>
#include <string>

#include "Core/Orca/Session/Keyframe.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0)
    return 0;
  const size_t piece = 1 + data[0] % 97;
  Orca::Net::ReplayStreamReader reader(size & 1 ? "orca2:fuzz" : "");
  std::string error;
  for (size_t at = 1; at < size; at += piece)
  {
    const size_t n = size - at < piece ? size - at : piece;
    if (!reader.Feed(data + at, n, &error))
      break;
  }
  return 0;
}
