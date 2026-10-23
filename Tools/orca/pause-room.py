#!/usr/bin/env python3
# pause-room.py: a host's room drops while its player has the game paused, over a real YouGame
# room, headless and muted.
#
#   Tools/orca/pause-room.py <dolphin-emu-nogui> <disc>
#
# The host pauses ("pause"), then its room's connection drops ("test-drop-room"). Still paused, it
# says so ("orca error network") and opens its room again ("Drop-in: hosting room ... again" in its
# log), without ever running ("state running") until "resume".
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
    sys.exit("usage: pause-room.py <nogui> <disc>")
BIN, DISC = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "bf-dropin.txt")
ROOM = "pau" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-pause-")
T0 = time.time()


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra):
        self.name = name
        self.lines = queue.Queue()
        self.all = []
        user = os.path.join(WORK, f"user-{name}")
        os.makedirs(user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_ROOM_DROP": "1",
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
            self.all.append(line)
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
time.sleep(10)
host.send("pause")
mark = len(host.all)
line = host.expect("state ", 10)
if "paused" not in line:
    fail(f"not paused: {line}")
time.sleep(3)
host.send("test-drop-room")
host.expect("orca state room-away self", 20)
host.expect("orca state room-back self", 90)
end = time.time() + 40
while time.time() < end and not host.log_lines(f"in room {ROOM} on seat"):
    time.sleep(1)
again = host.log_lines(f"in room {ROOM} on seat")
if not again:
    fail("the room never recovered while paused")
log(f"host: {again[-1][again[-1].find('Orca room'):]}")
if any("state running" in l for l in host.all[mark:]):
    fail("the game ran while paused")
host.send("resume")
host.expect("state ", 10)
host.stop()
log("PASS")
shutil.rmtree(WORK, ignore_errors=True)
