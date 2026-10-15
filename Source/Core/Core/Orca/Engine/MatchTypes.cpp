// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Engine/MatchTypes.h"

#include <fmt/format.h>

namespace Orca::Engine
{
std::string GameResult::DetailJson() const
{
  std::string out =
      fmt::format("{{\"f\":{},\"st\":{},\"c\":[{},{}],\"s\":[{},{}],\"to\":{}", frame, stage,
                  characters[0], characters[1], stocks[0], stocks[1], timeout ? 1 : 0);
  if (start >= 0)
  {
    out += fmt::format(",\"k\":{},\"lg\":[{},{}],\"tb\":{}", how, ledge[0], ledge[1],
                       tiebreak ? 1 : 0);
  }
  return out + "}";
}
}  // namespace Orca::Engine
