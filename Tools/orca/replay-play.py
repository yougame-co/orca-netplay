#!/usr/bin/env python3
# replay-play.py: plays one replay stream (a .ygr file, or a ws:// watch URL) in a headless Orca and
# prints its verdict (Rollback/ReplayStream.h). For checks by hand and for demos.
#
#   Tools/orca/replay-play.py <dolphin-emu-nogui> <disc> <file.ygr|ws://...> [--pplus <dol>]
#       [--realtime] [--follow] [--video Metal] [--shots MS] [--env K=V ...]
#       [--product [--names "<port> <url-encoded name> ..."]]
#
# --realtime plays at real speed (default: flat out); --follow keeps reading a growing stream (a live
# watcher); --video Metal renders off screen and --shots MS saves a screenshot every MS ms into the
# work folder (kept). The test knobs that are part of the compatibility key (ORCA_TEST_NET_*) must be
# set as the recording machines had them: pass them with --env.
# --product plays it the way the YouGame app does (ORCA_REPLAY_SOURCE: a file at real speed, a watch
# URL live) instead of through the test knobs; --names gives view-time names (ORCA_REPLAY_NAMES).
import os
import subprocess
import sys
import tempfile
import threading
import time

if len(sys.argv) < 4:
    sys.exit(__doc__ if __doc__ else "usage: replay-play.py <nogui> <disc> <file|ws-url> [options]")
BIN, DISC, SOURCE = sys.argv[1], sys.argv[2], sys.argv[3]
args = sys.argv[4:]
PPLUS = args[args.index("--pplus") + 1] if "--pplus" in args else None
VIDEO = args[args.index("--video") + 1] if "--video" in args else "Null"
SHOTS = int(args[args.index("--shots") + 1]) if "--shots" in args else 0
extra = {}
for i, a in enumerate(args):
    if a == "--env":
        k, _, v = args[i + 1].partition("=")
        extra[k] = v
WORK = tempfile.mkdtemp(prefix="orca-play-")
env = {
    "HOME": os.environ["HOME"],
    "PATH": os.environ["PATH"],
    "ORCA_SESSION": "1",
    "ORCA_TEST_COMMANDS": "1",
    "ORCA_TEST_DEV_GAME": "orca-replay-test",
    "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
    "ORCA_NAME": "Player",
}
if "--product" in args:
    env["ORCA_REPLAY_SOURCE"] = SOURCE
    if "--names" in args:
        env["ORCA_REPLAY_NAMES"] = args[args.index("--names") + 1]
else:
    env["ORCA_TEST_REPLAY_IN"] = SOURCE
if "--realtime" in args:
    env["ORCA_TEST_REPLAY_REALTIME"] = "1"
if "--follow" in args:
    env["ORCA_TEST_REPLAY_FOLLOW"] = "1"
if PPLUS:
    env["ORCA_PROFILE"] = "PPLUS32"
for key in ("ORCA_SITE", "SDL_HIDAPI", "SDL_JOYSTICK_HIDAPI"):
    if os.environ.get(key):
        env[key] = os.environ[key]
env.update(extra)
cmd = [BIN, "-p", "headless", "-u", os.path.join(WORK, "user"), "-v", VIDEO,
       "-C", "Dolphin.DSP.Backend=No Audio Output", "-C", "Logger.Logs.ROLLBACK=True",
       "-C", "Logger.Options.Verbosity=2"]
cmd += ["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS] if PPLUS else ["-e", DISC]
err = open(os.path.join(WORK, "orca.log"), "w")
proc = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err,
                        text=True, bufsize=1)
T0 = time.time()
started = threading.Event()


def shoot():
    started.wait()
    n = 0
    while proc.poll() is None:
        n += 1
        try:
            proc.stdin.write(f"test-shot r-{n:05d}\n")
            proc.stdin.flush()
        except Exception:
            return
        time.sleep(SHOTS / 1000)


if SHOTS and VIDEO != "Null":
    threading.Thread(target=shoot, daemon=True).start()
done = None
for line in proc.stdout:
    line = line.rstrip("\n")
    if line.startswith("orca caps"):
        proc.stdin.write("caps stats\n")
        proc.stdin.flush()
    # The product mode's position line comes 4 times a second: show one in 40.
    if line.startswith("orca replay time"):
        times = globals().get("times", 0) + 1
        globals()["times"] = times
        if times % 40 != 1:
            continue
    if line.startswith("orca replay"):
        print(f"[{time.time() - T0:6.1f}] {line}", flush=True)
    if line.startswith("orca replay start"):
        started.set()
    if line.startswith("orca replay done"):
        done = line
        break
    if line.startswith("orca error"):
        print(line, flush=True)
try:
    proc.stdin.write("quit\n")
    proc.stdin.flush()
    proc.wait(timeout=20)
except Exception:
    proc.kill()
print(f"work folder: {WORK}")
sys.exit(0 if done and " done ok " in done else 1)
