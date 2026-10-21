# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A stand-in for the YouGame desktop app's loopback bridge, for testing Orca's room client:
# POST /sdk (hello, mpTicket via an anonymous dev ticket, mpRoom recorded), GET /events (SSE: the
# page's Leave two seconds after the room reads ready). Prints its port. Usage:
#   FAKE_ROOM=orcabr123 FAKE_LOG=/tmp/bridge.log python3 Tools/orca/fake_bridge.py &
#   ORCA_LIVE_TEST=1 ORCA_TEST_BRIDGE_ROOM=orcabr123 YOUGAME_BRIDGE=http://127.0.0.1:<port> \
#     YOUGAME_TOKEN=t0ken YOUGAME_GAME=orca-test ./Binaries/Tests/tests \
#     --gtest_filter='OrcaLive.BridgeMirrorsTheRoomAndTakesLeave'
# Run that test alone: with a bridge configured, every room goes through it.
#
# GET /controllers/events: the app's controller stream (desktop/src/controllers.ts snapshots) with
# one GameCube adapter and a controller in port 1. Settings:
#   FAKE_PAD_HZ          report rate (default 125, the official adapter; 1000 when overclocked)
#   FAKE_PAD_NOISE=1     nudge the main stick's X on every report, so every report is a new pad
#                        (for Orca's input-age test, OrcaUXLatency.BoundaryInputAge)
#   FAKE_HELPER_SKEW_MS, FAKE_HELPER_DRIFT_PPM  offset and drift of the fake helper's clock
#   FAKE_PAD_SCRIPT      JSON lines of {"t": <s since the stream opened>, "buttons": <wire mask>,
#                        "axes": [x, y, cx, cy], "triggers": [l, r]}, each held until the next;
#                        without it, A is tapped every 2 s
#   FAKE_PAD_LOOP        seconds after which the script starts over (default never)
#   FAKE_PAD_GAMEPAD=1   send the script as a standard gamepad (W3C button indices) instead
#   FAKE_PAD_LIVE=1      no script: the pad is neutral until a test sets it with POST /test-pad
#                        {"buttons": <wire mask>, "axes": [x, y, cx, cy]} (authed); each holds
#                        until the next
# Wire mask: A 1, B 2, X 4, Y 8, Left 16, Right 32, Down 64, Up 128, Start 256, Z 512, R 1024, L 2048.
import json, os, socket, sys, threading, time, urllib.request, random, string, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOM = os.environ.get("FAKE_ROOM"); TOKEN = os.environ.get("FAKE_TOKEN", "t0ken")
SLUG = os.environ.get("FAKE_SLUG", "orca-test"); LOG = os.environ.get("FAKE_LOG", os.devnull)
# FAKE_COOKIE (a signed-in session's cookie header) and FAKE_TICKET_SLUG (a live Orca listing): real
# tickets, as the app gets them. FAKE_NO_LEAVE=1: the page never sends Leave.
COOKIE = os.environ.get("FAKE_COOKIE", ""); TICKET_SLUG = os.environ.get("FAKE_TICKET_SLUG", "")
# FAKE_ORCA_BUILD: the "orca build <version> [<commit>]" the page names in a real ticket (the site's
# protocol floor refuses a listing's Orca ticket with no build, 409 orca_outdated; lib/orca-protocol.ts).
ORCA_BUILD = os.environ.get("FAKE_ORCA_BUILD", "0.3.34").split()
SITE = os.environ.get("FAKE_SITE", "https://yougame.co"); NO_LEAVE = os.environ.get("FAKE_NO_LEAVE") == "1"
# FAKE_TICKET_DROP_FIRST=1: the first mpTicket gets no reply (the connection just closes), as when
# the page's ticket never comes back.
DROP_FIRST = os.environ.get("FAKE_TICKET_DROP_FIRST") == "1"
# FAKE_LEAVE_AFTER: seconds from the room reading ready to the page's Leave (default 2).
LEAVE_AFTER = float(os.environ.get("FAKE_LEAVE_AFTER", "2"))
PAD_HZ = float(os.environ.get("FAKE_PAD_HZ", "125")); PAD_NOISE = os.environ.get("FAKE_PAD_NOISE") == "1"
HELPER_SKEW_MS = float(os.environ.get("FAKE_HELPER_SKEW_MS", "0"))
HELPER_DRIFT = float(os.environ.get("FAKE_HELPER_DRIFT_PPM", "0")) * 1e-6; STARTED = time.time()

def helper_ms():
    t = time.time()
    return t * 1000 + HELPER_SKEW_MS + (t - STARTED) * 1000 * HELPER_DRIFT

PAD_LIVE = os.environ.get("FAKE_PAD_LIVE") == "1"
live_pad = {"buttons": 0, "axes": [128, 128, 128, 128]}

def pad_script():
    if PAD_LIVE:
        return [{"t": 0, "buttons": 0}], 0.0
    path = os.environ.get("FAKE_PAD_SCRIPT")
    if not path:
        return [{"t": 0, "buttons": 0}, {"t": 1.9, "buttons": 1}, {"t": 2.0, "buttons": 0}], 2.0
    with open(path) as f:
        steps = [json.loads(l) for l in f if l.strip()]
    loop = float(os.environ.get("FAKE_PAD_LOOP", "0"))
    return sorted(steps, key=lambda s: s["t"]), loop

def snapshot(session, seq, step, received):
    now = helper_ms()
    axes = list(step.get("axes", [128, 128, 128, 128])); triggers = step.get("triggers", [0, 0])
    if PAD_NOISE: axes[0] = (axes[0] + seq) % 256
    if os.environ.get("FAKE_PAD_GAMEPAD") == "1":
        mask = step.get("buttons", 0)
        pad = {"index": 0, "id": "Fake Gamepad (045e:0b13)", "connected": True, "mapping": "standard",
               "axes": [(a - 128) / 127 for a in axes],
               "buttons": [{"value": 1 if mask >> i & 1 else 0, "pressed": bool(mask >> i & 1)} for i in range(17)],
               "timestamp": now}
        return {"schema": 1, "session": session, "sequence": seq, "sentAt": now, "receivedAt": 0,
                "source": "native", "state": "ready", "message": "", "owned": False, "suspended": False,
                "ports": [], "pads": [pad]}
    ports = [{"adapterId": session + "-gc", "port": i, "seat": i, "connected": i == 0,
              "type": "wired" if i == 0 else None, "buttons": step.get("buttons", 0) if i == 0 else 0,
              "axes": axes if i == 0 else [128] * 4, "triggers": triggers if i == 0 else [0, 0],
              "origin": [128, 128, 128, 128, 0, 0], "calibrationRevision": 0} for i in range(4)]
    return {"schema": 1, "session": session, "sequence": seq, "sentAt": now, "receivedAt": received,
            "source": "native", "state": "connected", "message": "", "owned": True, "suspended": False,
            "ports": ports, "pads": []}

controller_stream = {"open": False}
state = {"leave_at": None, "rooms": 0, "left": False}
lock = threading.Lock()

def log(obj):
    # "t": when the bridge took it (time.time()), so a test can order it against Orca's output.
    with open(LOG, "a") as f: f.write(json.dumps(dict(obj, t=time.time())) + "\n")

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def authed(self):
        return self.headers.get("Authorization") == "Bearer " + TOKEN
    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        if self.path == "/test-pad" and self.authed() and PAD_LIVE:
            msg = json.loads(self.rfile.read(int(self.headers["content-length"])))
            with lock:
                live_pad["buttons"] = int(msg.get("buttons", 0))
                live_pad["axes"] = list(msg.get("axes", [128, 128, 128, 128]))
            return self.reply(200, {"ok": True})
        if self.path != "/sdk" or not self.authed(): return self.reply(401, {"ok": False, "error": "no"})
        msg = json.loads(self.rfile.read(int(self.headers["content-length"])))
        t = msg.get("type")
        if t == "controllers":
            log({"controllers": msg})
            return self.reply(200, {"ok": True, "result": None})
        if t == "hello": return self.reply(200, {"ok": True, "result": {"room": ROOM}})
        if t == "mpTicket":
            with lock:
                drop = DROP_FIRST and not state.get("dropped")
                state["dropped"] = True
            if drop:
                log({"ticket_dropped": msg["room"]})
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                return
            pid = "orcabr-" + "".join(random.choice(string.ascii_lowercase) for _ in range(12))
            headers = {"content-type": "application/json", "user-agent": "Mozilla/5.0 (Macintosh) OrcaTest"}
            if COOKIE:
                # A signed-in player's ticket for a real listing (the keyframe store refuses dev tickets).
                body = {"slug": TICKET_SLUG, "player_id": pid, "players": msg["players"], "mode": msg["mode"],
                        "room": msg["room"], "lobby": msg["lobby"],
                        "orca_build": {"version": ORCA_BUILD[0], "commit": ORCA_BUILD[1] if len(ORCA_BUILD) > 1 else None}}
                headers["cookie"] = COOKIE
                headers["origin"] = SITE
            else:
                body = {"dev": "orca-bridge-test", "player_id": pid, "name": "Bridge", "players": msg["players"],
                        "mode": msg["mode"], "room": msg["room"], "lobby": msg["lobby"]}
            req = urllib.request.Request(SITE + "/api/multiplayer/ticket", data=json.dumps(body).encode(),
                headers=headers)
            res = json.loads(urllib.request.urlopen(req, timeout=15).read())
            log({"ticket_for": msg["room"], "mode": msg["mode"]})
            return self.reply(200, {"ok": True, "result": res})
        if t == "mpRoom":
            log({"mpRoom": msg})
            with lock:
                state["rooms"] += 1
                if msg.get("ready") and state["leave_at"] is None and not NO_LEAVE:
                    state["leave_at"] = time.time() + LEAVE_AFTER
            return self.reply(200, {"ok": True, "result": {}})
        return self.reply(200, {"ok": False, "error": "unknown type " + str(t)})
    def controllers(self):
        with lock:
            if controller_stream["open"]:
                return self.reply(409, {"ok": False, "error": "This game's controller stream is already attached."})
            controller_stream["open"] = True
        try:
            self.send_response(200); self.send_header("content-type", "text/event-stream"); self.end_headers()
            steps, loop = pad_script(); session = str(uuid.uuid4()); start = time.time(); seq = 0
            log({"controllers": "attached"})
            # Each report on a fixed schedule (a late one doesn't push the rest back), like the
            # adapter's USB polls; after a long stall the schedule starts again from now.
            period = 1 / PAD_HZ; due = time.time()
            while True:
                t = time.time() - start
                if loop > 0: t %= loop
                step = steps[0]
                for s in steps:
                    if s["t"] <= t: step = s
                if PAD_LIVE:
                    with lock: step = dict(live_pad)
                seq += 1
                event = {"type": "controllers", "snapshot": snapshot(session, seq, step, helper_ms())}
                self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode()); self.wfile.flush()
                due += period
                if due < time.time() - 4 * period: due = time.time()
                time.sleep(max(0, due - time.time()))
        except Exception:
            return
        finally:
            with lock: controller_stream["open"] = False
            log({"controllers": "detached"})
    def do_GET(self):
        if self.path == "/controllers/events" and self.authed(): return self.controllers()
        if self.path != "/events" or not self.authed(): return self.reply(401, {"ok": False})
        self.send_response(200); self.send_header("content-type", "text/event-stream"); self.end_headers()
        self.wfile.write(b": connected\n\n"); self.wfile.flush()
        while True:
            time.sleep(0.2)
            with lock:
                # Once per bridge, as the page sends it: a room opened after the leave (its own event
                # stream) doesn't hear it again.
                at = state["leave_at"] if not state["left"] else None
                if at and time.time() >= at:
                    state["left"] = True
            try:
                if at and time.time() >= at:
                    self.wfile.write(("data: " + json.dumps({"slug": "someone-else", "cmd": "leave"}) + "\n\n").encode())
                    self.wfile.write(("data: " + json.dumps({"yg": 1, "slug": SLUG, "cmd": "leave"}) + "\n\n").encode())
                    self.wfile.flush(); log({"sent": "leave"})
                else:
                    self.wfile.write(b": ping\n\n"); self.wfile.flush()
            except Exception:
                return

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
