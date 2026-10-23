#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline Null-backend boot evidence; run under the machine's heavy/build locks.

Use --baseline for the parent, then the same --out with the instrumented binary.
Each run owns its user directory; no network, local input, audio or GPU is used.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess


def check_phases(text, launcher):
    lines = [line for line in text.splitlines() if line.startswith("orca boot phase ")]
    phases = []
    previous = 0
    for line in lines:
        match = re.fullmatch(r"orca boot phase ([a-z-]+) ([0-9]+) ([0-9]+)", line)
        assert match, line
        name, elapsed, delta = match.groups()
        elapsed, delta = int(elapsed), int(delta)
        assert name not in phases, line
        assert elapsed >= previous and delta == elapsed - previous, line
        phases.append(name)
        previous = elapsed
    required = ["main", "disc", "config", "emu-thread", "audio", "video", "shaders",
                "launcher", "first-frame"]
    assert all(name in phases for name in required), phases
    assert phases[0] == "main" and phases[-1] == "first-frame", phases
    assert "window" not in phases, phases
    for before, after in zip(required, required[1:]):
        assert phases.index(before) < phases.index(after), phases
    hashes = [name for name in phases if name.startswith("hash-")]
    assert len(hashes) == int(launcher), phases
    if launcher:
        assert hashes[0] in ("hash-cached", "hash-full"), phases
        assert phases.index("disc") < phases.index(hashes[0]) < phases.index("config"), phases
    print("\n".join(lines))


def hashes(path):
    return {int(parts[0]): parts[-1] for line in path.read_text().splitlines()
            if (parts := line.split()) and not line.startswith("#")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--disc", required=True, type=Path)
    parser.add_argument("--pplus", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--warm", action="store_true", help="reuse the instrumented run's caches")
    args = parser.parse_args()
    assert not (args.baseline and args.warm)
    root = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stage = "parent" if args.baseline else "timers-warm" if args.warm else "timers"
    for game, launcher in (("RSBE01", False), ("PPLUS32", True)):
        name = f"{stage}-{game}"
        user = out / f"{'parent' if args.baseline else 'timers'}-{game}-user"
        user.mkdir(exist_ok=True)
        temp = out / "tmp"
        temp.mkdir(exist_ok=True)
        env = dict(PATH="/usr/bin:/bin", HOME=str(out / "home"), TMPDIR=str(temp),
                   ORCA_NO_LOCAL_INPUT="1", SDL_JOYSTICK_HIDAPI="0",
                   SDL_JOYSTICK_HIDAPI_GAMECUBE="0", SDL_HIDAPI_LIBUSB="0",
                   SDL_JOYSTICK_IOKIT="0", SDL_JOYSTICK_MFI="0",
                   SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT="0x0000/0x0000",
                   ORCA_SESSION="1", YG_SCENES="1", YG_EXIT_AFTER="600",
                   YG_HASHLOG=str(out / f"{name}.hashlog"),
                   YG_INPUT=str(root / "Tools/orca/inputs" /
                                ("pplus-1v1.txt" if launcher else "bf-mario-link-results.txt")))
        if launcher:
            env["ORCA_PROFILE"] = "PPLUS32"
        cmd = [str(args.engine.resolve()), "-p", "headless", "-u", str(user), "-v", "Null",
               "-C", "Dolphin.DSP.Backend=No Audio Output", "-C", f"Logger.Logs.BOOT={not args.warm}",
               "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Options.Verbosity=2",
               "-C", "Logger.Options.WriteToFile=True"]
        if launcher:
            cmd += ["-C", f"Dolphin.Core.DefaultISO={args.disc.resolve()}"]
        cmd += ["-e", str((args.pplus if launcher else args.disc).resolve())]
        log = out / f"{name}.log"
        with log.open("w") as output:
            result = subprocess.run(cmd, env=env, stdout=output, stderr=subprocess.STDOUT,
                                    timeout=300)
        assert result.returncode == 0, f"{name}: exit {result.returncode}, see {log}"
        text = log.read_text()
        assert "Harness done at frame 600" in text, f"{name}: did not finish, see {log}"
        current = hashes(out / f"{name}.hashlog")
        assert set(range(601)) <= current.keys(), f"{name}: missing frames 0-600"
        print(f"{name}: exit 0, frames 0-600 present")
        if not args.baseline:
            check_phases(text, launcher)
            if launcher:
                expected = "hash-cached" if args.warm else "hash-full"
                assert f"orca boot phase {expected} " in text, f"{name}: expected {expected}"
            dolphin_log = (user / "Logs/dolphin.log").read_text()
            lines = [line for line in text.splitlines() if line.startswith("orca boot phase ")]
            assert all(line in dolphin_log for line in lines), f"{name}: missing NOTICE records"
            (out / f"{name}.phases.txt").write_text("\n".join(lines) + "\n")
            parent = hashes(out / f"parent-{game}.hashlog")
            differences = [frame for frame in range(601) if current[frame] != parent[frame]]
            assert not differences, f"{game}: changed hashes at {differences[:10]}"
            print(f"{game}: 601 parent frame hashes compared, 0 differences")


if __name__ == "__main__":
    main()
