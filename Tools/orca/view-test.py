#!/usr/bin/env python3
# view-test.py: screenshots of the embed `view` and `dim` lines (ORCA.md "Embedding"), headless. A
# solo session renders into ORCA_TEST_PRESENT and gets the stdin lines the YouGame page sends when
# its menu opens beside the game, then closes, then shows an invite toast. Each step saves a PNG in
# <out>/user/ScreenShots. Check by eye that the picture keeps its size and place, the frame meter
# and chat card stay inside the window, and the dim covers everything. It also checks that
# malformed lines answer `unsupported`.
#
#   Tools/orca/view-test.py <dolphin-emu-nogui> <Brawl disc> <out dir> [seconds before the shots]
#
# Takes about a minute (the scripted match starts ~40 s in).
import os
import queue
import shutil
import subprocess
import sys
import threading
import time

if len(sys.argv) < 4:
    sys.exit("usage: view-test.py <dolphin-emu-nogui> <disc> <out dir> [seconds]")
BIN, DISC, WORK = sys.argv[1], sys.argv[2], sys.argv[3]
FIGHT_S = float(sys.argv[4]) if len(sys.argv) > 4 else 45
HERE = os.path.dirname(os.path.abspath(__file__))
user = os.path.join(WORK, "user")
shutil.rmtree(user, ignore_errors=True)
os.makedirs(user)
env = {
    "HOME": os.environ["HOME"], "PATH": os.environ["PATH"],
    "ORCA_SESSION": "1", "ORCA_TEST_COMMANDS": "1", "ORCA_TEST_PRESENT": "1280x720",
    "YG_INPUT": os.path.join(HERE, "inputs", "bf-mario-link-results.txt"),
    "YG_THROTTLE": "1",
}
# InternalResolution 0 (Auto) as the YouGame app runs it: the view keeps its integral scale.
args = [BIN, "-p", "headless", "-v", "Metal", "-u", user,
        "-C", "Dolphin.DSP.Backend=No Audio Output",
        "-C", "Graphics.Settings.InternalResolution=0", "-e", DISC]
err = open(os.path.join(WORK, "orca.log"), "w")
proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err,
                        text=True, bufsize=1)
lines = queue.Queue()
T0 = time.time()
problems = []


def read():
    for line in proc.stdout:
        line = line.rstrip("\n")
        print(f"[{time.time() - T0:6.1f}] orca> {line}", flush=True)
        lines.put(line)


threading.Thread(target=read, daemon=True).start()


def send(line):
    print(f"[{time.time() - T0:6.1f}] < {line}", flush=True)
    proc.stdin.write(line + "\n")
    proc.stdin.flush()


def expect(prefix, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            line = lines.get(timeout=max(0.05, end - time.time()))
        except queue.Empty:
            break
        if line.startswith(prefix):
            return line
    return None


def shot(name, wait=0.8):
    time.sleep(wait)
    send(f"test-shot {name}")
    time.sleep(0.5)


# The player box at (100, 80), 1280x720; the menu's panel over its left 440 pixels.
BOX = "100 80 1280 720"
CUT = "540 80 840 720"
try:
    if not expect("orca caps", 60):
        sys.exit("no `orca caps` from Orca")
    send("caps join leave stats pause delay perf direct host chat yougame")
    send("perf fps")
    time.sleep(FIGHT_S)
    # A run's first screenshot starts the dump thread and comes out under the next one's name.
    shot("0-warm", 0)
    time.sleep(1.5)
    send(f"rect {BOX}")
    send("chat ada hello%20from%20the%20lobby")
    shot("a-whole", 1.0)
    send(f"view {BOX}")
    send(f"rect {CUT}")
    shot("b-cut")
    send("dim 50")
    shot("c-dim")
    for bad in ("view 1 2 3", "view 0 0 10 10", "view off now", "dim 101", "dim -1", "dim 5x",
                "dim"):
        send(bad)
        if not expect("unsupported", 2):
            problems.append(f"no `unsupported` for `{bad}`")
    send(f"rect {BOX}")
    send("dim 0")
    shot("d-closed")
    send(f"rect {CUT}")
    send("view off")
    shot("e-off-cut")
    send(f"view {BOX}")
    send("rect 100 200 1280 600")
    shot("f-toast-cut")
    send(f"rect {CUT}")
    send("dim 50")
    shot("g-dim-again")
    send("focus")
    shot("h-focus")
    send("quit")
    proc.wait(timeout=30)
finally:
    if proc.poll() is None:
        proc.kill()
    err.close()

shots = sorted(os.listdir(os.path.join(user, "ScreenShots")))
print("shots:", ", ".join(shots))
want = {"a-whole.png", "b-cut.png", "c-dim.png", "d-closed.png", "e-off-cut.png",
        "f-toast-cut.png", "g-dim-again.png", "h-focus.png"}
problems += [f"no {name}" for name in sorted(want - set(shots))]
print("PROBLEMS:\n  " + "\n  ".join(problems) if problems else "OK (now look at the shots)")
sys.exit(1 if problems else 0)
