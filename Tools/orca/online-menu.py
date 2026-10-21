#!/usr/bin/env python3
# online-menu.py: the game's own Online menu (ORCA.md "Online menu"), headless and muted, one Orca
# at a time, every way through it in Brawl or Project+.
#
#   Tools/orca/online-menu.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>] [--video Metal]
#                             [--only <scenario>[,<scenario>...]] [--shots <dir>] [--scale <n>]
#                             [--keep] [--sandbox]
#
# Scenarios (each a boot of its own, in a fresh user folder):
#   tour       PLAY ONLINE, the ONLINE page, B back to the top page and in again, every direction
#              on it, With Anyone (both buttons), B back, in again and Casual
#   casual     With Anyone > Casual
#   ranked     With Anyone > Ranked
#   friends    With Friends: straight to the character select (Orca skips the With Friends page)
#   back-friends  With Friends, B out of the character select: the menu comes back on the ONLINE
#              page with With Friends selected, and A there picks it again
#   back-casual, back-ranked  the same from With Anyone: back on With Anyone, Casual (Ranked)
#              selected, A picks it again
#   back-chain With Friends, B out: then B to the top page, in again, With Anyone > Casual (the
#              pages work after a menu that started on one of them)
#   back-versus  Group (VERSUS) > Brawl, B out: back on Group with Brawl selected, A goes to the
#              character select again, nothing printed
#
# Each run must show:
#   - stdout: exactly the scenario's "orca menu ..." lines, and nothing for page moves or B-backs;
#     the log's menu exit codes prove a back scenario's second pick landed on the same button;
#   - scenes: the main menu, then the local Versus character select, never an online scene;
#   - the log: every Orca patch group landed where it should (once each in sora_scene, the main
#     menu's online groups only on the main menu), the menu text was rewritten, and after boot the
#     game touched no network device, no save/WiiConnect24/network-settings NAND folder, and made no
#     invalid memory access.
# --shots <dir>: with a real video backend (--video Metal), screenshot every page the scenario
# visits. They show the disc's art: keep them local.
# --scale <n>: the internal resolution (default 1x; Orca's labels are drawn at 4x).
# --sandbox (macOS): run Orca under sandbox-exec with outbound and inbound IP denied.
# Runs nice'd, muted, with a timeout; prints PASS or the first failure per scenario.
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

if len(sys.argv) < 3:
    sys.exit("usage: online-menu.py <nogui> <disc> [--pplus <dol>] [--video Metal] [--only s,..] "
             "[--shots <dir>] [--scale <n>] [--keep] [--sandbox]")
BIN, DISC = sys.argv[1], sys.argv[2]


def arg_value(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


PPLUS = arg_value("--pplus")
VIDEO = arg_value("--video", "Null")
SHOTS = arg_value("--shots")
SCALE = arg_value("--scale")
ONLY = arg_value("--only")
KEEP = "--keep" in sys.argv
SANDBOX = "--sandbox" in sys.argv
GAME = "pplus" if PPLUS else "brawl"
WORK = tempfile.mkdtemp(prefix="orca-online-menu-")
T0 = time.time()

# Frames between two presses on the menu: enough for every page's animation to settle.
STEP = 50
# Booting to the main menu's top page, and the first press there (frames of the main menu scene).
# Menu presses are for the menu's first visit (`@muMenuMain:1`), a back scenario's for its second.
if PPLUS:
    # Orca boots Project+ to its main menu's top page, as Brawl (PPLUS32.patches: its codeset's
    # "Boot Directly to CSS" takes the title's path instead).
    BOOT = []
    FIRST = 120
    CSS_VISIT = 1
    EXIT_FRAMES = 700  # after the last press: out of the menu to character select
    PPLUS_MENU = 600  # the menu scene starts at frame 197; room to spare
else:
    BOOT = ["@scStrap mash 30 99999 1 40 A", "@scBoot mash 30 99999 1 40 A",
            "@scTitle mash 30 99999 1 40 A"]
    FIRST = 120
    CSS_VISIT = 1
    EXIT_FRAMES = 700
# A back scenario holds B on its character select from this frame (of that scene) until it leaves,
# then presses on the menu's next visit from BACK_FIRST, which leaves the page time to come in.
CSS_B = (150, 260)
BACK_FIRST = 200
# From the menu's first press to the character select and out of it again, at most.
CSS_FRAMES = 600

# The top page's cursor starts on Group / VERSUS; Play Online is below it. On the ONLINE page the
# cursor starts on With Friends; With Anyone is to its right. With Anyone's cursor starts on Casual,
# Ranked is below it. A on With Friends leaves the menu at once (RSBE01.patches, PPLUS32.patches:
# muProcWifi).
TO_ONLINE = [("DOWN", "top-play-online"), ("A", "online")]
# The menu's exit codes for With Friends, Casual and Ranked.
FRIENDS_CODE, CASUAL_CODE, RANKED_CODE = 25, 30, 31
FRIENDS = TO_ONLINE + [("A", None)]
CASUAL = TO_ONLINE + [("RIGHT", None), ("A", "with-anyone"), ("A", None)]
RANKED = TO_ONLINE + [("RIGHT", None), ("A", "with-anyone"), ("DOWN", "ranked-selected"), ("A", None)]


# name: (menu presses, stdout lines, exit codes, presses on the menu's next visit after B out of
# the character select or None, character selects reached)
SCENARIOS = {
    "tour": (
        TO_ONLINE + [
            ("B", "online-b-top"),
            ("A", "online-again"),
            ("RIGHT", "online-with-anyone"),
            ("LEFT", "online-with-friends"),
            ("UP", "online-up"),
            ("DOWN", "online-down"),
            ("RIGHT", "online-with-anyone-2"),
            ("A", "with-anyone"),
            ("DOWN", "with-anyone-ranked"),
            ("UP", "with-anyone-casual"),
            ("B", "with-anyone-b-online"),
            ("A", "with-anyone-again"),
            ("A", None),
        ],
        ["online casual local"], [CASUAL_CODE], None, 1),
    "casual": (CASUAL, ["online casual local"], [CASUAL_CODE], None, 1),
    "ranked": (RANKED, ["online ranked local"], [RANKED_CODE], None, 1),
}
SCENARIOS["friends"] = (FRIENDS, ["online friends"], [FRIENDS_CODE], None, 1)
SCENARIOS["back-friends"] = (FRIENDS, ["online friends"] * 2, [FRIENDS_CODE] * 2,
                             [("A", "back-online-with-friends")], 2)
SCENARIOS["back-casual"] = (CASUAL, ["online casual local"] * 2, [CASUAL_CODE] * 2,
                            [("A", "back-with-anyone")], 2)
SCENARIOS["back-ranked"] = (RANKED, ["online ranked local"] * 2, [RANKED_CODE] * 2,
                            [("A", "back-with-anyone-ranked")], 2)
SCENARIOS["back-chain"] = (
    FRIENDS, ["online friends", "online casual local"], [FRIENDS_CODE, CASUAL_CODE],
    [("B", "back-online"), ("A", "back-top"), ("RIGHT", "back-online-again"),
     ("A", "back-online-with-anyone"), ("A", "back-with-anyone")],
    2)
SCENARIOS["back-versus"] = ([("A", "top"), ("A", "group")], [], [], [("A", "back-group")], 2)
# The back scenarios whose page comes back on a button with a rewritten description (the ONLINE
# page's With Friends, Casual, Ranked; MenuText.h): the menu formats it in the frame its archive
# loads, so the description box's copy must be rewritten as well, at that boundary.
BOX_REWRITE = {"back-friends", "back-chain", "back-casual", "back-ranked"}

# The main menu module's online groups (RSBE01.patches; Project+'s module sits 0x1480 higher): each
# must land on the main menu (or the scene change into it, while the module loads) and nowhere else.
MENU_GROUPS = [a + (0x1480 if PPLUS else 0) for a in
               (0x81178A58, 0x81178C30, 0x81179F04, 0x8117A104, 0x81179A04, 0x81191D7C)]
# Orca's groups in sora_scene (resident, the same in both games): sqMenuMain::setNext's table, its
# block for the online codes and the back out's helper, sqVsMelee's back out. Each lands once.
SCENE_GROUPS = [0x80701A04, 0x806DC8C8, 0x806DCE34]

# NAND folders nothing on these pages may touch: the game's save (Brawl's title, which Project+
# keeps), WiiConnect24's, and the console's network settings.
GUARDED_NAND = ("/title/00010000/52534245", "/shared2/wc24", "/shared2/sys/net", "/shared2/DWC")


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def write_input(name, steps, back):
    lines = [f"# online-menu.py {GAME} {name}"] + BOOT
    frame = FIRST
    for button, _ in steps:
        lines.append(f"@muMenuMain:1 {frame} {frame + 3} 1 {button}")
        frame += STEP
    if back is not None:
        lines.append(f"@scSelctCharacter:{CSS_VISIT} {CSS_B[0]} {CSS_B[1]} 1 B")
        frame += CSS_FRAMES
        back_frame = BACK_FIRST
        for button, _ in back:
            lines.append(f"@muMenuMain:2 {back_frame} {back_frame + 3} 1 {button}")
            back_frame += STEP
        frame += back_frame
    path = os.path.join(WORK, f"{name}.txt")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path, frame


def run(name, steps, expected, codes, back, css_count):
    input_path, last = write_input(name, steps, back)
    user = os.path.join(WORK, f"user-{name}")
    os.makedirs(user)
    # The menu scene starts at about frame 1430 in Brawl and 197 in Project+.
    exit_after = (1430 if not PPLUS else PPLUS_MENU) + last + EXIT_FRAMES
    env = {
        "HOME": os.environ["HOME"],
        "PATH": os.environ["PATH"],
        "ORCA_SESSION": "1",
        "YG_SCENES": "1",
        "YG_INPUT": input_path,
        "YG_EXIT_AFTER": str(exit_after),
    }
    if PPLUS:
        env["ORCA_PROFILE"] = "PPLUS32"
    if SHOTS and VIDEO != "Null":
        env["YG_SHOT_EVERY"] = "5"
        env["YG_SHOT_FROM"] = "250" if PPLUS else "1000"
    args = [BIN, "-p", "headless", "-u", user, "-v", VIDEO,
            "-C", "Dolphin.DSP.Backend=No Audio Output",
            "-C", "Logger.Options.Verbosity=4", "-C", "Logger.Options.WriteToConsole=True"]
    for t in ("ROLLBACK", "IOS", "IOS_FS", "IOS_NET", "IOS_SSL", "IOS_WC24"):
        args += ["-C", f"Logger.Logs.{t}=True"]
    args += ["-C", "Logger.Logs.OSREPORT_HLE=False"]
    if SCALE:
        args += ["-C", f"Graphics.Settings.InternalResolution={int(SCALE)}"]
    args += (["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS] if PPLUS else ["-e", DISC])
    prefix = ["nice", "-n", "10"]
    if SANDBOX:
        prefix += ["sandbox-exec", "-p", "(version 1)(allow default)"
                   "(deny network-outbound (remote ip))(deny network-inbound (local ip))"]
    out_path = os.path.join(WORK, f"{name}.out")
    log_path = os.path.join(WORK, f"{name}.log")
    with open(out_path, "w") as out, open(log_path, "w") as err:
        try:
            code = subprocess.run(prefix + args, env=env, stdout=out, stderr=err,
                                  stdin=subprocess.DEVNULL, timeout=600).returncode
        except subprocess.TimeoutExpired:
            return "timed out"
    with open(out_path, errors="replace") as f:
        out_lines = f.read().splitlines()
    with open(log_path, errors="replace") as f:
        log_lines = f.read().splitlines()
    # Orca logs to stderr on macOS but to stdout on Windows (ConsoleListenerWin, redirected): the
    # landing checks read both. Event lines ("orca menu ...") match none of the log patterns.
    log_lines = out_lines + log_lines
    every = log_lines
    if code != 0:
        return f"exit code {code}"

    menu = [l[len("orca menu "):] for l in out_lines if l.startswith("orca menu ")]
    if menu != expected:
        return f"stdout menu lines {menu}, expected {expected}"
    exits = [int(m.group(1)) for l in log_lines if (m := re.search(r"Online menu: exit code (\d+):", l))]
    if exits != codes:
        return f"menu exit codes {exits}, expected {codes}"

    scenes = [m.group(1, 2) for l in every
              if (m := re.search(r"Scene frame (\d+): \S* -> (\S+)", l))]
    names = [s for _, s in scenes]
    if "muMenuMain" not in names:
        return f"never reached the main menu: {names}"
    after = names[names.index("muMenuMain"):]
    if "scSelctCharacter" not in after:
        return f"no character select after the menu: {names}"
    if after.count("scSelctCharacter") != css_count:
        return f"{after.count('scSelctCharacter')} character selects after the menu, expected {css_count}"
    if back is not None and after.count("muMenuMain") < 2:
        return f"B out of the character select never reached the menu: {after}"
    if back is not None:
        # The menu's second visit, up to the first press there: the description box's copy.
        second = [i for i, l in enumerate(log_lines) if "-> muMenuMain" in l][1]
        box = sum(int(m.group(1)) for l in log_lines[second:]
                  if (m := re.search(r"rewritten, (\d+) of them the description box", l)))
        if box != (1 if name in BOX_REWRITE else 0):
            return f"the description box's copy rewritten {box} times after B, expected " \
                   f"{1 if name in BOX_REWRITE else 0}"
    online = [s for s in after if re.search(r"Net|Wifi|Wi-Fi|Online", s, re.I)]
    if online:
        return f"an online scene: {online}"
    if not any("main menu text(s) rewritten" in l for l in every):
        return "the menu's text was never rewritten"
    # Where each online group landed: the scene before its line and the scene after.
    events = [(m.group(1), None) if (m := re.search(r"Scene frame \d+: \S* -> (\S+)", l)) else
              (None, int(g.group(1), 16)) if (g := re.search(r"patch group ([0-9a-f]{8}) ", l)) else
              None for l in log_lines]
    events = [e for e in events if e]
    landed = set()
    for i, (scene, group) in enumerate(events):
        if group not in MENU_GROUPS:
            continue
        landed.add(group)
        prev = next((s for s, _ in reversed(events[:i]) if s), None)
        following = next((s for s, _ in events[i + 1:] if s), None)
        if not (prev == "muMenuMain" or (prev == "scMemoryChange" and following == "muMenuMain")):
            return f"group {group:08x} landed on {prev} (next {following})"
    if landed != set(MENU_GROUPS):
        return f"groups never landed: {[f'{g:08x}' for g in set(MENU_GROUPS) - landed]}"
    scene_landed = [g for _, g in events if g in SCENE_GROUPS]
    if sorted(scene_landed) != sorted(SCENE_GROUPS):
        return f"sora_scene groups landed {[f'{g:08x}' for g in scene_landed]}, expected each once"

    # The boot's own device opens are fine; from the first main menu frame on, nothing.
    start = next(i for i, l in enumerate(log_lines) if "-> muMenuMain" in l)
    bad = []
    fs_commands = 0
    for l in log_lines[start:]:
        if "[IOS]: Opening /dev/net" in l or any(f"[{t}]" in l for t in
                                                 ("IOS_NET", "IOS_SSL", "IOS_WC24")):
            bad.append(l)
        if "[IOS_FS]" in l and "Command:" in l:
            fs_commands += 1
            if any(p in l for p in GUARDED_NAND):
                bad.append(l)
        if re.search(r"invalid (read|write)", l, re.I):
            bad.append(l)
    if bad:
        return f"{len(bad)} forbidden log lines after the boot, first: {bad[0]}"

    if SHOTS and VIDEO != "Null":
        suffix = f"-{int(SCALE)}x" if SCALE else ""
        menu_starts = [int(f) for f, s in scenes if s == "muMenuMain"]
        menu_start = menu_starts[0]
        shots = sorted(glob.glob(os.path.join(user, "ScreenShots", "**", "f*.png"), recursive=True))
        by_frame = {int(re.search(r"f(\d+)", os.path.basename(p)).group(1)): p for p in shots}
        os.makedirs(SHOTS, exist_ok=True)
        frame = FIRST
        for button, label in steps:
            frame += STEP
            if label:
                # Just before the next press (or the exit), the page has settled.
                want = menu_start + frame - 2
                have = [f for f in by_frame if f <= want]
                if have:
                    shutil.copy(by_frame[max(have)],
                                os.path.join(SHOTS, f"{GAME}-{name}-{label}{suffix}.png"))
        css = [int(f) for f, s in scenes if s == "scSelctCharacter" and int(f) > menu_start]
        if css:
            want = css[0] + 120
            have = [f for f in by_frame if f <= want]
            if have:
                shutil.copy(by_frame[max(have)],
                            os.path.join(SHOTS, f"{GAME}-{name}-css{suffix}.png"))
        if back is not None and len(menu_starts) > 1:
            frame = BACK_FIRST - STEP
            for button, label in back:
                frame += STEP
                if label:
                    want = menu_starts[1] + frame - 2
                    have = [f for f in by_frame if f <= want]
                    if have:
                        shutil.copy(by_frame[max(have)],
                                    os.path.join(SHOTS, f"{GAME}-{name}-{label}{suffix}.png"))
        shutil.rmtree(os.path.join(user, "ScreenShots"), ignore_errors=True)
    log(f"{name}: menu lines {menu}, exit codes {exits}; scenes after the menu {after}; "
        f"{fs_commands} NAND commands after the boot, none on the save, WiiConnect24 or network "
        f"folders")
    return None


failed = 0
for name, (steps, expected, codes, back, css_count) in SCENARIOS.items():
    if ONLY and name not in ONLY.split(","):
        continue
    log(f"{GAME} {name} ...")
    why = run(name, steps, expected, codes, back, css_count)
    if why:
        failed += 1
        log(f"FAIL {GAME} {name}: {why}")
    else:
        log(f"PASS {GAME} {name}")
if KEEP or failed:
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
log("PASS" if not failed else f"FAIL ({failed})")
sys.exit(1 if failed else 0)
