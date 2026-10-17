#!/usr/bin/env python3
# online-menu-dropin.py: a friend drops into a host who sits on the game's own ONLINE page
# (ORCA.md "Online menu", "Drop-in"), over a real YouGame dev room, headless and muted.
#
#   Tools/orca/online-menu-dropin.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>] [--invite]
#                                    [--hold] [--disc2 <disc>]
#
# --disc2: the friend boots this disc instead (Brawl Rev 1 against a Rev 2 host, or the reverse).
#
# 1. Host boots to the main menu and opens PLAY ONLINE: the ONLINE page (With Friends / With
#    Anyone), and stays there.
# 2. Friend sends "join <host's room>": the host's keyframe is taken on the ONLINE page, the friend
#    loads it, catches up and plugs into port 2 ("playing" / "friend-joined").
# 3. Half a second after the friend plugs in, both games move to With Friends' character select
#    at the same frame (UX/OnlineMenu.h FriendsMove). Neither Orca prints an "orca menu" line: menu
#    moves while friends play are not events.
# 4. The friend leaves: the host plays on alone ("friend-left left"), still printing nothing.
# 5. The checksum gate: each side's session matched checksums with the other, and neither logged a
#    desync.
# Fails on a desync, a timeout, an "orca error", or any "orca menu" line.
#
# --hold: the host's game acts as if in a single-player mode until frame HOLD_UNTIL
# (ORCA_TEST_HOLD), so the friend waits ("friend-waiting"), past the 90 s join limit if needed.
# Then the join goes on and step 3 follows.
#
# --invite (one Orca at a time, two boots): the app invites a friend who hasn't arrived yet. The
# host, on the ONLINE page, gets "prepare-join". Since 0.3.34 that makes no keyframe (the friend's
# arrival may fresh-start the host first, ORCA.md "Fresh starts"), so the game stays its player's
# alone: a pick of With Anyone > Casual some 15 s (`fresh`) or 45 s (`stale`) later prints
# "orca menu online casual local" either way. Each checks that no keyframe was made.
#
# Since 0.3.34 a friend arriving more than 10 s after the host's origin fresh-starts it straight to
# With Friends' character select (ORCA.md "Fresh starts"), with no FriendsMove. So both Orcas run
# with ORCA_TEST_FRESH_AFTER=100000000: the friend joins in place, which is what this tool checks.
# Set ORCA_TEST_FRESH or ORCA_TEST_FRESH_AFTER yourself to override that (long-join.py checks fresh
# starts). A local rooms stack: ORCA_SITE=http://127.0.0.1:<port>.
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
    sys.exit("usage: online-menu-dropin.py <nogui> <disc> [--pplus <dol>] [--invite] "
             "[--disc2 <disc>]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
DISC2 = sys.argv[sys.argv.index("--disc2") + 1] if "--disc2" in sys.argv else DISC
INVITE = "--invite" in sys.argv
HOLD = "--hold" in sys.argv
# --hold: the host's frame its hold ends at (it reaches the menu at about 1,430 in Brawl, and the
# friend arrives some 15 s later).
HOLD_UNTIL = 4200
ROOM = "olm" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-online-dropin-")
T0 = time.time()
# The host's pick, in frames of the main menu scene: late enough that the friend has plugged in
# (a friend plugs in some 30-60 s after the host reaches the menu). A join slower than this makes
# the host pick alone: it prints the event and the test fails on it, saying so.
PICK_AT = 7200
# --invite: the picks, some 15 s and some 45 s after the keyframe the invite stores (it is sent 10 s
# after the menu shows, and taken about 800 frames into the menu).
INVITE_PICKS = {"fresh": 1500, "stale": 3600}

# Both games boot to the main menu (Project+ through its title, PPLUS32.patches); the boot screens'
# presses do nothing in Project+.
HOST_INPUT = ["@scStrap mash 30 99999 1 40 A", "@scBoot mash 30 99999 1 40 A",
              "@scTitle mash 30 99999 1 40 A",
              "@muMenuMain 120 123 1 DOWN", "@muMenuMain 170 173 1 A"]


def pick_at(frame):
    return [f"@muMenuMain {frame} {frame + 3} 1 RIGHT",
            f"@muMenuMain {frame + 50} {frame + 53} 1 A",
            f"@muMenuMain {frame + 100} {frame + 103} 1 A"]


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def write(name, lines):
    path = os.path.join(WORK, name)
    with open(path, "w") as f:
        f.write("\n".join(["# online-menu-dropin.py"] + lines) + "\n")
    return path


class Orca:
    def __init__(self, name, env_extra, input_path, disc=DISC):
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
            "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
            "ORCA_NAME": name,
            "YG_INPUT": input_path,
            "YG_SCENES": "1",
        }
        if PPLUS:
            env["ORCA_PROFILE"] = "PPLUS32"
        # A local rooms stack (ORCA_SITE), and the fresh start's knobs: by default the friend joins
        # in place and FriendsMove moves both games.
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
            args += ["-C", f"Dolphin.Core.DefaultISO={disc}", "-e", PPLUS]
        else:
            args += ["-e", disc]
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
            if line.startswith("orca error") or line.startswith("orca menu"):
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


def start_host(room, picks, label="Host", env_extra=None):
    host = Orca(label, {"ORCA_ROOM": room, **(env_extra or {})},
                write(f"{label}.txt", HOST_INPUT + pick_at(picks)))
    orcas.append(host)
    host.expect("orca caps", 30)
    host.send("caps join leave stats")
    host.expect("orca state playing", 90)
    log(f"{label}: {host.wait_log('-> muMenuMain', 180)}")
    # On the ONLINE page (opened 170-250 frames into the menu) for a few seconds.
    time.sleep(10)
    return host


def frame_of(line, key):
    return int(line.split(key)[1].split(":")[0])


def invite(case):
    room = ROOM + case[0]
    log(f"--invite {case}: room {room}")
    host = start_host(room, INVITE_PICKS[case], f"Host-{case}")
    # Project+ boots into its character select: the one that counts is the next.
    css_before = len(host.log_lines("-> scSelctCharacter"))
    host.send("prepare-join")
    css = host.wait_log("-> scSelctCharacter", 180, after=css_before)
    log(f"{host.name}: {css[css.find('Scene frame'):]}")
    time.sleep(5)
    if host.log_lines("Drop-in: keyframe of frame"):
        fail(f"{host.name}: prepare-join made a keyframe: "
             f"{host.log_lines('Drop-in: keyframe of frame')[0]}")
    if host.proc.poll() is not None:
        fail(f"{host.name} stopped")
    want = ["orca menu online casual local"]
    if host.menu != want:
        fail(f"{host.name} printed {host.menu} after the invite (expected {want})")
    log(f"{host.name}: no keyframe for the invite; the pick printed {host.menu}")
    host.stop()
    orcas.remove(host)


log(f"room {ROOM}, work {WORK}")
if INVITE:
    for case in ("fresh", "stale"):
        try:
            invite(case)
        except Exception as e:  # never leave an Orca running
            fail(f"--invite {case}: {type(e).__name__}: {e}")
    log("PASS")
    if os.environ.get("ORCA_TEST_KEEP") == "1":
        log(f"logs kept in {WORK}")
    else:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(0)

host = start_host(ROOM, PICK_AT, env_extra={"ORCA_TEST_HOLD": f"0-{HOLD_UNTIL}"} if HOLD else None)

friend = Orca("Friend", {}, write("friend.txt", ["# no presses: port 2 stays still"]), DISC2)
orcas.append(friend)
friend.send(f"join {ROOM}")
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state joining", 60)
host.expect("orca state friend-joining", 60)
if HOLD:
    host.expect("orca state friend-holding", 30)
    friend.expect("orca state friend-waiting", 30)
    log("held: the host's game is in a single-player mode")
    friend.expect("orca state joining 0", 180)
    host.expect("orca state friend-joining", 30)
    held = host.wait_log("Drop-in: the join goes on at frame", 10)
    log(f"Host: {held[held.find('Drop-in'):]}")
friend.expect("orca state playing", 120)
host.expect("orca state friend-joined", 60)
keyframe = host.wait_log("Drop-in: keyframe of frame", 10)
log(f"Host: {keyframe}")

# The friend plugged in on the ONLINE page: both go to With Friends' character select, at the same
# frame.
MOVE = "the main menu goes to With Friends' character select"
before = {o.name: len(o.log_lines("-> scSelctCharacter")) for o in orcas}
frames = {}
for o in orcas:
    moved = o.wait_log(MOVE, 60)
    log(f"{o.name}: {moved[moved.find('Online menu'):]}")
    line = o.wait_log("-> scSelctCharacter", 240, after=before[o.name])
    frames[o.name] = int(line.split("Scene frame ")[1].split(":")[0])
    log(f"{o.name}: {line[line.find('Scene frame'):]}")
if frames["Host"] != frames["Friend"]:
    fail(f"character select at frame {frames['Host']} (host) and {frames['Friend']} (friend)")
if len(host.log_lines(MOVE)) != 1:
    fail(f"the host moved {len(host.log_lines(MOVE))} times")
time.sleep(10)
friend.send("leave")
friend.expect("orca state left", 30)
host.expect("orca state friend-left left", 30)
time.sleep(10)
for o in orcas:
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped")
    if o.menu:
        fail(f"{o.name} printed {o.menu} while not alone")
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
