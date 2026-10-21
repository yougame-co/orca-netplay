#!/usr/bin/env python3
# jit-history.py: does a game's emulated machine depend on its JIT's history? (ORCA.md "Code the JIT
# doesn't see".) Runs the same scripted boot twice, headless and unthrottled: once as it comes, once
# with every JIT block dropped at the end of the frames --clear names (as Dolphin does when its code
# space fills, at a moment that differs between machines). Compares the RAM hash logs frame by frame,
# and lists, at the frames --census names, every instruction word a live JIT block was compiled from
# that RAM no longer holds (code rewritten without an icbi). Exit 0 when the hash logs match.
#
#   jit-history.py <nogui> <disc> [--pplus "<dir>/Project+ Netplay Launcher.dol"] [--input <script>]
#                  [--frames 6000] [--clear 388,2000] [--census 600,3000] [--out <dir>] [--x86]
#
# Census lines count every such word and name those below 0x80800000 (the DOL and the codes), which
# still run; the rest (the heap, MEM2) are usually modules the game unloaded, never run again: the
# .jit file in --out lists them all ("stalecode" lines).
import argparse, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
p = argparse.ArgumentParser()
p.add_argument("nogui")
p.add_argument("disc")
p.add_argument("--pplus")
p.add_argument("--input")
p.add_argument("--frames", type=int, default=6000)
p.add_argument("--clear", default="388,2000")
p.add_argument("--census", default="")
p.add_argument("--out")
p.add_argument("--x86", action="store_true", help="the binary is x86-64 under Rosetta 2")
a = p.parse_args()
out = os.path.abspath(a.out or tempfile.mkdtemp(prefix="orca-jit-history-"))
os.makedirs(out, exist_ok=True)
script = a.input or os.path.join(HERE, "inputs", "pplus-1v1.txt" if a.pplus else
                                 "bf-mario-link-results.txt")
census = a.census or ",".join(str(f) for f in (1, 3, a.frames // 2, a.frames - 10))


def run(name, extra):
    user = tempfile.mkdtemp(prefix=f"orca-jit-{name}-")
    # The shell's own harness knobs would make "as-is" another history: none pass through.
    env = {k: v for k, v in os.environ.items() if not k.startswith("YG_")}
    env.update(ORCA_SESSION="1", YG_SCENES="1", YG_INPUT=os.path.abspath(script),
               YG_EXIT_AFTER=str(a.frames), YG_HASHLOG=os.path.join(out, name + ".hashlog"),
               YG_JITCODE_LOG=os.path.join(out, name + ".jit"), YG_JITCODE_CENSUS=census, **extra)
    if a.pplus:
        env["ORCA_PROFILE"] = "PPLUS32"
    if a.x86:
        env["ROSETTA_ADVERTISE_AVX"] = "1"
    cmd = [os.path.abspath(a.nogui), "-p", "headless", "-v", "Null", "-u", user,
           "-C", "Dolphin.DSP.Backend=No Audio Output", "-C", "Logger.Logs.ROLLBACK=True",
           "-C", "Logger.Options.Verbosity=2", "-C", "Logger.Options.WriteToConsole=True"]
    cmd += (["-C", f"Dolphin.Core.DefaultISO={os.path.abspath(a.disc)}", "-e",
             os.path.abspath(a.pplus)] if a.pplus else ["-e", os.path.abspath(a.disc)])
    t0 = time.time()
    with open(os.path.join(out, name + ".log"), "w") as log:
        code = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    shutil.rmtree(user, ignore_errors=True)
    print(f"{name}: exit {code}, {time.time() - t0:.0f} s")
    return code


def hashlog(name):
    frames = {}
    path = os.path.join(out, name + ".hashlog")
    if not os.path.exists(path):
        sys.exit(f"{name}: no hash log (see {os.path.join(out, name + '.log')})")
    for line in open(path):
        w = line.split()
        if len(w) >= 2 and not line.startswith("#"):
            frames[int(w[0])] = (w[1] if len(w) == 3 else "", w[-1])
    return frames


def report_census(name):
    words = []  # a census's words come before its summary line; only the low ones are printed
    for line in open(os.path.join(out, name + ".jit")):
        w = line.split()
        if len(w) > 4 and w[1] == "stalecode" and int(w[2], 16) < 0x80800000:
            words.append(f"    {w[2]} runs {w[3]}, RAM holds {w[4]}")
        elif len(w) > 2 and w[1] == "census":
            print(f"  {name} {' '.join(w[2:])}")
            for word in words:
                print(word)
            words = []


codes = [run("as-is", {}), run("cleared", {"YG_CLEARJIT_AT": a.clear})]
report_census("as-is")
base, other = hashlog("as-is"), hashlog("cleared")
common = sorted(set(base) & set(other))
diff = [f for f in common if base[f][1] != other[f][1]]
if diff:
    print(f"DIFFER: {len(diff)} of {len(common)} frames, the first {diff[0]} ({base[diff[0]][0]})"
          f" after clears at {a.clear}")
else:
    print(f"SAME: {len(common)} frames, clears at {a.clear}")
print(f"logs in {out}")
sys.exit(0 if not diff and not any(codes) and len(common) >= a.frames else 1)
