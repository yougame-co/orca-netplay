#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
#
# compat-key-template.py: the lobby compatibility key of a flat pack (mod.json format 2), computed
# without running Orca. Session/Online.cpp ModKeyParts builds the key's parts string from two kinds of
# input:
#   - the build's: its commit, Mod::ENGINE_API, the Sys/GameSettings INIs it ships, the forced
#     session settings (Profile.cpp ForEachUniversalSetting) and the UX version (UX.h kCompatVersion,
#     each game's Manifest::compat);
#   - the pack's: its tree hash (Bundle.cpp FlatTreeHash), the SD image's chunked SHA-256, the disc it
#     declares, its launcher, its forced settings and its builtin.
# `template` writes the build's half once per Orca build, at packaging time, as a small JSON (the
# "key template"); the release puts it beside the manifest. The site (web/src/lib/orca-compat-keys.ts,
# a port of `derive` below) fills in each pack version's half and hashes, so WP05's gates can allowlist
# a pack's keys without a runner. A derived key that is wrong matches nobody: the pack stays at
# friends rooms (fail-safe).
#
#   compat-key-template.py template --source <orca checkout> --sys <build's Sys dir>
#       (--scmrev <build>/Source/Core/Common/scmrev.h | --commit <40 hex>) --version <x.y.z>
#       [--cover RSBE01 ...] [--out <file>]
#     The key code is fingerprinted (KEY_CODE below): when Online.cpp's key functions or the keyed
#     settings change, this refuses until the tool, its TS port and the vectors are updated.
#   compat-key-template.py derive --template <t.json> --pack <pack folder> [--json]
#   compat-key-template.py derive --template <t.json> --mod <mod.json> --tree <64 hex> [--sd <64 hex>]
#     prints every key (one per disc/FMA variant) and the parts string it hashes.
#   compat-key-template.py check --template <t.json> --pack <folder> --log <orca log>...
#     compares with the "Orca: compatibility <key> from <parts>" lines a real run logged.
#   compat-key-template.py selftest [--vectors Tools/orca/mods/compat-key-vectors.json]
#   compat-key-template.py vectors --template <t.json> --name <name> --real <name> <pack> <log> ...
#       [--xxhsum <xxHash's CLI>] --out Tools/orca/mods/compat-key-vectors.json
#     rebuilds the shared vectors from real runs' logs (copy the file byte for byte to YouGame's
#     web/src/lib/orca-compat-keys.vectors.json).
#
# Run-time inputs the template pins (re-check them when the app changes how it runs Orca): the
# desktop app sets no ORCA_DISC_SHA1 (disc=-); sessions start from an empty session NAND, so the seed
# save hash is 0 (Orca.app never seeds; elsewhere the app's user folder holds no save); TEST overrides
# are off; online play forces four pads. FMA is the host's, so both values are variants.
#
# Deterministic: the same template and pack give the same keys on every machine (no clock, no host
# data; the FMA variants are both listed). Python 3.9+, standard library only.
import argparse
import hashlib
import json
import os
import re
import struct
import sys

FORMAT = "orca-compat-key-template 1"
MASK = (1 << 64) - 1

# ---------------------------------------------------------------------------------------------------
# XXH3-64 (seed 0, default secret), xxHash 0.8.x, as Orca's Externals/xxhash computes it.

P32_1, P32_2, P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
P64_1, P64_2, P64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
P64_4, P64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
PRIME_MX1, PRIME_MX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25
SECRET = bytes.fromhex(
    "b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671f"
    "cb79e64eccc0e578825ad07dccff7221b8084674f743248ee03590e6813a264c"
    "3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8"
    "a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364"
    "eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e"
    "2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e")


def _r64(b, i):
    return struct.unpack_from("<Q", b, i)[0]


def _r32(b, i):
    return struct.unpack_from("<I", b, i)[0]


def _rotl64(x, r):
    return ((x << r) | (x >> (64 - r))) & MASK


def _fold(a, b):
    p = a * b
    return (p & MASK) ^ (p >> 64)


def _xxh64_avalanche(h):
    h ^= h >> 33
    h = (h * P64_2) & MASK
    h ^= h >> 29
    h = (h * P64_3) & MASK
    return h ^ (h >> 32)


def _avalanche(h):
    h ^= h >> 37
    h = (h * PRIME_MX1) & MASK
    return h ^ (h >> 32)


def _rrmxmx(h, n):
    h ^= _rotl64(h, 49) ^ _rotl64(h, 24)
    h = (h * PRIME_MX2) & MASK
    h ^= ((h >> 35) + n) & MASK
    h = (h * PRIME_MX2) & MASK
    return h ^ (h >> 28)


def _mix16(data, i, s):
    return _fold(_r64(data, i) ^ _r64(SECRET, s), _r64(data, i + 8) ^ _r64(SECRET, s + 8))


def _accumulate_512(acc, data, i, s):
    for lane in range(8):
        v = _r64(data, i + 8 * lane)
        k = v ^ _r64(SECRET, s + 8 * lane)
        acc[lane ^ 1] = (acc[lane ^ 1] + v) & MASK
        acc[lane] = ((k & 0xFFFFFFFF) * (k >> 32) + acc[lane]) & MASK


def _scramble(acc, s):
    for lane in range(8):
        a = acc[lane]
        a ^= a >> 47
        a ^= _r64(SECRET, s + 8 * lane)
        acc[lane] = (a * P32_1) & MASK


def xxh3_64(data: bytes) -> int:
    n = len(data)
    if n == 0:
        return _xxh64_avalanche(_r64(SECRET, 56) ^ _r64(SECRET, 64))
    if n <= 3:
        c = (data[0] << 16) | (data[n >> 1] << 24) | data[n - 1] | (n << 8)
        return _xxh64_avalanche(c ^ (_r32(SECRET, 0) ^ _r32(SECRET, 4)))
    if n <= 8:
        i1, i2 = _r32(data, 0), _r32(data, n - 4)
        flip = _r64(SECRET, 8) ^ _r64(SECRET, 16)
        return _rrmxmx(((i2 + (i1 << 32)) & MASK) ^ flip, n)
    if n <= 16:
        lo = _r64(data, 0) ^ (_r64(SECRET, 24) ^ _r64(SECRET, 32))
        hi = _r64(data, n - 8) ^ (_r64(SECRET, 40) ^ _r64(SECRET, 48))
        swapped = int.from_bytes(lo.to_bytes(8, "little"), "big")
        return _avalanche((n + swapped + hi + _fold(lo, hi)) & MASK)
    if n <= 128:
        acc = (n * P64_1) & MASK
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(data, 48, 96) + _mix16(data, n - 64, 112)
                acc += _mix16(data, 32, 64) + _mix16(data, n - 48, 80)
            acc += _mix16(data, 16, 32) + _mix16(data, n - 32, 48)
        acc += _mix16(data, 0, 0) + _mix16(data, n - 16, 16)
        return _avalanche(acc & MASK)
    if n <= 240:
        acc = (n * P64_1) & MASK
        for i in range(8):
            acc += _mix16(data, 16 * i, 16 * i)
        acc_end = _mix16(data, n - 16, 136 - 17)
        acc = _avalanche(acc & MASK)
        for i in range(8, n // 16):
            acc_end += _mix16(data, 16 * i, 16 * (i - 8) + 3)
        return _avalanche((acc + acc_end) & MASK)
    # The long path: 1024-byte blocks of 16 stripes, then the last stripe.
    acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]
    stripes_per_block = (len(SECRET) - 64) // 8
    block = 64 * stripes_per_block
    blocks = (n - 1) // block
    for b in range(blocks):
        for s in range(stripes_per_block):
            _accumulate_512(acc, data, b * block + 64 * s, 8 * s)
        _scramble(acc, len(SECRET) - 64)
    stripes = ((n - 1) - block * blocks) // 64
    for s in range(stripes):
        _accumulate_512(acc, data, blocks * block + 64 * s, 8 * s)
    _accumulate_512(acc, data, n - 64, len(SECRET) - 64 - 7)
    result = (n * P64_1) & MASK
    for i in range(4):
        result += _fold(acc[2 * i] ^ _r64(SECRET, 11 + 16 * i), acc[2 * i + 1] ^ _r64(SECRET, 11 + 16 * i + 8))
    return _avalanche(result & MASK)


# ---------------------------------------------------------------------------------------------------
# The key code this tool reproduces. `template` hashes these functions' source text (whitespace
# collapsed) and refuses an unknown fingerprint: a change to the key means a change here, in the TS
# port (web/src/lib/orca-compat-keys.ts) and in the vectors, deliberately.

KEY_FUNCTIONS = [
    ("Source/Core/Core/Orca/Session/Online.cpp", "std::string RunningUxKeyPart()"),
    ("Source/Core/Core/Orca/Session/Online.cpp", "std::string UniversalKeyParts()"),
    ("Source/Core/Core/Orca/Session/Online.cpp", "std::string ModKeyParts(const Profile& profile)"),
    ("Source/Core/Core/Orca/Session/Online.cpp", "std::string UxKeyPart(int engine, int game)"),
    ("Source/Core/Core/Orca/Session/Online.cpp", "std::string CompatibilityKeyParts()"),
    ("Source/Core/Core/Orca/Session/Online.cpp", "std::string CompatibilityKey()"),
    ("Source/Core/Core/Orca/Mod.cpp",
     "std::optional<Profile> ProfileFromManifest(const std::string& dir, Mod::Tree tree,"),
    ("Source/Core/Core/Orca/Bundle.cpp",
     "std::string FlatTreeHash(std::span<const TreeFile> files, std::string_view jitwarm)"),
    ("Source/Core/Core/ConfigLoaders/GameConfigLoader.cpp",
     "std::vector<std::string> GetGameIniFilenames(std::string_view id, std::optional<u16> revision)"),
    ("Source/Core/Core/ConfigManager.cpp", "std::string SConfig::MakeGameID(std::string_view file_name)"),
]
# Fingerprints of KEY_FUNCTIONS this version of the tool reproduces (sha256 hex, first 16).
KEY_CODE = {
    "c6a3154642413e35": "train-fri 005be8ff1f (0.3.41): orca2|<rev>|api|tree|sd|disc|sys|forced|universal|ux",
    # The same key; ProfileFromManifest gives brawl-rev2-results RSBE01 too (a case falling through to
    # brawl-rev2's), and that game's compat is 1, so its key differs from Brawl's only in ux=<engine>.1.
    "f86645e20656efe5": "pmv-e1e2 28ad53925c (0.3.48): c6a3154642413e35 plus builtin brawl-rev2-results",
}

# The settings the key reads (UniversalKeyParts), as Profile.cpp ForEachUniversalSetting forces them
# in a session: regexes over Profile.cpp, each must match exactly once.
KEYED_SETTINGS = {
    "thread": r"f\(Config::MAIN_CPU_THREAD, (false|true)\);",
    "rtc_enable": r"f\(Config::MAIN_CUSTOM_RTC_ENABLE, (false|true)\);",
    "rtc": r"f\(Config::MAIN_CUSTOM_RTC_VALUE, (\d+)u\);",
    "sdcard": r"f\(Config::MAIN_WII_SD_CARD, (false|true)\);",
    "sd_writes": r"f\(Config::MAIN_ALLOW_SD_WRITES, (false|true)\);",
    "efb": r"f\(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, (false|true)\);",
    "xfb": r"f\(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, (false|true)\);",
    "access": r"f\(Config::GFX_HACK_EFB_ACCESS_ENABLE, (false|true)\);",
    "bbox": r"f\(Config::GFX_HACK_BBOX_ENABLE, (false|true)\);",
}
# Online play forces four GameCube pads (Profile.cpp: `four` is true whenever Online::Enabled(), and
# the key is only computed online). Each line must be in Profile.cpp exactly once.
SI_LINES = [
    ("f(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);", 1),
    ("f(Config::GetInfoForSIDevice(1), SerialInterface::SIDEVICE_GC_CONTROLLER);", 1),
    ("f(Config::GetInfoForSIDevice(2), four ? SerialInterface::SIDEVICE_GC_CONTROLLER : "
     "SerialInterface::SIDEVICE_NONE);", 1),
    ("f(Config::GetInfoForSIDevice(3), four ? SerialInterface::SIDEVICE_GC_CONTROLLER : "
     "SerialInterface::SIDEVICE_NONE);", 1),
    ("const bool four = TestPads() == 4 || Online::Enabled();", 1),
]
# The profile's game id ProfileFromManifest gives a builtin (BrawlGame.h's games); any other pack's is
# its disc id. Checked against Mod.cpp's switch (builtin_game_ids), case labels by their enum names.
BUILTIN_GAME_IDS = {"brawl-rev2": "RSBE01", "brawl-rev2-pplus": "PPLUS32", "brawl-rev2-results": "RSBE01"}
BUILTIN_ENUMS = {"brawl-rev2": "BrawlRev2", "brawl-rev2-pplus": "BrawlRev2PPlus",
                 "brawl-rev2-results": "BrawlRev2Results"}
# Each builtin's game compat constant in BrawlGame.h (Manifest::compat: the key's ux=<engine>.<compat>).
BUILTIN_COMPATS = {"brawl-rev2": "BRAWL_REV2_COMPAT", "brawl-rev2-pplus": "BRAWL_REV2_PPLUS_COMPAT",
                   "brawl-rev2-results": "BRAWL_REV2_RESULTS_COMPAT"}
# Config::System (Common/Config/Enums.h) by the name mod.json's forced keys use (Config.cpp
# system_to_name). Only Main is reachable through FORCED_ALLOWLIST today.
SYSTEMS = {"Dolphin": 0}
PLACEHOLDERS = ("tree", "sd", "disc", "sys", "forced", "fma", "rtc", "sdcard", "ux")


def _function_text(source, path, signature):
    with open(os.path.join(source, path), encoding="utf-8") as f:
        text = f.read()
    start = text.find(signature)
    if start < 0 or text.find(signature, start + 1) >= 0:
        raise SystemExit(f"key code: {signature!r} not found exactly once in {path}")
    brace = text.index("{", start)
    depth, i = 0, brace
    while True:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return " ".join(text[start:i + 1].split())


def key_code_fingerprint(source):
    h = hashlib.sha256()
    for path, signature in KEY_FUNCTIONS:
        h.update(path.encode() + b"\0" + _function_text(source, path, signature).encode() + b"\0")
    return h.hexdigest()[:16]


def _one(pattern, text, what):
    found = re.findall(pattern, text)
    if len(found) != 1:
        raise SystemExit(f"key code: {what} ({pattern}) matched {len(found)} times, not once")
    return found[0]


def mod_text(source):
    with open(os.path.join(source, "Source/Core/Core/Orca/Mod.cpp"), encoding="utf-8") as f:
        return f.read()


PROFILE_FROM_MANIFEST = "std::optional<Profile> ProfileFromManifest(const std::string& dir, Mod::Tree tree,"


def builtin_game_ids(mod_cpp):
    """ProfileFromManifest's `switch (manifest.builtin)`: {enum name: game id}, a run of case labels
    (falling through) sharing the `profile.game_id = "<id>";` that follows them. Only the switch's
    shape is read; anything else in it refuses."""
    start = mod_cpp.find(PROFILE_FROM_MANIFEST)
    if start < 0:
        raise SystemExit("key code: ProfileFromManifest not found in Mod.cpp")
    switch = re.search(r"switch \(manifest\.builtin\)\s*\{(.*?)\n  \}", mod_cpp[start:], re.S)
    if not switch:
        raise SystemExit("key code: ProfileFromManifest has no switch (manifest.builtin)")
    ids, labels = {}, []
    for line in switch.group(1).splitlines():
        line = line.strip()
        if not line:
            continue
        case = re.fullmatch(r"case Mod::Builtin::(\w+):", line)
        assign = re.fullmatch(r'profile\.game_id = "([A-Z0-9]{6,7})";', line)
        if case:
            labels.append(case.group(1))
        elif assign and labels:
            for label in labels:
                ids[label] = assign.group(1)
            labels = []
        elif line in ("break;", "default:") or line.startswith("profile.game_id = manifest.disc_id;"):
            labels = []
        else:
            raise SystemExit(f"key code: ProfileFromManifest's switch holds {line!r}, which this tool can't read")
    return ids


def _int_constant(source, path, name):
    with open(os.path.join(source, path), encoding="utf-8") as f:
        return int(_one(rf"constexpr int {name} = (\d+);", f.read(), name))


def ini_names(game_id, revision):
    """ConfigLoaders::GetGameIniFilenames(id, revision), revision always given."""
    if not game_id:
        return []
    names = []
    if len(game_id) == 6:
        names += [f"{game_id[0]}.ini", f"{game_id[:3]}.ini"]
    return names + [f"{game_id}.ini", f"{game_id}r{revision}.ini"]


def make_game_id(file_name):
    """SConfig::MakeGameID: "ID-" and the file name up to its last dot."""
    dot = file_name.rfind(".")
    return "ID-" + (file_name if dot < 0 else file_name[:dot])


def make_template(source, sys_dir, commit, version, covers):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise SystemExit("--commit must be 40 lowercase hex (the build's SCM_REV_STR)")
    fingerprint = key_code_fingerprint(source)
    if fingerprint not in KEY_CODE:
        raise SystemExit(
            f"key code fingerprint {fingerprint} is not one this tool reproduces ({', '.join(KEY_CODE)}): "
            "Online.cpp's key (or what it reads) changed. Update this tool, web/src/lib/orca-compat-keys.ts "
            "and Tools/orca/mods/compat-key-vectors.json, then add the fingerprint to KEY_CODE.")
    with open(os.path.join(source, "Source/Core/Core/Orca/Profile.cpp"), encoding="utf-8") as f:
        profile_cpp = f.read()
    s = {k: _one(p, profile_cpp, k) for k, p in KEYED_SETTINGS.items()}
    flat = " ".join(profile_cpp.split())
    for line, count in SI_LINES:
        if flat.count(line) != count:
            raise SystemExit(f"key code: Profile.cpp no longer has {line!r} {count} time(s)")
    with open(os.path.join(source, "Source/Core/Core/HW/SI/SI_Device.h"), encoding="utf-8") as f:
        si_enum = re.search(r"enum SIDevices : int\s*\{(.*?)\};", f.read(), re.S)
    si_names = re.findall(r"^\s*(SIDEVICE_\w+)\s*,", si_enum.group(1), re.M) if si_enum else []
    if "SIDEVICE_GC_CONTROLLER" not in si_names or "=" in si_enum.group(1):
        raise SystemExit("key code: SI_Device.h's SIDevices enum changed shape")
    si_gc = si_names.index("SIDEVICE_GC_CONTROLLER")
    switch_ids = builtin_game_ids(mod_text(source))
    for builtin, game_id in BUILTIN_GAME_IDS.items():
        if switch_ids.get(BUILTIN_ENUMS[builtin]) != game_id:
            raise SystemExit(f"key code: ProfileFromManifest no longer gives {builtin} the game id {game_id}")
    unknown = sorted(set(switch_ids) - set(BUILTIN_ENUMS.values()))
    if unknown:
        raise SystemExit(f"key code: ProfileFromManifest gives builtins this tool doesn't know a game id: {unknown}")
    api = _int_constant(source, "Source/Core/Core/Orca/Mod.h", "ENGINE_API")
    engine_ux = _int_constant(source, "Source/Core/Core/Orca/UX/UX.h", "kCompatVersion")
    brawl = "Source/Core/Core/Orca/Games/Brawl/BrawlGame.h"
    # No builtin: a flat pack runs GenericModule (UX.cpp), whose game compat is GENERIC_COMPAT (GenericModule.h):
    # the engine logs ux=<engine>.<GENERIC_COMPAT> for it (a 0.3.47 P+Ex run logged ux=31.1).
    games = {"": _int_constant(source, "Source/Core/Core/Orca/Games/Generic/GenericModule.h", "GENERIC_COMPAT")}
    for builtin, constant in BUILTIN_COMPATS.items():
        games[builtin] = _int_constant(source, brawl, constant)
    with open(os.path.join(source, "Source/Core/Core/Orca/Mod.cpp"), encoding="utf-8") as f:
        mod_cpp = f.read()
    allow = re.search(r"FORCED_ALLOWLIST = std::to_array<std::string_view>\(\{(.*?)\}\);", mod_cpp, re.S)
    forced_allow = re.findall(r'"([^"]+)"', re.sub(r"//[^\n]*", "", allow.group(1))) if allow else None
    if not forced_allow:
        raise SystemExit("key code: FORCED_ALLOWLIST not found in Mod.cpp")
    for key in forced_allow:
        if key.split(".")[0] not in SYSTEMS:
            raise SystemExit(f"key code: forced key {key} names a config system this tool doesn't map")
    settings_dir = os.path.join(sys_dir, "GameSettings")
    if not os.path.isdir(settings_dir):
        raise SystemExit(f"{settings_dir} is not a directory (pass the build's Sys folder)")
    names = sorted(os.listdir(settings_dir))
    for c in covers:
        if not re.fullmatch(r"[A-Z0-9]{6}", c):
            raise SystemExit(f"--cover {c}: a disc id is 6 characters A-Z 0-9")
    covers = sorted(set(covers) | set(BUILTIN_GAME_IDS.values()))

    def covered(name):
        if name.startswith("ID-") or name in ("I.ini",):
            return True  # a launcher's boot id (MakeGameID) and its system-code INI
        return any(name in ini_names(c, 0) or re.fullmatch(re.escape(c) + r"r\d+\.ini", name)
                   for c in covers)

    files = {}
    for name in names:
        if covered(name):
            with open(os.path.join(settings_dir, name), "rb") as f:
                files[name] = f"{xxh3_64(f.read()):016x}"
    parts = (f"orca2|{commit}|api={api}|tree={{tree}}|sd={{sd}}|disc={{disc}}|sys={{sys}}|{{forced}}"
             f"thread={s['thread']}|fma={{fma}}|rtc={s['rtc_enable']}:{{rtc}}"
             f"|si={','.join([str(si_gc)] * 4)}|efb={s['efb']}|xfb={s['xfb']}"
             f"|access={s['access']}|bbox={s['bbox']}|save={0:016x}|test=false"
             f"|sdcard={{sdcard}}:{s['sd_writes']}|ux={{ux}}")
    return {
        "format": FORMAT,
        "orca": {"version": version, "commit": commit},
        "keyCode": fingerprint,
        "prefix": "orca2:",
        "parts": parts,
        "fill": {
            # ORCA_DISC_SHA1: the desktop app passes none, so "-"; each value listed is one variant.
            "disc": ["-"],
            # SESSION_USE_FMA = the host's: every ARM64 Mac and most x86-64 PCs "true".
            "fma": ["true", "false"],
            "rtc": int(s["rtc"]),
            "sdcardOff": s["sdcard"],
            "ux": {"engine": engine_ux, "games": games},
            "systems": SYSTEMS,
            "forced": forced_allow,
            # A launcher pack's third INI game is the running one, the disc (proved by a real P+ run).
            "launcherRunning": "disc",
            "sysIni": {"covers": covers, "files": files},
        },
    }


# ---------------------------------------------------------------------------------------------------
# The pack's half (derive). Inputs are the validated mod.json (the site's WP10a validator has already
# refused anything Orca's loader would refuse), the tree hash and the SD image's chunked SHA-256.

class Refused(Exception):
    pass


def _check_template(t):
    if t.get("format") != FORMAT:
        raise Refused(f"template format {t.get('format')!r}, not {FORMAT!r}")
    parts, prefix = t.get("parts"), t.get("prefix")
    if prefix != "orca2:" or not isinstance(parts, str) or not parts.startswith("orca2|"):
        raise Refused("template prefix must be orca2: and its parts start orca2|")
    if not re.fullmatch(r"[!-~]{1,2048}", parts):
        raise Refused("template parts must be 1-2048 printable ASCII characters, no spaces")
    for p in PLACEHOLDERS:
        if parts.count("{" + p + "}") != 1:
            raise Refused(f"template parts must hold {{{p}}} exactly once")
    rest = re.sub(r"\{(?:" + "|".join(PLACEHOLDERS) + r")\}", "", parts)
    if "{" in rest or "}" in rest:
        raise Refused("template parts hold a brace outside its placeholders")


def _ini_part(t, games):
    fill = t["fill"]["sysIni"]
    covers, files = fill["covers"], fill["files"]
    out = ""
    for game_id, revision in games:
        if not (game_id.startswith("ID-") or game_id in covers):
            raise Refused(f"the template does not cover {game_id}'s Sys INIs")
        for name in ini_names(game_id, revision):
            out += files.get(name, "-") + ","
    return out


def pack_parts(t, mod, tree, sd):
    """Every parts string the pack can hash to under template t (one per disc/FMA variant)."""
    _check_template(t)
    fill = t["fill"]
    if not re.fullmatch(r"[0-9a-f]{64}", tree or ""):
        raise Refused("tree must be 64 lowercase hex")
    if sd is not None and not re.fullmatch(r"[0-9a-f]{64}", sd):
        raise Refused("sd must be 64 lowercase hex or none")
    if mod.get("format") != 2:
        raise Refused("only flat packs (mod.json format 2) have derived keys")
    disc = mod.get("disc") or {}
    disc_id, revision = disc.get("id"), disc.get("revision")
    if not (isinstance(disc_id, str) and re.fullmatch(r"[A-Z0-9]{6}", disc_id)) or not (
            isinstance(revision, int) and not isinstance(revision, bool) and 0 <= revision <= 255):
        raise Refused("mod.json disc must be {id: 6 A-Z0-9, revision: 0-255}")
    has_sd = "sdImage" in mod
    if has_sd != (sd is not None):
        raise Refused("sd must be given exactly when mod.json has an sdImage")
    builtin = mod.get("builtin", "")
    games = fill["ux"]["games"]
    if builtin not in games:
        raise Refused(f"the template knows no builtin {builtin!r}")
    # Mod.cpp ProfileFromManifest: the profile's game id.
    game_id = BUILTIN_GAME_IDS.get(builtin, disc_id)
    launcher = mod.get("launcher")
    if launcher is not None:
        if not isinstance(launcher, str) or not launcher or "/" in launcher or "\\" in launcher:
            raise Refused("mod.json launcher must be a file name")
        boot = make_game_id(launcher)
        if fill["launcherRunning"] != "disc":
            raise Refused(f"unknown launcherRunning {fill['launcherRunning']!r}")
        # A launcher boots the disc, and the key is computed once the game runs (the first online
        # frame): Dolphin's running game is then the disc's, its revision from the disc's TMD, which
        # may not be mod.json's. That is only safe while no revision-specific INI of the disc exists.
        if any(re.fullmatch(re.escape(disc_id) + r"r\d+\.ini", n) for n in fill["sysIni"]["files"]):
            raise Refused(f"the build ships a revision-specific {disc_id} INI: a launcher pack's running "
                          "revision (the disc's TMD) is not derivable")
        ini_games = [(boot, 0), (disc_id, revision), (disc_id, revision)]
    else:
        ini_games = [(game_id, revision)]
    sys_part = _ini_part(t, ini_games)
    # The forced settings: the SD card first, then mod.json's (picojson's std::map: key byte order).
    forced = ""
    rtc = fill["rtc"]
    sdcard = fill["sdcardOff"]
    if has_sd:
        forced += "0.Core.WiiSDCard=True|"
        sdcard = "true"
    raw = mod.get("forced") or {}
    if not isinstance(raw, dict):
        raise Refused("mod.json forced must be an object")
    for key in sorted(raw, key=lambda k: k.encode()):
        value = raw[key]
        if key not in fill["forced"]:
            raise Refused(f"forced.{key} is not a setting a mod may force")
        if not (isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 0xFFFFFFFF):
            raise Refused(f"forced.{key} must be an integer from 0 to 4294967295")
        system, section, name = key.split(".", 2)
        forced += f"{fill['systems'][system]}.{section}.{name}={value}|"
        if key == "Dolphin.Core.CustomRTCValue":
            rtc = value
    ux_game = games[builtin]
    ux = f"{fill['ux']['engine']}.{ux_game}" if ux_game > 0 else f"{fill['ux']['engine']}"
    out = []
    for disc_hash in fill["disc"]:
        for fma in fill["fma"]:
            values = {"tree": tree, "sd": sd or "-", "disc": disc_hash, "sys": sys_part,
                      "forced": forced, "fma": fma, "rtc": str(rtc), "sdcard": sdcard, "ux": ux}
            out.append(re.sub(r"\{(" + "|".join(PLACEHOLDERS) + r")\}", lambda m: values[m.group(1)],
                              t["parts"]))
    return out


def key_of(t, parts):
    return f"{t['prefix']}{xxh3_64(parts.encode()):016x}"


def derive(t, mod, tree, sd):
    """[(key, parts)], one per variant, in the template's variant order."""
    return [(key_of(t, p), p) for p in pack_parts(t, mod, tree, sd)]


# ---------------------------------------------------------------------------------------------------
# A pack folder's tree hash (Bundle.cpp FlatTreeHash), for `derive --pack`.

def flat_tree_hash(folder, mod):
    names = ["mod.json"] + list(mod.get("patches") or [])
    for k in ("launcher", "jitwarm"):
        if mod.get(k):
            names.append(mod[k])
    if mod.get("textures") and mod["textures"] != "engine":
        raise SystemExit("--pack: packs with their own textures folder are not handled here")
    names = sorted(set(names), key=lambda n: n.encode())
    text = "orca-flat-bundle 1\n"
    for name in names:
        if name == mod.get("jitwarm"):
            continue
        with open(os.path.join(folder, name), "rb") as f:
            data = f.read()
        text += f"{name}\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\n"
    return hashlib.sha256(text.encode()).hexdigest()


def load_pack(folder):
    with open(os.path.join(folder, "mod.json"), encoding="utf-8") as f:
        mod = json.load(f)
    sd = mod["sdImage"]["sha256"] if "sdImage" in mod else None
    return mod, flat_tree_hash(folder, mod), sd


LOG_LINE = re.compile(r"Orca: compatibility (orca2:[0-9a-f]{16}) from (\S+)")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    tp = sub.add_parser("template")
    tp.add_argument("--source", required=True)
    tp.add_argument("--sys", required=True)
    tp.add_argument("--scmrev")
    tp.add_argument("--commit")
    tp.add_argument("--version", required=True)
    tp.add_argument("--cover", action="append", default=[])
    tp.add_argument("--out")
    for name in ("derive", "check"):
        dp = sub.add_parser(name)
        dp.add_argument("--template", required=True)
        dp.add_argument("--pack")
        dp.add_argument("--mod")
        dp.add_argument("--tree")
        dp.add_argument("--sd")
        dp.add_argument("--json", action="store_true")
        if name == "check":
            dp.add_argument("--log", action="append", required=True)
            dp.add_argument("--test", action="store_true",
                            help="the run had TEST overrides on (test=true in its key)")
    sp = sub.add_parser("selftest")
    sp.add_argument("--vectors", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                      "mods", "compat-key-vectors.json"))
    vp = sub.add_parser("vectors")
    vp.add_argument("--template", required=True)
    vp.add_argument("--name", required=True, help="the template's name in the vectors")
    vp.add_argument("--real", nargs=3, action="append", required=True,
                    metavar=("NAME", "PACK", "LOG"), help="a pack folder and a real run's log")
    vp.add_argument("--xxhsum", help="xxHash's CLI, for independent XXH3 values")
    vp.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    if a.cmd == "template":
        commit = a.commit
        if a.scmrev:
            with open(a.scmrev, encoding="utf-8") as f:
                commit = _one(r'#define SCM_REV_STR "([0-9a-f]{40})"', f.read(), "SCM_REV_STR")
        if not commit:
            raise SystemExit("give --scmrev or --commit")
        covers = a.cover or sorted(
            n[:-4] for n in os.listdir(os.path.join(a.sys, "Orca"))
            if re.fullmatch(r"[A-Z0-9]{6}\.ini", n))
        t = make_template(a.source, a.sys, commit, a.version, covers)
        text = json.dumps(t, indent=1, sort_keys=True) + "\n"
        if a.out:
            with open(a.out, "w", encoding="utf-8") as f:
                f.write(text)
            print(f"{a.out}: {len(text)} bytes, sha256 {hashlib.sha256(text.encode()).hexdigest()}")
        else:
            sys.stdout.write(text)
        return 0

    if a.cmd in ("derive", "check"):
        with open(a.template, encoding="utf-8") as f:
            t = json.load(f)
        if a.pack:
            mod, tree, sd = load_pack(a.pack)
        else:
            with open(a.mod, encoding="utf-8") as f:
                mod = json.load(f)
            tree, sd = a.tree, a.sd
        if a.cmd == "check" and a.test:
            t = dict(t, parts=t["parts"].replace("|test=false|", "|test=true|"))
        try:
            got = derive(t, mod, tree, sd)
        except Refused as e:
            print(f"refused: {e}", file=sys.stderr)
            return 2
        if a.cmd == "derive":
            if a.json:
                print(json.dumps({"tree": tree, "keys": [k for k, _ in got], "parts": [p for _, p in got]},
                                 indent=1))
            else:
                for k, p in got:
                    print(f"{k} {p}")
            return 0
        seen = []
        for path in a.log:
            with open(path, errors="replace") as f:
                seen += LOG_LINE.findall(f.read())
        if not seen:
            print("check: no 'Orca: compatibility' line in the logs", file=sys.stderr)
            return 1
        ok = True
        derived = dict((p, k) for k, p in got)
        for key, parts in sorted(set(seen)):
            if parts in derived and derived[parts] == key:
                print(f"MATCH {key}")
            else:
                ok = False
                print(f"DIFF  real {key} from {parts}")
                for k, p in got:
                    print(f"      derived {k} from {p}")
        if all(key_of(t, p) == k for k, p in seen):
            print("hash: every logged key is XXH3-64 of its logged parts")
        else:
            ok = False
            print("hash: a logged key is not XXH3-64 of its parts (the hash port is wrong)")
        return 0 if ok else 1

    if a.cmd == "selftest":
        with open(a.vectors, encoding="utf-8") as f:
            v = json.load(f)
        fails = selftest(v)
        n = len(v["xxh3"]["cases"]) + len(v["xxh3"]["strings"]) + len(v["real"]) + len(v["derive"])
        print(f"selftest: {n - fails}/{n} vectors pass")
        return 1 if fails else 0

    if a.cmd == "vectors":
        with open(a.template, encoding="utf-8") as f:
            t = json.load(f)
        real = []
        for name, folder, log in a.real:
            real.append(real_vector(t, a.name, name, folder, log))
        v = make_vectors(a.name, t, real, a.xxhsum)
        text = json.dumps(v, indent=1, sort_keys=True) + "\n"
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
        fails = selftest(v)
        print(f"{a.out}: {len(v['derive'])} derive, {len(v['real'])} real, "
              f"{len(v['xxh3']['cases'])} xxh3 vectors; selftest {'ok' if not fails else 'FAILED'}")
        return 1 if fails else 0
    return 2


# ---------------------------------------------------------------------------------------------------
# Shared vectors (Tools/orca/mods/compat-key-vectors.json; YouGame keeps a byte-identical copy at
# web/src/lib/orca-compat-keys.vectors.json, and its tests run the same cases through the TS port).

XXH3_LENGTHS = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 96, 97, 127, 128, 129,
                160, 200, 239, 240, 241, 255, 256, 257, 383, 384, 511, 512, 513, 767, 1023, 1024,
                1025, 1100, 1500, 2047, 2048, 2049, 3000, 4096, 9000]
XXH3_STRINGS = ["", "a", "abc", "orca2|", "hello world", "orca2:0123456789abcdef",
                "Dolphin.Core.CustomRTCValue", "\u00e9\u00e8 utf-8"]


def _xxhsum(xxhsum, data):
    import subprocess
    out = subprocess.run([xxhsum, "-H3", "-"], input=data, capture_output=True, check=True).stdout
    return out.split()[0].decode().split("_", 1)[1]


def real_vector(t, template_name, name, folder, log):
    """A pack and the key a real Orca logged for it ("Orca: compatibility <key> from <parts>")."""
    mod, tree, sd = load_pack(folder)
    with open(log, errors="replace") as f:
        seen = sorted(set(LOG_LINE.findall(f.read())))
    if len(seen) != 1:
        raise SystemExit(f"{log}: {len(seen)} distinct compatibility lines, want 1")
    key, parts = seen[0]
    return {"name": name, "template": template_name, "mod": mod, "tree": tree, "sd": sd,
            "key": key, "parts": parts,
            "source": "a headless Orca run of this build (dolphin-emu-nogui, ORCA_SESSION=1, online)"}


def _case(name, template, mod, tree, sd, t):
    out = {"name": name, "template": template, "mod": mod, "tree": tree, "sd": sd}
    try:
        got = derive(t, mod, tree, sd)
        out["keys"] = [k for k, _ in got]
        out["parts"] = [p for _, p in got]
    except Refused as e:
        out["refused"] = str(e)
    return out


def make_vectors(template_name, t, real, xxhsum):
    """Every derive case is built from the real packs' mod.json by one change each."""
    bf = next(r for r in real if "launcher" not in r["mod"])
    pp = next(r for r in real if "launcher" in r["mod"])
    # A second template: Project+'s own compat bumped to 3 (ux=29.3) and a revision INI shipped.
    bumped = json.loads(json.dumps(t))
    bumped["fill"]["ux"]["games"]["brawl-rev2-pplus"] = 3
    bumped["fill"]["sysIni"]["files"]["RSBE01r2.ini"] = "0123456789abcdef"
    bumped["orca"]["version"] = t["orca"]["version"] + "-vector"
    templates = {template_name: t, "bumped": bumped}

    def edit(base, **changes):
        m = json.loads(json.dumps(base["mod"]))
        for k, val in changes.items():
            if val is None:
                m.pop(k, None)
            else:
                m[k] = val
        return m

    tree2 = hashlib.sha256(b"compat-key vector tree").hexdigest()
    sd2 = hashlib.sha256(b"compat-key vector sd").hexdigest()
    cases = [
        _case("bf-pack", template_name, bf["mod"], bf["tree"], bf["sd"], t),
        _case("pp-pack", template_name, pp["mod"], pp["tree"], pp["sd"], t),
        _case("no builtin (disc RSBE01 rev 2)", template_name, edit(bf, builtin=None), tree2, None, t),
        _case("forced clock", template_name,
              edit(bf, forced={"Dolphin.Core.CustomRTCValue": 1700000000}), tree2, None, t),
        _case("an SD image without a launcher", template_name,
              edit(bf, sdImage={"file": "sd.raw", "bytes": 1048576, "sha256": sd2}), tree2, sd2, t),
        _case("launcher with a 6-character boot id", template_name, edit(pp, launcher="abc.dol"),
              tree2, pp["sd"], t),
        _case("launcher with no dot", template_name, edit(pp, launcher="loader"), tree2, pp["sd"], t),
        _case("P+ builtin without a launcher", template_name, edit(pp, launcher=None), tree2,
              pp["sd"], t),
        _case("bumped: Brawl stays ux=29", "bumped", bf["mod"], bf["tree"], bf["sd"], bumped),
        _case("bumped: P+ builtin without a launcher is ux=29.3", "bumped", edit(pp, launcher=None),
              tree2, pp["sd"], bumped),
        _case("bumped: refused, a launcher's running revision", "bumped", pp["mod"], pp["tree"],
              pp["sd"], bumped),
        _case("refused: a disc the template does not cover", template_name,
              edit(bf, builtin=None, disc={"id": "GALE01", "revision": 2}), tree2, None, t),
        _case("refused: a forced key mods may not force", template_name,
              edit(bf, forced={"Dolphin.Core.CPUThread": 1}), tree2, None, t),
        _case("refused: a forced value that is not an integer", template_name,
              edit(bf, forced={"Dolphin.Core.CustomRTCValue": True}), tree2, None, t),
        _case("refused: an SD hash without an sdImage", template_name, bf["mod"], tree2, sd2, t),
        _case("refused: an sdImage without its hash", template_name, pp["mod"], tree2, None, t),
        _case("refused: a tree that is not 64 hex", template_name, bf["mod"], tree2.upper(), None, t),
        _case("refused: format 1", template_name, edit(bf, format=1), tree2, None, t),
        _case("refused: an unknown builtin", template_name, edit(bf, builtin="melee"), tree2, None, t),
        _case("refused: a launcher path", template_name, edit(pp, launcher="a/b.dol"), tree2,
              pp["sd"], t),
    ]
    xs = []
    for n in XXH3_LENGTHS:
        d = _pattern(n)
        xs.append({"len": n, "xxh3": _xxhsum(xxhsum, d) if xxhsum else f"{xxh3_64(d):016x}"})
    strs = []
    for text in XXH3_STRINGS:
        d = text.encode()
        strs.append({"text": text, "xxh3": _xxhsum(xxhsum, d) if xxhsum else f"{xxh3_64(d):016x}"})
    return {
        "about": "Shared vectors for the lobby compatibility key (Tools/orca/compat-key-template.py "
                 "and YouGame web/src/lib/orca-compat-keys.ts must both pass every one). `real` are "
                 "keys a real Orca logged; `derive` are built from those packs by one change each; "
                 "`xxh3` values come from " + ("xxhsum -H3 (xxHash's own CLI)" if xxhsum else
                                               "this tool's port") + ".",
        "templates": templates,
        "real": real,
        "derive": cases,
        "xxh3": {"bytes": "byte i of a case is (i * 31 + 7) mod 256", "cases": xs, "strings": strs},
    }


def selftest(v):
    fails = 0
    for vec in v["xxh3"]["cases"]:
        got = f"{xxh3_64(_pattern(vec['len'])):016x}"
        if got != vec["xxh3"]:
            fails += 1
            print(f"xxh3 len {vec['len']}: {got}, want {vec['xxh3']}")
    for vec in v["xxh3"]["strings"]:
        got = f"{xxh3_64(vec['text'].encode()):016x}"
        if got != vec["xxh3"]:
            fails += 1
            print(f"xxh3 {vec['text']!r}: {got}, want {vec['xxh3']}")
    for vec in v["real"]:
        t = v["templates"][vec["template"]]
        try:
            got = derive(t, vec["mod"], vec["tree"], vec["sd"])
        except Refused as e:
            got = [("refused", str(e))]
        if (vec["key"], vec["parts"]) not in got or key_of(t, vec["parts"]) != vec["key"]:
            fails += 1
            print(f"real {vec['name']}: {vec['key']} not derived ({got})")
    for vec in v["derive"]:
        t = v["templates"][vec["template"]]
        again = _case(vec["name"], vec["template"], vec["mod"], vec["tree"], vec["sd"], t)
        if again != vec:
            fails += 1
            print(f"derive {vec['name']}: got {again}, want {vec}")
    return fails


def _pattern(n):
    """The vectors' input bytes for a length: byte i is (i * 31 + 7) mod 256."""
    return bytes((i * 31 + 7) & 0xFF for i in range(n))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
