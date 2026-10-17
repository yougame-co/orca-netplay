#!/usr/bin/env python3
# replay-set-check.py: a whole set recorded, watched live and replayed (Rollback/ReplayStream.h), as
# a tournament set runs: two headless Orcas matched by the queue play a ranked best of 3 to its end
# (Tools/orca/queue-e2e.mjs --q2 --replay: character select, stage strikes, every game, results), over
# a local rooms stack. Headless and muted; run it through run-lock.py.
#
#   Tools/orca/replay-set-check.py <dolphin-emu-nogui> <disc> --site <url> [--pplus <launcher.dol>]
#       [--queue ranked|casual] [--games N] [--late-watch S] [--relay [--watch-drop S]]
#       [--scenario rejoin|kill|...] [--recorder ada|bo]
#
# --site is a local stand-in site with dev tickets and a local rooms Worker behind it (YouGame
# rooms/scripts/replay-stub-site.mjs + `wrangler dev`). The queue-e2e run's own checks (the same
# verdicts on both sides, matched checksums) must pass.
# 1. Live: once the set's room is up, Watcher follows the host's set stream from its first frame, 12
#    frames behind, and LateWatcher joins --late-watch seconds later (150: in game 2), catching up
#    from the set's first frame. --relay: both watch through the rooms Worker's /watch/<room> with a
#    watch ticket from --site, and the players stream to their room (ORCA_TEST_REPLAY_ROOM); without
#    it they read the host's file. Each must match every checksum to the set's end. Over the relay,
#    Watcher's connection drops once, --watch-drop seconds (40; 0: never) after it connected, and must
#    come back on a fresh watch ticket.
# --scenario goes to queue-e2e (rejoin: the joiner's room connection drops in game 1 and comes back;
# the set's recordings must still be whole).
# 2. Saved: the host's and the joiner's set files (the history they shared: same origin, same first
#    frame) replay flat out in fresh Orcas, every checksum matching, and hold the same frames and
#    checksums as each other.
# 3. Kept (--relay, stub site only): the compact copy the room handed the site at the set's end holds
#    the set, agrees with the set file and replays checksum-clean.
import glob
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.request

import replay_stream

if len(sys.argv) < 3 or "--site" not in sys.argv:
    sys.exit("usage: replay-set-check.py <nogui> <disc> --site <url> [--pplus <dol>] [--queue Q] "
             "[--games N] [--late-watch S] [--relay]")
BIN, DISC = os.path.abspath(sys.argv[1]), sys.argv[2]


def opt(name, fallback=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else fallback


SITE = opt("--site").rstrip("/")
PPLUS = opt("--pplus")
QUEUE = opt("--queue", "ranked")
GAMES = opt("--games")
SCENARIO = opt("--scenario")
# --recorder <ada|bo>: only that player streams to the room (queue-e2e --replay-room). With
# --scenario rejoin, bo (the joiner) is the one whose room connection drops and comes back, so
# --recorder bo checks that the live stream follows its recorder through the drop.
RECORDER = opt("--recorder")
LATE = int(opt("--late-watch", "150"))
RELAY = "--relay" in sys.argv
WATCH_DROP = opt("--watch-drop", "40")
HERE = os.path.dirname(os.path.abspath(__file__))
T0 = time.time()
results = []


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def check(name, ok, detail):
    results.append((name, bool(ok), detail))
    log(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")


def fields(done_line):
    out = {"ok": " done ok " in done_line}
    for k, v in re.findall(r"(\w+)=(\S*)", done_line):
        out[k] = v
    if "why=" in done_line:
        out["why"] = done_line.split("why=", 1)[1]
    return out


def play(name, source, extra=(), timeout=900):
    """replay-play.py on `source` (a file or a ws:// URL); returns (done fields, output lines)."""
    cmd = [sys.executable, os.path.join(HERE, "replay-play.py"), BIN, DISC, source, *extra]
    if PPLUS:
        cmd += ["--pplus", PPLUS]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []
    done = None
    start = time.time()
    for line in proc.stdout:
        line = line.rstrip("\n")
        lines.append(line)
        if "orca replay start" in line or "orca replay done" in line or "orca error" in line:
            log(f"{name}: {line.split('] ', 1)[-1][:300]}")
        if "orca replay done" in line:
            done = line
        if time.time() - start > timeout:
            proc.kill()
            break
    proc.wait()
    return (fields(done) if done else {"ok": False, "why": "no done line"}), lines


def watch_url(dev, room):
    # YG_SITE_COOKIE: a Cookie header value for --site (a staging site's team access cookie, handed in
    # by a wrapper). Never logged; queue-e2e.mjs sends it too.
    headers = {"content-type": "application/json", "user-agent": "Mozilla/5.0 (AGENT TEST replay-set-check)"}
    if os.environ.get("YG_SITE_COOKIE"):
        headers["cookie"] = os.environ["YG_SITE_COOKIE"]
    req = urllib.request.Request(f"{SITE}/api/multiplayer/ticket", method="POST",
                                 data=json.dumps({"dev": dev, "watch": room}).encode(), headers=headers)
    reply = json.loads(urllib.request.urlopen(req, timeout=10).read())
    base = reply["url"].replace("https://", "wss://").replace("http://", "ws://").rstrip("/")
    return f"{base}{reply['watch']}?ticket={reply['ticket']}"


# 1. The set, through queue-e2e.
cmd = ["node", os.path.join(HERE, "queue-e2e.mjs"), BIN, DISC, "--q2", "--queue", QUEUE, "--site", SITE,
       "--keep", "--replay", "--alarm", "1500"]
if PPLUS:
    cmd += ["--pplus", PPLUS]
if GAMES:
    cmd += ["--games", GAMES]
if SCENARIO:
    cmd += ["--scenario", SCENARIO]
if RECORDER:
    cmd += ["--replay-room", RECORDER]
env = dict(os.environ)
if RELAY:
    env["ORCA_TEST_REPLAY_ROOM"] = "1"
log(f"queue {QUEUE}, site {SITE}, relay {'yes' if RELAY else 'no (files)'}, late watch {LATE} s")
qe = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
state = {"work": None, "dev": None, "host": None, "joiner": None, "room": None, "lines": []}
matched = threading.Event()


def read_qe():
    for line in qe.stdout:
        line = line.rstrip("\n")
        state["lines"].append(line)
        m = re.search(r"work (\S+); dev game (\S+);", line)
        if m:
            state["work"], state["dev"] = m.group(1), m.group(2)
        m = re.search(r"host (\w+), joiner (\w+), room (\w+)", line)
        if m and not matched.is_set():
            state["host"], state["joiner"], state["room"] = m.groups()
            matched.set()
        if re.search(r"PASS|FAIL|RESULT|set end|matched into|host \w+, joiner", line):
            print(f"         queue-e2e: {line}", flush=True)


reader = threading.Thread(target=read_qe, daemon=True)
reader.start()
if not matched.wait(600):
    qe.terminate()
    sys.exit("FAIL: queue-e2e never matched the two players")
host, joiner, room, work = state["host"], state["joiner"], state["room"], state["work"]
log(f"matched: host {host}, joiner {joiner}, room {room}, work {work}")
before = set(glob.glob(os.path.join(work, f"rec-{host}-*.ygr")))
set_file = None
deadline = time.time() + 180
while time.time() < deadline and not set_file:
    new = sorted(set(glob.glob(os.path.join(work, f"rec-{host}-*.ygr"))) - before)
    if new:
        set_file = new[-1]
    time.sleep(0.5)
if not set_file:
    qe.terminate()
    qe.wait(30)
    sys.exit(f"FAIL: the host started no new history after the match ({sorted(before)})")
log(f"the set's stream: {set_file}")
t_set = time.time()

watchers = {}


def watcher(name):
    extra = ["--follow"]
    if RELAY and name == "Watcher" and WATCH_DROP != "0":
        # Its own watch tickets (watch:<room>) for the set's dev game, so its connection can drop
        # --watch-drop seconds in and come back on a fresh one.
        source = f"watch:{room}"
        extra += ["--env", f"ORCA_SITE={SITE}", "--env", f"ORCA_TEST_DEV_GAME={state['dev']}",
                  "--env", f"ORCA_TEST_REPLAY_WATCH_DROP={WATCH_DROP}"]
    else:
        source = watch_url(state["dev"], room) if RELAY else set_file
    watchers[name] = play(name, source, extra)


if RELAY:
    time.sleep(2)  # the room's stream starts once the session does
threads = [threading.Thread(target=watcher, args=("Watcher",), daemon=True)]
threads[0].start()
late_at = t_set + LATE
while time.time() < late_at and qe.poll() is None:
    time.sleep(1)
if qe.poll() is None:
    log(f"LateWatcher joins {time.time() - t_set:.0f} s into the set")
    threads.append(threading.Thread(target=watcher, args=("LateWatcher",), daemon=True))
    threads[1].start()
else:
    log("the set ended before the late watcher's time: no late watcher")
qe.wait()
reader.join(5)
qe_ok = qe.returncode == 0
check("the set played to its end (queue-e2e)", qe_ok,
      next((l.split("] ", 1)[-1][:400] for l in reversed(state["lines"]) if "PASS" in l or "FAIL" in l),
           f"exit {qe.returncode}"))
for t in threads:
    t.join(600)
set_stream = replay_stream.read(set_file)
log(f"host's set file: {replay_stream.summary(set_stream)}")

for name in ("Watcher", "LateWatcher"):
    if name not in watchers:
        continue
    w, _ = watchers[name]
    detail = (f"{w.get('frames')} frames, {w.get('checked')} checksums checked, {w.get('mismatched')} differ; "
              f"{w.get('live_avg_ms')} ms behind the players on average ({w.get('live_max_ms')} at most)")
    if name == "LateWatcher" and w.get("caught_up_ms"):
        detail += f"; caught up in {float(w['caught_up_ms']) / 1000:.1f} s"
    if not w["ok"]:
        detail += f"; why={w.get('why')}"
    covers = int(w.get("frames") or 0) >= len(set_stream.frames) - 120
    if name == "Watcher" and RELAY and WATCH_DROP != "0":
        back = [l for l in watchers[name][1] if "orca replay reconnected" in l]
        check("Watcher's connection dropped mid-set and came back", w["ok"] and back,
              f"{len(back)} reconnect(s), dropped {WATCH_DROP} s after it connected")
    if RELAY and SCENARIO == "rejoin" and RECORDER:
        resumed = [l for l in watchers[name][1] if "orca replay resumed" in l]
        check(f"{name} followed its recorder through the room drop", w["ok"] and resumed,
              f"{len(resumed)} resume(s): {resumed[0].split('] ', 1)[-1] if resumed else '-'}")
    check(f"{name} followed the whole set and matches", w["ok"] and int(w.get("checked") or 0) > 0
          and w.get("mismatched") == "0" and covers, detail + f" (the set: {len(set_stream.frames)} frames)")

# 2. Saved: both players' files of the set.
joiner_file = None
# Every history starts at the same canonical origin and frame; the set's is the one whose first frame
# (its seed and menu exit) is the host's.
for path in sorted(glob.glob(os.path.join(work, f"rec-{joiner}-*.ygr"))):
    s = replay_stream.read(path)
    if (s.info["origin_hash"] == set_stream.info["origin_hash"] and s.first == set_stream.first and
            s.frames[:1] == set_stream.frames[:1] and (not joiner_file or len(s.frames) > len(joiner_stream.frames))):
        joiner_file, joiner_stream = path, s
if not joiner_file:
    check("the joiner recorded the set", False, "no file of the joiner's starts where the host's set does")
else:
    log(f"joiner's set file: {replay_stream.summary(joiner_stream)}")
    for who, path in ((host, set_file), (joiner, joiner_file)):
        r, _ = play(f"Replay-{who}", path)
        check(f"replay of {who}'s set file", r["ok"] and int(r.get("checked") or 0) > 0 and r.get("mismatched") == "0",
              f"{r.get('frames')} frames, {r.get('checked')} checksums checked, {r.get('mismatched')} differ, "
              f"{int(r.get('wall_ms') or 0) / 1000:.1f} s wall" + ("" if r["ok"] else f"; why={r.get('why')}"))
    end = min(set_stream.end, joiner_stream.end)
    diff = [i for i in range(end - set_stream.first) if set_stream.frames[i] != joiner_stream.frames[i]]
    common = sorted(set(set_stream.checksums) & set(joiner_stream.checksums))
    cdiff = [c for c in common if set_stream.checksums[c] != joiner_stream.checksums[c]]
    check("both players recorded the same set", not diff and not cdiff and common,
          f"frames {set_stream.first}-{end}: {len(diff)} differ; {len(common)} checksums in both, {len(cdiff)} differ")

# 3. Kept: over the relay, the room hands the site a compact copy of the set (Orca's `rp` pieces at
# each result, the latest when its recorder leaves). The local stub site keeps it (GET
# /test/orca-replays); the real site has no such route, so this is skipped there. It must hold the
# set's frames from its start to near its end, agree with the set file, and replay checksum-clean.
if RELAY:
    def site_get(path):
        req = urllib.request.Request(f"{SITE}{path}", headers={"user-agent": "Mozilla/5.0 (AGENT TEST replay-set-check)"})
        return urllib.request.urlopen(req, timeout=10).read()
    try:
        site_get("/test/orca-replays")
        stub = True
    except Exception:
        stub = False
        log("the site keeps no test copies (not the stub site): no kept-copy check")
    if stub:
        copies, deadline = [], time.time() + 90
        while time.time() < deadline:
            copies = [c for c in json.loads(site_get("/test/orca-replays")) if c["code"] == state["room"]]
            if copies:
                break
            time.sleep(2)
        check("the room handed the site one kept copy of the set", len(copies) == 1,
              f"{len(copies)} copy(ies) for room {state['room']}" +
              (f": {copies[-1]['frames']} frames, {copies[-1]['bytes']} bytes, players "
               f"{[p['n'] for p in copies[-1]['players']]}" if copies else ""))
        if copies:
            kept_path = os.path.join(work, f"kept-{state['room']}.ygr")
            with open(kept_path, "wb") as f:
                f.write(site_get(f"/test/orca-replays/{copies[-1]['i']}"))
            kept = replay_stream.read(kept_path)
            log(f"kept copy: {replay_stream.summary(kept)}")
            n = len(kept.frames)
            same = kept.first == set_stream.first and kept.frames == set_stream.frames[:n]
            common = sorted(set(kept.checksums) & set(set_stream.checksums))
            cdiff = [c for c in common if kept.checksums[c] != set_stream.checksums[c]]
            live_bytes = os.path.getsize(set_file)
            check("the kept copy is the set, compact", same and not cdiff and common and n == copies[-1]["frames"]
                  and n >= len(set_stream.frames) - 3600,
                  f"{n} of the set file's {len(set_stream.frames)} frames, {'the same' if same else 'DIFFERENT'}; "
                  f"{len(common)} checksums in both, {len(cdiff)} differ; {os.path.getsize(kept_path)} bytes vs "
                  f"{live_bytes} live ({live_bytes / max(1, os.path.getsize(kept_path)):.0f}x smaller)")
            r, _ = play("Replay-kept", kept_path)
            check("replay of the kept copy", r["ok"] and int(r.get("checked") or 0) > 0 and r.get("mismatched") == "0",
                  f"{r.get('frames')} frames, {r.get('checked')} checksums checked, {r.get('mismatched')} differ, "
                  f"{int(r.get('wall_ms') or 0) / 1000:.1f} s wall" + ("" if r["ok"] else f"; why={r.get('why')}"))

failed = [r for r in results if not r[1]]
log(f"{len(results) - len(failed)} of {len(results)} checks passed; work {work}")
print("PASS" if not failed else "FAIL: " + ", ".join(r[0] for r in failed), flush=True)
sys.exit(1 if failed else 0)
