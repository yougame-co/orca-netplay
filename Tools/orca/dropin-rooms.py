#!/usr/bin/env python3
# dropin-rooms.py: rooms that go wrong, over real YouGame rooms, steered with the app's stdin
# commands (ORCA.md "Embedding") as in dropin-commands.py, headless and muted.
#
#   Tools/orca/dropin-rooms.py <dolphin-emu-nogui> <disc>
#
# 1. A friend joins a room nobody's game is in: it gives up within seconds (peer_left), plays on.
# 2. The friend reaches a room before its host's game does: the host finds someone else hosting
#    it, says so (orca error network) and opens a fresh room; the friend gives up (peer_left).
# 3. The friend leaves while it catches up (the host's keyframe is 25 s old, from a prepare-join, so
#    the friend replays 25 s of its game): it leaves at once ("left"), and the host hears a plain
#    leave, not a stall. Then it joins the host's fresh room: "friend-joined" once it plugs in.
# 4. The host's room socket drops mid-match (the test-only "test-drop-room"): it reconnects
#    in the background to the same room and seat. The friend retains its session and room.
#    Then the app's Stop on the host, "leave" and "quit" at once: the friend hears it within ~1 s.
# 5. Both behind a fake app bridge (Tools/orca/fake_bridge.py), the friend joining from an invite:
#    the host's page sends Leave. The host's page is told the room is empty, then shown the host's
#    new room. The friend's page is told the old room is empty and never shows it again: in a fight
#    the friend then says "no-room" (port 2); on the menus it goes home (port 1, a room of its own,
#    which its page is shown).
# Where a step leaves the friend on a port other than 1 (3, 4 and 5), it either goes home at once
# (it was on the menus) or says "no-room" (in a fight): both pass.
# 6. A friend whose host stops goes home and can queue: both behind the bridge with the queue's
#    character select (caps host queue2) and live pads, no input script. The friend joins the host
#    on the main menu, both go to With Friends' character select, and the host's app stops. The
#    friend hears "host-left", backs out (held B), picks With Anyone > Ranked from the ONLINE page,
#    a character, and Start: "orca queue ready ranked" within 10 s. It came home on the character
#    select (port 1), never said "no-room", and its page was shown a room of its own.
# 7. With Anyone with a friend in the game: the same pair on With Friends' character select; the
#    host backs out and picks With Anyone > Ranked with the friend still plugged in: "orca state
#    left lobby", then "orca menu online ranked". The friend hears "host-left" and goes home on the
#    character select the pick opened, where it takes the pick too. Neither backs out: each picks a
#    character and presses Start where it landed, and both say "orca queue ready ranked".
# 8. The same, but the friend presses With Anyone > Ranked in the shared menu: the host leaves for
#    the queue all the same, and the friend comes home on its Ranked select, armed.
# 9. A friend drops into the room of a guest that came home: the pair on With Friends' character
#    select, the host's app stops, and the friend goes home (port 1, a room of its own). A new Orca
#    joins that room from an invite, both play 15 s, the new one's app leaves, and the session ends
#    with matched checksums ("friend-joined", then "friend-left", no error).
# 10. The same pair, but the host's page can't search Ranked now (no pick-ranked in its caps): the
#    host's Ranked pick keeps the friend. The host prints `orca menu online ranked kept` and no
#    `left lobby`, the friend hears nothing, and the page's `queue-cancel` changes nothing; after
#    8 s both still play together. Then the friend's app leaves: the session ends with matched
#    checksums, the host arms the kept pick (`orca menu online ranked`, then `orca queue ready
#    ranked` after Start), and the friend goes home there and takes the pick too.
#   --only rooms|page|home|lobby|guest|rejoin|stay[,...]: those steps alone (rooms: 1-4, page: 5,
#   home: 6, lobby: 7, guest: 8, rejoin: 9, stay: 10).
# Each step waits for the lines it expects and fails on a timeout. Runs two Orca processes at most.
import json
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
import urllib.request

if len(sys.argv) > 1 and sys.argv[1] == "--doubles":
    import runpy
    doubles = os.path.join(os.path.dirname(os.path.abspath(__file__)), "doubles-rooms.py")
    sys.argv = [doubles, *sys.argv[2:]]
    runpy.run_path(doubles, run_name="__main__")
    sys.exit(0)

if len(sys.argv) < 3:
    sys.exit("usage: dropin-rooms.py <nogui> <disc> [--only rooms,page,home,lobby,guest,rejoin,stay]")
BIN, DISC = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "bf-mario-link-results.txt")
ONLY = set(sys.argv[sys.argv.index("--only") + 1].split(",")) if "--only" in sys.argv else None


def run_step(name):
    return ONLY is None or name in ONLY


def code(prefix):
    return prefix + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))


WORK = tempfile.mkdtemp(prefix="orca-rooms-")
T0 = time.time()


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra, script=True):
        self.name = name
        self.lines = queue.Queue()
        self.seen = []
        user = os.path.join(WORK, f"user-{name}")
        os.makedirs(user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_ROOM_DROP": "1",
            "ORCA_TEST_DEV_GAME": "orca-dropin-test",
            "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
            "ORCA_NAME": name,
        }
        # Without the input script, the pad is the app's controller stream (the bridge's live pad).
        if script:
            env["YG_INPUT"] = INPUT
        # A local rooms stack (ORCA_SITE) and the fresh start's knobs (ORCA.md "Fresh starts").
        for key in ("ORCA_SITE", "ORCA_TEST_FRESH", "ORCA_TEST_FRESH_AFTER"):
            if os.environ.get(key):
                env[key] = os.environ[key]
        env.update(env_extra)
        args = ["nice", "-n", "10", BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output", "-C", "Dolphin.Input.BackgroundInput=True",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Options.Verbosity=3", "-e", DISC]
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
            log(f"{self.name}> {line}")
            self.seen.append(line)
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

    def wait_log(self, pattern, timeout, after=0, poll=0.1):
        """Waits for a line of the Dolphin log (stderr) matching `pattern`, past offset `after`."""
        end = time.time() + timeout
        while time.time() < end:
            with open(self.log_path, errors="replace") as f:
                f.seek(after)
                m = re.search(pattern, f.read())
            if m:
                return m
            time.sleep(poll)
        fail(f"{self.name}: no log line like '{pattern}' in {timeout} s")

    def log_size(self):
        return os.path.getsize(self.log_path)

    def home_or_no_room(self, timeout, after=0, since=0):
        """A former joiner after its host went: home at once (it was on the menus: port 1, a room of
        its own) or "no-room" (in a fight, still on its port), past log offset `after` and stdout line
        `since`. Returns "home" or "no-room"."""
        end = time.time() + timeout
        while time.time() < end:
            if "orca state no-room" in self.seen[since:]:
                log(f"{self.name}: no-room (not on the menus)")
                return "no-room"
            with open(self.log_path, errors="replace") as f:
                f.seek(after)
                if "Drop-in: back to a game of its own on the menus" in f.read():
                    log(f"{self.name}: home (port 1, a room of its own)")
                    return "home"
            time.sleep(0.1)
        fail(f"{self.name}: neither home nor no-room in {timeout} s")

    def alive(self):
        if self.proc.poll() is not None:
            fail(f"{self.name} stopped (code {self.proc.returncode})")

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


log(f"work {WORK}")

if run_step("rooms"):
    # 1. Nobody's game in the room: the joiner gives up in seconds, not after the 90 s join limit.
    friend = Orca("Friend", {})
    orcas.append(friend)
    friend.expect("orca caps", 30)
    friend.send("caps join leave stats")
    friend.expect("orca state playing", 90)
    time.sleep(3)
    empty = code("emp")
    sent = time.time()
    friend.send(f"join {empty}")
    friend.expect("orca error peer_left", 30, forbid=("orca error network", "orca error internal"))
    log(f"gave up on an empty room after {time.time() - sent:.1f} s")
    if time.time() - sent > 20:
        fail("the friend waited too long in an empty room")
    friend.expect("orca state playing", 20)
    time.sleep(2)
    friend.alive()

    # 2. The friend gets to the host's room first: the host gives that room up and opens another.
    taken = code("tkn")
    mark = friend.log_size()
    friend.send(f"join {taken}")
    friend.wait_log(rf"in room {taken} on seat 1, joining", 30, after=mark)
    host_player = "orca-rooms-" + code("h")
    host = Orca("Host", {"ORCA_ROOM": taken, "ORCA_TEST_PLAYER": host_player})
    orcas.append(host)
    host.expect("orca caps", 30)
    host.send("caps join leave stats")
    host.expect("orca state playing", 90)
    line = host.expect("orca error network", 30)
    if "hosting this room" not in line:
        fail(f"the host's error doesn't say why: {line}")
    fresh = host.wait_log(r"Drop-in: hosting room ([a-z0-9]+) again", 30).group(1)
    if fresh == taken:
        fail("the host kept the room someone else hosts")
    log(f"the host now hosts {fresh}")
    friend.expect("orca error peer_left", 20, forbid=("orca error network", "orca error internal"))
    friend.expect("orca state playing", 20)

    # 3. A leave while catching up, then a join that plugs in.
    host.send("prepare-join")
    time.sleep(25)
    mark = friend.log_size()
    friend.send(f"join {fresh}")
    host.expect("orca state friend-joining", 60)
    # Catching up starts once the keyframe is loaded (the "joining" percent says 50 from the end of
    # the download until the last few seconds of catching up, so the log says when).
    friend.wait_log(r"Drop-in: loaded keyframe frame", 60, after=mark, poll=0.02)
    sent = time.time()
    since, after = len(friend.seen), friend.log_size()
    friend.send("leave")
    friend.expect("orca state left", 5)
    log(f"left while catching up after {time.time() - sent:.2f} s")
    friend.wait_log(r"Drop-in: left while catching up", 5, after=mark)
    host.expect("orca state friend-left left", 10,
                forbid=("orca error", "orca state friend-left stalled", "orca state friend-joined"))
    friend.home_or_no_room(10, after, since)
    time.sleep(2)
    friend.send(f"join {fresh}")
    host.expect("orca state friend-joining", 60)
    friend.expect("orca state playing", 120)
    host.expect("orca state friend-joined", 60)
    time.sleep(15)
    host.alive()
    friend.alive()

    # 4. The host's room socket reconnects while the session and room stay together.
    mark = host.log_size()
    friend_from = len(friend.seen)
    host_from = len(host.seen)
    host.send("test-drop-room")
    host.expect("orca state room-away self", 20)
    host.expect("orca state room-back self", 90)
    again = host.wait_log(r"in room ([a-z0-9]+) on seat", 10, after=mark).group(1)
    if again != fresh:
        fail(f"the host recovered {again}, not its room {fresh}")
    time.sleep(10)
    for peer, since in ((host, host_from), (friend, friend_from)):
        for line in peer.seen[since:]:
            if line.startswith(("orca error", "orca state host-left", "orca state no-room")):
                fail(f"{peer.name}: room-only recovery ended the game: {line}")
    for o in orcas:
        o.alive()
    sent = time.time()
    since, after = len(friend.seen), friend.log_size()
    host.send("leave")
    host.send("quit")
    friend.expect("orca state host-left", 10, forbid=("orca error internal",))
    took = time.time() - sent
    log(f"the friend heard the stopping host go after {took:.2f} s")
    if took > 2.5:
        fail(f"the stopping host's goodbye took {took:.1f} s to reach the friend")
    friend.home_or_no_room(10, after, since)
    if host.proc.wait(timeout=20) != 0:
        fail(f"the host exited with {host.proc.returncode}")
    friend.alive()
    for o in orcas:
        o.stop()
    orcas.clear()

# 5. The page's Leave through the app's bridge. Each Orca gets its own fake bridge minting dev
# tickets for the same dev game. The friend's bridge names the host's room, as an opened invite
# does; the host's sends Leave eight seconds after the room reads ready, once the friend is in.
paged = code("pge")
bridge_log = os.path.join(WORK, "bridge.log")
friend_bridge_log = os.path.join(WORK, "bridge-friend.log")
bridges = []
# Caps of a page with the queue's character select that can search both queues (signed in, both
# switches on).
CAPS_Q2 = "caps join leave stats host results queue2 pick-casual pick-ranked"
# The same page unable to search Ranked now (signed out, or orca_ranked off).
CAPS_Q2_NO_RANKED = "caps join leave stats host results queue2 pick-casual"
# GameCube adapter wire mask (fake_bridge.py).
WIRE = {"A": 1, "B": 2, "X": 4, "Y": 8, "LEFT": 16, "RIGHT": 32, "DOWN": 64, "UP": 128, "START": 256,
        "Z": 512}
# Presses from the menu, after a held B out of With Friends' character select, to With Anyone >
# Ranked. Before ux=18 that B lands on the With Friends page, one B short of the ONLINE page.
# UX_VERSION is this tree's kCompatVersion (DROPIN_UX=<n> for a binary built from another tree).
with open(os.path.join(HERE, "..", "..", "Source", "Core", "Core", "Orca", "UX", "UX.h")) as _f:
    UX_VERSION = int(os.environ.get("DROPIN_UX") or
                     re.search(r"kCompatVersion = (\d+);", _f.read()).group(1))
TO_RANKED = (["B"] if UX_VERSION < 18 else []) + ["RIGHT", "A", "DOWN", "A"]
CSS_SCENE = r"Scene frame \d+: \S* -> scSelctCharacter"
MENU_SCENE = r"Scene frame \d+: \S* -> muMenuMain"


def start_bridge(log_path, room=None, **extra):
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_bridge.py")], stdout=subprocess.PIPE,
                            text=True, env=dict(os.environ, FAKE_ROOM=room or paged, FAKE_LOG=log_path,
                                                **extra))
    bridges.append(proc)
    return proc.stdout.readline().strip()


def mirrors(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [dict(json.loads(l)["mpRoom"], t=json.loads(l)["t"]) for l in f if '"mpRoom"' in l]


def wait_mirror(path, test, timeout, why):
    """Waits for a mirror (mpRoom) the bridge logged that passes `test`."""
    end = time.time() + timeout
    while time.time() < end:
        for r in mirrors(path):
            if test(r):
                return r
        time.sleep(0.2)
    shown = ", ".join(f'{r["code"]}:{len(r["players"])}' for r in mirrors(path))
    fail(f"{why} in {timeout} s (mirrors: {shown})")


def pad(port, buttons=0, axes=None):
    """The app's controller (a bridge with FAKE_PAD_LIVE=1): held until the next call."""
    req = urllib.request.Request(f"http://127.0.0.1:{port}/test-pad",
                                 data=json.dumps({"buttons": buttons,
                                                  "axes": axes or [128, 128, 128, 128]}).encode(),
                                 headers={"Authorization": "Bearer t0ken",
                                          "content-type": "application/json"})
    urllib.request.urlopen(req, timeout=5).read()


def press(port, button, hold=0.15):
    pad(port, WIRE[button])
    time.sleep(hold)
    pad(port)
    time.sleep(0.15)


def walk(port, buttons, gap=0.8):
    """Presses on a menu page, a page's animation apart (online-menu.py's 50 frames)."""
    for b in buttons:
        press(port, b)
        time.sleep(gap)


def back_to_menu(o, port):
    """Held B out of a character select to the main menu, then time for its page to come in."""
    mark = o.log_size()
    pad(port, WIRE["B"])
    o.wait_log(MENU_SCENE, 20, after=mark)
    pad(port)
    time.sleep(3.5)


def pick_and_start(o, port, queue_name, mark):
    """On the queue's own character select (reached after `mark`): up into the grid, A on a
    character, Start: `orca queue ready <queue>` within 10 s."""
    o.wait_log(CSS_SCENE, 20, after=mark)
    time.sleep(2.0)
    pad(port, 0, [128, 255, 128, 128])
    time.sleep(0.65)
    pad(port)
    time.sleep(0.5)
    press(port, "A")
    time.sleep(1.0)
    sent = time.time()
    press(port, "START")
    line = o.expect(f"orca queue ready {queue_name}", 10)
    log(f"{o.name}: ready {time.time() - sent:.2f} s after Start: {line}")


def pair_on_with_friends(tag, caps=CAPS_Q2):
    """A host on its main menu and a friend joining it from the app's invite, both under bridges
    with live pads and the queue's caps (`caps`, both): both on With Friends' character select (the
    drop-in from the menus). Returns (host, host's bridge port, host's bridge log, friend, its port,
    its log, the room)."""
    room = code(tag)
    host_log = os.path.join(WORK, f"bridge-{tag}-host.log")
    friend_log = os.path.join(WORK, f"bridge-{tag}-friend.log")
    host_port = start_bridge(host_log, room, FAKE_NO_LEAVE="1", FAKE_PAD_LIVE="1")
    h = Orca(f"Host-{tag}", {"ORCA_ROOM": room, "YOUGAME_BRIDGE": f"http://127.0.0.1:{host_port}",
                             "YOUGAME_TOKEN": "t0ken", "YOUGAME_GAME": "orca-test", "YG_SCENES": "1"},
             script=False)
    orcas.append(h)
    h.expect("orca caps", 30)
    h.send(caps)
    h.expect("orca state playing", 90)
    wait_mirror(host_log, lambda r: r["code"] == room, 20, "the host's room never mirrored")
    friend_port = start_bridge(friend_log, room, FAKE_NO_LEAVE="1", FAKE_PAD_LIVE="1")
    f = Orca(f"Friend-{tag}", {"YOUGAME_BRIDGE": f"http://127.0.0.1:{friend_port}",
                               "YOUGAME_TOKEN": "t0ken", "YOUGAME_GAME": "orca-test",
                               "YG_SCENES": "1"},
             script=False)
    orcas.append(f)
    f.expect("orca caps", 30)
    f.send(caps)
    h.expect("orca state friend-joining", 90)
    f.expect("orca state playing", 120)
    h.expect("orca state friend-joined", 60)
    h.wait_log(CSS_SCENE, 90)
    time.sleep(3)
    return h, host_port, host_log, f, friend_port, friend_log, room


def came_home(f, friend_log, room, since, after):
    """The former joiner went home: port 1, never "no-room", and its page shown a room of its own."""
    if "orca state no-room" in f.seen[since:]:
        fail(f"{f.name} said no-room on the menus")
    f.wait_log(r"Drop-in: back to a game of its own on the menus", 1, after=after)
    f.wait_log(r"Drop-in: port 2 moves to port 1", 1, after=after)
    own = wait_mirror(friend_log, lambda r: r["code"] != room and r["players"], 20,
                      f"{f.name}'s page was never shown a room of its own")
    log(f"{f.name}'s own room: {own['code']}")
    return own["code"]


try:
    if run_step("page"):
        port = start_bridge(bridge_log, FAKE_LEAVE_AFTER="8")
        host = Orca("Host2", {"ORCA_ROOM": paged, "YOUGAME_BRIDGE": f"http://127.0.0.1:{port}",
                              "YOUGAME_TOKEN": "t0ken", "YOUGAME_GAME": "orca-test"})
        orcas.append(host)
        host.expect("orca caps", 30)
        host.send("caps join leave stats")
        host.expect("orca state playing", 90)
        end = time.time() + 20
        while not any(r["code"] == paged for r in mirrors(bridge_log)) and time.time() < end:
            time.sleep(0.2)
        friend_port = start_bridge(friend_bridge_log, FAKE_NO_LEAVE="1")
        friend = Orca("Friend2", {"YOUGAME_BRIDGE": f"http://127.0.0.1:{friend_port}",
                                  "YOUGAME_TOKEN": "t0ken", "YOUGAME_GAME": "orca-test"})
        orcas.append(friend)
        friend.expect("orca caps", 30)
        friend.send("caps join leave stats")
        host.expect("orca state friend-joining", 60)
        friend.expect("orca state playing", 120)
        since, after = len(friend.seen), friend.log_size()
        host.expect("orca state left", 30, forbid=("orca error",))
        friend.expect("orca state host-left", 30, forbid=("orca error",))
        how = friend.home_or_no_room(10, after, since)
        no_room_at = time.time()
        time.sleep(3)
        friend_rooms = mirrors(friend_bridge_log)
        shown = ", ".join(f'{r["code"]}:{len(r["players"])}@{r["t"] - no_room_at:+.2f}s'
                          for r in friend_rooms)
        log(f"friend's mirrors (time from {how}): {shown}")
        if how == "no-room":
            if any(r["code"] == paged and r["players"] and r["t"] >= no_room_at for r in friend_rooms):
                fail("the friend's page was shown the old room after no-room")
            if not any(r["code"] == paged and not r["players"] and r["t"] < no_room_at
                       for r in friend_rooms):
                fail("the friend's page wasn't told the old room was empty before no-room")
        else:
            # Home: the old room emptied and never shown again, then the friend's own room.
            emptied = next((i for i, r in enumerate(friend_rooms)
                            if r["code"] == paged and not r["players"]), None)
            if emptied is None:
                fail("the friend's page wasn't told the old room was empty")
            if any(r["code"] == paged and r["players"] for r in friend_rooms[emptied:]):
                fail("the friend's page was shown the old room after it emptied")
            wait_mirror(friend_bridge_log, lambda r: r["code"] != paged and r["players"], 20,
                        "the friend's page was never shown a room of its own")
        # The page ends on the host's new room: the old one emptied first (or, had the new room been
        # quicker, never mirrored again after it).
        end = time.time() + 20
        rooms = []
        while time.time() < end:
            with open(bridge_log) as f:
                rooms = [json.loads(l)["mpRoom"] for l in f if '"mpRoom"' in l]
            if rooms and rooms[-1]["code"] != paged and rooms[-1]["players"]:
                break
            time.sleep(0.5)
        shown = ", ".join(f'{r["code"]}:{len(r["players"])}' for r in rooms)
        log(f"host's mirrors: {shown}")
        if not rooms or rooms[-1]["code"] == paged or not rooms[-1]["players"]:
            fail(f"the page doesn't end on the host's new room: {shown}")
        if not any(r["code"] == paged and len(r["players"]) >= 2 for r in rooms):
            fail("the page never saw the friend in the host's room")
        first_new = next(i for i, r in enumerate(rooms) if r["code"] != paged)
        if any(r["code"] == paged for r in rooms[first_new:]):
            fail(f"the old room was mirrored after the new one: {shown}")
        if not any(r["code"] == paged and not r["players"] for r in rooms):
            log("(the new room's mirror went out before the old one's end)")
        for o in orcas:
            o.alive()
        for o in orcas:
            o.stop()
        orcas.clear()

    if run_step("home"):
        # 6. The host's app stops it while the friend is on With Friends' character select.
        host, host_port, host_log, friend, friend_port, friend_log, room = pair_on_with_friends("hom")
        since, after = len(friend.seen), friend.log_size()
        host.send("leave")
        host.send("quit")
        friend.expect("orca state host-left", 15)
        if host.proc.wait(timeout=20) != 0:
            fail(f"the host exited with {host.proc.returncode}")
        time.sleep(2)
        # Back out (With Friends, then the ONLINE page), With Anyone > Ranked, a character, Start.
        back_to_menu(friend, friend_port)
        mark = friend.log_size()
        walk(friend_port, TO_RANKED)
        friend.expect("orca menu online ranked", 15)
        pick_and_start(friend, friend_port, "ranked", mark)
        came_home(friend, friend_log, room, since, after)
        friend.wait_log(r"Online rules: header ranked .*the queue's own character select", 1,
                        after=after)
        friend.alive()
        for o in orcas:
            o.stop()
        orcas.clear()

    def friend_took_the_pick(f, since, queue_name):
        """The friend's `orca menu online <queue>` came after its "host-left", never before (the
        reader never announces a pick made with friends in the game), and from its way home."""
        seen = f.seen[since:]
        menu = f"orca menu online {queue_name}"
        f.expect(menu, 10)
        seen = f.seen[since:]
        left = next((i for i, l in enumerate(seen) if l.startswith("orca state host-left")), None)
        first = next(i for i, l in enumerate(seen) if l.startswith("orca menu"))
        if left is None or first < left or not seen[first].startswith(menu):
            fail(f"{f.name}'s menu line isn't its way home's: {seen}")

    def lobby_pick(tag, presser):
        """Steps 7 and 8: the pair on With Friends' character select, the host backs out (the shared
        game to the menus), and `presser` ("host" or "friend") picks With Anyone > Ranked."""
        host, host_port, host_log, friend, friend_port, friend_log, room = pair_on_with_friends(tag)
        back_to_menu(host, host_port)
        mark = host.log_size()
        since, after = len(friend.seen), friend.log_size()
        walk(host_port if presser == "host" else friend_port, TO_RANKED)
        host.expect("orca state left lobby", 20)
        host.expect("orca menu online ranked", 5)
        friend.expect("orca state host-left", 15)
        host.wait_log(r"Online menu: ranked picked with friends in room", 1, after=mark)
        friend.home_or_no_room(15, after, since)
        came_home(friend, friend_log, room, since, after)
        friend_took_the_pick(friend, since, "ranked")
        friend.wait_log(r"Drop-in: home on the ranked character select", 1, after=after)
        # Neither backs out: each picks a character and presses Start where it landed.
        pick_and_start(host, host_port, "ranked", mark)
        host.wait_log(r"Online rules: header ranked .*the queue's own character select", 1,
                      after=mark)
        pick_and_start(friend, friend_port, "ranked", after)
        friend.wait_log(r"Online rules: header ranked .*the queue's own character select", 1,
                        after=after)
        # The host's own new room, with nobody else in it.
        wait_mirror(host_log, lambda r: r["code"] != room and r["players"], 20,
                    "the host's page was never shown its new room")
        for o in orcas:
            o.alive()
        for o in orcas:
            o.stop()
        orcas.clear()

    if run_step("lobby"):
        # 7. The host picks With Anyone > Ranked with the friend still in its game.
        lobby_pick("lob", "host")

    if run_step("guest"):
        # 8. The friend picks it.
        lobby_pick("gst", "friend")

    if run_step("stay"):
        # 10. The host picks With Anyone > Ranked with the friend in; its page can't search Ranked.
        host, host_port, host_log, friend, friend_port, friend_log, room = pair_on_with_friends(
            "sty", CAPS_Q2_NO_RANKED)
        back_to_menu(host, host_port)
        mark = host.log_size()
        hsince = len(host.seen)
        since, after = len(friend.seen), friend.log_size()
        walk(host_port, TO_RANKED)
        kept = host.expect("orca menu online ranked", 20, forbid=("orca error", "orca state left"))
        if kept != "orca menu online ranked kept":
            fail(f"the host's pick wasn't marked kept: {kept}")
        host.wait_log(r"Online menu: ranked picked with friends in room \S+, which the page can't "
                      r"search now \(no pick-ranked\): staying with them", 1, after=mark)
        host.wait_log(CSS_SCENE, 10, after=mark)
        # The page answers a pick it can't search with `queue-cancel`.
        host.send("queue-cancel")
        time.sleep(8)
        for o, start, bad in ((host, hsince, ("orca state left", "orca state friend-left",
                                              "orca error", "orca queue", "orca menu")),
                              (friend, since, ("orca state host-left", "orca menu", "orca error",
                                               "orca queue"))):
            o.alive()
            hit = [l for l in o.seen[start:] if l.startswith(bad) and l != kept]
            if hit:
                fail(f"{o.name} after the unsearchable pick: {hit}")
        with open(host.log_path, errors="replace") as f:
            f.seek(mark)
            text = f.read()
        if re.search(r"Online rules: header ranked|Queue: on the ranked queue's own character select",
                     text):
            fail("the host armed the queue with its friend in the game")
        log("the host kept its friend: no left lobby, nothing armed, the session plays on")
        # The friend's app leaves: the session ends with matched checksums, and the host, now alone
        # on the RANKED select its kept pick opened, arms that pick.
        fafter = friend.log_size()
        hafter = host.log_size()
        hleft = len(host.seen)
        friend.send("leave")
        friend.expect("orca state left", 15)
        host.expect("orca state friend-left", 15)
        solo = (r"Drop-in: back to solo play at frame (\d+) \(([^)]*)\): (\d+) rollbacks, "
                r"(\d+) checksums matched")
        m = host.wait_log(solo, 20, after=hafter)
        log(f"{host.name}'s session ended at frame {m.group(1)} ({m.group(2)}): "
            f"{m.group(4)} checksums matched")
        if int(m.group(4)) <= 0:
            fail(f"{host.name}'s session matched no checksums")
        end = time.time() + 15
        while "orca menu online ranked" not in host.seen[hleft:] and time.time() < end:
            time.sleep(0.1)
        menus = [l for l in host.seen[hleft:] if l.startswith("orca menu")]
        if menus != ["orca menu online ranked"]:
            fail(f"the host's kept pick wasn't armed once: {menus}")
        host.wait_log(r"Online menu: the ranked pick kept with friends, alone now", 1, after=hafter)
        pick_and_start(host, host_port, "ranked", mark)
        host.wait_log(r"Online rules: header ranked .*the queue's own character select", 1,
                      after=hafter)
        # The friend goes home on that RANKED select and takes the pick too, whatever its page can
        # search.
        friend.home_or_no_room(15, fafter, since)
        friend.expect("orca menu online ranked", 10)
        friend.wait_log(r"Drop-in: home on the ranked character select", 1, after=fafter)
        pick_and_start(friend, friend_port, "ranked", after)
        for o in orcas:
            o.alive()
        for o in orcas:
            o.stop()
        orcas.clear()

    if run_step("rejoin"):
        # 9. The host's app stops it on With Friends' character select; the friend goes home there.
        host, host_port, host_log, friend, friend_port, friend_log, room = pair_on_with_friends("rej")
        since, after = len(friend.seen), friend.log_size()
        host.send("leave")
        host.send("quit")
        friend.expect("orca state host-left", 15)
        if host.proc.wait(timeout=20) != 0:
            fail(f"the host exited with {host.proc.returncode}")
        if friend.home_or_no_room(15, after, since) != "home":
            fail("the friend didn't go home on With Friends' character select")
        own = came_home(friend, friend_log, room, since, after)
        # A new friend joins that room from the app's invite.
        joiner_log = os.path.join(WORK, "bridge-rej-joiner.log")
        joiner_port = start_bridge(joiner_log, own, FAKE_NO_LEAVE="1", FAKE_PAD_LIVE="1")
        joiner = Orca("Joiner-rej", {"YOUGAME_BRIDGE": f"http://127.0.0.1:{joiner_port}",
                                     "YOUGAME_TOKEN": "t0ken", "YOUGAME_GAME": "orca-test",
                                     "YG_SCENES": "1"},
                      script=False)
        orcas.append(joiner)
        joiner.expect("orca caps", 30)
        joiner.send(CAPS_Q2)
        friend.expect("orca state friend-joining", 90)
        joiner.expect("orca state playing", 120)
        friend.expect("orca state friend-joined", 60)
        fafter, jafter = friend.log_size(), joiner.log_size()
        # Both play: each moves its hand about the character select now and then.
        end = time.time() + 15
        while time.time() < end:
            for p in (friend_port, joiner_port):
                pad(p, 0, [128, random.choice([40, 128, 215]), random.choice([40, 128, 215]), 128])
            time.sleep(0.4)
        for p in (friend_port, joiner_port):
            pad(p)
        time.sleep(1)
        joiner.send("leave")
        joiner.expect("orca state left", 20)
        friend.expect("orca state friend-left", 20)
        solo = (r"Drop-in: back to solo play at frame (\d+) \(([^)]*)\): (\d+) rollbacks, "
                r"(\d+) checksums matched")
        m = friend.wait_log(solo, 20, after=fafter)
        log(f"{friend.name}'s session as host ended at frame {m.group(1)} ({m.group(2)}): "
            f"{m.group(3)} rollbacks, {m.group(4)} checksums matched")
        if int(m.group(4)) <= 0:
            fail(f"{friend.name}'s session matched no checksums")
        m = joiner.wait_log(solo, 20, after=jafter)
        log(f"{joiner.name}'s session ended at frame {m.group(1)} ({m.group(2)}): "
            f"{m.group(4)} checksums matched")
        if int(m.group(4)) <= 0:
            fail(f"{joiner.name}'s session matched no checksums")
        for o in orcas:
            if o is not host:
                o.alive()
        for o in orcas:
            o.stop()
        orcas.clear()
finally:
    for o in orcas:
        if o.proc.poll() is None:
            o.proc.kill()
    for b in bridges:
        b.kill()
log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
