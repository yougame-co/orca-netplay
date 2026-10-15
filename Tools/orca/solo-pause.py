#!/usr/bin/env python3
# solo-pause.py: the player's pause in a session (ORCA.md "Embedding", `pause` / `resume`), steered
# with the app's stdin commands, headless and muted.
#
#   Tools/orca/solo-pause.py <dolphin-emu-nogui> <disc> [--rooms]
#
# 1. Offline, one Orca at a time: a reference run to frame 2800, then the same run paused for 5 s
#    mid-match. The paused run must answer "state paused" / "state running", stop its frames while
#    paused, and match the reference's RAM hash on every frame (YG_HASHLOG).
# 2. --rooms instead: two Orcas over real YouGame dev rooms (ORCA_TEST_DEV_GAME):
#    - the host pauses alone, stays paused past the room's 8 s silence limit (its room stays open)
#      and prints no stats meanwhile; a "prepare-join" (an invite) ends the pause ("state running");
#    - paused again, a friend joins its room: the host runs again by itself, takes the friend in,
#      and the friend plugs in;
#    - with the friend in, "pause" answers "unsupported pause" then "state running";
#    - the friend leaves: the host, alone again, pauses and resumes, and its first stats line
#      after the resume reports a real frame rate, not one diluted by the pause.
# Each step waits for the lines it expects and fails on a timeout.
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
    sys.exit("usage: solo-pause.py <nogui> <disc> [--rooms]")
BIN, DISC = sys.argv[1], sys.argv[2]
ROOMS = "--rooms" in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
WORK = tempfile.mkdtemp(prefix="orca-pause-")
T0 = time.time()
# Paused 35 frames into the match's fuzzed play (the match starts at frame ~2125, play at ~2525).
EXIT_AFTER = 2800
PAUSE_AT = 2560
PAUSE_S = 5


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra, input_file):
        self.name = name
        self.lines = queue.Queue()
        self.stats = []
        user = os.path.join(WORK, f"user-{name}")
        os.makedirs(user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_NAME": name,
            "YG_INPUT": os.path.join(HERE, "inputs", input_file),
            "YG_SCENES": "1",
        }
        env.update(env_extra)
        args = ["nice", "-n", "10", BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Options.Verbosity=2", "-e", DISC]
        self.err = open(os.path.join(WORK, f"{name}.log"), "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                self.stats.append((time.time(), line[len("orca stats "):]))
                continue
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

    def expect_none(self, seconds, forbid):
        """No stdout line starting with any of `forbid` for `seconds`."""
        end = time.time() + seconds
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                return
            if line is None:
                fail(f"{self.name} exited (code {self.proc.wait()})")
            if any(line.startswith(f) for f in forbid):
                fail(f"{self.name}: '{line}'")

    def expect_all(self, wants, timeout, forbid=("orca error",)):
        """Waits for a line starting with each of `wants`, in any order (they come from different
        threads)."""
        left = list(wants)
        end = time.time() + timeout
        while left:
            if time.time() > end:
                fail(f"{self.name}: no {left} in {timeout} s")
            try:
                line = self.lines.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                continue
            if line is None:
                fail(f"{self.name} exited (code {self.proc.wait()}) waiting for {left}")
            if any(line.startswith(f) for f in forbid):
                fail(f"{self.name}: '{line}' while waiting for {left}")
            left = [w for w in left if not line.startswith(w)]

    def frame(self):
        """The newest frame in the stats lines (-1 before the first)."""
        return json.loads(self.stats[-1][1])["f"] if self.stats else -1

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


def pause_alone(orca, timeout):
    """Sends "pause" until the game is alone and it pauses (a keyframe being made refuses it)."""
    end = time.time() + timeout
    while True:
        orca.send("pause")
        line = orca.expect("state ", 10)
        if line == "state paused":
            # It holds: nothing ends it at once.
            orca.expect_none(0.5, ("orca error", "state running"))
            return
        if time.time() > end:
            fail(f"{orca.name}: still no pause after {timeout} s")
        time.sleep(1)


def last_hashed(path):
    """The newest frame in a YG_HASHLOG file (written 60 frames at a time), -1 before any."""
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        return -1
    for line in reversed(lines):
        parts = line.split()
        if len(parts) == 3:
            return int(parts[0])
    return -1


def offline_run(name, pause):
    hashlog = os.path.join(WORK, f"{name}.hashes")
    orca = Orca(name, {"YG_EXIT_AFTER": str(EXIT_AFTER), "YG_THROTTLE": "1",
                       "YG_HASHLOG": hashlog}, "bf-mario-link-results.txt")
    orcas.append(orca)
    orca.expect("orca caps", 30)
    if pause:
        end = time.time() + 120
        while last_hashed(hashlog) < PAUSE_AT:
            if time.time() > end:
                fail(f"{name}: frame {last_hashed(hashlog)} after 120 s, not {PAUSE_AT}")
            time.sleep(0.05)
        orca.send("pause")
        orca.expect("state paused", 10, forbid=("orca error", "unsupported pause"))
        time.sleep(1)
        at_pause = last_hashed(hashlog)
        time.sleep(PAUSE_S - 1)
        still = last_hashed(hashlog)
        log(f"paused: frame {at_pause} after 1 s, {still} after {PAUSE_S} s")
        if still != at_pause:
            fail(f"frames went on while paused ({at_pause} -> {still})")
        orca.send("resume")
        orca.expect("state running", 10)
        time.sleep(3)
        after = last_hashed(hashlog)
        log(f"resumed: frame {after} 3 s later")
        if after < still + 120:
            fail(f"frames didn't go on after the resume ({still} -> {after})")
    orca.expect("orca state ended", 180)
    if orca.proc.wait(timeout=30) != 0:
        fail(f"{name} exited with {orca.proc.returncode}")
    orca.err.close()
    # The run stops a few frames after EXIT_AFTER, how many depends on timing.
    with open(hashlog) as f:
        return [line for line in f.read().splitlines() if int(line.split()[0]) <= EXIT_AFTER]


log(f"work {WORK}")
if not ROOMS:
    reference = offline_run("reference", False)
    paused = offline_run("paused", True)
    if len(reference) < EXIT_AFTER:
        fail(f"the reference run hashed {len(reference)} frames")
    if reference != paused:
        diff = next((i for i, (a, b) in enumerate(zip(reference, paused)) if a != b),
                    min(len(reference), len(paused)))
        fail(f"the paused run differs from the reference from line {diff}: "
             f"{reference[diff] if diff < len(reference) else '(end)'} / "
             f"{paused[diff] if diff < len(paused) else '(end)'}")
    log(f"offline: {len(paused)} frames hashed, the paused run equals the reference")

if ROOMS:
    room = "pz" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(7))
    common = {"ORCA_TEST_DEV_GAME": "orca-pause-test",
              "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf")}
    log(f"room {room}")
    host = Orca("Host", dict(common, ORCA_ROOM=room), "bf-dropin.txt")
    orcas.append(host)
    host.expect("orca caps", 30)
    host.send("caps join leave stats")
    host.expect("orca state playing", 90)
    # Past the first frame a keyframe may be taken of (5 s), with the room open.
    end = time.time() + 60
    while host.frame() < 420 and time.time() < end:
        time.sleep(0.2)

    # Alone: a pause holds past the room's silence limit, and an invite ends it.
    pause_alone(host, 30)
    stats_before = len(host.stats)
    host.expect_none(12, ("orca error", "orca state", "state running"))
    if len(host.stats) > stats_before + 1:
        fail(f"{len(host.stats) - stats_before} stats lines while paused")
    log(f"paused 12 s at frame {host.frame()}: no stats, no error, the room open")
    host.send("prepare-join")
    host.expect("state running", 10)
    time.sleep(3)

    # Paused again (once the invite's keyframe is stored), a friend arrives: the host runs again by
    # itself and takes the friend in.
    pause_alone(host, 60)
    friend = Orca("Friend", common, "bf-dropin.txt")
    orcas.append(friend)
    friend.send(f"join {room}")
    friend.expect("orca caps", 30)
    friend.send("caps join leave stats")
    host.expect_all(("state running", "orca state friend-joining"), 90)
    friend.expect("orca state playing", 120)
    host.expect("orca state friend-joined", 60)

    # With the friend plugged in, the pause is refused.
    host.send("pause")
    host.expect("unsupported pause", 10)
    host.expect("state running", 10)
    time.sleep(5)

    # The friend leaves: the host, alone again, pauses; the stats line after the resume counts
    # running time only.
    friend.send("leave")
    friend.expect("orca state left", 30)
    host.expect("orca state friend-left", 30)
    pause_alone(host, 60)
    time.sleep(PAUSE_S)
    resumed_at = time.time()
    host.send("resume")
    host.expect("state running", 10)
    time.sleep(3)
    after = [json.loads(s) for t, s in host.stats if t > resumed_at]
    if not after:
        fail("no stats line after the resume")
    log(f"first stats after the resume: {after[0]}")
    # Diluted by the 5 s pause it would read about 10 or less; running time alone, about 60.
    if after[0]["fps"] < 30:
        fail(f"fps {after[0]['fps']} after the resume")
    for o in orcas:
        if o.proc.poll() is not None:
            fail(f"{o.name} stopped")
    for o in orcas:
        o.stop()
    for o in orcas:
        with open(os.path.join(WORK, f"{o.name}.log"), errors="replace") as f:
            for line in f:
                if any(k in line for k in ("pause ends", "Drop-in: keyframe of frame",
                                           "Lost the connection", "plugged into port")):
                    log(f"{o.name}: {line.rstrip()[line.find('N[') if 'N[' in line else 0:]}")

log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
