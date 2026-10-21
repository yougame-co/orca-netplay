// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Core/Orca/UX/RankedSteps.h"

// Rulesets as data (Orca split S5, modules.md section 5): a ruleset file (format 1) names the legal
// stages, the stage steps' timers, the clauses, the set's rules and the character select's timer.
// The engine checks every file it loads against the compiled-in schema
// (Tools/orca/mods/ruleset.schema.json, Engine/Schema.h) and the rules a schema can't state (its
// x-orca-rules), then reads it into the engine's typed Ranked::Ruleset. Anything unknown, out of
// range or of the wrong type refuses the file; nothing is defaulted, repaired or clamped.
namespace Orca::Engine
{
// The compiled-in schema, byte for byte the file in Tools/orca/mods.
std::string_view RulesetSchemaText();
// Why the compiled-in schema is unusable (a build defect: every ruleset is then refused), or
// nullopt.
std::optional<std::string> RulesetSchemaProblem();

// A ruleset file, checked and read; nullopt and why when it is refused.
std::optional<UX::Ranked::Ruleset> ReadRuleset(std::string_view json, std::string* error);
}  // namespace Orca::Engine
