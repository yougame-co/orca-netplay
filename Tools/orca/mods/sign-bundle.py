#!/usr/bin/env python3
"""Sign and check Orca flat bundles (Source/Core/Core/Orca/Bundle.h), standard library only.

  sign-bundle.py keygen <private key file>          a new key: 32 random bytes as hex (mode 600);
                                                    prints the public key Orca pins
                                                    (CMake -DORCA_BUNDLE_PUBLIC_KEY=<hex>)
  sign-bundle.py sign <bundle> --key <private key>  checks the bundle as Orca does (the SD image's
                                                    size and chunked SHA-256 included), then writes
                                                    <bundle>/mod.sig
  sign-bundle.py verify <bundle> --public <hex>     checks the bundle as Orca does
  sign-bundle.py message <bundle>                   prints the lines mod.sig signs
  sign-bundle.py vector <out.json>                  the shared test vector (a fixed key, not a real one)

A bundle folder holds mod.json (format 2), mod.sig and exactly the files mod.json names, at most 8
in all. mod.sig is "ed25519 <public key> <signature>\\n" over "orca-flat-bundle 1\\n" and a
"<name>\\t<sha256>\\t<bytes>\\n" line for mod.json and every file it names but the SD image (mod.json
pins that one by size and chunked SHA-256, sdhash.py), sorted by name. Keep private keys out of every
repository.

Sign only a bundle that played on two Orcas (ORCA.md "Flat bundles"): one whose builtin's guards
never hold reaches no origin, so its players can't meet anyone.
"""

import argparse
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519ref  # noqa: E402
import sdhash  # noqa: E402

HEADER = b"orca-flat-bundle 1\n"
MAX_FILES = 8
MAX_FOLDER_ENTRIES = 64
MAX_MOD_JSON = 64 << 10
MAX_SIG = 512
MAX_LAUNCHER = 64 << 20
MAX_PATCHES = 1 << 20
MAX_JITWARM = 16 << 20
MAX_NAME = 100
NAME_RE = re.compile(r"^[A-Za-z0-9+ ()_-][A-Za-z0-9+ ()._-]*$")
DEVICES = {"con", "prn", "aux", "nul"}
OWN = {"mod.json", "mod.sig", "tree.json"}


def name_problem(name):
    """Mod::FlatNameProblem (Source/Core/Core/Orca/Mod.cpp)."""
    if not isinstance(name, str) or not name:
        return "an empty name"
    if len(name.encode()) > MAX_NAME:
        return f"longer than {MAX_NAME} bytes"
    if not NAME_RE.match(name):
        return "a character other than A-Z a-z 0-9 + ( ) _ - space, and '.' after the first"
    if ".." in name:
        return '".."'
    if name[0] == " ":
        return "a name that starts with a space"
    if name[-1] in ". ":
        return "a component that ends in a dot or a space"
    base = name.split(".")[0].lower().rstrip(" ")
    if base in DEVICES or (len(base) == 4 and base[:3] in ("com", "lpt") and base[3].isdigit()):
        return "a Windows device name"
    if name.lower() in OWN:
        return "one of the bundle's own names (mod.json, mod.sig, tree.json)"
    return None


def named_files(mod):
    """Manifest::FlatFiles: launcher, SD image, patches, JIT seed."""
    files = []
    if mod.get("launcher"):
        files.append(mod["launcher"])
    if "sdImage" in mod:
        files.append(mod["sdImage"]["file"])
    files += mod.get("patches", [])
    if mod.get("jitwarm"):
        files.append(mod["jitwarm"])
    return files


def read_bounded(path, limit):
    size = os.path.getsize(path)
    if size > limit:
        raise SystemExit(f"{os.path.basename(path)} is over {limit} bytes")
    with open(path, "rb") as f:
        return f.read()


def load(folder):
    """The bundle's mod.json and the lines mod.sig signs, checking the folder as Orca does."""
    entries = sorted(os.listdir(folder))
    if len(entries) > MAX_FOLDER_ENTRIES:
        raise SystemExit(f"more than {MAX_FOLDER_ENTRIES} entries in the folder")
    present = set()
    for name in entries:
        if name.startswith("."):
            continue
        full = os.path.join(folder, name)
        if os.path.islink(full):
            raise SystemExit(f"{name} is a link")
        if not os.path.isfile(full):
            raise SystemExit(f"{name} is not a file")
        present.add(name)
    if len(present) > MAX_FILES:
        raise SystemExit(f"{len(present)} files; a flat bundle has at most {MAX_FILES}")
    raw = read_bounded(os.path.join(folder, "mod.json"), MAX_MOD_JSON)
    mod = json.loads(raw)
    if mod.get("format") != 2:
        raise SystemExit("mod.json is not format 2")
    if not isinstance(mod.get("listing"), str) or not re.fullmatch(r"[a-z0-9-]{1,80}", mod["listing"]):
        raise SystemExit("mod.json has no listing (a listing's slug)")
    if type(mod.get("serial")) is not int or mod["serial"] < 1:
        raise SystemExit("mod.json has no serial (an integer from 1)")
    if "sdImage" in mod and set(mod["sdImage"]) != {"file", "bytes", "sha256"}:
        raise SystemExit("mod.json's sdImage is {file, bytes, sha256}")
    named = named_files(mod)
    for name in named:
        problem = name_problem(name)
        if problem:
            raise SystemExit(f"mod.json names {name!r}: {problem}")
    if len({n.lower() for n in named}) != len(named):
        raise SystemExit("mod.json names a file twice")
    if len(named) + 2 > MAX_FILES:
        raise SystemExit(f"a flat bundle has at most {MAX_FILES} files, mod.json and mod.sig included")
    expected = set(named) | {"mod.json", "mod.sig"}
    extra, missing = present - expected, (expected - {"mod.sig"}) - present
    if extra:
        raise SystemExit(f"{sorted(extra)[0]} is in the folder, and mod.json doesn't name it")
    if missing:
        raise SystemExit(f"{sorted(missing)[0]} is missing")
    lines = [("mod.json", hashlib.sha256(raw).hexdigest(), len(raw))]
    sd = mod.get("sdImage", {}).get("file")
    for name in named:
        if name == sd:
            continue
        limit = MAX_LAUNCHER if name == mod.get("launcher") else MAX_JITWARM if name == mod.get("jitwarm") else MAX_PATCHES
        data = read_bounded(os.path.join(folder, name), limit)
        lines.append((name, hashlib.sha256(data).hexdigest(), len(data)))
    lines.sort(key=lambda line: line[0].encode())
    if sd:
        size = os.path.getsize(os.path.join(folder, sd))
        if size != mod["sdImage"]["bytes"]:
            raise SystemExit(f"{sd} is {size} bytes, not the {mod['sdImage']['bytes']} mod.json pins")
    return mod, lines


def check_card(folder, mod):
    """The SD image is the one mod.json pins (its chunked SHA-256, as Orca checks it)."""
    if "sdImage" not in mod:
        return
    image = mod["sdImage"]
    got = sdhash.chunked_sha256(os.path.join(folder, image["file"]), image["bytes"])
    if got != image["sha256"]:
        raise SystemExit(f"{image['file']} is chunked SHA-256 {got}, not the {image['sha256']} mod.json pins")


def message(lines):
    return HEADER + b"".join(f"{n}\t{h}\t{b}\n".encode() for n, h, b in lines)


def read_seed(path):
    seed = bytes.fromhex(open(path).read().strip())
    if len(seed) != 32:
        raise SystemExit(f"{path} is not a 32-byte Ed25519 private key in hex")
    return seed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("key")
    s = sub.add_parser("sign")
    s.add_argument("bundle")
    s.add_argument("--key", required=True)
    v = sub.add_parser("verify")
    v.add_argument("bundle")
    v.add_argument("--public", required=True)
    m = sub.add_parser("message")
    m.add_argument("bundle")
    t = sub.add_parser("vector")
    t.add_argument("out")
    args = ap.parse_args()

    if args.cmd == "keygen":
        if os.path.exists(args.key):
            raise SystemExit(f"{args.key} exists")
        seed = os.urandom(32)
        fd = os.open(args.key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(seed.hex() + "\n")
        print(ed25519ref.keypair(seed).hex())
    elif args.cmd == "sign":
        seed = read_seed(args.key)
        mod, lines = load(args.bundle)
        check_card(args.bundle, mod)
        public = ed25519ref.keypair(seed)
        sig = ed25519ref.sign(seed, message(lines))
        with open(os.path.join(args.bundle, "mod.sig"), "w", newline="\n") as f:
            f.write(f"ed25519 {public.hex()} {sig.hex()}\n")
        print(f"{args.bundle}/mod.sig: {mod['listing']} serial {mod['serial']}, {len(lines)} signed lines, "
              f"key {public.hex()}")
    elif args.cmd == "verify":
        mod, lines = load(args.bundle)
        if not os.path.isfile(os.path.join(args.bundle, "mod.sig")):
            raise SystemExit("mod.sig is missing")
        text = read_bounded(os.path.join(args.bundle, "mod.sig"), MAX_SIG).decode("ascii", "replace")
        parts = text.rstrip("\r\n").split(" ")
        if len(parts) != 3 or parts[0] != "ed25519" or not re.fullmatch(r"[0-9a-f]{64}", parts[1]) or not re.fullmatch(r"[0-9a-f]{128}", parts[2]):
            raise SystemExit("mod.sig is malformed")
        if parts[1] != args.public:
            raise SystemExit(f"mod.sig is by {parts[1]}, not {args.public}")
        if not ed25519ref.verify(bytes.fromhex(parts[1]), message(lines), bytes.fromhex(parts[2])):
            raise SystemExit("mod.sig doesn't match the files")
        check_card(args.bundle, mod)
        print(f"{args.bundle}: {mod['listing']} serial {mod['serial']}, signature good ({len(lines)} lines)")
    elif args.cmd == "message":
        _, lines = load(args.bundle)
        sys.stdout.write(message(lines).decode())
    elif args.cmd == "vector":
        seed = hashlib.sha256(b"orca flat bundle signature test vector (not a real key)").digest()
        lines = [("PPLUS32.patches", hashlib.sha256(b"# none\n").hexdigest(), 7),
                 ("mod.json", hashlib.sha256(b"{}").hexdigest(), 2)]
        msg = message(lines)
        vector = {
            "comment": "sign-bundle.py vector: Python's Ed25519 (ed25519ref.py) against Orca's (Externals/ed25519)",
            "seed": seed.hex(),
            "public": ed25519ref.keypair(seed).hex(),
            "message": msg.decode(),
            "signature": ed25519ref.sign(seed, msg).hex(),
        }
        with open(args.out, "w") as f:
            json.dump(vector, f, indent=2)
            f.write("\n")
        print(f"{args.out}: public {vector['public']}")


if __name__ == "__main__":
    main()
