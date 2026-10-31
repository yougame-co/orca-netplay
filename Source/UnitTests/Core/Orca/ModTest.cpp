// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>
#include <xxh3.h>

#include "Common/FileUtil.h"
#include "Common/CommonPaths.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Common/Version.h"
#include "Core/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Music.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/UX/UX.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/CharOrder.h"
#include "Core/Orca/Games/Brawl/FreeSpace.h"
#include "Core/Orca/UX/MenuEvents.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/Search.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/InputGate.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

namespace
{
using Orca::Mod::Builtin;

void SetEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}

std::string RepoPath(const std::string& relative)
{
  return (std::filesystem::path(__FILE__).parent_path() / "../../../../" / relative).string();
}

picojson::value ReadJson(const std::string& path)
{
  std::string text;
  EXPECT_TRUE(File::ReadFileToString(path, text)) << path;
  picojson::value v;
  EXPECT_EQ(picojson::parse(v, text), "") << path;
  return v;
}

// Project+'s mod.json as Tools/orca/mods/make-bundle.py writes it.
constexpr char PPLUS_MOD[] = R"({
  "format": 1,
  "engine": {"api": 1, "minOrca": "0.3.35"},
  "title": "Project+",
  "disc": {"id": "RSBE01", "revision": 2},
  "launcher": "launcher.dol",
  "sd": {"bytes": 2097152000},
  "frameHook": {"address": "0x80017504", "word": "0x90170100"},
  "bootNandFrame": 600,
  "firstKeyframeFrame": 300,
  "jitClearFrame": 2,
  "keepGameCode": [["0x80023B88", "0x4182FF7C", "0x60000000"],
                   ["0x80024028", "0x4E800020", "0x481C4904"]],
  "saveTitle": "0x0001000052534245",
  "forced": {"Dolphin.Core.CustomRTCValue": 1735689600},
  "builtin": "brawl-rev2-pplus",
  "rulesets": ["pplus-2024"],
  "offers": ["results", "locks", "queue2"]
})";

constexpr char BRAWL_MOD[] = R"({
  "format": 1,
  "engine": {"api": 1, "minOrca": "0.3.35"},
  "disc": {"id": "RSBE01", "revision": 2},
  "frameHook": {"address": "0x80017504", "word": "0x90170100"},
  "bootNandFrame": 1800,
  "builtin": "brawl-rev2",
  "offers": ["results", "locks", "queue2"]
})";

// BRAWL_MOD with `key` replaced by (or, when absent, given) `json`.
std::string BrawlWith(const std::string& key, const std::string& json)
{
  picojson::value v;
  picojson::parse(v, BRAWL_MOD);
  picojson::value field;
  const std::string err = picojson::parse(field, json);
  EXPECT_EQ(err, "") << json;
  v.get<picojson::object>()[key] = field;
  return v.serialize();
}

std::string BrawlWithout(const std::string& key)
{
  picojson::value v;
  picojson::parse(v, BRAWL_MOD);
  v.get<picojson::object>().erase(key);
  return v.serialize();
}

bool Refused(const std::string& json)
{
  std::string error;
  const bool refused = !Orca::Mod::ParseManifest(json, &error).has_value();
  return refused && !error.empty();
}

// A bundle folder with these files and its tree.json.
struct Bundle
{
  std::string dir;
  std::vector<Orca::Mod::TreeFile> files;

  void Add(const std::string& path, const std::string& contents)
  {
    const std::string full = dir + path;
    File::CreateFullPath(full);
    ASSERT_TRUE(File::WriteStringToFile(full, contents));
    files.push_back({path, Orca::Mod::Sha256Hex(contents), contents.size()});
  }

  void WriteTree()
  {
    std::ranges::sort(files, {}, &Orca::Mod::TreeFile::path);
    picojson::array list;
    for (const auto& f : files)
    {
      picojson::object o;
      o["path"] = picojson::value(f.path);
      o["sha256"] = picojson::value(f.sha256);
      o["bytes"] = picojson::value(static_cast<double>(f.bytes));
      list.emplace_back(o);
    }
    picojson::object tree;
    tree["tree"] = picojson::value(Orca::Mod::TreeHash(files));
    tree["files"] = picojson::value(list);
    ASSERT_TRUE(File::WriteStringToFile(dir + "tree.json", picojson::value(tree).serialize()));
  }
};

Bundle MakeBundle(const std::string& mod_json)
{
  Bundle b{File::CreateTempDir() + "/", {}};
  b.Add("mod.json", mod_json);
  return b;
}
}  // namespace

// The shared vector (Tools/orca/mods/tree-hash-vector.json; the app and the site check the same).
TEST(OrcaMod, TreeHashVector)
{
  const picojson::value vector = ReadJson(RepoPath("Tools/orca/mods/tree-hash-vector.json"));
  const picojson::array& cases = vector.get("cases").get<picojson::array>();
  ASSERT_EQ(cases.size(), 1u);
  for (const picojson::value& c : cases)
  {
    std::vector<Orca::Mod::TreeFile> files;
    for (const picojson::value& f : c.get("files").get<picojson::array>())
    {
      files.push_back({f.get("path").get<std::string>(), f.get("sha256").get<std::string>(),
                       static_cast<u64>(f.get("bytes").get<double>())});
    }
    EXPECT_EQ(Orca::Mod::TreeHash(files), c.get("treeHash").get<std::string>())
        << c.get("name").get<std::string>();
    EXPECT_EQ(Orca::Mod::Sha256Hex(c.get("hashedText").get<std::string>()),
              c.get("treeHash").get<std::string>());
    // As a tree.json, it parses and its hash checks.
    picojson::object tree;
    tree["tree"] = c.get("treeHash");
    tree["files"] = c.get("files");
    std::string error;
    const auto parsed = Orca::Mod::ParseTree(picojson::value(tree).serialize(), &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->files, files);
  }
  // The desktop app's vector (YouGame desktop, orca-mods-p1).
  EXPECT_EQ(Orca::Mod::TreeHash(std::vector<Orca::Mod::TreeFile>{
                {"mod.json", "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a", 2},
                {"orca/jitwarm", "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881",
                 1},
                {"sd/a.txt", "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb",
                 1}}),
            "86a7f6c4a959028c0334ee254622d453a7993337703b41262080e1f1524abc1a");
}

TEST(OrcaMod, PathRules)
{
  for (const char* good : {"mod.json", "sd/Project+/pf/sound/strm/Super Mario/Bowser's Galaxy (M&S).brstm",
                           "orca/patches/a.patches", "launcher.dol", "sd/RSBE01.txt", "a b/c",
                           "consoles.txt", "com10", "nul_x"})
  {
    EXPECT_EQ(Orca::Mod::PathProblem(good), std::nullopt) << good;
  }
  for (const char* bad :
       {"", "/abs", "a//b", "a/", "../x", "a/../b", "./a", ".DS_Store", "sd/.hidden", "a./b", "a /b",
        "a.", "CON", "con.txt", "sd/NUL", "Com1.dat", "lpt9", "a\\b", "a:b", "a*b", "a?b", "a\"b",
        "a<b", "a|b", "caf\xc3\xa9", "tab\there"})
  {
    EXPECT_NE(Orca::Mod::PathProblem(bad), std::nullopt) << bad;
  }
  EXPECT_EQ(Orca::Mod::PathProblem(std::string(100, 'a')), std::nullopt);
  EXPECT_NE(Orca::Mod::PathProblem(std::string(101, 'a')), std::nullopt);
}

TEST(OrcaMod, TreeRefusals)
{
  const auto tree_json = [](const std::vector<Orca::Mod::TreeFile>& files,
                            std::optional<std::string> hash = std::nullopt) {
    picojson::array list;
    for (const auto& f : files)
    {
      picojson::object o;
      o["path"] = picojson::value(f.path);
      o["sha256"] = picojson::value(f.sha256);
      o["bytes"] = picojson::value(static_cast<double>(f.bytes));
      list.emplace_back(o);
    }
    picojson::object t;
    t["tree"] = picojson::value(hash.value_or(Orca::Mod::TreeHash(files)));
    t["files"] = picojson::value(list);
    return picojson::value(t).serialize();
  };
  const std::string h(64, 'a');
  std::string error;
  EXPECT_TRUE(Orca::Mod::ParseTree(tree_json({{"a", h, 1}, {"b", h, 2}}), &error)) << error;
  const auto refused = [&](const std::string& json) {
    std::string why;
    return !Orca::Mod::ParseTree(json, &why) && !why.empty();
  };
  EXPECT_TRUE(refused(tree_json({{"b", h, 1}, {"a", h, 2}})));            // out of order
  EXPECT_TRUE(refused(tree_json({{"a", h, 1}, {"a", h, 2}})));            // twice
  EXPECT_TRUE(refused(tree_json({{"A", h, 1}, {"a", h, 2}})));            // case
  EXPECT_TRUE(refused(tree_json({{"a", h, 1}, {"a/b", h, 2}})));          // file and folder
  EXPECT_TRUE(refused(tree_json({{"A", h, 1}, {"a/b", h, 2}})));          // ... ignoring case
  EXPECT_TRUE(refused(tree_json({{"tree.json", h, 1}})));                  // itself
  EXPECT_TRUE(refused(tree_json({{"../a", h, 1}})));                       // a bad path
  EXPECT_TRUE(refused(tree_json({{"a", std::string(64, 'A'), 1}})));      // hash case
  EXPECT_TRUE(refused(tree_json({{"a", h, 1}}, std::string(64, '0'))));    // wrong tree hash
  EXPECT_TRUE(refused(tree_json({{"a", h, 0x100000000}})));                // over FAT's limit
  EXPECT_TRUE(refused(R"({"tree": ")" + Orca::Mod::TreeHash({}) + R"(", "files": [], "x": 1})"));
  EXPECT_TRUE(refused(R"({"tree": ")" + Orca::Mod::TreeHash({}) +
                      R"(", "files": [{"path": "a", "sha256": ")" + h + R"(", "bytes": 1.5}]})"));
  EXPECT_TRUE(refused("[]"));
  EXPECT_TRUE(refused("{"));
}

TEST(OrcaMod, MinOrcaAboveThisBuildRefuses)
{
  // Number by number, not as text.
  using Orca::Mod::VersionAtLeast;
  EXPECT_TRUE(VersionAtLeast("0.3.47", "0.3.47"));
  EXPECT_TRUE(VersionAtLeast("0.3.47", "0.3.9"));
  EXPECT_TRUE(VersionAtLeast("0.3.10", "0.3.9"));
  EXPECT_TRUE(VersionAtLeast("1.0.0", "0.99.99"));
  EXPECT_TRUE(VersionAtLeast("0.3.047", "0.3.47"));
  EXPECT_TRUE(VersionAtLeast("10.0.0", "9.99.999"));
  EXPECT_FALSE(VersionAtLeast("0.3.9", "0.3.10"));
  EXPECT_FALSE(VersionAtLeast("0.3.47", "0.3.48"));
  EXPECT_FALSE(VersionAtLeast("0.3.47", "0.4.0"));
  EXPECT_FALSE(VersionAtLeast("0.3.47", "1.0.0"));
  EXPECT_FALSE(VersionAtLeast("0.3.47", "0.3.100000000000000000"));
  // This build's version is X.Y.Z, and a pack may need exactly it.
  const std::string running(Orca::Mod::EngineVersion());
  ASSERT_EQ(std::count(running.begin(), running.end(), '.'), 2) << running;
  std::string error;
  EXPECT_TRUE(Orca::Mod::ParseManifest(
      BrawlWith("engine", fmt::format(R"({{"api": 1, "minOrca": "{}"}})", running)), &error))
      << error;
  EXPECT_TRUE(Orca::Mod::ParseManifest(BrawlWith("engine", R"({"api": 1, "minOrca": "0.0.1"})"),
                                       &error))
      << error;
  // One patch newer: refused with the player's message, and before anything else this build can't
  // read in it (a key from a later release, a builtin it doesn't know).
  const size_t dot = running.rfind('.');
  const std::string next =
      running.substr(0, dot + 1) + std::to_string(std::stoull(running.substr(dot + 1)) + 1);
  const std::string message =
      fmt::format("This pack needs YG Dolphin {} or newer. Update the YouGame app and try again.",
                  next);
  for (const auto& [json, api] : std::vector<std::pair<std::string, double>>{
           {BRAWL_MOD, 1}, {BRAWL_MOD, 2}, {BrawlWith("laterKey", "1"), 1},
           {BrawlWith("builtin", R"("brawl-rev3")"), 1}})
  {
    picojson::value value;
    ASSERT_EQ(picojson::parse(value, json), "");
    value.get<picojson::object>()["engine"] = picojson::value(picojson::object{
        {"api", picojson::value(api)}, {"minOrca", picojson::value(next)}});
    error.clear();
    EXPECT_FALSE(Orca::Mod::ParseManifest(value.serialize(), &error));
    EXPECT_EQ(error, message) << value.serialize();
  }
  EXPECT_FALSE(Orca::Mod::ParseManifest(
      BrawlWith("engine", R"({"api": 1, "minOrca": "999999999999.0.0"})"), &error));
  EXPECT_EQ(error, "This pack needs YG Dolphin 999999999999.0.0 or newer. Update the YouGame app "
                   "and try again.");
  // A malformed one is still a form error, not an update.
  EXPECT_FALSE(
      Orca::Mod::ParseManifest(BrawlWith("engine", R"({"api": 1, "minOrca": "9.9"})"), &error));
  EXPECT_NE(error.find("engine.minOrca must be a version"), std::string::npos) << error;
}

TEST(OrcaMod, ParsesEveryField)
{
  std::string error;
  const auto m = Orca::Mod::ParseManifest(PPLUS_MOD, &error);
  ASSERT_TRUE(m) << error;
  EXPECT_EQ(m->engine_api, 1);
  EXPECT_EQ(m->min_orca, "0.3.35");
  EXPECT_EQ(m->title, "Project+");
  EXPECT_EQ(m->disc_id, "RSBE01");
  EXPECT_EQ(m->disc_revision, 2);
  EXPECT_EQ(m->launcher, "launcher.dol");
  EXPECT_EQ(m->sd_bytes, 2097152000u);
  EXPECT_EQ(m->frame_hook, 0x80017504u);
  EXPECT_EQ(m->frame_hook_word, 0x90170100u);
  EXPECT_EQ(m->boot_nand_frame, 600u);
  EXPECT_EQ(m->first_keyframe_frame, 300u);
  EXPECT_EQ(m->jit_clear_frame, 2u);
  const std::vector<Orca::Mod::KeptWord> kept{{0x80023B88, 0x4182FF7C, 0x60000000},
                                              {0x80024028, 0x4E800020, 0x481C4904}};
  EXPECT_EQ(m->keep_game_code, kept);
  EXPECT_EQ(m->save_title, 0x0001000052534245u);
  ASSERT_EQ(m->forced.size(), 1u);
  EXPECT_EQ(m->forced[0].first, "Dolphin.Core.CustomRTCValue");
  EXPECT_EQ(m->forced[0].second, "1735689600");
  EXPECT_EQ(m->builtin, Builtin::BrawlRev2PPlus);
  EXPECT_EQ(m->rulesets, std::vector<std::string>{"pplus-2024"});
  EXPECT_EQ(m->offers, (std::vector<std::string>{"results", "locks", "queue2"}));

  const auto b = Orca::Mod::ParseManifest(BRAWL_MOD, &error);
  ASSERT_TRUE(b) << error;
  EXPECT_EQ(b->launcher, "");
  EXPECT_EQ(b->sd_bytes, std::nullopt);
  EXPECT_EQ(b->first_keyframe_frame, Orca::Mod::DEFAULT_FIRST_KEYFRAME_FRAME);
  EXPECT_EQ(b->jit_clear_frame, std::nullopt);
  EXPECT_EQ(b->builtin, Builtin::BrawlRev2);
}

TEST(OrcaMod, RefusesAnythingElse)
{
  EXPECT_TRUE(Refused(""));
  EXPECT_TRUE(Refused("[]"));
  EXPECT_TRUE(Refused(BrawlWith("unknown", "1")));
  // Format 2 is a flat bundle's (Bundle.h, BundleTest.cpp): a tree bundle's "sd" is refused there.
  EXPECT_TRUE(Refused(BrawlWith("format", "3")));
  EXPECT_TRUE(Refused(BrawlWith("format", "0")));
  EXPECT_TRUE(Refused(BrawlWith("format", "\"1\"")));
  std::string pplus_flat = PPLUS_MOD;
  pplus_flat.replace(pplus_flat.find("\"format\": 1"), 11, "\"format\": 2");
  EXPECT_TRUE(Refused(pplus_flat));
  EXPECT_TRUE(Refused(BrawlWithout("format")));
  EXPECT_TRUE(Refused(BrawlWithout("engine")));
  EXPECT_TRUE(Refused(BrawlWithout("disc")));
  EXPECT_TRUE(Refused(BrawlWithout("frameHook")));
  EXPECT_TRUE(Refused(BrawlWith("engine", R"({"api": 2, "minOrca": "0.3.35"})")));
  EXPECT_TRUE(Refused(BrawlWith("engine", R"({"api": 1})")));
  EXPECT_TRUE(Refused(BrawlWith("engine", R"({"api": 1, "minOrca": "0.4"})")));
  EXPECT_TRUE(Refused(BrawlWith("engine", R"({"api": 1, "minOrca": "0.3.35", "x": 1})")));
  EXPECT_TRUE(Refused(BrawlWith("title", R"("")")));
  EXPECT_TRUE(Refused(BrawlWith("title", "\"" + std::string(49, 'x') + "\"")));
  EXPECT_TRUE(Refused(BrawlWith("title", "\"caf\xc3\xa9\"")));
  EXPECT_TRUE(Refused(BrawlWith("disc", R"({"id": "rsbe01", "revision": 2})")));
  EXPECT_TRUE(Refused(BrawlWith("disc", R"({"id": "RSBE01", "revision": 256})")));
  EXPECT_TRUE(Refused(BrawlWith("disc", R"({"id": "RSBE01"})")));
  EXPECT_TRUE(Refused(BrawlWith("launcher", R"("dir/launcher.dol")")));
  EXPECT_TRUE(Refused(BrawlWith("launcher", R"("launcher.exe")")));
  EXPECT_TRUE(Refused(BrawlWith("launcher", R"("../launcher.dol")")));
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 2147516416})")));  // over 2 GiB: SDHC
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 100000000})")));   // not whole clusters
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 32768})")));       // too small for FAT32
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 2097152000, "sync": true})")));
  EXPECT_FALSE(Refused(BrawlWith("sd", R"({"bytes": 2147483648})")));
  EXPECT_FALSE(Refused(BrawlWith("sd", R"({"bytes": 2097152000, "clusterBytes": 2048, "fats": 2})")));
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 2097152000, "clusterBytes": 3000})")));
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 2097152000, "clusterBytes": 65536})")));
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 2097152000, "fats": 3})")));
  EXPECT_TRUE(Refused(BrawlWith("sd", R"({"bytes": 67108864, "clusterBytes": 2048})")));  // FAT16-sized
  EXPECT_FALSE(Refused(BrawlWith("sd", R"({"bytes": 167772160, "clusterBytes": 2048})")));
  EXPECT_TRUE(Refused(BrawlWith("frameHook", R"({"address": "0x80017506", "word": "0x1"})")));
  EXPECT_TRUE(Refused(BrawlWith("frameHook", R"({"address": "0x90000000", "word": "0x1"})")));
  EXPECT_TRUE(Refused(BrawlWith("frameHook", R"({"address": "80017504", "word": "0x1"})")));
  EXPECT_TRUE(Refused(BrawlWith("frameHook", R"({"address": "0x80017504"})")));
  EXPECT_TRUE(Refused(BrawlWith("bootNandFrame", "0")));
  EXPECT_TRUE(Refused(BrawlWith("bootNandFrame", R"("600")")));
  EXPECT_TRUE(Refused(BrawlWith("firstKeyframeFrame", "10")));
  EXPECT_TRUE(Refused(BrawlWith("firstKeyframeFrame", "299")));
  EXPECT_FALSE(Refused(BrawlWith("firstKeyframeFrame", "300")));
  EXPECT_TRUE(Refused(std::string(BRAWL_MOD) + "x"));
  EXPECT_TRUE(Refused(std::string(BRAWL_MOD) + "{}"));
  EXPECT_FALSE(Refused(std::string(BRAWL_MOD) + "\n\t \r\n"));
  EXPECT_TRUE(Refused(BrawlWith("saveTitle", R"("0x+001000052534245")")));
  EXPECT_TRUE(Refused(BrawlWith("saveTitle", R"("0x 001000052534245")")));
  EXPECT_TRUE(Refused(BrawlWith("jitClearFrame", "-1")));
  EXPECT_TRUE(Refused(BrawlWith("keepGameCode", R"([["0x80023B88", "0x1", "0x1"]])")));
  EXPECT_TRUE(Refused(BrawlWith("keepGameCode", R"([["0x80023B88", "0x1"]])")));
  EXPECT_TRUE(Refused(BrawlWith("keepGameCode", R"([["0x00023B88", "0x1", "0x2"]])")));
  EXPECT_TRUE(Refused(
      BrawlWith("keepGameCode", R"([["0x80023B88", "0x1", "0x2"], ["0x80023B88", "0x3", "0x4"]])")));
  // saveTitle: only the disc's own.
  EXPECT_FALSE(Refused(BrawlWith("saveTitle", R"("0x0001000052534245")")));
  EXPECT_TRUE(Refused(BrawlWith("saveTitle", R"("0x0001000052534345")")));
  EXPECT_TRUE(Refused(BrawlWith("saveTitle", R"("0x0000000100000002")")));
  EXPECT_TRUE(Refused(BrawlWith("saveTitle", R"("0x52534245")")));
  // forced: the allowlist only, never what determinism or safety depend on.
  EXPECT_FALSE(Refused(BrawlWith("forced", R"({"Dolphin.Core.CustomRTCValue": 0})")));
  for (const char* key :
       {"Dolphin.Core.CPUThread", "Dolphin.Core.SyncOnSkipIdle", "Dolphin.Core.CustomRTCEnable",
        "Dolphin.Core.WiiSDCard", "Dolphin.Core.WiiSDCardAllowWrites",
        "Dolphin.Core.WiiSDCardEnableFolderSync", "Dolphin.Core.EnableCheats",
        "GFX.Hacks.EFBToTextureEnable", "GFX.Settings.EnableMods", "Session.Core.FMA",
        "Dolphin.Core.CustomRTCValue2"})
  {
    EXPECT_TRUE(Refused(BrawlWith("forced", fmt::format(R"({{"{}": 1}})", key)))) << key;
  }
  EXPECT_TRUE(Refused(BrawlWith("forced", R"({"Dolphin.Core.CustomRTCValue": "1735689600"})")));
  EXPECT_TRUE(Refused(BrawlWith("forced", R"({"Dolphin.Core.CustomRTCValue": 4294967296})")));
  EXPECT_TRUE(Refused(BrawlWith("builtin", R"("brawl")")));
  EXPECT_TRUE(Refused(BrawlWith("disc", R"({"id": "RSBE01", "revision": 1})")));  // builtin's disc
  EXPECT_TRUE(Refused(BrawlWith("rulesets", R"(["Pplus"])")));
  EXPECT_TRUE(Refused(BrawlWith("rulesets", R"(["a", "a"])")));
  EXPECT_TRUE(Refused(BrawlWith("offers", R"(["ranked"])")));
  EXPECT_TRUE(Refused(BrawlWith("offers", R"(["results", "results"])")));
  // A mod with no builtin offers nothing (BRAWL_MOD offers three caps).
  EXPECT_TRUE(Refused(BrawlWithout("builtin")));
}

// The engine's lists equal the schema's (the site validates with the schema).
TEST(OrcaMod, SchemaMatchesTheEngine)
{
  const picojson::value schema = ReadJson(RepoPath("Tools/orca/mods/mod.schema.json"));
  const picojson::value& limits = schema.get("x-orca-limits");
  const auto strings = [](const picojson::value& v) {
    std::vector<std::string> out;
    for (const picojson::value& s : v.get<picojson::array>())
      out.push_back(s.get<std::string>());
    return out;
  };
  EXPECT_EQ(limits.get("engineApi").get<double>(), static_cast<double>(Orca::Mod::ENGINE_API));
  EXPECT_EQ(limits.get("sdBuilder").get<std::string>(), Orca::Mod::SD_BUILDER);
  EXPECT_EQ(limits.get("maxPathBytes").get<double>(),
            static_cast<double>(Orca::Mod::MAX_PATH_BYTES));
  EXPECT_EQ(limits.get("maxFiles").get<double>(), static_cast<double>(Orca::Mod::MAX_FILES));
  const auto allow = Orca::Mod::ForcedAllowlist();
  EXPECT_EQ(strings(limits.get("forcedAllowlist")),
            std::vector<std::string>(allow.begin(), allow.end()));
  const auto caps = Orca::Mod::OfferableCaps();
  EXPECT_EQ(strings(limits.get("offerableCaps")), std::vector<std::string>(caps.begin(), caps.end()));
  EXPECT_EQ(strings(limits.get("builtins")),
            (std::vector<std::string>{std::string(Orca::Mod::BuiltinName(Builtin::BrawlRev2)),
                                      std::string(Orca::Mod::BuiltinName(Builtin::BrawlRev2PPlus)),
                                      std::string(Orca::Mod::BuiltinName(Builtin::BrawlRev2Results))}));
  // Every forced key of the schema's own list is on the allowlist, and nothing else is.
  std::vector<std::string> forced_keys;
  for (const auto& [key, value] : schema.get("properties").get("forced").get("properties").get<picojson::object>())
    forced_keys.push_back(key);
  EXPECT_EQ(forced_keys, std::vector<std::string>(allow.begin(), allow.end()));
  // The schema's examples of limits agree with the parser's.
  EXPECT_EQ(schema.get("properties").get("sd").get("properties").get("bytes").get("maximum").get<double>(),
            static_cast<double>(Orca::Mod::SD_MAX_BYTES));
}

// The allowlist starts from what the shipped profiles force; the SD card is "sd" in a mod.
TEST(OrcaMod, AllowlistCoversTheShippedProfiles)
{
  const std::string sys = RepoPath("Data/Sys/");
  for (const char* name : {"RSBE01", "PPLUS32"})
  {
    std::string error;
    const auto profile = Orca::LoadProfileFrom(sys, name, std::nullopt, &error);
    ASSERT_TRUE(profile) << error;
    for (const auto& [location, value] : profile->forced)
    {
      const std::string key = fmt::format("{}.{}.{}", Config::GetSystemName(location.system),
                                          location.section, location.key);
      const auto allow = Orca::Mod::ForcedAllowlist();
      EXPECT_TRUE(key == "Dolphin.Core.WiiSDCard" ||
                  std::ranges::find(allow, key) != allow.end())
          << key;
    }
  }
}

TEST(OrcaMod, LoadsABundle)
{
  Bundle b = MakeBundle(PPLUS_MOD);
  b.Add("launcher.dol", std::string(64, '\x01'));
  b.Add("sd/Project+/RSBE01.txt", "codes");
  b.Add("orca/patches/pplus.patches", "# none\n");
  b.Add("orca/jitwarm", "warm");
  b.WriteTree();
  std::string error;
  const auto profile = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(profile) << error;
  EXPECT_EQ(profile->game_id, "PPLUS32");  // the legacy profile its builtin replaces (room mode)
  EXPECT_EQ(profile->title, "Project+");
  EXPECT_EQ(profile->revision, 2);
  EXPECT_TRUE(profile->IsLauncher());
  EXPECT_EQ(profile->disc, "RSBE01");
  EXPECT_EQ(profile->frame_hook, 0x80017504u);
  EXPECT_EQ(profile->boot_nand_frame, 600u);
  EXPECT_EQ(profile->jit_clear_frame, 2u);
  EXPECT_EQ(profile->kept_code.size(), 2u);
  EXPECT_EQ(profile->save_title, 0x0001000052534245u);
  // The card's switch from "sd", then mod.json's forced keys, in the legacy profile's order.
  ASSERT_EQ(profile->forced.size(), 2u);
  EXPECT_EQ(profile->forced[0].first.key, "WiiSDCard");
  EXPECT_EQ(profile->forced[0].second, "True");
  EXPECT_EQ(profile->forced[1].first.key, "CustomRTCValue");
  EXPECT_EQ(profile->forced[1].second, "1735689600");
  ASSERT_TRUE(profile->mod);
  EXPECT_EQ(profile->mod->tree.hash, Orca::Mod::TreeHash(b.files));
  EXPECT_EQ(profile->mod->tree.Under("sd/").size(), 1u);

  // The same with a vanilla Brawl bundle: no launcher, no card.
  Bundle v = MakeBundle(BRAWL_MOD);
  v.WriteTree();
  const auto brawl = Orca::LoadModProfile(v.dir, &error);
  ASSERT_TRUE(brawl) << error;
  EXPECT_EQ(brawl->game_id, "RSBE01");
  EXPECT_FALSE(brawl->IsLauncher());
  EXPECT_TRUE(brawl->forced.empty());
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(v.dir);
}

#ifndef _WIN32  // A symbolic link needs an elevated or developer-mode Windows.
TEST(OrcaMod, LocalTreeRefusesLinkedFiles)
{
  for (const char* name : {"tree.json", "mod.json", "launcher.dol", "sd/codes.txt",
                          "orca/patches/local.patches", "orca/jitwarm"})
  {
    SCOPED_TRACE(name);
    Bundle b = MakeBundle(PPLUS_MOD);
    const std::string outside = File::CreateTempDir() + "/";
    const Common::ScopeGuard done([&] {
      Orca::Mod::SetLocalPack({});
      File::DeleteDirRecursively(b.dir);
      File::DeleteDirRecursively(outside);
    });
    b.Add("launcher.dol", "launcher");
    b.Add("sd/codes.txt", "codes");
    b.Add("orca/patches/local.patches", "# none\n");
    b.Add("orca/jitwarm", "warm");
    b.WriteTree();
    Orca::Mod::SetLocalPack(b.dir);
    std::string error;
    ASSERT_TRUE(Orca::LoadModProfile(b.dir, &error)) << error;
    ASSERT_TRUE(File::Rename(b.dir + name, outside + "source"));
    std::error_code ec;
    std::filesystem::create_symlink(outside + "source", b.dir + name, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find(std::string(name) + " is a link"), std::string::npos) << error;
    Orca::Mod::ResolveBundleForTests(nullptr, {}, false);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::Refused);
    EXPECT_NE(Orca::Mod::ResolvedError().find(std::string(name) + " is a link"),
              std::string::npos);
    EXPECT_EQ(Orca::Mod::ResolvedError().find(outside), std::string::npos);
    // App-delivered trees retain their existing handling of links.
    Orca::Mod::SetLocalPack({});
    EXPECT_TRUE(Orca::LoadModProfile(b.dir, &error)) << error;
  }
}

TEST(OrcaMod, LocalTreeRefusesLinkedDirectoryEscape)
{
  const std::string root = File::CreateTempDir() + "/";
  Bundle b{root + "pack/", {}};
  const Common::ScopeGuard done([&] {
    Orca::Mod::SetLocalPack({});
    File::DeleteDirRecursively(root);
  });
  b.Add("mod.json", BRAWL_MOD);
  b.Add("sd/codes.txt", "codes");
  b.WriteTree();
  // The sibling shares the pack's name prefix, but is outside its canonical folder.
  ASSERT_TRUE(File::Rename(b.dir + "sd", root + "pack-other"));
  std::error_code ec;
  std::filesystem::create_directory_symlink(root + "pack-other", b.dir + "sd", ec);
  ASSERT_FALSE(ec) << ec.message();
  Orca::Mod::SetLocalPack(b.dir);
  Orca::Mod::ResolveBundleForTests(nullptr, {}, false);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::Refused);
  EXPECT_NE(Orca::Mod::ResolvedError().find("sd/codes.txt leads outside the pack folder"),
            std::string::npos) << Orca::Mod::ResolvedError();
  Orca::Mod::SetLocalPack({});
  std::string error;
  EXPECT_TRUE(Orca::LoadModProfile(b.dir, &error)) << error;
}

TEST(OrcaMod, LocalTreeRefusesSpecialFiles)
{
  for (const char* name : {"tree.json", "mod.json", "sd/codes.txt"})
  {
    SCOPED_TRACE(name);
    Bundle b = MakeBundle(BRAWL_MOD);
    const Common::ScopeGuard done([&] {
      Orca::Mod::SetLocalPack({});
      File::DeleteDirRecursively(b.dir);
    });
    b.Add("sd/codes.txt", "");
    b.WriteTree();
    ASSERT_TRUE(File::Delete(b.dir + name));
    ASSERT_EQ(mkfifo((b.dir + name).c_str(), 0600), 0);
    Orca::Mod::SetLocalPack(b.dir);
    std::string error;
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find(std::string(name) + " is not a regular file"), std::string::npos) << error;
  }
}

TEST(OrcaMod, LocalSdSourcesCheckedAtBuildTime)
{
  for (const bool linked_directory : {false, true})
  {
    SCOPED_TRACE(linked_directory);
    Bundle b = MakeBundle(BrawlWith("sd", R"({"bytes": 67108864})"));
    const std::string outside = File::CreateTempDir() + "/";
    const Common::ScopeGuard done([&] {
      Orca::Mod::SetLocalPack({});
      File::DeleteDirRecursively(b.dir);
      File::DeleteDirRecursively(outside);
    });
    b.Add("sd/codes.txt", "codes");
    b.WriteTree();
    Orca::Mod::SetLocalPack(b.dir);
    std::string error;
    const auto profile = Orca::LoadModProfile(b.dir, &error);
    ASSERT_TRUE(profile) << error;
    const std::string name = linked_directory ? "sd" : "sd/codes.txt";
    ASSERT_TRUE(File::Rename(b.dir + name, outside + "source"));
    std::error_code ec;
    if (linked_directory)
      std::filesystem::create_directory_symlink(outside + "source", b.dir + name, ec);
    else
      std::filesystem::create_symlink(outside + "source", b.dir + name, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_FALSE(Orca::Mod::PrepareSdImage(*profile->mod, outside + "cache", &error));
    EXPECT_NE(error.find(linked_directory ? "leads outside the pack folder" : "is a link"),
              std::string::npos) << error;
    EXPECT_EQ(error.find(outside), std::string::npos);
  }
}
#endif

TEST(OrcaMod, RefusesABundleThatDiffersFromItsTree)
{
  std::string error;
  {
    // A file whose size isn't tree.json's.
    Bundle b = MakeBundle(BRAWL_MOD);
    b.Add("orca/patches/a.patches", "# x\n");
    b.WriteTree();
    ASSERT_TRUE(File::WriteStringToFile(b.dir + "orca/patches/a.patches", "# xy\n"));
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find("size"), std::string::npos) << error;
    File::DeleteDirRecursively(b.dir);
  }
  {
    // The same size, other bytes: refused for what reaches the game (mod.json, patches, launcher).
    Bundle b = MakeBundle(BRAWL_MOD);
    b.Add("orca/patches/a.patches", "# x\n");
    b.WriteTree();
    ASSERT_TRUE(File::WriteStringToFile(b.dir + "orca/patches/a.patches", "# y\n"));
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find("sha256"), std::string::npos) << error;
    File::DeleteDirRecursively(b.dir);
  }
  {
    // A listed file that isn't there.
    Bundle b = MakeBundle(BRAWL_MOD);
    b.Add("orca/textures/tex1_1x1_0.png", "png");
    b.WriteTree();
    File::Delete(b.dir + "orca/textures/tex1_1x1_0.png");
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    File::DeleteDirRecursively(b.dir);
  }
  {
    // A launcher mod.json names that isn't in the tree.
    Bundle b = MakeBundle(BrawlWith("launcher", R"("launcher.dol")"));
    b.WriteTree();
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    File::DeleteDirRecursively(b.dir);
  }
  {
    // Patches that don't parse.
    Bundle b = MakeBundle(BRAWL_MOD);
    b.Add("orca/patches/a.patches", "not a patch\n");
    b.WriteTree();
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find("a.patches"), std::string::npos) << error;
    File::DeleteDirRecursively(b.dir);
  }
  {
    // A launcher whose extension isn't lower case is hash-checked too.
    Bundle b = MakeBundle(BrawlWith("launcher", R"("BOOT.DOL")"));
    b.Add("BOOT.DOL", std::string(64, '\x01'));
    b.WriteTree();
    ASSERT_TRUE(Orca::LoadModProfile(b.dir, &error)) << error;
    ASSERT_TRUE(File::WriteStringToFile(b.dir + "BOOT.DOL", std::string(64, '\x02')));
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find("sha256"), std::string::npos) << error;
    File::DeleteDirRecursively(b.dir);
  }
  {
    // mod.json edited after tree.json was written, at the same size.
    Bundle b = MakeBundle(BRAWL_MOD);
    b.WriteTree();
    std::string edited = BRAWL_MOD;
    edited.replace(edited.find("1800"), 4, "1801");
    ASSERT_TRUE(File::WriteStringToFile(b.dir + "mod.json", edited));
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find("sha256"), std::string::npos) << error;
    File::DeleteDirRecursively(b.dir);
  }
  {
    // A file the session reads whole, over its limit, is refused before it is read.
    Bundle b = MakeBundle(BRAWL_MOD);
    b.Add("orca/patches/big.patches", std::string(Orca::Mod::MAX_PATCHES_BYTES + 1, '\n'));
    b.WriteTree();
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    EXPECT_NE(error.find("over"), std::string::npos) << error;
    File::DeleteDirRecursively(b.dir);
  }
  {
    // No tree.json, or a malformed mod.json.
    Bundle b = MakeBundle("{}");
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    b.WriteTree();
    EXPECT_FALSE(Orca::LoadModProfile(b.dir, &error));
    File::DeleteDirRecursively(b.dir);
  }
}

// mod.json and tree.json are size-limited and nesting-limited before picojson sees them (it recurses
// once per level: 20 KB of brackets would overflow Windows' 1 MiB main-thread stack).
TEST(OrcaMod, RefusesOversizedOrDeepJson)
{
  std::string error;
  const auto deep = [](int levels) {
    return R"({"format":)" + std::string(levels, '[') + std::string(levels, ']') + "}";
  };
  // 40 KB (under mod.json's limit) and 100 KB: either crashes picojson on Windows' main thread.
  EXPECT_FALSE(Orca::Mod::ParseManifest(deep(20000), &error));
  EXPECT_NE(error.find("nested"), std::string::npos) << error;
  EXPECT_FALSE(Orca::Mod::ParseTree(deep(50000), &error));
  EXPECT_NE(error.find("nested"), std::string::npos) << error;
  // Seventeen levels are refused, sixteen parse (and then fail on content, not depth).
  const auto nest = [](int levels) {
    return R"({"format":)" + std::string(levels - 1, '[') + std::string(levels - 1, ']') + "}";
  };
  EXPECT_FALSE(Orca::Mod::ParseManifest(nest(17), &error));
  EXPECT_NE(error.find("nested"), std::string::npos) << error;
  EXPECT_FALSE(Orca::Mod::ParseManifest(nest(16), &error));
  EXPECT_EQ(error.find("nested"), std::string::npos) << error;
  // Brackets inside strings don't count.
  EXPECT_TRUE(Orca::Mod::ParseManifest(BrawlWith("title", R"("[[[[[[[[[[[[[[[[[[[[\"[[[[")"), &error))
      << error;
  // Over 64 KiB: refused, even as trailing whitespace.
  EXPECT_FALSE(Orca::Mod::ParseManifest(BRAWL_MOD + std::string(Orca::Mod::MAX_MOD_JSON_BYTES, ' '),
                                        &error));
  EXPECT_NE(error.find("over"), std::string::npos) << error;
}

TEST(OrcaMod, RefusesHugeJsonNumbersAndTrailingValues)
{
  std::string error;
  for (const auto& number : {std::string("1e999"), std::string(400, '9')})
  {
    EXPECT_FALSE(Orca::Mod::ParseManifest(R"({"format":)" + number + "}", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(Orca::Mod::ParseTree(R"({"files":[{"bytes":)" + number + "}]}", &error));
    EXPECT_FALSE(error.empty());
  }
  EXPECT_FALSE(Orca::Mod::ParseManifest(std::string(BRAWL_MOD) + " {}", &error));
  EXPECT_NE(error.find("after the JSON value"), std::string::npos);
}

// The card is built from the listed files only, and the same files and size give the same bytes.
TEST(OrcaMod, SdImageIsDeterministic)
{
  const std::string dir = File::CreateTempDir() + "/";
  ASSERT_TRUE(File::WriteStringToFile(dir + "a.txt", "alpha"));
  ASSERT_TRUE(File::WriteStringToFile(dir + "b.bin", std::string(100000, 'b')));
  ASSERT_TRUE(File::WriteStringToFile(dir + "stray", "not listed"));
  const std::vector<Orca::Mod::SdSource> files{
      {"Project+/pf/b.bin", dir + "b.bin", 100000},
      {"Project+/RSBE01.txt", dir + "a.txt", 5},
      {"z.txt", dir + "a.txt", 5},
  };
  constexpr u64 BYTES = 64 << 20;
  std::string error;
  ASSERT_TRUE(Orca::Mod::BuildSdImage(files, BYTES, dir + "one.raw", &error)) << error;
  std::vector<Orca::Mod::SdSource> reversed(files.rbegin(), files.rend());
  ASSERT_TRUE(Orca::Mod::BuildSdImage(reversed, BYTES, dir + "two.raw", &error)) << error;
  EXPECT_EQ(File::GetSize(dir + "one.raw"), BYTES);
  const std::string one = Orca::HashFile(dir + "one.raw");
  EXPECT_FALSE(one.empty());
  EXPECT_EQ(one, Orca::HashFile(dir + "two.raw"));
  // Pinned, so the PC's run proves it builds the Mac's bytes (and every later builder the same).
  EXPECT_EQ(one, "0da52d2d776c7a5b5caeeeb2cd106ed1");
  std::string bytes;
  ASSERT_TRUE(File::ReadFileToString(dir + "one.raw", bytes));
  EXPECT_NE(bytes.find("alpha"), std::string::npos);
  EXPECT_EQ(bytes.find("not listed"), std::string::npos);
  // A pinned geometry: 2 KiB clusters and two FATs in the boot sector, the same bytes twice.
  constexpr u64 GEOMETRY_BYTES = 160 << 20;  // FAT32 at 2 KiB clusters needs over 65,525 of them
  ASSERT_TRUE(Orca::Mod::BuildSdImage(files, GEOMETRY_BYTES, dir + "g1.raw", &error, 2048, 2))
      << error;
  ASSERT_TRUE(Orca::Mod::BuildSdImage(reversed, GEOMETRY_BYTES, dir + "g2.raw", &error, 2048, 2))
      << error;
  EXPECT_EQ(Orca::HashFile(dir + "g1.raw"), Orca::HashFile(dir + "g2.raw"));
  EXPECT_EQ(Orca::HashFile(dir + "g1.raw"), "1a80cd449366c2cab73a447e410d91d0");
  EXPECT_NE(Orca::HashFile(dir + "g1.raw"), one);
  {
    std::string boot;
    ASSERT_TRUE(File::ReadFileToString(dir + "g1.raw", boot));
    EXPECT_EQ(static_cast<u8>(boot[13]), 4);   // sectors per cluster
    EXPECT_EQ(static_cast<u8>(boot[16]), 2);   // FATs
  }
  // Each folder's files, then its folders, by upper-cased name: z.txt before Project+ at the root.
  {
    std::string image;
    ASSERT_TRUE(File::ReadFileToString(dir + "g1.raw", image));
    const size_t z = image.find("Z       TXT"), project = image.find("PROJEC~1   ");
    ASSERT_NE(z, std::string::npos);
    ASSERT_NE(project, std::string::npos);
    EXPECT_LT(z, project);
  }
  EXPECT_FALSE(Orca::Mod::BuildSdImage(files, GEOMETRY_BYTES, dir + "g3.raw", &error, 3000, 1));
  EXPECT_FALSE(Orca::Mod::BuildSdImage(files, BYTES, dir + "g4.raw", &error, 2048, 2));
  // A source whose size changed is refused.
  const std::vector<Orca::Mod::SdSource> wrong{{"a.txt", dir + "a.txt", 6}};
  EXPECT_FALSE(Orca::Mod::BuildSdImage(wrong, BYTES, dir + "three.raw", &error));
  // Too small a card for the files.
  const std::vector<Orca::Mod::SdSource> big{{"b.bin", dir + "b.bin", 100000}};
  EXPECT_FALSE(Orca::Mod::BuildSdImage(big, 32768, dir + "four.raw", &error));
  File::DeleteDirRecursively(dir);
}

// The cache: built once, then reused while it hashes as recorded; a damaged one is rebuilt; at
// most the newest two images stay.
TEST(OrcaMod, SdImageCache)
{
  Bundle b = MakeBundle(BrawlWith("sd", R"({"bytes": 67108864})"));
  b.Add("sd/RSBE01.txt", "codes");
  b.WriteTree();
  std::string error;
  auto profile = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(profile) << error;
  const std::string cache = File::CreateTempDir() + "/sd";
  const auto first = Orca::Mod::PrepareSdImage(*profile->mod, cache, &error);
  ASSERT_TRUE(first) << error;
  EXPECT_TRUE(first->built);
  EXPECT_EQ(first->hash, Orca::HashFile(first->path));
  EXPECT_NE(first->path.find(Orca::Mod::SdTreeHash(profile->mod->tree, 67108864)),
            std::string::npos);
  const auto again = Orca::Mod::PrepareSdImage(*profile->mod, cache, &error);
  ASSERT_TRUE(again) << error;
  EXPECT_FALSE(again->built);
  EXPECT_EQ(again->hash, first->hash);
  {
    File::IOFile f(first->path, "r+b");
    f.Seek(1 << 20, File::SeekOrigin::Begin);
    f.WriteBytes("damage", 6);
  }
  const auto rebuilt = Orca::Mod::PrepareSdImage(*profile->mod, cache, &error);
  ASSERT_TRUE(rebuilt) << error;
  EXPECT_TRUE(rebuilt->built);
  EXPECT_EQ(rebuilt->hash, first->hash);
  // Two other cards: the oldest of three goes.
  for (const char* size : {"67141632", "67174400"})
  {
    Bundle other = MakeBundle(BrawlWith("sd", fmt::format(R"({{"bytes": {}}})", size)));
    other.WriteTree();
    auto p = Orca::LoadModProfile(other.dir, &error);
    ASSERT_TRUE(p) << error;
    ASSERT_TRUE(Orca::Mod::PrepareSdImage(*p->mod, cache, &error)) << error;
    File::DeleteDirRecursively(other.dir);
  }
  int images = 0;
  for (const auto& e : std::filesystem::directory_iterator(cache))
    images += e.path().extension() == ".raw";
  EXPECT_EQ(images, 2);
  EXPECT_FALSE(File::Exists(first->path));
  // No cache folder: refused.
  EXPECT_FALSE(Orca::Mod::PrepareSdImage(*profile->mod, "", &error));
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(cache);
}

// A guard word is code the game never rewrites, so Orca's own game patches must never write one
// (0.3.33's RSBE01.patches moved Brawl's v-sync call, which the vanilla guard used to read). The
// vanilla guard is a word Project+'s codes rewrite (PPLUS32.ini KeepGameCode), so brawl-rev2 never
// holds on Project+.
TEST(OrcaMod, GuardsAreNotOrcasOwnPatches)
{
  std::set<u32> patched;
  for (const char* name : {"RSBE01.patches", "PPLUS32.patches"})
  {
    std::string text, error;
    ASSERT_TRUE(File::ReadFileToString(RepoPath(std::string("Data/Sys/Orca/") + name), text)) << name;
    const auto patches = Orca::UX::ParseGamePatches(text, &error);
    ASSERT_TRUE(patches) << name << ": " << error;
    for (const auto& p : *patches)
    {
      if (!p.original || *p.original != p.value)
        patched.insert(p.address);
    }
  }
  ASSERT_FALSE(patched.empty());
  for (const Builtin builtin : {Builtin::BrawlRev2, Builtin::BrawlRev2PPlus})
  {
    for (const auto& g : Orca::Mod::BuiltinGuards(builtin))
      EXPECT_FALSE(patched.contains(g.address)) << std::hex << g.address;
  }
  std::string error;
  const auto pplus = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "PPLUS32", 2, &error);
  ASSERT_TRUE(pplus) << error;
  std::map<u32, u32> pplus_words;
  for (const auto& k : pplus->kept_code)
    pplus_words[k.address] = k.loader;
  bool vanilla_only = false;
  for (const auto& g : Orca::Mod::BuiltinGuards(Builtin::BrawlRev2))
  {
    if (pplus_words.contains(g.address) && pplus_words[g.address] != g.word)
      vanilla_only = true;
  }
  EXPECT_TRUE(vanilla_only);
}

namespace
{
std::string FlatPPlusGuards(const std::string& guards)
{
  picojson::value value;
  picojson::parse(value, BRAWL_MOD);
  auto& object = value.get<picojson::object>();
  object["format"] = picojson::value(2.0);
  object["listing"] = picojson::value("pplus-fork");
  object["serial"] = picojson::value(1.0);
  object["builtin"] = picojson::value("brawl-rev2-pplus");
  picojson::value addresses;
  EXPECT_EQ(picojson::parse(addresses, guards), "");
  object["pplusGuards"] = addresses;
  return value.serialize();
}
constexpr char PPLUS_EX_GUARDS[] =
    R"({"rssExdata":"0x80551DD4","pageIndex":"0x80552B70","currentPage":"0x80552F90"})";
}  // namespace

TEST(OrcaMod, PPlusGuardAddressesAreValidated)
{
  std::string error;
  const auto manifest = Orca::Mod::ParseManifest(FlatPPlusGuards(PPLUS_EX_GUARDS), &error);
  ASSERT_TRUE(manifest) << error;
  ASSERT_TRUE(manifest->pplus_guards);
  EXPECT_EQ(*manifest->pplus_guards, (Orca::Mod::PPlusGuardAddresses{0x80551DD4, 0x80552B70, 0x80552F90}));
  const auto defaults = Orca::Mod::ParseManifest(PPLUS_MOD, &error);
  ASSERT_TRUE(defaults) << error;
  EXPECT_FALSE(defaults->pplus_guards);
  const auto words = Orca::Mod::BuiltinGuards(Builtin::BrawlRev2PPlus);
  ASSERT_EQ(words.size(), 14u);
  EXPECT_EQ(words[8].address, 0x80553594u);
  EXPECT_EQ(words[10].address, 0x80554320u);
  EXPECT_EQ(words[12].address, 0x805518D0u);
  for (const char* invalid : {
           R"(null)", R"({})",
           R"({"rssExdata":"0x80551DD4","pageIndex":"0x80552B70"})",
           R"({"rssExdata":"0x80551DD5","pageIndex":"0x80552B70","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x8055000C","pageIndex":"0x80552B70","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x8059FFFC","pageIndex":"0x80552B70","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x805A0000","pageIndex":"0x80552B70","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x90551DD4","pageIndex":"0x80552B70","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x80551DD4","pageIndex":"0x80551DD4","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x80551DD4","pageIndex":"0x80551DD8","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x80551DD4","pageIndex":"0x80551DD0","currentPage":"0x80552F90"})",
           R"({"rssExdata":"0x80551DD4","pageIndex":"0x80552B70","currentPage":"0x80552B74"})",
           R"({"rssExdata":"0x80551DD4","pageIndex":"0x80552B70","currentPage":"0x80552F90","word":"0x1234"})"})
  {
    EXPECT_TRUE(Refused(FlatPPlusGuards(invalid))) << invalid;
  }
  EXPECT_TRUE(Orca::Mod::ParseManifest(FlatPPlusGuards(
      R"({"rssExdata":"0x80550010","pageIndex":"0x80550018","currentPage":"0x8059FFF8"})"), &error)) << error;
  picojson::value value;
  picojson::parse(value, FlatPPlusGuards(PPLUS_EX_GUARDS));
  auto& object = value.get<picojson::object>();
  object["builtin"] = picojson::value("brawl-rev2");
  EXPECT_FALSE(Orca::Mod::ParseManifest(value.serialize(), &error));
  EXPECT_NE(error.find("requires builtin brawl-rev2-pplus"), std::string::npos);
  object.erase("builtin");
  object.erase("offers");
  EXPECT_FALSE(Orca::Mod::ParseManifest(value.serialize(), &error));
  EXPECT_NE(error.find("requires builtin brawl-rev2-pplus"), std::string::npos);
  object["format"] = picojson::value(1.0);
  object.erase("listing");
  object.erase("serial");
  object["builtin"] = picojson::value("brawl-rev2-pplus");
  EXPECT_TRUE(Refused(value.serialize()));
}

TEST(OrcaMod, PPlusGuardsReadTheDeclaredPairs)
{
  std::string error;
  const auto manifest = Orca::Mod::ParseManifest(FlatPPlusGuards(PPLUS_EX_GUARDS), &error);
  ASSERT_TRUE(manifest) << error;
  std::map<u32, u32> memory;
  const auto guards = Orca::Mod::BuiltinGuards(Builtin::BrawlRev2PPlus);
  for (size_t i = 0; i < 8; ++i)
    memory[guards[i].address] = guards[i].word;
  // Explicit fixture: no words at the P+ 3.2 codeset addresses.
  memory.insert({{0x80551DD4, 0x3CA08042}, {0x80551DD8, 0x60A5C4E8},
                 {0x80552B70, 0x3C608042}, {0x80552B74, 0x6063C821},
                 {0x80552F90, 0x3E008049}, {0x80552F94, 0x62106000}});
  size_t reads = 0;
  const auto read = [&](u32 address) -> std::optional<u32> {
    ++reads;
    const auto it = memory.find(address);
    return it == memory.end() ? std::nullopt : std::optional(it->second);
  };
  EXPECT_TRUE(Orca::Mod::GuardsHold(manifest->builtin, read, manifest->pplus_guards));
  EXPECT_EQ(reads, 14u);
  EXPECT_FALSE(Orca::Mod::GuardsHold(manifest->builtin, read));
  for (auto& [address, word] : memory)
  {
    word ^= 1;
    EXPECT_FALSE(Orca::Mod::GuardsHold(manifest->builtin, read, manifest->pplus_guards)) << std::hex << address;
    word ^= 1;
  }
  memory.erase(0x80552F94);
  EXPECT_FALSE(Orca::Mod::GuardsHold(manifest->builtin, read, manifest->pplus_guards));
}

TEST(OrcaMod, SchemaKnowsPPlusGuards)
{
  const picojson::value schema = ReadJson(RepoPath("Tools/orca/mods/mod.schema.json"));
  std::vector<std::string> required;
  for (const auto& key :
       schema.get("properties").get("pplusGuards").get("required").get<picojson::array>())
  {
    required.push_back(key.get<std::string>());
  }
  EXPECT_EQ(required, (std::vector<std::string>{"rssExdata", "pageIndex", "currentPage"}));
  // The address pattern accepts exactly what the engine accepts for one pair, the other two kept
  // where they don't overlap it.
  const std::regex pattern(
      schema.get("$defs").get("pplusGuardWord").get("pattern").get<std::string>(),
      std::regex::ECMAScript);
  for (const char* address :
       {"0x80550010", "0x80550014", "0x8055000C", "0x80550000", "0x8054FFFC", "0x80551DD4",
        "0x80551dd4", "0X80551DD4", "0x80551DD6", "0x80551DD5", "0x80580000", "0x8059FFF8",
        "0x8059fff8", "0x8059FFFC", "0x8059FFF4", "0x805A0000", "0x805A0004", "0x8155D000",
        "0x0080551DD4", "0x551DD4", "80551DD4"})
  {
    const std::string json = FlatPPlusGuards(
        fmt::format(R"({{"rssExdata":"{}","pageIndex":"0x80558000","currentPage":"0x80558010"}})",
                    address));
    EXPECT_EQ(std::regex_match(std::string(address), pattern), !Refused(json)) << address;
  }
}

TEST(OrcaMod, Guards)
{
  for (const Builtin builtin : {Builtin::BrawlRev2, Builtin::BrawlRev2PPlus})
  {
    const auto guards = Orca::Mod::BuiltinGuards(builtin);
    ASSERT_GE(guards.size(), 4u);
    const auto memory = [&guards](std::optional<u32> broken) {
      return [&guards, broken](u32 address) -> std::optional<u32> {
        for (const auto& g : guards)
        {
          if (g.address == address)
            return broken == address ? g.word ^ 1 : g.word;
        }
        return std::nullopt;
      };
    };
    EXPECT_TRUE(Orca::Mod::GuardsHold(builtin, memory(std::nullopt)));
    for (const auto& g : guards)
      EXPECT_FALSE(Orca::Mod::GuardsHold(builtin, memory(g.address))) << g.address;
    EXPECT_FALSE(Orca::Mod::GuardsHold(builtin,
                                       [](u32) -> std::optional<u32> { return std::nullopt; }));
  }
  EXPECT_FALSE(Orca::Mod::GuardsHold(Builtin::None,
                                     [](u32) -> std::optional<u32> { return 0; }));
}

// The whole legacy caps line, word for word, for each ruleset profile (Brawl rev 2 and Project+'s
// launcher): the mod-mode branch in OfferedCaps must never reorder or drop a legacy word, and the
// engine caps stay last.
TEST(OrcaMod, LegacyCapsLinesArePinned)
{
  const std::string line = "join leave stats pause delay perf direct host chat yougame gfx music "
                           "results locks queue2 pick-casual pick-ranked talk rumble record hostui";
  std::string error;
  const auto brawl = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "RSBE01", 2, &error);
  ASSERT_TRUE(brawl) << error;
  Orca::SetActiveProfile(brawl);
  ASSERT_EQ(Orca::Mod::ActiveMod(), nullptr);
  EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::UX::Rules::Ruleset::Brawl);
  EXPECT_EQ(Orca::Status::OfferedCaps(), line);
  const auto pplus = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "PPLUS32", 2, &error);
  ASSERT_TRUE(pplus) << error;
  Orca::SetActiveProfile(pplus);
  ASSERT_EQ(Orca::Mod::ActiveMod(), nullptr);
  EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::UX::Rules::Ruleset::PPlus);
  EXPECT_EQ(Orca::Status::OfferedCaps(), line);
  Orca::SetActiveProfile(std::nullopt);
}

// Mod mode's gates: a builtin's modules stay off until its guards were checked against memory
// (RefreshGuards, at the frame hook), and a mod's caps are what it offers.
TEST(OrcaMod, ModeGates)
{
  Bundle b = MakeBundle(BRAWL_MOD);
  b.WriteTree();
  std::string error;
  auto profile = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(profile) << error;
  Orca::SetActiveProfile(profile);
  ASSERT_NE(Orca::Mod::ActiveMod(), nullptr);
  EXPECT_EQ(Orca::Mod::DeclaredBuiltin(), Builtin::BrawlRev2);
  EXPECT_FALSE(Orca::Mod::BuiltinOn(Builtin::BrawlRev2));
  EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::UX::Rules::Ruleset::None);
  EXPECT_FALSE(Orca::UX::ResultsVerified());
  // A fresh start's origin at the built main menu (0.3.34) is decided as the match is set up, before
  // any guard is read: by the declared builtin, so a mod boots to the same origin as legacy.
  EXPECT_TRUE(Orca::UX::FreshMove::Supported());
  // A builtin's caps are never offered before its guards hold (they are checked at the frame hook).
  // The music switch is host-only and follows the disc as in legacy (Music::Supported), guards or
  // not; the pick caps go with "locks". The engine caps "talk", "rumble" and "record" are offered in
  // every mode, last, guards or not.
  const std::string tail = fmt::format(" {} {} {} {}", Orca::Status::TALK_CAP,
                                       Orca::Status::RUMBLE_CAP, Orca::Status::RECORD_CAP,
                                       Orca::Status::HOSTUI_CAP);
  const std::string base =
      std::string(Orca::Status::CAPS) + " " + Orca::Status::MUSIC_CAP + tail;
  const std::string all = std::string(Orca::Status::CAPS) + " " + Orca::Status::MUSIC_CAP +
                          " results locks queue2 " + Orca::Status::PICK_CAPS + tail;
  EXPECT_EQ(Orca::Status::OfferedCaps(), base);
  EXPECT_FALSE(Orca::Mod::GuardsDecided());
  // Not holding yet in the first frames (a loader's codes still landing): undecided, nothing offered.
  Orca::Mod::NoteGuards(false, 1);
  EXPECT_FALSE(Orca::Mod::GuardsDecided());
  EXPECT_EQ(Orca::Status::OfferedCaps(), base);
  // Held: decided, offered, the modules on.
  Orca::Mod::NoteGuards(true, 2);
  EXPECT_TRUE(Orca::Mod::GuardsDecided());
  EXPECT_TRUE(Orca::Mod::BuiltinOn(Builtin::BrawlRev2));
  EXPECT_EQ(Orca::Status::OfferedCaps(), all);
  // Broken after holding (the mod rewrote what the modules rely on): the caps are withdrawn for the
  // boot; the modules follow memory alone, so they run again wherever the words hold again.
  Orca::Mod::NoteGuards(false, 500);
  EXPECT_TRUE(Orca::Mod::GuardsFailed());
  EXPECT_FALSE(Orca::Mod::BuiltinOn(Builtin::BrawlRev2));
  EXPECT_EQ(Orca::Status::OfferedCaps(), base);
  Orca::Mod::NoteGuards(true, 501);
  EXPECT_TRUE(Orca::Mod::BuiltinOn(Builtin::BrawlRev2));
  EXPECT_TRUE(Orca::Mod::GuardsFailed());
  EXPECT_EQ(Orca::Status::OfferedCaps(), base);
  // A new boot: never holding by firstKeyframeFrame is a failure (decided, nothing offered).
  Orca::SetActiveProfile(profile);
  EXPECT_FALSE(Orca::Mod::GuardsDecided());
  Orca::Mod::NoteGuards(false, 299);
  EXPECT_FALSE(Orca::Mod::GuardsDecided());
  Orca::Mod::NoteGuards(false, 300);
  EXPECT_TRUE(Orca::Mod::GuardsDecided());
  EXPECT_TRUE(Orca::Mod::GuardsFailed());
  EXPECT_EQ(Orca::Status::OfferedCaps(), base);
  Orca::SetActiveProfile(profile);
  EXPECT_EQ(Orca::UX::FriendsMove::FirstFriendFrame(), 300);

  picojson::value plain;
  picojson::parse(plain, BRAWL_MOD);
  plain.get<picojson::object>().erase("builtin");
  plain.get<picojson::object>().erase("offers");
  plain.get<picojson::object>()["firstKeyframeFrame"] = picojson::value(600.0);
  Bundle n = MakeBundle(plain.serialize());
  n.WriteTree();
  auto none = Orca::LoadModProfile(n.dir, &error);
  ASSERT_TRUE(none) << error;
  Orca::SetActiveProfile(none);
  EXPECT_EQ(Orca::Mod::DeclaredBuiltin(), Builtin::None);
  EXPECT_FALSE(Orca::UX::FreshMove::Supported());
  // No builtin: no builtin caps; the music switch still follows the disc (RSBE01).
  EXPECT_EQ(Orca::Status::OfferedCaps(), base);
  EXPECT_TRUE(Orca::Mod::GuardsDecided());  // nothing to wait for
  EXPECT_EQ(Orca::Mod::FirstKeyframeFrame(), 600u);
  EXPECT_EQ(Orca::UX::FriendsMove::FirstFriendFrame(), 600);

  // Legacy: exactly today's.
  const auto legacy = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "RSBE01", 2, &error);
  ASSERT_TRUE(legacy) << error;
  Orca::SetActiveProfile(legacy);
  EXPECT_EQ(Orca::Mod::ActiveMod(), nullptr);
  EXPECT_TRUE(Orca::Mod::GuardsDecided());
  EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::UX::Rules::Ruleset::Brawl);
  EXPECT_TRUE(Orca::UX::ResultsVerified());
  EXPECT_EQ(Orca::Status::OfferedCaps(), all);
  EXPECT_EQ(Orca::UX::FriendsMove::FirstFriendFrame(), Orca::UX::FriendsMove::FIRST_FRIEND_FRAME);
  EXPECT_TRUE(Orca::UX::FreshMove::Supported());
  Orca::SetActiveProfile(std::nullopt);
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(n.dir);
}

// S3: which game a session runs is the builtin module's answer (Module::Describe, Active,
// ProfileRuleset over BrawlGame::Identify), for every kind of profile, legacy and mod mode, and the
// predicates the game files and the engine asked before S3 (each compared ids itself) agree.
TEST(OrcaMod, GameIdentityIsTheModules)
{
  // No active profile afterwards, whatever fails.
  struct ProfileDone
  {
    ~ProfileDone() { Orca::SetActiveProfile(std::nullopt); }
  } profile_done;
  using Orca::Engine::Ruleset;
  Orca::UX::BrawlModule& module = Orca::UX::Builtin();
  const auto expect = [&module](std::string_view id, Ruleset ruleset, bool on, bool music) {
    const Orca::Engine::Manifest game = module.Describe();
    EXPECT_EQ(game.id, id);
    EXPECT_EQ(game.ruleset, ruleset);
    EXPECT_EQ(game.results, !id.empty());
    EXPECT_EQ(game.music, music);
    EXPECT_EQ(module.Active(), on);
    EXPECT_EQ(module.ProfileRuleset(), on ? ruleset : Ruleset::None);
    EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), module.ProfileRuleset());
    EXPECT_EQ(Orca::UX::ResultsVerified(), on);
    EXPECT_EQ(Orca::UX::FreshMove::Supported(), !id.empty());
    EXPECT_EQ(Orca::Music::Supported(), music);
  };
  {
    SCOPED_TRACE("no profile");
    Orca::SetActiveProfile(std::nullopt);
    expect("", Ruleset::None, false, false);
  }
  Orca::Profile disc;
  disc.game_id = "RSBE01";
  disc.revision = 2;
  {
    SCOPED_TRACE("Brawl rev 2");
    Orca::SetActiveProfile(disc);
    expect("brawl-rev2", Ruleset::Brawl, true, true);
  }
  {
    SCOPED_TRACE("Brawl, another revision: the music switch only");
    disc.revision = 1;
    Orca::SetActiveProfile(disc);
    expect("", Ruleset::None, false, true);
  }
  {
    SCOPED_TRACE("another disc");
    disc.game_id = "RMCE01";
    disc.revision = 2;
    Orca::SetActiveProfile(disc);
    expect("", Ruleset::None, false, false);
  }
  Orca::Profile launcher;
  launcher.game_id = "PPLUS32";
  launcher.revision = 2;
  launcher.launcher_hash = std::string(32, '0');
  launcher.disc = "RSBE01";
  {
    SCOPED_TRACE("Project+'s launcher on Brawl rev 2");
    Orca::SetActiveProfile(launcher);
    expect("brawl-rev2-pplus", Ruleset::PPlus, true, true);
  }
  {
    SCOPED_TRACE("another launcher build on Brawl rev 2: no ruleset, the rest");
    launcher.game_id = "LXP";
    Orca::SetActiveProfile(launcher);
    expect("brawl-rev2-launcher", Ruleset::None, true, true);
  }
  {
    SCOPED_TRACE("Project+'s launcher on another disc");
    launcher.game_id = "PPLUS32";
    launcher.disc = "RSBP01";
    Orca::SetActiveProfile(launcher);
    expect("", Ruleset::None, false, false);
  }
  {
    SCOPED_TRACE("Project+'s launcher on Brawl, another revision");
    launcher.disc = "RSBE01";
    launcher.revision = 1;
    Orca::SetActiveProfile(launcher);
    expect("", Ruleset::None, false, true);
  }

  // Mod mode: the builtin mod.json declares, on while its guard words hold.
  std::string error;
  Bundle b = MakeBundle(BRAWL_MOD);
  b.WriteTree();
  auto brawl = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(brawl) << error;
  {
    SCOPED_TRACE("Brawl mod");
    Orca::SetActiveProfile(brawl);
    expect("brawl-rev2", Ruleset::Brawl, false, true);
    Orca::Mod::NoteGuards(true, 2);
    expect("brawl-rev2", Ruleset::Brawl, true, true);
    Orca::Mod::NoteGuards(false, 500);
    expect("brawl-rev2", Ruleset::Brawl, false, true);
  }
  Bundle pp = MakeBundle(PPLUS_MOD);
  pp.Add("launcher.dol", std::string(64, '\x01'));
  pp.WriteTree();
  auto pplus = Orca::LoadModProfile(pp.dir, &error);
  ASSERT_TRUE(pplus) << error;
  {
    SCOPED_TRACE("Project+ mod");
    Orca::SetActiveProfile(pplus);
    expect("brawl-rev2-pplus", Ruleset::PPlus, false, true);
    Orca::Mod::NoteGuards(true, 2);
    expect("brawl-rev2-pplus", Ruleset::PPlus, true, true);
  }
  picojson::value plain;
  picojson::parse(plain, BRAWL_MOD);
  plain.get<picojson::object>().erase("builtin");
  plain.get<picojson::object>().erase("offers");
  Bundle n = MakeBundle(plain.serialize());
  n.WriteTree();
  auto none = Orca::LoadModProfile(n.dir, &error);
  ASSERT_TRUE(none) << error;
  {
    SCOPED_TRACE("a mod with no builtin on Brawl's disc");
    Orca::SetActiveProfile(none);
    Orca::Mod::NoteGuards(true, 2);
    expect("", Ruleset::None, false, true);
  }
  Orca::SetActiveProfile(std::nullopt);
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(pp.dir);
  File::DeleteDirRecursively(n.dir);
}

// S3: the harness reads scene names where the module says it can (Rollback/Harness.cpp's
// scene-relative inputs): Dolphin running Brawl rev 2, a mod with a builtin, or a launcher profile
// booting Brawl rev 2, whatever its build or mode; else not at all.
TEST(OrcaMod, SceneNamesAreTheModules)
{
  struct ProfileDone
  {
    ~ProfileDone() { Orca::SetActiveProfile(std::nullopt); }
  } profile_done;
  Orca::UX::BrawlModule& module = Orca::UX::Builtin();
  // `readable`: with no game running in Dolphin (the harness passes SConfig's ID and revision).
  const auto readable = [&module] { return module.ScenesReadable("", 0); };
  Orca::SetActiveProfile(std::nullopt);
  EXPECT_FALSE(readable());
  // Dolphin running Brawl rev 2 is enough, with no profile at all; another revision is not.
  EXPECT_TRUE(module.ScenesReadable("RSBE01", 2));
  EXPECT_FALSE(module.ScenesReadable("RSBE01", 1));
  EXPECT_FALSE(module.ScenesReadable("RMCE01", 2));
  Orca::Profile disc;
  disc.game_id = "RSBE01";
  disc.revision = 2;
  // A disc profile reads them through Dolphin's running game only.
  Orca::SetActiveProfile(disc);
  EXPECT_FALSE(readable());
  Orca::Profile launcher;
  launcher.game_id = "LXP";
  launcher.revision = 2;
  launcher.launcher_hash = std::string(32, '0');
  launcher.disc = "RSBE01";
  Orca::SetActiveProfile(launcher);
  EXPECT_TRUE(readable());
  launcher.revision = 1;
  Orca::SetActiveProfile(launcher);
  EXPECT_FALSE(readable());

  std::string error;
  Bundle b = MakeBundle(BRAWL_MOD);
  b.WriteTree();
  auto brawl = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(brawl) << error;
  Orca::SetActiveProfile(brawl);
  EXPECT_TRUE(readable());  // declared, guards or not
  picojson::value plain;
  picojson::parse(plain, PPLUS_MOD);
  plain.get<picojson::object>().erase("builtin");
  plain.get<picojson::object>().erase("offers");
  plain.get<picojson::object>().erase("rulesets");
  Bundle n = MakeBundle(plain.serialize());
  n.Add("launcher.dol", std::string(64, '\x01'));
  n.WriteTree();
  auto none = Orca::LoadModProfile(n.dir, &error);
  ASSERT_TRUE(none) << error;
  // No builtin, but a launcher booting Brawl rev 2.
  Orca::SetActiveProfile(none);
  EXPECT_TRUE(readable());
  Orca::SetActiveProfile(std::nullopt);
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(n.dir);
}

// Whether the modules run is a pure function of memory, first runs and re-runs: a joiner whose own
// frames already held replays frame 1 from the starting snapshot (Project+'s codes not landed yet)
// with the modules off, exactly as the host ran it, and a rollback re-run after a failure runs the
// modules wherever the first run did. Only the caps keep history.
TEST(OrcaMod, GuardsFollowMemoryOnReRuns)
{
  Bundle b = MakeBundle(PPLUS_MOD);
  b.Add("launcher.dol", std::string(64, '\x01'));
  b.WriteTree();
  std::string error;
  auto profile = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(profile) << error;
  Orca::SetActiveProfile(profile);
  const Builtin pplus = Builtin::BrawlRev2PPlus;
  // The joiner's own boot: frame 1 before the codes land, then held.
  Orca::Mod::NoteGuards(false, 1);
  EXPECT_FALSE(Orca::Mod::BuiltinOn(pplus));
  Orca::Mod::NoteGuards(true, 2);
  EXPECT_TRUE(Orca::Mod::BuiltinOn(pplus));
  EXPECT_TRUE(Orca::Mod::Offers("results"));
  Orca::Mod::NoteGuards(true, 450);
  // It joins: the starting snapshot's frame 1 again, then frame 2.
  Orca::Mod::NoteGuards(false, 1);
  EXPECT_FALSE(Orca::Mod::BuiltinOn(pplus));
  EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::UX::Rules::Ruleset::None);
  EXPECT_FALSE(Orca::Mod::GuardsFailed());
  EXPECT_TRUE(Orca::Mod::Offers("results"));
  Orca::Mod::NoteGuards(true, 2);
  EXPECT_TRUE(Orca::Mod::BuiltinOn(pplus));
  EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::UX::Rules::Ruleset::PPlus);
  // The input gate's calls carry no frame: off while not holding, never a failure.
  Orca::Mod::NoteGuards(false, std::nullopt);
  EXPECT_FALSE(Orca::Mod::BuiltinOn(pplus));
  EXPECT_FALSE(Orca::Mod::GuardsFailed());
  Orca::Mod::NoteGuards(true, std::nullopt);
  EXPECT_TRUE(Orca::Mod::BuiltinOn(pplus));
  // A real failure (at or after firstKeyframeFrame), then a rollback re-runs a frame that held.
  Orca::Mod::NoteGuards(false, 600);
  EXPECT_TRUE(Orca::Mod::GuardsFailed());
  EXPECT_FALSE(Orca::Mod::BuiltinOn(pplus));
  EXPECT_FALSE(Orca::Mod::Offers("results"));
  Orca::Mod::NoteGuards(true, 598);
  EXPECT_TRUE(Orca::Mod::BuiltinOn(pplus));
  EXPECT_FALSE(Orca::Mod::Offers("results"));
  Orca::SetActiveProfile(std::nullopt);
  File::DeleteDirRecursively(b.dir);
}

// With ORCA_TEST_DISC_REV1 (Brawl (USA) (Rev 1)): a mod's launcher boots it as Rev 2, as a legacy
// launcher profile does (AliasRevision); with ORCA_TEST_DISC (Rev 2) too, that one as it is.
TEST(OrcaMod, LauncherBootReadsRev1AsRev2)
{
  const char* rev1 = std::getenv("ORCA_TEST_DISC_REV1");
  if (!rev1)
    GTEST_SKIP() << "set ORCA_TEST_DISC_REV1 to a Brawl (USA) (Rev 1) image";
  picojson::value json;
  picojson::parse(json, PPLUS_MOD);
  json.get<picojson::object>().erase("sd");
  Bundle b = MakeBundle(json.serialize());
  b.Add("launcher.dol", "dol");
  b.WriteTree();
  std::string error;
  auto profile = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(profile) << error;
  const std::string launcher = b.dir + "launcher.dol";
  bool wrong_disc = false;
  EXPECT_TRUE(Orca::PrepareModBoot(&*profile, &launcher, rev1, "", 0, &error, &wrong_disc)) << error;
  if (const char* rev2 = std::getenv("ORCA_TEST_DISC"))
  {
    EXPECT_TRUE(Orca::PrepareModBoot(&*profile, &launcher, rev2, "", 0, &error, &wrong_disc))
        << error;
  }
  // A mod that needs Rev 1 itself still sees Rev 2 for that dump: one alias, the same as legacy's.
  // (A builtin runs on revision 2 only, so this one has none.)
  json.get<picojson::object>()["disc"].get<picojson::object>()["revision"] = picojson::value(1.0);
  for (const char* key : {"builtin", "offers", "rulesets"})
    json.get<picojson::object>().erase(key);
  Bundle one = MakeBundle(json.serialize());
  one.Add("launcher.dol", "dol");
  one.WriteTree();
  auto profile1 = Orca::LoadModProfile(one.dir, &error);
  ASSERT_TRUE(profile1) << error;
  const std::string launcher1 = one.dir + "launcher.dol";
  EXPECT_FALSE(Orca::PrepareModBoot(&*profile1, &launcher1, rev1, "", 0, &error, &wrong_disc));
  EXPECT_TRUE(wrong_disc);
  EXPECT_NE(error.find("revision 2"), std::string::npos) << error;
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(one.dir);
}

// A mod's launcher boot says why its disc didn't open, as a legacy launcher profile does
// (Profile.cpp DiscOpenProblem): no path, or a file that isn't a disc image. Never the path.
TEST(OrcaMod, LauncherBootSaysWhyTheDiscDidNotOpen)
{
  picojson::value json;
  picojson::parse(json, PPLUS_MOD);
  json.get<picojson::object>().erase("sd");
  Bundle b = MakeBundle(json.serialize());
  b.Add("launcher.dol", "dol");
  b.WriteTree();
  const std::string elsewhere = File::CreateTempDir() + "/";
  const std::string not_disc = elsewhere + "not-a-disc.iso";
  ASSERT_TRUE(File::WriteStringToFile(not_disc, "not a disc image"));
  std::string error;
  auto profile = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(profile) << error;
  const std::string launcher = b.dir + "launcher.dol";
  bool wrong_disc = false;
  EXPECT_FALSE(Orca::PrepareModBoot(&*profile, &launcher, "", "", 0, &error, &wrong_disc));
  EXPECT_FALSE(wrong_disc);
  EXPECT_NE(error.find("from the drive, and no disc is set"), std::string::npos) << error;
  EXPECT_FALSE(Orca::PrepareModBoot(&*profile, &launcher, not_disc, "", 0, &error, &wrong_disc));
  EXPECT_FALSE(wrong_disc);
  EXPECT_NE(error.find("from the drive, and its disc file (.iso, 16 bytes) isn't a disc image"),
            std::string::npos)
      << error;
  EXPECT_EQ(error.find(elsewhere), std::string::npos) << error;
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(elsewhere);
}

// S3 (WP19 a): the key's UX version is the engine's, then the running game's own revision when it
// has one. Brawl rev 2 and Project+ revision 1 distinguish the multi-friend token return.
TEST(OrcaMod, UxKeyPartIsPerGame)
{
  EXPECT_EQ(Orca::Online::UxKeyPart(29, 0), "|ux=29");
  EXPECT_EQ(Orca::Online::UxKeyPart(29, 3), "|ux=29.3");
  // A launcher profile's key reads the running game's ID (SConfig), as in a session.
  SConfig::Init();
  struct ConfigDone
  {
    ~ConfigDone() { SConfig::Shutdown(); }
  } config_done;
  const std::string ux = fmt::format("|ux={}.2", Orca::UX::kCompatVersion);
  const int before = Orca::Events::UXCompatVersion();
  // The module, the UX version and the profile as they were, whatever fails.
  struct SessionDone
  {
    int ux;
    ~SessionDone()
    {
      Orca::Engine::SetModule(nullptr);
      Orca::Events::SetUXCompatVersion(ux);
      Orca::SetActiveProfile(std::nullopt);
    }
  } session_done{before};
  Orca::Events::SetUXCompatVersion(Orca::UX::kCompatVersion);
  Orca::Engine::SetModule(&Orca::UX::Builtin());
  std::string error;
  for (const char* name : {"RSBE01", "PPLUS32"})
  {
    SCOPED_TRACE(name);
    const auto profile = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), name, 2, &error);
    ASSERT_TRUE(profile) << error;
    Orca::SetActiveProfile(profile);
    EXPECT_EQ(Orca::UX::Builtin().Describe().compat, int{2});
    EXPECT_TRUE(Orca::Online::CompatibilityKeyParts().ends_with(ux));
  }
  // Mod mode's key ends the same way.
  Bundle b = MakeBundle(BRAWL_MOD);
  b.WriteTree();
  Bundle pp = MakeBundle(PPLUS_MOD);
  pp.Add("launcher.dol", std::string(64, '\x01'));
  pp.WriteTree();
  for (const Bundle* bundle : {&b, &pp})
  {
    const auto mod = Orca::LoadModProfile(bundle->dir, &error);
    ASSERT_TRUE(mod) << error;
    Orca::SetActiveProfile(mod);
    EXPECT_EQ(Orca::UX::Builtin().Describe().compat, int{2});
    const std::string parts = Orca::Online::CompatibilityKeyParts();
    EXPECT_TRUE(parts.starts_with("orca2|")) << parts;
    EXPECT_TRUE(parts.ends_with(ux)) << parts;
  }
  File::DeleteDirRecursively(b.dir);
  File::DeleteDirRecursively(pp.dir);
  Orca::Engine::SetModule(nullptr);
  Orca::Events::SetUXCompatVersion(before);
  Orca::SetActiveProfile(std::nullopt);
}

// S3: a module's guest writes are recorded against the regions it declares, never refused
// (Engine/Regions.h); the builtin's are sorted, disjoint, and hold its match block.
TEST(OrcaMod, ModuleWritesAreRecordedNotRefused)
{
  using Orca::Engine::Region;
  using Orca::Engine::RegionKind;
  const std::span<const Region> builtin = Orca::UX::Builtin().Regions();
  ASSERT_FALSE(builtin.empty());
  for (size_t i = 0; i < builtin.size(); ++i)
  {
    EXPECT_LT(builtin[i].begin, builtin[i].end) << builtin[i].name;
    if (i > 0)
      EXPECT_LE(builtin[i - 1].end, builtin[i].begin) << builtin[i].name;
  }
  static constexpr std::array<Region, 1> regions{{{0x80001000, 0x80001010, RegionKind::Data, "a"}}};
  Orca::Engine::SetWriteRegions("test", regions);
  Orca::Engine::NoteWrite(0x80001000, 4);
  Orca::Engine::NoteWrite(0x8000100C, 4);
  EXPECT_EQ(Orca::Engine::Writes().writes, 2u);
  EXPECT_EQ(Orca::Engine::Writes().outside, 0u);
  Orca::Engine::NoteWrite(0x8000100E, 4);  // runs past the end
  Orca::Engine::NoteWrite(0x80000FFF, 1);
  Orca::Engine::NoteWrite(0x80002000, 2);
  EXPECT_EQ(Orca::Engine::Writes().writes, 5u);
  EXPECT_EQ(Orca::Engine::Writes().outside, 3u);
  // No regions: nothing recorded.
  Orca::Engine::SetWriteRegions({}, {});
  Orca::Engine::NoteWrite(0x80002000, 2);
  EXPECT_EQ(Orca::Engine::Writes().writes, 0u);
  // The builtin, through SetModule: its match block is in, main.dol's code is not.
  Orca::Engine::SetModule(&Orca::UX::Builtin());
  Orca::Engine::NoteWrite(0x806F2380, 4);
  Orca::Engine::NoteWrite(0x80004000, 4);
  EXPECT_EQ(Orca::Engine::Writes().writes, 2u);
  EXPECT_EQ(Orca::Engine::Writes().outside, 1u);
  Orca::Engine::SetModule(nullptr);
  EXPECT_EQ(Orca::Engine::Writes().writes, 0u);
}

// The legacy key's inputs, pinned: with no ORCA_MOD_DIR the key is "orca2:" (the room protocol's
// prefix, in both modes) over exactly what release-0.3.33 hashed (the profile and patches files, every game INI, the forced settings, the
// universal settings, the disc and the UX version, in that order).
TEST(OrcaMod, LegacyKeyIsUnchanged)
{
  std::string error;
  // The key reads the files where a session finds them (Sys); the profile comes from the source tree.
  const std::string sys = File::GetSysDirectory();
  const auto legacy = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "RSBE01", 2, &error);
  ASSERT_TRUE(legacy) << error;
  Orca::SetActiveProfile(legacy);
  const std::string parts = Orca::Online::CompatibilityKeyParts();
  const auto file_hash = [](const std::string& path) {
    std::string contents;
    return File::ReadFileToString(path, contents) ?
               fmt::format("{:016x}|", XXH3_64bits(contents.data(), contents.size())) :
               std::string("-|");
  };
  const std::string head = fmt::format("orca|{}|RSBE01|2|{}{}", Common::GetScmRevGitStr(),
                                       file_hash(sys + "Orca/RSBE01.ini"),
                                       file_hash(sys + "Orca/RSBE01.patches"));
  EXPECT_TRUE(parts.starts_with(head)) << parts;
  EXPECT_NE(parts.find("|thread="), std::string::npos);
  EXPECT_NE(parts.find("|sdcard="), std::string::npos);
  EXPECT_NE(parts.find("|disc=" + Orca::GetEnv("ORCA_DISC_SHA1") + "|ux="), std::string::npos);
  EXPECT_EQ(parts.find("orca2"), std::string::npos);
  EXPECT_EQ(Orca::Online::CompatibilityKey(),
            fmt::format("orca2:{:016x}", XXH3_64bits(parts.data(), parts.size())));

  // Mod mode: "orca2:" over "orca2|", the engine, the tree, the card and the disc (never the same
  // hash input as a legacy key, which starts "orca|").
  // A launcher profile's key reads the running game's ID (SConfig), as in a session.
  SConfig::Init();
  struct ConfigDone
  {
    ~ConfigDone() { SConfig::Shutdown(); }
  } config_done;
  Bundle b = MakeBundle(PPLUS_MOD);
  b.Add("launcher.dol", "dol");
  b.WriteTree();
  auto mod = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(mod) << error;
  mod->mod->sd_image_hash = std::string(32, 'c');
  Orca::SetActiveProfile(mod);
  const std::string mod_parts = Orca::Online::CompatibilityKeyParts();
  const std::string disc = Orca::GetEnv("ORCA_DISC_SHA1");
  EXPECT_TRUE(mod_parts.starts_with(fmt::format(
      "orca2|{}|api={}|tree={}|sd={}|disc={}|sys=", Common::GetScmRevGitStr(),
      Orca::Mod::ENGINE_API, Orca::Mod::TreeHash(b.files), std::string(32, 'c'),
      disc.empty() ? "-" : disc)))
      << mod_parts;
  // The Sys game INIs' hashes, then the forced settings.
  EXPECT_NE(mod_parts.find("|0.Core.WiiSDCard=True|0.Core.CustomRTCValue=1735689600|thread="),
            std::string::npos)
      << mod_parts;
  EXPECT_EQ(Orca::Online::CompatibilityKey(),
            fmt::format("orca2:{:016x}", XXH3_64bits(mod_parts.data(), mod_parts.size())));
  Orca::SetActiveProfile(std::nullopt);
  File::DeleteDirRecursively(b.dir);
}

// "ready" and `orca caps` wait until a builtin's guards are decided, and a cap withdrawn after the
// first line is off at once (a second `orca caps` line, which the app takes as the new offer).
TEST(OrcaMod, CapsWaitForTheGuards)
{
  const std::string session = Orca::GetEnv("ORCA_SESSION");
  SetEnv("ORCA_SESSION", "1");
  Bundle b = MakeBundle(BRAWL_MOD);
  b.WriteTree();
  std::string error;
  auto profile = Orca::LoadModProfile(b.dir, &error);
  ASSERT_TRUE(profile) << error;
  Orca::SetActiveProfile(profile);
  EXPECT_FALSE(Orca::Status::CapsSettled());
  // An app that answers early (a test tool) gets the builtin's caps once they are offered.
  Orca::Status::SetAppCaps("join results locks queue2");
  EXPECT_FALSE(Orca::Status::Cap("results"));
  Orca::Mod::NoteGuards(true, 2);
  Orca::Status::UpdateCaps();
  EXPECT_TRUE(Orca::Status::Cap("results"));
  EXPECT_TRUE(Orca::Status::CapsSettled());
  Orca::Status::PrintCaps();
  Orca::Status::SetAppCaps("join results locks queue2");
  EXPECT_TRUE(Orca::Status::Cap("results"));
  EXPECT_TRUE(Orca::Status::Cap("join"));
  Orca::Mod::NoteGuards(false, 400);
  Orca::Status::UpdateCaps();
  EXPECT_FALSE(Orca::Status::Cap("results"));
  EXPECT_FALSE(Orca::Status::Cap("locks"));
  EXPECT_TRUE(Orca::Status::Cap("join"));
  Orca::Status::SetAppCaps("");
  Orca::SetActiveProfile(std::nullopt);
  EXPECT_TRUE(Orca::Status::CapsSettled());  // legacy: never waits
  SetEnv("ORCA_SESSION", session);
  File::DeleteDirRecursively(b.dir);
}

namespace
{
std::string WithOrigin(const std::string& origin, int format = 1,
                       const picojson::object& extra = {})
{
  picojson::value root;
  picojson::parse(root, BRAWL_MOD);
  auto& object = root.get<picojson::object>();
  object.erase("builtin");
  object.erase("offers");
  object.erase("bootNandFrame");
  object["format"] = picojson::value(static_cast<double>(format));
  if (format == 2)
  {
    object["listing"] = picojson::value("generic-game");
    object["serial"] = picojson::value(1.0);
  }
  picojson::value decl;
  EXPECT_EQ(picojson::parse(decl, origin), "");
  object["origin"] = decl;
  for (const auto& [key, value] : extra)
    object[key] = value;
  return root.serialize();
}
}

TEST(OrcaMod, OriginAcceptedBoundsInBothFormats)
{
  for (const int format : {1, 2})
  {
    for (const u32 frame : {1u, 36000u})
    {
      std::string error;
      const auto m = Orca::Mod::ParseManifest(
          WithOrigin(fmt::format(R"({{"frame":{}}})", frame), format), &error);
      ASSERT_TRUE(m) << error;
      ASSERT_TRUE(m->origin);
      EXPECT_EQ(m->origin->frame, frame);
      EXPECT_EQ(m->origin->settle, 30u);
      EXPECT_TRUE(m->origin->words.empty());
    }
    for (const std::string& address : {"0x80000000", "0x817FFFFC", "0x90000000", "0x93FFFFFC"})
    {
      for (const int settle : {1, 600})
      {
        std::string error;
        const auto m = Orca::Mod::ParseManifest(WithOrigin(fmt::format(
            R"({{"frame":1,"words":[{{"address":"{}","word":"0xFFFFFFFF"}}],"settle":{}}})",
            address, settle), format), &error);
        ASSERT_TRUE(m) << error;
        EXPECT_EQ(m->origin->settle, static_cast<u32>(settle));
        ASSERT_EQ(m->origin->words.size(), 1u);
        EXPECT_EQ(m->origin->words[0].word, 0xFFFFFFFFu);
      }
    }
    EXPECT_FALSE(Refused(WithOrigin(R"({"frame":1,"words":[],"settle":1})", format)));
    picojson::array words;
    for (int i = 0; i < 8; ++i)
      words.emplace_back(picojson::object{{"address", picojson::value(fmt::format("0x{:08x}", 0x80000000u + i * 4))},
                                          {"word", picojson::value("0x00000000")}});
    const picojson::object decl{{"frame", picojson::value(1.0)}, {"words", picojson::value(words)}};
    EXPECT_FALSE(Refused(WithOrigin(picojson::value(decl).serialize(), format)));
    words.push_back(words.back());
    EXPECT_TRUE(Refused(WithOrigin(picojson::value(picojson::object{
        {"frame", picojson::value(1.0)}, {"words", picojson::value(words)}}).serialize(), format)));
  }
}

TEST(OrcaMod, OriginRefusesMalformedFields)
{
  for (const int format : {1, 2})
  {
    for (const char* decl : {"null", "[]", "1", "{}", R"({"frame":0})", R"({"frame":36001})",
                             R"({"frame":-1})", R"({"frame":1.5})", R"({"frame":"1"})",
                             R"({"frame":true})", R"({"frame":1,"ram":"x"})",
                             R"({"frame":1,"words":null})", R"({"frame":1,"words":{}})",
                             R"({"frame":1,"words":[[]]})", R"({"frame":1,"words":[{}]})",
                             R"({"frame":1,"settle":1})", R"({"frame":1,"words":[],"settle":0})",
                             R"({"frame":1,"words":[],"settle":601})",
                             R"({"frame":1,"words":[],"settle":1.5})",
                             R"({"frame":1,"words":[],"settle":"1"})",
                             R"({"frame":1,"words":[{"address":"0x80000000","word":0}]})",
                             R"({"frame":1,"words":[{"address":"0x80000000","word":"0x100000000"}]})",
                             R"({"frame":1,"words":[{"address":"0x80000000","word":"0xzz"}]})",
                             R"({"frame":1,"words":[{"address":"0x80000000","word":"0x0","x":1}]})",
                             R"({"frame":1,"words":[{"address":"0x80000000","word":"0x0"},{"address":"0x80000000","word":"0x1"}]})"})
      EXPECT_TRUE(Refused(WithOrigin(decl, format))) << decl;
    for (const char* address : {"0x7FFFFFFC", "0x81800000", "0x80000002", "0x8FFFFFFC",
                               "0x94000000", "0x90000001", "80000000", "0x180000000"})
      EXPECT_TRUE(Refused(WithOrigin(fmt::format(
          R"({{"frame":1,"words":[{{"address":"{}","word":"0x0"}}]}})", address), format))) << address;
  }
}

TEST(OrcaMod, OriginFollowsBootFramesAndBuiltinVerdict)
{
  for (const int format : {1, 2})
  {
    for (const char* key : {"jitClearFrame", "bootNandFrame"})
    {
      for (const int boot : {9, 10, 11})
      {
        EXPECT_EQ(Refused(WithOrigin(R"({"frame":10})", format,
            {{key, picojson::value(static_cast<double>(boot))}})), boot >= 10);
      }
    }
    const picojson::object builtin{{"builtin", picojson::value("brawl-rev2")},
                                   {"firstKeyframeFrame", picojson::value(600.0)}};
    EXPECT_TRUE(Refused(WithOrigin(R"({"frame":599})", format, builtin)));
    EXPECT_FALSE(Refused(WithOrigin(R"({"frame":600})", format, builtin)));
    EXPECT_FALSE(Refused(WithOrigin(R"({"frame":601})", format, builtin)));
  }
}

TEST(OrcaMod, OriginChangesOnlyThePacksTreeHash)
{
  const std::string before = WithOrigin(R"({"frame":1801})");
  const std::string after = WithOrigin(R"({"frame":1802})");
  const auto hash = [](const std::string& json) {
    const std::vector<Orca::Mod::TreeFile> files{
        {"mod.json", Orca::Mod::Sha256Hex(json), json.size()}};
    return Orca::Mod::TreeHash(files);
  };
  EXPECT_NE(hash(before), hash(after));
}

TEST(OrcaMod, NoOriginWithdrawsOnlineForOnlyThisBoot)
{
  Orca::Profile profile;
  profile.game_id = "GALE01";
  profile.mod.emplace();
  Orca::SetActiveProfile(profile);
  SetEnv("ORCA_SESSION", "1");
  Orca::Status::SetAppCaps("join host direct leave pause");
  EXPECT_TRUE(Orca::Status::Cap("join"));
  Orca::Status::WithdrawOnline();
  EXPECT_FALSE(Orca::Status::Cap("join"));
  EXPECT_FALSE(Orca::Status::Cap("host"));
  EXPECT_FALSE(Orca::Status::Cap("direct"));
  EXPECT_TRUE(Orca::Status::Cap("leave"));
  Orca::Status::SetAppCaps("join host direct leave");
  EXPECT_FALSE(Orca::Status::Cap("join"));
  Orca::SetActiveProfile(profile);
  Orca::Status::SetAppCaps("join host direct leave");
  EXPECT_TRUE(Orca::Status::Cap("join"));
  EXPECT_TRUE(Orca::Status::Cap("host"));
  EXPECT_TRUE(Orca::Status::Cap("direct"));
  Orca::Status::SetAppCaps("");
  SetEnv("ORCA_SESSION", "");
  Orca::SetActiveProfile(std::nullopt);
}

// Pin the complete legacy serialization, including absent INIs, per-profile settings and the
// engine/game UX suffix. Only the build revision is normalized: commits intentionally change it.
TEST(OrcaMod, GoldenLegacyCompatibilityKey)
{
  const std::string dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const std::string old_disc = Orca::GetEnv("ORCA_DISC_SHA1");
  const std::string old_session = Orca::GetEnv("ORCA_SESSION");
  const int old_ux = Orca::Events::UXCompatVersion();
  const u64 old_save = Orca::GetSeedSaveHash();
  const auto done = [&] {
    Orca::SetActiveProfile(std::nullopt);
    Orca::Engine::SetModule(nullptr);
    Orca::Events::SetUXCompatVersion(old_ux);
    Orca::SetSeedSaveHash(old_save);
    Config::Shutdown();
    SetEnv("ORCA_DISC_SHA1", old_disc);
    SetEnv("ORCA_SESSION", old_session);
    File::DeleteDirRecursively(dir);
  };
  struct Cleanup { std::function<void()> run; ~Cleanup() { run(); } } cleanup{done};
  File::SetUserPath(D_USER_IDX, dir + "/user/");
  ASSERT_TRUE(File::CreateFullPath(dir + "/user/GameSettings/RSBE01.ini"));
  ASSERT_TRUE(File::WriteStringToFile(dir + "/user/GameSettings/RSBE01.ini",
                                    "fixed local game settings\n"));
  SetEnv("ORCA_SESSION", "0");
  SetEnv("ORCA_DISC_SHA1", "0123456789abcdef0123456789abcdef01234567");
  Config::Init();
  std::string error;
  const auto loaded = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "RSBE01", 2, &error);
  ASSERT_TRUE(loaded) << error;
  const auto& profile = *loaded;
  Config::AddLayer(Orca::GenerateConfigLoader(profile));
  // FMA support is a host input, so pin it for this portable fixture.
  Config::Set(Config::LayerType::Netplay, Config::SESSION_USE_FMA, false);
  Orca::SetActiveProfile(profile);
  Orca::Engine::SetModule(&Orca::UX::Builtin());
  Orca::Events::SetUXCompatVersion(Orca::UX::kCompatVersion);
  Orca::SetSeedSaveHash(0x1122334455667788);
  std::string parts = Orca::Online::CompatibilityKeyParts();
  EXPECT_EQ(Orca::Online::CompatibilityKey(),
            fmt::format("orca2:{:016x}", XXH3_64bits(parts.data(), parts.size())));
  const std::string revision = Common::GetScmRevGitStr();
  ASSERT_TRUE(parts.starts_with("orca|" + revision + "|"));
  parts.replace(5, revision.size(), "fixture-engine-revision");
  // UX 32 plus Brawl revision 2 (room team colours). Normalize only that suffix
  // to keep both earlier serialization goldens pinned.
  ASSERT_TRUE(parts.ends_with("|ux=32.2"));
  std::string previous = parts;
  previous.replace(previous.size() - 8, 8, "|ux=30");
  EXPECT_EQ(fmt::format("orca2:{:016x}", XXH3_64bits(previous.data(), previous.size())),
            "orca2:ad4097b8dd752ca6");
  previous.replace(previous.size() - 6, 6, "|ux=29");
  EXPECT_EQ(fmt::format("orca2:{:016x}", XXH3_64bits(previous.data(), previous.size())),
            "orca2:395b59606b61d7e7");
}

namespace
{
std::string ResultsMod()
{
  picojson::value value;
  picojson::parse(value, BRAWL_MOD);
  auto& object = value.get<picojson::object>();
  object["builtin"] = picojson::value("brawl-rev2-results");
  object["offers"] = picojson::value(picojson::array{picojson::value("results")});
  picojson::value rules;
  picojson::parse(rules, R"({"stocks":3,"timeMinutes":8,"items":"off"})");
  object["rankedRules"] = rules;
  return value.serialize();
}

// ResultsMod without rankedRules (and so offering nothing): a friends-only pack.
std::string UnruledResultsMod()
{
  picojson::value value;
  picojson::parse(value, ResultsMod());
  auto& object = value.get<picojson::object>();
  object.erase("rankedRules");
  object["offers"] = picojson::value(picojson::array{});
  return value.serialize();
}

std::string ResultsWith(std::string_view key, const std::string& json)
{
  picojson::value value, field;
  picojson::parse(value, ResultsMod());
  EXPECT_EQ(picojson::parse(field, json), "");
  value.get<picojson::object>()[std::string(key)] = field;
  return value.serialize();
}
}  // namespace

TEST(OrcaMod, ResultsOnlyBuiltinIsValidated)
{
  EXPECT_EQ(Orca::Mod::BuiltinFromName("brawl-rev2-results"), Builtin::BrawlRev2Results);
  EXPECT_EQ(Orca::Mod::BuiltinName(Builtin::BrawlRev2Results), "brawl-rev2-results");
  std::string error;
  const auto plain = Orca::Mod::ParseManifest(ResultsMod(), &error);
  ASSERT_TRUE(plain) << error;
  EXPECT_EQ(plain->builtin, Builtin::BrawlRev2Results);
  EXPECT_EQ(plain->offers, std::vector<std::string>{"results"});
  // results only with declared ranked rules; without them the pack offers nothing.
  const auto unruled = Orca::Mod::ParseManifest(UnruledResultsMod(), &error);
  ASSERT_TRUE(unruled) << error;
  EXPECT_FALSE(unruled->ranked_rules);
  EXPECT_TRUE(unruled->offers.empty());
  picojson::value value;
  picojson::parse(value, UnruledResultsMod());
  value.get<picojson::object>()["offers"] =
      picojson::value(picojson::array{picojson::value("results")});
  EXPECT_FALSE(Orca::Mod::ParseManifest(value.serialize(), &error));
  EXPECT_NE(error.find("offers \"results\" only with \"rankedRules\""), std::string::npos) << error;
  EXPECT_TRUE(Refused(ResultsWith("builtin", R"("brawl-rev2-result")")));
  for (const char* offers : {R"(["locks"])", R"(["queue2"])", R"(["pick-ranked"])",
                             R"(["pick-casual"])", R"(["results","locks"])"})
    EXPECT_TRUE(Refused(ResultsWith("offers", offers))) << offers;
  EXPECT_FALSE(Refused(ResultsWith("offers", "[]")));
  EXPECT_TRUE(Refused(ResultsWith("disc", R"({"id":"RSBE01","revision":1})")));
  EXPECT_TRUE(Refused(ResultsWith("disc", R"({"id":"RSBP01","revision":2})")));
}

TEST(OrcaMod, ResultsOnlyUsesEightCommonGuards)
{
  const auto guards = Orca::Mod::BuiltinGuards(Builtin::BrawlRev2Results);
  ASSERT_EQ(guards.size(), 8u);
  const auto vanilla = Orca::Mod::BuiltinGuards(Builtin::BrawlRev2);
  std::map<u32, u32> image;
  for (size_t i = 0; i < guards.size(); ++i)
  {
    EXPECT_EQ(guards[i].address, vanilla[i].address);
    EXPECT_EQ(guards[i].word, vanilla[i].word);
    image[guards[i].address] = guards[i].word;
  }
  const auto read = [&](u32 address) -> std::optional<u32> {
    const auto it = image.find(address);
    return it == image.end() ? std::nullopt : std::optional(it->second);
  };
  EXPECT_TRUE(Orca::Mod::GuardsHold(Builtin::BrawlRev2Results, read));
  EXPECT_FALSE(Orca::Mod::GuardsHold(Builtin::BrawlRev2, read));
  EXPECT_FALSE(Orca::Mod::GuardsHold(Builtin::BrawlRev2PPlus, read));
  for (const auto& word : guards)
  {
    image[word.address] ^= 1;
    EXPECT_FALSE(Orca::Mod::GuardsHold(Builtin::BrawlRev2Results, read));
    image[word.address] ^= 1;
  }
}

namespace
{
// An isolated in-memory Wii guest (MEM1 and MEM2, zeros), with no disc, device backend or user
// configuration, as RoomTest's FriendsHeadlessCore.
struct ResultsGuest
{
  ResultsGuest()
  {
    old_user = File::GetUserPath(D_USER_IDX);
    directory = File::CreateTempDir();
    File::SetUserPath(D_USER_IDX, directory + DIR_SEP);
    Config::Init();
    SConfig::Init();
    system.Initialize();
    old_wii = system.IsWii();
    system.SetIsWii(true);
    system.GetMemory().Init();
    // Brawl's view of memory: MEM1 at 0x80000000 and MEM2 at 0x90000000, the BATs a Wii boot sets
    // (CBoot::SetupBAT).
    auto& ppc = system.GetPPCState();
    ppc.spr[SPR_DBAT0U] = 0x80001fff;
    ppc.spr[SPR_DBAT0L] = 0x00000002;
    ppc.spr[SPR_DBAT4U] = 0x90001fff;
    ppc.spr[SPR_DBAT4L] = 0x10000002;
    HID4(ppc).SBE = 1;
    system.GetMMU().DBATUpdated();
    ppc.msr.DR = 1;
    system.GetPowerPC().MSRUpdated();
    Core::DeclareAsCPUThread();
  }
  ~ResultsGuest()
  {
    Core::UndeclareAsCPUThread();
    Orca::Status::SetAppCaps("");
    Orca::UX::AnnounceMenuCancel();
    Orca::UX::Builtin().Header().SetWanted(Orca::UX::Rules::Mode::None);
    Orca::UX::Tracker().Reset();
    Orca::SetActiveProfile(std::nullopt);
    auto& ppc = system.GetPPCState();
    for (const int spr : {SPR_DBAT0U, SPR_DBAT0L, SPR_DBAT4U, SPR_DBAT4L})
      ppc.spr[spr] = 0;
    HID4(ppc).SBE = 0;
    ppc.msr.DR = 0;
    system.GetPowerPC().MSRUpdated();
    system.GetMMU().DBATUpdated();
    system.GetMemory().Shutdown();
    system.SetIsWii(old_wii);
    SConfig::Shutdown();
    Config::Shutdown();
    File::SetUserPath(D_USER_IDX, old_user);
    File::DeleteDirRecursively(directory);
  }
  Core::System& system = Core::System::GetInstance();
  std::string old_user, directory;
  bool old_wii = false;
};

// Brawl's scene manager and g_GameGlobal as Results.h reads them, at free MEM1 addresses: the scene
// named `scene`, a 1v1's result info (port 1 won) and the rules block (3 stocks, 8:00, handicap
// off, damage 1.0), items off.
void ResultsScreen(Orca::UX::GuestMemory& m, std::string_view scene)
{
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, 0x80901000);
  m.Write32(0x80901000, 0x80902000);
  for (u32 i = 0; i <= scene.size(); ++i)
    m.Write8(0x80902000 + i, i == scene.size() ? u8{0} : static_cast<u8>(scene[i]));
  m.Write32(0x805A00E0, 0x80903000);
  m.Write32(0x80903018, 0x80904000);
  m.Write32(0x8090301C, 0x80905000);
  m.Write8(0x80904001, 1);
  m.Write16(0x8090400E, 1);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 record = 0x80904024 + port * 0x2AC;
    m.Write8(record + 0x01, port < 2 ? 0 : 3);
    m.Write8(record + 0x0A, port == 0 ? 2 : 0);
    m.Write8(record + 0x0E, static_cast<u8>(port));
  }
  m.Write8(0x80905002, 1);
  m.Write8(0x80905004, 3);
  m.Write8(0x80905005, 0);
  m.Write8(0x80905006, 10);
  m.Write8(0x80905008, 8);
  m.Write8(0x9017BE50, 0);
}
}  // namespace

TEST(OrcaMod, ResultsOnlyAdmitsRankedWithoutRulesetGates)
{
  ResultsGuest guest;
  struct Dirs
  {
    std::vector<std::string> list;
    ~Dirs()
    {
      for (const std::string& dir : list)
        File::DeleteDirRecursively(dir);
    }
  } dirs;
  Bundle bundle = MakeBundle(ResultsMod());
  dirs.list.push_back(bundle.dir);
  bundle.WriteTree();
  std::string error;
  const auto profile = Orca::LoadModProfile(bundle.dir, &error);
  ASSERT_TRUE(profile) << error;
  EXPECT_EQ(profile->game_id, "RSBE01");
  Orca::SetActiveProfile(profile);
  auto& module = Orca::UX::Builtin();
  const auto game = module.Describe();
  EXPECT_EQ(game.id, Orca::UX::BrawlGame::BRAWL_REV2_RESULTS);
  EXPECT_EQ(game.ruleset, Orca::Engine::Ruleset::None);
  EXPECT_TRUE(game.results);
  EXPECT_TRUE(game.music);
  EXPECT_EQ(game.compat, Orca::UX::BrawlGame::BRAWL_REV2_RESULTS_COMPAT);
  EXPECT_EQ(Orca::UX::BrawlGame::BRAWL_REV2_RESULTS_COMPAT, 1);
  EXPECT_FALSE(Orca::UX::BrawlGame::UsesFreeSpace(game));
  EXPECT_FALSE(module.Active());
  EXPECT_FALSE(Orca::Mod::Offers("results"));
  const std::string tail = fmt::format(" {} {} {} {}", Orca::Status::TALK_CAP,
                                       Orca::Status::RUMBLE_CAP, Orca::Status::RECORD_CAP,
                                       Orca::Status::HOSTUI_CAP);
  const std::string base = std::string(Orca::Status::CAPS) + " " + Orca::Status::MUSIC_CAP;
  EXPECT_EQ(Orca::Status::OfferedCaps(), base + tail);
  // Its eight guard words held: results and the menus, and nothing a ruleset brings.
  Orca::Mod::NoteGuards(true, 2);
  ASSERT_TRUE(module.Active());
  EXPECT_TRUE(Orca::UX::ResultsVerified());
  EXPECT_TRUE(Orca::UX::FreshMove::Supported());
  EXPECT_TRUE(Orca::Music::Supported());
  EXPECT_EQ(Orca::Status::OfferedCaps(), base + " results" + tail);
  EXPECT_TRUE(Rollback::OnlineMatch::RoomRefusal("ranked", game.results).empty());
  EXPECT_TRUE(Orca::Mod::Offers("results"));
  for (const char* cap : {"locks", "queue2", "pick-casual", "pick-ranked"})
    EXPECT_FALSE(Orca::Mod::Offers(cap)) << cap;
  EXPECT_EQ(module.ProfileRuleset(), Orca::Engine::Ruleset::None);
  EXPECT_EQ(Orca::UX::Rules::ProfileRuleset(), Orca::UX::Rules::Ruleset::None);
  // Mod::Offers keeps to the builtin's one cap even if a manifest listed more (the parser refuses
  // them first).
  auto listed = *profile;
  listed.mod->manifest.offers = {"results", "locks", "queue2"};
  Orca::SetActiveProfile(listed);
  Orca::Mod::NoteGuards(true, 2);
  EXPECT_TRUE(Orca::Mod::Offers("results"));
  EXPECT_FALSE(Orca::Mod::Offers("locks"));
  EXPECT_FALSE(Orca::Mod::Offers("queue2"));
  EXPECT_EQ(Orca::Status::OfferedCaps(), base + " results" + tail);
  Orca::SetActiveProfile(profile);
  Orca::Mod::NoteGuards(true, 2);
  Orca::Status::SetAppCaps("host results locks queue2 pick-casual pick-ranked");
  EXPECT_TRUE(Orca::Status::Cap("host"));
  EXPECT_TRUE(Orca::Status::Cap("results"));
  for (const char* cap : {"locks", "queue2", "pick-casual", "pick-ranked"})
    EXPECT_FALSE(Orca::Status::Cap(cap)) << cap;
  // The Online menu's exit codes 30 and 31 start the older search at the pick (no queue2).
  for (const u32 code : {30u, 31u})
  {
    Orca::UX::OnlineMenuReader reader;
    EXPECT_FALSE(reader.Boundary({Orca::UX::MenuState::Where::Menu, 0}, false, true, 0));
    EXPECT_FALSE(reader.Boundary({Orca::UX::MenuState::Where::Menu, 0}, false, true, 0));
    const auto pick = reader.Boundary({Orca::UX::MenuState::Where::Leaving, code}, false, true, 0);
    ASSERT_TRUE(pick);
    EXPECT_EQ(*pick, code == 30 ? Orca::UX::OnlinePick::Casual : Orca::UX::OnlinePick::Ranked);
    Orca::UX::AnnounceOnlinePick(*pick);
    EXPECT_EQ(Orca::UX::Search::Current(), Orca::UX::Search::State::Searching);
    EXPECT_FALSE(Orca::UX::QueueApp::Active());
    Orca::UX::AnnounceMenuCancel();
  }

  // sora_scene's free space holds a valid ranked Project+ header, and the rest of it bytes that are
  // nobody's: every step that reads the match block would act on it. None may write there.
  const Core::CPUThreadGuard guard(guest.system);
  Orca::UX::GuardMemory memory(guard);
  constexpr u32 free_begin = Orca::UX::FreeSpace::kMatchBlock.begin;
  constexpr u32 free_end = Orca::UX::FreeSpace::kCodeCaves.back().end;
  for (u32 address = free_begin; address < free_end; ++address)
    memory.Write8(address, 0xA5);
  memory.Write32(free_begin, Orca::UX::FreeSpace::kMatchBlockOriginalWord);
  ASSERT_GT(Orca::UX::Rules::WriteHeader(memory, Orca::UX::Rules::Mode::Ranked,
                                         Orca::UX::Rules::Ruleset::PPlus),
            0);
  ASSERT_TRUE(Orca::UX::Rules::ReadHeader(memory).Locked());
  ASSERT_TRUE(Orca::UX::SetBlock::ReadHeader(memory));
  // A set's second game (port 1 won the first), whose character order would start on the
  // character select below: its task (scene +0x400) and two human player areas (task +0x44, kind
  // at +0x1B4).
  memory.Write8(Orca::UX::SetBlock::BASE + Orca::UX::SetBlock::SET_WINS, 1);
  memory.Write8(Orca::UX::SetBlock::BASE + Orca::UX::SetBlock::SET_LAST_WINNER, 0);
  memory.Write32(0x80901400, 0x80906000);
  memory.Write32(0x80906044, 0x80907000);
  memory.Write32(0x80906048, 0x80908000);
  memory.Write32(0x80907000 + 0x1B4, 1);
  memory.Write32(0x80908000 + 0x1B4, 1);
  std::vector<u8> before;
  for (u32 address = free_begin; address < free_end; ++address)
    before.push_back(memory.Read8(address));
  ASSERT_TRUE(memory.Valid(0x9017BE50));
  (void)module.Header().TakeMismatch();
  // The session's asks too: a ranked room's header and the queue's rules preset.
  module.Header().SetWanted(Orca::UX::Rules::Mode::Ranked, 0, "room");
  Orca::UX::RequestQueueRules();
  // Two players plugged in on the built main menu, a friend's drop-in move due; pads pressing
  // everything the masks would take.
  const std::vector<Orca::Events::PortInfo> ports{{0, "Ada", false, {}, {}},
                                                  {1, "Bo", true, {}, {}}};
  std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS> raw;
  GCPadStatus pad;
  pad.button = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_START | PAD_TRIGGER_Z | PAD_BUTTON_UP;
  raw[0] = raw[1] = pad;
  ResultsScreen(memory, "muMenuMain");
  memory.Write32(0x80900000 + 0x288, Orca::UX::FriendsMove::STEP_RUNNING);
  memory.Write8(0x80901000 + Orca::UX::FriendsMove::MENU_BUILT, 1);
  Orca::UX::Tracker().Reset();
  int frame = 400;
  const auto run = [&](std::string_view scene, int frames) {
    ResultsScreen(memory, scene);
    if (scene == "scSelctCharacter")
    {
      // What a ruleset game's character order would start from here.
      const auto set = Orca::UX::CharOrder::ReadSet(memory);
      EXPECT_NE(set.ruleset, Orca::UX::CharOrder::Ruleset::None);
      EXPECT_EQ(set.game, 2);
      EXPECT_EQ(set.last_winner, 0);
      const auto css = Orca::UX::CharOrder::ReadCss(memory);
      EXPECT_TRUE(css.on_css);
      EXPECT_TRUE(css.ports[0].human && css.ports[1].human);
    }
    for (int i = 0; i < frames; ++i, ++frame)
    {
      for (const bool resimulating : {false, true})
      {
        for (const bool alone : {true, false})
        {
          module.Latch(guard, raw);
          for (const auto& mask : module.Masks(guard))
          {
            EXPECT_EQ(mask.buttons, 0);
            EXPECT_EQ(mask.press, 0);
            EXPECT_EQ(mask.a_as, 0);
            EXPECT_FALSE(mask.main_stick);
            EXPECT_FALSE(mask.c_stick);
            EXPECT_FALSE(mask.steer);
          }
          module.OnBoundary(guard, frame, resimulating, ports, alone);
          Orca::UX::ApplyQueueRules(guard, alone);
        }
      }
    }
  };
  run("muMenuMain", 40);
  run("scSelctCharacter", 2);
  const int fight_start = frame;
  run("scMelee", 2);
  run("scVsResult", 2);
  for (u32 address = free_begin; address < free_end; ++address)
    ASSERT_EQ(memory.Read8(address), before[address - free_begin]) << std::hex << address;
  EXPECT_FALSE(module.Header().TakeMismatch());
  EXPECT_TRUE(module.Header().HeaderInPlace());
  // The results come from the vanilla result info, not from the header's (empty) ranked set.
  std::vector<int> starts;
  auto results = module.Results().Confirm(frame, -1, &starts);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].kind, Orca::UX::GameResult::Kind::Win);
  EXPECT_EQ(results[0].winner_port, 0);
  EXPECT_TRUE(results[0].why.empty());
  EXPECT_EQ(results[0].start, fight_start);
  EXPECT_EQ(starts, std::vector<int>{fight_start});

  // rankedRules reaches the reader from mod.json: the same match under items Medium is not rated.
  Bundle rules_bundle =
      MakeBundle(ResultsWith("rankedRules", R"({"stocks":3,"timeMinutes":8,"items":"off"})"));
  dirs.list.push_back(rules_bundle.dir);
  rules_bundle.WriteTree();
  const auto rules_profile = Orca::LoadModProfile(rules_bundle.dir, &error);
  ASSERT_TRUE(rules_profile) << error;
  Orca::SetActiveProfile(rules_profile);
  Orca::Mod::NoteGuards(true, 2);
  ASSERT_TRUE(Orca::UX::ResultsVerified());
  for (const u8 items : {u8{0}, u8{2}})
  {
    Orca::UX::Tracker().Reset();
    for (const std::string_view scene : {"muMenuMain", "scMelee", "scVsResult"})
    {
      ResultsScreen(memory, scene);
      memory.Write8(0x9017BE50, items);
      Orca::UX::ReadResultsFrame(guard, frame++);
    }
    results = Orca::UX::Tracker().Confirm(frame, -1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].kind, items == 0 ? Orca::UX::GameResult::Kind::Win :
                                            Orca::UX::GameResult::Kind::Void);
    EXPECT_EQ(results[0].why, items == 0 ? "" : "rules-items");
  }

  // Its guards broken after holding: nothing offered, nothing on.
  Orca::Mod::NoteGuards(false, 500);
  EXPECT_FALSE(Orca::Mod::Offers("results"));
  EXPECT_FALSE(module.Active());
  EXPECT_FALSE(Orca::UX::ResultsVerified());
  // Without rankedRules: no results read, so no Ranked, even if a manifest got "results" past the
  // parser.
  auto unruled = *profile;
  unruled.mod->manifest.ranked_rules.reset();
  Orca::SetActiveProfile(unruled);
  Orca::Mod::NoteGuards(true, 2);
  ASSERT_TRUE(module.Active());
  EXPECT_FALSE(module.Describe().results);
  EXPECT_FALSE(Orca::UX::ResultsVerified());
  EXPECT_FALSE(Orca::Mod::Offers("results"));
  EXPECT_EQ(Orca::Status::OfferedCaps(), base + tail);
  EXPECT_FALSE(Rollback::OnlineMatch::RoomRefusal("ranked", module.Describe().results).empty());
  // A Brawl pack with no builtin still can't play Ranked.
  auto none = *profile;
  none.mod->manifest.builtin = Builtin::None;
  none.mod->manifest.offers.clear();
  Orca::SetActiveProfile(none);
  EXPECT_FALSE(module.Describe().results);
  EXPECT_FALSE(Rollback::OnlineMatch::RoomRefusal("ranked", module.Describe().results).empty());
}

TEST(OrcaMod, ResultsOnlyKeyEndsWithItsOwnGameCompat)
{
  SConfig::Init();
  struct Done
  {
    int ux;
    ~Done()
    {
      Orca::Engine::SetModule(nullptr);
      Orca::Events::SetUXCompatVersion(ux);
      Orca::SetActiveProfile(std::nullopt);
      SConfig::Shutdown();
    }
  } done{Orca::Events::UXCompatVersion()};
  Orca::Events::SetUXCompatVersion(Orca::UX::kCompatVersion);
  Orca::Engine::SetModule(&Orca::UX::Builtin());
  std::string error;
  Bundle results = MakeBundle(ResultsMod());
  results.WriteTree();
  const auto profile = Orca::LoadModProfile(results.dir, &error);
  ASSERT_TRUE(profile) << error;
  Orca::SetActiveProfile(profile);
  EXPECT_EQ(Orca::UX::Builtin().Describe().compat, 1);
  const std::string parts = Orca::Online::CompatibilityKeyParts();
  EXPECT_TRUE(parts.starts_with("orca2|")) << parts;
  EXPECT_TRUE(parts.ends_with(fmt::format("|ux={}.1", Orca::UX::kCompatVersion))) << parts;
  File::DeleteDirRecursively(results.dir);
}

TEST(OrcaMod, SchemaKnowsRankedRules)
{
  const picojson::value schema = ReadJson(RepoPath("Tools/orca/mods/mod.schema.json"));
  const picojson::value& ranked = schema.get("properties").get("rankedRules");
  std::vector<std::string> required;
  for (const auto& key : ranked.get("required").get<picojson::array>())
    required.push_back(key.get<std::string>());
  EXPECT_EQ(required, (std::vector<std::string>{"stocks", "timeMinutes", "items"}));
  const picojson::value& rules = ranked.get("properties");
  for (const char* key : {"stocks", "timeMinutes"})
  {
    EXPECT_EQ(rules.get(key).get("minimum").get<double>(), 1.0) << key;
    EXPECT_EQ(rules.get(key).get("maximum").get<double>(), 99.0) << key;
  }
  EXPECT_EQ(rules.get("items").get("const").get<std::string>(), "off");
  // brawl-rev2-results offers results only with it.
  bool results_need_rules = false;
  for (const auto& rule : schema.get("allOf").get<picojson::array>())
  {
    if (!rule.get("then").contains("required"))
      continue;
    const auto& then = rule.get("then").get("required").get<picojson::array>();
    const picojson::value& when = rule.get("if").get("properties");
    if (then.size() == 1 && then[0].get<std::string>() == "rankedRules" &&
        when.get("builtin").get("const").get<std::string>() == "brawl-rev2-results" &&
        when.get("offers").get("contains").get("const").get<std::string>() == "results")
    {
      results_need_rules = true;
    }
  }
  EXPECT_TRUE(results_need_rules);
}

TEST(OrcaMod, RankedRulesAreValidated)
{
  std::string error;
  const auto plain = Orca::Mod::ParseManifest(ResultsMod(), &error);
  ASSERT_TRUE(plain) << error;
  EXPECT_EQ(plain->ranked_rules, (Orca::Mod::RankedRules{3, 8}));
  const auto unruled = Orca::Mod::ParseManifest(UnruledResultsMod(), &error);
  ASSERT_TRUE(unruled) << error;
  EXPECT_FALSE(unruled->ranked_rules);
  constexpr char rules[] = R"({"stocks":3,"timeMinutes":8,"items":"off"})";
  for (const int format : {1, 2})
  {
    picojson::value value;
    picojson::parse(value, ResultsWith("rankedRules", rules));
    auto& object = value.get<picojson::object>();
    if (format == 2)
    {
      object["format"] = picojson::value(2.0);
      object["listing"] = picojson::value("results-pack");
      object["serial"] = picojson::value(1.0);
    }
    const auto manifest = Orca::Mod::ParseManifest(value.serialize(), &error);
    ASSERT_TRUE(manifest) << error;
    EXPECT_EQ(manifest->ranked_rules, (Orca::Mod::RankedRules{3, 8}));
    for (const char* builtin : {"brawl-rev2", "brawl-rev2-pplus", ""})
    {
      object["builtin"] = picojson::value(builtin);
      if (std::string_view(builtin).empty())
        object.erase("builtin");
      EXPECT_FALSE(Orca::Mod::ParseManifest(value.serialize(), &error));
      EXPECT_NE(error.find("rankedRules requires builtin brawl-rev2-results"), std::string::npos);
    }
  }
  for (const char* invalid :
       {"null", "[]", "{}", R"({"stocks":3,"timeMinutes":8})",
        R"({"stocks":0,"timeMinutes":8,"items":"off"})",
        R"({"stocks":100,"timeMinutes":8,"items":"off"})",
        R"({"stocks":3,"timeMinutes":0,"items":"off"})",
        R"({"stocks":3,"timeMinutes":100,"items":"off"})",
        R"({"stocks":3.5,"timeMinutes":8,"items":"off"})",
        R"({"stocks":3,"timeMinutes":"8","items":"off"})",
        R"({"stocks":3,"timeMinutes":8,"items":"on"})", R"({"stocks":3,"timeMinutes":8,"items":0})",
        R"({"stocks":3,"timeMinutes":8,"items":"off","extra":1})"})
    EXPECT_TRUE(Refused(ResultsWith("rankedRules", invalid))) << invalid;
  EXPECT_FALSE(
      Refused(ResultsWith("rankedRules", R"({"stocks":99,"timeMinutes":99,"items":"off"})")));
}

TEST(OrcaMod, BuiltinFixedWritersHaveExactRegions)
{
  const auto regions = Orca::UX::Builtin().Regions();
  const auto contains = [&](u32 address, u32 bytes) {
    return std::any_of(regions.begin(), regions.end(),
                       [&](const auto& region) { return region.Contains(address, bytes); });
  };
  // ClearStaleMusicSelect's complete word list, ForceCodeMenu's pause word, and
  // FriendsMove::Apply / FreshMove::Apply's two scene-manager words.
  for (const u32 address : {0x80002810u, 0x80002818u, 0x8000281Cu, 0x80002820u,
                            0x80002824u, 0x80002828u, 0x80002830u, 0x80002834u,
                            0x80002838u, 0x8000283Cu, 0x805B8A08u, 0x805B8E24u,
                            0x805B8E28u})
    EXPECT_TRUE(contains(address, 4)) << std::hex << address;
  // The adjacent words and the holes between the My Music words stay undeclared.
  for (const u32 address : {0x8000280Cu, 0x80002814u, 0x8000282Cu, 0x80002840u,
                            0x805B8A04u, 0x805B8A0Cu, 0x805B8E20u, 0x805B8E2Cu})
    EXPECT_FALSE(contains(address, 4)) << std::hex << address;
  EXPECT_FALSE(contains(0x805B8A08, 5));
  EXPECT_FALSE(contains(0x805B8E28, 5));
}
