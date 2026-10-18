// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Mod.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <random>
#include <system_error>

#include <fmt/format.h>
#include <mbedtls/sha256.h>
#include <picojson.h>

#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/PowerPC/MMU.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

namespace Orca::Mod
{
namespace
{
constexpr auto FORCED_ALLOWLIST = std::to_array<std::string_view>({
    // A later pinned clock than the universal one (Project+ needs 2025-01-01: PPLUS32.ini).
    "Dolphin.Core.CustomRTCValue",
});
constexpr auto OFFERABLE_CAPS = std::to_array<std::string_view>({"results", "locks", "queue2"});

bool IsLowerHex(std::string_view s, size_t length)
{
  return s.size() == length && std::ranges::all_of(s, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string ToLowerAscii(std::string_view s)
{
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A JSON integer (picojson keeps numbers as doubles: exact up to 2^53) within [min, max].
std::optional<u64> Integer(const picojson::value& v, u64 min, u64 max)
{
  if (!v.is<double>())
    return std::nullopt;
  const double d = v.get<double>();
  if (!std::isfinite(d) || d != std::floor(d) || d < static_cast<double>(min) ||
      d > static_cast<double>(max))
  {
    return std::nullopt;
  }
  return static_cast<u64>(d);
}

// "0x" and 1-8 hex digits.
std::optional<u32> HexWord(const picojson::value& v)
{
  if (!v.is<std::string>())
    return std::nullopt;
  const std::string& s = v.get<std::string>();
  if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') ||
      !std::all_of(s.begin() + 2, s.end(), [](unsigned char c) { return std::isxdigit(c); }))
  {
    return std::nullopt;
  }
  return static_cast<u32>(std::stoul(s.substr(2), nullptr, 16));
}

// Whether brackets outside strings nest deeper than MAX_JSON_DEPTH. picojson recurses once per level
// with no limit of its own: 20 KB of brackets overflow Windows' 1 MiB main-thread stack.
bool TooDeep(std::string_view json)
{
  int depth = 0;
  bool in_string = false, escaped = false;
  for (const char c : json)
  {
    if (in_string)
    {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        in_string = false;
      continue;
    }
    if (c == '"')
      in_string = true;
    else if ((c == '[' || c == '{') && ++depth > MAX_JSON_DEPTH)
      return true;
    else if ((c == ']' || c == '}') && depth > 0)
      --depth;
  }
  return false;
}

// One JSON value of at most max_bytes and MAX_JSON_DEPTH levels, and nothing after it but whitespace
// (picojson's string parse ignores the rest).
std::string ParseWhole(picojson::value* root, std::string_view json, u64 max_bytes)
{
  if (json.size() > max_bytes)
    return fmt::format("over {} bytes", max_bytes);
  if (TooDeep(json))
    return fmt::format("nested deeper than {} levels", MAX_JSON_DEPTH);
  std::string error;
  const auto end = picojson::parse(*root, json.begin(), json.end(), &error);
  if (!error.empty())
    return error;
  if (!std::all_of(end, json.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }))
    return "something after the JSON value";
  return {};
}

bool InMem1Word(u32 address)
{
  return address >= 0x80000000 && address <= 0x817FFFFC && address % 4 == 0;
}

// The object's keys are exactly among `allowed` (nullopt) or the first that isn't.
std::optional<std::string> UnknownKey(const picojson::object& o,
                                      std::initializer_list<std::string_view> allowed)
{
  for (const auto& [key, value] : o)
  {
    if (std::ranges::find(allowed, key) == allowed.end())
      return key;
  }
  return std::nullopt;
}

bool IsVersion(std::string_view s)
{
  // <major>.<minor>.<patch>, decimal.
  int dots = 0;
  bool digit = false;
  for (char c : s)
  {
    if (c == '.')
    {
      if (!digit)
        return false;
      ++dots;
      digit = false;
    }
    else if (c >= '0' && c <= '9')
    {
      digit = true;
    }
    else
    {
      return false;
    }
  }
  return dots == 2 && digit && s.size() <= 20;
}

// A version's three numbers as written (IsVersion), without leading zeros ("007" is "7").
std::array<std::string_view, 3> VersionParts(std::string_view s)
{
  std::array<std::string_view, 3> parts{};
  for (std::string_view& part : parts)
  {
    const size_t dot = s.find('.');
    part = s.substr(0, dot);
    s = dot == std::string_view::npos ? std::string_view{} : s.substr(dot + 1);
    while (part.size() > 1 && part.front() == '0')
      part.remove_prefix(1);
  }
  return parts;
}
}  // namespace

#ifndef ORCA_ENGINE_VERSION
#error "ORCA_ENGINE_VERSION (CMake's ORCA_VERSION) is set for this file in Source/Core/Core/CMakeLists.txt"
#endif
std::string_view EngineVersion()
{
  return ORCA_ENGINE_VERSION;
}

bool VersionAtLeast(std::string_view version, std::string_view minimum)
{
  const auto have = VersionParts(version), need = VersionParts(minimum);
  for (size_t i = 0; i < have.size(); ++i)
  {
    // Decimal digits without leading zeros: the longer number is the larger, else by digit.
    if (have[i].size() != need[i].size())
      return have[i].size() > need[i].size();
    if (have[i] != need[i])
      return have[i] > need[i];
  }
  return true;
}

// --- The tree ----------------------------------------------------------------------------------

const TreeFile* Tree::Find(std::string_view path) const
{
  const auto it = std::ranges::lower_bound(files, path, {}, &TreeFile::path);
  return it != files.end() && it->path == path ? &*it : nullptr;
}

std::vector<const TreeFile*> Tree::Under(std::string_view folder) const
{
  std::vector<const TreeFile*> out;
  for (const TreeFile& f : files)
  {
    if (f.path.starts_with(folder))
      out.push_back(&f);
  }
  return out;
}

std::optional<std::string> PathProblem(std::string_view path)
{
  if (path.empty())
    return "an empty path";
  if (path.size() > MAX_PATH_BYTES)
    return fmt::format("longer than {} bytes", MAX_PATH_BYTES);
  for (const char c : path)
  {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7e || std::string_view("<>:\"\\|?*").find(c) != std::string_view::npos)
      return "a character outside printable ASCII, or one of <>:\"\\|?*";
  }
  size_t start = 0;
  while (true)
  {
    const size_t slash = path.find('/', start);
    const std::string_view part =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (part.empty())
      return "an empty component (a leading, trailing or doubled '/')";
    if (part.front() == '.')
      return "a component that starts with a dot";
    if (part.back() == '.' || part.back() == ' ')
      return "a component that ends in a dot or a space";
    // Windows device names, with or without an extension.
    std::string base = ToLowerAscii(part.substr(0, part.find('.')));
    while (!base.empty() && base.back() == ' ')
      base.pop_back();
    static constexpr std::array<std::string_view, 4> devices{"con", "prn", "aux", "nul"};
    if (std::ranges::find(devices, base) != devices.end() ||
        (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) &&
         base[3] >= '0' && base[3] <= '9'))
    {
      return "a Windows device name";
    }
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return std::nullopt;
}

std::optional<std::string> FlatNameProblem(std::string_view name)
{
  if (name.empty())
    return "an empty name";
  if (name.size() > MAX_PATH_BYTES)
    return fmt::format("longer than {} bytes", MAX_PATH_BYTES);
  // The YouGame app's runtime file names (desktop orca-policy.ts FILE_RE), which are what lands in
  // the folder.
  const auto plain = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
           c == ' ' || c == '(' || c == ')' || c == '_' || c == '-';
  };
  if (!plain(name.front()) ||
      !std::all_of(name.begin() + 1, name.end(), [&](char c) { return plain(c) || c == '.'; }))
  {
    return "a character other than A-Z a-z 0-9 + ( ) _ - space, and '.' after the first";
  }
  if (name.find("..") != std::string_view::npos)
    return "\"..\"";
  if (name.front() == ' ')
    return "a name that starts with a space";
  // A Windows device name, or a name ending in a dot or a space.
  if (const auto problem = PathProblem(name))
    return problem;
  const std::string lower = ToLowerAscii(name);
  if (lower == FLAT_MANIFEST || lower == FLAT_SIGNATURE || lower == "tree.json")
    return "one of the bundle's own names (mod.json, mod.sig, tree.json)";
  return std::nullopt;
}

bool HostOnly(std::string_view path)
{
  return path == "orca/jitwarm" || path.starts_with("orca/test/");
}

std::string Sha256Hex(std::string_view data)
{
  std::array<u8, 32> digest{};
  mbedtls_sha256_ret(reinterpret_cast<const u8*>(data.data()), data.size(), digest.data(), 0);
  std::string hex;
  hex.reserve(64);
  for (const u8 b : digest)
    hex += fmt::format("{:02x}", b);
  return hex;
}

std::string TreeHash(std::span<const TreeFile> files)
{
  std::string text;
  for (const TreeFile& f : files)
  {
    if (!HostOnly(f.path))
      text += fmt::format("{}\t{}\t{}\n", f.path, f.sha256, f.bytes);
  }
  return Sha256Hex(text);
}

std::string SdTreeHash(const Tree& tree, u64 bytes, u32 cluster_bytes, u8 fats)
{
  std::string text = fmt::format("bytes={} cluster={} fats={}\n", bytes, cluster_bytes, fats);
  for (const TreeFile* f : tree.Under("sd/"))
    text += fmt::format("{}\t{}\t{}\n", f->path, f->sha256, f->bytes);
  return Sha256Hex(text);
}

std::optional<Tree> ParseTree(std::string_view json, std::string* error)
{
  const auto fail = [error](std::string why) {
    *error = "tree.json: " + std::move(why);
    return std::nullopt;
  };
  picojson::value root;
  const std::string parse_error = ParseWhole(&root, json, MAX_TREE_JSON_BYTES);
  if (!parse_error.empty())
    return fail(parse_error);
  if (!root.is<picojson::object>())
    return fail("not an object");
  const picojson::object& o = root.get<picojson::object>();
  if (const auto key = UnknownKey(o, {"tree", "files"}))
    return fail(fmt::format("unknown key {}", *key));
  if (!o.contains("tree") || !o.at("tree").is<std::string>() ||
      !IsLowerHex(o.at("tree").get<std::string>(), 64))
  {
    return fail("\"tree\" must be 64 lowercase hex digits");
  }
  if (!o.contains("files") || !o.at("files").is<picojson::array>())
    return fail("\"files\" must be an array");
  const picojson::array& list = o.at("files").get<picojson::array>();
  if (list.size() > MAX_FILES)
    return fail(fmt::format("more than {} files", MAX_FILES));

  Tree tree;
  tree.hash = o.at("tree").get<std::string>();
  std::set<std::string> lower;
  for (const picojson::value& item : list)
  {
    if (!item.is<picojson::object>())
      return fail("a file that is not an object");
    const picojson::object& f = item.get<picojson::object>();
    if (const auto key = UnknownKey(f, {"path", "sha256", "bytes"}))
      return fail(fmt::format("unknown key {} in a file", *key));
    if (!f.contains("path") || !f.at("path").is<std::string>())
      return fail("a file without a path");
    TreeFile file;
    file.path = f.at("path").get<std::string>();
    if (const auto problem = PathProblem(file.path))
      return fail(fmt::format("{}: {}", file.path, *problem));
    if (file.path == "tree.json")
      return fail("tree.json lists itself");
    if (!f.contains("sha256") || !f.at("sha256").is<std::string>() ||
        !IsLowerHex(f.at("sha256").get<std::string>(), 64))
    {
      return fail(fmt::format("{}: sha256 must be 64 lowercase hex digits", file.path));
    }
    file.sha256 = f.at("sha256").get<std::string>();
    const auto bytes = f.contains("bytes") ? Integer(f.at("bytes"), 0, MAX_FILE_BYTES) : std::nullopt;
    if (!bytes)
      return fail(fmt::format("{}: bytes must be an integer from 0 to {}", file.path, MAX_FILE_BYTES));
    file.bytes = *bytes;
    if (!tree.files.empty() && !(tree.files.back().path < file.path))
      return fail(fmt::format("{} is out of order or listed twice (sorted by path)", file.path));
    if (!lower.insert(ToLowerAscii(file.path)).second)
      return fail(fmt::format("{} differs from another path only in case", file.path));
    tree.files.push_back(std::move(file));
  }
  // Never both a file and a folder (also without case).
  for (const TreeFile& f : tree.files)
  {
    const std::string l = ToLowerAscii(f.path);
    for (size_t slash = l.find('/'); slash != std::string::npos; slash = l.find('/', slash + 1))
    {
      if (lower.contains(l.substr(0, slash)))
        return fail(fmt::format("{} is inside a path that is also a file", f.path));
    }
  }
  if (TreeHash(tree.files) != tree.hash)
    return fail("\"tree\" is not the hash of the files listed");
  return tree;
}

// --- mod.json ----------------------------------------------------------------------------------

std::optional<Builtin> BuiltinFromName(std::string_view name)
{
  if (name == "brawl-rev2")
    return Builtin::BrawlRev2;
  if (name == "brawl-rev2-pplus")
    return Builtin::BrawlRev2PPlus;
  if (name == "brawl-rev2-results")
    return Builtin::BrawlRev2Results;
  return std::nullopt;
}

std::string_view BuiltinName(Builtin builtin)
{
  switch (builtin)
  {
  case Builtin::BrawlRev2:
    return "brawl-rev2";
  case Builtin::BrawlRev2PPlus:
    return "brawl-rev2-pplus";
  case Builtin::BrawlRev2Results:
    return "brawl-rev2-results";
  default:
    return "none";
  }
}

std::span<const std::string_view> ForcedAllowlist()
{
  return FORCED_ALLOWLIST;
}

std::span<const std::string_view> OfferableCaps()
{
  return OFFERABLE_CAPS;
}

u64 DiscTitleId(std::string_view disc_id)
{
  u64 code = 0;
  for (size_t i = 0; i < 4 && i < disc_id.size(); ++i)
    code = (code << 8) | static_cast<u8>(disc_id[i]);
  return (u64{0x00010000} << 32) | code;
}

std::optional<Manifest> ParseManifest(std::string_view json, std::string* error)
{
  const auto fail = [error](std::string why) {
    *error = "mod.json: " + std::move(why);
    return std::nullopt;
  };
  picojson::value root;
  const std::string parse_error = ParseWhole(&root, json, MAX_MOD_JSON_BYTES);
  if (!parse_error.empty())
    return fail(parse_error);
  if (!root.is<picojson::object>())
    return fail("not an object");
  const picojson::object& o = root.get<picojson::object>();
  const auto get = [&o](const char* key) -> const picojson::value* {
    const auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
  };

  // A pack for a newer YG Dolphin says so before anything else it holds that this build may not
  // know (keys, builtins, limits): the player's message, as the session's error.
  if (const picojson::value* v = get("engine"); v && v->is<picojson::object>())
  {
    const picojson::object& e = v->get<picojson::object>();
    if (const auto it = e.find("minOrca"); it != e.end() && it->second.is<std::string>() &&
                                           IsVersion(it->second.get<std::string>()) &&
                                           !VersionAtLeast(EngineVersion(), it->second.get<std::string>()))
    {
      *error = fmt::format(fmt::runtime(UPDATE_MESSAGE), it->second.get<std::string>());
      return std::nullopt;
    }
  }

  Manifest m;
  // format: 1, a tree bundle (ORCA_MOD_DIR with tree.json); 2, a flat bundle (Bundle.h). It decides
  // which keys there may be.
  {
    const picojson::value* v = get("format");
    const auto format = v ? Integer(*v, 1, 2) : std::nullopt;
    if (!format)
      return fail("\"format\" must be 1 (a tree bundle) or 2 (a flat bundle)");
    m.format = static_cast<int>(*format);
  }
  const bool flat = m.format == 2;
  const auto unknown =
      flat ? UnknownKey(o, {"format", "engine", "listing", "serial", "title", "disc", "launcher",
                            "sdImage", "patches", "jitwarm", "textures", "frameHook",
                            "bootNandFrame", "firstKeyframeFrame", "jitClearFrame", "keepGameCode",
                            "saveTitle", "forced", "origin", "builtin", "pplusGuards",
                            "rankedRules", "rulesets", "offers"}) :
             UnknownKey(o, {"format", "engine", "title", "disc", "launcher", "sd", "frameHook",
                            "bootNandFrame", "firstKeyframeFrame", "jitClearFrame", "keepGameCode",
                            "saveTitle", "forced", "origin", "builtin", "rankedRules", "rulesets",
                            "offers"});
  if (unknown)
    return fail(fmt::format("unknown key {} (format {})", *unknown, m.format));

  // engine
  {
    const picojson::value* v = get("engine");
    if (!v || !v->is<picojson::object>())
      return fail("\"engine\" must be an object");
    const picojson::object& e = v->get<picojson::object>();
    if (const auto key = UnknownKey(e, {"api", "minOrca"}))
      return fail(fmt::format("unknown key engine.{}", *key));
    const auto api = e.contains("api") ? Integer(e.at("api"), 1, 1000) : std::nullopt;
    if (!api)
      return fail("engine.api must be an integer from 1");
    if (*api > static_cast<u64>(ENGINE_API))
    {
      return fail(fmt::format("this mod needs engine API {}; this Orca has {}. Update Orca.", *api,
                              ENGINE_API));
    }
    m.engine_api = static_cast<int>(*api);
    if (!e.contains("minOrca") || !e.at("minOrca").is<std::string>() ||
        !IsVersion(e.at("minOrca").get<std::string>()))
    {
      return fail("engine.minOrca must be a version (major.minor.patch)");
    }
    m.min_orca = e.at("minOrca").get<std::string>();
  }

  // title
  if (const picojson::value* v = get("title"))
  {
    if (!v->is<std::string>())
      return fail("\"title\" must be a string");
    const std::string& t = v->get<std::string>();
    if (t.empty() || t.size() > 48 ||
        !std::ranges::all_of(t, [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
    {
      return fail("\"title\" must be 1-48 printable ASCII characters");
    }
    m.title = t;
  }

  // disc
  {
    const picojson::value* v = get("disc");
    if (!v || !v->is<picojson::object>())
      return fail("\"disc\" must be an object");
    const picojson::object& d = v->get<picojson::object>();
    if (const auto key = UnknownKey(d, {"id", "revision"}))
      return fail(fmt::format("unknown key disc.{}", *key));
    if (!d.contains("id") || !d.at("id").is<std::string>())
      return fail("disc.id must be a game ID");
    const std::string& id = d.at("id").get<std::string>();
    if (id.size() != 6 || !std::ranges::all_of(id, [](unsigned char c) {
          return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }))
    {
      return fail("disc.id must be 6 characters A-Z 0-9");
    }
    const auto revision = d.contains("revision") ? Integer(d.at("revision"), 0, 255) : std::nullopt;
    if (!revision)
      return fail("disc.revision must be an integer from 0 to 255");
    m.disc_id = id;
    m.disc_revision = static_cast<u16>(*revision);
  }

  // launcher
  if (const picojson::value* v = get("launcher"))
  {
    if (!v->is<std::string>())
      return fail("\"launcher\" must be a file name");
    const std::string& l = v->get<std::string>();
    if (flat)
    {
      if (const auto problem = FlatNameProblem(l))
        return fail(fmt::format("\"launcher\" must be a file name of the bundle: {}", *problem));
    }
    else if (PathProblem(l) || l.find('/') != std::string::npos || l == "mod.json" ||
             l == "tree.json")
    {
      return fail("\"launcher\" must be a file name at the bundle's root");
    }
    const std::string lower = ToLowerAscii(l);
    if (!lower.ends_with(".dol") && !lower.ends_with(".elf"))
      return fail("\"launcher\" must be a .dol or .elf");
    m.launcher = l;
  }

  // sd
  if (const picojson::value* v = get("sd"))
  {
    if (!v->is<picojson::object>())
      return fail("\"sd\" must be an object");
    const picojson::object& s = v->get<picojson::object>();
    if (const auto key = UnknownKey(s, {"bytes", "clusterBytes", "fats"}))
      return fail(fmt::format("unknown key sd.{}", *key));
    const auto bytes =
        s.contains("bytes") ? Integer(s.at("bytes"), SD_MIN_BYTES, SD_MAX_BYTES) : std::nullopt;
    if (!bytes || *bytes % SD_ALIGN != 0)
    {
      return fail(fmt::format("sd.bytes must be a multiple of {} from {} to {} (an SDSC card)",
                              SD_ALIGN, SD_MIN_BYTES, SD_MAX_BYTES));
    }
    m.sd_bytes = *bytes;
    // The volume's geometry, for a card that must read as another (P+'s official image: 2 KiB
    // clusters, two FATs). Absent: FatFs's cluster for the size, one FAT.
    if (s.contains("clusterBytes"))
    {
      const auto cluster = Integer(s.at("clusterBytes"), 512, 32768);
      if (!cluster || (*cluster & (*cluster - 1)) != 0)
        return fail("sd.clusterBytes must be a power of two from 512 to 32768");
      m.sd_cluster_bytes = static_cast<u32>(*cluster);
    }
    if (s.contains("fats"))
    {
      const auto fats = Integer(s.at("fats"), 1, 2);
      if (!fats)
        return fail("sd.fats must be 1 or 2");
      m.sd_fats = static_cast<u8>(*fats);
    }
    // FAT32 needs more than 65,525 clusters (FatFs's f_mkfs, as it lays the volume out).
    if (m.sd_cluster_bytes != 0)
    {
      const u64 sectors = *bytes / 512, per_cluster = m.sd_cluster_bytes / 512;
      const u64 fat_sectors = ((sectors / per_cluster) * 4 + 8 + 511) / 512;
      if ((sectors - 32 - fat_sectors * m.sd_fats) / per_cluster <= 65525)
        return fail("sd.clusterBytes is too large for sd.bytes (FAT32 needs over 65,525 clusters)");
    }
  }

  // frameHook
  {
    const picojson::value* v = get("frameHook");
    if (!v || !v->is<picojson::object>())
      return fail("\"frameHook\" must be an object");
    const picojson::object& h = v->get<picojson::object>();
    if (const auto key = UnknownKey(h, {"address", "word"}))
      return fail(fmt::format("unknown key frameHook.{}", *key));
    const auto address = h.contains("address") ? HexWord(h.at("address")) : std::nullopt;
    const auto word = h.contains("word") ? HexWord(h.at("word")) : std::nullopt;
    if (!address || !InMem1Word(*address))
      return fail("frameHook.address must be a hex word address in MEM1 (\"0x80017504\")");
    if (!word)
      return fail("frameHook.word must be a hex word (\"0x90170100\")");
    m.frame_hook = *address;
    m.frame_hook_word = *word;
  }

  // Frames
  const auto frame = [&](const char* key, u64 min, u64 max) -> std::optional<std::optional<u32>> {
    const picojson::value* v = get(key);
    if (!v)
      return std::optional<u32>{};
    const auto n = Integer(*v, min, max);
    if (!n)
      return std::nullopt;
    return std::optional<u32>(static_cast<u32>(*n));
  };
  const auto boot_nand = frame("bootNandFrame", 1, 1'000'000);
  if (!boot_nand)
    return fail("bootNandFrame must be an integer from 1 to 1000000");
  m.boot_nand_frame = *boot_nand;
  // Not below today's 300: a boot's first frames can run differently on a machine whose JIT holds
  // other blocks (Project+'s 3-9; Rollback/OnlineMatch.cpp FIRST_KEYFRAME_FRAME), and a joiner's
  // always does.
  const auto first_keyframe = frame("firstKeyframeFrame", DEFAULT_FIRST_KEYFRAME_FRAME, 36'000);
  if (!first_keyframe)
    return fail("firstKeyframeFrame must be an integer from 300 to 36000");
  m.first_keyframe_frame = first_keyframe->value_or(DEFAULT_FIRST_KEYFRAME_FRAME);
  const auto jit_clear = frame("jitClearFrame", 0, 1'000'000);
  if (!jit_clear)
    return fail("jitClearFrame must be an integer from 0 to 1000000");
  m.jit_clear_frame = *jit_clear;

  // origin: a start point declared by the pack, verified from local RAM at boot.
  if (const picojson::value* v = get("origin"))
  {
    if (!v->is<picojson::object>())
      return fail("origin must be an object");
    const auto& origin_obj = v->get<picojson::object>();
    if (const auto key = UnknownKey(origin_obj, {"frame", "words", "settle"}))
      return fail(fmt::format("unknown key origin.{}", *key));
    const auto n =
        origin_obj.contains("frame") ? Integer(origin_obj.at("frame"), 1, 36'000) : std::nullopt;
    if (!n)
      return fail("origin.frame must be an integer from 1 to 36000");
    OriginDecl decl;
    decl.frame = static_cast<u32>(*n);
    if (origin_obj.contains("words"))
    {
      const auto& words = origin_obj.at("words");
      if (!words.is<picojson::array>() || words.get<picojson::array>().size() > 8)
        return fail("origin.words must be an array of at most 8 words");
      for (const auto& item : words.get<picojson::array>())
      {
        if (!item.is<picojson::object>())
          return fail("origin.words entries must be objects");
        const auto& w = item.get<picojson::object>();
        if (const auto key = UnknownKey(w, {"address", "word"}))
          return fail(fmt::format("unknown key origin.words.{}", *key));
        const auto address = w.contains("address") ? HexWord(w.at("address")) : std::nullopt;
        const auto word = w.contains("word") ? HexWord(w.at("word")) : std::nullopt;
        if (!address || !(InMem1Word(*address) ||
                         (*address >= 0x90000000 && *address <= 0x93FFFFFC && *address % 4 == 0)))
          return fail("origin.words.address must be a word address in MEM1 or MEM2");
        if (!word)
          return fail("origin.words.word must be a hex word");
        if (std::ranges::any_of(decl.words, [&](const OriginWord& w) {
              return w.address == *address;
            }))
          return fail("origin.words repeats an address");
        decl.words.push_back({*address, *word});
      }
    }
    if (origin_obj.contains("settle"))
    {
      if (!origin_obj.contains("words"))
        return fail("origin.settle requires origin.words");
      const auto settle = Integer(origin_obj.at("settle"), 1, 600);
      if (!settle)
        return fail("origin.settle must be an integer from 1 to 600");
      decl.settle = static_cast<u32>(*settle);
    }
    if ((m.jit_clear_frame && decl.frame <= *m.jit_clear_frame) ||
        (m.boot_nand_frame && decl.frame <= *m.boot_nand_frame))
      return fail("origin.frame must follow jitClearFrame and bootNandFrame");
    m.origin = std::move(decl);
  }

  // keepGameCode
  if (const picojson::value* v = get("keepGameCode"))
  {
    if (!v->is<picojson::array>() || v->get<picojson::array>().size() > 64)
      return fail("\"keepGameCode\" must be an array of at most 64 entries");
    for (const picojson::value& item : v->get<picojson::array>())
    {
      const auto* triple = item.is<picojson::array>() ? &item.get<picojson::array>() : nullptr;
      const auto address = triple && triple->size() == 3 ? HexWord((*triple)[0]) : std::nullopt;
      const auto game = triple && triple->size() == 3 ? HexWord((*triple)[1]) : std::nullopt;
      const auto loader = triple && triple->size() == 3 ? HexWord((*triple)[2]) : std::nullopt;
      if (!address || !game || !loader)
        return fail("keepGameCode entries must be [address, the game's word, the mod's word], hex");
      if (!InMem1Word(*address))
        return fail("keepGameCode's address must be a word in MEM1 (0x80000000-0x817FFFFC)");
      if (*game == *loader)
        return fail("keepGameCode's two words are the same");
      if (std::ranges::any_of(m.keep_game_code,
                              [&](const KeptWord& k) { return k.address == *address; }))
      {
        return fail(fmt::format("keepGameCode {:#010x} twice", *address));
      }
      m.keep_game_code.push_back({*address, *game, *loader});
    }
  }

  // saveTitle: only the disc's own.
  if (const picojson::value* v = get("saveTitle"))
  {
    u64 title = 0;
    if (!v->is<std::string>() || !v->get<std::string>().starts_with("0x") ||
        v->get<std::string>().size() != 18 ||
        !std::all_of(v->get<std::string>().begin() + 2, v->get<std::string>().end(),
                     [](unsigned char c) { return std::isxdigit(c); }) ||
        !TryParse(v->get<std::string>().substr(2), &title, 16))
    {
      return fail("saveTitle must be a 16-digit hex title ID (\"0x0001000052534245\")");
    }
    if (title != DiscTitleId(m.disc_id))
      return fail("saveTitle may name only the disc's own title");
    m.save_title = title;
  }

  // forced
  if (const picojson::value* v = get("forced"))
  {
    if (!v->is<picojson::object>())
      return fail("\"forced\" must be an object");
    for (const auto& [key, value] : v->get<picojson::object>())
    {
      if (std::ranges::find(FORCED_ALLOWLIST, key) == FORCED_ALLOWLIST.end())
        return fail(fmt::format("forced.{} is not a setting a mod may force", key));
      // Every allowed key so far is a 32-bit unsigned integer.
      const auto n = Integer(value, 0, 0xFFFFFFFFu);
      if (!n)
        return fail(fmt::format("forced.{} must be an integer from 0 to 4294967295", key));
      m.forced.emplace_back(key, fmt::format("{}", *n));
    }
  }

  // builtin
  if (const picojson::value* v = get("builtin"))
  {
    const auto builtin = v->is<std::string>() ? BuiltinFromName(v->get<std::string>()) : std::nullopt;
    if (!builtin)
      return fail("\"builtin\" must be \"brawl-rev2\", \"brawl-rev2-pplus\" or "
                  "\"brawl-rev2-results\"");
    if (m.disc_id != "RSBE01" || m.disc_revision != 2)
      return fail(fmt::format("builtin {} runs on RSBE01 revision 2 only", BuiltinName(*builtin)));
    m.builtin = *builtin;
  }

  // Only flat packs may relocate the three pairs; their instruction words stay engine constants.
  if (const picojson::value* v = get("pplusGuards"))
  {
    if (m.builtin != Builtin::BrawlRev2PPlus)
      return fail("pplusGuards requires builtin brawl-rev2-pplus");
    if (!v->is<picojson::object>())
      return fail("pplusGuards must be an object");
    const auto& addresses = v->get<picojson::object>();
    if (const auto key = UnknownKey(addresses, {"rssExdata", "pageIndex", "currentPage"}))
      return fail(fmt::format("unknown key pplusGuards.{}", *key));
    std::array<u32, 3> pairs{};
    size_t index = 0;
    for (const char* key : {"rssExdata", "pageIndex", "currentPage"})
    {
      const auto address = addresses.contains(key) ? HexWord(addresses.at(key)) : std::nullopt;
      if (!address || !InMem1Word(*address) || *address < PPLUS_CODESET_REGION.first ||
          *address > PPLUS_CODESET_REGION.second - 8)
      {
        return fail(fmt::format("pplusGuards.{} must start an aligned 8-byte pair in "
                                "0x80550010-0x8059FFFF", key));
      }
      for (size_t previous = 0; previous < index; ++previous)
      {
        if (*address < pairs[previous] + 8 && pairs[previous] < *address + 8)
          return fail("pplusGuards pairs must not overlap");
      }
      pairs[index++] = *address;
    }
    m.pplus_guards = PPlusGuardAddresses{pairs[0], pairs[1], pairs[2]};
  }

  if (const picojson::value* v = get("rankedRules"))
  {
    if (m.builtin != Builtin::BrawlRev2Results)
      return fail("rankedRules requires builtin brawl-rev2-results");
    if (!v->is<picojson::object>())
      return fail("rankedRules must be an object");
    const auto& rules = v->get<picojson::object>();
    if (const auto key = UnknownKey(rules, {"stocks", "timeMinutes", "items"}))
      return fail(fmt::format("unknown key rankedRules.{}", *key));
    const auto stocks =
        rules.contains("stocks") ? Integer(rules.at("stocks"), 1, 99) : std::nullopt;
    const auto minutes =
        rules.contains("timeMinutes") ? Integer(rules.at("timeMinutes"), 1, 99) : std::nullopt;
    if (!stocks || !minutes)
      return fail("rankedRules.stocks and timeMinutes must be integers from 1 to 99");
    if (!rules.contains("items") || !rules.at("items").is<std::string>() ||
        rules.at("items").get<std::string>() != "off")
      return fail("rankedRules.items must be \"off\"");
    m.ranked_rules = RankedRules{static_cast<u8>(*stocks), static_cast<u8>(*minutes)};
  }

  if (m.origin && m.builtin != Builtin::None &&
      m.origin->frame < m.first_keyframe_frame)
    return fail("origin.frame must be at least firstKeyframeFrame with a builtin");

  // rulesets
  if (const picojson::value* v = get("rulesets"))
  {
    if (!v->is<picojson::array>() || v->get<picojson::array>().size() > 16)
      return fail("\"rulesets\" must be an array of at most 16 names");
    for (const picojson::value& item : v->get<picojson::array>())
    {
      const std::string* name = item.is<std::string>() ? &item.get<std::string>() : nullptr;
      if (!name || name->empty() || name->size() > 32 || (*name)[0] == '-' ||
          !std::ranges::all_of(*name, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
          }))
      {
        return fail("rulesets must be names of 1-32 characters a-z 0-9 -");
      }
      if (std::ranges::find(m.rulesets, *name) != m.rulesets.end())
        return fail(fmt::format("ruleset {} twice", *name));
      m.rulesets.push_back(*name);
    }
  }

  // offers
  if (const picojson::value* v = get("offers"))
  {
    if (!v->is<picojson::array>())
      return fail("\"offers\" must be an array");
    for (const picojson::value& item : v->get<picojson::array>())
    {
      const std::string* cap = item.is<std::string>() ? &item.get<std::string>() : nullptr;
      if (!cap || std::ranges::find(OFFERABLE_CAPS, *cap) == OFFERABLE_CAPS.end())
        return fail("offers must be among \"results\", \"locks\", \"queue2\"");
      if (m.builtin == Builtin::BrawlRev2Results && *cap != "results")
        return fail("builtin brawl-rev2-results may offer only \"results\"");
      if (std::ranges::find(m.offers, *cap) != m.offers.end())
        return fail(fmt::format("offer {} twice", *cap));
      m.offers.push_back(*cap);
    }
    if (!m.offers.empty() && m.builtin == Builtin::None)
      return fail("a mod with no builtin has no logic to offer caps with");
  }
  // Without declared rules a results-only game would count whatever Brawl's rules menu holds (its
  // defaults: a 2-minute timed match, items on): it reads no results, so it offers none.
  if (m.builtin == Builtin::BrawlRev2Results && !m.ranked_rules &&
      std::ranges::find(m.offers, "results") != m.offers.end())
  {
    return fail("builtin brawl-rev2-results offers \"results\" only with \"rankedRules\"");
  }

  if (!flat)
    return m;

  // Format 2. listing: the YouGame listing the bundle is for (desktop orca-policy.ts SLUG_RE).
  {
    const picojson::value* v = get("listing");
    const auto slug = [](const std::string& l) {
      return !l.empty() && l.size() <= 80 && std::ranges::all_of(l, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      });
    };
    if (!v || !v->is<std::string>() || !slug(v->get<std::string>()))
      return fail("\"listing\" must be a listing's slug (a-z 0-9 -, at most 80)");
    m.listing = v->get<std::string>();
  }
  // serial: the bundle's number, raised with every bundle signed for the listing.
  {
    const picojson::value* v = get("serial");
    const auto serial = v ? Integer(*v, 1, u64{1} << 53) : std::nullopt;
    if (!serial)
      return fail("\"serial\" must be an integer from 1");
    m.serial = *serial;
  }
  // Every file a name at the bundle's root.
  // sdImage: the card used as it is, pinned by size and chunked SHA-256 (Bundle.h SD_HASH_HEADER).
  if (const picojson::value* v = get("sdImage"))
  {
    if (!v->is<picojson::object>())
      return fail("\"sdImage\" must be an object");
    const picojson::object& s = v->get<picojson::object>();
    if (const auto key = UnknownKey(s, {"file", "bytes", "sha256"}))
      return fail(fmt::format("unknown key sdImage.{}", *key));
    SdImageFile image;
    if (!s.contains("file") || !s.at("file").is<std::string>())
      return fail("sdImage.file must be a file name");
    image.file = s.at("file").get<std::string>();
    if (const auto problem = FlatNameProblem(image.file))
      return fail(fmt::format("sdImage.file: {}", *problem));
    const auto bytes = s.contains("bytes") ? Integer(s.at("bytes"), 1, SD_MAX_BYTES) : std::nullopt;
    if (!bytes)
      return fail(fmt::format("sdImage.bytes must be an integer from 1 to {} (an SDSC card)",
                              SD_MAX_BYTES));
    image.bytes = *bytes;
    if (!s.contains("sha256") || !s.at("sha256").is<std::string>() ||
        !IsLowerHex(s.at("sha256").get<std::string>(), 64))
    {
      return fail("sdImage.sha256 must be 64 lowercase hex digits (its chunked SHA-256)");
    }
    image.sha256 = s.at("sha256").get<std::string>();
    m.sd_image = std::move(image);
  }
  // patches: the game patches, applied in this order.
  if (const picojson::value* v = get("patches"))
  {
    if (!v->is<picojson::array>() || v->get<picojson::array>().size() > FLAT_MAX_PATCHES)
    {
      return fail(
          fmt::format("\"patches\" must be an array of at most {} file names", FLAT_MAX_PATCHES));
    }
    for (const picojson::value& item : v->get<picojson::array>())
    {
      if (!item.is<std::string>())
        return fail("\"patches\" must name files");
      const std::string& name = item.get<std::string>();
      if (const auto problem = FlatNameProblem(name))
        return fail(fmt::format("patches: {}: {}", name, *problem));
      if (!name.ends_with(".patches"))
        return fail(fmt::format("patches: {} must end in .patches", name));
      m.patches.push_back(name);
    }
  }
  // jitwarm: the JIT seed (host-only).
  if (const picojson::value* v = get("jitwarm"))
  {
    if (!v->is<std::string>())
      return fail("\"jitwarm\" must be a file name");
    if (const auto problem = FlatNameProblem(v->get<std::string>()))
      return fail(fmt::format("jitwarm: {}", *problem));
    m.jitwarm = v->get<std::string>();
  }
  // textures: the engine's own pack, the only kind so far.
  if (const picojson::value* v = get("textures"); v && (!v->is<std::string>() || v->get<std::string>() != "engine"))
    return fail("\"textures\" must be \"engine\" (a flat bundle can't carry a texture pack yet)");
  // Each file once (also without case: Windows and macOS folders), and few enough to be a listing's.
  const std::vector<std::string> files = m.FlatFiles();
  std::set<std::string> lower;
  for (const std::string& file : files)
  {
    if (!lower.insert(ToLowerAscii(file)).second)
      return fail(fmt::format("{} is named twice", file));
  }
  if (files.size() + 2 > FLAT_MAX_FILES)
  {
    return fail(fmt::format("a flat bundle has at most {} files, mod.json and mod.sig included",
                            FLAT_MAX_FILES));
  }
  return m;
}

std::vector<std::string> Manifest::FlatFiles() const
{
  std::vector<std::string> files;
  if (!launcher.empty())
    files.push_back(launcher);
  if (sd_image)
    files.push_back(sd_image->file);
  files.insert(files.end(), patches.begin(), patches.end());
  if (!jitwarm.empty())
    files.push_back(jitwarm);
  return files;
}

std::string ModInfo::JitWarmPath() const
{
  if (flat)
    return manifest.jitwarm.empty() ? std::string{} : dir + manifest.jitwarm;
  return tree.Find("orca/jitwarm") ? dir + "orca/jitwarm" : std::string{};
}

// --- The session -------------------------------------------------------------------------------

bool Requested()
{
  if (!SessionActive())
    return false;
  if (const std::optional<BundleKind> kind = ResolvedKind())
    return *kind != BundleKind::None;
  return !GetEnv("ORCA_MOD_DIR").empty();
}

std::string Dir()
{
  if (ResolvedKind())
    return ResolvedDir();
  std::string dir = GetEnv("ORCA_MOD_DIR");
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  return dir;
}

const ModInfo* ActiveMod()
{
  const Profile* profile = ActiveProfile();
  return profile && profile->mod ? &*profile->mod : nullptr;
}

u32 FirstKeyframeFrame()
{
  const ModInfo* mod = ActiveMod();
  return mod ? mod->manifest.first_keyframe_frame : DEFAULT_FIRST_KEYFRAME_FRAME;
}

namespace
{
// A name for this process's temporary files (host-only: never reaches the game).
u64 ProcessId()
{
  static const u64 id = [] {
    std::random_device device;
    return (u64{device()} << 32) | device();
  }();
  return id;
}

// Keeps the newest `keep` cached images (by mtime) in `dir`, and drops the rest with their notes;
// also what a build killed midway left (a .part over an hour old) and notes without an image.
void PruneSdCache(const std::string& dir, size_t keep)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, fs::path>> images;
  std::vector<fs::path> notes;
  const auto stale = fs::file_time_type::clock::now() - std::chrono::hours(1);
  for (const auto& entry : fs::directory_iterator(StringToPath(dir), ec))
  {
    const std::string name = PathToString(entry.path().filename());
    // <64 hex>-<builder>.raw[.xxh3][.<pid>.part]
    if (name.size() < 70 || !IsLowerHex(std::string_view(name).substr(0, 64), 64) ||
        name[64] != '-')
    {
      continue;
    }
    if (name.ends_with(".part") || name.ends_with(".xxx"))
    {
      if (entry.last_write_time(ec) < stale)
        fs::remove(entry.path(), ec);
      continue;
    }
    if (name.ends_with(".raw.xxh3"))
      notes.push_back(entry.path());
    if (!name.ends_with(".raw"))
      continue;
    images.emplace_back(entry.last_write_time(ec), entry.path());
  }
  for (const fs::path& note : notes)
  {
    fs::path image = note;
    image.replace_extension();
    if (!fs::exists(image, ec))
      fs::remove(note, ec);
  }
  std::ranges::sort(images, std::greater{});
  for (size_t i = keep; i < images.size(); ++i)
  {
    fs::remove(images[i].second, ec);
    fs::path note = images[i].second;
    note += ".xxh3";
    fs::remove(note, ec);
    NOTICE_LOG_FMT(BOOT, "Orca mod: dropped the cached SD image {}", PathToString(images[i].second));
  }
}
}  // namespace

bool BuildSdImage(std::span<const SdSource> files, u64 bytes, const std::string& out_path,
                  std::string* error, u32 cluster_bytes, u8 fats)
{
  std::vector<Common::SDImageFile> list;
  list.reserve(files.size());
  for (const SdSource& f : files)
    list.push_back({f.image_path, f.source_path, f.bytes});
  return Common::BuildSDImageFromFiles(list, bytes, out_path, error, cluster_bytes, fats);
}

std::optional<SdImage> PrepareSdImage(const ModInfo& mod, const std::string& cache_dir,
                                      std::string* error)
{
  if (!mod.manifest.sd_bytes)
  {
    *error = "Orca mod: this mod has no SD card";
    return std::nullopt;
  }
  if (cache_dir.empty())
  {
    *error = "Orca mod: a mod with an SD card needs ORCA_SD_CACHE (where its image is built)";
    return std::nullopt;
  }
  const u64 bytes = *mod.manifest.sd_bytes;
  std::string dir = cache_dir;
  if (dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  if (!File::CreateFullPath(dir))
  {
    *error = fmt::format("Orca mod: can't make the SD cache folder {}", dir);
    return std::nullopt;
  }
  SdImage image;
  const u32 cluster = mod.manifest.sd_cluster_bytes;
  const u8 fats = mod.manifest.sd_fats;
  image.path =
      fmt::format("{}{}-{}.raw", dir, SdTreeHash(mod.tree, bytes, cluster, fats), SD_BUILDER);
  const std::string note = image.path + ".xxh3";

  // A cached image is reused while it hashes as when it was built (a damaged one is built again).
  if (File::Exists(image.path) && File::GetSize(image.path) == bytes)
  {
    std::string recorded;
    File::ReadFileToString(note, recorded);
    const std::string hash = HashFile(image.path);
    if (!hash.empty() && hash == StripWhitespace(recorded))
      image.hash = hash;
    else
      NOTICE_LOG_FMT(BOOT, "Orca mod: the cached SD image {} is damaged: building it again",
                     image.path);
  }
  if (image.hash.empty())
  {
    std::vector<SdSource> files;
    for (const TreeFile* f : mod.tree.Under("sd/"))
    {
      files.push_back({f->path.substr(3), mod.dir + f->path, f->bytes});
      // The tree's sizes were checked at load; checked again here, as the bytes go in.
      if (File::GetSize(files.back().source_path) != f->bytes)
      {
        *error = fmt::format("Orca mod: {} is not the size tree.json names", f->path);
        return std::nullopt;
      }
    }
    const auto start = std::chrono::steady_clock::now();
    std::string why;
    File::Delete(note, File::IfAbsentBehavior::NoConsoleWarning);
    // Built under this process's own name (two Orcas sharing the cache may build the same card),
    // then hashed and moved into place, its hash recorded last.
    const std::string building = fmt::format("{}.{:016x}.part", image.path, ProcessId());
    if (!BuildSdImage(files, bytes, building, &why, cluster, fats))
    {
      File::Delete(building, File::IfAbsentBehavior::NoConsoleWarning);
      *error = fmt::format("Orca mod: couldn't build the SD card: {}", why);
      return std::nullopt;
    }
    image.hash = HashFile(building);
    const std::string note_part = fmt::format("{}.{:016x}.part", note, ProcessId());
    if (image.hash.empty() || !File::Rename(building, image.path) ||
        !File::WriteStringToFile(note_part, image.hash) || !File::Rename(note_part, note))
    {
      File::Delete(building, File::IfAbsentBehavior::NoConsoleWarning);
      File::Delete(note_part, File::IfAbsentBehavior::NoConsoleWarning);
      *error = fmt::format("Orca mod: couldn't hash or record the SD image {}", image.path);
      return std::nullopt;
    }
    image.built = true;
    NOTICE_LOG_FMT(BOOT, "Orca mod: built the SD card ({} files, {} bytes) in {} ms: {}",
                   files.size(), bytes,
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count(),
                   image.path);
  }
  // Newest first by use: this one is the newest.
  std::error_code ec;
  std::filesystem::last_write_time(StringToPath(image.path),
                                   std::filesystem::file_time_type::clock::now(), ec);
  PruneSdCache(dir, 2);
  return image;
}

// --- Builtins and their guards -----------------------------------------------------------------

namespace
{
// Brawl rev 2's executable (main.dol, NTSC-U), as every builtin module reads it: the frame boundary,
// the small-data base (r13) and the code that loads the globals the modules follow through it (the
// scene manager, g_GameGlobal, the pad system), and the message buffer's vtable (NativeText). Words of
// the DOL's text and data, never written while the game runs: the same in vanilla and Project+ at
// every frame boundary from the hook's first (picked from RAM at frame 1,000 of both, 2026-10-05).
constexpr auto BRAWL_REV2_GUARDS = std::to_array<GuardWord>({
    {0x80017504, 0x90170100},  // the frame loop's end: stw r0,0x100(r23)
    {0x8000422C, 0x3DA0805A},  // __init_registers: lis r13,0x805A
    {0x80004230, 0x61AD4420},  // ori r13,r13,0x4420
    {0x8002D018, 0x806DBC40},  // gfSceneManager::getInstance: lwz r3,-0x43C0(r13) (0x805A0060)
    {0x8000D8F4, 0x806DBCC0},  // lwz r3,-0x4340(r13): g_GameGlobal (0x805A00E0)
    {0x80017538, 0x83EDBC20},  // the frame loop: lwz r31,-0x43E0(r13): gfPadSystem (0x805A0040)
    {0x804540E8, 0x80069820},  // the message buffer's vtable (0x804540E0), +8
    {0x804540EC, 0x8004755C},  // and +C
});
// Project+ v3.2 adds its codeset's code that reads and writes the stage select data RankedPPlus and
// the online rules use (Source/Netplay/Net-Random.asm: the data at 0x8042C4E8, its page index at
// 0x8042C821, the current page at 0x80496000). Flat packs can relocate these instruction pairs
// with pplusGuards; the referenced data and the exact instruction words stay the same.
constexpr auto PPLUS_GUARDS = std::to_array<GuardWord>({
    {0x80553594, 0x3CA08042},  // lis r5,0x8042
    {0x80553598, 0x60A5C4E8},  // ori r5,r5,0xC4E8: the stage select data
    {0x80554320, 0x3C608042},  // lis r3,0x8042
    {0x80554324, 0x6063C821},  // ori r3,r3,0xC821: its page index
    {0x805518D0, 0x3E008049},  // lis r16,0x8049
    {0x805518D4, 0x62106000},  // ori r16,r16,0x6000: the current page
});
// Vanilla's own: a word of Brawl's code that Project+'s codes rewrite in frame 1 and Orca's own
// RSBE01.patches never touches (PPLUS32 keepGameCode: RAM holds Project+'s word, only instruction
// fetches see Brawl's), so the vanilla modules (BrawlStages' stage flow, the rules screen) never run
// on Project+. Not Brawl's v-sync call (0x80023B88): since 0.3.33 RSBE01.patches moves it as
// Project+ does, so it reads the same in both.
constexpr auto VANILLA_GUARDS = std::to_array<GuardWord>({
    {0x8001CD24, 0x38631198},  // addi r3,r3,0x1198 (Project+: lwz r3,0x11A0(r3))
});
template <size_t A, size_t B>
constexpr std::array<GuardWord, A + B> Join(const std::array<GuardWord, A>& a,
                                            const std::array<GuardWord, B>& b)
{
  std::array<GuardWord, A + B> all{};
  std::ranges::copy(a, all.begin());
  std::ranges::copy(b, all.begin() + A);
  return all;
}
// pplusGuards' defaults are these pairs' first words (Mod.h).
static_assert(PPlusGuardAddresses{}.rss_exdata == PPLUS_GUARDS[0].address &&
              PPlusGuardAddresses{}.page_index == PPLUS_GUARDS[2].address &&
              PPlusGuardAddresses{}.current_page == PPLUS_GUARDS[4].address);
constexpr auto VANILLA_BRAWL_GUARDS = Join(BRAWL_REV2_GUARDS, VANILLA_GUARDS);
constexpr auto BRAWL_REV2_PPLUS_GUARDS = Join(BRAWL_REV2_GUARDS, PPLUS_GUARDS);

u32 GuardAddress(Builtin builtin, u32 address, const std::optional<PPlusGuardAddresses>& declared)
{
  if (builtin != Builtin::BrawlRev2PPlus || !declared)
    return address;
  const std::array<u32, 3> pairs{declared->rss_exdata, declared->page_index, declared->current_page};
  for (size_t i = 0; i < pairs.size(); ++i)
  {
    const u32 base = PPLUS_GUARDS[2 * i].address;
    if (address == base || address == base + 4)
      return pairs[i] + (address - base);
  }
  return address;
}

std::string BuiltinLogName(const Manifest& manifest)
{
  if (const auto& addresses = manifest.pplus_guards)
  {
    return fmt::format("{} (pplusGuards rssExdata={:#010x} pageIndex={:#010x} currentPage={:#010x})",
                       BuiltinName(manifest.builtin), addresses->rss_exdata,
                       addresses->page_index, addresses->current_page);
  }
  return std::string(BuiltinName(manifest.builtin));
}
}  // namespace

std::span<const GuardWord> BuiltinGuards(Builtin builtin)
{
  switch (builtin)
  {
  case Builtin::BrawlRev2:
    return VANILLA_BRAWL_GUARDS;
  case Builtin::BrawlRev2Results:
    return BRAWL_REV2_GUARDS;
  case Builtin::BrawlRev2PPlus:
    return BRAWL_REV2_PPLUS_GUARDS;
  default:
    return {};
  }
}

bool GuardsHold(Builtin builtin, const std::function<std::optional<u32>(u32)>& read,
                const std::optional<PPlusGuardAddresses>& pplus_guards)
{
  if (builtin == Builtin::None)
    return false;
  for (const GuardWord& g : BuiltinGuards(builtin))
  {
    if (read(GuardAddress(builtin, g.address, pplus_guards)) != g.word)
      return false;
  }
  return true;
}

namespace
{
// Whether the builtin's modules run: 1 while the guards hold in memory as it is now, else 0 (failed
// for the caps) or -1 (not decided yet). A pure function of memory, first runs and re-runs alike.
std::atomic<int> s_guards{-1};
// For the caps only (Offers, GuardsDecided): the guards held at some frame since boot, or failed
// for the rest of the boot. Never what the modules do: a joiner replays frame 1 from the starting
// snapshot after its own frames held, and must run it as the host did (off, Project+'s codes not
// landed yet).
std::atomic<bool> s_guards_failed{false};
std::atomic<bool> s_held_once{false};

void LogFirstBrokenGuard(const Core::CPUThreadGuard& guard, const Manifest& manifest)
{
  for (const GuardWord& g : BuiltinGuards(manifest.builtin))
  {
    const u32 address = GuardAddress(manifest.builtin, g.address, manifest.pplus_guards);
    const u32 word = PowerPC::MMU::HostIsRAMAddress(guard, address) ?
                         PowerPC::MMU::HostRead<u32>(guard, address) :
                         0;
    if (word != g.word)
    {
      WARN_LOG_FMT(BOOT,
                   "Orca mod: builtin {} failed: {:08x} holds {:08x}, not {:08x} (this mod's code "
                   "moved what the builtin's modules rely on; its caps are withdrawn)",
                   BuiltinLogName(manifest), address, word, g.word);
      return;
    }
  }
}
}  // namespace

void ResetGuards()
{
  s_guards = -1;
  s_guards_failed = false;
  s_held_once = false;
}

void RefreshGuards(const Core::CPUThreadGuard& guard, std::optional<int> frame)
{
  const Profile* profile = ActiveProfile();
  const ModInfo* mod = profile && profile->mod ? &*profile->mod : nullptr;
  if (!mod || mod->manifest.builtin == Builtin::None)
    return;
  // Before the game's code is live (a launcher still running), the modules stay off.
  if (!FrameHookLive(guard, *profile))
  {
    s_guards = -1;
    return;
  }
  const Builtin builtin = mod->manifest.builtin;
  const bool held = GuardsHold(builtin, [&guard](u32 address) -> std::optional<u32> {
    if (!PowerPC::MMU::HostIsRAMAddress(guard, address))
      return std::nullopt;
    return PowerPC::MMU::HostRead<u32>(guard, address);
  }, mod->manifest.pplus_guards);
  // Logged once each per boot: memory may go back and forth (a join's replay of frame 1).
  const bool held_before = s_held_once, failed_before = s_guards_failed;
  NoteGuards(held, frame);
  if (!held_before && s_held_once)
  {
    NOTICE_LOG_FMT(BOOT, "Orca mod: builtin {} on (its guard words hold)",
                   BuiltinLogName(mod->manifest));
    // pplusGuards moves the guards only (ORCA.md "P+ fork guards"): the data stays v3.2's.
    if (mod->manifest.pplus_guards)
    {
      WARN_LOG_FMT(BOOT, "Orca mod: pplusGuards: known limits on a fork: a locked header's "
                         "character select writes Project+ v3.2's stage preset to 0x8042C4E8, and "
                         "the Code Menu is locked only where v3.2's layout is found");
    }
  }
  if (!failed_before && s_guards_failed)
    LogFirstBrokenGuard(guard, mod->manifest);
}

void NoteGuards(bool held, std::optional<int> frame)
{
  const ModInfo* mod = ActiveMod();
  if (!mod || mod->manifest.builtin == Builtin::None)
    return;
  if (held)
  {
    s_guards = 1;
    s_held_once = true;
    return;
  }
  // Not holding: the modules are off for this frame. For the caps it is a failure (for the rest of
  // the boot: what they withdrew stays withdrawn) only at or after the first frame a friend could
  // join from. Earlier frames are a loader's codes still landing (Project+'s in frame 1), or a
  // join's or a rollback's re-run of them; the input gate's calls carry no frame and never decide.
  if (frame && *frame >= static_cast<int>(mod->manifest.first_keyframe_frame))
    s_guards_failed = true;
  s_guards = s_guards_failed ? 0 : -1;
}

bool GuardsDecided()
{
  const ModInfo* mod = ActiveMod();
  return !mod || mod->manifest.builtin == Builtin::None || s_held_once || s_guards_failed;
}

bool BuiltinOn(Builtin builtin)
{
  const ModInfo* mod = ActiveMod();
  return mod && builtin != Builtin::None && mod->manifest.builtin == builtin && s_guards == 1;
}

Builtin DeclaredBuiltin()
{
  const ModInfo* mod = ActiveMod();
  return mod ? mod->manifest.builtin : Builtin::None;
}

bool GuardsFailed()
{
  return ActiveMod() && s_guards_failed;
}

bool Offers(std::string_view cap)
{
  const ModInfo* mod = ActiveMod();
  // Only the builtin's supported caps, once its guards have held and never after failure.
  return mod && mod->manifest.builtin != Builtin::None && s_held_once && !s_guards_failed &&
         (mod->manifest.builtin != Builtin::BrawlRev2Results ||
          (cap == "results" && mod->manifest.ranked_rules)) &&
         std::ranges::find(mod->manifest.offers, cap) != mod->manifest.offers.end();
}
}  // namespace Orca::Mod

// --- The profile -------------------------------------------------------------------------------

namespace Orca
{
namespace
{
// A bundle file the session reads whole (to parse or hash it), refused when over max_bytes: a tree
// may list files up to MAX_FILE_BYTES, and only the card's are streamed.
bool ReadBounded(const std::string& path, u64 max_bytes, std::string* out, std::string* why)
{
  File::IOFile f(path, "rb");
  if (!f)
  {
    *why = "can't be read";
    return false;
  }
  const u64 size = f.GetSize();
  if (size > max_bytes)
  {
    *why = fmt::format("is over {} bytes", max_bytes);
    return false;
  }
  out->resize(size);
  if (size != 0 && !f.ReadBytes(out->data(), size))
  {
    *why = "can't be read";
    return false;
  }
  return true;
}

// The profile a bundle's validated mod.json makes, a tree bundle's or a flat one's.
std::optional<Profile> ProfileFromManifest(const std::string& dir, Mod::Tree tree,
                                           Mod::Manifest manifest, std::string* error)
{
  Profile profile;
  // The room's mode and the boot log name it as the legacy profile its builtin replaces, so a
  // bundle of Brawl or Project+ plays in the same modes as before.
  switch (manifest.builtin)
  {
  case Mod::Builtin::BrawlRev2:
  case Mod::Builtin::BrawlRev2Results:
    profile.game_id = "RSBE01";
    break;
  case Mod::Builtin::BrawlRev2PPlus:
    profile.game_id = "PPLUS32";
    break;
  default:
    profile.game_id = manifest.disc_id;
    break;
  }
  profile.title = manifest.title;
  profile.revision = manifest.disc_revision;
  profile.frame_hook = manifest.frame_hook;
  profile.frame_hook_word = manifest.frame_hook_word;
  profile.boot_nand_frame = manifest.boot_nand_frame;
  profile.jit_clear_frame = manifest.jit_clear_frame;
  for (const Mod::KeptWord& k : manifest.keep_game_code)
    profile.kept_code.push_back({k.address, k.game, k.loader});
  profile.save_title = manifest.save_title;
  // The card is in exactly when mod.json has one (never a forced key of its own).
  if (manifest.sd_bytes || manifest.sd_image)
    profile.forced.emplace_back(Config::Location{Config::System::Main, "Core", "WiiSDCard"}, "True");
  for (const auto& [key, value] : manifest.forced)
  {
    const size_t dot1 = key.find('.');
    const size_t dot2 = key.find('.', dot1 + 1);
    const std::optional<Config::System> system = Config::GetSystemFromName(key.substr(0, dot1));
    if (!system)
    {
      *error = fmt::format("Orca mod: unknown system in {}", key);
      return std::nullopt;
    }
    profile.forced.emplace_back(
        Config::Location{*system, key.substr(dot1 + 1, dot2 - dot1 - 1), key.substr(dot2 + 1)},
        value);
  }
  if (const Mod::TreeFile* launcher =
          manifest.launcher.empty() ? nullptr : tree.Find(manifest.launcher))
  {
    profile.launcher_hash = launcher->sha256;
    profile.disc = manifest.disc_id;
  }
  if (manifest.sd_bytes)
    profile.sd_image = "sd/";
  else if (manifest.sd_image)
    profile.sd_image = manifest.sd_image->file;
  profile.mod = Mod::ModInfo{dir, std::move(tree), std::move(manifest), {}};
  return profile;
}
}  // namespace

std::optional<Profile> LoadModProfile(const std::string& dir_in, std::string* error)
{
  std::string dir = dir_in;
  if (dir.empty())
  {
    *error = "Orca mod: no mod folder";
    return std::nullopt;
  }
  if (dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  std::string text, why;
  if (!ReadBounded(dir + "tree.json", Mod::MAX_TREE_JSON_BYTES, &text, &why))
  {
    *error = fmt::format("Orca mod: {}tree.json {}", dir, why);
    return std::nullopt;
  }
  std::optional<Mod::Tree> tree = Mod::ParseTree(text, error);
  if (!tree)
    return std::nullopt;
  // Every file there, at the size tree.json names (its content is the app's sha256 check at
  // download, and for the SD card the built image's hash in the key).
  for (const Mod::TreeFile& f : tree->files)
  {
    const std::string path = dir + f.path;
    if (!File::Exists(path) || File::IsDirectory(path) || File::GetSize(path) != f.bytes)
    {
      *error = fmt::format("Orca mod: {} is missing or not the size tree.json names ({} bytes)",
                           f.path, f.bytes);
      return std::nullopt;
    }
  }
  // mod.json: hashed against tree.json and parsed from the same bytes.
  const Mod::TreeFile* mod_file = tree->Find("mod.json");
  if (!mod_file)
  {
    *error = "Orca mod: the bundle has no mod.json";
    return std::nullopt;
  }
  if (!ReadBounded(dir + "mod.json", Mod::MAX_MOD_JSON_BYTES, &text, &why))
  {
    *error = fmt::format("Orca mod: mod.json {}", why);
    return std::nullopt;
  }
  if (Mod::Sha256Hex(text) != mod_file->sha256)
  {
    *error = "Orca mod: mod.json is not the file tree.json names (sha256)";
    return std::nullopt;
  }
  std::optional<Mod::Manifest> manifest = Mod::ParseManifest(text, error);
  if (!manifest)
    return std::nullopt;
  if (manifest->format != 1)
  {
    *error = "Orca mod: a run folder with tree.json needs a format 1 mod.json (format 2 is a flat "
             "bundle's)";
    return std::nullopt;
  }
  const Mod::TreeFile* launcher =
      manifest->launcher.empty() ? nullptr : tree->Find(manifest->launcher);
  if (!manifest->launcher.empty() && !launcher)
  {
    *error = fmt::format("Orca mod: mod.json's launcher {} is not in the bundle", manifest->launcher);
    return std::nullopt;
  }
  // What else reaches the game besides the SD card (whose built image's hash is in the key) is
  // checked against tree.json's sha256 too: the launcher and any other .dol or .elf at the root
  // (any case), and the game patches. Small files; a run folder edited in place since the app laid
  // it out is refused rather than desyncing.
  for (const Mod::TreeFile& f : tree->files)
  {
    const std::string lower = Mod::ToLowerAscii(f.path);
    const bool executable =
        f.path == manifest->launcher ||
        (f.path.find('/') == std::string::npos && (lower.ends_with(".dol") || lower.ends_with(".elf")));
    if (!executable && !f.path.starts_with("orca/patches/"))
      continue;
    std::string contents;
    if (!ReadBounded(dir + f.path, executable ? Mod::MAX_LAUNCHER_BYTES : Mod::MAX_PATCHES_BYTES,
                     &contents, &why))
    {
      *error = fmt::format("Orca mod: {} {}", f.path, why);
      return std::nullopt;
    }
    if (Mod::Sha256Hex(contents) != f.sha256)
    {
      *error = fmt::format("Orca mod: {} is not the file tree.json names (sha256)", f.path);
      return std::nullopt;
    }
  }

  // Game patches (orca/patches/<name>.patches): every file must parse, or the session is refused
  // here rather than running without them.
  for (const Mod::TreeFile* f : tree->Under("orca/patches/"))
  {
    if (!f->path.ends_with(".patches") ||
        f->path.find('/', sizeof("orca/patches/") - 1) != std::string::npos)
    {
      continue;
    }
    std::string patch_text;
    why.clear();
    if (!ReadBounded(dir + f->path, Mod::MAX_PATCHES_BYTES, &patch_text, &why) ||
        !UX::ParseGamePatches(patch_text, &why))
    {
      *error = fmt::format("Orca mod: {}: {}", f->path, why.empty() ? "can't be read" : why);
      return std::nullopt;
    }
  }

  return ProfileFromManifest(dir, std::move(*tree), std::move(*manifest), error);
}

std::optional<Profile> LoadFlatProfile(const Mod::FlatBundle& bundle, std::string* error)
{
  std::optional<Profile> profile =
      ProfileFromManifest(bundle.dir, bundle.tree, bundle.manifest, error);
  if (!profile)
    return std::nullopt;
  Mod::ModInfo& mod = *profile->mod;
  mod.flat = true;
  mod.flat_patches = bundle.patches;
  mod.flat_jitwarm = bundle.jitwarm;
  mod.sd_image_hash = bundle.sd_image_hash;
  return profile;
}
}  // namespace Orca

namespace Orca
{
bool PrepareModBoot(Profile* profile, const std::string* executable_path,
                    const std::string& default_disc, const std::string& boot_game_id,
                    u16 boot_revision, std::string* error, bool* wrong_disc)
{
  *wrong_disc = false;
  Mod::ModInfo& mod = *profile->mod;
  const Mod::Manifest& m = mod.manifest;
  if (executable_path)
  {
    if (m.launcher.empty())
    {
      *error = "Orca mod: this mod boots its disc, not a loader";
      return false;
    }
    // Exactly the bundle's launcher (the app lays it out at <run folder>/<launcher>).
    // By name too: a loader's game ID (which game INIs apply to it) comes from its file name.
    std::error_code ec;
    if (PathToFileName(*executable_path) != m.launcher ||
        !std::filesystem::equivalent(StringToPath(*executable_path),
                                     StringToPath(mod.dir + m.launcher), ec))
    {
      *error = fmt::format("Orca mod: {} is not this mod's launcher ({})", *executable_path,
                           m.launcher);
      return false;
    }
    const std::unique_ptr<DiscIO::VolumeDisc> disc =
        default_disc.empty() ? nullptr : DiscIO::CreateDisc(default_disc);
    if (!disc)
    {
      *error = fmt::format("Orca mod: the launcher boots {} from the drive, and {}", m.disc_id,
                           DiscOpenProblem(default_disc));
      return false;
    }
    // The boot inserts the disc with the same alias (Boot.cpp SetDefaultDisc): Brawl's Rev 1 dump
    // reads as Rev 2, as for a legacy launcher profile (Profile.cpp).
    AliasRevision(*disc);
    const u16 revision = disc->GetRevision().value_or(0);
    if (disc->GetGameID() != m.disc_id || revision != m.disc_revision)
    {
      *wrong_disc = true;
      *error = fmt::format("Orca mod: this mod needs {} revision {}; the disc is {} revision {}",
                           m.disc_id, m.disc_revision, disc->GetGameID(), revision);
      return false;
    }
  }
  else
  {
    if (!m.launcher.empty())
    {
      *error = "Orca mod: this mod boots through its launcher, not the disc";
      return false;
    }
    if (boot_game_id != m.disc_id || boot_revision != m.disc_revision)
    {
      *wrong_disc = true;
      *error = fmt::format("Orca mod: this mod needs {} revision {}; the disc is {} revision {}",
                           m.disc_id, m.disc_revision, boot_game_id, boot_revision);
      return false;
    }
  }
  if (m.sd_image)
  {
    // A flat bundle's card, used as it is: its size and chunked SHA-256 were checked as the bundle
    // was verified (Bundle.cpp), once per process.
    if (mod.sd_image_hash.empty())
    {
      *error = "Orca mod: the SD image was not verified";
      return false;
    }
    profile->sd_image_path = mod.dir + m.sd_image->file;
    profile->sd_image_hash = mod.sd_image_hash;
  }
  else if (m.sd_bytes)
  {
    const std::optional<Mod::SdImage> image =
        Mod::PrepareSdImage(mod, GetEnv("ORCA_SD_CACHE"), error);
    if (!image)
      return false;
    // The session's config layer points the SD card at this image, read-only (writes and folder
    // sync are forced off), as for a legacy launcher profile's sd.raw.
    profile->sd_image_path = image->path;
    profile->sd_image_hash = image->hash;
    mod.sd_image_hash = image->hash;
  }
  return true;
}
}  // namespace Orca
