#!/bin/bash
# build-ruleset-fuzz.sh [seconds]: builds the ruleset reader's libFuzzer target (ruleset_fuzz.cpp)
# with Homebrew LLVM's clang (Apple's has no libFuzzer runtime), ASan + UBSan, and runs it for
# `seconds` (default 300) on one core, nice'd, from a corpus seeded with the builtin rulesets and
# the shared vectors' texts. Outputs in $OUT (default: a temp folder): the binary, corpus/,
# crash-* files if any, fuzz.log. Exits non-zero on a finding.
#   Tools/orca/fuzz/build-ruleset-fuzz.sh 600
set -euo pipefail
SECONDS_TO_RUN=${1:-300}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${OUT:-$(mktemp -d -t orca-ruleset-fuzz)}
CLANG=${CLANG:-/opt/homebrew/opt/llvm/bin/clang++}
mkdir -p "$OUT/inc" "$OUT/corpus"
# The schema compiled in, as Source/Core/Core/CMakeLists.txt writes it.
python3 - "$ROOT/Tools/orca/mods/ruleset.schema.json" "$OUT/inc/ruleset.schema.json.inc" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
open(sys.argv[2], "w").write("".join("'\\x%02x'," % b for b in data) + "\n")
PY
# Seeds: the builtin rulesets, and every vector case's text the harness can rebuild cheaply.
cp "$ROOT"/Source/Core/Core/Orca/Games/Brawl/Rulesets/*.json "$OUT/corpus/"
python3 - "$ROOT/Tools/orca/mods/ruleset-vectors.json" "$OUT/corpus" <<'PY'
import copy, json, sys
v = json.load(open(sys.argv[1]))
for i, c in enumerate(v["cases"]):
    if any(k.startswith("text") for k in c):
        t = c.get("text", v["baseText"])
        if "text_replace" in c:
            t = t.replace(c["text_replace"][0], c["text_replace"][1], 1)
        open(f"{sys.argv[2]}/v{i:03d}", "w").write(t)
        continue
    d = copy.deepcopy(v["base"])
    def parent(p):
        toks = p[1:].split("/"); at = d
        for k in toks[:-1]: at = at[int(k)] if isinstance(at, list) else at[k]
        return at, toks[-1]
    for p in c.get("remove", []):
        at, k = parent(p)
        del at[int(k) if isinstance(at, list) else k]
    for p, val in c.get("set", {}).items():
        at, k = parent(p)
        at[int(k) if isinstance(at, list) else k] = val
    open(f"{sys.argv[2]}/v{i:03d}", "w").write(json.dumps(d))
PY
nice -n 10 "$CLANG" -isysroot "$(xcrun --show-sdk-path)" -std=c++20 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
  -I"$ROOT/Source/Core" -I"$ROOT/Externals/picojson" -I"$OUT/inc" \
  "$ROOT/Tools/orca/fuzz/ruleset_fuzz.cpp" \
  "$ROOT/Source/Core/Core/Orca/Engine/StrictJson.cpp" \
  "$ROOT/Source/Core/Core/Orca/Engine/Schema.cpp" \
  "$ROOT/Source/Core/Core/Orca/Engine/Ruleset.cpp" \
  -o "$OUT/ruleset_fuzz"
cd "$OUT"
nice -n 10 ./ruleset_fuzz -max_total_time="$SECONDS_TO_RUN" -max_len=20000 -timeout=5 -rss_limit_mb=1024 \
  -print_final_stats=1 corpus 2>&1 | tee fuzz.log | tail -25
echo "fuzz output in $OUT"
