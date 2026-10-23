#!/usr/bin/env python3
# versus-cpu-dropin.py: a friend drops into a host's local Versus character select (Group > Brawl)
# that has CPUs (ORCA.md "No CPUs online"), over a real YouGame dev room. Runs two Orca processes,
# headless and muted.
#
#   Tools/orca/versus-cpu-dropin.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#
# 1. The host goes to Group > Brawl's character select, makes CPUs of panels 2 and 3, picks, and
#    waits there.
# 2. The friend joins the host's room, loads the keyframe taken on that select, catches up and
#    plugs into port 2, whose panel is a CPU's.
# 3. At the plug-in both games clear both CPU panels on the same frame, and port 2's panel becomes
#    the friend's (kind 1) on the same frame in both, with no input from the friend: Orca presses A
#    for port 2 on its player-type button (OnlineSeats::PressesJoin).
# 4. The friend leaves. Each side's session must end with matched checksums and no desync.
# Fails on a desync, a timeout or an "orca error".
#
# Since 0.3.34 a friend arriving more than 10 s after the host's last start would fresh-start it to
# With Friends' select (ORCA.md "Fresh starts"), so both Orcas run with
# ORCA_TEST_FRESH_AFTER=100000000 unless ORCA_TEST_FRESH or ORCA_TEST_FRESH_AFTER is set. A local
# rooms stack: ORCA_SITE.
import os
import queue
import random
import re
import shutil
import string
import subprocess
import sys
import tempfile
import threading
import time

if len(sys.argv) < 3:
    sys.exit("usage: versus-cpu-dropin.py <nogui> <disc> [--pplus <dol>]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
ROOM = "vcd" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-versus-cpu-dropin-")
T0 = time.time()

# The host's presses: Group > Brawl, A on panel 2's and panel 3's player-type buttons (two CPUs),
# then up into the grid and a pick. The hand takes 18 frames a panel in Brawl, 15 in Project+.
if PPLUS:
    HOST_INPUT = ["@scBoot mash 30 99999 1 40 A",
                  "@muMenuMain:1 120 123 1 A", "@muMenuMain:1 170 173 1 A",
                  "@scSelctCharacter:1 60 74 1 SX=255", "@scSelctCharacter:1 100 103 1 A",
                  "@scSelctCharacter:1 110 124 1 SX=255", "@scSelctCharacter:1 150 153 1 A",
                  "@scSelctCharacter:1 170 194 1 SY=255", "@scSelctCharacter:1 220 223 1 A"]
else:
    HOST_INPUT = ["@scStrap mash 30 99999 1 40 A", "@scBoot mash 30 99999 1 40 A",
                  "@scTitle mash 30 99999 1 40 A",
                  "@muMenuMain:1 120 123 1 A", "@muMenuMain:1 170 173 1 A",
                  "@scSelctCharacter:1 60 77 1 SX=255", "@scSelctCharacter:1 100 103 1 A",
                  "@scSelctCharacter:1 110 127 1 SX=255", "@scSelctCharacter:1 150 153 1 A",
                  "@scSelctCharacter:1 170 210 1 SY=255", "@scSelctCharacter:1 230 233 1 A"]
# Both Orcas log each panel's kind (an ORCA_UX_PROBE watch; ORCA_TEST_DEV_GAME allows it).
WATCH = os.path.join(WORK, "watch.txt")
with open(WATCH, "w") as f:
    for i, at in enumerate(("44", "48", "4c", "50")):
        f.write(f"@* 0 watch [[[[805a0060]+4]+400]+{at}]+1b4 a{i + 1}-kind\n")


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def write(name, lines):
    path = os.path.join(WORK, name)
    with open(path, "w") as f:
        f.write("\n".join(["# online-menu-dropin.py"] + lines) + "\n")
    return path


class Orca:
    def __init__(self, name, env_extra, input_path):
        self.name = name
        self.lines = queue.Queue()
        self.menu = []
        user = os.path.join(WORK, f"user-{name}")
        os.makedirs(user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_DEV_GAME": "orca-dropin-test",
            "ORCA_UX_PROBE": WATCH,
            "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
            "ORCA_NAME": name,
            "YG_INPUT": input_path,
            "YG_SCENES": "1",
        }
        if PPLUS:
            env["ORCA_PROFILE"] = "PPLUS32"
        # A local rooms stack (ORCA_SITE) and the fresh start's knobs (ORCA.md "Fresh starts"). By
        # default the friend joins in place: a fresh start would leave the host's Versus select,
        # CPUs and all, for With Friends'.
        if not os.environ.get("ORCA_TEST_FRESH") and not os.environ.get("ORCA_TEST_FRESH_AFTER"):
            env["ORCA_TEST_FRESH_AFTER"] = "100000000"
        for key in ("ORCA_SITE", "ORCA_TEST_FRESH", "ORCA_TEST_FRESH_AFTER"):
            if os.environ.get(key):
                env[key] = os.environ[key]
        env.update(env_extra)
        args = ["nice", "-n", "10", BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Options.Verbosity=2"]
        if PPLUS:
            args += ["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS]
        else:
            args += ["-e", DISC]
        self.log_path = os.path.join(WORK, f"{name}.log")
        self.err = open(self.log_path, "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                continue
            if line.startswith("orca menu "):
                self.menu.append(line)
            log(f"{self.name}> {line}")
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line):
        log(f"{self.name}< {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def expect(self, want, timeout):
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
            if line.startswith("orca error"):
                fail(f"{self.name}: '{line}' while waiting for '{want}'")
        fail(f"{self.name}: no '{want}' in {timeout} s")

    def log_lines(self, text):
        with open(self.log_path, errors="replace") as f:
            return [l.rstrip() for l in f if text in l]

    def wait_log(self, text, timeout, after=0):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                fail(f"{self.name} exited (code {self.proc.returncode}) waiting for '{text}'")
            found = self.log_lines(text)
            if len(found) > after:
                return found[after]
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


def start_host():
    host = Orca("Host", {"ORCA_ROOM": ROOM}, write("Host.txt", HOST_INPUT))
    orcas.append(host)
    host.expect("orca caps", 30)
    host.send("caps join leave stats")
    host.expect("orca state playing", 90)
    log(f"Host: {host.wait_log('-> scSelctCharacter', 240)}")
    # The CPUs (by the select's frame 151) and the pick (233).
    host.wait_log("a3-kind = 00000002", 60)
    time.sleep(5)
    return host


log(f"room {ROOM}, work {WORK}")
host = start_host()
cpus = host.log_lines("-kind = 00000002")
if len(cpus) < 2:
    fail(f"Host: the CPUs weren't made ({cpus})")
friend = Orca("Friend", {}, write("friend.txt", ["# no presses: port 2 stays still"]))
orcas.append(friend)
friend.send(f"join {ROOM}")
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state joining", 60)
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 120)
host.expect("orca state friend-joined", 60)
log(f"Host: {host.wait_log('Drop-in: keyframe of frame', 10)}")

CLEARED = re.compile(r"Online seats: frame (\d+): \d+ record state\(s\) and 2 panel\(s\) cleared")
JOINED = re.compile(r"Watch frame (\d+) \(scSelctCharacter \d+\): a2-kind = 00000001")
got = {}
for o in orcas:
    line = o.wait_log("panel(s) cleared", 60)
    m = CLEARED.search(line)
    if not m:
        fail(f"{o.name}: {line}")
    end = time.time() + 30
    joined = None
    while joined is None and time.time() < end:
        joined = next((int(j.group(1)) for j in (JOINED.search(l) for l in o.log_lines("a2-kind"))
                       if j and int(j.group(1)) > int(m.group(1))), None)
        time.sleep(1)
    if joined is None:
        fail(f"{o.name}: port 2's panel never became the friend's after the clear at {m.group(1)}")
    got[o.name] = (int(m.group(1)), joined)
    log(f"{o.name}: CPUs cleared at {m.group(1)}, port 2's panel the friend's at {joined}")
if got["Host"] != got["Friend"]:
    fail(f"the host and the friend disagree: {got}")
time.sleep(10)
friend.send("leave")
friend.expect("orca state left", 30)
host.expect("orca state friend-left left", 30)
time.sleep(10)
for o in orcas:
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped")
    desync = (o.log_lines("desync") + o.log_lines("Desync at frame") +
              o.log_lines("the session failed") + o.log_lines("doesn't match the host's"))
    if desync:
        fail(f"{o.name}: {desync[0]}")
SOLO_RE = re.compile(r"Drop-in: back to solo play at frame (\d+) \(([^)]*)\): (\d+) rollbacks, "
                     r"(\d+) checksums matched")
sums = []
for o in orcas:
    ends = [m for m in (SOLO_RE.search(l) for l in o.log_lines("Drop-in: back to solo play")) if m]
    if not ends:
        fail(f"{o.name}: its session never ended (no 'back to solo play' line)")
    for m in ends:
        if int(m.group(4)) <= 0:
            fail(f"{o.name}: the session that ended at frame {m.group(1)} ({m.group(2)}) matched "
                 f"no checksums")
    sums.append(f"{o.name} " + " + ".join(f"{m.group(4)} ({m.group(2)}, {m.group(3)} rollbacks)"
                                          for m in ends))
for o in orcas:
    o.stop()
KEYS = ("Scene frame", "Drop-in: keyframe of frame", "Drop-in: loaded keyframe", "Online match",
        "Drop-in: back to solo", "main menu text", "Online menu")
for o in orcas:
    for line in o.log_lines(""):
        if any(k in line for k in KEYS):
            log(f"{o.name}: {line[line.find('N[') if 'N[' in line else 0:]}")
log(f"PASS; checksums: {'; '.join(sums)}")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
