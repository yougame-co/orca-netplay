// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Mod.h"

// Orca flat bundles: a game's data (its profile, patches, JIT seed and, for a launcher game, the
// launcher and the SD image) as at most eight files in one folder, signed. They ride as a YouGame
// listing's runtime files, which the app lays out in userData/orca/run/<slug>/, so Project+ and Brawl
// can change without an Orca release or an app release.
//
//   mod.json   format 2 (Mod.h ParseManifest): the profile, the listing it is for, its serial, and
//              the name of every other file
//   mod.sig    "ed25519 <public key> <signature>\n", 64 and 128 lowercase hex digits
//   ...        the files mod.json names, nothing else (hidden files such as the app's .copies.json
//              aside)
//
// The signature is over FLAT_SIGNED_HEADER and one "<name>\t<sha256>\t<bytes>\n" line for mod.json
// and every file it names but the SD image, sorted by name: mod.json pins the image by size and its
// chunked SHA-256 (ChunkedSha256File, hashed in parallel: the card is 2 GB). Only a key this build
// pins (TrustedKeys) may sign; a build that pins none trusts no bundle.
//
// A bundle is hostile input: every read is bounded, every name checked, and anything wrong refuses
// the whole bundle with a log line, and the session is refused (never the built-in game in its
// place). No mod.json where Orca looks is legacy (Data/Sys/Orca). A session finds the folder itself
// (FlatBundleDir), checks it once per process (ResolveBundle), and plays it in mod mode from the
// bytes it verified (the launcher the boot runs, the patches, the JIT seed): its key is
// "orca2|..." over the signed lines (the JIT seed's left out: host-only) and the SD image's hash,
// so it never meets a legacy player.
namespace Orca::Mod
{
inline constexpr std::string_view FLAT_SIGNED_HEADER = "orca-flat-bundle 1\n";
inline constexpr u64 MAX_SIG_BYTES = 512;
// The SD image's pin: SHA-256 over SD_HASH_HEADER, "<bytes> <SD_HASH_CHUNK_BYTES>\n" and the
// lowercase hex SHA-256 of each SD_HASH_CHUNK_BYTES piece of the image in order, one per line (the
// last piece may be shorter). The pieces hash on every core, so a 2 GB card takes about as long as
// XXH3.
inline constexpr std::string_view SD_HASH_HEADER = "orca-chunked-sha256 1\n";
inline constexpr u64 SD_HASH_CHUNK_BYTES = u64{64} << 20;
// Bundles with a lower mod.json "serial" are refused: a release raises it to retire every bundle
// signed before (a signature can't be taken back).
inline constexpr u64 MIN_FLAT_SERIAL = 1;
// A folder with more entries than this is refused before anything in it is read.
inline constexpr size_t MAX_FOLDER_ENTRIES = 64;

using PublicKey = std::array<u8, 32>;
using Signature = std::array<u8, 64>;

struct FlatBundle
{
  std::string dir;  // with a trailing '/'
  Manifest manifest;
  // mod.json and every file it names but the SD image, sorted by name, as verified; `hash` is the
  // key's: the hash of those lines without the JIT seed's.
  Tree tree;
  std::vector<std::pair<std::string, std::string>> patches;  // (name, text), mod.json's order
  std::string jitwarm;        // the JIT seed's bytes as verified; empty without one
  std::string sd_image_hash;  // the SD image's chunked SHA-256 as checked; empty without one
  PublicKey signer{};
};

// What mod.sig signs.
std::string FlatSignedMessage(std::span<const TreeFile> files);
// The key's tree hash for a flat bundle: TreeHash's lines for every file but the JIT seed.
std::string FlatTreeHash(std::span<const TreeFile> files, std::string_view jitwarm);
// mod.sig's contents, or nullopt.
std::optional<std::pair<PublicKey, Signature>> ParseSignature(std::string_view text);
bool VerifySignature(const PublicKey& key, const Signature& signature, std::string_view message);
// The SD image pin of the first `bytes` bytes of `path` (SD_HASH_HEADER), or empty when the file
// can't be read or is shorter.
std::string ChunkedSha256File(const std::string& path, u64 bytes);

// The keys a bundle may be signed with: the one this build pins (CMake ORCA_BUNDLE_PUBLIC_KEY), and
// the bundle-flat TEST key only in a build configured with ORCA_BUNDLE_TRUST_TEST_KEY (tests and
// gates; release scripts refuse it). A build with neither trusts no key: flat bundles are off.
std::span<const PublicKey> TrustedKeys();
// Tests: trust exactly these keys (none: no bundle), until ResetTrustedKeysForTests.
void TrustKeysForTests(std::span<const PublicKey> keys);
void ResetTrustedKeysForTests();
bool IsTestKey(const PublicKey& key);
std::string KeyHex(const PublicKey& key);

// The launcher a boot runs: its file name, and the bytes the boot's reader holds (Boot.h), which
// are what runs. Empty bytes: read the bundle's file instead.
struct BootLauncher
{
  std::string name;
  std::span<const u8> bytes;
};

// Checks the folder `dir` as a flat bundle signed by one of `keys`: nullopt and why on anything
// wrong. With `boot`, the bundle's launcher must be the one the boot runs, and the signed line for
// it is over the boot's own bytes (never the file read a second time). `listing`, when not empty,
// must be mod.json's. Hashes the SD image only once everything else, the signature included, holds.
std::optional<FlatBundle> LoadFlatBundle(const std::string& dir, std::span<const PublicKey> keys,
                                         std::string* error, const BootLauncher* boot = nullptr,
                                         std::string_view listing = {});

// Where a session looks for a flat bundle: ORCA_MOD_DIR when set; else a launcher boot's own
// folder (where Project+'s sd.raw already lies); else, for a disc boot of a YouGame listing, the
// app's run folder beside the user folder: <-u user>/../run/<YOUGAME_GAME>/ (YouGame desktop
// orca.ts: userData/orca/{user,run/<slug>}). Empty when there is nowhere to look.
std::string FlatBundleDir(const std::string* executable_path);

// The session's bundle, decided once per process at boot (BootManager, before any game setting is
// read): a tree bundle (ORCA_MOD_DIR with a tree.json, mods phase 1, unchanged), a flat bundle that
// passed LoadFlatBundle, none (legacy: no mod.json where Orca looked), or Refused: a mod.json that
// didn't pass (whether Orca found it or ORCA_MOD_DIR named its folder; a build that trusts no key
// refuses every one), or an ORCA_MOD_DIR that is neither a tree bundle nor a flat one, so the
// session is refused (BootManager) rather than run legacy.
enum class BundleKind
{
  None,
  Tree,
  Flat,
  Refused,
};
// `launcher_bytes`: what the boot's reader holds for `executable_path` (Boot.h).
void ResolveBundle(const std::string* executable_path, std::span<const u8> launcher_bytes = {});
// Tests: exercise the same resolution with a supplied jail state. Production boots always use
// ResolveBundle's OS query.
void ResolveBundleForTests(const std::string* executable_path, std::span<const u8> launcher_bytes,
                          bool jailed);
// nullopt before ResolveBundle ran.
std::optional<BundleKind> ResolvedKind();
std::string ResolvedDir();
const FlatBundle* ResolvedFlat();
// Why a Refused bundle was refused.
std::string ResolvedError();
// Tests: forget the decision.
void ResetResolvedBundle();
}  // namespace Orca::Mod
