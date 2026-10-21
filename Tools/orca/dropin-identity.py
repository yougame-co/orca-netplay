#!/usr/bin/env python3
# dropin-identity.py: each player's own name-tag controls in a friend's game (UX/NameTags.h, the
# session's value channel in Rollback/OnlineMatch.cpp "Ports"). Two Orcas over a real YouGame room
# (dev tickets, a local keyframe folder), headless and muted.
#
#   Tools/orca/dropin-identity.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#
# 1. Host and Friend each boot their own game, solo in a room of their own, and join port 1 of
#    their character select (each gets its YouGame tag there). Each then sets its own tag's
#    controls (a probe write, as the game's controls menu would): Host A as jump, B as grab, tap
#    jump off (GameCube and Classic); Friend L and R as grab, Y and X as nothing, rumble on.
# 2. Friend sends "join <host's room>" from its character select, as a queue match's joiner does:
#    its hello carries its controls, the host puts them in port 2, and the friend loads the host's
#    game, catches up and plugs in.
# 3. Both pick and start a match. Both games' tags must hold each player's own controls (the whole
#    layout and rumble), and as the match starts the game copies each port's tag layout for its
#    controller (a GameCube controller's 12 bytes) into its per-port table (0x805B7480): both Orcas
#    must show port 1 with Host's and port 2 with Friend's. No session may report a desync, and the
#    frames both ran from the host's keyframe on must hash alike (YG_HASHLOG; only a frame first
#    run on a guessed input may differ there, before its re-run). Runs two Orca processes.
import os
import queue
import random
import shutil
import string
import subprocess
import sys
import tempfile
import threading
import time

if len(sys.argv) < 3:
    sys.exit("usage: dropin-identity.py <nogui> <disc> [--pplus <dol>]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "pplus-identity.txt" if PPLUS else "bf-identity.txt")
ROOM = "idn" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-identity-")
T0 = time.time()

# Brawl rev 2's default tag controls (0x80406938): GameCube 12, Wii Remote 8, Nunchuk 12, Classic 13.
DEFAULTS = bytes.fromhex("0303040a0b0c00010502028001000509030402040a0c0b00010403020204034003030404"
                         "0a0b0c000105020280")
assert len(DEFAULTS) == 0x2D
HOST_LAYOUT = bytearray(DEFAULTS)
HOST_LAYOUT[6], HOST_LAYOUT[7], HOST_LAYOUT[11], HOST_LAYOUT[0x2C] = 0x02, 0x04, 0x00, 0x00
FRIEND_LAYOUT = bytearray(DEFAULTS)
FRIEND_LAYOUT[0], FRIEND_LAYOUT[1], FRIEND_LAYOUT[9], FRIEND_LAYOUT[10] = 0x04, 0x04, 0x0E, 0x0E
HOST_RUMBLE, FRIEND_RUMBLE = 0, 1
# Each own game's first tag is the lowest slot (an empty save), as the game makes them: 0,
# records + 0xE0 + 0 * 0x124.
TAG = 0xE0 + 0 * 0x124
# Just before P1 starts (the input file's START): the tags as both games hold them.
RECORDS_AT = 1940 if PPLUS else 4380


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def probe_file(name, layout, rumble):
    path = os.path.join(WORK, f"probe-{name}.txt")
    with open(path, "w") as f:
        # Its own tag's controls, once it has its tag (CSS frame 53), long before the join.
        f.write(f"@scSelctCharacter:1 120 write [[805a00e0]+28]+{TAG + 0x0C:x} {rumble:02x}\n")
        f.write(f"@scSelctCharacter:1 120 write [[805a00e0]+28]+{TAG + 0x14:x} {layout.hex()}\n")
        # Reads only: the per-port table, as the match's first frame fills it.
        f.write("@scMelee 60 dump 805b7480 b4 ports\n")
        f.write(f"@scSelctCharacter {RECORDS_AT} dump [[805a00e0]+28] 89c0 records\n")
    return path


class Orca:
    def __init__(self, name, env_extra):
        self.name = name
        self.lines = queue.Queue()
        self.states = []
        self.user = os.path.join(WORK, f"user-{name}")
        os.makedirs(self.user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_DEV_GAME": "orca-dropin-test",
            "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
            "ORCA_NAME": name,
            "YG_INPUT": INPUT,
            "YG_SCENES": "1",
            "YG_HASHLOG": os.path.join(WORK, f"hash-{name}.txt"),
        }
        if PPLUS:
            env["ORCA_PROFILE"] = "PPLUS32"
        # A local rooms stack (ORCA_SITE) and the fresh start's knobs (ORCA.md "Fresh starts").
        for key in ("ORCA_SITE", "ORCA_TEST_FRESH", "ORCA_TEST_FRESH_AFTER"):
            if os.environ.get(key):
                env[key] = os.environ[key]
        env.update(env_extra)
        args = [BIN, "-p", "headless", "-u", self.user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Logs.CORE=True", "-C", "Logger.Options.Verbosity=2"]
        if PPLUS:
            args += ["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS]
        else:
            args += ["-e", DISC]
        self.err = open(os.path.join(WORK, f"{name}.log"), "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                continue
            if line.startswith("orca state "):
                self.states.append(line[len("orca state "):])
            log(f"{self.name}> {line}")
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line):
        log(f"{self.name}< {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def expect(self, want, timeout, forbid=("orca error",)):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                break
            if line is None:
                fail(f"{self.name} exited (code {self.proc.wait()}) waiting for '{want}'")
            if line.startswith(want):
                return line
            if any(line.startswith(f) for f in forbid):
                fail(f"{self.name}: '{line}' while waiting for '{want}'")
        fail(f"{self.name}: no '{want}' in {timeout} s")

    def log_lines(self, text):
        with open(os.path.join(WORK, f"{self.name}.log"), errors="replace") as f:
            return [line.rstrip() for line in f if text in line]

    def wait_log(self, text, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                fail(f"{self.name} exited (code {self.proc.returncode}) waiting for '{text}'")
            found = self.log_lines(text)
            if found:
                return found[0]
            time.sleep(1)
        fail(f"{self.name}: no '{text}' in its log in {timeout} s")

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.send("quit")
                self.proc.wait(timeout=20)
            except Exception:
                self.proc.kill()
        self.err.close()


orcas = []


def fail(why):
    log(f"FAIL: {why}")
    for o in orcas:
        if o.proc.poll() is None:
            o.proc.kill()
    log(f"logs kept in {WORK}")
    sys.exit(1)


def short(line):
    return line[line.find("N[") if "N[" in line else 0:]


log(f"room {ROOM}, work {WORK}")
host = Orca("Host", {"ORCA_ROOM": ROOM,
                     "ORCA_UX_PROBE": probe_file("Host", HOST_LAYOUT, HOST_RUMBLE)})
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
host.expect("orca state playing", 90)
# The friend boots once the host's own controls are set: the host's frames are then well ahead of
# the friend's own, so the friend's hash log jumps forward where it loads the host's game.
log(f"Host: {short(host.wait_log('Probe scSelctCharacter 120: write', 120))}")
friend = Orca("Friend", {"ORCA_UX_PROBE": probe_file("Friend", FRIEND_LAYOUT, FRIEND_RUMBLE)})
orcas.append(friend)
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state playing", 90)

# 1. Both on their own character select with their own controls set.
log(f"Friend: {short(friend.wait_log('Probe scSelctCharacter 120: write', 120))}")
time.sleep(1)

# 2. The friend drops in from its character select.
friend.send(f"join {ROOM}")
friend.expect("orca state joining", 60)
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 120)
host.expect("orca state friend-joined", 60)
log(f"joined: host {short(host.wait_log('Drop-in: port 2 is', 10))}")
for line in friend.log_lines("Drop-in: port "):
    log(f"friend: {short(line)}")

# 3. The match: its first frames fill the per-port table.
for o in (host, friend):
    log(f"{o.name}: {short(o.wait_log('-> scMelee', 240))}")
for o in (host, friend):
    o.wait_log("Probe scMelee 60: dump", 30)
time.sleep(5)
for o in (host, friend):
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped during the match")
    if o.log_lines("Desync at frame") or any("desync" in s for s in o.states):
        fail(f"{o.name} saw a desync: {o.log_lines('Desync at frame')[:3]}")
for o in orcas:
    o.stop()


ok = True
tags = {}
layouts = [HOST_LAYOUT, FRIEND_LAYOUT, DEFAULTS, DEFAULTS]
for o in (host, friend):
    with open(os.path.join(o.user, "probe-ports.bin"), "rb") as f:
        table = f.read()
    # The match's per-port table: GameCube layouts first, one per port.
    for port in range(4):
        gc = table[12 * port:12 * port + 12]
        want = bytes(layouts[port][:12])
        ok = ok and gc == want
        log(f"{o.name}: port {port + 1} plays with GameCube controls {gc.hex()} "
            f"{'ok' if gc == want else 'WANT ' + want.hex()}")
    path = os.path.join(o.user, "probe-records.bin")
    records = open(path, "rb").read() if os.path.exists(path) else b""
    tags[o.name] = records
    named = {}
    for slot in range(120):
        # The tag's own bytes (name, rumble, layout), not the next entry's per-name block.
        tag = records[0xE0 + slot * 0x124:0xE0 + slot * 0x124 + 0x44]
        if any(tag[:10]):
            name = tag[:10].decode("utf-16-be", "replace").split("\0")[0]
            named[name] = (tag[0x0C], bytes(tag[0x14:0x41]))
            log(f"{o.name}: tag {slot} {name} rumble {tag[0x0C]} controls {tag[0x14:0x41].hex()}")
    # Each player's whole layout and rumble, in the tag its port wears.
    for name, rumble, layout in (("HOST", HOST_RUMBLE, HOST_LAYOUT),
                                 ("FRIEN", FRIEND_RUMBLE, FRIEND_LAYOUT)):
        if named.get(name) != (rumble, bytes(layout)):
            ok = False
            log(f"{o.name}: tag {name} WANT rumble {rumble} controls {bytes(layout).hex()}")
if not ok:
    fail("the controls differ from each player's own")
if not tags["Host"] or tags["Host"] != tags["Friend"]:
    fail("the two games' tags differ")

# Hash logs: the host's, and the friend's from its load of the host's game on (its frame numbers
# jump forward there, from its own game's to the host's keyframe).
def hashes(name, last_run_only):
    runs, last = [{}], None
    for line in open(os.path.join(WORK, f"hash-{name}.txt")):
        parts = line.split()
        if len(parts) < 3:
            continue
        frame = int(parts[0])
        if last is not None and frame != last + 1:
            runs.append({})
        last = frame
        runs[-1][frame] = parts[-1]
    if last_run_only:
        return runs[-1]
    return {k: v for run in runs for k, v in run.items()}


h, f = hashes("Host", False), hashes("Friend", True)
common = sorted(set(h) & set(f))
if not common:
    fail("no frames in both hash logs")
diff = [x for x in common if h[x] != f[x]]
log(f"hash logs: frames {common[0]}-{common[-1]}: {len(common) - len(diff)} alike, {len(diff)} "
    f"differ (first {diff[:8]})")
# The joiner's catch-up replays the host's final frames: those are alike before any guess.
log(f"hash logs: alike from {common[0]} to the first that differs: "
    f"{(diff[0] if diff else common[-1] + 1) - common[0]} frames")
if len(diff) > 0.05 * len(common):
    fail("the two games' frames differ")
for o in (host, friend):
    for line in o.log_lines("Online match ")[-1:]:
        log(f"{o.name}: {short(line)}")
log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
