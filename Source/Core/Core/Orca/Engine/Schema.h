// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <picojson.h>

// The one-schema validator for game data (Orca split S5, pack-contract.md section 3): a JSON schema
// file (Tools/orca/mods/ruleset.schema.json first) is compiled into Orca and is also the site's
// copy, and both sides interpret the same small subset of JSON Schema, so a file one accepts the
// other accepts. The site's twin is YouGame web/src/lib/orca-data-schema.ts.
//
// The subset, chosen so every value a schema admits is typed and bounded:
//   - type object (properties, required, additionalProperties: false, always), array (items,
//     maxItems, minItems), integer (minimum and maximum, always), boolean, string (minLength and
//     maxLength in UTF-8 bytes and an x-orca-charset, always; a pattern only as the charset's own
//     spelling for other tools), or a node of strings given by enum;
//   - $ref to #/$defs/<name> (one hop: nothing inside a definition is a $ref), description and
//     title anywhere;
//   - x-orca-charset: "id" ([a-z0-9-], not starting with '-') or "text" (UTF-8, no control
//     characters);
//   - x-orca-indexOf "/a/b": an integer below the length of the array at that pointer of the same
//     document;
//   - at the root also $schema, $id, $defs, x-orca-limits, x-orca-rules and x-orca-keywords, which
//     must list exactly the keywords above (the site checks the same list).
// Anything else in a schema refuses the schema (SchemaProblem): a keyword one validator would
// ignore is a rule the other would not enforce.
namespace Orca::Engine
{
// The keywords the interpreter understands, in the schema's x-orca-keywords order.
std::span<const std::string_view> SchemaKeywords();
// Why `schema` is outside the subset, or nullopt.
std::optional<std::string> SchemaProblem(const picojson::value& schema);
// Why `document` does not match `schema` ("<path>: why", the path as a JSON pointer), or nullopt.
// `schema` must have passed SchemaProblem.
std::optional<std::string> SchemaMismatch(const picojson::value& schema,
                                          const picojson::value& document);
}  // namespace Orca::Engine
