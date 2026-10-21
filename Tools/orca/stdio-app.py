#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
#
# stdio-app.py: plays the YouGame desktop app's half of the stdio bridge (ORCA.md "The stdio
# bridge") against a real Orca process, the way fake_bridge.py plays the loopback bridge. It starts
# Orca with YOUGAME_STDIO=1 and ORCA_SESSION=1 (plus ORCA_TEST_COMMANDS=1 unless the arguments embed
# it), then on Orca's stdout answers `hello` (no invite: Orca hosts its own room), mints `mpTicket` as an anonymous dev ticket
# from the site (dev game orca-stdio-test: no account, no real player), records `mpRoom`, and on
# Orca's stdin streams controller snapshots (schema 1, one GameCube adapter, A tapped every 2 s) at
# --pad-hz. Once the room is mirrored it sends the page's Leave as a push, then `quit`.
#
#   python3 Tools/orca/stdio-app.py [--secs 25] [--pad-hz 125] [--log FILE] [--no-leave] \
#       -- <dolphin-emu-nogui> -p headless -v Null -u <user> -e <Brawl disc>
#
# It fails (exit 1) when Orca asks for anything but hello, mpTicket and mpRoom, sends an mpTicket
# field the app would drop or a ranked one, never mirrors the room, doesn't end the room on the
# page's Leave, doesn't say `orca alive` about once a second once it runs (the heartbeat the app's
# watchdog arms on: at least one every 3 s), or exits with a code other than 0. ORCA_NET and the rest of the environment pass
# through, so `ORCA_NET=internet` checks the sandbox's network mode too (no loopback, no LAN).
import json, os, random, string, subprocess, sys, threading, time, urllib.request, uuid

args = sys.argv[1:]
if "--" not in args:
    sys.exit(__doc__ if __doc__ else "usage: stdio-app.py [options] -- <orca> [args...]")
opts, orca = args[: args.index("--")], args[args.index("--") + 1 :]
def opt(name, default):
    return opts[opts.index(name) + 1] if name in opts else default
SECS = float(opt("--secs", "25")); PAD_HZ = float(opt("--pad-hz", "125")); LOG = opt("--log", None)
# --invite CODE: hello names a friend's room, so Orca joins it instead of hosting.
INVITE = opt("--invite", None)
LEAVE = "--no-leave" not in opts
SITE = os.environ.get("ORCA_SITE", "https://yougame.co"); SLUG = "orca-stdio-test"
TICKET_FIELDS = {"id", "type", "players", "mode", "ranked", "room", "lobby"}

start = time.time()
problems, rooms, requests = [], [], []
state = {"left_sent": False, "ended": False, "sandbox_line": None, "keyboard_line": False}
alive = []
out_lock = threading.Lock(); log_lock = threading.Lock()
logf = open(LOG, "w") if LOG else None

def log(source, text):
    line = f"{time.time() - start:8.3f} {source}> {text}"
    with log_lock:
        if logf: logf.write(line + "\n"); logf.flush()
        elif source != "orca-log": print(line, flush=True)

env = dict(os.environ, ORCA_SESSION="1", YOUGAME_STDIO="1", YOUGAME_GAME=SLUG,
           SDL_JOYSTICK_HIDAPI="0", SDL_HIDAPI_LIBUSB="0")
env.pop("YOUGAME_BRIDGE", None); env.pop("YOUGAME_TOKEN", None); env.pop("ORCA_TEST_DEV_GAME", None)
if "--embed" not in orca:
    env["ORCA_TEST_COMMANDS"] = "1"
proc = subprocess.Popen(orca, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
log("app", f"started {' '.join(orca)} (invite {INVITE}, ORCA_NET={env.get('ORCA_NET', '')})")

def send(line):
    with out_lock:
        try:
            proc.stdin.write((line + "\n").encode()); proc.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass

def answer(rid, ok, payload):
    send("yg " + json.dumps({"re": rid, "ok": ok, ("result" if ok else "error"): payload}))

def ticket(msg):
    body = {"dev": SLUG, "player_id": "orcast-" + "".join(random.choice(string.ascii_lowercase) for _ in range(12)),
            "name": "Stdio", "players": msg.get("players"), "mode": msg.get("mode"), "room": msg.get("room"),
            "lobby": msg.get("lobby")}
    req = urllib.request.Request(SITE + "/api/multiplayer/ticket", data=json.dumps(body).encode(),
        headers={"content-type": "application/json", "user-agent": "Mozilla/5.0 (Macintosh) OrcaTest"})
    try:
        return json.loads(urllib.request.urlopen(req, timeout=15).read())
    except urllib.error.HTTPError as e:
        problems.append(f"dev ticket refused: HTTP {e.code} {e.read()[:200]!r}")
        return None

def request(line):
    try:
        msg = json.loads(line[3:])
        rid, kind = msg["id"], msg["type"]
    except Exception:
        problems.append("not a request: " + line[:200]); return
    requests.append(kind)
    if kind == "hello":
        answer(rid, True, {"room": INVITE} if INVITE else {})
    elif kind == "mpTicket":
        extra = set(msg) - TICKET_FIELDS
        if extra: problems.append(f"mpTicket fields the app drops: {sorted(extra)}")
        if msg.get("ranked") is not False: problems.append("an mpTicket that isn't ranked: false")
        t = ticket(msg)
        if t: answer(rid, True, t)
        else: answer(rid, False, "no ticket")
    elif kind == "mpRoom":
        rooms.append(msg); answer(rid, True, {})
        if LEAVE and not state["left_sent"] and msg.get("code"):
            state["left_sent"] = True
            def leave():
                time.sleep(2)
                log("app", "the page's Leave")
                send("yg " + json.dumps({"push": {"slug": "another-game", "cmd": "leave"}}))
                send("yg " + json.dumps({"push": {"slug": SLUG, "cmd": "leave"}}))
            threading.Thread(target=leave, daemon=True).start()
    else:
        problems.append(f"asked for {kind}")
        answer(rid, False, "not allowed")

def read_stdout():
    for raw in proc.stdout:
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        if line.startswith("yg "):
            log("orca", line[:300]); request(line)
        elif line == "orca alive":
            alive.append(time.time())
        elif line.startswith("orca ") or line.startswith("exit ") or line.startswith("ready"):
            log("orca", line)
            if line.startswith("orca error"): problems.append(line)
        else:
            log("orca-log", line)
    log("app", "Orca's stdout closed")

def read_stderr():
    for raw in proc.stderr:
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        if "in the app's sandbox" in line: state["sandbox_line"] = line
        if "no keyboard of its own" in line: state["keyboard_line"] = True
        if "controller input age" in line or "Orca:" in line or "You left the room" in line:
            log("orca-err", line[:400])
        else:
            log("orca-log", line)

def pads():
    session = str(uuid.uuid4()); seq = 0; period = 1 / PAD_HZ; due = time.time()
    while proc.poll() is None:
        seq += 1; now = time.time() * 1000
        a = 1 if (time.time() - start) % 2.0 > 1.9 else 0
        ports = [{"adapterId": session + "-gc", "port": i, "seat": i, "connected": i == 0,
                  "type": "wired" if i == 0 else None, "buttons": a if i == 0 else 0,
                  "axes": [128, 128, 128, 128], "triggers": [0, 0], "origin": [128, 128, 128, 128, 0, 0],
                  "calibrationRevision": 0} for i in range(4)]
        snap = {"schema": 1, "session": session, "sequence": seq, "sentAt": now, "receivedAt": now,
                "source": "native", "state": "connected", "message": "", "owned": True, "suspended": False,
                "ports": ports, "pads": []}
        send("yg " + json.dumps({"push": {"type": "controllers", "snapshot": snap}}, separators=(",", ":")))
        due += period
        if due < time.time() - 4 * period: due = time.time()
        time.sleep(max(0, due - time.time()))

for f in (read_stdout, read_stderr, pads):
    threading.Thread(target=f, daemon=True).start()
deadline = time.time() + SECS
while proc.poll() is None and time.time() < deadline:
    time.sleep(0.1)
log("app", "quit")
send("quit")
try:
    code = proc.wait(timeout=30)
except subprocess.TimeoutExpired:
    proc.kill(); code = "killed after 30 s"
time.sleep(0.5)

code0 = next((r["code"] for r in rooms if r.get("code")), None)
if not code0: problems.append("the room was never mirrored with its code")
if LEAVE and code0 and not any(r.get("code") == code0 and r.get("players") == [] and r.get("participants") == [] for r in rooms):
    problems.append("the page never saw the room empty after its Leave")
if code != 0: problems.append(f"exit code {code}")
gaps = [b - a for a, b in zip(alive, alive[1:])]
if len(alive) < 3: problems.append(f"only {len(alive)} `orca alive` lines")
elif max(gaps) > 3: problems.append(f"`orca alive` went quiet for {max(gaps):.1f} s")
if os.environ.get("ORCA_NET") and not state["sandbox_line"]: problems.append("no 'in the app's sandbox' line")
summary = {"exit": code, "requests": {k: requests.count(k) for k in sorted(set(requests))},
           "mpRoom": len(rooms), "room": code0,
           "alive": len(alive), "alive_gap_max_s": round(max(gaps), 2) if gaps else None, "sandbox": state["sandbox_line"], "no_keyboard_note": state["keyboard_line"],
           "problems": problems}
log("app", json.dumps(summary))
print(("PASS " if not problems else "FAIL ") + json.dumps(summary), flush=True)
sys.exit(1 if problems else 0)
