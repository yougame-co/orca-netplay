#!/usr/bin/env python3
"""Turn one of Orca's legacy profiles into a mod bundle (YouGame docs/ORCA_MODS.md, Phase 1).

  make-bundle.py RSBE01 <out> [--tree]
  make-bundle.py PPLUS32 <out> --pplus-user <Project+ release's user/ folder> [--tree]
  make-bundle.py --tree <folder>        (tree.json for an existing bundle folder)
  make-bundle.py RSBE01 <out> --flat --serial <n> [--listing <slug>] [--key <private key>]
  make-bundle.py PPLUS32 <out> --flat --serial <n> (--pplus-user <user/> | --pplus-run <folder>)
                 [--listing <slug>] [--key <private key>]

--flat writes a flat bundle (Source/Core/Core/Orca/Bundle.h): mod.json format 2 and the files it
names side by side, at most eight with mod.sig, which --key signs (sign-bundle.py). It is for one
listing (--listing; open-brawl for RSBE01, project-rollback for PPLUS32) and carries --serial, which
must go up with every bundle signed for that listing. Project+'s launcher keeps its official name and
the official sd.raw is used as it is (both checked by their full SHA-256; --any-card takes another
card), pinned by size and chunked SHA-256 (sdhash.py); on macOS big files are APFS clones.
--pplus-run takes a folder with the launcher and sd.raw side by side (the desktop app's run folder).

Reads Data/Sys/Orca/<profile>.ini, .patches, .jitwarm and Textures/RSBE01/ from this checkout (or
--sys), and for Project+ the official release's files: Launcher/Project+ Netplay Launcher.dol and the
files inside Wii/sd.raw (unpacked by fatread.py, standard library only; --7z <7-Zip> to
unpack with 7-Zip instead), plus its license.txt. Writes the bundle to
<out>/ (which must not exist yet): mod.json, launcher.dol, sd/..., orca/patches/<profile>.patches,
orca/textures/..., orca/jitwarm. A bundle never holds tree.json (the app writes it into the run
folder it lays out); --tree also writes one, making <out> a run folder for local mod-mode runs
(ORCA_MOD_DIR, or the desktop app's YOUGAME_ORCA_DEV_MOD).

None of Project+'s files belong in this repository: write P+ bundles outside it.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

MAX_PATH_BYTES = 100
DEVICES = {"con", "prn", "aux", "nul"}
# What each legacy profile becomes.
BUILTIN = {"RSBE01": "brawl-rev2", "PPLUS32": "brawl-rev2-pplus"}
RULESETS = {"RSBE01": ["brawl"], "PPLUS32": ["pplus-2024"]}
OFFERS = ["results", "locks", "queue2"]
# Project+'s official SD image: the card size the bundle pins.
PPLUS_SD_BYTES = 2_097_152_000
PPLUS_LAUNCHER = "Project+ Netplay Launcher.dol"
# SHA-256 of Project+ v3.2's Netplay Launcher and of its sd.raw (PPLUS32.ini names their XXH3-128).
PPLUS_LAUNCHER_SHA256 = "74a0decf1595725928c8830bddc17207669323883112640b0e25e9f2da95c0da"
PPLUS_SD_SHA256 = "cfe5d86976092df50dfdcb21c627d19ba305aa5c9175827a18ca816fc1402d2d"
# The listing each profile's flat bundle is for.
LISTING = {"RSBE01": "open-brawl", "PPLUS32": "project-rollback"}

sys.path.insert(0, HERE)
import sdhash  # noqa: E402


def path_problem(path):
    """Mod::PathProblem (Source/Core/Core/Orca/Mod.cpp)."""
    if not path:
        return "an empty path"
    if len(path.encode()) > MAX_PATH_BYTES:
        return f"longer than {MAX_PATH_BYTES} bytes"
    if any(ord(c) < 0x20 or ord(c) > 0x7E or c in '<>:"\\|?*' for c in path):
        return 'a character outside printable ASCII, or one of <>:"\\|?*'
    for part in path.split("/"):
        if not part:
            return "an empty component"
        if part[0] == ".":
            return "a component that starts with a dot"
        if part[-1] in ". ":
            return "a component that ends in a dot or a space"
        base = part.split(".")[0].lower().rstrip(" ")
        if base in DEVICES or (len(base) == 4 and base[:3] in ("com", "lpt") and base[3].isdigit()):
            return "a Windows device name"
    return None


def host_only(path):
    return path == "orca/jitwarm" or path.startswith("orca/test/")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(8 << 20), b""):
            h.update(block)
    return h.hexdigest()


def tree_hash(files):
    text = "".join(f"{f['path']}\t{f['sha256']}\t{f['bytes']}\n" for f in files if not host_only(f["path"]))
    return hashlib.sha256(text.encode()).hexdigest()


def write_tree(folder):
    """tree.json for a bundle folder: every file but tree.json (and .DS_Store), sorted by path."""
    files = []
    for root, dirs, names in os.walk(folder):
        for name in names:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if rel == "tree.json" or name == ".DS_Store":
                continue
            problem = path_problem(rel)
            if problem:
                sys.exit(f"{rel}: {problem}")
            files.append({"path": rel, "sha256": sha256_file(full), "bytes": os.path.getsize(full)})
    files.sort(key=lambda f: f["path"].encode())
    lower = [f["path"].lower() for f in files]
    if len(set(lower)) != len(lower):
        sys.exit("two paths differ only in case")
    tree = {"tree": tree_hash(files), "files": files}
    with open(os.path.join(folder, "tree.json"), "w") as f:
        json.dump(tree, f, indent=1)
        f.write("\n")
    return tree


def read_profile(path):
    """Data/Sys/Orca/<profile>.ini: {"Profile": [(key, value)], "Forced": [(key, value)]}."""
    sections = {"Profile": [], "Forced": []}
    section = None
    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\r\n")
        at = line.find("#")
        if at == 0 or (at > 0 and line[at - 1] in " \t"):
            line = line[:at]
        line = line.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            if section not in sections:
                sys.exit(f"{path}: unknown section [{section}]")
            continue
        key, eq, value = line.partition("=")
        if not eq or section is None:
            sys.exit(f"{path}: bad line {raw!r}")
        sections[section].append((key.strip(), value.strip()))
    return sections


def hex_word(value):
    return f"0x{int(value, 0):08X}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", nargs="?", choices=sorted(BUILTIN))
    ap.add_argument("out")
    ap.add_argument("--tree", action="store_true", help="also write tree.json (a local run folder)")
    ap.add_argument("--sys", default=os.path.join(REPO, "Data", "Sys"), help="the Sys folder (default: this checkout's)")
    ap.add_argument("--pplus-user", help="Project+ release's user/ folder (Launcher/, Wii/sd.raw)")
    ap.add_argument("--7z", dest="sevenzip", default=None,
                    help="unpack sd.raw with this 7-Zip (7zz or 7z.exe); default: fatread.py, no 7-Zip needed")
    ap.add_argument("--min-orca", default=None, help="engine.minOrca (default 0.4.0; --flat 0.3.35)")
    ap.add_argument("--flat", action="store_true", help="a flat bundle (mod.json format 2)")
    ap.add_argument("--pplus-run", help="--flat: a folder with Project+'s launcher and sd.raw side by side")
    ap.add_argument("--key", help="--flat: sign with this private key (sign-bundle.py keygen)")
    ap.add_argument("--listing", help="--flat: the listing's slug (default: the profile's)")
    ap.add_argument("--serial", type=int, help="--flat: the bundle's serial, above every one signed before")
    ap.add_argument("--any-card", action="store_true", help="--flat: take an sd.raw that isn't Project+ v3.2's")
    args = ap.parse_args()
    if args.min_orca is None:
        args.min_orca = "0.3.35" if args.flat else "0.4.0"
    if args.flat:
        if args.tree or not args.profile:
            sys.exit("--flat takes a profile and no --tree")
        return make_flat(args)

    out = os.path.abspath(args.out)
    if args.profile is None:
        if not args.tree or not os.path.isdir(out):
            sys.exit("give a profile, or --tree <existing bundle folder>")
        tree = write_tree(out)
        print(f"{out}/tree.json: {len(tree['files'])} files, tree {tree['tree']}")
        return
    if os.path.exists(out):
        sys.exit(f"{out} exists; give a new folder")
    if os.path.commonpath([out, REPO]) == REPO and args.profile == "PPLUS32":
        sys.exit("Project+'s files are not ours to commit: write its bundle outside the repository")
    orca = os.path.join(args.sys, "Orca")
    ini = read_profile(os.path.join(orca, args.profile + ".ini"))
    p = dict()
    kept = []
    for key, value in ini["Profile"]:
        if key == "KeepGameCode":
            kept.append([hex_word(w) for w in value.split()])
        else:
            p[key] = value

    launcher = "Launcher" in p
    mod = {
        "format": 1,
        "engine": {"api": 1, "minOrca": args.min_orca},
    }
    if "Title" in p:
        mod["title"] = p["Title"]
    mod["disc"] = {"id": p.get("Disc", args.profile), "revision": int(p["Revision"], 0)}
    if launcher:
        mod["launcher"] = "launcher.dol"
    if "SDImage" in p:
        # The official image's geometry (its boot sector: 2 KiB clusters, two FATs), so the game's
        # SD reads take as many commands as on the official card.
        mod["sd"] = {"bytes": PPLUS_SD_BYTES, "clusterBytes": 2048, "fats": 2}
    mod["frameHook"] = {"address": hex_word(p["FrameHook"]), "word": hex_word(p["FrameHookWord"])}
    if "BootNandFrame" in p:
        mod["bootNandFrame"] = int(p["BootNandFrame"], 0)
    mod["firstKeyframeFrame"] = 300
    if "JitClearFrame" in p:
        mod["jitClearFrame"] = int(p["JitClearFrame"], 0)
    if kept:
        mod["keepGameCode"] = kept
    if "SaveTitle" in p:
        mod["saveTitle"] = f"0x{int(p['SaveTitle'], 16):016X}"
    forced = {}
    for key, value in ini["Forced"]:
        if key == "Dolphin.Core.WiiSDCard":
            # In a mod the card follows "sd".
            if value != "True" or "sd" not in mod:
                sys.exit(f"{args.profile}: WiiSDCard={value} without an SD image")
            continue
        if key != "Dolphin.Core.CustomRTCValue":
            sys.exit(f"{args.profile}: {key} is not on the engine's allowlist")
        forced[key] = int(value, 0)
    if forced:
        mod["forced"] = forced
    mod["builtin"] = BUILTIN[args.profile]
    mod["rulesets"] = RULESETS[args.profile]
    mod["offers"] = OFFERS
    unknown = set(p) - {"Title", "Disc", "Revision", "Launcher", "SDImage", "SDImageHash", "SaveTitle", "FrameHook",
                        "FrameHookWord", "BootNandFrame", "JitClearFrame"}
    if unknown:
        sys.exit(f"{args.profile}: keys this tool doesn't map: {sorted(unknown)}")

    tmp = tempfile.mkdtemp(prefix="orca-bundle-", dir=os.path.dirname(out))
    try:
        def put(rel, src=None, data=None, move=False):
            dst = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if data is not None:
                with open(dst, "wb") as f:
                    f.write(data)
            elif move:
                os.rename(src, dst)
            else:
                shutil.copyfile(src, dst)

        put(f"orca/patches/{args.profile.lower()}.patches", os.path.join(orca, args.profile + ".patches"))
        jitwarm = os.path.join(orca, args.profile + ".jitwarm")
        if os.path.exists(jitwarm):
            put("orca/jitwarm", jitwarm)
        # Orca's texture pack is for the disc's game (RSBE01), which Project+ runs too.
        textures = os.path.join(orca, "Textures", mod["disc"]["id"])
        if os.path.isdir(textures):
            for name in sorted(os.listdir(textures)):
                if name.lower().endswith(".png"):
                    put(f"orca/textures/{name}", os.path.join(textures, name))

        if launcher:
            if not args.pplus_user:
                sys.exit("Project+: give --pplus-user <release's user/ folder>")
            dol = os.path.join(args.pplus_user, "Launcher", PPLUS_LAUNCHER)
            if sha256_file(dol) != PPLUS_LAUNCHER_SHA256:
                sys.exit(f"{dol} is not Project+ v3.2's Netplay Launcher")
            put("launcher.dol", dol)
            lic = os.path.join(os.path.dirname(os.path.abspath(args.pplus_user)), "license.txt")
            if os.path.exists(lic):
                put("license.txt", lic)
        if "sd" in mod:
            image = os.path.join(args.pplus_user, "Wii", p["SDImage"])
            if os.path.getsize(image) != PPLUS_SD_BYTES:
                sys.exit(f"{image} is not {PPLUS_SD_BYTES} bytes")
            unpacked = os.path.join(tmp, ".sd-unpack")
            if args.sevenzip:
                subprocess.run([args.sevenzip, "x", "-y", "-bso0", "-bsp0", f"-o{unpacked}", image], check=True)
            else:
                sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                import fatread

                fatread.extract(image, unpacked)
            for root, dirs, names in os.walk(unpacked):
                dirs.sort()
                for name in sorted(names):
                    src = os.path.join(root, name)
                    rel = os.path.relpath(src, unpacked).replace(os.sep, "/")
                    put("sd/" + rel, src, move=True)
            shutil.rmtree(unpacked)

        put("mod.json", data=(json.dumps(mod, indent=2) + "\n").encode())

        tree = write_tree(tmp)
        files = tree["files"]
        if not args.tree:
            os.remove(os.path.join(tmp, "tree.json"))
        os.rename(tmp, out)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    sd = sum(1 for f in files if f["path"].startswith("sd/"))
    print(f"{out}: {len(files)} files ({sd} on the SD card), tree {tree['tree']}")


def clone(src, dst):
    """A copy of src at dst: an APFS clone on macOS (copy on write, never a hard link), else a copy."""
    if sys.platform == "darwin" and subprocess.run(["cp", "-c", src, dst], capture_output=True).returncode == 0:
        return
    shutil.copyfile(src, dst)


def make_flat(args):
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        sys.exit(f"{out} exists; give a new folder")
    if os.path.commonpath([out, REPO]) == REPO and args.profile == "PPLUS32":
        sys.exit("Project+'s files are not ours to commit: write its bundle outside the repository")
    orca = os.path.join(args.sys, "Orca")
    ini = read_profile(os.path.join(orca, args.profile + ".ini"))
    p, kept = {}, []
    for key, value in ini["Profile"]:
        if key == "KeepGameCode":
            kept.append([hex_word(w) for w in value.split()])
        else:
            p[key] = value
    unknown = set(p) - {"Title", "Disc", "Revision", "Launcher", "SDImage", "SDImageHash", "SaveTitle", "FrameHook",
                        "FrameHookWord", "BootNandFrame", "JitClearFrame"}
    if unknown:
        sys.exit(f"{args.profile}: keys this tool doesn't map: {sorted(unknown)}")

    sources = {}  # bundle file name -> source path
    if args.serial is None or args.serial < 1:
        sys.exit("--flat needs --serial <n> (1 and up, above every bundle signed for the listing)")
    listing = args.listing or LISTING[args.profile]
    if not re.fullmatch(r"[a-z0-9-]{1,80}", listing):
        sys.exit(f"--listing {listing!r} is not a listing slug")
    mod = {"format": 2, "engine": {"api": 1, "minOrca": args.min_orca}, "listing": listing,
           "serial": args.serial}
    if "Title" in p:
        mod["title"] = p["Title"]
    mod["disc"] = {"id": p.get("Disc", args.profile), "revision": int(p["Revision"], 0)}
    if "Launcher" in p:
        if args.pplus_run:
            dol, image = (os.path.join(args.pplus_run, n) for n in (PPLUS_LAUNCHER, p.get("SDImage", "")))
        elif args.pplus_user:
            dol = os.path.join(args.pplus_user, "Launcher", PPLUS_LAUNCHER)
            image = os.path.join(args.pplus_user, "Wii", p.get("SDImage", ""))
        else:
            sys.exit("Project+: give --pplus-user <release's user/ folder> or --pplus-run <folder>")
        if sha256_file(dol) != PPLUS_LAUNCHER_SHA256:
            sys.exit(f"{dol} is not Project+ v3.2's Netplay Launcher")
        mod["launcher"] = PPLUS_LAUNCHER
        sources[PPLUS_LAUNCHER] = dol
        if "SDImage" in p:
            size = os.path.getsize(image)
            if size != PPLUS_SD_BYTES:
                sys.exit(f"{image} is not {PPLUS_SD_BYTES} bytes")
            if sdhash.sha256_file(image) != PPLUS_SD_SHA256:
                if not args.any_card:
                    sys.exit(f"{image} is not Project+ v3.2's sd.raw (--any-card takes it anyway)")
                print(f"note: {image} is not Project+ v3.2's sd.raw")
            mod["sdImage"] = {"file": p["SDImage"], "bytes": size,
                              "sha256": sdhash.chunked_sha256(image, size)}
            sources[p["SDImage"]] = image
    patches = f"{args.profile}.patches"
    mod["patches"] = [patches]
    sources[patches] = os.path.join(orca, patches)
    jitwarm = os.path.join(orca, args.profile + ".jitwarm")
    if os.path.exists(jitwarm):
        mod["jitwarm"] = f"{args.profile}.jitwarm"
        sources[mod["jitwarm"]] = jitwarm
    mod["textures"] = "engine"
    mod["frameHook"] = {"address": hex_word(p["FrameHook"]), "word": hex_word(p["FrameHookWord"])}
    if "BootNandFrame" in p:
        mod["bootNandFrame"] = int(p["BootNandFrame"], 0)
    mod["firstKeyframeFrame"] = 300
    if "JitClearFrame" in p:
        mod["jitClearFrame"] = int(p["JitClearFrame"], 0)
    if kept:
        mod["keepGameCode"] = kept
    if "SaveTitle" in p:
        mod["saveTitle"] = f"0x{int(p['SaveTitle'], 16):016X}"
    forced = {}
    for key, value in ini["Forced"]:
        if key == "Dolphin.Core.WiiSDCard":
            if value != "True" or "sdImage" not in mod:
                sys.exit(f"{args.profile}: WiiSDCard={value} without an SD image")
            continue
        if key != "Dolphin.Core.CustomRTCValue":
            sys.exit(f"{args.profile}: {key} is not on the engine's allowlist")
        forced[key] = int(value, 0)
    if forced:
        mod["forced"] = forced
    mod["builtin"] = BUILTIN[args.profile]
    mod["rulesets"] = RULESETS[args.profile]
    mod["offers"] = OFFERS

    tmp = tempfile.mkdtemp(prefix="orca-flat-", dir=os.path.dirname(out))
    try:
        for name, src in sources.items():
            if path_problem(name) or "/" in name:
                sys.exit(f"{name}: not a flat name")
            clone(src, os.path.join(tmp, name))
        with open(os.path.join(tmp, "mod.json"), "w", newline="\n") as f:
            f.write(json.dumps(mod, indent=2) + "\n")
        os.rename(tmp, out)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    print(f"{out}: {len(sources) + 1} files (mod.json, {', '.join(sources)})")
    if args.key:
        sign = os.path.join(HERE, "sign-bundle.py")
        subprocess.run([sys.executable, "-I", sign, "sign", out, "--key", args.key], check=True)


if __name__ == "__main__":
    main()
