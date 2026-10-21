#!/usr/bin/env python3
# dropin-delay.py: the input delay around a drop-in, over a real YouGame room: two Orcas on this
# machine, steered with the app's stdin commands as in dropin-commands.py, headless and muted.
#
#   Tools/orca/dropin-delay.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#       [--at css|match] [--minutes 5] [--host-env K=V ...] [--friend-env K=V ...]
#       [--expect-max N] [--expect-raise S1-S2 [--raise-on host|friend|both]]
#
# 1. The host boots its game, solo, in a room of its own, and reaches character select.
# 2. --at css: the friend joins 30 s later, at character select. --at match: the friend joins at
#    character select, both start a match, and 10 s in the friend leaves; then it joins again,
#    into the running match.
# 3. From that (last) join, both play for --minutes (the input scripts pick, fight, and go round
#    again). Each second's "orca stats" line with a friend plugged in gives the delay in use.
#
# PASS needs no error, no desync, and the expected delay on both sides after the plug-in:
# - by default, never above --expect-max (default 2, what a clean link must hold);
# - with --expect-raise S1-S2 (a test link that is late from about S1 to S2 s after the join), above
#   2 at some point in that span (or up to 15 s after) and back to 2 for the last minute, on the
#   sides --raise-on names. The other side must hold 2 throughout: only the late side should pay.
# The log's "input delay A -> B" lines say why each change happened. Runs two Orca processes;
# ORCA_TEST_KEEP=1 keeps the logs. Example test links (Profile.h):
#
#   a slow, bursty uplink for a minute and a half (raises the friend's delay, then back to 2):
#     --friend-env ORCA_TEST_NET_UPLINK=90+120/60-210 --host-env ORCA_TEST_NET_DELAY_MS=0 \
#       --minutes 5 --expect-raise 45-210 --raise-on friend
#   dropouts a frame would cure, 130-150 ms every 2 s (raises both):
#     --friend-env ORCA_TEST_NET_SPIKES=2000-2000/130-150/40-150 \
#       --host-env ORCA_TEST_NET_DELAY_MS=0 --minutes 4 --expect-raise 30-150
#   a Mac's bad-day Wi-Fi, 80-220 ms spikes at random every 2.7 s on average (holds 2):
#     --friend-env ORCA_TEST_NET_SPIKES=~2700/80-220 --host-env ORCA_TEST_NET_DELAY_MS=0
import argparse
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

p = argparse.ArgumentParser()
p.add_argument("bin")
p.add_argument("disc")
p.add_argument("--pplus")
p.add_argument("--at", choices=["css", "match"], default="css")
p.add_argument("--minutes", type=float, default=5)
p.add_argument("--host-env", action="append", default=[])
p.add_argument("--friend-env", action="append", default=[])
p.add_argument("--expect-max", type=int, default=2)
p.add_argument("--expect-raise")
p.add_argument("--raise-on", choices=["host", "friend", "both"], default="both")
a = p.parse_args()
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "pplus-dropin.txt" if a.pplus else "bf-dropin.txt")
ROOM = "dly" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-delay-")
T0 = time.time()
# The test link's spikes and slow uplink only shape the room relay, so a direct link would hide them.
# Those runs, and every --expect-raise run, stay on the relay (ORCA_DIRECT=0) unless the caller
# sets ORCA_DIRECT itself.
SHAPED = any(pair.partition("=")[0] in ("ORCA_TEST_NET_SPIKES", "ORCA_TEST_NET_UPLINK")
             for pair in a.host_env + a.friend_env)
RELAY_ONLY = SHAPED or bool(a.expect_raise)


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def env_pairs(pairs):
    out = {}
    for pair in pairs:
        key, _, value = pair.partition("=")
        out[key] = value
    return out


class Orca:
    def __init__(self, name, env_extra):
        self.name = name
        self.lines = queue.Queue()
        self.stats = []  # (time, stats) for every "orca stats" line
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
        if a.pplus:
            env["ORCA_PROFILE"] = "PPLUS32"
        if RELAY_ONLY:
            env["ORCA_DIRECT"] = "0"
        env.update(env_extra)
        args = [a.bin, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Options.Verbosity=2"]
        if a.pplus:
            args += ["-C", f"Dolphin.Core.DefaultISO={a.disc}", "-e", a.pplus]
        else:
            args += ["-e", a.disc]
        self.log_path = os.path.join(WORK, f"{name}.log")
        self.err = open(self.log_path, "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                try:
                    self.stats.append((time.time(), json.loads(line[len("orca stats "):])))
                except ValueError:
                    pass
                with open(os.path.join(WORK, f"{self.name}.stats"), "a") as f:
                    f.write(f"{time.time() - T0:.1f} {line[len('orca stats '):]}\n")
                continue
            if not line.startswith("orca state joining "):
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

    def wait_log(self, text, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                fail(f"{self.name} exited (code {self.proc.returncode}) waiting for '{text}'")
            with open(self.log_path, errors="replace") as f:
                for line in f:
                    if text in line:
                        return line.rstrip()
            time.sleep(1)
        fail(f"{self.name}: no '{text}' in its log in {timeout} s")

    def errors(self):
        out = []
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                return out
            if line is not None and line.startswith("orca error"):
                out.append(line)

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


log(f"room {ROOM}, work {WORK}, at {a.at}, {a.minutes} min"
    + (", relay only (ORCA_DIRECT=0)" if RELAY_ONLY else ""))
host = Orca("Host", {"ORCA_ROOM": ROOM, **env_pairs(a.host_env)})
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
host.expect("orca state playing", 90)
time.sleep(30)

friend = Orca("Friend", env_pairs(a.friend_env))
orcas.append(friend)
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state playing", 90)
friend.send(f"join {ROOM}")
friend.expect("orca state joining", 60)
friend.expect("orca state playing", 180)
if a.at == "match":
    for o in (host, friend):
        log(f"{o.name}: {o.wait_log('-> scMelee', 240)}")
    time.sleep(10)
    friend.send("leave")
    friend.expect("orca state left", 30)
    host.expect("orca state friend-left", 30)
    time.sleep(5)
    friend.send(f"join {ROOM}")
    friend.expect("orca state joining", 60)
    friend.expect("orca state playing", 180)
joined = time.time()
log(f"friend plugged in; playing {a.minutes} min")
end = joined + a.minutes * 60
while time.time() < end:
    time.sleep(5)
    for o in orcas:
        if o.proc.poll() is not None:
            fail(f"{o.name} exited (code {o.proc.returncode}) during play")
        errors = o.errors()
        if errors:
            fail(f"{o.name}: {errors[0]}")
for o in orcas:
    o.stop()

ok = True
summary = {}
for o in orcas:
    text = open(o.log_path, errors="replace").read()
    after = [(t - joined, st) for t, st in o.stats if t >= joined and st.get("peers", 0) > 0]
    delays = [st.get("delay", 0) for _, st in after]
    desyncs = (max((st.get("ds", 0) for _, st in o.stats), default=0)
               + text.count("Desync at frame"))
    notes = [l[l.find("input delay"):] for l in text.splitlines() if "Online match: input delay" in l]
    ends = re.findall(r"Online match [^\n]*?(\d+) stalls \((\d+) counted\), (\d+) waits[^\n]*?"
                      r"input delay (\d+)[^\n]*?(\d+) checksums matched", text)
    spared = re.findall(r"(\d+) counted stalls a frame more would have spared", text)
    s = {
        "seconds": len(after),
        "delay_max": max(delays, default=None),
        "delay_end": delays[-1] if delays else None,
        "stalls": sum(st.get("st", 0) for _, st in after),
        "ping_median": after[-1][1].get("pmed") if after else None,
        "desyncs": desyncs,
        "checksums_matched": max((int(e[4]) for e in ends), default=0),
        "counted_stalls": max((int(e[1]) for e in ends), default=0),
        "spared_by_a_frame": max((int(n) for n in spared), default=0),
        "delay_changes": notes,
    }
    summary[o.name] = s
    log(f"{o.name}: {json.dumps(s)}")
    if desyncs or not after or s["checksums_matched"] == 0:
        ok = False
    elif a.expect_raise and a.raise_on not in ("both", o.name.lower()):
        if s["delay_max"] > 2:
            log(f"{o.name}: delay {s['delay_max']} at most; the other side's link was the late one")
            ok = False
    elif a.expect_raise:
        lo, hi = (float(x) for x in a.expect_raise.split("-"))
        during = [st.get("delay", 0) for t, st in after if lo <= t <= hi + 15]
        last = [st.get("delay", 0) for t, st in after if t >= a.minutes * 60 - 60]
        if not during or max(during) <= 2 or not last or max(last) > 2:
            log(f"{o.name}: delay {max(during, default=None)} at most while late, "
                f"{max(last, default=None)} at most in the last minute")
            ok = False
    elif s["delay_max"] > a.expect_max:
        ok = False
print("DELAY " + json.dumps(summary), flush=True)
log("PASS" if ok else "FAIL")
if ok and os.environ.get("ORCA_TEST_KEEP") != "1":
    shutil.rmtree(WORK, ignore_errors=True)
else:
    # The logs and stats only: user directories and keyframes are tens of MB each.
    for name in [f"user-{o.name}" for o in orcas] + ["kf"]:
        shutil.rmtree(os.path.join(WORK, name), ignore_errors=True)
    log(f"logs kept in {WORK}")
sys.exit(0 if ok else 1)
