// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <ed25519.h>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Version.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/ElfReader.h"
#include "Core/ConfigManager.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Sandbox.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"

namespace
{
using Orca::Mod::PublicKey;

// The bundle-flat TEST key (Bundle.cpp), trusted only by a build with ORCA_BUNDLE_TRUST_TEST_KEY.
[[maybe_unused]] constexpr char TEST_KEY_HEX[] =
    "afb9463d5c16f0f617432d76f75f37a8c649e36d7c626088ba00b19dea115a43";

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

// Sets an environment variable for one scope.
class ScopedEnv
{
public:
  ScopedEnv(const char* name, const std::string& value) : m_name(name), m_old(Orca::GetEnv(name))
  {
    SetEnv(name, value);
  }
  ~ScopedEnv() { SetEnv(m_name, m_old); }

private:
  const char* m_name;
  std::string m_old;
};

std::string RepoPath(const std::string& relative)
{
  return (std::filesystem::path(__FILE__).parent_path() / "../../../../" / relative).string();
}

std::string Hex(const u8* data, size_t size)
{
  std::string hex;
  for (size_t i = 0; i < size; ++i)
    hex += fmt::format("{:02x}", data[i]);
  return hex;
}

std::vector<u8> FromHex(const std::string& hex)
{
  std::vector<u8> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<u8>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

// A signing key made from a fixed seed (Externals/ed25519, as the engine verifies).
struct Signer
{
  unsigned char public_key[32]{};
  unsigned char private_key[64]{};

  explicit Signer(u8 fill)
  {
    unsigned char seed[32];
    std::fill(std::begin(seed), std::end(seed), fill);
    ed25519_create_keypair(public_key, private_key, seed);
  }
  PublicKey Key() const
  {
    PublicKey key;
    std::copy(std::begin(public_key), std::end(public_key), key.begin());
    return key;
  }
  std::string Sign(const std::string& message) const
  {
    unsigned char signature[64];
    ed25519_sign(signature, reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                 public_key, private_key);
    return fmt::format("ed25519 {} {}\n", Hex(public_key, 32), Hex(signature, 64));
  }
};

// Project+'s flat mod.json as make-bundle.py --flat writes it, for a card of `sd_bytes` with this
// chunked SHA-256.
std::string PPlusFlat(u64 sd_bytes, const std::string& sha256)
{
  return fmt::format(R"({{
  "format": 2,
  "engine": {{"api": 1, "minOrca": "0.3.35"}},
  "listing": "project-rollback",
  "serial": 1,
  "title": "Project+",
  "disc": {{"id": "RSBE01", "revision": 2}},
  "launcher": "Project+ Netplay Launcher.dol",
  "sdImage": {{"file": "sd.raw", "bytes": {}, "sha256": "{}"}},
  "patches": ["PPLUS32.patches"],
  "jitwarm": "PPLUS32.jitwarm",
  "textures": "engine",
  "frameHook": {{"address": "0x80017504", "word": "0x90170100"}},
  "bootNandFrame": 600,
  "firstKeyframeFrame": 300,
  "jitClearFrame": 2,
  "keepGameCode": [["0x8001CD24", "0x38631198", "0x806311A0"]],
  "saveTitle": "0x0001000052534245",
  "forced": {{"Dolphin.Core.CustomRTCValue": 1735689600}},
  "builtin": "brawl-rev2-pplus",
  "rulesets": ["pplus-2024"],
  "offers": ["results", "locks", "queue2"]
}})",
                     sd_bytes, sha256);
}

constexpr char BRAWL_FLAT[] = R"({
  "format": 2,
  "engine": {"api": 1, "minOrca": "0.3.35"},
  "listing": "open-brawl",
  "serial": 1,
  "disc": {"id": "RSBE01", "revision": 2},
  "patches": ["RSBE01.patches"],
  "jitwarm": "RSBE01.jitwarm",
  "frameHook": {"address": "0x80017504", "word": "0x90170100"},
  "bootNandFrame": 1800,
  "builtin": "brawl-rev2",
  "rulesets": ["brawl"],
  "offers": ["results", "locks", "queue2"]
})";

const std::string PATCHES = "# A patch file with no patches.\n";

// A flat bundle folder: its files as written, then mod.sig over them (computed here, not with the
// engine's own helpers, so the two can disagree).
struct Flat
{
  std::string dir;
  std::map<std::string, std::string> files;  // every file but mod.sig, mod.json included
  std::string sd_name;                       // left out of the signed lines

  void Put(const std::string& name, const std::string& contents)
  {
    files[name] = contents;
    ASSERT_TRUE(File::WriteStringToFile(dir + name, contents)) << name;
  }
  std::string Message() const
  {
    std::string text = "orca-flat-bundle 1\n";
    for (const auto& [name, contents] : files)  // std::map: sorted by name, byte order
    {
      if (name != sd_name)
        text += fmt::format("{}\t{}\t{}\n", name, Orca::Mod::Sha256Hex(contents), contents.size());
    }
    return text;
  }
  void Sign(const Signer& signer)
  {
    ASSERT_TRUE(File::WriteStringToFile(dir + "mod.sig", signer.Sign(Message())));
  }
  std::optional<Orca::Mod::FlatBundle> Load(const Signer& signer, std::string* error,
                                            const Orca::Mod::BootLauncher* boot = nullptr,
                                            std::string_view listing = {}) const
  {
    const PublicKey key = signer.Key();
    return Orca::Mod::LoadFlatBundle(dir, std::span(&key, 1), error, boot, listing);
  }
};

// Project+ (launcher, a small card, patches, JIT seed) in `dir`, signed.
Flat MakePPlus(const std::string& dir, const Signer& signer)
{
  Flat f{dir, {}, "sd.raw"};
  File::CreateFullPath(dir);
  f.Put("Project+ Netplay Launcher.dol", std::string(4096, '\x5a'));
  f.Put("sd.raw", std::string(65536, '\x11'));
  f.Put("PPLUS32.patches", PATCHES);
  f.Put("PPLUS32.jitwarm", "warm");
  f.Put("mod.json", PPlusFlat(65536, Orca::Mod::ChunkedSha256File(dir + "sd.raw", 65536)));
  f.Sign(signer);
  return f;
}

Flat MakeBrawl(const std::string& dir, const Signer& signer)
{
  Flat f{dir, {}, ""};
  File::CreateFullPath(dir);
  f.Put("RSBE01.patches", PATCHES);
  f.Put("RSBE01.jitwarm", "warm");
  f.Put("mod.json", BRAWL_FLAT);
  f.Sign(signer);
  return f;
}

void PutBE32(std::string* out, u32 value)
{
  for (int shift = 24; shift >= 0; shift -= 8)
    out->push_back(static_cast<char>(value >> shift));
}

void PutBE16(std::string* out, u16 value)
{
  out->push_back(static_cast<char>(value >> 8));
  out->push_back(static_cast<char>(value));
}

// A small big-endian PowerPC executable as devkitPPC links one (Project M 3.6's launcher is one):
// the ELF header, one loadable code segment at `entry` (a branch to itself), no sections. Written
// here byte by byte, not with the engine's structs. Its header fields are big-endian in the file,
// so a reader that byte-swaps them where they lie no longer holds the file.
std::string MinimalElf(u32 entry)
{
  constexpr u32 code_offset = 0x60, code_size = 0x20;
  std::string elf = "\x7f" "ELF";
  elf += '\x01';  // 32-bit
  elf += '\x02';  // big-endian
  elf += '\x01';  // version 1
  elf.resize(16, '\0');
  PutBE16(&elf, 2);   // e_type: ET_EXEC
  PutBE16(&elf, 20);  // e_machine: EM_PPC
  PutBE32(&elf, 1);   // e_version
  PutBE32(&elf, entry);
  PutBE32(&elf, 52);  // e_phoff: right after this header
  PutBE32(&elf, 0);   // e_shoff: no sections
  PutBE32(&elf, 0);   // e_flags
  PutBE16(&elf, 52);  // e_ehsize
  PutBE16(&elf, 32);  // e_phentsize
  PutBE16(&elf, 1);   // e_phnum
  PutBE16(&elf, 40);  // e_shentsize
  PutBE16(&elf, 0);   // e_shnum
  PutBE16(&elf, 0);   // e_shstrndx: SHN_UNDEF
  PutBE32(&elf, 1);   // p_type: PT_LOAD
  PutBE32(&elf, code_offset);
  PutBE32(&elf, entry);  // p_vaddr
  PutBE32(&elf, entry);  // p_paddr
  PutBE32(&elf, code_size);
  PutBE32(&elf, code_size);
  PutBE32(&elf, 5);  // p_flags: PF_R | PF_X
  PutBE32(&elf, 4);  // p_align
  elf.resize(code_offset, '\0');
  for (u32 i = 0; i < code_size / 4; ++i)
    PutBE32(&elf, 0x48000000);  // b .
  return elf;
}

// A small DOL: the 0x100-byte header and one 0x20-byte text section at `entry` (a branch to itself).
std::string MinimalDol(u32 entry)
{
  std::string dol;
  for (u32 word = 0; word < 0x40; ++word)
  {
    const u32 offset = word * 4;
    PutBE32(&dol, offset == 0x00 ? 0x100u :  // text 0's file offset
                      offset == 0x48 ? entry :   // text 0's address
                      offset == 0x90 ? 0x20u :   // text 0's size
                      offset == 0xe0 ? entry :   // the entry point
                                       0u);
  }
  for (u32 i = 0; i < 8; ++i)
    PutBE32(&dol, 0x48000000);  // b .
  return dol;
}

std::string TempDir()
{
  return File::CreateTempDir() + "/";
}

bool Refused(const std::string& json)
{
  std::string error;
  return !Orca::Mod::ParseManifest(json, &error) && !error.empty();
}

std::string With(std::string json, const std::string& key, const std::string& value)
{
  picojson::value v;
  picojson::parse(v, json);
  picojson::value field;
  EXPECT_EQ(picojson::parse(field, value), "") << value;
  v.get<picojson::object>()[key] = field;
  return v.serialize();
}

std::string Without(std::string json, const std::string& key)
{
  picojson::value v;
  picojson::parse(v, json);
  v.get<picojson::object>().erase(key);
  return v.serialize();
}

// A launcher bundle as Project+'s (a .dol launcher) and Project M 3.6's (an .elf) are laid out: the
// launcher `name` holding `contents`, a small card, patches and JIT seed, signed.
Flat MakeLauncherPack(const std::string& dir, const Signer& signer, const std::string& name,
                      const std::string& contents)
{
  Flat f{dir, {}, "sd.raw"};
  File::CreateFullPath(dir);
  f.Put(name, contents);
  f.Put("sd.raw", std::string(65536, '\x11'));
  f.Put("PPLUS32.patches", PATCHES);
  f.Put("PPLUS32.jitwarm", "warm");
  f.Put("mod.json", With(PPlusFlat(65536, Orca::Mod::ChunkedSha256File(dir + "sd.raw", 65536)),
                         "launcher", fmt::format("\"{}\"", name)));
  f.Sign(signer);
  return f;
}
}  // namespace

// Python's signer (Tools/orca/mods/sign-bundle.py, ed25519ref.py) and the engine's verifier agree:
// the same key and signature from the same seed, and the message is the documented one.
TEST(OrcaBundle, SignatureVector)
{
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(RepoPath("Tools/orca/mods/flat-sig-vector.json"), text));
  picojson::value v;
  ASSERT_EQ(picojson::parse(v, text), "");
  const std::vector<u8> seed = FromHex(v.get("seed").get<std::string>());
  ASSERT_EQ(seed.size(), 32u);
  unsigned char public_key[32], private_key[64], signature[64];
  ed25519_create_keypair(public_key, private_key, seed.data());
  EXPECT_EQ(Hex(public_key, 32), v.get("public").get<std::string>());
  const std::string message = v.get("message").get<std::string>();
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(message.data()), message.size(),
               public_key, private_key);
  EXPECT_EQ(Hex(signature, 64), v.get("signature").get<std::string>());

  const std::string sig_line = fmt::format("ed25519 {} {}\n", v.get("public").get<std::string>(),
                                           v.get("signature").get<std::string>());
  const auto parsed = Orca::Mod::ParseSignature(sig_line);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(Orca::Mod::VerifySignature(parsed->first, parsed->second, message));
  EXPECT_FALSE(Orca::Mod::VerifySignature(parsed->first, parsed->second, message + "x"));
  // The vector's message is FlatSignedMessage's for those two lines.
  const std::vector<Orca::Mod::TreeFile> lines{
      {"PPLUS32.patches", Orca::Mod::Sha256Hex("# none\n"), 7},
      {"mod.json", Orca::Mod::Sha256Hex("{}"), 2}};
  EXPECT_EQ(Orca::Mod::FlatSignedMessage(lines), message);

  // mod.sig's form: one line, lowercase hex, one optional newline.
  EXPECT_TRUE(Orca::Mod::ParseSignature(sig_line.substr(0, sig_line.size() - 1)));
  EXPECT_FALSE(Orca::Mod::ParseSignature(sig_line + "\n"));
  EXPECT_FALSE(Orca::Mod::ParseSignature("ed25519  " + sig_line.substr(8)));
  std::string upper = sig_line;
  std::ranges::transform(upper, upper.begin(), [](char c) { return std::toupper(c); });
  EXPECT_FALSE(Orca::Mod::ParseSignature(upper));
  EXPECT_FALSE(Orca::Mod::ParseSignature(""));
  EXPECT_FALSE(Orca::Mod::ParseSignature(sig_line.substr(0, 100)));
}

// The build trusts exactly the keys CMake gave it (the same definitions reach this file): the
// pinned key, the TEST key only when asked for, and none at all without either.
TEST(OrcaBundle, PinnedKey)
{
  std::vector<std::string> expected;
#ifdef ORCA_BUNDLE_PUBLIC_KEY
  expected.push_back(ORCA_BUNDLE_PUBLIC_KEY);
#endif
#ifdef ORCA_BUNDLE_TRUST_TEST_KEY
  if (std::find(expected.begin(), expected.end(), TEST_KEY_HEX) == expected.end())
    expected.push_back(TEST_KEY_HEX);
#endif
  std::vector<std::string> trusted;
  for (const PublicKey& key : Orca::Mod::TrustedKeys())
    trusted.push_back(Orca::Mod::KeyHex(key));
  EXPECT_EQ(trusted, expected);
  for (const PublicKey& key : Orca::Mod::TrustedKeys())
  {
#ifdef ORCA_BUNDLE_TRUST_TEST_KEY
    EXPECT_EQ(Orca::Mod::IsTestKey(key), Orca::Mod::KeyHex(key) == TEST_KEY_HEX);
#else
    EXPECT_FALSE(Orca::Mod::IsTestKey(key));
#endif
  }
  const Signer other(7);
  EXPECT_FALSE(Orca::Mod::IsTestKey(other.Key()));
  // Tests can trust none: nothing loads.
  const PublicKey none{};
  Orca::Mod::TrustKeysForTests(std::span(&none, 0));
  EXPECT_TRUE(Orca::Mod::TrustedKeys().empty());
  Orca::Mod::ResetTrustedKeysForTests();
  EXPECT_EQ(Orca::Mod::TrustedKeys().size(), expected.size());
}

// The SD image's pin (Bundle.h SD_HASH_HEADER): the engine's parallel hash, the definition written
// out here, and Python's (sd-hash-vector.json, from sign-bundle.py) agree, across a chunk boundary.
TEST(OrcaBundle, ChunkedSha256)
{
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(RepoPath("Tools/orca/mods/sd-hash-vector.json"), text));
  picojson::value v;
  ASSERT_EQ(picojson::parse(v, text), "");
  const u64 bytes = static_cast<u64>(v.get("bytes").get<double>());
  ASSERT_GT(bytes, Orca::Mod::SD_HASH_CHUNK_BYTES);
  // Byte i is (i * 131 + 7) mod 251.
  std::string data(bytes, '\0');
  for (u64 i = 0; i < bytes; ++i)
    data[i] = static_cast<char>((i * 131 + 7) % 251);
  const std::string root = TempDir();
  ASSERT_TRUE(File::WriteStringToFile(root + "card", data));
  std::string written = fmt::format("orca-chunked-sha256 1\n{} {}\n", bytes,
                                    Orca::Mod::SD_HASH_CHUNK_BYTES);
  for (u64 at = 0; at < bytes; at += Orca::Mod::SD_HASH_CHUNK_BYTES)
  {
    const std::string_view piece =
        std::string_view(data).substr(at, Orca::Mod::SD_HASH_CHUNK_BYTES);
    written += Orca::Mod::Sha256Hex(piece);
    written += '\n';
  }
  const std::string hash = Orca::Mod::ChunkedSha256File(root + "card", bytes);
  EXPECT_EQ(hash, Orca::Mod::Sha256Hex(written));
  EXPECT_EQ(hash, v.get("chunkedSha256").get<std::string>());
  // The first bytes only; a file shorter than asked is unreadable.
  EXPECT_EQ(Orca::Mod::ChunkedSha256File(root + "card", 1000),
            Orca::Mod::Sha256Hex(fmt::format("orca-chunked-sha256 1\n1000 {}\n{}\n",
                                              Orca::Mod::SD_HASH_CHUNK_BYTES,
                                              Orca::Mod::Sha256Hex(data.substr(0, 1000)))));
  EXPECT_EQ(Orca::Mod::ChunkedSha256File(root + "card", bytes + 1), "");
  EXPECT_EQ(Orca::Mod::ChunkedSha256File(root + "missing", 10), "");
  EXPECT_EQ(Orca::Mod::ChunkedSha256File(root + "card", 0), "");
  File::DeleteDirRecursively(root);
}

TEST(OrcaBundle, FlatNames)
{
  for (const char* good : {"Project+ Netplay Launcher.dol", "sd.raw", "RSBE01.patches",
                           "PPLUS32.jitwarm", "mod (1).patches", "a", "_x-y+z"})
  {
    EXPECT_FALSE(Orca::Mod::FlatNameProblem(good)) << good;
  }
  for (const std::string& bad :
       {std::string(""), std::string("../RSBE01.patches"), std::string("x/RSBE01.patches"),
        std::string("x\\RSBE01.patches"), std::string(".hidden"), std::string("a..b"),
        std::string("CON.patches"), std::string("com1"), std::string("x."), std::string("x "),
        std::string(" x"), std::string("mod.json"), std::string("MOD.SIG"),
        std::string("tree.json"), std::string(101, 'a'), std::string("caf\xc3\xa9"),
        std::string("a:b"), std::string("a*b"), std::string("tab\tname")})
  {
    EXPECT_TRUE(Orca::Mod::FlatNameProblem(bad)) << bad;
  }
}

// mod.schema.json states the flat form's limits as the engine has them.
TEST(OrcaBundle, SchemaKnowsTheFlatForm)
{
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(RepoPath("Tools/orca/mods/mod.schema.json"), text));
  picojson::value schema;
  ASSERT_EQ(picojson::parse(schema, text), "");
  const picojson::value& limits = schema.get("x-orca-limits");
  EXPECT_EQ(limits.get("flatMaxFiles").get<double>(),
            static_cast<double>(Orca::Mod::FLAT_MAX_FILES));
  EXPECT_EQ(limits.get("flatMaxPatches").get<double>(),
            static_cast<double>(Orca::Mod::FLAT_MAX_PATCHES));
  EXPECT_EQ(limits.get("minFlatSerial").get<double>(),
            static_cast<double>(Orca::Mod::MIN_FLAT_SERIAL));
  EXPECT_EQ(limits.get("sdHashChunkBytes").get<double>(),
            static_cast<double>(Orca::Mod::SD_HASH_CHUNK_BYTES));
  const picojson::value& properties = schema.get("properties");
  EXPECT_EQ(properties.get("patches").get("maxItems").get<double>(),
            static_cast<double>(Orca::Mod::FLAT_MAX_PATCHES));
  EXPECT_EQ(properties.get("sdImage").get("properties").get("bytes").get("maximum").get<double>(),
            static_cast<double>(Orca::Mod::SD_MAX_BYTES));
}

TEST(OrcaBundle, ParsesTheFlatForm)
{
  const std::string card = std::string(64, 'c');
  const std::string pplus = PPlusFlat(2097152000, card);
  std::string error;
  const auto m = Orca::Mod::ParseManifest(pplus, &error);
  ASSERT_TRUE(m) << error;
  EXPECT_EQ(m->format, 2);
  EXPECT_EQ(m->launcher, "Project+ Netplay Launcher.dol");
  ASSERT_TRUE(m->sd_image);
  EXPECT_EQ(m->sd_image->file, "sd.raw");
  EXPECT_EQ(m->sd_image->bytes, 2097152000u);
  EXPECT_EQ(m->sd_image->sha256, card);
  EXPECT_EQ(m->listing, "project-rollback");
  EXPECT_EQ(m->serial, 1u);
  EXPECT_EQ(m->patches, std::vector<std::string>{"PPLUS32.patches"});
  EXPECT_EQ(m->jitwarm, "PPLUS32.jitwarm");
  EXPECT_FALSE(m->sd_bytes);
  EXPECT_EQ(m->FlatFiles(), (std::vector<std::string>{"Project+ Netplay Launcher.dol", "sd.raw",
                                                      "PPLUS32.patches", "PPLUS32.jitwarm"}));
  EXPECT_TRUE(Orca::Mod::ParseManifest(BRAWL_FLAT, &error)) << error;

  // Format 2 has no built card; format 1 has no flat files.
  EXPECT_TRUE(Refused(With(pplus, "sd", R"({"bytes": 2097152000})")));
  EXPECT_TRUE(Refused(With(pplus, "format", "1")));
  EXPECT_TRUE(Refused(With(pplus, "format", "3")));
  EXPECT_TRUE(Refused(With(Without(BRAWL_FLAT, "patches"), "format", "1") + ""));
  // Names: flat, never one of the bundle's own, never twice (also without case).
  EXPECT_TRUE(Refused(With(pplus, "launcher", R"("../Project+ Netplay Launcher.dol")")));
  EXPECT_TRUE(Refused(With(pplus, "launcher", R"("run/launcher.dol")")));
  EXPECT_TRUE(Refused(With(pplus, "launcher", R"("launcher.txt")")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["../RSBE01.patches"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["RSBE01.ini"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["mod.json"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"(["a.patches", "A.patches"])")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "jitwarm", R"("RSBE01.patches")")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "jitwarm", R"("mod.sig")")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "jitwarm", "1")));
  EXPECT_TRUE(Refused(With(BRAWL_FLAT, "patches", R"("RSBE01.patches")")));
  EXPECT_TRUE(Refused(
      With(BRAWL_FLAT, "patches", R"(["a.patches", "b.patches", "c.patches", "d.patches", "e.patches"])")));
  // At most eight files with mod.json and mod.sig: a launcher, a card, four patches and a seed are 9.
  EXPECT_TRUE(Refused(With(pplus, "patches", R"(["a.patches", "b.patches", "c.patches", "d.patches"])")));
  EXPECT_FALSE(Refused(With(pplus, "patches", R"(["a.patches", "b.patches", "c.patches"])")));
  // The pinned card: by size and chunked SHA-256 (never XXH3).
  const auto image = [&](const std::string& fields) {
    return With(pplus, "sdImage", "{" + fields + "}");
  };
  const std::string sha = "\"sha256\": \"" + card + "\"";
  const std::string upper = "\"sha256\": \"" + std::string(64, 'C') + "\"";
  const std::string short_hash = "\"sha256\": \"" + std::string(63, 'c') + "\"";
  const std::string xxh3 = "\"xxh3\": \"5293a846d4de6139fd3ccd61faa9ac8d\"";
  EXPECT_FALSE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 100, " + sha)));
  EXPECT_TRUE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 2147483649, " + sha)));
  EXPECT_TRUE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 0, " + sha)));
  EXPECT_TRUE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 100, " + upper)));
  EXPECT_TRUE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 100, " + short_hash)));
  EXPECT_TRUE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 100")));
  EXPECT_TRUE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 100, " + xxh3)));
  EXPECT_TRUE(Refused(image("\"file\": \"sd/sd.raw\", \"bytes\": 100, " + sha)));
  EXPECT_TRUE(Refused(image("\"file\": \"sd.raw\", \"bytes\": 100, " + sha + ", \"xxh3\": \"x\"")));
  EXPECT_TRUE(Refused(image("\"file\": \"PPLUS32.patches\", \"bytes\": 100, " + sha)));
  // Its listing and serial.
  EXPECT_TRUE(Refused(Without(pplus, "listing")));
  EXPECT_TRUE(Refused(With(pplus, "listing", R"("Project-Rollback")")));
  EXPECT_TRUE(Refused(With(pplus, "listing", R"("../open-brawl")")));
  EXPECT_TRUE(Refused(With(pplus, "listing", R"("")")));
  EXPECT_TRUE(Refused(With(pplus, "listing", "1")));
  EXPECT_TRUE(Refused(Without(pplus, "serial")));
  EXPECT_TRUE(Refused(With(pplus, "serial", "0")));
  EXPECT_TRUE(Refused(With(pplus, "serial", "1.5")));
  EXPECT_TRUE(Refused(With(pplus, "serial", R"("2")")));
  EXPECT_FALSE(Refused(With(pplus, "serial", "4096")));
  // Textures stay the engine's.
  EXPECT_TRUE(Refused(With(pplus, "textures", R"("bundle")")));
  EXPECT_FALSE(Refused(Without(pplus, "textures")));
}

TEST(OrcaBundle, LoadsASignedBundle)
{
  const Signer signer(1);
  const std::string root = TempDir();
  Flat p = MakePPlus(root + "pplus/", signer);
  // The app's own hidden file beside the bundle (YouGame desktop orca-files.ts) is left alone.
  ASSERT_TRUE(File::WriteStringToFile(p.dir + ".copies.json", "{}"));
  std::string error;
  const auto bundle = p.Load(signer, &error);
  ASSERT_TRUE(bundle) << error;
  EXPECT_EQ(bundle->dir, p.dir);
  EXPECT_EQ(bundle->sd_image_hash, Orca::Mod::ChunkedSha256File(p.dir + "sd.raw", 65536));
  EXPECT_EQ(bundle->sd_image_hash.size(), 64u);
  EXPECT_EQ(bundle->jitwarm, "warm");
  ASSERT_EQ(bundle->patches.size(), 1u);
  EXPECT_EQ(bundle->patches[0].first, "PPLUS32.patches");
  EXPECT_EQ(bundle->patches[0].second, PATCHES);
  // The tree: every signed file by name; its hash leaves the JIT seed out.
  ASSERT_EQ(bundle->tree.files.size(), 4u);
  EXPECT_EQ(bundle->tree.files[0].path, "PPLUS32.jitwarm");
  EXPECT_EQ(bundle->tree.files[3].path, "mod.json");
  EXPECT_EQ(bundle->tree.hash, Orca::Mod::FlatTreeHash(bundle->tree.files, "PPLUS32.jitwarm"));
  EXPECT_NE(bundle->tree.hash, Orca::Mod::FlatTreeHash(bundle->tree.files, ""));

  const auto profile = Orca::LoadFlatProfile(*bundle, &error);
  ASSERT_TRUE(profile) << error;
  EXPECT_EQ(profile->game_id, "PPLUS32");
  EXPECT_TRUE(profile->IsLauncher());
  EXPECT_EQ(profile->disc, "RSBE01");
  EXPECT_EQ(profile->sd_image, "sd.raw");
  EXPECT_EQ(profile->jit_clear_frame, 2u);
  ASSERT_EQ(profile->forced.size(), 2u);
  EXPECT_EQ(profile->forced[0].first.key, "WiiSDCard");
  ASSERT_TRUE(profile->mod);
  EXPECT_TRUE(profile->mod->flat);
  EXPECT_TRUE(profile->mod->EngineTextures());
  EXPECT_EQ(profile->mod->JitWarmPath(), p.dir + "PPLUS32.jitwarm");
  EXPECT_EQ(profile->mod->flat_jitwarm, "warm");
  EXPECT_EQ(profile->mod->sd_image_hash, bundle->sd_image_hash);

  // The launcher boot uses the card as it is, with the hash checked at load.
  auto boot = *profile;
  const std::string launcher = p.dir + "Project+ Netplay Launcher.dol";
  if (const char* disc = std::getenv("ORCA_TEST_DISC"))
  {
    bool wrong_disc = false;
    ASSERT_TRUE(Orca::PrepareModBoot(&boot, &launcher, disc, "", 0, &error, &wrong_disc)) << error;
    EXPECT_EQ(boot.sd_image_path, p.dir + "sd.raw");
    EXPECT_EQ(boot.sd_image_hash, bundle->sd_image_hash);
  }

  Flat b = MakeBrawl(root + "brawl/", signer);
  const auto brawl = b.Load(signer, &error);
  ASSERT_TRUE(brawl) << error;
  const auto brawl_profile = Orca::LoadFlatProfile(*brawl, &error);
  ASSERT_TRUE(brawl_profile) << error;
  EXPECT_EQ(brawl_profile->game_id, "RSBE01");
  EXPECT_FALSE(brawl_profile->IsLauncher());
  EXPECT_TRUE(brawl_profile->forced.empty());
  EXPECT_TRUE(brawl->sd_image_hash.empty());
  File::DeleteDirRecursively(root);
}

// Hostile bundles: each refused with a reason, whole (LoadFlatBundle returns nothing).
TEST(OrcaBundle, RefusesTamperedBundles)
{
  const Signer signer(1), stranger(2);
  int n = 0;
  const auto refused = [&](const std::string& what, const std::function<void(Flat&)>& tamper,
                           const std::string& reason, bool pplus = true) {
    const std::string root = TempDir();
    Flat f = pplus ? MakePPlus(root + "b/", signer) : MakeBrawl(root + "b/", signer);
    tamper(f);
    std::string error;
    const auto bundle = f.Load(signer, &error);
    EXPECT_FALSE(bundle) << what;
    EXPECT_NE(error.find(reason), std::string::npos) << what << ": " << error;
    File::DeleteDirRecursively(root);
    ++n;
  };
  // The untampered bundles load.
  {
    const std::string root = TempDir();
    std::string error;
    EXPECT_TRUE(MakePPlus(root + "b/", signer).Load(signer, &error)) << error;
    EXPECT_TRUE(MakeBrawl(root + "c/", signer).Load(signer, &error)) << error;
    File::DeleteDirRecursively(root);
  }

  const auto flip = [](Flat& f, const std::string& name, size_t at) {
    std::string bytes = f.files[name];
    bytes[at] ^= 0x01;
    ASSERT_TRUE(File::WriteStringToFile(f.dir + name, bytes));  // same size, one byte changed
  };
  refused("a patches byte", [&](Flat& f) { flip(f, "PPLUS32.patches", 3); }, "doesn't match");
  refused("a mod.json byte", [&](Flat& f) { flip(f, "mod.json", f.files["mod.json"].find("600") + 2); },
          "doesn't match");
  refused("a launcher byte", [&](Flat& f) { flip(f, "Project+ Netplay Launcher.dol", 100); },
          "doesn't match");
  refused("a JIT seed byte", [&](Flat& f) { flip(f, "PPLUS32.jitwarm", 0); }, "doesn't match");
  refused("an SD image byte", [&](Flat& f) { flip(f, "sd.raw", 40000); }, "not the SD image");
  refused("an SD image of another size",
          [&](Flat& f) { ASSERT_TRUE(File::WriteStringToFile(f.dir + "sd.raw", "short")); },
          "bytes, not the");
  refused("a signature byte",
          [&](Flat& f) {
            std::string sig;
            File::ReadFileToString(f.dir + "mod.sig", sig);
            sig[100] = sig[100] == 'a' ? 'b' : 'a';
            File::WriteStringToFile(f.dir + "mod.sig", sig);
          },
          "doesn't match");
  refused("another key", [&](Flat& f) { f.Sign(stranger); }, "doesn't trust");
  refused("no signature", [&](Flat& f) { File::Delete(f.dir + "mod.sig"); }, "mod.sig is missing");
  refused("a malformed signature",
          [&](Flat& f) { File::WriteStringToFile(f.dir + "mod.sig", "signed, honest\n"); },
          "mod.sig is not");
  refused("a huge signature",
          [&](Flat& f) { File::WriteStringToFile(f.dir + "mod.sig", std::string(4096, 'a')); },
          "mod.sig is over");
  refused("an extra file", [&](Flat& f) { f.Put("RSBE01.ini", "[Profile]\n"); }, "doesn't name it");
  refused("a missing file", [&](Flat& f) { File::Delete(f.dir + "PPLUS32.jitwarm"); },
          "PPLUS32.jitwarm is missing");
  refused("a folder", [&](Flat& f) { File::CreateDir(f.dir + "Textures"); }, "not a file");
  refused("nine files",
          [&](Flat& f) {
            for (const char* name : {"a.patches", "b.patches", "c.patches", "d.patches", "e.patches"})
              f.Put(name, PATCHES);
          },
          "at most 8", false);
  refused("too many entries",
          [&](Flat& f) {
            for (int i = 0; i < 70; ++i)
              File::WriteStringToFile(f.dir + fmt::format(".hidden{}", i), "");
          },
          "entries");
#ifndef _WIN32  // (a symbolic link needs an elevated or developer-mode Windows)
  refused("a link",
          [&](Flat& f) {
            File::Delete(f.dir + "PPLUS32.patches");
            std::error_code ec;
            std::filesystem::create_symlink(f.dir + "PPLUS32.jitwarm", f.dir + "PPLUS32.patches", ec);
            ASSERT_FALSE(ec) << ec.message();
          },
          "is a link");
#endif
  refused("a name that climbs out",
          [&](Flat& f) {
            f.Put("mod.json", With(f.files["mod.json"], "patches", R"(["../PPLUS32.patches"])"));
            f.Sign(signer);
          },
          "patches: ../PPLUS32.patches");
  refused("a named folder",
          [&](Flat& f) {
            f.Put("mod.json", With(f.files["mod.json"], "jitwarm", R"("orca/jitwarm")"));
            f.Sign(signer);
          },
          "jitwarm: a character");
  refused("a huge patches file",
          [&](Flat& f) {
            f.Put("PPLUS32.patches", std::string((1 << 20) + 1, '#'));
            f.Sign(signer);
          },
          "PPLUS32.patches is over");
  refused("a huge launcher (signed)",
          [&](Flat& f) {
            File::IOFile big(f.dir + "Project+ Netplay Launcher.dol", "r+b");
            ASSERT_TRUE(big.Resize((u64{64} << 20) + 1));
          },
          "Project+ Netplay Launcher.dol is over");
  refused("a huge mod.json",
          [&](Flat& f) { f.Put("mod.json", f.files["mod.json"] + std::string(64 << 10, ' ')); },
          "mod.json is over");
  refused("a format 1 mod.json",
          [&](Flat& f) {
            f.Put("mod.json", R"({"format": 1, "engine": {"api": 1, "minOrca": "0.3.35"},
              "disc": {"id": "RSBE01", "revision": 2},
              "frameHook": {"address": "0x80017504", "word": "0x90170100"}})");
            f.Sign(signer);
          },
          "format 1");
  refused("patches that don't parse",
          [&](Flat& f) {
            f.Put("PPLUS32.patches", "this is not a patch\n");
            f.Sign(signer);
          },
          "PPLUS32.patches:");
  refused("no mod.json", [&](Flat& f) { File::Delete(f.dir + "mod.json"); }, "no mod.json");
  EXPECT_GE(n, 24);

  // The listing and the launcher the boot runs.
  const std::string root = TempDir();
  Flat p = MakePPlus(root + "p/", signer);
  std::string error;
  EXPECT_TRUE(p.Load(signer, &error, nullptr, "project-rollback")) << error;
  EXPECT_FALSE(p.Load(signer, &error, nullptr, "open-brawl"));
  EXPECT_NE(error.find("for listing project-rollback, and this is open-brawl"), std::string::npos)
      << error;
  const std::string& dol = p.files["Project+ Netplay Launcher.dol"];
  std::vector<u8> bytes(dol.begin(), dol.end());
  const Orca::Mod::BootLauncher same{"Project+ Netplay Launcher.dol", bytes};
  EXPECT_TRUE(p.Load(signer, &error, &same)) << error;
  const Orca::Mod::BootLauncher other_name{"Other.dol", bytes};
  EXPECT_FALSE(p.Load(signer, &error, &other_name));
  EXPECT_NE(error.find("the boot runs Other.dol, not the bundle's launcher"), std::string::npos)
      << error;
  // What the boot read is what is checked: other bytes are refused even with the file intact, and
  // the file changing after the boot read it changes nothing.
  std::vector<u8> changed = bytes;
  changed[100] ^= 1;
  const Orca::Mod::BootLauncher changed_bytes{"Project+ Netplay Launcher.dol", changed};
  EXPECT_FALSE(p.Load(signer, &error, &changed_bytes));
  EXPECT_NE(error.find("doesn't match"), std::string::npos) << error;
  ASSERT_TRUE(File::WriteStringToFile(p.dir + "Project+ Netplay Launcher.dol", "swapped"));
  EXPECT_TRUE(p.Load(signer, &error, &same)) << error;
  EXPECT_FALSE(p.Load(signer, &error));
  // A Brawl bundle has no launcher: a launcher boot isn't its.
  Flat b = MakeBrawl(root + "b/", signer);
  EXPECT_FALSE(b.Load(signer, &error, &same));
  EXPECT_NE(error.find("not the bundle's launcher (none)"), std::string::npos) << error;
  // No key trusted: nothing loads.
  EXPECT_FALSE(Orca::Mod::LoadFlatBundle(b.dir, {}, &error));
  EXPECT_NE(error.find("trusts no bundle key"), std::string::npos) << error;
  File::DeleteDirRecursively(root);
}

// A launcher as the boot reads it (BootParameters::GenerateFromFile's reader, whose GetBytes()
// BootManager hands ResolveBundle) is checked as its file's bytes, read once: an .elf launcher's
// (Project M 3.6's) as well as a .dol's. The ELF reader parses its own copy of those bytes, made
// before Initialize() byte-swaps the copy's headers, so what runs comes from what was signed.
TEST(OrcaBundle, BootLauncherIsCheckedAsItsFile)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv no_dir("ORCA_MOD_DIR", "");
  const ScopedEnv game("YOUGAME_GAME", "project-rollback");
  const Signer signer(1);
  const PublicKey key = signer.Key();
  Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  const std::string root = TempDir();
  ASSERT_FALSE(root.empty());
  using Orca::Mod::BundleKind;
  std::string error;
  // The session's decision for this launcher boot, from the boot's own reader.
  const auto resolve = [](const BootParameters::Executable& launcher) {
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundleForTests(&launcher.path, launcher.reader->GetBytes(), true);
    return Orca::Mod::ResolvedKind();
  };

  // Every copy an ELF reader makes, as it is made.
  struct Seen
  {
    std::vector<u8> file, copy;
    const u8* file_data = nullptr;
    const u8* copy_data = nullptr;
  };
  std::vector<Seen> seen;
  struct ObserverDone
  {
    ~ObserverDone() { ElfReader::SetCopyObserverForTests({}); }
  } observer_done;
  ElfReader::SetCopyObserverForTests(
      [&seen](std::span<const u8> file_bytes, std::span<const u8> copy_bytes) {
        seen.push_back({{file_bytes.begin(), file_bytes.end()},
                        {copy_bytes.begin(), copy_bytes.end()},
                        file_bytes.data(),
                        copy_bytes.data()});
      });

  // Project M 3.6's layout: an .elf launcher.
  const std::string elf_name = "Project M Launcher.elf";
  Flat pm = MakeLauncherPack(root + "pm/", signer, elf_name, MinimalElf(0x80004000));
  const std::string elf_path = pm.dir + elf_name;
  const std::string elf_file = pm.files[elf_name];
  const std::vector<u8> elf_signed(elf_file.begin(), elf_file.end());
  {
    std::unique_ptr<BootParameters> boot = BootParameters::GenerateFromFile(elf_path);
    ASSERT_TRUE(boot);
    const auto* launcher = std::get_if<BootParameters::Executable>(&boot->parameters);
    ASSERT_NE(launcher, nullptr);
    ASSERT_TRUE(launcher->reader);
    ASSERT_TRUE(launcher->reader->IsValid());
    EXPECT_EQ(launcher->reader->GetEntryPoint(), 0x80004000u);
    const std::vector<u8>& hashed = launcher->reader->GetBytes();

    // The reader's copy, before Initialize() ran: the bytes hashed, which are the file's, in a
    // buffer of its own; the hashed bytes are the reader's one read of the file.
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].file, elf_signed);
    EXPECT_EQ(seen[0].copy, elf_signed);
    EXPECT_EQ(seen[0].copy, hashed);
    EXPECT_EQ(seen[0].file_data, hashed.data());
    EXPECT_NE(seen[0].copy_data, hashed.data());
    // After it: the copy is what the reader parsed (its header now byte-swapped), and the hashed
    // bytes are still the file's.
    const auto& elf = static_cast<const ElfReader&>(*launcher->reader);
    EXPECT_EQ(elf.GetPtr(0), seen[0].copy_data);
    EXPECT_NE(std::memcmp(elf.GetPtr(0), elf_signed.data(), 52), 0);
    EXPECT_EQ(hashed, elf_signed);

    // The pack passes, as LoadFlatBundle and as the session's decision.
    const Orca::Mod::BootLauncher elf_boot{elf_name, hashed};
    EXPECT_TRUE(pm.Load(signer, &error, &elf_boot)) << error;
    EXPECT_EQ(resolve(*launcher), BundleKind::Flat) << Orca::Mod::ResolvedError();
    // The parsed copy (what the boot checked before) is not the signed file.
    const std::vector<u8> parsed(elf.GetPtr(0), elf.GetPtr(0) + hashed.size());
    const Orca::Mod::BootLauncher parsed_boot{elf_name, parsed};
    EXPECT_FALSE(pm.Load(signer, &error, &parsed_boot));
    EXPECT_NE(error.find("doesn't match"), std::string::npos) << error;
  }

  // One byte changed in the .elf, in its code or in its header: refused.
  for (const size_t at : {size_t{0x60}, size_t{0x1b}})
  {
    std::string changed = elf_file;
    changed[at] ^= 1;
    ASSERT_TRUE(File::WriteStringToFile(elf_path, changed));
    std::unique_ptr<BootParameters> boot = BootParameters::GenerateFromFile(elf_path);
    ASSERT_TRUE(boot);
    const auto* launcher = std::get_if<BootParameters::Executable>(&boot->parameters);
    ASSERT_NE(launcher, nullptr);
    ASSERT_TRUE(launcher->reader);
    EXPECT_TRUE(launcher->reader->IsValid()) << at;
    EXPECT_EQ(launcher->reader->GetBytes(), std::vector<u8>(changed.begin(), changed.end()))
        << at;
    const Orca::Mod::BootLauncher changed_boot{elf_name, launcher->reader->GetBytes()};
    EXPECT_FALSE(pm.Load(signer, &error, &changed_boot)) << at;
    EXPECT_NE(error.find("doesn't match"), std::string::npos) << at << ": " << error;
    EXPECT_EQ(resolve(*launcher), BundleKind::Refused) << at;
    EXPECT_NE(Orca::Mod::ResolvedError().find("mod.sig doesn't match the files"),
              std::string::npos)
        << at << ": " << Orca::Mod::ResolvedError();
  }
  ASSERT_TRUE(File::WriteStringToFile(elf_path, elf_file));

  // Project+'s layout, a .dol launcher: its reader holds the file as read, as before; no ELF copy.
  const size_t elf_copies = seen.size();
  const std::string dol_name = "Project+ Netplay Launcher.dol";
  Flat pplus = MakeLauncherPack(root + "pplus/", signer, dol_name, MinimalDol(0x80003100));
  const std::string dol_path = pplus.dir + dol_name;
  const std::string dol_file = pplus.files[dol_name];
  {
    std::unique_ptr<BootParameters> boot = BootParameters::GenerateFromFile(dol_path);
    ASSERT_TRUE(boot);
    const auto* launcher = std::get_if<BootParameters::Executable>(&boot->parameters);
    ASSERT_NE(launcher, nullptr);
    ASSERT_TRUE(launcher->reader);
    ASSERT_TRUE(launcher->reader->IsValid());
    EXPECT_EQ(launcher->reader->GetEntryPoint(), 0x80003100u);
    EXPECT_EQ(launcher->reader->GetBytes(), std::vector<u8>(dol_file.begin(), dol_file.end()));
    EXPECT_EQ(seen.size(), elf_copies);
    const Orca::Mod::BootLauncher dol_boot{dol_name, launcher->reader->GetBytes()};
    EXPECT_TRUE(pplus.Load(signer, &error, &dol_boot)) << error;
    EXPECT_EQ(resolve(*launcher), BundleKind::Flat) << Orca::Mod::ResolvedError();
  }
  {
    std::string changed = dol_file;
    changed[0x100] ^= 1;
    ASSERT_TRUE(File::WriteStringToFile(dol_path, changed));
    std::unique_ptr<BootParameters> boot = BootParameters::GenerateFromFile(dol_path);
    ASSERT_TRUE(boot);
    const auto* launcher = std::get_if<BootParameters::Executable>(&boot->parameters);
    ASSERT_NE(launcher, nullptr);
    ASSERT_TRUE(launcher->reader);
    const Orca::Mod::BootLauncher changed_boot{dol_name, launcher->reader->GetBytes()};
    EXPECT_FALSE(pplus.Load(signer, &error, &changed_boot));
    EXPECT_NE(error.find("doesn't match"), std::string::npos) << error;
    EXPECT_EQ(resolve(*launcher), BundleKind::Refused);
  }

  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResetTrustedKeysForTests();
  File::DeleteDirRecursively(root);
}

// The key: mod mode over the signed lines and the card; a JIT seed change keeps it, anything that
// reaches the game moves it; never a legacy key.
TEST(OrcaBundle, KeyMovesWithTheBundleOnly)
{
  // A launcher profile's key reads the running game's ID (SConfig), as in a session.
  SConfig::Init();
  struct ConfigDone
  {
    ~ConfigDone() { SConfig::Shutdown(); }
  } config_done;
  const Signer signer(1);
  const std::string root = TempDir();
  std::string error;
  const auto key_of = [&](const Flat& f) {
    const auto bundle = f.Load(signer, &error);
    EXPECT_TRUE(bundle) << error;
    auto profile = Orca::LoadFlatProfile(*bundle, &error);
    EXPECT_TRUE(profile) << error;
    Orca::SetActiveProfile(profile);
    const std::string parts = Orca::Online::CompatibilityKeyParts();
    Orca::SetActiveProfile(std::nullopt);
    return parts;
  };
  Flat p = MakePPlus(root + "p/", signer);
  const std::string parts = key_of(p);
  const auto bundle = p.Load(signer, &error);
  ASSERT_TRUE(bundle);
  const std::string disc = Orca::GetEnv("ORCA_DISC_SHA1");
  EXPECT_TRUE(parts.starts_with(fmt::format("orca2|{}|api={}|tree={}|sd={}|disc={}|",
                                            Common::GetScmRevGitStr(), Orca::Mod::ENGINE_API,
                                            bundle->tree.hash, bundle->sd_image_hash,
                                            disc.empty() ? "-" : disc)))
      << parts;
  p.Put("PPLUS32.jitwarm", "warmer");
  p.Sign(signer);
  EXPECT_EQ(key_of(p), parts);
  p.Put("PPLUS32.patches", "# Another patch file with no patches.\n");
  p.Sign(signer);
  EXPECT_NE(key_of(p), parts);
  // Legacy: "orca|..." (OrcaMod.LegacyKeyIsUnchanged pins the rest).
  const auto legacy = Orca::LoadProfileFrom(RepoPath("Data/Sys/"), "RSBE01", 2, &error);
  ASSERT_TRUE(legacy) << error;
  Orca::SetActiveProfile(legacy);
  EXPECT_TRUE(Orca::Online::CompatibilityKeyParts().starts_with("orca|"));
  Orca::SetActiveProfile(std::nullopt);
  File::DeleteDirRecursively(root);
}

// A pack session (mod mode, flat bundle) offers the engine caps "talk" and "rumble" like a legacy
// one, last, whatever its builtin's guards say: they are the app's and the engine's, not the game's.
// The builtin caps (results, locks, ...) stay behind the guards (OrcaMod.ModeGates).
TEST(OrcaBundle, PackSessionsOfferTheEngineCaps)
{
  SConfig::Init();
  struct ConfigDone
  {
    ~ConfigDone() { SConfig::Shutdown(); }
  } config_done;
  const Signer signer(1);
  const std::string root = TempDir();
  const std::string tail =
      fmt::format(" {} {} {} {}", Orca::Status::TALK_CAP, Orca::Status::RUMBLE_CAP,
                  Orca::Status::RECORD_CAP, Orca::Status::HOSTUI_CAP);
  const auto ends_with_tail = [&tail] {
    const std::string offered = Orca::Status::OfferedCaps();
    return offered.size() >= tail.size() &&
           offered.compare(offered.size() - tail.size(), tail.size(), tail) == 0;
  };
  std::string error;
  for (const Flat& f : {MakePPlus(root + "pplus/", signer), MakeBrawl(root + "brawl/", signer)})
  {
    const auto bundle = f.Load(signer, &error);
    ASSERT_TRUE(bundle) << error;
    auto profile = Orca::LoadFlatProfile(*bundle, &error);
    ASSERT_TRUE(profile) << error;
    Orca::SetActiveProfile(profile);
    ASSERT_NE(Orca::Mod::ActiveMod(), nullptr) << f.dir;
    // Guards not read yet, then held, then failed: the engine caps are there every time.
    EXPECT_TRUE(ends_with_tail()) << Orca::Status::OfferedCaps();
    Orca::Mod::NoteGuards(true, 2);
    EXPECT_TRUE(ends_with_tail()) << Orca::Status::OfferedCaps();
    Orca::Mod::NoteGuards(false, 5000);
    EXPECT_TRUE(Orca::Mod::GuardsFailed());
    EXPECT_TRUE(ends_with_tail()) << Orca::Status::OfferedCaps();
    // And the app's agreement turns them on, as in legacy.
    Orca::Status::SetAppCaps("join talk rumble");
    EXPECT_TRUE(Orca::Status::Cap(Orca::Status::TALK_CAP));
    EXPECT_TRUE(Orca::Status::Cap(Orca::Status::RUMBLE_CAP));
    Orca::Status::SetAppCaps("");
    Orca::SetActiveProfile(std::nullopt);
  }
  File::DeleteDirRecursively(root);
}

TEST(OrcaBundle, FindsItsFolder)
{
  const ScopedEnv no_dir("ORCA_MOD_DIR", "");
  const std::string root = TempDir();
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  File::SetUserPath(D_USER_IDX, root + "user/");
  {
    const ScopedEnv game("YOUGAME_GAME", "open-brawl");
    EXPECT_EQ(Orca::Mod::FlatBundleDir(nullptr), root + "user/../run/open-brawl/");
    const std::string launcher = root + "run/project-rollback/Project+ Netplay Launcher.dol";
    EXPECT_EQ(Orca::Mod::FlatBundleDir(&launcher), root + "run/project-rollback/");
    const ScopedEnv dir("ORCA_MOD_DIR", root + "elsewhere");
    EXPECT_EQ(Orca::Mod::FlatBundleDir(nullptr), root + "elsewhere/");
    EXPECT_EQ(Orca::Mod::FlatBundleDir(&launcher), root + "elsewhere/");
  }
  for (const char* bad : {"", "../x", "Open-Brawl", "a/b", "a b"})
  {
    const ScopedEnv game("YOUGAME_GAME", bad);
    EXPECT_EQ(Orca::Mod::FlatBundleDir(nullptr), "") << bad;
  }
  File::SetUserPath(D_USER_IDX, old_user);
  File::DeleteDirRecursively(root);
}

// What a session decides at boot: a good bundle is mod mode; no mod.json is legacy; a refused one
// refuses the session, whether Orca found it by itself or ORCA_MOD_DIR named it (state.md item 7:
// never the built-in game in a refused pack's place).
TEST(OrcaBundle, ResolvesOrRefuses)
{
  const auto resolve = [](const std::string* executable_path,
                          std::span<const u8> launcher_bytes = {}) {
    Orca::Mod::ResolveBundleForTests(executable_path, launcher_bytes, true);
  };
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv no_dir("ORCA_MOD_DIR", "");
  const ScopedEnv brawl_game("YOUGAME_GAME", "open-brawl");
  const Signer signer(1), stranger(2);
  const PublicKey key = signer.Key();
  Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  const std::string root = TempDir();
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  File::SetUserPath(D_USER_IDX, root + "user/");
  File::CreateFullPath(root + "user/");
  using Orca::Mod::BundleKind;

  // Nothing resolved yet: as before bundles.
  Orca::Mod::ResetResolvedBundle();
  EXPECT_FALSE(Orca::Mod::ResolvedKind());
  EXPECT_FALSE(Orca::Mod::Requested());

  // No folder, or one with no mod.json: legacy.
  resolve(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
  EXPECT_FALSE(Orca::Mod::Requested());

  // Brawl's run folder with a signed bundle: mod mode, from that folder.
  Orca::Mod::ResetResolvedBundle();
  Flat b = MakeBrawl(root + "run/open-brawl/", signer);
  resolve(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);
  EXPECT_TRUE(Orca::Mod::Requested());
  ASSERT_NE(Orca::Mod::ResolvedFlat(), nullptr);
  EXPECT_EQ(Orca::Mod::Dir(), root + "user/../run/open-brawl/");
  // Once per process: a change after the check isn't seen until the next process.
  b.Put("RSBE01.patches", "# changed\n");
  resolve(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);

  // The same folder tampered with, in a new process: the session is refused, with the reason the
  // player reads (no folder path), not played as legacy.
  Orca::Mod::ResetResolvedBundle();
  resolve(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
  EXPECT_TRUE(Orca::Mod::Requested());
  EXPECT_EQ(Orca::Mod::ResolvedFlat(), nullptr);
  EXPECT_TRUE(Orca::Mod::ResolvedError().starts_with("YG Dolphin refused this game's pack: "))
      << Orca::Mod::ResolvedError();
  EXPECT_EQ(Orca::Mod::ResolvedError().find(root), std::string::npos)
      << Orca::Mod::ResolvedError();
  b.Sign(signer);
  Orca::Mod::ResetResolvedBundle();
  resolve(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);

  // A build that trusts no key: a mod.json it finds refuses the session.
  Orca::Mod::TrustKeysForTests({});
  Orca::Mod::ResetResolvedBundle();
  resolve(nullptr);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
  EXPECT_NE(Orca::Mod::ResolvedError().find("trusts no bundle key"), std::string::npos);
  Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  // Without a mod.json in the folder that build is legacy, exactly as before bundles.
  {
    const ScopedEnv game("YOUGAME_GAME", "no-pack-here");
    Orca::Mod::TrustKeysForTests({});
    Orca::Mod::ResetResolvedBundle();
    resolve(nullptr);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
    EXPECT_FALSE(Orca::Mod::Requested());
    Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  }

  // Project+: the launcher's own folder; the boot must run the bundle's launcher, for its listing.
  const ScopedEnv pplus_game("YOUGAME_GAME", "project-rollback");
  Flat p = MakePPlus(root + "run/project-rollback/", signer);
  const std::string launcher = p.dir + "Project+ Netplay Launcher.dol";
  const std::string& dol = p.files["Project+ Netplay Launcher.dol"];
  const std::vector<u8> dol_bytes(dol.begin(), dol.end());
  Orca::Mod::ResetResolvedBundle();
  resolve(&launcher, dol_bytes);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);
  Orca::Mod::ResetResolvedBundle();
  resolve(&launcher);  // no reader bytes: the file
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);
  const std::string other = p.dir + "sd.raw";
  Orca::Mod::ResetResolvedBundle();
  resolve(&other);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
  // Under another listing: refused.
  {
    const ScopedEnv game("YOUGAME_GAME", "open-brawl");
    Orca::Mod::ResetResolvedBundle();
    resolve(&launcher, dol_bytes);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
  }
  // Signed by a key the build doesn't pin: refused.
  const PublicKey stranger_key = stranger.Key();
  Orca::Mod::TrustKeysForTests(std::span(&stranger_key, 1));
  Orca::Mod::ResetResolvedBundle();
  resolve(&launcher, dol_bytes);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
  EXPECT_TRUE(Orca::Mod::Requested());
  Orca::Mod::TrustKeysForTests(std::span(&key, 1));

  // A launcher folder without mod.json (legacy Project+): legacy, with a key or without one.
  {
    File::CreateFullPath(root + "legacy-pplus/");
    const std::string legacy_launcher = root + "legacy-pplus/Project+ Netplay Launcher.dol";
    ASSERT_TRUE(File::WriteStringToFile(legacy_launcher, dol));
    Orca::Mod::ResetResolvedBundle();
    resolve(&legacy_launcher, dol_bytes);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
    EXPECT_FALSE(Orca::Mod::Requested());
    Orca::Mod::TrustKeysForTests({});
    Orca::Mod::ResetResolvedBundle();
    resolve(&legacy_launcher, dol_bytes);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
    EXPECT_FALSE(Orca::Mod::Requested());
    Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  }

  // ORCA_MOD_DIR names the folder: a good bundle plays; anything refused refuses the session.
  {
    const ScopedEnv dir("ORCA_MOD_DIR", p.dir);
    Orca::Mod::ResetResolvedBundle();
    resolve(&launcher, dol_bytes);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Flat);
    // A launcher bundle for a disc boot.
    Orca::Mod::ResetResolvedBundle();
    resolve(nullptr);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
    EXPECT_TRUE(Orca::Mod::Requested());
    EXPECT_EQ(Orca::Mod::ResolvedFlat(), nullptr);
    EXPECT_NE(Orca::Mod::ResolvedError().find("this boot is a disc's"), std::string::npos)
        << Orca::Mod::ResolvedError();
    // The player reads it (orca error): the new name, and never the folder's path.
    EXPECT_TRUE(Orca::Mod::ResolvedError().starts_with("YG Dolphin refused this game's pack: "))
        << Orca::Mod::ResolvedError();
    EXPECT_EQ(Orca::Mod::ResolvedError().find(root), std::string::npos)
        << Orca::Mod::ResolvedError();
    // No key trusted.
    Orca::Mod::TrustKeysForTests({});
    Orca::Mod::ResetResolvedBundle();
    resolve(&launcher, dol_bytes);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
    EXPECT_NE(Orca::Mod::ResolvedError().find("trusts no bundle key"), std::string::npos);
    Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  }
  {
    File::CreateFullPath(root + "empty/");
    const ScopedEnv dir("ORCA_MOD_DIR", root + "empty");
    Orca::Mod::ResetResolvedBundle();
    resolve(nullptr);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Refused);
    EXPECT_NE(Orca::Mod::ResolvedError().find("neither tree.json nor mod.json"), std::string::npos);
  }

  // ORCA_MOD_DIR with a tree.json: mods phase 1's tree bundle, as before.
  {
    File::CreateFullPath(root + "tree/");
    File::WriteStringToFile(root + "tree/tree.json", "{}");
    const ScopedEnv dir("ORCA_MOD_DIR", root + "tree");
    Orca::Mod::ResetResolvedBundle();
    resolve(nullptr);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::Tree);
    EXPECT_TRUE(Orca::Mod::Requested());
    EXPECT_EQ(Orca::Mod::Dir(), root + "tree/");
  }
  // Outside a session nothing is looked at.
  {
    const ScopedEnv no_session("ORCA_SESSION", "");
    Orca::Mod::ResetResolvedBundle();
    resolve(&launcher, dol_bytes);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), BundleKind::None);
    EXPECT_FALSE(Orca::Mod::Requested());
  }
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResetTrustedKeysForTests();
  File::SetUserPath(D_USER_IDX, old_user);
  File::DeleteDirRecursively(root);
}

TEST(OrcaBundle, FlatRunsRequireAJailAndLegacyRunsKeepTheirBehavior)
{
  const ScopedEnv session("ORCA_SESSION", "1");
  const ScopedEnv no_dir("ORCA_MOD_DIR", "");
  const ScopedEnv game("YOUGAME_GAME", "open-brawl");
  const std::string root = TempDir();
  ASSERT_FALSE(root.empty());
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  File::SetUserPath(D_USER_IDX, root + "user/");
  File::CreateFullPath(root + "user/");
  const auto done = [&] {
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResetTrustedKeysForTests();
    File::SetUserPath(D_USER_IDX, old_user);
    File::DeleteDirRecursively(root);
  };
  struct Cleanup { std::function<void()> run; ~Cleanup() { run(); } } cleanup{done};
  const Signer signer(1);
  const PublicKey key = signer.Key();
  Orca::Mod::TrustKeysForTests(std::span(&key, 1));
  const Flat pack = MakeBrawl(root + "run/open-brawl/", signer);
  const auto refused = [&] {
    EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::Refused);
    EXPECT_EQ(Orca::Mod::ResolvedError(),
              "YG Dolphin refused this game's pack: this pack requires the YouGame app's sandbox; "
              "open it through YouGame");
    EXPECT_EQ(Orca::Mod::ResolvedFlat(), nullptr);
    EXPECT_TRUE(Orca::Mod::Requested());
  };
  // A valid signed run/<slug>/mod.json is refused before it can supply a boot profile.
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResolveBundleForTests(nullptr, {}, false);
  refused();
  // Direct launcher boots also require the jail when session mode was not requested.
  {
    const ScopedEnv no_session("ORCA_SESSION", "0");
    const std::string launcher = pack.dir + "game.dol";
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundleForTests(&launcher, {}, false);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::Refused);
    EXPECT_EQ(Orca::Mod::ResolvedFlat(), nullptr);
  }
  // Naming the same flat folder explicitly does not change the requirement.
  {
    const ScopedEnv explicit_dir("ORCA_MOD_DIR", pack.dir);
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundleForTests(nullptr, {}, false);
    refused();
  }
  // Launch switches do not change the OS-confirmed state. Also exercise the production entry.
  const bool jailed = Orca::Sandbox::PackJailed();
  {
    const ScopedEnv marker("ORCA_NET", "internet");
    EXPECT_EQ(Orca::Sandbox::PackJailed(), jailed);
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundle(nullptr);
    if (jailed)
      EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::Flat);
    else
      refused();
  }
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResolveBundleForTests(nullptr, {}, true);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::Flat);
  // No mod.json remains legacy, including a launcher's own folder, without a jail.
  {
    const ScopedEnv legacy_game("YOUGAME_GAME", "legacy");
    Orca::Mod::ResetResolvedBundle();
    Orca::Mod::ResolveBundleForTests(nullptr, {}, false);
    EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::None);
    EXPECT_FALSE(Orca::Mod::Requested());
  }
  File::CreateFullPath(root + "legacy/");
  const std::string launcher = root + "legacy/game.dol";
  ASSERT_TRUE(File::WriteStringToFile(launcher, "legacy fixture"));
  Orca::Mod::ResetResolvedBundle();
  Orca::Mod::ResolveBundleForTests(&launcher, {}, false);
  EXPECT_EQ(Orca::Mod::ResolvedKind(), Orca::Mod::BundleKind::None);
  EXPECT_FALSE(Orca::Mod::Requested());
}
