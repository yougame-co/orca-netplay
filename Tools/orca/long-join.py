#!/usr/bin/env python3
# long-join.py: how long a friend's drop-in takes against how long the host has been playing
# (ORCA.md "Fresh starts"), over a local rooms stack, headless and muted. Run it through
# run-lock.py.
#
#   Tools/orca/long-join.py <dolphin-emu-nogui> <Brawl disc> --pplus-dol <launcher.dol>
#       --site http://127.0.0.1:<port> --out <dir> --plan bf:fight:2,bf:menu:10,pp:fight:25,...
#       [--pp-offset 180] [--host-env K=V,K=V] [--max-plug 10]
#
# Every plan entry (game:mode:minutes) is its own host Orca in its own friends room (dev game),
# throttled like any online session. "fight": Group > Brawl against one CPU forever (port 1
# fuzzed); "menu": to character select, then idle. Brawl hosts start at t=0, Project+ hosts at
# --pp-offset seconds. When a host reaches its age, a new friend Orca that already sits on its own
# character select sends "join <host's room>" (an in-play join, as a matched queue joiner's). Joins
# run one at a time, so no two rebuilds share the machine. Each join's replay frame, rebuild and
# plug-in times, or its error, go to <out>/results.jsonl.
#
# --host-env ORCA_TEST_FAST_UNTIL=<frame> ages hosts quickly: their solo frames run unthrottled up
# to that frame (54000 is 15 minutes of play), so a short plan entry stands for a long host.
# Since 0.3.34 a friend at a host with more than 10 s of play since its last start fresh-starts the
# host first, so every join should plug in within a few seconds whatever the host's age.
# --fast-age: each host gets its own ORCA_TEST_FAST_UNTIL, its origin's frame plus its plan minutes of
# frames, and its friend joins once the host has played that far (a plan entry's minutes are then
# frames of play, not wall time); the hosts reach their ages in a few minutes, side by side.
# After each plug-in the two play --together seconds (10); then the friend leaves, and both Orcas'
# "back to solo play" lines must count matched checksums, with no desync in either log.
# Fails (exit 1) when a join doesn't plug in within --max-plug seconds (0: never fails), or a session's
# checksums didn't match.
import argparse
import json
import os
import queue
import random
import re
import string
import subprocess
import sys
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("bin")
ap.add_argument("disc")
ap.add_argument("--pplus-dol", required=True)
ap.add_argument("--site", default="http://127.0.0.1:8796")
ap.add_argument("--out", required=True)
ap.add_argument("--plan", required=True)
ap.add_argument("--pp-offset", type=float, default=180)
ap.add_argument("--join-timeout", type=float, default=420)
ap.add_argument("--host-env", default="")
ap.add_argument("--max-plug", type=float, default=10)
ap.add_argument("--fast-age", action="store_true")
ap.add_argument("--together", type=float, default=10)
A = ap.parse_args()
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "inputs")
os.makedirs(A.out, exist_ok=True)
T0 = time.time()
LOG = open(os.path.join(A.out, "run.log"), "a", buffering=1)
RESULTS = os.path.join(A.out, "results.jsonl")


def log(text):
    line = f"[{time.time() - T0:7.1f}] {text}"
    print(line, flush=True)
    LOG.write(line + "\n")


# Each game's origin (ORCA.md "Fresh starts"): Brawl Rev 2's built main menu, Project+'s.
ORIGIN = {"bf": 1524, "pp": 230}
INPUTS = {
    ("bf", "fight"): "bf-cpu-loop.txt", ("bf", "menu"): "bf-menu-idle.txt",
    ("pp", "fight"): "pplus-cpu-loop.txt", ("pp", "menu"): "pplus-menu-idle.txt",
}


class Orca:
    def __init__(self, name, game, inp, extra_env):
        self.name = name
        self.lines = queue.Queue()
        self.last_stats = None
        self.started = time.time()
        user = os.path.join(A.out, f"user-{name}")
        os.makedirs(user, exist_ok=True)
        self.logpath = os.path.join(A.out, f"{name}.log")
        env = {
            "HOME": os.environ["HOME"], "PATH": "/usr/bin:/bin",
            "SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0",
            "SDL_HIDAPI_LIBUSB": "0", "ORCA_SESSION": "1", "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_DEV_GAME": "orca-dropin-test",
            "ORCA_SITE": A.site, "ORCA_NAME": name, "YG_INPUT": os.path.join(HERE, inp),
            "YG_SCENES": "1",
        }
        if game == "pp":
            env["ORCA_PROFILE"] = "PPLUS32"
        env.update(extra_env)
        args = [A.bin, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Logs.CORE=True", "-C", "Logger.Options.Verbosity=2"]
        if game == "pp":
            args += ["-C", f"Dolphin.Core.DefaultISO={A.disc}", "-e", A.pplus_dol]
        else:
            args += ["-e", A.disc]
        self.err = open(self.logpath, "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            now = time.time()
            if line.startswith("orca stats "):
                try:
                    self.last_stats = (now, json.loads(line[len("orca stats "):]))
                except ValueError:
                    pass
                continue
            if line.startswith(("orca state ", "orca error ")):
                log(f"{self.name}> {line}")
            self.lines.put((now, line))
        self.lines.put((time.time(), None))

    def send(self, line):
        log(f"{self.name}< {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def drain(self):
        while True:
            try:
                self.lines.get_nowait()
            except queue.Empty:
                return

    def expect(self, want, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                _, line = self.lines.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                break
            if line is None:
                raise RuntimeError(f"{self.name} exited waiting for '{want}'")
            if line.startswith(want):
                return line
        raise RuntimeError(f"{self.name}: no '{want}' in {timeout} s")

    def grep(self, *keys):
        try:
            with open(self.logpath, errors="replace") as f:
                return [l.rstrip() for l in f if any(k in l for k in keys)]
        except OSError:
            return []

    def wait_log(self, text, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                raise RuntimeError(f"{self.name} exited waiting for '{text}' in its log")
            if self.grep(text):
                return
            time.sleep(1)
        raise RuntimeError(f"{self.name}: no '{text}' in its log in {timeout} s")

    def frame(self):
        return self.last_stats[1].get("f") if self.last_stats else None

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.send("quit")
                self.proc.wait(timeout=20)
            except Exception:
                self.proc.kill()
                self.proc.wait()
        self.err.close()


failures = []
plan = []
for i, item in enumerate(A.plan.split(",")):
    game, mode, minutes = item.split(":")
    start = 0 if game == "bf" else A.pp_offset
    if A.fast_age:
        start = 0
    plan.append({"i": i, "game": game, "mode": mode, "minutes": float(minutes),
                 "host_start": start, "target": start + float(minutes) * 60,
                 "until": ORIGIN[game] + int(float(minutes) * 3600)})
orcas = []
try:
    log(f"plan {A.plan}, pp offset {A.pp_offset} s, out {A.out}")
    host_error = []

    def start_hosts():
        # Hosts, in start order, beside the joins below.
        try:
            for h in sorted(plan, key=lambda h: h["host_start"]):
                while time.time() - T0 < h["host_start"]:
                    time.sleep(1)
                h["room"] = "lj" + "".join(random.choice(string.ascii_lowercase + string.digits)
                                           for _ in range(7))
                name = f"H{h['i']}-{h['game']}-{h['mode']}{int(h['minutes'])}"
                host_env = dict(kv.split("=", 1) for kv in A.host_env.split(",") if kv)
                if A.fast_age:
                    host_env["ORCA_TEST_FAST_UNTIL"] = str(h["until"])
                orca = Orca(name, h["game"], INPUTS[(h["game"], h["mode"])],
                            dict({"ORCA_ROOM": h["room"]}, **host_env))
                orcas.append(orca)
                orca.expect("orca caps", 60)
                orca.send("caps join leave stats")
                orca.expect("orca state playing", 120)
                h["orca"] = orca
                h["t_playing"] = time.time()
                log(f"{name}: playing in room {h['room']} (load {os.getloadavg()[0]:.1f})")
        except Exception as e:  # noqa: BLE001
            host_error.append(str(e))
            log(f"host start failed: {e}")

    threading.Thread(target=start_hosts, daemon=True).start()
    # Joins, one at a time, in target order.
    for h in sorted(plan, key=lambda h: h["target"]):
        while "t_playing" not in h:
            if host_error:
                raise RuntimeError(host_error[0])
            time.sleep(0.5)
        host = h["orca"]
        name = f"F{h['i']}-{h['game']}"
        # The friend boots ~45 s ahead so it sits on its character select at the host's age.
        while not A.fast_age and time.time() - T0 < h["target"] - 45:
            time.sleep(1)
        friend = Orca(name, h["game"], INPUTS[(h["game"], "menu")], {})
        orcas.append(friend)
        friend.expect("orca caps", 60)
        friend.send("caps join leave stats")
        friend.expect("orca state playing", 120)
        friend.wait_log("-> scSelctCharacter", 180)
        if A.fast_age:
            # Played its age: past its ORCA_TEST_FAST_UNTIL frame, at its own pace again.
            while (host.frame() or 0) < h["until"] + 60:
                if host.proc.poll() is not None:
                    raise RuntimeError(f"host {h['i']} exited before its age")
                time.sleep(0.5)
        while time.time() - T0 < h["target"] and not A.fast_age:
            time.sleep(0.5)
        time.sleep(3)
        if host.proc.poll() is not None:
            raise RuntimeError(f"host {h['i']} exited before its join")
        rec = {"game": h["game"], "mode": h["mode"], "plan_min": h["minutes"],
               "host_age_s": round(time.time() - h["t_playing"], 1),
               "host_frame_at_join": host.frame(), "load_at_join": os.getloadavg()[0]}
        friend.drain()
        t_join = time.time()
        friend.send(f"join {h['room']}")
        marks = {}
        outcome = None
        end = t_join + A.join_timeout
        while time.time() < end and outcome is None:
            try:
                t, line = friend.lines.get(timeout=1)
            except queue.Empty:
                continue
            if line is None:
                outcome = "friend exited"
                break
            if line.startswith("orca state joining "):
                pct = int(line.split()[-1])
                for mark in (0, 50, 90, 99):
                    if pct >= mark and mark not in marks:
                        marks[mark] = round(t - t_join, 1)
                        if mark == 50:
                            rec["load_at_rebuild_start"] = os.getloadavg()[0]
            elif line.startswith("orca state playing"):
                outcome = "playing"
                rec["plugged_s"] = round(t - t_join, 1)
            elif line.startswith("orca error"):
                outcome = line
                rec["error_s"] = round(t - t_join, 1)
        rec["outcome"] = outcome or f"timeout {A.join_timeout} s"
        rec["joining_marks_s"] = marks
        rec["load_after"] = os.getloadavg()[0]
        time.sleep(2)
        rec["friend_log"] = [l[l.find("N[") if "N[" in l else 0:] for l in friend.grep(
            "Drop-in:", "rebuilt replay", "plugged into", "Couldn't", "JIT cleared")]
        rec["host_log"] = [l[l.find("N[") if "N[" in l else 0:] for l in host.grep(
            "Drop-in: keyframe of frame", "Drop-in: replay", "refus", "friend")][-6:]
        log(f"JOIN {h['game']} {h['mode']} ~{h['minutes']:g} min: {json.dumps(rec)}")
        with open(RESULTS, "a") as f:
            f.write(json.dumps(rec) + "\n")
        if outcome != "playing" or (A.max_plug and rec["plugged_s"] > A.max_plug):
            late = f", plugged in after {rec['plugged_s']} s" if "plugged_s" in rec else ""
            failures.append(
                f"{h['game']} {h['mode']} ~{h['minutes']:g} min: {rec['outcome']}{late}")
        if outcome == "playing":
            time.sleep(A.together)
            rec2 = host.last_stats[1] if host.last_stats else None
            log(f"{A.together:g} s after plug-in: host stats {rec2} / friend stats "
                f"{friend.last_stats[1] if friend.last_stats else None}")
            # The friend leaves; each side's session must have matched checksums, and no desync.
            friend.send("leave")
            solo = re.compile(r"back to solo play at frame (\d+) \(([^)]*)\): (\d+) rollbacks, "
                              r"(\d+) checksums matched")
            counts = {}
            end = time.time() + 20
            while time.time() < end and len(counts) < 2:
                for who, o in (("host", host), ("friend", friend)):
                    lines = o.grep("back to solo play at frame")
                    if lines and who not in counts:
                        m = solo.search(lines[-1])
                        if m:
                            counts[who] = {"why": m.group(2), "rollbacks": int(m.group(3)),
                                           "checksums": int(m.group(4))}
                time.sleep(0.5)
            desyncs = host.grep("Desync at frame", "the session failed") + friend.grep(
                "Desync at frame", "the session failed")
            rec["sessions"] = counts
            rec["desyncs"] = len(desyncs)
            log(f"SESSION {h['game']} ~{h['minutes']:g} min: {json.dumps(counts)}, "
                f"{len(desyncs)} desync lines")
            with open(RESULTS, "a") as f:
                f.write(json.dumps({"game": h["game"], "plan_min": h["minutes"],
                                    "sessions": counts, "desyncs": len(desyncs)}) + "\n")
            if (len(counts) < 2 or any(c["checksums"] <= 0 for c in counts.values())
                    or desyncs):
                failures.append(f"{h['game']} ~{h['minutes']:g} min: session checksums "
                                f"{counts}, {len(desyncs)} desync lines")
        friend.stop()
        host.stop()
except Exception as e:  # noqa: BLE001
    failures.append(f"{type(e).__name__}: {e}")
finally:
    for o in orcas:
        if o.proc.poll() is None:
            o.proc.kill()
if failures:
    log("FAIL: " + "; ".join(failures))
    sys.exit(1)
log("PASS")
