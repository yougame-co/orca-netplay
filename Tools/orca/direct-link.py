#!/usr/bin/env python3
# direct-link.py: two Orcas on this machine over a real YouGame dev room, checking the direct link
# beside the relay (ORCA.md "Direct links"), headless and muted.
#
#   Tools/orca/direct-link.py <dolphin-emu-nogui> <disc> [--both K=V ...] [--host K=V ...]
#                             [--friend K=V ...] [--expect relay|direct|turn] [--quick]
#                             [--ticket-direct off]
#
# 1. Host boots solo in a room of its own; Friend joins it (drop-in) through the app's commands.
# 2. Both report the expected path in "orca stats" ("tx": 0 relay, 1 direct, 2 TURN) within 30 s,
#    and the log says which pair the link took.
# 3. Both start a match (the input script); 20 s into it, unless --quick:
#    - "test-direct off" on the host: both fall back to the relay at once, then "on": back;
#    - "test-direct in" on the host: only the host's inputs still go direct, then "on";
#    - three "test-direct rebuild": a new link each time, nothing refused;
#    - the player's switch on the host ("direct off", with "caps ... direct"): both on the relay at
#      once, then "direct on": back, the host's links starting over while the friend's carry on.
# 4. Another 20 s, then both quit. PASS needs no error and no desync, checksums matched, and the
#    expected path for most of the match.
# --both/--host/--friend set environment variables (ORCA_DIRECT_FORCE=turn, ORCA_DIRECT_LOSS=20,
# ORCA_TEST_NET_DELAY_MS=30, ORCA_DIRECT=0 ...). --ticket-direct off: both Orcas get their tickets
# through a proxy on this machine (ORCA_SITE) that adds YouGame's `"direct": false` to each reply,
# as the site's remote switch does: no link is ever tried (implies --expect relay; the log must
# say why and show no link). Runs two Orca processes.
import http.server
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
import urllib.error
import urllib.request

args = sys.argv[1:]
if len(args) < 2:
    sys.exit("usage: direct-link.py <nogui> <disc> [--both K=V] [--host K=V] [--friend K=V] "
             "[--expect relay|direct|turn] [--quick]")
BIN, DISC = args[0], args[1]
ENV = {"both": {}, "host": {}, "friend": {}}
EXPECT = "direct"
QUICK = False
TICKET_DIRECT = None
i = 2
while i < len(args):
    if args[i] in ("--both", "--host", "--friend"):
        k, _, v = args[i + 1].partition("=")
        ENV[args[i][2:]][k] = v
        i += 2
    elif args[i] == "--expect":
        EXPECT = args[i + 1]
        i += 2
    elif args[i] == "--quick":
        QUICK = True
        i += 1
    elif args[i] == "--ticket-direct":
        TICKET_DIRECT = args[i + 1] != "off"
        EXPECT = "direct" if TICKET_DIRECT else "relay"
        i += 2
    else:
        sys.exit(f"unknown argument {args[i]}")
TX = {"relay": 0, "direct": 1, "turn": 2}[EXPECT]
HERE = os.path.dirname(os.path.abspath(__file__))
# Mario vs Link on Battlefield, both players fuzzed: the friend plugs in at the title screen,
# before the picks.
INPUT = os.path.join(HERE, "inputs", "bf-mario-link-results.txt")
ROOM = "dl" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(7))
WORK = tempfile.mkdtemp(prefix="orca-direct-")
T0 = time.time()


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


SITE = "https://yougame.co"


class TicketProxy(http.server.BaseHTTPRequestHandler):
    """The site, as the Orcas see it with --ticket-direct: each request goes to yougame.co, and a
    ticket reply gets YouGame's switch for direct links."""

    def log_message(self, *a):
        pass

    def forward(self, body=None):
        req = urllib.request.Request(SITE + self.path, data=body, method=self.command, headers={
            "content-type": self.headers.get("content-type", "application/json"),
            "user-agent": "Mozilla/5.0 (Macintosh) OrcaTest"})
        try:
            res = urllib.request.urlopen(req, timeout=15)
            code, data = res.status, res.read()
        except urllib.error.HTTPError as e:
            code, data = e.code, e.read()
        if self.path.startswith("/api/multiplayer/ticket") and code == 200:
            reply = json.loads(data)
            reply["direct"] = TICKET_DIRECT
            data = json.dumps(reply).encode()
            log(f"proxy: a ticket with direct {TICKET_DIRECT}")
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.forward()

    def do_POST(self):
        self.forward(self.rfile.read(int(self.headers.get("content-length", 0))))


if TICKET_DIRECT is not None:
    proxy = http.server.ThreadingHTTPServer(("127.0.0.1", 0), TicketProxy)
    threading.Thread(target=proxy.serve_forever, daemon=True).start()
    ENV["both"]["ORCA_SITE"] = f"http://127.0.0.1:{proxy.server_address[1]}"


class Orca:
    def __init__(self, name, env_extra):
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
            "ORCA_TEST_DEV_GAME": "orca-dropin-test",
            "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
            "ORCA_NAME": name,
            "YG_INPUT": INPUT,
            "YG_SCENES": "1",
        }
        env.update(ENV["both"])
        env.update(env_extra)
        argv = [BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Logs.CORE=True", "-C", "Logger.Options.Verbosity=2", "-e", DISC]
        self.err = open(os.path.join(WORK, f"{name}.log"), "w")
        self.proc = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                try:
                    st = json.loads(line[len("orca stats "):])
                    st["t"] = time.time() - T0
                    self.stats.append(st)
                except ValueError:
                    log(f"{self.name}: unreadable stats line")
                continue
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

    def tx(self):
        return self.stats[-1].get("tx", 0) if self.stats else None

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


def wait_tx(want, timeout, who=None):
    """Waits until each Orca's newest stats line (a new one, after now) reports `want` (a dict
    name -> tx, or one tx for all); returns the seconds it took."""
    start = time.time()
    end = start + timeout
    while time.time() < end:
        for o in orcas:
            if o.proc.poll() is not None:
                fail(f"{o.name} exited")
        ok = True
        for o in orcas:
            w = want.get(o.name) if isinstance(want, dict) else want
            fresh = [s for s in o.stats if s["t"] > start - T0]
            if w is not None and (not fresh or fresh[-1].get("tx", 0) != w):
                ok = False
        if ok:
            return time.time() - start
        time.sleep(0.2)
    fail(f"no tx {want} in {timeout} s: " + ", ".join(f"{o.name} {o.tx()}" for o in orcas))


def check_alive():
    for o in orcas:
        if o.proc.poll() is not None:
            fail(f"{o.name} stopped")
        for line in o.log_lines("Desync"):
            fail(f"{o.name}: {line}")


log(f"room {ROOM}, work {WORK}, expecting {EXPECT}, env {ENV}")
host = Orca("Host", dict(ENV["host"], ORCA_ROOM=ROOM))
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats direct")
host.expect("orca state playing", 90)
time.sleep(15)
friend = Orca("Friend", ENV["friend"])
orcas.append(friend)
friend.send(f"join {ROOM}")
friend.expect("orca caps", 30)
friend.send("caps join leave stats direct")
friend.expect("orca state joining", 60)
friend.expect("orca state playing", 120)
took = wait_tx(TX, 30)
log(f"both on {EXPECT} {took:.1f} s after the friend plugged in")
for o in orcas:
    for line in o.log_lines("Orca direct link: seat")[:3]:
        log(f"{o.name}: {line[line.find('Orca direct'):]}")

# A match together (the input script picks and starts one).
for o in orcas:
    log(f"{o.name}: {o.wait_log('-> scMelee', 240)[-60:]}")
match_start = time.time() - T0
time.sleep(20)
check_alive()
events = []
if not QUICK and TX != 0:
    # Datagrams blocked on the host, both ways: both on the relay at once; lifted: back.
    host.send("test-direct off")
    events.append(("off", time.time() - T0))
    log(f"relay after {wait_tx(0, 10):.1f} s")
    time.sleep(8)
    check_alive()
    host.send("test-direct on")
    events.append(("on", time.time() - T0))
    log(f"{EXPECT} again after {wait_tx(TX, 40):.1f} s")
    time.sleep(5)
    # Only the host's incoming datagrams dropped: the friend still gets the host's inputs direct.
    host.send("test-direct in")
    events.append(("in", time.time() - T0))
    log(f"one way after {wait_tx({'Host': 0, 'Friend': TX}, 10):.1f} s")
    time.sleep(8)
    check_alive()
    host.send("test-direct on")
    events.append(("on", time.time() - T0))
    wait_tx(TX, 40)
    # Three new links in a row.
    for n in range(3):
        time.sleep(3)
        (host if n % 2 == 0 else friend).send("test-direct rebuild")
        events.append((f"rebuild {n + 1}", time.time() - T0))
        time.sleep(1.5)
        log(f"rebuild {n + 1}: {EXPECT} after {wait_tx(TX, 30):.1f} s")
    check_alive()
    # The player's switch (the app's "direct off|on"): the host drops its links at once and takes
    # every input through the relay; on again, its links start over (generation 1) while the
    # friend's side still counts from the links above.
    time.sleep(3)
    host.send("direct off")
    events.append(("direct off", time.time() - T0))
    log(f"switch off: relay after {wait_tx(0, 10):.1f} s")
    time.sleep(8)
    check_alive()
    host.send("direct on")
    events.append(("direct on", time.time() - T0))
    log(f"switch on: {EXPECT} again after {wait_tx(TX, 40):.1f} s")
    check_alive()
time.sleep(20)
check_alive()
for o in orcas:
    o.stop()
check_alive_lines = {o.name: o.log_lines("Online match") for o in orcas}

ok = True
for o in orcas:
    text = open(os.path.join(WORK, f"{o.name}.log"), errors="replace").read()
    if "Desync" in text or "session failed" in text:
        log(f"{o.name}: desync or session failure in its log")
        ok = False
    ends = [l for l in check_alive_lines[o.name] if "checksums matched" in l]
    for line in ends[-2:]:
        log(f"{o.name}: {line[line.find('Online match'):]}")
    for line in o.log_lines("Orca direct link: seat"):
        log(f"{o.name}: {line[line.find('Orca direct'):]}")
    matched = max([int(l.split(" checksums matched")[0].split(", ")[-1]) for l in ends] or [0])
    if matched == 0:
        log(f"{o.name}: no checksums matched")
        ok = False
    # The match, second by second: on which path, the link's round trip, rollbacks and stalls.
    peers = [s for s in o.stats if s.get("peers", 0) > 0 and s["t"] >= match_start]
    by = {}
    for s in peers:
        by.setdefault(s.get("tx", 0), []).append(s)
    for k, rows in sorted(by.items()):
        lrtt = sorted(s.get("lrtt", -1) for s in rows if s.get("lrtt", -1) >= 0)
        ping = sorted(s.get("ping", -1) for s in rows if s.get("ping", -1) >= 0)
        log(f"{o.name}: tx {k}: {len(rows)} s, rollbacks/s {sum(s['rb'] for s in rows) / len(rows):.2f}"
            f", re-run frames/s {sum(s['rbf'] for s in rows) / len(rows):.2f}, deepest "
            f"{max(s['rbmax'] for s in rows)}, stalls {sum(s['st'] for s in rows)}, delay max "
            f"{max(s['delay'] for s in rows)}, link rtt median "
            f"{lrtt[len(lrtt) // 2] if lrtt else '-'} ms, relay ping median "
            f"{ping[len(ping) // 2] if ping else '-'} ms, fps min {min(s['fps'] for s in rows)}")
    main = max(by, key=lambda k: len(by[k])) if by else None
    if main != TX:
        log(f"{o.name}: mostly on tx {main}, expected {TX}")
        ok = False
if TICKET_DIRECT is False:
    # YouGame's switch: each Orca says why it has no links, and never tried one.
    for o in orcas:
        why = o.log_lines("direct links off (YouGame's ticket")
        tried = o.log_lines("Orca direct link: seat")
        log(f"{o.name}: {why[0][why[0].find('Orca room'):] if why else 'no switch line'}; "
            f"{len(tried)} link lines")
        if not why or tried:
            ok = False
log(f"events {events}")
log("PASS" if ok else "FAIL")
if ok and os.environ.get("ORCA_TEST_KEEP") != "1":
    shutil.rmtree(WORK, ignore_errors=True)
else:
    log(f"logs kept in {WORK}")
sys.exit(0 if ok else 1)
