#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check host input sources and train additions without starting a native input backend."""

from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
KEY_STATE = re.compile(
    r"\b(CGEventSourceKeyState|CGEventSourceFlagsState|GetAsyncKeyState|GetKeyState|"
    r"GetKeyboardState|XQueryKeymap)\s*\("
)


def main():
    failures = []
    # Existing device backends are intentionally outside this scan. Native host windows must
    # use their own delivered events or the focused app's stdin commands on every platform.
    for source in sorted((ROOT / "Source/Core/DolphinNoGUI").glob("Platform*")):
        if source.suffix not in (".cpp", ".h", ".mm"):
            continue
        for number, line in enumerate(source.read_text().splitlines(), 1):
            if KEY_STATE.search(line):
                failures.append(f"{source.relative_to(ROOT)}:{number}")

    # Also reject additions anywhere else in this train, including device backends.
    diff = subprocess.check_output(
        ["git", "diff", "--unified=0", "ce0a4d906a", "--", "Source"], cwd=ROOT, text=True
    )
    source = ""
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            source = line[6:]
        elif line.startswith("+") and KEY_STATE.search(line[1:]):
            failures.append(f"train addition in {source}")
    if failures:
        raise SystemExit("Host input check failed: " + ", ".join(failures))
    print("PASS: host input sources and train additions")


if __name__ == "__main__":
    main()
