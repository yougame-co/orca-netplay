// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/Rulesets.h"

#include <string>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Core/Orca/Engine/Ruleset.h"

namespace Orca::UX::Rulesets
{
namespace
{
// The data files, as bytes (CMake writes the lists from Games/Brawl/Rulesets/).
constexpr char BRAWL_2025[] = {
#include "brawl-supernova-2025.json.inc"
};
constexpr char BRAWL_2026[] = {
#include "brawl-supernova-2026.json.inc"
};
constexpr char PPLUS_2024[] = {
#include "pplus-2024.json.inc"
};

Ranked::Ruleset Load(std::string_view name, std::string_view json)
{
  std::string error;
  auto ruleset = Orca::Engine::ReadRuleset(json, &error);
  if (!ruleset)
  {
    ERROR_LOG_FMT(ROLLBACK, "Builtin ruleset {} refused: {}", name, error);
    return {};
  }
  return std::move(*ruleset);
}
}  // namespace

const Ranked::Ruleset& Brawl2025()
{
  static const Ranked::Ruleset r =
      Load("brawl-supernova-2025", std::string_view(BRAWL_2025, sizeof(BRAWL_2025)));
  return r;
}

const Ranked::Ruleset& Brawl2026()
{
  static const Ranked::Ruleset r =
      Load("brawl-supernova-2026", std::string_view(BRAWL_2026, sizeof(BRAWL_2026)));
  return r;
}

const Ranked::Ruleset& RulesFor(bool casual)
{
  return casual ? Brawl2025() : Brawl2026();
}

const Ranked::Ruleset& PPlus2024()
{
  static const Ranked::Ruleset r = Load("pplus-2024", std::string_view(PPLUS_2024, sizeof(PPLUS_2024)));
  return r;
}

bool Loaded()
{
  static const bool loaded = Brawl2025().id != 0 && Brawl2026().id != 0 && PPlus2024().id != 0;
  return loaded;
}

const Ranked::Ruleset* ByBlock(u8 block)
{
  for (const Ranked::Ruleset* r : {&Brawl2026(), &PPlus2024()})
  {
    if (r->id != 0 && r->id == block)
      return r;
  }
  return nullptr;
}
}  // namespace Orca::UX::Rulesets
