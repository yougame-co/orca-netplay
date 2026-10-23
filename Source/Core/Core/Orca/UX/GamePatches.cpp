// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/GamePatches.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/PowerPC/MMU.h"

namespace Orca::UX
{
namespace
{
std::optional<u32> Hex32(std::string_view word)
{
  if (word.empty() || word.size() > 8)
    return std::nullopt;
  u32 v = 0;
  for (const char c : word)
  {
    int d;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      d = c - 'A' + 10;
    else
      return std::nullopt;
    v = v << 4 | static_cast<u32>(d);
  }
  return v;
}

std::vector<std::string_view> Words(std::string_view line)
{
  std::vector<std::string_view> words;
  size_t i = 0;
  while (i < line.size())
  {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      ++i;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t')
      ++i;
    if (i > start)
      words.push_back(line.substr(start, i - start));
  }
  return words;
}

// A word-aligned address in MEM1 or MEM2.
bool InRam(std::optional<u32> address)
{
  return address && (*address % 4 == 0) &&
         ((*address >= 0x80000000 && *address < 0x81800000) ||
          (*address >= 0x90000000 && *address < 0x94000000));
}

// Parses `when <address> [& <mask>] ==|!= <value>`. Rejects a zero mask, or a value with bits
// outside the mask.
std::optional<PatchCondition> ParseCondition(std::span<const std::string_view> words)
{
  if (words.size() != 4 && words.size() != 6)
    return std::nullopt;
  PatchCondition c;
  const auto address = Hex32(words[1]);
  if (!InRam(address))
    return std::nullopt;
  c.address = *address;
  size_t op = 2;
  if (words.size() == 6)
  {
    const auto mask = Hex32(words[3]);
    if (words[2] != "&" || !mask || *mask == 0)
      return std::nullopt;
    c.mask = *mask;
    op = 4;
  }
  if (words[op] != "==" && words[op] != "!=")
    return std::nullopt;
  c.equal = words[op] == "==";
  const auto value = Hex32(words[op + 1]);
  if (!value || (*value & ~c.mask) != 0)
    return std::nullopt;
  c.value = *value;
  return c;
}

// One past the last line of the group starting at `start`.
size_t GroupEnd(std::span<const GamePatch> patches, size_t start)
{
  size_t end = start + 1;
  while (end < patches.size() && patches[end].joins_previous)
    ++end;
  return end;
}
}  // namespace

std::optional<std::vector<GamePatch>> ParseGamePatches(std::string_view text, std::string* error)
{
  std::vector<GamePatch> patches;
  std::vector<int> lines;  // each patch's line number
  std::set<u32> seen;
  // The open `when` block.
  std::vector<PatchCondition> when;
  int when_line = 0;
  bool block_has_lines = false;
  // Set after `when` or `end`: the next line always starts a new group.
  bool group_break = false;
  // Checked at the end, once every written address is known.
  std::vector<std::pair<u32, int>> condition_lines;
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };
  int number = 0;
  if (text.starts_with("\xEF\xBB\xBF"))  // UTF-8 BOM
    text.remove_prefix(3);
  while (!text.empty())
  {
    ++number;
    const size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (const size_t hash = line.find('#'); hash != std::string_view::npos)
      line = line.substr(0, hash);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    const auto words = Words(line);
    if (words.empty())
      continue;
    if (words[0] == "when")
    {
      if (block_has_lines)
        return fail(fmt::format("line {}: `when` inside a block: close it with `end` first", number));
      const auto condition = ParseCondition(words);
      if (!condition)
      {
        return fail(fmt::format("line {}: expected `when <address> [& <mask>] ==|!= <value>` in hex "
                                "(a nonzero mask, the value inside it)",
                                number));
      }
      if (when.empty())
        when_line = number;
      when.push_back(*condition);
      condition_lines.emplace_back(condition->address, number);
      group_break = true;
      continue;
    }
    if (words[0] == "end")
    {
      if (words.size() != 1)
        return fail(fmt::format("line {}: `end` takes nothing after it", number));
      if (when.empty())
        return fail(fmt::format("line {}: `end` without `when`", number));
      if (!block_has_lines)
        return fail(fmt::format("line {}: a `when` block with no lines", number));
      when.clear();
      block_has_lines = false;
      group_break = true;
      continue;
    }
    const auto address = words.size() == 3 ? Hex32(words[0]) : std::nullopt;
    const auto value = words.size() == 3 ? Hex32(words[2]) : std::nullopt;
    const auto original = words.size() == 3 && words[1] != "*" ? Hex32(words[1]) : std::nullopt;
    // Address must be word-aligned RAM; original must be `*` or hex.
    if (!InRam(address) || !value || (words[1] != "*" && !original))
      return fail(fmt::format("line {}: expected `<address> <original|*> <value>` in hex", number));
    if (!seen.insert(*address).second)
      return fail(fmt::format("line {}: address {:08X} repeated", number, *address));
    if (!when.empty() && !original)
      return fail(fmt::format("line {}: a `*` line can't be toggled: give its original", number));
    const bool joins = !group_break && original && !patches.empty() && patches.back().original &&
                       patches.back().address + 4 == *address;
    patches.push_back({*address, original, *value, joins, when});
    lines.push_back(number);
    group_break = false;
    block_has_lines = !when.empty();
  }
  if (!when.empty())
    return fail(fmt::format("line {}: `when` without `end`", when_line));
  for (const auto& [address, line] : condition_lines)
  {
    if (seen.contains(address))
    {
      return fail(fmt::format("line {}: the condition reads {:08X}, a word this file writes", line,
                              address));
    }
  }
  // A toggled group matches in two states, so require at least two words (to avoid matching
  // unrelated code by chance) and at least one that changes.
  for (size_t start = 0; start < patches.size();)
  {
    const size_t end = GroupEnd(patches, start);
    const std::span<const GamePatch> group(patches.data() + start, end - start);
    const int line = lines[start];
    start = end;
    if (group[0].when.empty())
      continue;
    if (group.size() < 2)
    {
      return fail(fmt::format("line {}: a toggled group needs two words at least: add the words "
                              "around it as guards",
                              line));
    }
    if (std::none_of(group.begin(), group.end(),
                     [](const GamePatch& p) { return p.value != *p.original; }))
    {
      return fail(fmt::format("line {}: a toggled group that changes nothing", line));
    }
  }
  return patches;
}

bool GroupApplies(std::span<const GamePatch> group, std::span<const u32> now)
{
  if (group.empty() || group.size() != now.size())
    return false;
  if (!group[0].original)  // a `*` line
    return now[0] != group[0].value;
  bool changes = false;
  for (size_t i = 0; i < group.size(); ++i)
  {
    if (now[i] != *group[i].original)
      return false;
    changes |= group[i].value != *group[i].original;
  }
  return changes;
}

bool ConditionsHold(std::span<const PatchCondition> when, const GuestMemory& memory)
{
  for (const PatchCondition& c : when)
  {
    if (!memory.Valid(c.address))
      return false;
    if (((memory.Read32(c.address) & c.mask) == c.value) != c.equal)
      return false;
  }
  return true;
}

Toggle ToggleGroup(std::span<const GamePatch> group, std::span<const u32> now, bool holds)
{
  if (group.empty() || group.size() != now.size())
    return Toggle::Leave;
  bool originals = true, values = true, changes = false;
  for (size_t i = 0; i < group.size(); ++i)
  {
    if (!group[i].original)  // can't happen in a parsed file
      return Toggle::Leave;
    originals &= now[i] == *group[i].original;
    values &= now[i] == group[i].value;
    changes |= group[i].value != *group[i].original;
  }
  // A group of guards only never writes.
  if (!changes)
    return Toggle::Leave;
  if (holds && originals)
    return Toggle::On;
  if (!holds && values)
    return Toggle::Off;
  return Toggle::Leave;
}

PatchPlan PlanGamePatches(std::span<const GamePatch> patches, const GuestMemory& memory,
                          bool toggled)
{
  PatchPlan plan;
  std::vector<u32> now;
  for (size_t start = 0; start < patches.size();)
  {
    const size_t end = GroupEnd(patches, start);
    const std::span<const GamePatch> group = patches.subspan(start, end - start);
    start = end;
    if (group[0].when.empty() == toggled)
      continue;
    now.clear();
    for (const GamePatch& p : group)
    {
      if (!memory.Valid(p.address))
        break;
      now.push_back(memory.Read32(p.address));
    }
    bool restore = false;
    if (!toggled)
    {
      if (!GroupApplies(group, now))
        continue;
      if (group[0].original)
        plan.landed.push_back({group[0].address, static_cast<u32>(group.size())});
    }
    else
    {
      const Toggle toggle = ToggleGroup(group, now, ConditionsHold(group[0].when, memory));
      if (toggle == Toggle::Leave)
        continue;
      restore = toggle == Toggle::Off;
      (restore ? plan.off : plan.on).push_back({group[0].address, static_cast<u32>(group.size())});
    }
    for (size_t i = 0; i < group.size(); ++i)
    {
      const u32 target = restore ? *group[i].original : group[i].value;
      if (now[i] != target)
        plan.writes.push_back({group[i].address, target, group[i].original.has_value()});
    }
  }
  return plan;
}

namespace
{
// Reloaded when the profile changes. CPU thread only.
const std::vector<GamePatch>& ActivePatches()
{
  static std::string s_for;
  static std::vector<GamePatch> s_patches;
  // e.g. RSBE01.patches for a disc, PPLUS32.patches for a launcher.
  const Orca::Profile* profile = Orca::ActiveProfile();
  // Mod mode: the bundle's orca/patches/*.patches, in name order (keyed by its tree).
  if (profile && profile->mod)
  {
    // Compared in place: this runs every frame, re-runs too.
    const std::string& tree = profile->mod->tree.hash;
    if (s_for.size() == 4 + tree.size() && s_for.starts_with("mod:") &&
        std::string_view(s_for).substr(4) == tree)
    {
      return s_patches;
    }
    s_for = "mod:" + tree;
    s_patches.clear();
    // A flat bundle: its patches files as they were verified (Core/Orca/Bundle.cpp), in mod.json's
    // order.
    if (profile->mod->flat)
    {
      for (const auto& [name, text] : profile->mod->flat_patches)
      {
        std::string error;
        const std::optional<std::vector<GamePatch>> parsed = ParseGamePatches(text, &error);
        if (!parsed)
        {
          ERROR_LOG_FMT(ROLLBACK, "Orca: {}{} refused, {}: no game patches", profile->mod->dir, name,
                        error);
          Orca::Status::Error("profile", "This mod's game patches are damaged.");
          s_patches.clear();
          break;
        }
        NOTICE_LOG_FMT(ROLLBACK, "Orca: {} game patches from {}{}", parsed->size(),
                       profile->mod->dir, name);
        s_patches.insert(s_patches.end(), parsed->begin(), parsed->end());
      }
      return s_patches;
    }
    for (const Orca::Mod::TreeFile* file : profile->mod->tree.Under("orca/patches/"))
    {
      if (!file->path.ends_with(".patches") ||
          file->path.find('/', sizeof("orca/patches/") - 1) != std::string::npos)
      {
        continue;
      }
      const std::string path = profile->mod->dir + file->path;
      std::string text, error;
      std::optional<std::vector<GamePatch>> parsed;
      if (File::ReadFileToString(path, text))
        parsed = ParseGamePatches(text, &error);
      else
        error = "it can't be read";
      if (!parsed)
      {
        ERROR_LOG_FMT(ROLLBACK, "Orca: {} refused, {}: no game patches", path, error);
        Orca::Status::Error("profile", "This mod's game patches are damaged.");
        s_patches.clear();
        break;
      }
      NOTICE_LOG_FMT(ROLLBACK, "Orca: {} game patches from {}", parsed->size(), path);
      s_patches.insert(s_patches.end(), parsed->begin(), parsed->end());
    }
    return s_patches;
  }
  const std::string game = profile ? profile->game_id : std::string{};
  if (game != s_for)
  {
    s_for = game;
    s_patches.clear();
    const std::string path = File::GetSysDirectory() + "Orca/" + game + ".patches";
    std::string text;
    if (!game.empty() && File::ReadFileToString(path, text))
    {
      std::string error;
      if (auto parsed = ParseGamePatches(text, &error))
      {
        s_patches = std::move(*parsed);
        NOTICE_LOG_FMT(ROLLBACK, "Orca: {} game patches from {}", s_patches.size(), path);
      }
      else
      {
        ERROR_LOG_FMT(ROLLBACK, "Orca: {} refused, {}: no game patches", path, error);
        Orca::Status::Error("profile",
                            "YG Dolphin's game data for this disc is damaged. Reinstall YG Dolphin.");
      }
    }
  }
  return s_patches;
}

void Apply(const Core::CPUThreadGuard& guard, bool toggled)
{
  const std::vector<GamePatch>& patches = ActivePatches();
  if (patches.empty())
    return;
  GuardMemory memory(guard);
  const PatchPlan plan = PlanGamePatches(patches, memory, toggled);
  // Tools/orca/online-menu.py parses "patch group <address> " lines; keep the format.
  for (const PatchPlan::Group& g : plan.landed)
    INFO_LOG_FMT(ROLLBACK, "Orca: patch group {:08x} ({} words) landed", g.address, g.words);
  for (const PatchPlan::Group& g : plan.on)
    INFO_LOG_FMT(ROLLBACK, "Orca: toggled group {:08x} ({} words) on", g.address, g.words);
  for (const PatchPlan::Group& g : plan.off)
    INFO_LOG_FMT(ROLLBACK, "Orca: toggled group {:08x} ({} words) off", g.address, g.words);
  for (const PatchWrite& w : plan.writes)
  {
    if (w.code)
      ApplyMemoryPatch<u32>(guard, w.value, w.address);  // invalidates the JIT's copy
    else
      PowerPC::MMU::HostWrite<u32>(guard, w.value, w.address);
  }
}
}  // namespace

void ApplyGamePatches(const Core::CPUThreadGuard& guard)
{
  Apply(guard, false);
}

void ApplyToggledGamePatches(const Core::CPUThreadGuard& guard)
{
  Apply(guard, true);
}
}  // namespace Orca::UX
