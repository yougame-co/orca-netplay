# Metal shader-cache comparison on macOS

Run an uninstrumented BASE and candidate Orca.app with Brawl USA Rev 2, isolated users and Apple compiler-cache namespaces. No installation, signing, screen capture, online room or credential is needed. Requires existing Python 3, Node, macOS tools and the project's resource scheduler.

```sh
python3 Tools/orca/metal-shader-cache/run.py \
  --root /Volumes/YGWorktrees/repro/shader-cache-unique \
  --base-app /path/to/base/Orca.app --new-app /path/to/new/Orca.app \
  --disc /path/to/Brawl-rev2.wbfs \
  --heavy-slot /path/to/heavy-slot.sh \
  --gate /path/to/benchmark-gate.sh --yield-script /path/to/luis-yield.sh
```

The root must be new. The runner copies apps, clones the source disc read-only, disables Discord/analytics/audio, and prepares empty caches. It keeps each arm's caches through one cold and three warm launches, ordered BASE, NEW, NEW, BASE, BASE, NEW, NEW, BASE. Every launch queues `--pool gpu --game --prio 3 --mac-idle 600 --label orca:metal-shaderbin` and then runs the supplied gate immediately before spawning. The gate must fail closed unless idle >=600 seconds, unlocked, Luis not playing, load <20 and free memory >20%. The runner stops after three hours, preserving all evidence. `luis-yield.sh status` must say `Luis is not playing`; it is rechecked during play.

The existing `inputs/bf-cpu-loop.txt` scene script, its recorded SHA256, canonical session NAND and `ORCA_TEST_RNG_SEED=0x03981031` fix the game sequence. `ORCA_TEST_DEV_GAME=1` permits that existing test knob. Read-only guest probes capture the stage, all four character/type slots and RNG at match offsets 30 and 600. Every later launch must match BASE cold's roster/stage and both RNG snapshots or the run fails. Probe files and hashes remain available to inspect the full initialization structure too.

Each run lasts 3000 guest frames. The local stdio bridge replies locally and refuses every multiplayer ticket. `ready` is absent in this nonembedded mode, so the report uses the separately named first-frame signal. Frame p99/max include boot menus and replay-origin capture; they are not gameplay-only statistics. `sc/sm` cover emitted full Perf windows only. No shared Apple or Dolphin cache is erased.

`results.json`, `summary.json` and `BENCHMARK.md` contain metrics. Raw console, gate, cache, timing, config and fixture artifacts remain under each arm. Cleanup is deliberately manual: the manifest lists the exact owned Apple namespaces and the root contains the owned disc clone; preserve evidence before deleting only those test assets.

For a separate diagnostic experiment built with temporary `Common/ShaderCacheTrace.h` instrumentation, add `--diagnostic`. It passes `ORCA_SHADER_CACHE_TRACE=1`; production runs leave this absent. `python3 Tools/orca/metal-shader-cache/correlate.py ROOT` correlates every traced >100 ms CPU-frame interval with native-thread steady-clock spans. Same-time activity on another thread alone does not prove a blocking cause; nested spans must not be summed. Temporary engine instrumentation is intentionally not part of this harness commit.
