// Offline performance run of the scripted Brawl match, with the env-gated measurements in
// Source/Core/Core/Rollback/Diag.h. Prints the diag lines and writes the full log.
//
//   DISC=<RSBE01 rev 2 image> [DOLPHIN=<DolphinNoGUI>] node Tools/orca/perf.mjs <label> [KEY=VAL ...] [-- dolphin args]
//
// Defaults: frames 6000-9000 (the fight; the script reaches scMelee at frame 5995), unthrottled,
// muted, Null video, a fresh user dir. Useful variables (KEY=VAL):
//   YG_THROTTLE=1             real-time pacing instead of unthrottled
//   YG_SAVE_ONLY_FROM=5900    a rollback snapshot at every frame (as an online match that predicts)
//   YG_SNAPTIME=1             time those snapshots (state / MEM1 / MEM2)
//   YG_NTCOPY=1               non-temporal MEM1/MEM2 copies (x86-64)
//   YG_PAGEDIFF=6000          dirty 4 KB pages per frame vs 1/2/4 frames earlier (slow; ignore its fps)
//   YG_EXIT_AFTER, YG_FRAMETIME to move the window.
// Set PERF_AUDIO=1 to keep the default audio backend.
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "..", "..");
const [label = "run", ...rest] = process.argv.slice(2);
const sep = rest.indexOf("--");
const pairs = sep < 0 ? rest : rest.slice(0, sep);
const extra = sep < 0 ? [] : rest.slice(sep + 1);

const dolphin =
  process.env.DOLPHIN ||
  [
    path.join(repo, "build", "release", "x64", "Binaries", "DolphinNoGUI.exe"),
    path.join(repo, "build", "Binaries", "DolphinNoGUI"),
  ].find((p) => fs.existsSync(p));
const disc = process.env.DISC;
if (!dolphin || !disc) {
  console.error("Set DISC to the Brawl image (and DOLPHIN if the build is not in the default place).");
  process.exit(2);
}

const user = path.join(os.tmpdir(), `orca-perf-${label}`);
fs.rmSync(user, { recursive: true, force: true });
fs.mkdirSync(user, { recursive: true });

const env = { ...process.env };
for (const k of Object.keys(env)) if (k.startsWith("ORCA_") || k.startsWith("YG_")) delete env[k];
Object.assign(env, {
  ORCA_SESSION: "1",
  YG_EXIT_AFTER: "9000",
  YG_FRAMETIME: "6000",
  YG_INPUT: path.join(here, "inputs", "bf-mario-link-results.txt"),
});
for (const p of pairs) {
  const i = p.indexOf("=");
  env[p.slice(0, i)] = p.slice(i + 1);
}

const args = [
  "-p", "headless", "-u", user, "-v", "Null",
  ...(process.env.PERF_AUDIO ? [] : ["-C", "Dolphin.DSP.Backend=No Audio Output"]),
  "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.BOOT=True", "-C", "Logger.Options.Verbosity=2",
  ...extra,
  "-e", disc,
];
const started = Date.now();
const child = spawn(dolphin, args, { env, stdio: ["ignore", "pipe", "pipe"] });
let out = "";
child.stdout.on("data", (d) => (out += d));
child.stderr.on("data", (d) => (out += d));
child.on("exit", (code) => {
  const log = path.join(os.tmpdir(), `orca-perf-${label}.log`);
  fs.writeFileSync(log, out);
  console.log(`[${label}] exit ${code}, ${((Date.now() - started) / 1000).toFixed(1)} s, log ${log}`);
  for (const line of out.split(/\r?\n/)) {
    if (/Diag |Orca: profile|Harness done|E\[|W\[/.test(line))
      console.log("  " + line.replace(/^\d+:\d+:\d+ \S+ /, ""));
  }
  fs.rmSync(user, { recursive: true, force: true });
});
