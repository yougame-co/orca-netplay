// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Bundle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <set>
#include <system_error>
#include <thread>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(_WIN32)
#include <windows.h>
// (after windows.h)
#include <bcrypt.h>
#else
#include <mbedtls/sha256.h>
#endif

#include <ed25519.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Sandbox.h"
#include "Core/Orca/UX/GamePatches.h"

namespace Orca::Mod
{
namespace
{
#ifdef ORCA_BUNDLE_TRUST_TEST_KEY
// TEST KEY (bundle-flat branch): compiled in only with CMake -DORCA_BUNDLE_TRUST_TEST_KEY=ON, for
// tests and gates. Its private half was made for them and never committed; release scripts refuse a
// build that has it.
constexpr std::string_view TEST_PUBLIC_KEY =
    "afb9463d5c16f0f617432d76f75f37a8c649e36d7c626088ba00b19dea115a43";
#endif

std::optional<u8> Nibble(char c)
{
  if (c >= '0' && c <= '9')
    return static_cast<u8>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<u8>(c - 'a' + 10);
  return std::nullopt;
}

// Lowercase hex of exactly N bytes.
template <size_t N>
std::optional<std::array<u8, N>> FromHex(std::string_view hex)
{
  if (hex.size() != 2 * N)
    return std::nullopt;
  std::array<u8, N> out{};
  for (size_t i = 0; i < N; ++i)
  {
    const auto hi = Nibble(hex[2 * i]), lo = Nibble(hex[2 * i + 1]);
    if (!hi || !lo)
      return std::nullopt;
    out[i] = static_cast<u8>(*hi << 4 | *lo);
  }
  return out;
}

// A file of the bundle read whole, refused over max_bytes before anything is read.
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

// SHA-256 with the platform's own code where it has one (Apple and Windows use the CPU's SHA
// instructions when there are any), else mbedtls.
class Sha256
{
public:
  Sha256()
  {
#if defined(__APPLE__)
    CC_SHA256_Init(&m_ctx);
#elif defined(_WIN32)
    m_ok =
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0));
#else
    mbedtls_sha256_init(&m_ctx);
    m_ok = mbedtls_sha256_starts_ret(&m_ctx, 0) == 0;
#endif
  }
  ~Sha256()
  {
#if defined(_WIN32)
    if (m_hash)
      BCryptDestroyHash(m_hash);
    if (m_alg)
      BCryptCloseAlgorithmProvider(m_alg, 0);
#elif !defined(__APPLE__)
    mbedtls_sha256_free(&m_ctx);
#endif
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  // `size` at most 4 GiB - 1 (the platform calls take 32 bits).
  void Update(const u8* data, size_t size)
  {
#if defined(__APPLE__)
    CC_SHA256_Update(&m_ctx, data, static_cast<CC_LONG>(size));
#elif defined(_WIN32)
    m_ok = m_ok && BCRYPT_SUCCESS(BCryptHashData(m_hash, const_cast<PUCHAR>(data),
                                                 static_cast<ULONG>(size), 0));
#else
    m_ok = m_ok && mbedtls_sha256_update_ret(&m_ctx, data, size) == 0;
#endif
  }
  std::optional<std::array<u8, 32>> Finish()
  {
    std::array<u8, 32> digest{};
#if defined(__APPLE__)
    CC_SHA256_Final(digest.data(), &m_ctx);
#elif defined(_WIN32)
    m_ok = m_ok && BCRYPT_SUCCESS(BCryptFinishHash(m_hash, digest.data(), 32, 0));
#else
    m_ok = m_ok && mbedtls_sha256_finish_ret(&m_ctx, digest.data()) == 0;
#endif
    return m_ok ? std::optional(digest) : std::nullopt;
  }

private:
  bool m_ok = true;
#if defined(__APPLE__)
  CC_SHA256_CTX m_ctx{};
#elif defined(_WIN32)
  BCRYPT_ALG_HANDLE m_alg = nullptr;
  BCRYPT_HASH_HANDLE m_hash = nullptr;
#else
  mbedtls_sha256_context m_ctx{};
#endif
};

struct Resolution
{
  std::string looked_in;
  BundleKind kind = BundleKind::None;
  std::optional<FlatBundle> flat;
  std::string error;  // Refused: why
};
std::optional<Resolution> s_resolved;
std::optional<std::vector<PublicKey>> s_test_keys;
}  // namespace

std::string FlatSignedMessage(std::span<const TreeFile> files)
{
  std::string text(FLAT_SIGNED_HEADER);
  for (const TreeFile& f : files)
    text += fmt::format("{}\t{}\t{}\n", f.path, f.sha256, f.bytes);
  return text;
}

std::string FlatTreeHash(std::span<const TreeFile> files, std::string_view jitwarm)
{
  std::string text(FLAT_SIGNED_HEADER);
  for (const TreeFile& f : files)
  {
    if (f.path != jitwarm)
      text += fmt::format("{}\t{}\t{}\n", f.path, f.sha256, f.bytes);
  }
  return Sha256Hex(text);
}

std::optional<std::pair<PublicKey, Signature>> ParseSignature(std::string_view text)
{
  if (text.ends_with("\r\n"))
    text.remove_suffix(2);
  else if (text.ends_with('\n'))
    text.remove_suffix(1);
  constexpr std::string_view PREFIX = "ed25519 ";
  if (!text.starts_with(PREFIX))
    return std::nullopt;
  text.remove_prefix(PREFIX.size());
  if (text.size() != 64 + 1 + 128 || text[64] != ' ')
    return std::nullopt;
  const auto key = FromHex<32>(text.substr(0, 64));
  const auto signature = FromHex<64>(text.substr(65));
  if (!key || !signature)
    return std::nullopt;
  return std::make_pair(*key, *signature);
}

bool VerifySignature(const PublicKey& key, const Signature& signature, std::string_view message)
{
  return ed25519_verify(signature.data(), reinterpret_cast<const unsigned char*>(message.data()),
                        message.size(), key.data()) == 1;
}

std::string ChunkedSha256File(const std::string& path, u64 bytes)
{
  const u64 chunks = (bytes + SD_HASH_CHUNK_BYTES - 1) / SD_HASH_CHUNK_BYTES;
  if (chunks == 0)
    return {};
  std::vector<std::array<u8, 32>> digests(chunks);
  std::atomic<bool> ok = true;
  const u64 threads =
      std::clamp<u64>(std::thread::hardware_concurrency(), 1, std::min<u64>(chunks, 8));
  const auto work = [&](u64 first) {
    File::IOFile file(path, "rb");
    std::vector<u8> buffer(4 << 20);
    for (u64 i = first; i < chunks && ok; i += threads)
    {
      const u64 offset = i * SD_HASH_CHUNK_BYTES;
      u64 left = std::min(SD_HASH_CHUNK_BYTES, bytes - offset);
      if (!file || !file.Seek(static_cast<s64>(offset), File::SeekOrigin::Begin))
      {
        ok = false;
        return;
      }
      Sha256 sha;
      while (left > 0)
      {
        const size_t n = static_cast<size_t>(std::min<u64>(left, buffer.size()));
        if (!file.ReadBytes(buffer.data(), n))
        {
          ok = false;
          return;
        }
        sha.Update(buffer.data(), n);
        left -= n;
      }
      const std::optional<std::array<u8, 32>> digest = sha.Finish();
      if (!digest)
      {
        ok = false;
        return;
      }
      digests[i] = *digest;
    }
  };
  std::vector<std::thread> pool;
  for (u64 t = 1; t < threads; ++t)
    pool.emplace_back(work, t);
  work(0);
  for (std::thread& t : pool)
    t.join();
  if (!ok)
    return {};
  std::string text(SD_HASH_HEADER);
  text += fmt::format("{} {}\n", bytes, SD_HASH_CHUNK_BYTES);
  for (const std::array<u8, 32>& digest : digests)
  {
    for (const u8 b : digest)
      text += fmt::format("{:02x}", b);
    text += '\n';
  }
  return Sha256Hex(text);
}

void TrustKeysForTests(std::span<const PublicKey> keys)
{
  s_test_keys.emplace(keys.begin(), keys.end());
}

void ResetTrustedKeysForTests()
{
  s_test_keys.reset();
}

std::span<const PublicKey> TrustedKeys()
{
  if (s_test_keys)
    return *s_test_keys;
  static const std::vector<PublicKey> keys = [] {
    std::vector<PublicKey> out;
#ifdef ORCA_BUNDLE_PUBLIC_KEY
    // (Logged by text: the release script finds the key in the executable.)
    if (const auto key = FromHex<32>(ORCA_BUNDLE_PUBLIC_KEY))
      out.push_back(*key);
    NOTICE_LOG_FMT(BOOT, "Orca bundle: this build trusts key {}{}", ORCA_BUNDLE_PUBLIC_KEY,
                   out.empty() ? " (not 64 lowercase hex digits: ignored)" : "");
#endif
#ifdef ORCA_BUNDLE_TRUST_TEST_KEY
    const auto test_key = FromHex<32>(TEST_PUBLIC_KEY);
    if (test_key && std::ranges::find(out, *test_key) == out.end())
      out.push_back(*test_key);
    WARN_LOG_FMT(BOOT, "Orca bundle: this build trusts the TEST key {} (tests only, not a release)",
                 TEST_PUBLIC_KEY);
#endif
    return out;
  }();
  return keys;
}

bool IsTestKey(const PublicKey& key)
{
#ifdef ORCA_BUNDLE_TRUST_TEST_KEY
  return FromHex<32>(TEST_PUBLIC_KEY) == key;
#else
  (void)key;
  return false;
#endif
}

std::string KeyHex(const PublicKey& key)
{
  std::string hex;
  for (const u8 b : key)
    hex += fmt::format("{:02x}", b);
  return hex;
}

std::optional<FlatBundle> LoadFlatBundle(const std::string& dir_in, std::span<const PublicKey> keys,
                                         std::string* error, const BootLauncher* boot,
                                         std::string_view listing)
{
  namespace fs = std::filesystem;
  const auto fail = [error](std::string why) -> std::optional<FlatBundle> {
    *error = std::move(why);
    return std::nullopt;
  };
  std::string dir = dir_in;
  if (dir.empty())
    return fail("no folder");
  if (dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  if (keys.empty())
    return fail("this build trusts no bundle key");

  // The folder: regular files only (hidden ones aside: the app's .copies.json, a .DS_Store), no
  // more than a flat bundle holds. Nothing is read yet.
  std::set<std::string> present;
  {
    std::error_code ec;
    size_t entries = 0;
    for (fs::directory_iterator it(StringToPath(dir), ec), end; !ec && it != end; it.increment(ec))
    {
      if (++entries > MAX_FOLDER_ENTRIES)
        return fail(fmt::format("more than {} entries in the folder", MAX_FOLDER_ENTRIES));
      const std::string name = PathToString(it->path().filename());
      if (name.starts_with('.'))
        continue;
      std::error_code status_ec;
      const fs::file_status status = it->symlink_status(status_ec);
      if (status_ec)
        return fail(fmt::format("{} can't be read", name));
      if (fs::is_symlink(status))
        return fail(fmt::format("{} is a link (a bundle's files are its own)", name));
      if (!fs::is_regular_file(status))
        return fail(fmt::format("{} is not a file (a flat bundle has no folders)", name));
      present.insert(name);
    }
    if (ec)
      return fail(fmt::format("the folder can't be read ({})", ec.message()));
  }
  if (present.size() > FLAT_MAX_FILES)
  {
    return fail(fmt::format("{} files in the folder; a flat bundle has at most {}", present.size(),
                            FLAT_MAX_FILES));
  }
  if (!present.contains(std::string(FLAT_MANIFEST)))
    return fail("no mod.json");

  // mod.sig first, by a key this Orca trusts: nobody else's mod.json reaches the JSON parser.
  std::string why;
  if (!present.contains(std::string(FLAT_SIGNATURE)))
    return fail("mod.sig is missing");
  std::string sig_text;
  if (!ReadBounded(dir + std::string(FLAT_SIGNATURE), MAX_SIG_BYTES, &sig_text, &why))
    return fail("mod.sig " + why);
  const auto signature = ParseSignature(sig_text);
  if (!signature)
    return fail("mod.sig is not \"ed25519 <public key> <signature>\" in lowercase hex");
  if (std::ranges::find(keys, signature->first) == keys.end())
  {
    return fail(fmt::format("mod.sig is by key {}, which this build doesn't trust",
                            KeyHex(signature->first)));
  }

  // mod.json, format 2, for this listing and not retired.
  std::string json;
  if (!ReadBounded(dir + std::string(FLAT_MANIFEST), MAX_MOD_JSON_BYTES, &json, &why))
    return fail("mod.json " + why);
  std::string parse_error;
  std::optional<Manifest> manifest = ParseManifest(json, &parse_error);
  if (!manifest)
    return fail(parse_error);
  if (manifest->format != 2)
    return fail("mod.json is format 1 (a tree bundle's); a flat bundle's is format 2");
  if (!listing.empty() && manifest->listing != listing)
  {
    return fail(
        fmt::format("the bundle is for listing {}, and this is {}", manifest->listing, listing));
  }
  if (manifest->serial < MIN_FLAT_SERIAL)
  {
    return fail(fmt::format("serial {} is retired (this build takes {} and up)", manifest->serial,
                            MIN_FLAT_SERIAL));
  }
  if (boot && boot->name != manifest->launcher)
  {
    return fail(fmt::format("the boot runs {}, not the bundle's launcher ({})", boot->name,
                            manifest->launcher.empty() ? "none" : manifest->launcher));
  }

  // Exactly the files mod.json names, mod.json and mod.sig.
  const std::vector<std::string> named = manifest->FlatFiles();
  std::set<std::string> expected(named.begin(), named.end());
  expected.emplace(FLAT_MANIFEST);
  expected.emplace(FLAT_SIGNATURE);
  for (const std::string& name : present)
  {
    if (!expected.contains(name))
      return fail(fmt::format("{} is in the folder, and mod.json doesn't name it", name));
  }
  for (const std::string& name : expected)
  {
    if (!present.contains(name))
      return fail(fmt::format("{} is missing", name));
  }

  // Every file but the SD image, read (bounded) and hashed; the patches and JIT seed are kept as
  // read, and the launcher is the boot's own bytes when there are any.
  FlatBundle bundle;
  bundle.dir = dir;
  std::vector<TreeFile> files;
  files.push_back({std::string(FLAT_MANIFEST), Sha256Hex(json), json.size()});
  for (const std::string& name : named)
  {
    if (manifest->sd_image && name == manifest->sd_image->file)
      continue;
    const bool patches = std::ranges::find(manifest->patches, name) != manifest->patches.end();
    const bool launcher = name == manifest->launcher;
    const u64 limit = launcher                     ? MAX_LAUNCHER_BYTES :
                      name == manifest->jitwarm ? MAX_JITWARM_BYTES :
                                                  MAX_PATCHES_BYTES;
    std::string contents;
    if (launcher && boot && !boot->bytes.empty())
    {
      if (boot->bytes.size() > limit)
        return fail(fmt::format("{} is over {} bytes", name, limit));
      contents.assign(reinterpret_cast<const char*>(boot->bytes.data()), boot->bytes.size());
    }
    else if (!ReadBounded(dir + name, limit, &contents, &why))
    {
      return fail(fmt::format("{} {}", name, why));
    }
    files.push_back({name, Sha256Hex(contents), contents.size()});
    if (patches)
      bundle.patches.emplace_back(name, std::move(contents));
    else if (name == manifest->jitwarm)
      bundle.jitwarm = std::move(contents);
  }
  std::ranges::sort(files, {}, &TreeFile::path);
  if (!VerifySignature(signature->first, signature->second, FlatSignedMessage(files)))
  {
    return fail("mod.sig doesn't match the files (one changed since the bundle was signed, or the "
                "signature is damaged)");
  }

  // Signed, so these parse; checked anyway rather than run half of them.
  for (const auto& [name, text] : bundle.patches)
  {
    std::string patch_error;
    if (!UX::ParseGamePatches(text, &patch_error))
      return fail(fmt::format("{}: {}", name, patch_error));
  }

  // The SD image last (up to 2 GiB): its size, then its chunked SHA-256, as mod.json pins them.
  if (manifest->sd_image)
  {
    const SdImageFile& image = *manifest->sd_image;
    const std::string path = dir + image.file;
    const u64 size = File::GetSize(path);
    if (size != image.bytes)
    {
      return fail(fmt::format("{} is {} bytes, not the {} mod.json pins", image.file, size,
                              image.bytes));
    }
    const std::string hash = ChunkedSha256File(path, image.bytes);
    if (hash != image.sha256)
    {
      return fail(fmt::format("{} is not the SD image mod.json pins (chunked SHA-256 {}, not {})",
                              image.file, hash.empty() ? "unreadable" : hash, image.sha256));
    }
    bundle.sd_image_hash = hash;
  }

  bundle.tree.hash = FlatTreeHash(files, manifest->jitwarm);
  bundle.tree.files = std::move(files);
  bundle.manifest = std::move(*manifest);
  bundle.signer = signature->first;
  return bundle;
}

std::string FlatBundleDir(const std::string* executable_path)
{
  std::string dir = GetEnv("ORCA_MOD_DIR");
  if (dir.empty())
  {
    if (executable_path)
    {
      SplitPath(*executable_path, &dir, nullptr, nullptr);
    }
    else
    {
      // A YouGame listing's slug (desktop orca-policy.ts SLUG_RE).
      const std::string slug = GetEnv("YOUGAME_GAME");
      if (slug.empty() || slug.size() > 80 || !std::ranges::all_of(slug, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
          }))
      {
        return {};
      }
      dir = File::GetUserPath(D_USER_IDX);
      if (dir.empty())
        return {};
      if (dir.back() != '/' && dir.back() != '\\')
        dir += '/';
      dir += "../run/" + slug;
    }
  }
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
    dir += '/';
  return dir;
}

static void ResolveBundleWithJail(const std::string* executable_path,
                                 std::span<const u8> launcher_bytes, bool jailed)
{
  const std::string dir = FlatBundleDir(executable_path);
  // Once per process (DolphinNoGUI boots once): a second boot from the same folder keeps the first
  // check. A frontend that boots again in one process must reset it (ResetResolvedBundle) first.
  if (s_resolved && s_resolved->looked_in == dir)
    return;
  Resolution resolution;
  resolution.looked_in = dir;
  const bool explicit_dir = !GetEnv("ORCA_MOD_DIR").empty();
  const auto decide = [&](BundleKind kind) {
    resolution.kind = kind;
    s_resolved = std::move(resolution);
  };
  // A refused bundle refuses the session (as a broken tree bundle does), whether ORCA_MOD_DIR named
  // the folder or Orca found its mod.json by itself: never the built-in game in a pack's place, which
  // would split the player from the pack's room unseen.
  const auto refuse = [&](const std::string& why) {
    ERROR_LOG_FMT(BOOT, "Orca bundle: {} refused: {}. The session is refused.", dir, why);
    // What the player reads (orca error): no folder path, which names their home folder; the log
    // line above keeps it.
    resolution.error = fmt::format("YG Dolphin refused this game's pack: {}", why);
    decide(BundleKind::Refused);
  };
  if (dir.empty())
    return decide(BundleKind::None);
  // Mods phase 1: a run folder the app wrote with tree.json (checked by LoadModProfile at boot).
  if (explicit_dir && File::Exists(dir + "tree.json"))
    return decide(SessionActive() ? BundleKind::Tree : BundleKind::None);
  if (!File::Exists(dir + std::string(FLAT_MANIFEST)))
  {
    if (SessionActive() && explicit_dir)
      return refuse("it has neither tree.json nor mod.json");
    return decide(BundleKind::None);
  }
  if (!jailed)
    return refuse("this pack requires the YouGame app's sandbox; open it through YouGame");
  if (!SessionActive())
    return decide(BundleKind::None);
  if (TrustedKeys().empty())
    return refuse("this build trusts no bundle key");

  const auto start = std::chrono::steady_clock::now();
  std::string why;
  // (Every filesystem call there takes an error code: nothing in it throws.) A launcher boot runs
  // the bundle's launcher, checked as the bytes the boot read; a disc boot needs a bundle without
  // one (BootManager's PrepareModBoot would refuse the session instead).
  std::optional<BootLauncher> boot;
  if (executable_path)
    boot = BootLauncher{PathToFileName(*executable_path), launcher_bytes};
  std::optional<FlatBundle> bundle =
      LoadFlatBundle(dir, TrustedKeys(), &why, boot ? &*boot : nullptr, GetEnv("YOUGAME_GAME"));
  if (bundle && !executable_path && !bundle->manifest.launcher.empty())
  {
    why = fmt::format("the bundle boots through its launcher ({}), and this boot is a disc's",
                      bundle->manifest.launcher);
    bundle.reset();
  }
  if (!bundle)
    return refuse(why);
  NOTICE_LOG_FMT(BOOT,
                 "Orca bundle: {} ({} for {}, serial {}, {} files, tree {}, SD {}), signed by "
                 "{}{}, checked in {} ms",
                 dir, bundle->manifest.title.empty() ? "untitled" : bundle->manifest.title,
                 bundle->manifest.listing, bundle->manifest.serial,
                 bundle->tree.files.size() + (bundle->manifest.sd_image ? 2 : 1),
                 bundle->tree.hash,
                 bundle->sd_image_hash.empty() ? "none" : bundle->sd_image_hash,
                 KeyHex(bundle->signer),
                 IsTestKey(bundle->signer) ? " (the TEST key: not for a release)" : "",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count());
  resolution.flat = std::move(bundle);
  decide(BundleKind::Flat);
}

void ResolveBundle(const std::string* executable_path, std::span<const u8> launcher_bytes)
{
  ResolveBundleWithJail(executable_path, launcher_bytes, Sandbox::PackJailed());
}

void ResolveBundleForTests(const std::string* executable_path, std::span<const u8> launcher_bytes,
                          bool jailed)
{
  ResolveBundleWithJail(executable_path, launcher_bytes, jailed);
}

std::optional<BundleKind> ResolvedKind()
{
  return s_resolved ? std::optional(s_resolved->kind) : std::nullopt;
}

std::string ResolvedDir()
{
  return s_resolved && s_resolved->kind != BundleKind::None ? s_resolved->looked_in : std::string{};
}

std::string ResolvedError()
{
  return s_resolved ? s_resolved->error : std::string{};
}

const FlatBundle* ResolvedFlat()
{
  return s_resolved && s_resolved->flat ? &*s_resolved->flat : nullptr;
}

void ResetResolvedBundle()
{
  s_resolved.reset();
}
}  // namespace Orca::Mod
