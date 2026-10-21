#!/usr/bin/env python3
# occluded-test.py: checks that a session keeps its pace while its window can't be seen. One Orca
# runs embedded in Tools/orca/embed-test-host.m (Metal, muted) while the window is minimized,
# hidden, and covered by another app. Each phase reports fps, frames lost and hitches from
# "orca stats". PASS when every phase kept 55 fps or more and a friend never waited over a second.
#
#   clang -fobjc-arc -framework AppKit -o embed-test-host Tools/orca/embed-test-host.m
#   Tools/orca/occluded-test.py <embed-test-host> <dolphin-emu-nogui> <disc> [--friend [--overlay]]
#                               [--from-frame N] [--sample DIR] [--shots DIR]
#
# --friend: a second, headless Orca drops into the game first (as dropin-commands.py does); its
#   stall numbers show whether it had to wait for the hidden side.
# --from-frame N: start the phases at frame N (default 7300, 20 s into the match), away from scene
#   changes, whose shader compiles would hitch either way.
# --sample DIR: `sample` the embedded Orca for 3 s in each out-of-sight phase.
# --shots DIR: a screenshot of the main display in each phase.
# It takes the screen for about two minutes (the test host's window comes to the front) and runs
# two Orca processes.
# ORCA_TEST_KEEP=1 keeps the logs.
#
# --overlay (with --friend): instead of hiding the window, open and close the YouGame overlay
# mid-fight on the embedded side, sending the same lines the page does. Nothing may pause: the
# embedded side keeps its pace with neutral input, and the friend sees no stalls and no more
# hitches than with the overlay shut.
import json
import os
import queue
import random
import re
import shutil
import statistics
import string
import subprocess
import sys
import tempfile
import threading
import time


def arg_value(name):
    if name not in sys.argv:
        return None
    i = sys.argv.index(name)
    if i + 1 >= len(sys.argv):
        sys.exit(f"{name} needs a value")
    return sys.argv[i + 1]


if len(sys.argv) < 4:
    sys.exit("usage: occluded-test.py <embed-test-host> <dolphin-emu-nogui> <disc> "
             "[--friend [--overlay]] [--sample DIR] [--shots DIR]")
HOST_APP, BIN, DISC = sys.argv[1], sys.argv[2], sys.argv[3]
FRIEND = "--friend" in sys.argv
OVERLAY = "--overlay" in sys.argv
if OVERLAY and not FRIEND:
    sys.exit("--overlay needs --friend: it is the friend's wait that it checks")
SAMPLE = arg_value("--sample")
SHOTS = arg_value("--shots")
FROM_FRAME = int(arg_value("--from-frame") or (3000 if OVERLAY else 7300))
# With --overlay the friend drops in mid-fight at this host frame (the match starts at about 2134).
# Dropping in on the main menu would move both to a character select this input script crashes on.
OVERLAY_JOIN_FRAME = int(arg_value("--join-frame") or 2400)
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "bf-mario-link-results.txt")
ROOM = "occ" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-occluded-")
T0 = time.time()
PHASE_S = 12
MIN_FPS = 55
MAX_FRIEND_STALL_MS = 1000
# With --overlay: how long the friend may wait while the overlay is open or just shut, and how many
# extra hitches either side may have.
MAX_OVERLAY_STALL_MS = 100
MAX_OVERLAY_EXTRA_HITCHES = 1


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def session_env(name, extra):
    env = {
        "HOME": os.environ["HOME"],
        "PATH": os.environ["PATH"],
        "ORCA_SESSION": "1",
        "ORCA_TEST_DEV_GAME": "orca-present-test",
        "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
        "ORCA_NAME": name,
        "YG_INPUT": INPUT,
    }
    env.update(extra)
    return env


def common_args(name):
    user = os.path.join(WORK, f"user-{name}")
    os.makedirs(user)
    return ["-u", user, "-C", "Dolphin.DSP.Backend=No Audio Output",
            "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.VIDEO=True",
            "-C", "Logger.Options.Verbosity=2", "-e", DISC]


class Failed(Exception):
    pass


class Proc:
    """A process whose stdout lines are queued; Orca's own lines (prefix stripped) are kept."""

    def __init__(self, name, args, env, prefix=None):
        self.name = name
        self.prefix = prefix
        self.lines = queue.Queue()
        self.stats = []  # (time, dict)
        self.where = None
        self.err = open(os.path.join(WORK, f"{name}.log"), "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if self.prefix:
                m = self.prefix.match(line)
                if not m:
                    if line.startswith("where window "):
                        self.where = line
                    elif not line.startswith("host> "):
                        log(f"{self.name}: {line}")
                    continue
                line = m.group(1)
            if line.startswith("orca stats "):
                try:
                    self.stats.append((time.time(), json.loads(line[len("orca stats "):])))
                except ValueError:
                    log(f"{self.name}: unreadable stats line: {line}")
                continue
            log(f"{self.name}> {line}")
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, ValueError):
            raise Failed(f"{self.name} is gone (writing '{line}')")

    def expect(self, want, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                break
            if line is None:
                raise Failed(f"{self.name} exited waiting for '{want}'")
            if line.startswith(want):
                return line
            if line.startswith("orca error"):
                raise Failed(f"{self.name}: '{line}' while waiting for '{want}'")
        raise Failed(f"{self.name}: no '{want}' in {timeout} s")

    def stop(self, quit_line):
        if self.proc.poll() is None:
            try:
                self.send(quit_line)
                self.proc.wait(timeout=20)
            except Exception:
                self.proc.kill()
        self.err.close()


EMBED_LINE = re.compile(r"^orca> \[\s*[\d.]+\] (.*)$")


class TestHost(Proc):
    """A test host window, with an Orca in it when `orca_args` are given; `host` steers the
    window, `orca` writes a line to Orca's stdin."""

    def __init__(self, name, env, orca_args=None):
        # Without Orca: a stand-in child that keeps reading what the host writes to it.
        if not orca_args:
            stand_in = os.path.join(WORK, "stand-in.sh")
            with open(stand_in, "w") as f:
                f.write("#!/bin/sh\nexec cat > /dev/null\n")
            os.chmod(stand_in, 0o755)
            orca_args = [stand_in]
        super().__init__(name, [HOST_APP] + orca_args, env, prefix=EMBED_LINE)

    def orca(self, line):
        log(f"{self.name}< {line}")
        self.send(f"send {line}")

    def host(self, line):
        log(f"{self.name} window: {line}")
        self.send(line)

    def orca_pid(self):
        out = subprocess.run(["pgrep", "-P", str(self.proc.pid)], capture_output=True, text=True)
        pids = out.stdout.split()
        return int(pids[0]) if pids else None

    def frame(self):
        """The window's frame in global top-left points, from the host's 'where' line."""
        self.where = None
        self.send("where")
        end = time.time() + 5
        while time.time() < end and not self.where:
            time.sleep(0.1)
        m = re.match(r"where window (-?\d+),(-?\d+),(\d+),(\d+)", self.where or "")
        return tuple(int(v) for v in m.groups()) if m else None


def summarize(proc, start, end):
    secs = [s for t, s in proc.stats if start < t <= end]
    if not secs:
        return {"seconds": 0}
    before = [s for t, s in proc.stats if t <= start]
    hi_before = before[-1].get("hi", 0) if before else 0
    fps = [s.get("fps", 0) for s in secs]
    return {
        "seconds": len(secs),
        "fps_mean": round(statistics.mean(fps), 1),
        "fps_min": min(fps),
        "lost": round(sum(max(0.0, 59.94 - f) for f in fps)),  # frames short of the VI rate
        "hi": secs[-1].get("hi", 0) - hi_before,
        "phi_total": secs[-1].get("phi", 0),
        "stalls": sum(s.get("st", 0) for s in secs),
        "stall_ms": sum(s.get("stms", 0) for s in secs),
        "rb": sum(s.get("rb", 0) for s in secs),
    }


phases = []  # (name, start, end, {player: summary})


def phase(name, players, sample=False):
    start = time.time()
    if SHOTS:
        time.sleep(3)
        shot = os.path.join(SHOTS, f"{name}.jpg")
        subprocess.run(["screencapture", "-x", "-m", "-t", "jpg", shot], capture_output=True)
        subprocess.run(["sips", "-Z", "1400", shot], capture_output=True)
    if sample and SAMPLE:
        pid = players[0].orca_pid()
        if pid:
            time.sleep(max(0.0, 5 - (time.time() - start)))
            out = os.path.join(SAMPLE, f"{name}.txt")
            subprocess.run(["sample", str(pid), "3", "-file", out], capture_output=True)
            log(f"sampled {pid} into {out}")
    time.sleep(max(0.0, PHASE_S - (time.time() - start)))
    end = time.time()
    # The first 1.5 s of a phase still has the previous one in it.
    row = {p.name: summarize(p, start + 1.5, end) for p in players}
    phases.append((name, start, end, row))
    clock = time.strftime("%H:%M:%S", time.localtime(start))
    log(f"phase {name} (from {clock}): {json.dumps(row)}")


def window_events():
    """The Host log's window and drawable lines, as (seconds since T0, text). Dolphin stamps its
    log lines with minutes:seconds:milliseconds."""
    t0 = time.localtime(T0)
    t0_s = t0.tm_min * 60 + t0.tm_sec + (T0 % 1)
    events = []
    try:
        f = open(os.path.join(WORK, "Host.log"), errors="replace")
    except OSError:
        return events
    with f:
        for line in f:
            if "the game's window is" not in line and "Metal:" not in line:
                continue
            m = re.match(r"(\d+):(\d+):(\d+) \S+ \S+: (.*)", line)
            if m:
                s = int(m.group(1)) * 60 + int(m.group(2)) + int(m.group(3)) / 1000
                events.append(((s - t0_s) % 3600, m.group(4).strip()))
    return events


def run():
    host = TestHost("Host", session_env("Host", {"ORCA_ROOM": ROOM}),
                    [BIN, "-p", "macos", "-v", "Metal"] + common_args("Host"))
    procs.append(host)
    host.expect("orca caps", 60)
    host.orca("caps join leave stats yougame" if OVERLAY else "caps join leave stats")
    host.expect("orca state playing", 90)
    players = [host]

    if FRIEND and OVERLAY:
        end = time.time() + 300
        while not (host.stats and host.stats[-1][1].get("f", 0) >= OVERLAY_JOIN_FRAME):
            if time.time() > end or host.proc.poll() is not None:
                raise Failed(f"the game didn't reach frame {OVERLAY_JOIN_FRAME} for the friend")
            time.sleep(0.5)
    if FRIEND:
        friend = Proc("Friend", [BIN, "-p", "headless", "-v", "Null"] + common_args("Friend"),
                      dict(session_env("Friend", {}), ORCA_TEST_COMMANDS="1"))
        procs.append(friend)
        friend.send(f"join {ROOM}")
        friend.expect("orca caps", 30)
        friend.send("caps join leave stats")
        friend.expect("orca state joining", 60)
        friend.expect("orca state playing", 120)
        players.append(friend)
        log("friend plugged in")

    # The occluder: another app's window (a test host with no Orca), out of the way until needed.
    occluder = TestHost("Occluder", {"HOME": os.environ["HOME"], "PATH": os.environ["PATH"]})
    procs.append(occluder)
    time.sleep(1)
    occluder.host("mini")
    host.host("front")
    log(f"waiting for frame {FROM_FRAME}")
    end = time.time() + 300
    while not (host.stats and host.stats[-1][1].get("f", 0) >= FROM_FRAME):
        if time.time() > end or host.proc.poll() is not None:
            raise Failed(f"the game didn't reach frame {FROM_FRAME}")
        time.sleep(0.5)

    if OVERLAY:
        # The YouGame button where the page's stands (OrcaEmbed syncOrb), the overlay shut.
        host.orca("orb 32 32 80 0 0 0")
        phase("visible", players)
        # Open: the menu beside the game takes the controls; the button goes.
        host.orca("orb off")
        host.orca("blur")
        host.host("panel 300")
        host.orca("dim 50")
        phase("ux-open", players, sample=True)
        # Shut: the game has the controls again, the button is back, a line beside it.
        host.host("panel 0")
        host.orca("dim 0")
        host.orca("focus")
        host.orca("orb 32 32 80 1 0 0")
        host.orca("notice voice Ada is%20on%20voice Shift%2BTab Join")
        phase("ux-closed", players)
        friend.stop("quit")
        host.orca("quit")
        time.sleep(2)
        host.stop("exit")
        occluder.stop("exit")
        return

    phase("visible", players)
    host.host("mini")
    phase("minimized", players, sample=True)
    host.host("unmini")
    host.host("front")
    phase("restored", players)
    host.orca("hide")
    phase("hidden", players, sample=True)
    host.orca("show")
    phase("shown", players)
    frame = host.frame()
    if not frame:
        raise Failed("no 'where' from the test host")
    x, y, w, h = frame
    occluder.host("unmini")
    occluder.host(f"size {w + 80} {h + 80}")
    occluder.host(f"move {x - 40} {y - 40}")
    occluder.host("front")
    phase("covered", players, sample=True)
    occluder.host("mini")
    host.host("front")
    phase("uncovered", players)

    if FRIEND:
        friend.stop("quit")
    host.orca("quit")
    time.sleep(2)
    host.stop("exit")
    occluder.stop("exit")


procs = []
log(f"room {ROOM}, work {WORK}")
for d in (SAMPLE, SHOTS):
    if d:
        os.makedirs(d, exist_ok=True)
failure = None
try:
    run()
except Failed as e:
    failure = str(e)
finally:
    for p in procs:
        if p.proc.poll() is None:
            p.proc.kill()

events = window_events()
problems = [failure] if failure else []
# A build that reports its window state (PlatformMacOS) must say it was out of sight mid-phase in
# each hiding phase (or earlier: another app can cover the test host at any time).
window = [(t, "out of sight" in text) for t, text in events if "the game's window is" in text]
for name, start, end, row in phases:
    if window and name in ("minimized", "hidden", "covered"):
        middle = (start + end) / 2 - T0
        before = [hidden for t, hidden in window if t <= middle]
        if not (before and before[-1]):
            problems.append(f"{name}: Orca's window wasn't out of sight")
    print(f"{name:10} " + " | ".join(
        f"{who} fps {s.get('fps_mean', '-')} (min {s.get('fps_min', '-')}, lost "
        f"{s.get('lost', '-')}) hi {s.get('hi', '-')} stalls {s.get('stalls', '-')} "
        f"{s.get('stall_ms', '-')} ms" for who, s in row.items()))
    for t, text in events:
        if start - T0 < t <= end - T0:
            print(f"{'':10} [{t:6.1f}] Host log: {text}")
    for who, s in row.items():
        if not s.get("seconds"):
            problems.append(f"{name}: no stats from {who}")
        elif s["fps_mean"] < MIN_FPS:
            problems.append(f"{name}: {who} at {s['fps_mean']} fps")
        elif who == "Friend" and s["stall_ms"] > MAX_FRIEND_STALL_MS:
            problems.append(f"{name}: the friend waited {s['stall_ms']} ms")
if OVERLAY:
    base = next((row for name, _, _, row in phases if name == "visible"), None)
    for name, _, _, row in phases:
        if name not in ("ux-open", "ux-closed"):
            continue
        friend_row = row.get("Friend", {})
        if friend_row.get("seconds") and friend_row["stall_ms"] > MAX_OVERLAY_STALL_MS:
            problems.append(f"{name}: the friend waited {friend_row['stall_ms']} ms")
        for who, s in row.items():
            was = (base or {}).get(who, {}).get("hi", 0)
            if s.get("seconds") and s["hi"] > was + MAX_OVERLAY_EXTRA_HITCHES:
                problems.append(f"{name}: {who} had {s['hi']} hitches ({was} with it shut)")
for p in problems:
    log(f"FAIL: {p}")
if problems or os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
log("FAIL" if problems else "PASS")
sys.exit(1 if problems else 0)
