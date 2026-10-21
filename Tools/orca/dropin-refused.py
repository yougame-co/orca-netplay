#!/usr/bin/env python3
# dropin-refused.py: a friend joins a host whose keyframes YouGame's store refuses for good, over a
# real YouGame room, headless and muted.
#
#   Tools/orca/dropin-refused.py <dolphin-emu-nogui> <disc>
#
# Both Orcas use dev tickets and the real keyframe store, which refuses them for good (403
# dev_ticket). The host tells the waiting friend instead of packing and uploading another keyframe:
# the host prints "orca state friend-left refused", the friend "orca error network", and the host's
# log has exactly one refused keyframe. (A 403 signed_out goes the same way, with that code.)
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

if len(sys.argv) < 3:
    sys.exit("usage: dropin-refused.py <nogui> <disc>")
BIN, DISC = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "bf-dropin.txt")
ROOM = "ref" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-refused-")
T0 = time.time()


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra):
        self.name = name
        self.lines = queue.Queue()
        user = os.path.join(WORK, f"user-{name}")
        os.makedirs(user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_DEV_GAME": "orca-dropin-test",
            "ORCA_NAME": name,
            "YG_INPUT": INPUT,
            "YG_SCENES": "1",
        }
        env.update(env_extra)
        args = [BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Options.Verbosity=2", "-e", DISC]
        self.err = open(os.path.join(WORK, f"{name}.log"), "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                continue
            log(f"{self.name}> {line}")
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line):
        log(f"{self.name}< {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def expect(self, want, timeout):
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
        fail(f"{self.name}: no '{want}' in {timeout} s")

    def log_lines(self, text):
        with open(os.path.join(WORK, f"{self.name}.log"), errors="replace") as f:
            return [line.rstrip() for line in f if text in line]

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


log(f"room {ROOM}, work {WORK}")
host = Orca("Host", {"ORCA_ROOM": ROOM})
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
host.expect("orca state playing", 90)
time.sleep(20)

friend = Orca("Friend", {})
orcas.append(friend)
friend.send(f"join {ROOM}")
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state joining", 60)
host.expect("orca state friend-left refused", 90)
friend.expect("orca error network", 30)
# No keyframe after the refusal: wait a few more of the old 3.5 s retries.
time.sleep(12)
refused = host.log_lines("refused (dev_ticket)")
captured = host.log_lines("Drop-in: keyframe of frame")
log(f"host: {len(refused)} refused, {len(captured)} keyframe lines")
for line in captured:
    log(f"  {line[line.find('Drop-in'):]}")
for o in orcas:
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped")
for o in orcas:
    o.stop()
if len(refused) != 1:
    fail(f"expected one refused keyframe, saw {len(refused)}")
log("PASS")
shutil.rmtree(WORK, ignore_errors=True)
