// Summarizes the first seconds of each match in a windowed harness run (ORCA.md, "Match start"):
// the host's VI intervals from 0.6 s before each scMelee scene to 12 s after it. VI intervals come
// from Graphics.Settings.LogRenderTimeToFile (<user>/Logs/vblank_times.txt), so this works on any
// Orca build.
//
//   node Tools/orca/match-start.mjs [--from-frame N] <run folder> [...]
//
// --from-frame N also counts VIs from the match's frame N on, located by the "JIT warm-up: frame N:"
// line (needs ORCA_JITWARM_LOG=1). The stage first appears at frame 28 in Project+ and ~170 in Brawl.
//
// A run folder holds:
//   stdout.log         the run's whole output, with YG_SCENES=1 (its "Scene frame" lines) and
//                      Logger.Logs.ROLLBACK=True; the log's own MM:SS:mmm stamps place the scenes
//   vblank_times.txt   copied from <user>/Logs after the run (delete it before the run)
//   meta.txt           "t_start <unix seconds>" and "t_end <unix seconds>" around the process
// The VI series is placed in time by its end: the "Diag frame times" line if the run has one
// (YG_FRAMETIME, printed right after the last frame), else t_end.
import fs from "node:fs";
import path from "node:path";

const VI_MS = 1000 / 59.94;
const BEFORE_S = 0.6;
const AFTER_S = 12;

// Windows PowerShell's `*>` writes UTF-16 with a byte order mark; everything else here is 8-bit.
function readText(file) {
  const bytes = fs.readFileSync(file);
  if (bytes[0] === 0xff && bytes[1] === 0xfe) return bytes.subarray(2).toString("utf16le");
  if (bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf)
    return bytes.subarray(3).toString("utf8");
  return bytes.toString("latin1");
}

function readMeta(dir) {
  const meta = {};
  for (const line of readText(path.join(dir, "meta.txt")).split(/\r?\n/)) {
    const m = /^(\S+)\s+(.*)$/.exec(line.trim());
    if (m) meta[m[1]] = m[2];
  }
  return meta;
}

// "MM:SS:mmm" (the wall clock's minutes and seconds) to unix seconds near tStart.
function logTime(stamp, tStart) {
  const [m, s, ms] = stamp.split(":").map(Number);
  let t = Math.floor(tStart / 3600) * 3600 + m * 60 + s + ms / 1000;
  while (t < tStart - 5) t += 3600;
  return t;
}

function stats(v) {
  const over = (x) => v.filter((y) => y > x).length;
  return {
    n: v.length,
    max: v.length ? Math.round(Math.max(...v) * 10) / 10 : 0,
    over25: over(25),
    over33: over(33.4),
    over50: over(50),
    lostMs: Math.round(
      v.filter((x) => x > 20).reduce((a, x) => a + x - VI_MS, 0),
    ),
  };
}

function run(dir, fromFrame) {
  const meta = readMeta(dir);
  const t0 = Number(meta.t_start);
  const scenes = [];
  const frameLines = [];
  let diagT = null;
  let boot = null;
  for (const line of readText(path.join(dir, "stdout.log")).split(/\r?\n/)) {
    let m = /^(\d+:\d+:\d+) .*Scene frame (\d+): (\S*) -> (\S+)/.exec(line);
    if (m) scenes.push({ t: logTime(m[1], t0), frame: Number(m[2]), to: m[4] });
    m = /^(\d+:\d+:\d+) .*JIT warm-up: frame (\d+): /.exec(line);
    if (m && fromFrame !== null && Number(m[2]) === fromFrame)
      frameLines.push(logTime(m[1], t0));
    m = /^(\d+:\d+:\d+) .*Diag frame times/.exec(line);
    if (m) diagT = logTime(m[1], t0);
    m = /Orca: (compiled .*|stopped waiting .*)$/.exec(line);
    if (m) boot = m[1];
  }
  const vbPath = path.join(dir, "vblank_times.txt");
  const vb = fs.existsSync(vbPath)
    ? readText(vbPath)
        .split(/\r?\n/)
        .filter((x) => x.trim())
        .map(Number)
    : [];
  const anchor = diagT ?? Number(meta.t_end);
  const total = vb.reduce((a, x) => a + x, 0);
  let acc = 0;
  const vis = vb.map((x) => {
    acc += x;
    return { ms: x, end: anchor - (total - acc) / 1000 };
  });
  const out = [`== ${path.basename(dir)}${boot ? `  (shaders: ${boot})` : ""}`];
  if (!vb.length) out.push("  no vblank_times.txt");
  scenes
    .filter((s) => s.to === "scMelee")
    .forEach((s, i) => {
      const inWindow = vis.filter(
        (v) => v.end >= s.t - BEFORE_S && v.end < s.t + AFTER_S,
      );
      const st = stats(inWindow.map((v) => v.ms));
      const stalls = inWindow
        .filter((v) => v.ms > 33.4)
        .map(
          (v) =>
            `${(v.end - v.ms / 1000 - s.t).toFixed(2)}s:${Math.round(v.ms)}`,
        );
      out.push(
        `  match ${i + 1} (frame ${s.frame}): VI n=${st.n} >25 ms ${st.over25}, >33 ms ${st.over33}, ` +
          `>50 ms ${st.over50}, worst ${st.max} ms, lost ${st.lostMs} ms`,
      );
      out.push(
        `    VI over 33 ms (seconds after scMelee:ms): ${stalls.join(" ") || "none"}`,
      );
      if (fromFrame === null) return;
      // The boundary before frame N: the first such line after this scMelee.
      const tf = frameLines.find((t) => t >= s.t - 0.002);
      if (tf === undefined || tf >= s.t + AFTER_S) {
        out.push(`    from frame ${fromFrame}: no "JIT warm-up: frame ${fromFrame}:" line`);
        return;
      }
      const after = inWindow.filter((v) => v.end - v.ms / 1000 >= tf - 0.001);
      const sa = stats(after.map((v) => v.ms));
      const late = after
        .filter((v) => v.ms > 25)
        .map((v) => `${(v.end - v.ms / 1000 - s.t).toFixed(2)}s:${Math.round(v.ms)}`);
      out.push(
        `    from frame ${fromFrame} (+${(tf - s.t).toFixed(2)} s): VI n=${sa.n} >25 ms ${sa.over25}, ` +
          `>33 ms ${sa.over33}, >50 ms ${sa.over50}, worst ${sa.max} ms; over 25 ms: ` +
          `${late.join(" ") || "none"}`,
      );
    });
  return out.join("\n");
}

const args = process.argv.slice(2);
let fromFrame = null;
const at = args.indexOf("--from-frame");
if (at >= 0) {
  fromFrame = Number(args[at + 1]);
  args.splice(at, 2);
}
if (!args.length || (fromFrame !== null && !Number.isInteger(fromFrame))) {
  console.error("usage: node Tools/orca/match-start.mjs [--from-frame N] <run folder> [...]");
  process.exit(2);
}
for (const d of args) console.log(run(d, fromFrame));
