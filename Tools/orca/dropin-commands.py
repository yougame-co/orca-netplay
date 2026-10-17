#!/usr/bin/env python3
# dropin-commands.py: two Orcas over a real YouGame room, steered with the app's stdin commands
# (ORCA.md "Embedding": caps, join, leave) the way the desktop app would, headless and muted.
#
#   Tools/orca/dropin-commands.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#                                 [--env NAME=VALUE ...]
#
# --env (repeatable) sets a variable in both Orcas, a test override such as ORCA_TEST_GATE, which
# then marks both compatibility keys alike.
#
# 1. Host and Friend each boot their own game, solo, in rooms of their own.
# 2. Friend starts and sends "join <host's room>" before the caps handshake: it drops into the
#    host's game once caps turn join on.
# 3. Both pick and start a match (the input scripts wait 100 s into character select, past the
#    join); 10 s into it the friend sends "leave": unplugged at an agreed frame, it plays on solo
#    ("left").
# 4. Friend joins again, into the running match; then the host sends "leave": Friend sees
#    "host-left" and plays on.
# Each step waits for the states it expects and fails on a timeout. Runs two Orca processes.
import json
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
    sys.exit(__doc__ if __doc__ else "usage: dropin-commands.py <nogui> <disc> [--pplus <dol>]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
BOTH_ENV = dict(sys.argv[i + 1].split("=", 1) for i, a in enumerate(sys.argv) if a == "--env")
HERE = os.path.dirname(os.path.abspath(__file__))
# Both games reach character select before the friend is there: the scripts act late there, so the
# two start a match together that the friend's second join drops into.
INPUT = os.path.join(HERE, "inputs", "pplus-dropin.txt" if PPLUS else "bf-dropin.txt")
ROOM = "cmd" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-cmd-")
T0 = time.time()


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra):
        self.name = name
        self.lines = queue.Queue()
        self.states = []
        self.stats = []
        user = os.path.join(WORK, f"user-{name}")
        os.makedirs(user)
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
        }
        if PPLUS:
            env["ORCA_PROFILE"] = "PPLUS32"
        # A local rooms stack (ORCA_SITE) and the fresh start's knobs (ORCA.md "Fresh starts").
        for key in ("ORCA_SITE", "ORCA_TEST_FRESH", "ORCA_TEST_FRESH_AFTER"):
            if os.environ.get(key):
                env[key] = os.environ[key]
        env.update(BOTH_ENV)
        env.update(env_extra)
        args = [BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Logs.CORE=True", "-C", "Logger.Logs.IOS_FS=True",
                "-C", "Logger.Options.Verbosity=2"]
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
                self.stats.append(line[len("orca stats "):])
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
        """Waits for a stdout line starting with `want`; fails on a line starting with `forbid`."""
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

    def wait_log(self, text, timeout):
        """Waits for a line containing `text` in this Orca's log (stderr)."""
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                fail(f"{self.name} exited (code {self.proc.returncode}) waiting for '{text}' in its log")
            with open(os.path.join(WORK, f"{self.name}.log"), errors="replace") as f:
                for line in f:
                    if text in line:
                        return line.rstrip()
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


def last_stats(orca):
    return orca.stats[-1] if orca.stats else "(none)"


log(f"room {ROOM}, work {WORK}")
host = Orca("Host", {"ORCA_ROOM": ROOM})
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
# In mod mode the caps wait for the builtin's guards (ORCA.md "Mods (phase 1)"), so "playing" may
# come before them.
if "playing" not in host.states:
    host.expect("orca state playing", 90)
time.sleep(30)

# 2. A join sent at once, before the caps handshake (as the app does for an invite it opened): Orca
# holds it until "caps" turns join on, then joins.
friend = Orca("Friend", {})
orcas.append(friend)
friend.send(f"join {ROOM}")
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state joining", 60)
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 120)
log(f"joined; host stats {last_stats(host)}")

# 3. A match together; 10 s into it the friend leaves at an agreed frame and plays on.
for o in (host, friend):
    log(f"{o.name}: {o.wait_log('-> scMelee', 240)}")
time.sleep(10)
log(f"10 s into the match: host {last_stats(host)} / friend {last_stats(friend)}")
friend.send("leave")
friend.expect("orca state left", 30)
host.expect("orca state friend-left left", 30)
time.sleep(5)
if friend.proc.poll() is not None:
    fail("friend stopped after leaving")

# 4. Again; then the host leaves.
friend.send(f"join {ROOM}")
friend.expect("orca state playing", 120)
time.sleep(20)
host.send("leave")
host.expect("orca state left", 30)
friend.expect("orca state host-left", 30)
time.sleep(10)
for o in orcas:
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped after the host left")
log(f"host stats {last_stats(host)} / friend {last_stats(friend)}")
for o in orcas:
    o.stop()
# What happened, from the logs: scenes, keyframes (with the NAND they named instead of sending),
# loads, and each session's numbers.
KEYS = ("Scene frame", "Drop-in: keyframe of frame", "Drop-in: loaded keyframe", "Online match",
        "Drop-in: back to solo", "Drop-in: joining room", "doesn't match the host's")
for o in orcas:
    with open(os.path.join(WORK, f"{o.name}.log"), errors="replace") as f:
        for line in f:
            if any(k in line for k in KEYS):
                log(f"{o.name}: {line.rstrip()[line.find('N[') if 'N[' in line else 0:]}")
# Each Orca's "orca stats" lines while a friend was plugged in: frame rate and rollbacks.
for o in orcas:
    together = []
    for line in o.stats:
        try:
            st = json.loads(line)
        except ValueError:
            continue
        if st.get("peers", 0) > 0:
            together.append(st)
    if together:
        fps = [st["fps"] for st in together]
        log(f"{o.name}: {len(together)} s with a friend: fps avg {sum(fps) / len(fps):.1f} min "
            f"{min(fps):.1f}, rollbacks {sum(st['rb'] for st in together)} "
            f"({sum(st['rbf'] for st in together)} frames, deepest "
            f"{max(st['rbmax'] for st in together)}), stalls {sum(st['st'] for st in together)}, "
            f"delay {together[-1]['delay']}, ping {together[-1]['ping']} ms")
log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
