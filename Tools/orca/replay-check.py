#!/usr/bin/env python3
# replay-check.py: saved matches and live watching (Session/Keyframe.h "Replay streams",
# Rollback/OnlineMatch.cpp "Replay streams"). Two players over a real YouGame room (dev tickets, a
# local keyframe folder), a third Orca watching them live, then replays of what they recorded.
# Headless and muted.
#
#   Tools/orca/replay-check.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#                              [--match-seconds N] [--no-watch] [--late-watch S]
#                              [--net-delay MS] [--net-jitter MS] [--realtime]
#                              [--video Metal --shots MS --shot-seconds S] [--relay <site>]
#                              [--watch-drop S]
#
# 1. Host and Friend each boot their own game, writing their history to a replay stream
#    (ORCA_TEST_REPLAY_OUT). Friend drops into Host's character select (as dropin-identity.py), and
#    both play a fuzzed match for --match-seconds (60). Neither may report a desync.
# 2. Live: once Friend is in, Watcher plays Host's stream as it grows (ORCA_TEST_REPLAY_FOLLOW),
#    12 frames behind, catching up from Host's origin first. Its RAM must match every checksum the
#    stream carries, and it reports how far behind live it ran. A second watcher joins --late-watch
#    seconds into the fight (30) and reports how long it took to catch up.
#    --net-delay/--net-jitter hold Friend's side of the room (ORCA_TEST_NET_DELAY_MS/_JITTER_MS), so
#    the players roll back, and the recordings must still be exact.
# 3. Saved: fresh Orcas replay Host's file and Friend's file flat out, and Host's again with a seek
#    back (ORCA_TEST_REPLAY_SEEK); every checksum must match. Host's replay also repacks the file
#    into one compact stream (ORCA_TEST_REPLAY_REPACK), which must replay the same (and in real
#    time with --realtime). A copy with another build's compatibility key and a copy with a
#    damaged byte must both be refused.
# 4. Host's and Friend's recordings of the shared match must hold the same frames (every seat's
#    inputs and typed events) and the same checksums.
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

import replay_stream

if len(sys.argv) < 3:
    sys.exit("usage: replay-check.py <nogui> <disc> [--pplus <dol>] [--match-seconds N] [--no-watch]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
MATCH_SECONDS = int(sys.argv[sys.argv.index("--match-seconds") + 1]) if "--match-seconds" in sys.argv else 60
WATCH = "--no-watch" not in sys.argv
LATE = int(sys.argv[sys.argv.index("--late-watch") + 1]) if "--late-watch" in sys.argv else 30
DELAY = sys.argv[sys.argv.index("--net-delay") + 1] if "--net-delay" in sys.argv else None
JITTER = sys.argv[sys.argv.index("--net-jitter") + 1] if "--net-jitter" in sys.argv else "0"
REALTIME = "--realtime" in sys.argv
# --video Metal renders Host and Watcher off screen (headless) and --shots asks both for a screenshot
# every MS ms for --shot-seconds of the fight (test-shot), for a side-by-side video of what the
# player and the live watcher saw (side-by-side.mp4 in the work folder, kept with ORCA_TEST_KEEP=1).
VIDEO = sys.argv[sys.argv.index("--video") + 1] if "--video" in sys.argv else "Null"
SHOTS = int(sys.argv[sys.argv.index("--shots") + 1]) if "--shots" in sys.argv else 0
SHOT_SECONDS = int(sys.argv[sys.argv.index("--shot-seconds") + 1]) if "--shot-seconds" in sys.argv else 20
# --relay <site>: everything over a local rooms stack (rooms/scripts/replay-stub-site.mjs in YouGame,
# with `wrangler dev` rooms): Host also streams to its room (ORCA_TEST_REPLAY_ROOM), and both
# watchers watch through the rooms Worker's /watch/<code> with a watch ticket from that site,
# instead of reading Host's file. Watcher's connection drops once, --watch-drop seconds (20; 0: never)
# after it connected, and must come back on a fresh ticket and play on with every checksum matching.
RELAY = sys.argv[sys.argv.index("--relay") + 1].rstrip("/") if "--relay" in sys.argv else None
WATCH_DROP = sys.argv[sys.argv.index("--watch-drop") + 1] if "--watch-drop" in sys.argv else "20"
DEV = "orca-replay-test"


def watch_url():
    import json
    import urllib.request
    req = urllib.request.Request(f"{RELAY}/api/multiplayer/ticket", method="POST",
                                 data=json.dumps({"dev": DEV, "watch": ROOM}).encode(),
                                 headers={"content-type": "application/json"})
    reply = json.loads(urllib.request.urlopen(req, timeout=10).read())
    base = reply["url"].replace("https://", "wss://").replace("http://", "ws://").rstrip("/")
    return f"{base}{reply['watch']}?ticket={reply['ticket']}"


def net(side):
    """The network test knobs for one process: real ones on Friend's side, 0 elsewhere (the knobs
    are part of the compatibility key, so every process sets them alike)."""
    if not DELAY:
        return {}
    if side == "Friend":
        return {"ORCA_TEST_NET_DELAY_MS": DELAY, "ORCA_TEST_NET_JITTER_MS": JITTER}
    return {"ORCA_TEST_NET_DELAY_MS": "0", "ORCA_TEST_NET_JITTER_MS": "0"}
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "pplus-identity.txt" if PPLUS else "bf-identity.txt")
ROOM = "rpl" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-replay-")
T0 = time.time()
results = []


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra, inputs=True):
        self.name = name
        self.lines = queue.Queue()
        self.states = []
        self.replay_out = []
        self.done = None
        self.user = os.path.join(WORK, f"user-{name}")
        os.makedirs(self.user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_DEV_GAME": DEV,
            "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
            "ORCA_NAME": name,
            "YG_SCENES": "1",
        }
        if inputs:
            env["YG_INPUT"] = INPUT
        if PPLUS:
            env["ORCA_PROFILE"] = "PPLUS32"
        # ORCA_TEST_SNAPSHOT_EVERY=<n>: the session snapshots every n frames, as a slow machine's does
        # (Windows: every 2-4), so rollbacks restore from before frames already recorded.
        for key in ("ORCA_SITE", "SDL_HIDAPI", "SDL_JOYSTICK_HIDAPI", "ORCA_TEST_SNAPSHOT_EVERY"):
            if os.environ.get(key):
                env[key] = os.environ[key]
        if RELAY:
            env["ORCA_SITE"] = RELAY
        env.update(net(name))
        env.update(env_extra)
        video = VIDEO if name in ("Host", "Watcher") else "Null"
        args = [BIN, "-p", "headless", "-u", self.user, "-v", video,
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Logs.CORE=True", "-C", "Logger.Options.Verbosity=2"]
        if PPLUS:
            args += ["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS]
        else:
            args += ["-e", DISC]
        self.err = open(os.path.join(WORK, f"{name}.log"), "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats ") or line.startswith("orca perf "):
                continue
            if line.startswith("orca state "):
                self.states.append(line[len("orca state "):])
            if line.startswith("orca replay-out "):
                self.replay_out.append(line[len("orca replay-out "):])
            if line.startswith("orca replay done "):
                self.done = line
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

    def wait_done(self, timeout):
        end = time.time() + timeout
        while time.time() < end and self.done is None:
            if self.proc.poll() is not None and self.done is None:
                time.sleep(0.5)
                break
            time.sleep(0.5)
        return self.done

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


def check(name, ok, detail):
    results.append((name, ok, detail))
    log(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")


def fields(done_line):
    head, _, why = done_line.partition(" why=")
    out = {}
    for part in head.split()[4:]:
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    out["ok"] = head.split()[3] == "ok"
    out["why"] = why
    return out


def short(line):
    return line[line.find("N[") if "N[" in line else 0:]


def replay(name, path, env_extra=None, timeout=600):
    env = {"ORCA_TEST_REPLAY_IN": path}
    env.update(env_extra or {})
    o = Orca(name, env, inputs=False)
    orcas.append(o)
    o.expect("orca caps", 30)
    o.send("caps stats")
    done = o.wait_done(timeout)
    o.stop()
    if not done:
        fail(f"{name}: no 'orca replay done' line")
    return fields(done)


log(f"room {ROOM}, work {WORK}, relay {RELAY or 'no (file)'}, match {MATCH_SECONDS} s, watch {WATCH}, late watch {LATE} s, "
    f"net delay {DELAY or 0} ms + jitter {JITTER} ms on Friend's side")

# 1. Record a real match.
host = Orca("Host", {"ORCA_ROOM": ROOM, "ORCA_TEST_REPLAY_OUT": os.path.join(WORK, "rec-host"),
                     **({"ORCA_TEST_REPLAY_ROOM": "1"} if RELAY else {})})
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
host.expect("orca state playing", 90)
log(f"Host: {short(host.wait_log('-> scSelctCharacter', 240))}")
friend = Orca("Friend", {"ORCA_TEST_REPLAY_OUT": os.path.join(WORK, "rec-friend")})
orcas.append(friend)
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state playing", 90)
log(f"Friend: {short(friend.wait_log('-> scSelctCharacter', 240))}")
time.sleep(2)
friend.send(f"join {ROOM}")
friend.expect("orca state joining", 60)
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 180)
host.expect("orca state friend-joined", 60)
time.sleep(1)
if not host.replay_out:
    fail("Host wrote no replay stream")
host_file = host.replay_out[-1]
log(f"Host's stream: {host_file}")

# 2. Live: a watcher of Host's stream, from Host's origin.
watcher = None
if WATCH:
    # Over the relay, Watcher gets its own watch tickets (watch:<room>), so its connection can drop
    # (--watch-drop seconds after it connected, 20 by default) and come back with a fresh one.
    watcher = Orca("Watcher", {"ORCA_TEST_REPLAY_IN": (f"watch:{ROOM}" if WATCH_DROP != "0" else watch_url())
                                                      if RELAY else host_file,
                               "ORCA_TEST_REPLAY_FOLLOW": "1",
                               "ORCA_TEST_REPLAY_DELAY": "12",
                               **({"ORCA_TEST_REPLAY_WATCH_DROP": WATCH_DROP} if RELAY and WATCH_DROP != "0" else {})},
                   inputs=False)
    orcas.append(watcher)
    watcher.expect("orca caps", 30)
    watcher.send("caps stats")
    watcher.expect("orca replay start", 180)

for o in (host, friend):
    log(f"{o.name}: {short(o.wait_log('-> scMelee', 300))}")
log(f"the match runs for {MATCH_SECONDS} s")


def shoot():
    end = time.time() + SHOT_SECONDS
    n = 0
    while time.time() < end:
        n += 1
        for o, tag in ((host, "h"), (watcher, "w")):
            if o and o.proc.poll() is None:
                try:
                    o.proc.stdin.write(f"test-shot {tag}-{n:04d}\n")
                    o.proc.stdin.flush()
                except Exception:
                    pass
        time.sleep(SHOTS / 1000)


if SHOTS and VIDEO != "Null":
    threading.Thread(target=shoot, daemon=True).start()
late = None
if WATCH and 0 < LATE < MATCH_SECONDS:
    time.sleep(LATE)
    late = Orca("LateWatcher", {"ORCA_TEST_REPLAY_IN": watch_url() if RELAY else host_file,
                                "ORCA_TEST_REPLAY_FOLLOW": "1",
                                "ORCA_TEST_REPLAY_DELAY": "12"}, inputs=False)
    orcas.append(late)
    late.expect("orca caps", 30)
    late.send("caps stats")
    time.sleep(MATCH_SECONDS - LATE)
else:
    time.sleep(MATCH_SECONDS)
for o in (host, friend):
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped during the match")
    if o.log_lines("Desync at frame") or any("desync" in s for s in o.states):
        fail(f"{o.name} saw a desync: {o.log_lines('Desync at frame')[:3]}")
check("no desync between the players", True, f"{MATCH_SECONDS} s of fuzzed play")
# The last final frames reach the files within 100 ms.
time.sleep(1)
rollbacks = 0
for o in (host, friend):
    for line in o.log_lines("Online match ")[-1:]:
        log(f"{o.name}: {short(line)}")
        part = line.split(" rollbacks")[0].split(", ")[-1]
        rollbacks = max(rollbacks, int(part) if part.isdigit() else 0)
    o.stop()
log(f"rollbacks during the match: {rollbacks}")

if watcher:
    done = watcher.wait_done(300)
    watcher.stop()
    if not done:
        fail("Watcher: no 'orca replay done' line")
    w = fields(done)
    check("live watcher matches the players", w["ok"] and int(w["checked"]) > 0 and w["mismatched"] == "0",
          f"{w['checked']} checksums checked, {w['mismatched']} differ, {w['frames']} frames")
    if RELAY and WATCH_DROP != "0":
        back = watcher.log_lines("back on the match's stream")
        check("the live watcher's connection dropped and came back", w["ok"] and back,
              f"{len(back)} reconnect(s): {short(back[0]) if back else '-'}")
    check("live watcher latency", int(w["checked"]) > 0,
          f"{w['live_avg_ms']} ms behind the players on average ({w['live_max_ms']} ms at most); "
          f"{w['behind_avg_ms']} ms of it behind the stream; {w['waits']} waits for data")
if late:
    done = late.wait_done(300)
    late.stop()
    if not done:
        fail("LateWatcher: no 'orca replay done' line")
    lw = fields(done)
    check("a watcher joining mid-fight catches up and matches",
          lw["ok"] and int(lw["checked"]) > 0 and lw["mismatched"] == "0",
          f"caught up in {float(lw['caught_up_ms']) / 1000:.1f} s from the stream's start, then "
          f"{lw['live_avg_ms']} ms behind the players; {lw['checked']} checksums checked, "
          f"{lw['mismatched']} differ")

# 3. Saved replays.
packed = os.path.join(WORK, "packed.ygr")
h = replay("ReplayHost", host_file, {"ORCA_TEST_REPLAY_REPACK": packed})
check("replay of Host's file", h["ok"] and int(h["checked"]) > 0 and h["mismatched"] == "0",
      f"{h['frames']} frames, {h['checked']} checksums checked, {h['mismatched']} differ, "
      f"{float(h['wall_ms']) / 1000:.1f} s wall")

host_stream = replay_stream.read(host_file)
if os.path.exists(packed):
    p = replay("ReplayPacked", packed, {"ORCA_TEST_REPLAY_REALTIME": "1"} if REALTIME else None)
    packed_stream = replay_stream.read(packed)
    same = packed_stream.frames == host_stream.frames and packed_stream.checksums == host_stream.checksums
    check("a repacked replay is the same match, smaller",
          p["ok"] and p["mismatched"] == "0" and same,
          f"{p['checked']} checksums checked, {p['mismatched']} differ, frames and checksums "
          f"{'identical' if same else 'DIFFER'}; {os.path.getsize(host_file)} bytes live, "
          f"{os.path.getsize(packed)} packed ({replay_stream.summary(packed_stream)})"
          + (f"; played in real time in {float(p['wall_ms']) / 1000:.0f} s" if REALTIME else ""))
else:
    check("a repacked replay is the same match, smaller", False, "no packed file was written")
friend_file = None
for path in reversed(friend.replay_out):
    s = replay_stream.read(path)
    if s.info["origin_hash"] == host_stream.info["origin_hash"] and s.first == host_stream.first:
        friend_file, friend_stream = path, s
        break
if not friend_file:
    fail(f"Friend wrote no stream of Host's history (it wrote {friend.replay_out})")
f = replay("ReplayFriend", friend_file)
check("replay of Friend's file", f["ok"] and int(f["checked"]) > 0 and f["mismatched"] == "0",
      f"{f['frames']} frames, {f['checked']} checksums checked, {f['mismatched']} differ")

checked = sorted(host_stream.checksums)
if len(checked) > 30:
    a = checked[len(checked) // 3]
    b = a + 900
    s = replay("ReplaySeek", host_file, {"ORCA_TEST_REPLAY_SEEK": f"{a}:{b}"})
    check("replay with a seek back", s["ok"] and s["seek"] == "done" and s["mismatched"] == "0",
          f"back from frame {b} to {a}: {s['checked']} checksums checked, {s['mismatched']} differ")

data = open(host_file, "rb").read()
tampered = os.path.join(WORK, "tampered-compat.ygr")
i = data.index(b'"compat":"') + len(b'"compat":"') + 2
with open(tampered, "wb") as out:
    out.write(data[:i] + (b"x" if data[i:i + 1] != b"x" else b"y") + data[i + 1:])
t = replay("ReplayOtherBuild", tampered)
check("a replay from another build is refused", not t["ok"] and "another" in t.get("why", ""),
      f"why={t.get('why')}")
# One byte flipped in the middle (wherever it lands), and in the first chunk's live frame (a
# preamble field only the chunk's hash covers): both must be refused.
import struct
header_len = 12 + struct.unpack("<I", data[8:12])[0]
for name, j in (("middle", len(data) // 2), ("live-field", header_len + 12)):
    damaged = os.path.join(WORK, f"damaged-{name}.ygr")
    with open(damaged, "wb") as out:
        out.write(data[:j] + bytes([data[j] ^ 0x5A]) + data[j + 1:])
    d = replay(f"ReplayDamaged-{name}", damaged)
    check(f"a damaged replay is refused ({name})", not d["ok"] and "damaged" in d.get("why", ""),
          f"why={d.get('why')}")

# 4. Both players recorded the same match.
end = min(host_stream.end, friend_stream.end)
first = host_stream.first
diff = [i for i in range(end - first) if host_stream.frames[i] != friend_stream.frames[i]]
common = sorted(set(host_stream.checksums) & set(friend_stream.checksums))
cdiff = [c for c in common if host_stream.checksums[c] != friend_stream.checksums[c]]
if DELAY:
    check("the players rolled back during the recorded match", rollbacks > 0, f"{rollbacks} rollbacks")
# One file per history: each player's own boot, then the shared match. A rollback that re-runs
# frames already written must not start another (ORCA_TEST_SNAPSHOT_EVERY=3 shows it).
check("each player wrote one file per history", len(host.replay_out) == 2 and len(friend.replay_out) == 2,
      f"Host {len(host.replay_out)} files, Friend {len(friend.replay_out)}")
check("Host and Friend recorded the same frames", not diff and end - first > 0,
      f"frames {first}-{end}: {len(diff)} differ (first {diff[:5]})")
check("Host and Friend recorded the same checksums", common and not cdiff,
      f"{len(common)} in both, {len(cdiff)} differ")
log(f"Host's file: {replay_stream.summary(host_stream)}")
log(f"Friend's file: {replay_stream.summary(friend_stream)}")

failed = [r for r in results if not r[1]]
log(f"{len(results) - len(failed)} of {len(results)} checks passed")
if failed:
    fail(", ".join(r[0] for r in failed))
log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
