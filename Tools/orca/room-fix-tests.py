#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the existing unit binary with loopback-only room/keyframe fixtures; no dependencies."""
import base64
import hashlib
import json
import os
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from tempfile import TemporaryDirectory
from urllib.parse import parse_qs, urlsplit

rooms = {}
lock = threading.RLock()


def roster(code, cfg, kind="welcome"):
    joining = cfg.get("joining", True)
    local, remote = (1, 0) if joining else (0, 1)
    result = {"t": kind, "me": "me", "host": "peer" if joining else "me",
            "lifecycle": "game", "revision": 1,
            "room": {"code": code, "queue": cfg.get("queue", "private"), "size": 2},
            "player": {"id": "peer"},
            "players": [{"id": "me", "name": "Me"}, {"id": "peer", "name": "Friend"}],
            "participants": [{"id": "me", "connectionId": "me", "slot": local, "name": "Me"},
                             {"id": "peer", "connectionId": "peer", "slot": remote, "name": "Friend"}]}
    if "recovery" in cfg:
        result["room"]["recovery"] = cfg["recovery"]
    if cfg.get("doubles"):
        result["room"]["doubles"] = True
        result["room"]["size"] = cfg.get("size", 4)
    if cfg.get("peers") == 3:
        for slot in (2, 3):
            result["players"].append({"id": f"peer{slot}", "name": f"Friend{slot}"})
            result["participants"].append({"id": f"peer{slot}", "connectionId": f"peer{slot}",
                                            "slot": slot, "name": f"Friend{slot}"})
    return result


def send(ws, value, opcode=1):
    payload = json.dumps(value).encode() if opcode == 1 else value
    header = bytes([0x80 | opcode])
    if len(payload) < 126:
        header += bytes([len(payload)])
    else:
        header += bytes([126]) + struct.pack("!H", len(payload))
    with ws[1]:
        ws[0].sendall(header + payload)


def peer_hello(cfg, peer="peer", acknowledged=False):
    data = {"k": "hello", "c": cfg.get("compatibility", "test"), "r": acknowledged,
            "h": cfg.get("host", cfg.get("joining", True)) and peer == "peer", "dl": int(cfg.get("direct", False))}
    if cfg.get("doubles") and not cfg.get("legacy"):
        slot = int(peer[-1]) if peer[-1].isdigit() else 1
        data.update(doubles=True, ct=f"{slot:02x}")
    return {"t": "msg", "from": peer, "d": data}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, value, status=200):
        data = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        if self.path == "/api/multiplayer/ticket":
            code = body["room"]
            with lock:
                cfg = rooms[code]
                cfg["tickets"] += 1
                ticket_number = cfg["tickets"]
                delay = cfg.get("ticket_delay", 0) if cfg["tickets"] > 1 else 0
                ticket_failed = cfg.get("ticket_fail_count", 0) > 0 and ticket_number > 1
                if ticket_failed:
                    cfg["ticket_fail_count"] -= 1
            if ticket_failed:
                self.reply({"error": "fixture ticket unavailable"}, 503)
                return
            if delay:
                time.sleep(delay)
            try:
                self.reply({"url": f"http://127.0.0.1:{self.server.server_port}",
                            "ticket": f"loopback-fixture-{ticket_number}", "direct": cfg.get("direct", False),
                            "ice": [{"urls": "stun:127.0.0.1:9"}],
                            **({"tourney": cfg["tourney"]} if "tourney" in cfg else {})})
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        code = self.path.split("/")[-1]
        if self.path.startswith("/configure/"):
            with lock:
                rooms[code] = dict(body, tickets=0, connects=0, probes=0, closes=0,
                                   messages=[], sockets=[], close_codes=[], ticket_uses=[])
            self.reply({})
        elif self.path.startswith("/action/"):
            with lock:
                cfg = rooms[code]
                ws = cfg["sockets"][-1] if cfg["sockets"] else None
                if "ticket_delay" in body:
                    cfg["ticket_delay"] = body["ticket_delay"]
                if "block_for" in body:
                    cfg["blocked_until"] = time.monotonic() + body["block_for"]
                if "upgrade_delay" in body:
                    cfg["upgrade_delay"] = body["upgrade_delay"]
                for name in ("refuse", "refuse_status", "refuse_count", "reject_first_ticket",
                             "ticket_fail_count"):
                    if name in body:
                        cfg[name] = body[name]
            if ws:
                try:
                    if "close" in body:
                        send(ws, struct.pack("!H", body["close"]), 8)
                    if body.get("close_empty"):
                        send(ws, b"", 8)
                    if "message" in body:
                        send(ws, body["message"])
                except OSError:
                    pass
            self.reply({})

    def do_GET(self):
        code = self.path.split("?")[0].split("/")[-1]
        if self.path.startswith("/state/"):
            with lock:
                cfg = rooms[code]
                self.reply({k: cfg[k] for k in ("tickets", "connects", "probes", "closes",
                                              "messages", "close_codes", "ticket_uses")})
            return
        with lock:
            cfg = rooms[code]
            probe = self.headers.get("Sec-WebSocket-Key") == "AAAAAAAAAAAAAAAAAAAAAA=="
            cfg["probes" if probe else "connects"] += 1
            attempt = cfg["connects"]
            refuse = cfg.get("refuse")
            refuse_count = cfg.get("refuse_count", 100)
            refuse_status = cfg.get("refuse_status", 409)
            ticket = parse_qs(urlsplit(self.path).query).get("ticket", [""])[0]
            ticket_number = int(ticket.rsplit("-", 1)[-1]) if ticket.startswith("loopback-fixture-") else 0
            cfg["ticket_uses"].append(ticket_number)
            if attempt > 1 and cfg.get("same_ticket") and ticket_number != 1:
                refuse = {"error": "This seat is in use. Join again in a moment.", "code": "seat_in_use"}
                refuse_count, refuse_status = attempt, 409
            if cfg.get("reject_first_ticket") and ticket_number == 1:
                refuse = {"error": "Bad or expired ticket"}
                refuse_count, refuse_status = attempt, 401
            delay = max(cfg.get("probe_delay" if probe else "upgrade_delay", 0),
                        cfg.get("blocked_until", 0) - time.monotonic())
        if delay:
            time.sleep(delay)
        if refuse and attempt <= refuse_count:
            self.reply(refuse if isinstance(refuse, dict) else {"error": refuse}, refuse_status)
            return
        self.send_response(101)
        ray = cfg.get("recovery_cf_ray", cfg.get("cf_ray")) if attempt > 1 else cfg.get("cf_ray")
        if ray is not None:
            self.send_header("cF-rAy", ray)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        accept = base64.b64encode(hashlib.sha1(
            (self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        ws = (self.connection, threading.Lock())
        with lock:
            cfg["sockets"].append(ws)
        try:
            welcome = roster(code, cfg)
            if attempt > 1 and cfg.get("recovery_identity"):
                welcome["me"] = cfg["recovery_identity"]
            send(ws, welcome)
            send(ws, peer_hello(cfg))
            if cfg.get("peers") == 3:
                for slot in (2, 3):
                    send(ws, peer_hello(cfg, f"peer{slot}"))
            if attempt > 1 and cfg.get("recovery_packet"):
                send(ws, cfg["recovery_packet"])
            while True:
                header = self.rfile.read(2)
                if len(header) != 2:
                    break
                opcode, length = header[0] & 15, header[1] & 127
                if length == 126:
                    length = struct.unpack("!H", self.rfile.read(2))[0]
                elif length == 127:
                    length = struct.unpack("!Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if header[1] & 128 else b""
                payload = self.rfile.read(length)
                if mask:
                    payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                if opcode == 8:
                    with lock:
                        cfg["closes"] += 1
                        cfg["close_codes"].append(struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0)
                    if cfg.get("close_delay"):
                        time.sleep(cfg["close_delay"])
                    send(ws, payload, 8)
                    break
                if opcode != 1:
                    continue
                message = json.loads(payload)
                with lock:
                    cfg["messages"].append(message)
                if message.get("d", {}).get("k") == "hello":
                    send(ws, peer_hello(cfg, message.get("to", "peer"), True))
                if message.get("t") == "ping":
                    send(ws, {"t": "pong", "at": message["at"]})
                if message.get("t") == "leave":
                    with lock:
                        cfg["left"] = True
        except (OSError, ValueError, struct.error):
            pass
        self.close_connection = True


class FixtureServer(ThreadingHTTPServer):
    def handle_error(self, request, address):
        if not isinstance(sys.exc_info()[1], OSError):
            super().handle_error(request, address)


def main():
    room_server = FixtureServer(("127.0.0.1", 0), Handler)
    room_server.daemon_threads = True
    threading.Thread(target=room_server.serve_forever, daemon=True).start()
    # Import only definitions; the fixture's normal CLI starts its server at module scope.
    scope = {"__name__": "room_fix_keyframes"}
    with open("Tools/orca/fake_keyframes.py") as source:
        exec(source.read().rsplit("ThreadingHTTPServer(", 1)[0], scope)
    keyframes = ThreadingHTTPServer(("127.0.0.1", 0), scope["Handler"])
    keyframes.daemon_threads = True
    threading.Thread(target=keyframes.serve_forever, daemon=True).start()
    with TemporaryDirectory(prefix="orca-room-fix-") as temp:
        # Explicit, minimal test environment; no inherited app credentials or live-test opt-ins.
        test_env = {"PATH": os.environ.get("PATH", "") if os.name == "nt" else
                    "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin", "TMPDIR": temp,
                    "ORCA_TEST_ROOM_SERVER": f"http://127.0.0.1:{room_server.server_port}",
                    "ORCA_TEST_KEYFRAME_SERVER": f"http://127.0.0.1:{keyframes.server_port}",
                    "ORCA_NO_LOCAL_INPUT": "1", "SDL_JOYSTICK_HIDAPI": "0",
                    "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0", "SDL_HIDAPI_LIBUSB": "0",
                    "SDL_JOYSTICK_IOKIT": "0", "SDL_JOYSTICK_MFI": "0",
                    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0000"}
        if os.name == "nt":
            test_env["SystemRoot"] = os.environ.get("SystemRoot", r"C:\Windows")
        result = subprocess.run(sys.argv[1:], env=test_env)
    room_server.shutdown()
    keyframes.shutdown()
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
