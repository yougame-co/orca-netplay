#!/usr/bin/env python3
"""Friends-room and opt-in public queue fixture; dev tickets, isolated users, no app credentials.

  dropin-rooms.py --doubles --self-test
  dropin-rooms.py --doubles --serve --listen 127.0.0.1 --port 8765
  dropin-rooms.py --doubles --run --binary <nogui> --disc <disc> --site http://127.0.0.1:8765

For the lead's two-machine run, serve on an isolated LAN and run --roles 0,1 on
Mac and --roles 2,3 on PC, with the same --site and --room. --input is optional;
without it steer each isolated headless instance with stdin pad commands.

--queue casual|ranked uses normal host/join commands from private home rooms and
opts in with room.recovery:true; --no-recovery omits that field. A log-triggered
--events entry {"action":"drop","role":1,"pattern":"-> scMelee","seconds":16}
drops only the room socket, retaining the direct link and the same room seat.

--role-binary ROLE=PATH overrides --binary for one process. --version-refusals
checks the lobby and YG Dolphin version at the WebSocket upgrade, as the rooms
Worker does. --role-version ROLE=X.Y.Z supplies the page's ticket ob (the dev
binary requests a ticket without the desktop page). --owner-relaunch also lets
the owner reopen its empty room on another lobby, logging orca_room_started_over.
"""
import argparse
import base64
import faulthandler
import hashlib
import ipaddress
import json
import os
import queue
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HOST_OLDER = ("Your friend is on an older YG Dolphin. Ask them to press Play to update it "
              "and send you a new invite.")
HOST_NEWER = "Your friend is on a newer YG Dolphin. Press Play to update yours."
LOBBY_MISMATCH = "This room uses a different lobby protocol or configuration"


def clean_version(value):
    return value if isinstance(value, str) and re.fullmatch(r"[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}", value) else None


def lobby_mismatch(key_differs, host_version, your_version):
    """rooms/src/room-rules.ts lobbyMismatch: versions explain a different key only."""
    host, yours = clean_version(host_version), clean_version(your_version)
    body = {"error": LOBBY_MISMATCH, "code": "lobby_mismatch"}
    if host:
        body["host_version"] = host
    if yours:
        body["your_version"] = yours
    if key_differs and host and yours:
        left, right = tuple(map(int, host.split("."))), tuple(map(int, yours.split(".")))
        if left < right:
            body.update(error=HOST_OLDER, code="host_older")
        elif left > right:
            body.update(error=HOST_NEWER, code="host_newer")
    return body


def role_values(values):
    result = {}
    for value in values:
        role, separator, setting = value.partition("=")
        if not separator or role not in {"0", "1", "2", "3"} or not setting or role in result:
            raise ValueError("role overrides must be distinct ROLE=VALUE entries for roles 0..3")
        result[role] = setting
    return result


def local_counts(values):
    counts = role_values(value for value in values if value != "server")
    if any(count != "2" for count in counts.values()):
        raise ValueError("--local needs ROLE=2 (at most two local players)")
    return {int(role): 2 for role in counts}


def role_caps(role, local, queue_mode):
    return ("caps join leave stats" + (" coseats" if role in local else "") +
            (" host results locks" if queue_mode else "") + "\n")


def room_assignment(text, room, named):
    return re.search(rf"in room {re.escape(room)} on seat (\d+)" if named else r"on seat (\d+)", text)


class FixturePage:
    """The local page's bridge: tickets and an explicit one-time Add request."""
    def __init__(self, site, role, add):
        self.site, self.role, self.add = site, role, add
        self.requested = False

    def receive(self, line):
        if not line.startswith("yg "):
            return []
        request = json.loads(line[3:])
        kind = request.get("type")
        if kind not in {"hello", "mpTicket", "mpRoom"} or "id" not in request:
            return []
        result = {}
        if kind == "mpTicket":
            body = {key: value for key, value in request.items() if key not in {"id", "type"}}
            body.update(dev="orca-doubles-test", player_id=f"Doubles{self.role}",
                        name=f"Doubles{self.role}")
            result = post_fixture(self.site, "/api/multiplayer/ticket", body)
        messages = ["yg " + json.dumps({"re": request["id"], "ok": True, "result": result}) + "\n"]
        mine = [p for p in request.get("participants", []) if p.get("connectionId") == request.get("me")]
        if (kind == "mpRoom" and self.add and not self.requested and len(mine) == 1 and
                request.get("lobby", {}).get("maxLocalPlayers") == 2):
            self.requested = True
            messages.append("yg " + json.dumps({"push": {"slug": "doubles-fixture", "cmd": "local-add"}}) + "\n")
        return messages


def file_argument(value):
    # Preserve PATH executable names; pin real relative files before the isolated child chdir.
    return str(Path(value).resolve()) if Path(value).is_file() else value


def isolated_host(host):
    if host == "localhost":
        return True
    try:
        address = ipaddress.ip_address(host)
        return address.is_loopback or (address.is_private and not address.is_unspecified)
    except ValueError:
        return False


def frame(payload, masked=False):
    data = json.dumps(payload).encode()
    header = bytes([0x81, (0x80 if masked else 0) | (len(data) if len(data) < 126 else 126)])
    if len(data) >= 126:
        header += struct.pack("!H", len(data))
    if masked:
        mask = b"test"
        return header + mask + bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))
    return header + data


def receive(stream):
    header = stream.read(2)
    if len(header) != 2:
        raise EOFError
    opcode, size = header[0] & 15, header[1] & 127
    if size == 126:
        size = struct.unpack("!H", stream.read(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", stream.read(8))[0]
    if size > 65536:
        raise ValueError("fixture message exceeds limit")
    mask = stream.read(4) if header[1] & 128 else b""
    data = stream.read(size)
    if mask:
        data = bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))
    if opcode == 8:
        raise EOFError
    return json.loads(data) if opcode == 1 else {}


class Rooms:
    def __init__(self, doubles=True, version_refusals=False, owner_relaunch=False,
                 queue_mode=None, recovery=True, queue_room=None):
        self.queue_mode = queue_mode
        self.queue_room = queue_room
        self.recovery = recovery
        self.doubles = doubles and not queue_mode
        self.matches = {}
        self.match_numbers = {}
        self.game_reports = {}
        self.receipts = {}
        self.drop_receipts = 0
        self.lost_receipts = []
        self.lose_ranked_game_receipt = False
        self.missing_ranked_game = None
        self.final_delivered = set()
        self.commands = []
        self.drops = []
        self.verdicts = []
        self.connections = []
        self.timers = []
        self.version_refusals = version_refusals or owner_relaunch
        self.owner_relaunch = owner_relaunch
        self.lobbies = {}
        self.version_events = []
        self.lock = threading.RLock()
        self.members = {}
        self.locals = {}  # (room, connection) -> {participant id: slot}; no extra socket.
        self.sockets = {}
        self.joined = set()
        self.offers = []
        self.captures = {}
        self.hashes = {}
        self.activity = {}
        self.frames = {}
        self.reports = {}
        self.expired = set()
        self.blocked = set()
        self.away = set()
        self.hold_ms = None
        self.hold_until = {}
        self.verdict_delay = 0
        self.peer_away_after_rejoin = None

    def mode(self, room):
        return self.queue_mode if self.queue_room in (None, room) else None

    def admit(self, room, ticket):
        """Called under lock at the upgrade; refused tickets never acquire a seat."""
        player = ticket["p"]
        meta = self.lobbies.get(room)
        if meta and self.version_refusals:
            key_differs = meta.get("l") != ticket.get("l")
            differs = key_differs or meta.get("m") != ticket.get("m") or meta.get("k") != ticket.get("k")
            others = [name for code, name in self.members if code == room and
                      not (name == player and (room, name) in self.away)]
            if (differs and self.owner_relaunch and meta["p"] == player and not others
                    and ticket.get("l")):
                event = {"ev": "orca_room_started_over", "why": "owner_relaunch", "room": room}
                self.version_events.append(event)
                print(json.dumps(event), flush=True)
                # A retained owner seat and its old socket belong to the old lobby.
                old = self.sockets.pop((room, player), None)
                if old:
                    old[0].shutdown(socket.SHUT_RDWR)
                self.members.pop((room, player), None)
                self.locals.pop((room, player), None)
                self.joined.discard((room, player))
                self.away.discard((room, player))
                self.lobbies.pop(room)
                meta = None
            elif differs:
                opener_here = meta["p"] != player and (room, meta["p"]) in self.members
                body = lobby_mismatch(key_differs, meta.get("ob") if opener_here else None, ticket.get("ob"))
                self.version_events.append({"ev": "fixture_version_refusal", "room": room,
                                            "player": player, **body})
                return body, 403
        if not meta:
            self.lobbies[room] = ticket.copy()
        key = (room, player)
        if key not in self.members:
            used = self.used_slots(room)
            if len(used) == (2 if self.mode(room) else 4):
                return {}, 409
            self.members[key] = min(set(range(2 if self.mode(room) else 4)) - used)
        return None

    def used_slots(self, room):
        return ({seat for (code, _), seat in self.members.items() if code == room} |
                {seat for (code, _), seats in self.locals.items() if code == room
                 for seat in seats.values()})

    def local_command(self, room, player, message):
        if self.mode(room):
            return "queue"
        if not self.doubles:
            return "not-doubles"
        seats = self.locals.setdefault((room, player), {})
        if message["t"] == "local-remove":
            if message.get("id") not in seats:
                return "in-match"
            del seats[message["id"]]
        else:
            free = set(range(4)) - self.used_slots(room)
            if seats or not free:
                return "in-match"
            seats[f"local:{player}:1"] = min(free)
        return None

    def blackout(self, room, player):
        with self.lock:
            self.blocked.add((room, player))
            connection = self.sockets.get((room, player))
            if connection:
                connection[0].shutdown(socket.SHUT_RDWR)
            expiry = threading.Timer(90, self.expire, (room, player))
            expiry.daemon = True
            expiry.start()

    def expire(self, room, player):
        with self.lock:
            self.expired.add((room, player))
            self.members.pop((room, player), None)
            self.locals.pop((room, player), None)
            connection = self.sockets.pop((room, player), None)
            if connection:
                connection[0].shutdown(socket.SHUT_RDWR)
            for (code, name), other in list(self.sockets.items()):
                if code == room:
                    self.send(other, self.roster(room, name, "leave", player))

    def drop(self, room, player, seconds):
        """Drop only the room socket; the real direct link remains available to the peers."""
        with self.lock:
            self.blocked.add((room, player))
            hold = (self.hold_ms / 1000) if self.hold_ms is not None else 90
            self.hold_until[(room, player)] = time.monotonic() + hold
            self.drops.append({"room": room, "player": player, "seconds": seconds,
                               "at": time.monotonic()})
            connection = self.sockets.get((room, player))
            if connection:
                if self.mode(room) and self.recovery and self.hold_ms is not None:
                    remaining = max(0, int((self.hold_until[(room, player)] - time.monotonic()) * 1000))
                    self.send(connection, {"t": "recovering", "hold_ms": remaining})
                connection[0].shutdown(socket.SHUT_RDWR)
            def back():
                with self.lock:
                    self.blocked.discard((room, player))
            timer = threading.Timer(seconds, back)
            timer.daemon = True
            self.timers.append(timer)
            timer.start()
            if self.mode(room) == "ranked" and seconds >= hold:
                # The server owns this verdict, including for a socket which cannot come back.
                timer = threading.Timer(hold + self.verdict_delay, self.forfeit, (room, player))
                timer.daemon = True
                self.timers.append(timer)
                timer.start()

    def notify_away(self, room, player):
        with self.lock:
            if (room, player) not in self.away:
                return
            for (code, name), other in list(self.sockets.items()):
                if code == room and name != player:
                    try:
                        self.send(other, self.roster(room, name, "away", player, True))
                    except OSError:
                        pass

    def forfeit(self, room, player):
        with self.lock:
            match = self.matches.get(room)
            if not match:
                return
            winner = next(name for code, name in self.members if code == room and name != player)
            self.final_result(room, {"t": "result", "matchId": match,
                                    "ranking": [winner, player], "disposition": "forfeit",
                                    "reason": "The other player lost the connection. You win this set."})

    def final_result(self, room, verdict):
        self.verdicts.append(verdict)
        self.matches.pop(room, None)
        for (code, _), connection in list(self.sockets.items()):
            if code == room:
                try:
                    self.send(connection, verdict)
                    if self.mode(room) == "ranked":
                        self.send(connection, {"t": "set-over", "ms": 300000})
                except OSError:
                    pass

    def match_command(self, room, player, message):
        """Minimal agreement/result protocol, with stable game ids recorded as evidence."""
        self.commands.append({"player": player, **message})
        kind = message["t"]
        if kind == "begin":
            if room in self.matches:
                return
            self.match_numbers[room] = self.match_numbers.get(room, 0) + 1
            self.matches[room] = f"fixture-{room}-{self.match_numbers[room]}"
            for (code, name), connection in list(self.sockets.items()):
                if code == room:
                    self.send(connection, self.roster(room, name, "start"))
            return
        key = (message.get("matchId"), message.get("id", "$final"))
        if kind in {"game-report", "finish"} and key in self.receipts:
            self.deliver_receipt(room, self.receipts[key])
            return
        match = self.matches.get(room)
        if not match or message.get("matchId") != match or kind == "game-start":
            return
        key = (match, message.get("id", "$final"))
        reports = self.game_reports.setdefault(key, {})
        reports[player] = message
        if len(reports) < 2:
            return
        outcomes = [(r.get("winner"), r.get("draw", False), r.get("void", False))
                    for r in reports.values()]
        verdict = {"t": "game-result" if kind == "game-report" else "result", "matchId": match}
        if kind == "game-report":
            verdict["id"] = message["id"]
        if len(set(outcomes)) != 1 or outcomes[0][2]:
            verdict["void"] = True
        elif outcomes[0][1]:
            verdict["draw"] = True
        else:
            winner = outcomes[0][0]
            verdict["ranking"] = [winner] + [name for code, name in self.members
                                                       if code == room and name != winner]
        if "detail" in message:
            verdict["detail"] = message["detail"]
        self.receipts[key] = verdict
        if kind == "finish":
            self.matches.pop(room, None)
        self.deliver_receipt(room, verdict)

    def deliver_receipt(self, room, verdict):
        if self.lose_ranked_game_receipt and verdict["t"] == "game-result":
            key = (verdict["matchId"], verdict["id"])
            if self.missing_ranked_game is None:
                self.missing_ranked_game = key
            if key == self.missing_ranked_game and verdict["matchId"] not in self.final_delivered:
                if verdict not in self.lost_receipts:
                    self.lost_receipts.append(verdict)
                return
        if self.drop_receipts:
            self.drop_receipts -= 1
            self.lost_receipts.append(verdict)
            return
        self.verdicts.append(verdict)
        if verdict["t"] == "result":
            self.final_delivered.add(verdict["matchId"])
        for (code, _), connection in list(self.sockets.items()):
            if code == room:
                self.send(connection, verdict)

    def diagnostics(self, room):
        active = [name for code, name in self.members if code == room]
        reports = {name: value for (code, name), value in self.reports.items() if code == room}
        hashes = {name: value for (code, name), value in self.hashes.items() if code == room}
        return {"active": active, "reports": reports,
                "local_seats": {name: list(seats.values()) for (code, name), seats in self.locals.items()
                                if code == room},
                "input_activity": {name: sorted(seats) for (code, name), seats in self.activity.items()
                                   if code == room},
                "offers": list(self.offers), "version_events": list(self.version_events),
                "commands": list(self.commands), "drops": list(self.drops),
                "lost_receipts": list(self.lost_receipts),
                "verdicts": list(self.verdicts), "connections": list(self.connections),
                "frames": {name: frame_number for (code, name), frame_number in self.frames.items() if code == room}, "hashes": {name: dict(value) for name, value in hashes.items()}}

    def record(self, room, player, data):
        packets = [entry[1] for entry in data.get("p", [])] if data.get("k") == "bp" else [data]
        for packet in packets:
            if packet.get("k") == "p":
                key = (room, player)
                self.frames[key] = max(self.frames.get(key, 0), packet.get("c", 0))
                inputs = [(packet.get("s", -1), packet.get("p", "")), *packet.get("co", [])]
                for slot, pads in inputs:
                    if not isinstance(slot, int) or slot < 0 or slot >= 4:
                        continue
                    samples = [bytes.fromhex(pads[index:index + 16])
                               for index in range(0, len(pads), 16)]
                    if any(len(pad) == 8 and not pad[0] & 0x80 and any(pad) for pad in samples):
                        self.activity.setdefault(key, set()).add(slot)
            if packet.get("k") == "p" and "x" in packet and "h" in packet:
                self.hashes.setdefault((room, player), {})[str(packet["x"])] = packet["h"]

    def roster(self, room, local, kind="welcome", changed=None, away=False):
        members = sorted(((seat, name) for (code, name), seat in self.members.items() if code == room))
        result = {"t": kind, "me": local, "host": members[0][1] if members else local, "lifecycle": "game", "revision": 1,
                "room": {"code": room, "queue": self.mode(room) or "private", "size": 2 if self.mode(room) else 4, "doubles": self.doubles},
                "player": {"id": changed or local},
                "players": [{"id": name, "name": name,
                             **({"away": True} if (room, name) in self.away or (away and name == changed) else {})}
                            for _, name in members],
                "participants": [{"id": name, "connectionId": name, "slot": seat, "name": name}
                                 for seat, name in members]}
        for _, name in members:
            for participant, slot in self.locals.get((room, name), {}).items():
                result["participants"].append({"id": participant, "connectionId": name,
                    "localIndex": 1, "slot": slot, "name": name + " · Player 2"})
        result["participants"].sort(key=lambda participant: participant["slot"])
        if self.mode(room) and self.recovery and kind == "welcome":
            result["room"]["recovery"] = True
        if self.mode(room) and self.recovery and self.hold_ms is not None:
            held = self.hold_until.get((room, changed if kind == "away" else local))
            if held is not None and kind in {"away", "welcome", "recovering"}:
                result["hold_ms"] = max(0, int((held - time.monotonic()) * 1000))
        if room in self.matches:
            result["matchId"] = self.matches[room]
        return result

    def send(self, connection, data):
        sock, guard = connection
        with guard:
            try:
                # A blackout can stop a peer reading. Bound writes without timing out idle
                # reads, so this socket cannot hold the room lock throughout the blackout.
                timeout = 1000 if os.name == "nt" else struct.pack("ll", 1, 0)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, timeout)
                sock.sendall(frame(data))
            except OSError:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                raise

    def broadcast(self, room, local, message):
        for (code, name), connection in list(self.sockets.items()):
            if code == room and name != local:
                try:
                    self.send(connection, message)
                except OSError:
                    pass


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, turn_binary=None, doubles=True, version_refusals=False,
                 owner_relaunch=False, role_versions=None, queue_mode=None, recovery=True, queue_room=None):
        self.rooms = Rooms(doubles, version_refusals, owner_relaunch, queue_mode, recovery, queue_room)
        self.role_versions = role_versions or {}
        self.turn = None
        self.ice = []
        if turn_binary:
            self.turn = subprocess.Popen([turn_binary, address[0]], stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL, text=True)
            port = int(self.turn.stdout.readline())
            self.ice = [{"urls": f"turn:{address[0]}:{port}", "username": "orca-fixture",
                         "credential": "orca-fixture-only"}]
        super().__init__(address, Handler)

    def server_close(self):
        for timer in self.rooms.timers:
            timer.cancel()
        super().server_close()
        if self.turn:
            self.turn.terminate()
            self.turn.wait(timeout=5)
            self.turn.stdout.close()

    def handle_error(self, request, address):
        pass  # Expected fixture disconnects never print packet contents.


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, body, status=200):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.path.startswith(("/report/", "/expire/", "/blackout/", "/away/", "/back/", "/drop/")):
            count = int(self.headers.get("Content-Length", 0))
            if count > 4096:
                return self.reply({}, 413)
            body = json.loads(self.rfile.read(count))
            room = self.path.rsplit("/", 1)[-1]
            player = body.get("player")
            if player not in {f"Doubles{seat}" for seat in range(4)}:
                return self.reply({}, 400)
            rooms = self.server.rooms
            with rooms.lock:
                if self.path.startswith("/drop/"):
                    seconds = body.get("seconds", 15)
                    if not isinstance(seconds, (int, float)) or not 1 <= seconds <= 300:
                        return self.reply({}, 400)
                    rooms.drop(room, player, seconds)
                elif self.path.startswith("/blackout/"):
                    rooms.blackout(room, player)
                elif self.path.startswith("/away/"):
                    rooms.blocked.add((room, player))
                    connection = rooms.sockets.get((room, player))
                    if connection:
                        connection[0].shutdown(socket.SHUT_RDWR)
                elif self.path.startswith("/back/"):
                    rooms.blocked.discard((room, player))
                elif self.path.startswith("/expire/"):
                    rooms.expire(room, player)
                else:
                    rooms.reports[(room, player)] = body
            return self.reply({})
        if self.path != "/api/multiplayer/ticket":
            return self.reply({}, 404)
        count = int(self.headers.get("Content-Length", 0))
        if count > 4096:
            return self.reply({}, 413)
        body = json.loads(self.rfile.read(count))
        room, player = body["room"], body["player_id"]
        if body.get("dev") != "orca-doubles-test" or player not in {f"Doubles{seat}" for seat in range(5)}:
            return self.reply({}, 403)
        with self.server.rooms.lock:
            members = self.server.rooms.members
            key = (room, player)
            if key in self.server.rooms.blocked:
                return self.reply({"error": "Service Unavailable"}, 503) if self.server.rooms.mode(room) else self.reply({"error": "room-recovering"}, 409)
            if key in self.server.rooms.expired:
                return self.reply({"error": "room-hold-expired"}, 409)
            if key not in members and not self.server.rooms.version_refusals:
                used = self.server.rooms.used_slots(room)
                size = 2 if self.server.rooms.mode(room) else 4
                if len(used) == size:
                    return self.reply({}, 409)
                members[key] = min(set(range(size)) - used)
        # This literal is a throwaway local fixture value, never a production ticket.
        host = self.headers["Host"]
        ticket = player
        if self.server.rooms.version_refusals:
            payload = {"p": player, "c": room, "l": body.get("lobby"), "m": body.get("mode"),
                       "k": body.get("players", 4),
                       "ob": self.server.role_versions.get(player[-1], body.get("ob"))}
            ticket = "fixture." + base64.urlsafe_b64encode(json.dumps(payload).encode()).decode()
        self.reply({"url": "http://" + host, "ticket": ticket, "direct": True, "ice": self.server.ice})

    def do_GET(self):
        if self.path.startswith("/diagnostics/"):
            with self.server.rooms.lock:
                return self.reply(self.server.rooms.diagnostics(self.path.rsplit("/", 1)[-1]))
        if self.path.startswith("/api/orca/keyframes/"):
            return self.keyframe("get")
        parsed = urllib.parse.urlsplit(self.path)
        room = parsed.path.rsplit("/", 1)[-1]
        raw_ticket = urllib.parse.parse_qs(parsed.query).get("ticket", [""])[0]
        rooms = self.server.rooms
        ticket = None
        player = raw_ticket
        if rooms.version_refusals:
            try:
                ticket = json.loads(base64.urlsafe_b64decode(raw_ticket.removeprefix("fixture.")))
                player = ticket["p"]
                if ticket.get("c") != room or player not in {f"Doubles{seat}" for seat in range(4)}:
                    return self.reply({}, 403)
            except (ValueError, KeyError, TypeError):
                return self.reply({}, 403)
        with rooms.lock:
            if (room, player) in rooms.blocked:
                return self.reply({"error": "Service Unavailable"}, 503) if self.server.rooms.mode(room) else self.reply({"error": "room-recovering"}, 409)
            if ticket:
                refusal = rooms.admit(room, ticket)
                if refusal:
                    return self.reply(*refusal)
            elif (room, player) not in rooms.members:
                return self.reply({}, 403)
        self.send_response(101)
        self.send_header("Connection", "Upgrade")
        self.send_header("Upgrade", "websocket")
        accept = hashlib.sha1((self.headers["Sec-WebSocket-Key"] +
                               "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        self.send_header("Sec-WebSocket-Accept", base64.b64encode(accept).decode())
        self.end_headers()
        connection = (self.connection, threading.Lock())
        with rooms.lock:
            returning = (room, player) in rooms.joined
            rooms.away.discard((room, player))
            rooms.joined.add((room, player))
            rooms.sockets[(room, player)] = connection
            welcome = rooms.roster(room, player)
            rooms.connections.append({"room": room, "player": player, "returning": returning,
                                      "slot": rooms.members[(room, player)], "at": time.monotonic(),
                                      "hold_ms": welcome.get("hold_ms"),
                                      "away": [p["id"] for p in welcome["players"] if p.get("away")]})
            rooms.send(connection, welcome)
            if returning and rooms.peer_away_after_rejoin is not None:
                for peer in welcome["players"]:
                    if peer.get("away") and peer["id"] != player:
                        timer = threading.Timer(rooms.peer_away_after_rejoin, rooms.notify_away,
                                                (room, peer["id"]))
                        timer.daemon = True
                        rooms.timers.append(timer)
                        timer.start()
            for (code, name), other in list(rooms.sockets.items()):
                if code == room and name != player:
                    rooms.send(other, rooms.roster(room, name, "back" if returning else "join", player))
        departed = False
        try:
            while True:
                message = receive(self.rfile)
                with rooms.lock:
                    if message.get("t") == "ping":
                        rooms.send(connection, {"t": "pong", "at": message["at"]})
                    elif message.get("t") == "msg":
                        rooms.record(room, player, message["d"])
                        outgoing = {"t": "msg", "from": player, "d": message["d"]}
                        if message["d"].get("k") == "kf":
                            rooms.offers.append((message.get("to"), message["d"].get("id"), message["d"].get("f")))
                        target = message.get("to")
                        if target:
                            other = rooms.sockets.get((room, target))
                            if other:
                                rooms.send(other, outgoing)
                        else:
                            rooms.broadcast(room, player, outgoing)
                    elif rooms.mode(room) and message.get("t") in {"begin", "game-start", "game-report", "finish"}:
                        rooms.match_command(room, player, message)
                    elif message.get("t") in {"local-add", "local-remove"}:
                        error = rooms.local_command(room, player, message)
                        if error:
                            rooms.send(connection, {"t": "command-error", "action": message["t"], "error": error})
                        else:
                            for (code, name), other in list(rooms.sockets.items()):
                                if code == room:
                                    rooms.send(other, rooms.roster(room, name, "lobby", player))
                    elif message.get("t") == "leave":
                        rooms.members.pop((room, player), None)
                        rooms.locals.pop((room, player), None)
                        for (code, name), other in list(rooms.sockets.items()):
                            if code == room and name != player:
                                rooms.send(other, rooms.roster(room, name, "leave", player))
                        departed = True
                        break
        except (EOFError, OSError, ValueError):
            pass
        finally:
            with rooms.lock:
                if rooms.sockets.get((room, player)) is connection:
                    rooms.sockets.pop((room, player), None)
                    if not departed and (room, player) in rooms.members:
                        rooms.away.add((room, player))
                        rooms.notify_away(room, player)
            self.close_connection = True

    def keyframe(self, operation):
        parts = self.path.split("/")
        authorization = self.headers.get("Authorization", "")
        valid = authorization in {f"Ticket Doubles{seat}" for seat in range(4)}
        if self.server.rooms.version_refusals and authorization.startswith("Ticket fixture."):
            try:
                ticket = json.loads(base64.urlsafe_b64decode(authorization[len("Ticket fixture."):]))
                valid = ticket.get("p") in {f"Doubles{seat}" for seat in range(4)} and ticket.get("c") == parts[4]
            except (ValueError, TypeError, IndexError):
                valid = False
        if len(parts) != 6 or not valid:
            return self.reply({}, 403)
        capture = (parts[4], parts[5])
        rooms = self.server.rooms
        with rooms.lock:
            if operation == "put":
                count = int(self.headers.get("Content-Length", 0))
                if count > 128 * 1024 * 1024:
                    return self.reply({}, 413)
                if capture in rooms.captures:
                    return self.reply({"code": "exists"}, 409)
                rooms.captures[capture] = self.rfile.read(count)
                return self.reply({"id": capture[1], "size": count})
            if operation == "delete":
                rooms.captures.pop(capture, None)
                return self.reply({})
            data = rooms.captures.get(capture)
            if data is None:
                return self.reply({"code": "gone"}, 404)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self):
        self.keyframe("put")

    def do_DELETE(self):
        self.keyframe("delete")


def client(site, player, room, **fields):
    request = urllib.request.Request(site + "/api/multiplayer/ticket", json.dumps(
        {"dev": "orca-doubles-test", "room": room, "player_id": player, **fields}).encode(),
        {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=3) as response:
        ticket = json.load(response)["ticket"]
    address = urllib.parse.urlsplit(site)
    sock = socket.create_connection((address.hostname, address.port), timeout=3)
    sock.sendall((f"GET /room/{room}?ticket={urllib.parse.quote(ticket)} HTTP/1.1\r\nHost: {address.netloc}\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                  "Sec-WebSocket-Key: dGVzdGRvdWJsZXNmaXh0dXJl\r\n\r\n").encode())
    stream = sock.makefile("rb")
    status = int(stream.readline().split()[1])
    headers = {}
    while line := stream.readline().strip():
        key, value = line.decode().split(":", 1)
        headers[key.lower()] = value.strip()
    if status != 101:
        body = stream.read(int(headers.get("content-length", 0)))
        stream.close()
        sock.close()
        raise FixtureRefusal(status, json.loads(body))
    return sock, stream


class FixtureRefusal(Exception):
    def __init__(self, status, body):
        self.status, self.body = status, body
        super().__init__(f"fixture HTTP {status}: {body.get('code', 'refused')}")


def self_test(turn_binary=None):
    server = Server(("127.0.0.1", 0), turn_binary)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    site = f"http://127.0.0.1:{server.server_port}"
    clients = []
    try:
        for seat in range(4):
            clients.append(client(site, f"Doubles{seat}", "dry444"))
            welcome = receive(clients[-1][1])
            assert welcome["room"]["doubles"] and welcome["room"]["queue"] == "private"
            assert welcome["participants"][-1]["slot"] == seat
        for seat, (sock, stream) in enumerate(clients):
            # Drain the expected subsequent roster joins, then check the complete mesh fan-out.
            for _ in range(3 - seat):
                assert receive(stream)["t"] == "join"
            sock.sendall(frame({"t": "msg", "d": {"k": "hello", "doubles": True}}, True))
        try:
            client(site, "Doubles4", "dry444")
            raise AssertionError("fifth client was accepted")
        except urllib.error.HTTPError as error:
            assert error.code == 409
        assert len(server.rooms.members) == 4
        for seat, (_, stream) in enumerate(clients):
            senders = {receive(stream)["from"] for _ in range(3)}
            assert senders == {f"Doubles{other}" for other in range(4) if other != seat}
        for seat in range(1, 4):
            clients[0][0].sendall(frame({"t": "msg", "to": f"Doubles{seat}",
                                       "d": {"k": "kf", "id": "dry-capture", "f": 240}}, True))
            assert receive(clients[seat][1])["d"]["f"] == 240
        assert len({capture for _, capture, _ in server.rooms.offers}) == 1
        # An ungraceful disconnect retains the seat and code; reconnect replaces only that socket.
        clients[3][0].shutdown(socket.SHUT_RDWR)
        clients[3][1].close()
        clients[3][0].close()
        for _, stream in clients[:3]:
            assert receive(stream)["t"] == "away"
        # A newcomer/reconnecting peer must see every retained away seat, not only the latest event.
        assert next(player for player in server.rooms.roster("dry444", "Doubles0")["players"]
                    if player["id"] == "Doubles3")["away"]
        clients[3] = client(site, "Doubles3", "dry444")
        resumed = receive(clients[3][1])
        assert resumed["room"]["code"] == "dry444" and resumed["participants"][-1]["slot"] == 3
        for _, stream in clients[:3]:
            assert receive(stream)["t"] == "back"
        clients[3][0].sendall(frame({"t": "leave"}, True))
        for _, stream in clients[:3]:
            verdict = receive(stream)
            assert verdict["t"] == "leave" and len(verdict["participants"]) == 3
        clients[2][0].shutdown(socket.SHUT_RDWR)
        clients[2][1].close()
        clients[2][0].close()
        for _, stream in clients[:2]:
            assert receive(stream)["t"] == "away"
        server.rooms.expire("dry444", "Doubles2")
        for _, stream in clients[:2]:
            verdict = receive(stream)
            assert verdict["t"] == "leave" and verdict["player"]["id"] == "Doubles2"
        for role in range(3):
            server.rooms.reports[("dry444", f"Doubles{role}")] = {"port": role + 1, "plug": 360 if role else None}
            server.rooms.record("dry444", f"Doubles{role}", {"k": "p", "x": 600, "h": "fixture-hash"})
        verify_diagnostics(server.rooms.diagnostics("dry444"), expected=3)
        bad = server.rooms.diagnostics("dry444")
        bad["reports"]["Doubles2"] = {"port": 3, "plug": 361}
        try:
            verify_diagnostics(bad, expected=3)
            raise AssertionError("different cross-machine plug frames accepted")
        except ValueError:
            pass
        late = server.rooms.diagnostics("dry444")
        late["reports"]["Doubles2"] = {"port": 3, "plug": 480, "capture": "later-capture"}
        verify_diagnostics(late, expected=3)
        bad = server.rooms.diagnostics("dry444")
        bad["hashes"]["Doubles1"] = {"600": "different"}
        try:
            verify_diagnostics(bad, expected=3)
            raise AssertionError("different final hashes accepted")
        except ValueError:
            pass
        print("PASS: four clients, fifth refused, six pairs, shared capture, retained away roster, "
              "recovery, leave, hold expiry, capture-scoped plugs and confirmed hashes")
    finally:
        for sock, stream in clients:
            stream.close()
            sock.close()
        server.shutdown()
        server.server_close()
    version_self_test()
    queue_self_test()
    receipt_self_test()


def receipt_self_test():
    server = Server(("127.0.0.1", 0), queue_mode="ranked")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    site = f"http://127.0.0.1:{server.server_port}"
    clients = []
    try:
        for role in (0, 1):
            clients.append(client(site, f"Doubles{role}", "receipt355"))
            receive(clients[-1][1])
        receive(clients[0][1])
        clients[0][0].sendall(frame({"t": "begin"}, True))
        starts = [receive(stream) for _, stream in clients]
        match = starts[0]["matchId"]
        for kind in ("game-report", "finish"):
            report = {"t": kind, "matchId": match, "winner": "Doubles0"}
            if kind == "game-report":
                report["id"] = "g120"
            server.rooms.drop_receipts = 1
            for sock, _ in clients:
                sock.sendall(frame(report, True))
            deadline = time.monotonic() + 2
            while not server.rooms.lost_receipts or server.rooms.lost_receipts[-1]["t"] != ("game-result" if kind == "game-report" else "result"):
                assert time.monotonic() < deadline, "receipt was not recorded/dropped"
                time.sleep(0.01)
            # No new gameplay report is generated: resend the exact wire payload after rejoin.
            clients[1][0].shutdown(socket.SHUT_RDWR)
            clients[1][1].close()
            clients[1][0].close()
            assert receive(clients[0][1])["t"] == "away"
            clients[1] = client(site, "Doubles1", "receipt355")
            receive(clients[1][1])
            assert receive(clients[0][1])["t"] == "back"
            clients[1][0].sendall(frame(report, True))
            verdicts = [receive(stream) for _, stream in clients]
            assert verdicts[0] == verdicts[1] == server.rooms.lost_receipts[-1]
            matching = [entry for entry in server.rooms.commands if entry["t"] == kind]
            assert all({key: value for key, value in entry.items() if key != "player"} == report for entry in matching)
        assert len(server.rooms.receipts) == 2
        print("PASS: report fixture loses game/final receipts, rejoins, replays identical reports and returns recorded receipts")
    finally:
        for sock, stream in clients:
            stream.close()
            sock.close()
        server.shutdown()
        server.server_close()


def queue_self_test():
    for mode, recovery in (("casual", True), ("casual", False), ("ranked", True)):
        server = Server(("127.0.0.1", 0), queue_mode=mode, recovery=recovery)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        site = f"http://127.0.0.1:{server.server_port}"
        clients = []
        try:
            for role in (0, 1):
                clients.append(client(site, f"Doubles{role}", "queue354"))
                welcome = receive(clients[-1][1])
                assert welcome["room"]["queue"] == mode
                assert welcome["room"].get("recovery", False) is recovery
                assert welcome["room"]["size"] == 2 and not welcome["room"]["doubles"]
            assert receive(clients[0][1])["t"] == "join"
            clients[0][0].sendall(frame({"t": "begin"}, True))
            starts = [receive(stream) for _, stream in clients]
            assert starts[0]["matchId"] == starts[1]["matchId"]
            match = starts[0]["matchId"]
            for sock, _ in clients:
                sock.sendall(frame({"t": "game-report", "matchId": match,
                                    "id": "g120", "winner": "Doubles0"}, True))
            assert all(receive(stream)["t"] == "game-result" for _, stream in clients)
            # Socket-only outage holds the seat and puts away/back in the other peer's roster.
            server.rooms.drop("queue354", "Doubles1", 0.05)
            assert receive(clients[0][1])["t"] == "away"
            clients[1][1].close()
            clients[1][0].close()
            time.sleep(0.06)
            clients[1] = client(site, "Doubles1", "queue354")
            assert receive(clients[1][1])["matchId"] == match
            assert receive(clients[0][1])["t"] == "back"
            if mode == "ranked":
                server.rooms.forfeit("queue354", "Doubles1")
                for _, stream in clients:
                    verdict = receive(stream)
                    assert verdict["disposition"] == "forfeit" and verdict["ranking"][0] == "Doubles0"
                    assert receive(stream)["t"] == "set-over"
        finally:
            for sock, stream in clients:
                stream.close()
                sock.close()
            server.shutdown()
            server.server_close()
    print("PASS: casual/ranked welcome opt-in, legacy omission, stable match, game agreement, socket drop/rejoin, server forfeit")


def version_self_test():
    server = Server(("127.0.0.1", 0), version_refusals=True, owner_relaunch=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    site = f"http://127.0.0.1:{server.server_port}"
    clients = []

    def connect(room, role, version, key, **fields):
        peer = client(site, f"Doubles{role}", room, ob=version, mode="orca-rsbe01", players=4,
                      lobby={"version": 1, "minPlayers": 2, "maxLocalPlayers": 1,
                             "compatibility": key}, **fields)
        clients.append(peer)
        assert receive(peer[1])["room"]["code"] == room
        return peer

    def refuse(room, version, key, expected, role=1):
        before = dict(server.rooms.members)
        try:
            connect(room, role, version, key)
            raise AssertionError("mismatched lobby was accepted")
        except FixtureRefusal as error:
            assert error.status == 403 and error.body == expected, error.body
        assert server.rooms.members == before

    def disconnect(peer, room, role, leave=False):
        if leave:
            peer[0].sendall(frame({"t": "leave"}, True))
        else:
            peer[0].shutdown(socket.SHUT_RDWR)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            with server.rooms.lock:
                ready = ((room, f"Doubles{role}") not in server.rooms.members if leave else
                         (room, f"Doubles{role}") in server.rooms.away)
            if ready:
                return
            time.sleep(0.01)
        raise AssertionError("fixture did not observe disconnect")

    try:
        connect("old444", 0, "0.3.47", "old-key")
        refuse("old444", "0.3.48", "new-key", {"error": HOST_OLDER, "code": "host_older",
                                               "host_version": "0.3.47", "your_version": "0.3.48"})
        connect("new444", 0, "0.3.49", "new-key")
        refuse("new444", "0.3.48", "old-key", {"error": HOST_NEWER, "code": "host_newer",
                                               "host_version": "0.3.49", "your_version": "0.3.48"})
        connect("same444", 0, "0.3.48", "disc-a")
        refuse("same444", "0.3.48", "disc-b", {"error": LOBBY_MISMATCH, "code": "lobby_mismatch",
                                                 "host_version": "0.3.48", "your_version": "0.3.48"})
        refuse("old444", None, "new-key", {"error": LOBBY_MISMATCH, "code": "lobby_mismatch",
                                            "host_version": "0.3.47"})
        assert lobby_mismatch(False, "0.3.47", "0.3.48")["code"] == "lobby_mismatch"
        assert lobby_mismatch(True, "0.3.9", "0.3.10")["code"] == "host_older"
        # A version alone is not a compatibility key; matching lobbies still admit.
        connect("old444", 1, "0.3.48", "old-key")
        refuse("old444", "0.3.49", "another-key", {"error": LOBBY_MISMATCH,
               "code": "lobby_mismatch", "your_version": "0.3.49"}, role=0)
        assert not server.rooms.version_events[-1].get("why")

        # Empty after an explicit leave, and empty except for the owner's held seat.
        for held in (False, True):
            room = "held444" if held else "empty444"
            peer = connect(room, 0, "0.3.47", "old-key")
            disconnect(peer, room, 0, leave=not held)
            before = len(server.rooms.version_events)
            connect(room, 0, "0.3.48", "new-key")
            assert server.rooms.version_events[before:] == [
                {"ev": "orca_room_started_over", "why": "owner_relaunch", "room": room}]
            assert server.rooms.lobbies[room]["ob"] == "0.3.48"
            assert server.rooms.members[(room, "Doubles0")] == 0
        # A friend's held seat prevents an owner reset too.
        owner = connect("busy444", 0, "0.3.47", "old-key")
        friend = connect("busy444", 1, "0.3.47", "old-key")
        disconnect(friend, "busy444", 1)
        disconnect(owner, "busy444", 0)
        refuse("busy444", "0.3.48", "new-key", {"error": LOBBY_MISMATCH,
               "code": "lobby_mismatch", "your_version": "0.3.48"}, role=0)
        print("PASS: upgrade 403 host_older/host_newer/lobby_mismatch bodies, numeric versions, "
              "matching keys, empty/held owner relaunch, occupied-room refusal")
    finally:
        for sock, stream in clients:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            stream.close()
            sock.close()
        server.shutdown()
        server.server_close()


def verify_diagnostics(data, expected=4):
    reports = data["reports"]
    if len(reports) < expected:
        raise ValueError("waiting for reports from both machines")
    batches = {}
    for report in reports.values():
        if report.get("plug") is not None:
            batches.setdefault(report.get("capture", "initial"), set()).add(report["plug"])
    if not batches or any(len(plugs) != 1 for plugs in batches.values()):
        raise ValueError("cross-machine plug frames differ within a capture")
    ports = [report["port"] for report in reports.values() if report.get("port") is not None]
    if len(ports) != len(set(ports)):
        raise ValueError("duplicate ports across machines")
    active = data["active"]
    hashes = [data["hashes"].get(name, {}) for name in active]
    common = set.intersection(*(set(values) for values in hashes)) if hashes else set()
    if not common:
        raise ValueError("no common confirmed frame hash")
    for frame_number in common:
        if len({values[frame_number] for values in hashes}) != 1:
            raise ValueError(f"confirmed frame {frame_number} differs across machines")
    latest = max(map(int, common))
    if latest < min(data["frames"].get(name, 0) for name in active) - 120:
        raise ValueError("common confirmed hashes stopped before the end of the run")
    return latest


def post_fixture(site, path, body):
    request = urllib.request.Request(site + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=3) as response:
        return json.load(response)


def verify_gameplay(work, masks):
    """Require actual human fighter records and a return to CSS for every requested fight."""
    for index, mask in enumerate(masks, 1):
        expected = {port for port in range(4) if mask & (1 << port)}
        for path in Path(work).glob(f"user-*/probe-fight-{index}.bin"):
            record = path.read_bytes()
            humans = {port for port in range(4) if record[port * 0x5c + 1] == 0}
            if humans != expected:
                raise RuntimeError(f"fight {index}: {path.parent.name} humans {humans}, expected {expected}")
        host_probe = Path(work) / "user-0" / f"probe-fight-{index}.bin"
        if not host_probe.exists():
            raise RuntimeError(f"fight {index}: no host fighter record")
    host_log = (Path(work) / "peer-0.log").read_text()
    scenes = re.findall(r"Scene frame (\d+): .* -> (\w+)", host_log)
    fights = [offset for offset, (_, scene) in enumerate(scenes) if scene == "scMelee"]
    if len(fights) < len(masks):
        raise RuntimeError("not all requested fights began")
    for offset in fights[:len(masks)]:
        if not any(scene == "scSelctCharacter" for _, scene in scenes[offset + 1:]):
            raise RuntimeError("a requested fight did not return to character select")


def verify_queue_run(args, work, diagnostics, errors):
    drops = [drop for drop in diagnostics["drops"] if drop["room"] == args.room]
    if not args.expect_ranked_reconciliation and (not drops or min(drop["seconds"] for drop in drops) < 15):
        raise RuntimeError("queue check needs a scheduled room drop of at least 15 seconds")
    if not any(command["t"] == "begin" for command in diagnostics["commands"]):
        raise RuntimeError("queue match never began")
    logs = [(Path(work) / f"peer-{role}.stdout.log").read_text() for role in (0, 1)]
    if args.expect_ranked_reconciliation:
        if errors or args.queue != "ranked" or len(diagnostics["lost_receipts"]) != 1:
            raise RuntimeError("ranked reconciliation requires one missing game receipt and no error")
        lost = diagnostics["lost_receipts"][0]
        verdicts = diagnostics["verdicts"]
        final_index = next(i for i, v in enumerate(verdicts) if v["t"] == "result")
        earlier_index = next(i for i, v in enumerate(verdicts)
                             if v.get("id") == lost["id"] and v["t"] == "game-result")
        if not 0 < final_index < earlier_index:
            raise RuntimeError("ranked receipts did not cross the final in the required order")
        for role, log in enumerate(logs):
            results = [json.loads(line[len("orca result "):])
                       for line in log.splitlines() if line.startswith("orca result ")]
            games = [r for r in results if r.get("k") == "game"]
            finals = [r for r in results if r.get("k") == "set"]
            if len(games) != 2 or {r["n"] for r in games} != {1, 2} or len(finals) != 1:
                raise RuntimeError("ranked game/final receipts were missing or applied more than once")
            if results[-1] != finals[0] or finals[0]["score"] != ([2, 0] if role == 0 else [0, 2]):
                raise RuntimeError("ranked final published before the full tally was reconciled")
        for player in ("Doubles0", "Doubles1"):
            reports = [c for c in diagnostics["commands"]
                       if c["player"] == player and c["t"] == "game-report" and c["id"] == lost["id"]]
            if len(reports) < 2 or any(report != reports[0] for report in reports):
                raise RuntimeError("missing ranked game did not resend its original identity/payload")
        print("PASS: earlier ranked game receipt follows later game and final; two games once, full final tally", flush=True)
    elif args.expect_queue_ending == "network":
        if not args.no_recovery or not errors:
            raise RuntimeError("legacy queue did not produce its network ending")
        if any("room-away self" in log or "room-back self" in log for log in logs):
            raise RuntimeError("legacy queue entered recovery")
    elif args.expect_queue_ending == "forfeit":
        dropped = int(drops[-1]["player"][-1])
        if not any(command["t"] == "game-start" for command in diagnostics["commands"]):
            raise RuntimeError("ranked fight never reported that its fighters were in")
        if drops[-1]["seconds"] < (args.hold_ms / 1000 if args.hold_ms is not None else 90):
            raise RuntimeError("ranked forfeit check requires an outage exceeding the hold")
        if errors.get(dropped) != ("orca error network Lost the connection to the match. "
                                   "Press Play to look for a new one."):
            raise RuntimeError("absent ranked peer did not end its expired hold as a network loss")
        if args.queue != "ranked" or not any(v.get("disposition") == "forfeit" for v in diagnostics["verdicts"]):
            raise RuntimeError("fixture did not decide a ranked forfeit")
        results = [json.loads(line[len("orca result "):])
                   for line in logs[1 - dropped].splitlines() if line.startswith("orca result ")]
        if not any(result.get("by") == "forfeit" and result.get("out") == "won" for result in results):
            # A forfeit can close before an unmatched game has any game verdict. The
            # authoritative outcome must show while that report remains retryable;
            # its unknown score must not be published as a completed tally.
            winner = 1 - dropped
            ui_log = (Path(work) / f"peer-{winner}.log").read_text()
            if "Set end: the room's verdict: won by forfeit score pending" not in ui_log:
                raise RuntimeError("server's winning forfeit verdict never reached the other player")
            reports = [c for c in diagnostics["commands"] if c["player"] == f"Doubles{winner}" and c["t"] == "game-report"]
            if len(reports) < 2 or any(c != reports[0] for c in reports):
                raise RuntimeError("unsettled game did not retain and resend its original report")
            if any(result.get("k") == "set" for result in results):
                raise RuntimeError("forfeit published an unreconciled score")
            print("PASS: authoritative forfeit shown with score pending; unsettled game report remains retryable", flush=True)
        if any(command["t"] == "finish" and "winner" in command for command in diagnostics["commands"]):
            raise RuntimeError("engine claimed the set during recovery")
    else:
        if errors:
            raise RuntimeError("recovering queue ended with an error")
        if not any(("room-back self" in log and "kept same-seat" in log) or
                   (args.expect_report_replay and "same-seat-fresh-drop-in" in log) for log in logs):
            raise RuntimeError("dropped queue socket did not recover its same seat/session")
        drop = drops[-1]
        backs = [connection for connection in diagnostics["connections"]
                 if connection["player"] == drop["player"] and connection["returning"]]
        if not backs or backs[-1]["at"] - drop["at"] < 15:
            raise RuntimeError("room socket was not absent for 15 seconds")
    if args.expect_crossed_holds:
        returning = [entry for entry in diagnostics["connections"]
                     if entry["player"] == "Doubles0" and entry["returning"] and
                     entry["away"] == ["Doubles1"]]
        if not returning or not 0 <= returning[-1]["hold_ms"] < 10000:
            raise RuntimeError("crossed hold did not return with a small self remainder and away peer")
        peer_drop = next(drop for drop in drops if drop["player"] == "Doubles1")
        if returning[-1]["at"] >= peer_drop["at"] + 90:
            raise RuntimeError("crossed peer was no longer inside its independent hold")
        if 0 in errors:
            raise RuntimeError("returning player departed before the peer's verdict")
        print("PASS: self WELCOME small remainder preserves the still-away peer's independent hold and verdict", flush=True)
    if args.expect_report_replay:
        if not diagnostics["lost_receipts"]:
            raise RuntimeError("report replay run did not lose a receipt")
        dropped = drops[-1]["player"]
        seats = [entry["slot"] for entry in diagnostics["connections"]
                 if entry["room"] == args.room and entry["player"] == dropped]
        if len(seats) < 2 or len(set(seats)) != 1:
            raise RuntimeError("report replay did not retain its original seat")
        reports = [entry for entry in diagnostics["commands"]
                   if entry["player"] == dropped and entry["t"] in {"game-report", "finish"}]
        if len(reports) < 2 or any(entry != reports[0] for entry in reports):
            raise RuntimeError("dropped peer did not replay identical report bytes/identity")
        for log in logs:
            results = [line for line in log.splitlines() if line.startswith("orca result ")]
            if len(results) != 1:
                raise RuntimeError(f"report replay applied {len(results)} local results instead of one")
        print("PASS: engine report replay after lost receipt and rejoin; stable payload, one result per peer", flush=True)
    check = "receipt reconciliation" if args.expect_ranked_reconciliation else args.expect_queue_ending or "same-seat recovery"
    print(f"PASS: {args.queue} queue {check}; "
          f"room drops {[(d['player'], d['seconds']) for d in drops]}", flush=True)


def run(args):
    # Preserve callers of this runner that predate the optional queue/version fields.
    for name, default in {"role_binary": [], "expect_version_refusal": [], "queue": None,
                          "expect_queue_ending": None, "expect_report_replay": False,
                          "expect_ranked_reconciliation": False, "expect_fight": [],
                          "local": [], "scratch": None, "friend_from_queue": None}.items():
        if not hasattr(args, name):
            setattr(args, name, default)
    address = urllib.parse.urlsplit(args.site)
    if address.scheme != "http" or not isolated_host(address.hostname) or address.username or address.password:
        raise ValueError("--site must be a loopback or isolated LAN dev fixture")
    if args.hold_expiry and args.seconds < 120:
        raise ValueError("--hold-expiry needs at least 120 seconds for the 90-second hold")
    roles = [int(role) for role in args.roles.split(",")]
    local = local_counts(args.local if isinstance(args.local, list) else [])
    if args.friend_from_queue is not None and (args.friend_from_queue not in roles or args.queue or local):
        raise ValueError("--friend-from-queue needs a single-seat friends joiner in this run")
    if (args.queue or getattr(args, "ordinary", False)) and local:
        raise ValueError("local players require friends doubles")
    if not set(local) <= set(roles) or sum(local.get(role, 1) for role in roles) > 4:
        raise ValueError("local players must belong to this run and fit its four seats")
    if local and len(roles) > 3:
        raise ValueError("co-seat runs use at most three headless instances")
    ports_by_role = {role: role + 1 + sum(count - 1 for owner, count in local.items() if owner < role)
                     for role in roles}
    if len(set(roles)) != len(roles) or not set(roles) <= set(range(4)):
        raise ValueError("roles must be distinct values from 0..3")
    binaries = role_values(args.role_binary)
    if not args.binary and any(str(role) not in binaries for role in roles):
        raise ValueError("each role needs --binary or --role-binary")
    version_refused = role_values(args.expect_version_refusal)
    if not set(map(int, version_refused)) <= set(roles) - {0}:
        raise ValueError("expected version refusals must name joiners in --roles")
    if any(code not in {"host_older", "host_newer", "lobby_mismatch"} for code in version_refused.values()):
        raise ValueError("expected version refusal must be host_older, host_newer or lobby_mismatch")
    if version_refused and (args.defer_role or args.events):
        raise ValueError("version refusal checks start all roles directly")
    version_refused_seen = set()
    players = []
    role_inputs = dict(value.split("=", 1) for value in args.input_role)
    role_blackouts = dict(value.split("=", 1) for value in args.blackout_role)
    events = json.loads(Path(args.events).read_text()) if args.events else []
    deferred = set(args.defer_role)
    if not deferred <= set(roles) or any(event.get("role", 0) not in roles for event in events):
        raise ValueError("deferred/event roles must belong to --roles")
    refused = set(args.refused_role)
    refused_seen = set()
    queue_errors = {}
    with tempfile.TemporaryDirectory(prefix="orca-doubles-", dir=args.scratch) as work:
        try:
            print(f"Fixture PID: {os.getpid()}", flush=True)
            if getattr(args, "dump_stacks", False):
                faulthandler.register(signal.SIGUSR1, file=sys.stderr, all_threads=True)

            def version_refusal(role, text):
                errors = [line for line in text.splitlines() if line.startswith("orca error ")]
                for line in errors:
                    code = version_refused.get(str(role))
                    sentence = {"host_older": HOST_OLDER, "host_newer": HOST_NEWER,
                                "lobby_mismatch": LOBBY_MISMATCH}.get(code)
                    if sentence and line == f"orca error room_mismatch {sentence}":
                        version_refused_seen.add(str(role))
                        print(f"peer {role}: {line}", flush=True)
                    else:
                        raise RuntimeError(f"peer {role}: {line}")
                return str(role) in version_refused_seen

            def spawn(role):
                user = Path(work) / f"user-{role}"
                user.mkdir()
                environment = {"PATH": os.environ.get("PATH", ""),
                               "ORCA_SESSION": "1", "ORCA_TEST_COMMANDS": "1", "ORCA_TEST_ROOM_DROP": "1",
                               "ORCA_TEST_DEV_GAME": "orca-doubles-test", "ORCA_SITE": args.site,
                               "ORCA_ROOM": args.room, "ORCA_JOIN": "0" if role == 0 else "1",
                               "ORCA_TEST_PLAYER": f"Doubles{role}", "ORCA_NAME": f"Doubles{role}",
                               "ORCA_DIRECT": "1", "ORCA_DIRECT_LOOPBACK": "1", "ORCA_DIRECT_STUN": "0"}
                if role in local:
                    environment["YOUGAME_STDIO"] = "1"
                    environment["YOUGAME_GAME"] = "doubles-fixture"
                if args.queue or role == args.friend_from_queue:
                    # Public matchmaking is entered by host/join from the game's own lobby.
                    # ORCA_ROOM's startup shortcut intentionally does not arm the queue header.
                    environment["ORCA_ROOM"] = f"qhome{role}"
                    environment["ORCA_JOIN"] = "0"
                # Headless peers still start the input layer: never let one open a controller or a
                # GameCube adapter that is plugged into this machine (it is somebody's).
                environment.update({"ORCA_NO_LOCAL_INPUT": "1",
                                    "SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0",
                                    "SDL_HIDAPI_LIBUSB": "0", "SDL_JOYSTICK_IOKIT": "0", "SDL_JOYSTICK_MFI": "0",
                                    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0000"})
                if args.transport == "turn":
                    environment["ORCA_DIRECT_FORCE"] = "turn"
                if os.name == "nt":
                    environment["SystemRoot"] = os.environ.get("SystemRoot", r"C:\Windows")
                environment["YG_SCENES"] = "1"
                if role_blackouts:
                    # The knob adds test=true to the compatibility key. Keep every peer in
                    # that test setup, while only the requested role blacks out during the run.
                    environment["ORCA_TEST_BLACKOUT"] = role_blackouts.get(
                        str(role), "1000000-1000001")
                if args.probe:
                    environment["ORCA_UX_PROBE"] = str(Path(args.probe).resolve())
                input_path = role_inputs.get(str(role), args.input)
                if input_path and not local:
                    environment["YG_INPUT"] = str(Path(input_path).resolve())
                if args.profile or args.pplus:
                    environment["ORCA_PROFILE"] = "PPLUS32" if args.pplus else args.profile
                if args.mod_dir:
                    environment["ORCA_MOD_DIR"] = str(Path(args.mod_dir).resolve())
                    environment["ORCA_SD_CACHE"] = str(Path(work) / "sd-cache")
                stderr = open(Path(work) / f"peer-{role}.log", "w")
                binary = binaries.get(str(role), args.binary)
                print(f"peer {role} binary: {binary}", flush=True)
                command = [file_argument(binary), "-p", "headless", "-u", str(user), "-v", "Null",
                           "-C", "Dolphin.DSP.Backend=No Audio Output",
                           "-C", "Dolphin.Input.BackgroundInput=True", "-C", "Logger.Logs.ROLLBACK=True",
                           "-C", "Logger.Logs.NETPLAY=True", "-C", "Logger.Options.Verbosity=3"]
                if args.pplus:
                    command += ["-C", f"Dolphin.Core.DefaultISO={file_argument(args.disc)}", "-e", file_argument(args.pplus)]
                else:
                    command += ["-e", file_argument(args.disc)]
                process = subprocess.Popen(command, env=environment, cwd=user, stdin=subprocess.PIPE,
                                           stdout=subprocess.PIPE, stderr=stderr, text=True, bufsize=1)
                lines = queue.Queue()
                page = FixturePage(args.site, role, role in local)
                def read_output(proc=process, output=lines, role_number=role, page=page):
                    with open(Path(work) / f"peer-{role_number}.stdout.log", "w") as log:
                        for line in proc.stdout:
                            log.write(line)
                            log.flush()
                            output.put(line.rstrip())
                            if role_number in local:
                                replies = page.receive(line)
                                if replies:
                                    proc.stdin.write("".join(replies))
                                    proc.stdin.flush()
                threading.Thread(target=read_output, daemon=True).start()
                players.append((role, process, stderr, lines))
                caps = role_caps(role, local, args.queue)
                if role == args.friend_from_queue:
                    caps = caps.rstrip() + " host queue2 pick-casual\n"
                process.stdin.write(caps)
                if input_path and local:
                    # One script may contain independent rules for both of this role's ports.
                    process.stdin.write("test-input " + str(Path(input_path).resolve()) + "\n")
                process.stdin.flush()
                # Tickets assign seats in arrival order. Wait for every role, since a slow
                # ticket for role 1 must not let role 2 claim its port and invalidate pad scripts.
                deadline = time.monotonic() + 90
                queue_commanded = False
                while time.monotonic() < deadline:
                    if version_refused:
                        output = Path(work) / f"peer-{role}.stdout.log"
                        if output.exists() and version_refusal(role, output.read_text()):
                            return
                    with open(Path(work) / f"peer-{role}.log") as log:
                        text = log.read()
                    if args.queue and not queue_commanded and re.search(r"Scene frame .* -> scSelctCharacter", text):
                        process.stdin.write(f"{'host' if role == 0 else 'join'} {args.room}\n")
                        process.stdin.flush()
                        queue_commanded = True
                    if (role == args.friend_from_queue and not queue_commanded and
                            "Online menu: exit code 30: orca menu online casual" in text and
                            re.search(r"Scene frame .* -> scSelctCharacter", text)):
                        process.stdin.write(f"join {args.room}\n")
                        process.stdin.flush()
                        queue_commanded = True
                        print(f"peer {role}: joining friend from the casual queue select", flush=True)
                    assignment = room_assignment(text, args.room,
                                                 args.queue or role == args.friend_from_queue)
                    if assignment:
                        if int(assignment[1]) != ports_by_role[role]:
                            raise RuntimeError(f"peer {role} owns unexpected seat {assignment[1]}")
                        if role in local:
                            with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                                roster = json.load(response)
                            if len(roster.get("local_seats", {}).get(f"Doubles{role}", [])) != 1:
                                time.sleep(0.1)
                                continue
                        break
                    if process.poll() is not None:
                        raise RuntimeError(f"peer {role} exited before owning its room seat")
                    time.sleep(0.1)
                else:
                    raise RuntimeError(f"peer {role} never owned its room seat")
            for role in roles:
                if role not in deferred:
                    spawn(role)
            if version_refused:
                if version_refused_seen != set(version_refused):
                    raise RuntimeError("expected version refusals were not observed")
                for role, _, _, _ in players:
                    version_refusal(role, (Path(work) / f"peer-{role}.stdout.log").read_text())
                    if str(role) in version_refused:
                        text = (Path(work) / f"peer-{role}.log").read_text()
                        if "plugged into port" in text or re.search(r"on seat (\d+)", text):
                            raise RuntimeError(f"refused peer {role} acquired a seat")
                print("PASS: requested room_mismatch sentences reached the joiners; no refused peer seated")
                return
            print(f"Running roles {args.roles}; isolated logs: {work}", flush=True)
            # Operator commands are scoped to one exact harness child, e.g. `3 test-drop-room`.
            def commands():
                for line in sys.stdin:
                    role_text, separator, command = line.rstrip().partition(" ")
                    if separator and role_text.isdigit():
                        for role, process, _, _ in players:
                            if role == int(role_text) and process.poll() is None:
                                try:
                                    process.stdin.write(command + "\n")
                                    process.stdin.flush()
                                except OSError:
                                    pass
            threading.Thread(target=commands, daemon=True).start()
            deadline = time.monotonic() + args.seconds
            expire_at = time.monotonic() + max(15, (args.seconds - 105) / 2) if args.hold_expiry else None
            expired = False
            while time.monotonic() < deadline:
                for event in events:
                    if event.get("done"):
                        continue
                    if "armed" not in event:
                        source = Path(work) / f"peer-{event.get('source', 0)}.log"
                        text = source.read_text() if source.exists() else ""
                        if len(re.findall(event["pattern"], text)) < event.get("occurrence", 1):
                            continue
                        event["armed"] = time.monotonic()
                    if time.monotonic() < event["armed"] + event.get("delay", 0):
                        continue
                    role = event.get("role", 0)
                    if event["action"] == "spawn":
                        if any(existing == role for existing, _, _, _ in players):
                            raise RuntimeError(f"duplicate scheduled role {role}")
                        spawn(role)
                    elif event["action"] == "command":
                        process = next(proc for number, proc, _, _ in players if number == role)
                        process.stdin.write(event["command"] + "\n")
                        process.stdin.flush()
                    elif event["action"] == "drop":
                        post_fixture(args.site, f"/drop/{args.room}",
                                     {"player": f"Doubles{role}", "seconds": event.get("seconds", 15)})
                    elif event["action"] in ("away", "back"):
                        post_fixture(args.site, f"/{event['action']}/{args.room}", {"player": f"Doubles{role}"})
                    elif event["action"] == "fifth":
                        try:
                            client(args.site, "Doubles4", args.room)
                            raise RuntimeError("fifth arrival was accepted")
                        except urllib.error.HTTPError as error:
                            if error.code != 409:
                                raise
                    else:
                        raise ValueError("unknown fixture event action")
                    event["done"] = True
                    print(f"EVENT: {event}", flush=True)
                if expire_at and not expired and time.monotonic() >= expire_at and 3 in roles:
                    process = next(proc for role, proc, _, _ in players if role == 3)
                    process.stdin.write("test-direct off\n")
                    process.stdin.flush()
                    post_fixture(args.site, "/blackout/" + args.room, {"player": "Doubles3"})
                    expired = True
                for role, process, _, lines in players:
                    if args.hold_expiry and role == 3 and expired:
                        continue
                    if process.poll() is not None:
                        raise RuntimeError(f"peer {role} exited")
                    while not lines.empty():
                        line = lines.get()
                        if line.startswith("orca error"):
                            if role in refused and line.startswith("orca error room_full"):
                                refused_seen.add(role)
                                print(f"peer {role}: {line}", flush=True)
                                continue
                            if args.expect_queue_ending and line.startswith("orca error network "):
                                queue_errors[role] = line
                                print(f"peer {role}: {line}", flush=True)
                                continue
                            raise RuntimeError(f"peer {role}: {line}")
                        if line.startswith("orca stats ") and json.loads(line[11:]).get("ds", 0):
                            raise RuntimeError(f"peer {role} reported a desync")
                        if line.startswith(("orca state", "orca menu")):
                            print(f"peer {role}: {line}", flush=True)
                time.sleep(0.1)
            if any(not event.get("done") for event in events):
                raise RuntimeError("scheduled fixture events did not run")
            if refused_seen != refused:
                raise RuntimeError("expected room_full refusals were not observed")
            # Each capture's joiners must plug in together; later captures have their own frame.
            with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                offers = json.load(response)["offers"]
            frames = []
            ports = []
            for role, _, _, _ in players:
                if role in refused:
                    text = (Path(work) / f"peer-{role}.log").read_text()
                    if "plugged into port" in text:
                        raise RuntimeError("refused newcomer was plugged in")
                    continue
                if role:
                    text = (Path(work) / f"peer-{role}.log").read_text()
                    assignment = room_assignment(text, args.room,
                                                 args.queue or role == args.friend_from_queue)
                    match = re.search(r"plugged into port (\d+) at frame (\d+)", text)
                    if (not assignment or not match or int(match[1]) not in (2, 3, 4) or
                            match[1] != assignment[1]):
                        raise RuntimeError(f"peer {role} did not plug into its assigned port")
                    ports.append(int(match[1]))
                    frames.append(int(match[2]))
                    capture = next((capture for target, capture, _ in offers if target == f"Doubles{role}"), "initial")
                    report = {"player": f"Doubles{role}", "port": int(match[1]), "plug": int(match[2]), "capture": capture}
                else:
                    report = {"player": "Doubles0", "port": 1, "plug": None}
                post_fixture(args.site, "/report/" + args.room, report)
            if len(set(ports)) != len(ports):
                raise RuntimeError("joiners were assigned duplicate ports")
            report_deadline = time.monotonic() + 90
            while True:
                with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                    diagnostics = json.load(response)
                if len(diagnostics["reports"]) == args.expected:
                    break
                if time.monotonic() > report_deadline:
                    raise RuntimeError("missing cross-machine reports")
                time.sleep(0.2)
            if refused:
                diagnostics["active"] = [name for name in diagnostics["active"]
                                         if name not in {f"Doubles{role}" for role in refused}]
            for role in local:
                if not role_inputs.get(str(role), args.input):
                    continue
                name = f"Doubles{role}"
                expected_ports = {ports_by_role[role] - 1, *diagnostics["local_seats"].get(name, [])}
                if not expected_ports <= set(diagnostics["input_activity"].get(name, [])):
                    raise RuntimeError(f"peer {role} did not drive both scripted local ports")
            if args.queue:
                verify_queue_run(args, work, diagnostics, queue_errors)
            if args.expect_queue_ending or args.expect_report_replay or args.expect_ranked_reconciliation:
                samples = list(diagnostics["hashes"].values())
                common = set.intersection(*(set(sample) for sample in samples)) if len(samples) == args.expected else set()
                if not common:
                    raise RuntimeError("ending fixture has no common confirmed checksum samples")
                for sample in common:
                    if len({values[sample] for values in samples}) != 1:
                        raise RuntimeError(f"pre-ending checksum mismatch at {sample}")
                final_frame = f"{max(map(int, common))} ({len(common)} common pre-ending samples)"
            else:
                final_frame = verify_diagnostics(diagnostics, expected=args.expected)
            verify_gameplay(work, args.expect_fight)
            samples = [diagnostics["hashes"].get(name, {}) for name in diagnostics["active"]]
            common = set.intersection(*(set(sample) for sample in samples)) if samples else set()
            print(f"PASS: all machines agree on plug {frames}, {len(common)} common confirmed hashes through frame {final_frame}; "
                  f"0 desyncs; {len(args.expect_fight)} requested fights returned to CSS")
        finally:
            if args.keep_logs:
                target = Path(args.keep_logs)
                target.mkdir(parents=True, exist_ok=True)
                try:
                    with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                        (target / "diagnostics.json").write_bytes(response.read())
                except OSError as error:
                    (target / "diagnostics-error.txt").write_text(type(error).__name__ + "\n")
            for _, process, stderr, _ in players:
                if process.poll() is None:
                    try:
                        process.stdin.write("quit\n")
                        process.stdin.flush()
                        process.wait(timeout=10)
                    except (OSError, subprocess.TimeoutExpired):
                        process.kill()  # Only this harness's exact child PID.
                        process.wait()
                stderr.close()
            if args.keep_logs:
                import shutil
                target = Path(args.keep_logs)
                target.mkdir(parents=True, exist_ok=True)
                for log in Path(work).glob("peer-*.log"):
                    shutil.copyfile(log, target / log.name)
                for role in roles:
                    for probe in (Path(work) / f"user-{role}").rglob("probe-*.bin"):
                        shutil.copyfile(probe, target / f"peer-{role}-{probe.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--self-test", action="store_true")
    mode.add_argument("--receipt-self-test", action="store_true")
    mode.add_argument("--serve", action="store_true")
    mode.add_argument("--run", action="store_true")
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--site", default="http://127.0.0.1:8765")
    parser.add_argument("--room", default="doubles444")
    parser.add_argument("--roles", default="0,1,2,3")
    parser.add_argument("--binary")
    parser.add_argument("--role-binary", action="append", default=[], metavar="ROLE=PATH",
                        help="override --binary for one role")
    parser.add_argument("--role-version", action="append", default=[], metavar="ROLE=X.Y.Z",
                        help="page-supplied ticket ob for a role in the local fixture")
    parser.add_argument("--version-refusals", action="store_true",
                        help="refuse incompatible lobbies at the WebSocket upgrade with Worker bodies")
    parser.add_argument("--owner-relaunch", action="store_true",
                        help="start an owner's empty room over on another lobby; enables version checks")
    parser.add_argument("--expect-version-refusal", action="append", default=[], metavar="ROLE=CODE",
                        help="require the exact room_mismatch sentence instead of a gameplay session")
    parser.add_argument("--disc")
    parser.add_argument("--input")
    parser.add_argument("--input-role", action="append", default=[], metavar="ROLE=PATH",
                        help="override --input for one role; an empty script keeps it idle")
    parser.add_argument("--blackout-role", action="append", default=[], metavar="ROLE=FROM-TO",
                        help="set ORCA_TEST_BLACKOUT for one role")
    parser.add_argument("--probe", help="read-only gameplay evidence probe script")
    parser.add_argument("--dump-stacks", action="store_true",
                        help="debug this fixture's threads with SIGUSR1 (POSIX only)")
    parser.add_argument("--expect-fight", action="append", default=[], type=lambda value: int(value, 0),
                        help="human-port bitmask for each fight; requires probe-fight-N.bin and return to CSS")
    parser.add_argument("--events", help="JSON log-triggered events: pattern, source/role, delay; "
                        "action=spawn|command|away|back|drop (seconds, default 15); drop severs only room socket")
    parser.add_argument("--queue", choices=("casual", "ranked"),
                        help="two-player public room; welcome room.queue plus room.recovery:true")
    parser.add_argument("--hold-ms", type=int, help="fake server hold remaining milliseconds; omit for legacy 90 s fallback")
    parser.add_argument("--verdict-delay", type=float, default=0, help="deliver the server verdict this many seconds after its hold")
    parser.add_argument("--expect-crossed-holds", action="store_true", help="verify small self WELCOME remainder while a peer retains its later hold")
    parser.add_argument("--expect-report-replay", action="store_true", help="require lost receipt, stable report replay after rejoin, and one local result per peer")
    parser.add_argument("--expect-ranked-reconciliation", action="store_true", help="lose the first ranked game receipt until after the final; verify both games once and final score")
    parser.add_argument("--drop-report-receipts", type=int, default=0, help="lose this many report receipt broadcasts; retained receipts replay on retry")
    parser.add_argument("--no-recovery", action="store_true", help="omit recovery from the queue welcome (legacy policy)")
    parser.add_argument("--expect-queue-ending", choices=("network", "forfeit"),
                        help="verify legacy network ending or the fixture's ranked forfeit after a 90 s hold")
    parser.add_argument("--scratch", help="directory for isolated temporary users/logs")
    parser.add_argument("--friend-from-queue", type=int, choices=range(1, 4),
                        help="join the friend's room only after this role reaches the casual queue select")
    parser.add_argument("--defer-role", action="append", default=[], type=int, choices=range(4))
    parser.add_argument("--refused-role", action="append", default=[], type=int, choices=range(1, 4),
                        help="require room_full for only this newcomer, which must never plug in")
    parser.add_argument("--expected", type=int, default=4, choices=(2, 3, 4))
    parser.add_argument("--doubles", action="store_true", help="accepted for dropin-rooms compatibility")
    parser.add_argument("--ordinary", action="store_true", help="serve an ordinary friends room")
    parser.add_argument("--local", nargs="?", const="server", action="append", default=[], metavar="ROLE=2",
                        help="own a loopback server; optionally add and script a role's second local seat")
    game = parser.add_mutually_exclusive_group()
    game.add_argument("--profile", help="launcher profile, e.g. PPLUS32")
    game.add_argument("--pplus", help="legacy P+ launcher DOL with sd.raw beside it; --disc is Brawl")
    game.add_argument("--mod-dir", help="local prepared mod bundle; no download or install")
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--keep-logs")
    parser.add_argument("--turn-binary", help="locally built orca-turn-fixture for --serve or --self-test")
    parser.add_argument("--transport", choices=("direct", "turn"), default="direct")
    parser.add_argument("--hold-expiry", action="store_true", help="expire role 3 halfway through the run")
    args = parser.parse_args()
    versions = role_values(args.role_version)
    if any(not clean_version(version) for version in versions.values()):
        parser.error("--role-version needs X.Y.Z versions")
    if args.queue:
        if local_counts(args.local):
            parser.error("--local ROLE=2 requires friends doubles")
        if args.roles not in {"0,1", "0,1,2,3"}:
            parser.error("--queue runs exactly roles 0,1")
        args.roles = "0,1"
        args.expected = 2
    if args.no_recovery and not args.queue:
        parser.error("--no-recovery requires --queue")
    if args.receipt_self_test:
        receipt_self_test()
    elif args.self_test:
        self_test(args.turn_binary)
    elif args.serve:
        if not isolated_host(args.listen):
            parser.error("--listen must be loopback or an isolated LAN address")
        server = Server((args.listen, args.port), args.turn_binary, doubles=not args.ordinary,
                        version_refusals=args.version_refusals, owner_relaunch=args.owner_relaunch,
                        role_versions=versions, queue_mode=args.queue, recovery=not args.no_recovery, queue_room=args.room)
        server.rooms.hold_ms = args.hold_ms
        server.rooms.verdict_delay = args.verdict_delay
        server.rooms.peer_away_after_rejoin = 20 if args.expect_crossed_holds else None
        server.rooms.drop_receipts = args.drop_report_receipts
        server.rooms.lose_ranked_game_receipt = args.expect_ranked_reconciliation
        print(f"Dev friends-room fixture listening on port {server.server_port}", flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    else:
        if not (args.binary or args.role_binary) or not args.disc:
            parser.error("--run requires --binary/--role-binary and --disc")
        server = None
        try:
            if args.local:
                server = Server(("127.0.0.1", 0), args.turn_binary, doubles=not args.ordinary,
                                version_refusals=args.version_refusals, owner_relaunch=args.owner_relaunch,
                                role_versions=versions, queue_mode=args.queue, recovery=not args.no_recovery, queue_room=args.room)
                server.rooms.hold_ms = args.hold_ms
                server.rooms.verdict_delay = args.verdict_delay
                server.rooms.peer_away_after_rejoin = 20 if args.expect_crossed_holds else None
                server.rooms.drop_receipts = args.drop_report_receipts
                server.rooms.lose_ranked_game_receipt = args.expect_ranked_reconciliation
                threading.Thread(target=server.serve_forever, daemon=True).start()
                args.site = f"http://127.0.0.1:{server.server_port}"
            run(args)
        finally:
            if server:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
