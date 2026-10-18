#!/usr/bin/env python3
# free-space.py: proves the match block and the code caves of Core/Orca/Games/Brawl/FreeSpace.h free in Brawl
# or Project+ (ORCA.md "Free space"), headless and muted, one Orca at a time.
#
#   Tools/orca/free-space.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>] [--keep]
#
# Two runs of one scripted loop through every scene Orca's online play visits: boot, PLAY ONLINE >
# With Anyone > Casual, B out of the character select (Orca's back out), Ranked, two whole matches
# with their results, B out, the top page, Group > Brawl, B out.
#   observe  watches every word of the block and every 64th byte of the caves at every frame
#            boundary, and dumps MEM1 at 16 points and MEM2 at 5 (6 in Project+);
#   trap     the same, after filling the block and every cave with illegal instructions (primary
#            opcode 0, each word different) 5 frames into the first scene, once sora_scene is
#            loaded; with a drop-in keyframe test (YG_KEYFRAME_TEST) in the first match.
# PASS needs:
#   - observe: the block and the caves hold the same bytes in every dump, and no watch changes
#     after the module's load;
#   - trap: the fill intact in every dump, no watch change after it, no unknown instruction ever
#     executed, the keyframe test's two PASS lines;
#   - both: the same scenes at the same frames, and every dump the same outside the block and the
#     caves (nothing reads them: the game ran the same).
# The dumps derive from the disc: they are deleted unless --keep. Runs nice'd, muted, with a
# timeout; prints PASS or each failure.
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

if len(sys.argv) < 3:
    sys.exit("usage: free-space.py <nogui> <disc> [--pplus <dol>] [--keep]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
KEEP = "--keep" in sys.argv
GAME = "pplus" if PPLUS else "brawl"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = tempfile.mkdtemp(prefix="orca-free-space-")
T0 = time.time()
MEM1 = 0x80000000


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


# ---- The ranges, read from the header so the tool and the code never disagree ----

header = open(os.path.join(ROOT, "Source/Core/Core/Orca/Games/Brawl/FreeSpace.h")).read()


def ranges(name):
    body = re.search(name + r"\{\{(.*?)\}\};", header, re.S).group(1)
    return [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\{(0x[0-9A-F]+), (0x[0-9A-F]+)\}", body)]


BLOCK = tuple(int(x, 16) for x in re.search(
    r"kMatchBlock\{(0x[0-9A-F]+), (0x[0-9A-F]+)\}", header).groups())
CAVES = ranges("kCodeCaves")
FREE = [BLOCK] + CAVES


def sentinel(address):
    # An illegal instruction (primary opcode 0) that is never zero: 0x00A5xxxx in the block,
    # 0x00C0xxxx in the caves, xxxx the word's index from 0x806F0000.
    tag = 0x00A50000 if BLOCK[0] <= address < BLOCK[1] else 0x00C00000
    return tag | (((address - 0x806F0000) >> 2) & 0xFFFF)


# ---- The loop ----

if PPLUS:
    # Project+ boots to its main menu's top page, as Brawl (PPLUS32.patches).
    CSS = ["scSelctCharacter:2", "scSelctCharacter:3", "scSelctCharacter:4", "scSelctCharacter:5"]
    BOOT = []
    FIRST = 120
    PICK = [(60, 1, "A"), (70, 2, "A"), (140, 1, "A"), (150, 2, "A"), (200, 1, "START")]
    UP_FRAMES = (90, 116)
    SSS1 = ["@scSelStage:1 40 44 1 SY=255", "@scSelStage:1 60 63 1 A"]
    FUZZ_FROM = 300
    EXIT_AFTER = 13000
    KEYFRAME_AT = 4000
    CSS_POINTS = ["scSelctCharacter:1 100 css-casual mem2"]
else:
    CSS = ["scSelctCharacter:2", "scSelctCharacter:3", "scSelctCharacter:4", "scSelctCharacter:5"]
    BOOT = ["@scStrap mash 30 99999 1 40 A", "@scBoot mash 30 99999 1 40 A",
            "@scTitle mash 30 99999 1 40 A"]
    FIRST = 120
    PICK = [(53, 1, "A"), (83, 2, "A"), (173, 1, "A"), (193, 2, "A"), (233, 1, "START")]
    UP_FRAMES = (113, 153)
    # Up to the top row, one step left onto Battlefield (1, not 0: 0 reads as neutral), A.
    SSS1 = ["@scSelStage:1 35 73 1 SY=255", "@scSelStage:1 75 78 1 SX=1", "@scSelStage:1 95 98 1 A"]
    FUZZ_FROM = 400
    EXIT_AFTER = 22000
    KEYFRAME_AT = 5000
    CSS_POINTS = ["scSelctCharacter:1 100 css-casual"]
CASUAL_CSS = "scSelctCharacter:1"
RANKED_CSS, NEXT_CSS, OUT_CSS, VERSUS_CSS = CSS


def input_script():
    lines = [f"# free-space.py {GAME}"] + BOOT
    frame = FIRST
    for button in ["DOWN", "A", "RIGHT", "A", "A"]:  # PLAY ONLINE > With Anyone > Casual
        lines.append(f"@muMenuMain:1 {frame} {frame + 3} 1 {button}")
        frame += 50
    lines.append(f"@{CASUAL_CSS} 150 260 1 B")
    lines += ["@muMenuMain:2 200 203 1 DOWN", "@muMenuMain:2 250 253 1 A"]  # Ranked
    for at, port, button in PICK:
        lines.append(f"@{RANKED_CSS} {at} {at + 3} {port} {button}")
    for port in (1, 2):
        lines.append(f"@{RANKED_CSS} {UP_FRAMES[0]} {UP_FRAMES[1]} {port} SY=255")
    lines += SSS1
    lines += [f"@scMelee fuzz {FUZZ_FROM} 99999 1 11", f"@scMelee fuzz {FUZZ_FROM} 99999 2 22",
              "@scVsResult mash 240 99999 1 40 A", "@scVsResult mash 260 99999 2 40 A",
              "@scVsResult mash 600 99999 1 40 START", "@scVsResult mash 620 99999 2 40 START"]
    lines += [f"@{NEXT_CSS} 100 103 1 START", "@scSelStage:2 60 63 1 A"]
    lines.append(f"@{OUT_CSS} 150 260 1 B")
    frame = 200
    for button in ["B", "B", "UP", "A", "A"]:  # the ONLINE page, the top page, Group > Brawl
        lines.append(f"@muMenuMain:3 {frame} {frame + 3} 1 {button}")
        frame += 50
    lines.append(f"@{VERSUS_CSS} 150 260 1 B")
    return "\n".join(lines) + "\n"


POINTS = (["muMenuMain:1 100 menu mem2"] + CSS_POINTS + [
    "muMenuMain:2 150 menu-back", f"{RANKED_CSS} 100 css-ranked mem2", "scSelStage:1 30 sss",
    "scMelee:1 1000 match-early mem2", "scMelee:1 4000 match-late", "scVsResult:1 100 results mem2",
    f"{NEXT_CSS} 50 css-next", "scSelStage:2 30 sss-next", "scMelee:2 1500 match-next",
    "scVsResult:2 100 results-next", f"{OUT_CSS} 100 css-out", "muMenuMain:3 150 menu-out",
    f"{VERSUS_CSS} 100 css-versus", "muMenuMain:4 150 menu-versus mem2"])
FILL_AT = "scStrap 5"


def probe_script(trap):
    lines = [f"@* 0 watch {a:08x} b{a:08x}" for a in range(BLOCK[0], BLOCK[1], 4)]
    for lo, hi in CAVES:
        lines += [f"@* 0 watch {a:08x} c{a:08x}" for a in list(range(lo, hi, 0x40)) + [hi - 4]]
    if trap:
        for lo, hi in FREE:
            words = "".join(f"{sentinel(a):08x}" for a in range(lo, hi, 4))
            lines.append(f"@{FILL_AT} write {lo:08x} {words}")
    for point in POINTS:
        scene, frame, name, *mem2 = point.split()
        lines.append(f"@{scene} {frame} dump 80000000 1800000 {name}-mem1")
        if mem2:
            lines.append(f"@{scene} {frame} dump 90000000 4000000 {name}-mem2")
    return "\n".join(lines) + "\n"


def run(mode):
    trap = mode == "trap"
    user = os.path.join(WORK, f"user-{mode}")
    os.makedirs(user)
    for name, text in (("input.txt", input_script()), ("probe.txt", probe_script(trap))):
        with open(os.path.join(WORK, f"{mode}-{name}"), "w") as f:
            f.write(text)
    env = {
        "HOME": os.environ["HOME"],
        "PATH": os.environ["PATH"],
        "ORCA_SESSION": "1",
        "YG_SCENES": "1",
        "YG_INPUT": os.path.join(WORK, f"{mode}-input.txt"),
        "ORCA_UX_PROBE": os.path.join(WORK, f"{mode}-probe.txt"),
        "YG_EXIT_AFTER": str(EXIT_AFTER),
    }
    if trap:
        env["YG_KEYFRAME_TEST"] = str(KEYFRAME_AT)
    if PPLUS:
        env["ORCA_PROFILE"] = "PPLUS32"
    args = [BIN, "-p", "headless", "-u", user, "-v", "Null",
            "-C", "Dolphin.DSP.Backend=No Audio Output", "-C", "Dolphin.DSP.Volume=0",
            "-C", "Logger.Options.Verbosity=4", "-C", "Logger.Options.WriteToConsole=True",
            "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.POWERPC=True"]
    args += (["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS] if PPLUS else ["-e", DISC])
    log_path = os.path.join(WORK, f"{mode}.log")
    with open(log_path, "w") as out:
        try:
            code = subprocess.run(["nice", "-n", "10"] + args, env=env, stdout=out,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  timeout=1200).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    log(f"{mode}: exit {code}")
    return user, open(log_path, errors="replace").read()


def scenes(text):
    return re.findall(r"Scene frame (\d+): (\w*) -> (\w+)", text)


def watches(text):
    # name -> [(frame, value)] in log order
    out = {}
    for frame, name, value in re.findall(r"Watch frame (\d+) \([^)]*\): (\S+) = ([0-9a-f]{8})", text):
        out.setdefault(name, []).append((int(frame), int(value, 16)))
    return out


def main():
    failures = []
    observe_user, observe_log = run("observe")
    trap_user, trap_log = run("trap")
    names = [p.split()[2] for p in POINTS]

    def dump(user, name, mem):
        path = os.path.join(user, f"probe-{name}-{mem}.bin")
        return open(path, "rb").read() if os.path.exists(path) else None

    # The same game in both runs.
    if scenes(observe_log) != scenes(trap_log):
        failures.append("the scene timelines differ")
    want = len(CSS) + (2 if PPLUS else 1)
    if sum(1 for s in scenes(observe_log) if s[2] == "scSelctCharacter") < want:
        failures.append("the loop did not reach every character select")
    for log_text, mode in ((observe_log, "observe"), (trap_log, "trap")):
        if "Unknown instruction" in log_text:
            failures.append(f"{mode}: an unknown instruction ran")
        missing = [n for n in names if f"{n}-mem1" not in log_text]
        if missing:
            failures.append(f"{mode}: no dump at {', '.join(missing)}")
    if not (re.search(r"Keyframe test: loaded .* PASS", trap_log) and
            re.search(r"Keyframe test re-run: .*\(PASS\)", trap_log)):
        failures.append("trap: the keyframe test did not pass")

    # Watches: the module's load, then nothing (observe); the fill, then nothing (trap). Each
    # watch logs its first read (frame 1) and every change after it.
    if "Probe scStrap 5: write" not in trap_log:
        failures.append("trap: the fill never ran")
    for mode, text in (("observe", observe_log), ("trap", trap_log)):
        for name, events in watches(text).items():
            address = int(name[1:], 16)
            if mode == "observe":
                ok = len(events) <= 2 and events[-1][0] <= 200
            else:
                ok = events[-1][1] == sentinel(address) and events[-1][0] <= 200
            if not ok:
                failures.append(f"{mode}: {name} changed at frame {events[-1][0]}")
                break

    # Dumps.
    first = dump(observe_user, names[0], "mem1")
    checked = 0
    for name in names:
        a, b = dump(observe_user, name, "mem1"), dump(trap_user, name, "mem1")
        if a is None or b is None:
            continue
        checked += 1
        for lo, hi in FREE:
            o, e = lo - MEM1, hi - MEM1
            if first and a[o:e] != first[o:e]:
                failures.append(f"observe {name}: {lo:08x}-{hi:08x} changed")
            want_fill = b"".join(sentinel(x).to_bytes(4, "big") for x in range(lo, hi, 4))
            if b[o:e] != want_fill:
                failures.append(f"trap {name}: the fill at {lo:08x}-{hi:08x} changed")
        # Outside the free space the two runs hold the same RAM.
        cuts = sorted((lo - MEM1, hi - MEM1) for lo, hi in FREE)
        start = 0
        for o, e in cuts + [(len(a), len(a))]:
            if a[start:o] != b[start:o]:
                diff = next(i for i in range(start, o) if a[i] != b[i])
                failures.append(f"{name}: MEM1 differs outside the free space at {diff + MEM1:08x}")
                break
            start = e
        a2, b2 = dump(observe_user, name, "mem2"), dump(trap_user, name, "mem2")
        if a2 is not None and a2 != b2:
            failures.append(f"{name}: MEM2 differs")
    if checked < len(names):
        failures.append(f"only {checked} of {len(names)} points dumped in both runs")
    log(f"{checked} points compared, {len(FREE)} ranges ({sum(h - l for l, h in FREE)} bytes)")
    if not KEEP:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        log(f"kept {WORK} (disc-derived dumps: delete it when done)")
    if failures:
        for f in failures:
            print(f"FAIL {GAME}: {f}")
        sys.exit(1)
    print(f"PASS {GAME}")


main()
