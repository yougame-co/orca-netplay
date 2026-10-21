#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A stand-in for YouGame's keyframe endpoint (ORCA.md, "Keyframe store"), for the OrcaKeyframe
# tests: python3 Tools/orca/fake_keyframes.py <port>, then
# ORCA_TEST_KEYFRAME_SERVER=http://127.0.0.1:<port> ./Binaries/Tests/tests --gtest_filter='OrcaKeyframe*'
#
#   PUT    /api/orca/keyframes/<room>/<id>   Authorization: Ticket good; X-Orca-Frame, X-Orca-Hash;
#                                            409 {"code": "exists"} for an id already there
#   GET    /api/orca/keyframes/<room>/<id>   404 {"code": "gone"} when absent
#   DELETE /api/orca/keyframes/<room>/<id>
# Ticket "guest" gets 403 {"code": "signed_out"}, "edge" a 403 page with no code (the edge's), any
# other 401 {"code": "ticket"}. Room "flaky" answers 503 to every other request.
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

store = {}
flaky = {"n": 0}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def parts(self):
        p = self.path.strip("/").split("/")
        return p

    def check(self):
        p = self.parts()
        if len(p) >= 5 and p[3] == "flaky":
            flaky["n"] += 1
            if flaky["n"] % 2 == 1:
                self.reply(503, b'{"error":"busy"}')
                return False
        if self.headers.get("Authorization") == "Ticket guest":
            self.reply(403, b'{"error":"Sign in to share a game.","code":"signed_out"}')
            return False
        if self.headers.get("Authorization") == "Ticket edge":
            self.reply(403, b"<html>Access denied</html>", "text/html")
            return False
        if self.headers.get("Authorization") != "Ticket good":
            self.reply(401, b'{"error":"Bad or expired ticket","code":"ticket"}')
            return False
        return True

    def body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n)

    def do_PUT(self):
        if not self.check():
            return
        p = self.parts()
        data = self.body()
        frame, digest = self.headers.get("X-Orca-Frame"), self.headers.get("X-Orca-Hash", "")
        if not frame or len(digest) != 16 or not p[4].endswith("-" + digest[:8]):
            return self.reply(400, b'{"error":"bad keyframe headers","code":"bad_request"}')
        if (p[3], p[4]) in store:
            return self.reply(409, b'{"error":"That keyframe is already stored","code":"exists"}')
        store[(p[3], p[4])] = data
        self.reply(200, json.dumps({"id": p[4], "size": len(data)}).encode())

    def do_GET(self):
        if not self.check():
            return
        p = self.parts()
        data = store.get((p[3], p[4]))
        if data is None:
            return self.reply(404, b'{"error":"That keyframe is gone","code":"gone"}')
        self.reply(200, data, "application/octet-stream")

    def do_DELETE(self):
        if not self.check():
            return
        p = self.parts()
        existed = store.pop((p[3], p[4]), None) is not None
        self.reply(200 if existed else 404, b"{}")


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
