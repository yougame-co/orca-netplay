# Standalone disc reports (phase 1)

`dolphin-nogui --disc-report <image> [--against <reference.json>]` opens its own
DiscIO volume and returns before application initialization. It does not boot,
initialize configuration, create a user directory, or write disc data. All
container types supported by DiscIO are read through the existing readers.

Stdout contains a JSON header, then file objects `{path,size,sha1}` sorted by path,
then one verdict line. Digests are lowercase SHA-1 hex. Exit codes are 0 for PASS,
2 for DIFF, and 1 for FAIL. With no reference, successful reading ends with
`PASS: files match <game id>`. On failure, stdout may contain only `FAIL: <reason>`.

The header contains `id` and `revision`; Wii also includes `tmdContentSha1`,
`h3Sha1` and `partitionOffset` (the absolute start of the game partition, in bytes).
Layout and scrub status are omitted: neither is inferred from file equality.
The TMD digest and actual H3 digest are reported independently.

Every FST file is included at its root-relative path. The additional entries are
`sys/main.dol`, `sys/apploader.img`, `sys/bi2.bin`, `sys/fst.bin`, and `sys/boot.bin`.
Only boot.bin bytes `[0x20,0x60)` are zeroed in memory before hashing.
FST bytes and boot.bin's layout fields remain part of the comparison: a recomposed
disc with a different FST will produce DIFF even if its other files match.

References have the shape `{name,files:[{path,size,sha1}],partitionOffset?}`.
Missing, extra, resized or changed files each count once. Optional partition offset
mismatches count as a difference named `partitionOffset`. The verdict lists at
most five sorted differing paths. Header hashes and revision are informational;
the supplied reference selects the expected revision. Duplicate paths, invalid
sizes and invalid hashes fail reference parsing. Reference files are limited to
32 MiB. Directories themselves are not entries.

## Make a reference from the agents' own Rev 2 disc

Run from the worktree. Use the binary built here (the bundle path on team Mac
builds is `build-disc-report/Binaries/Orca.app/Contents/MacOS/Orca`). Keep these
controller guards on every binary invocation, including unit tests:

```sh
export ORCA_NO_LOCAL_INPUT=1
export SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_HIDAPI_GAMECUBE=0 SDL_HIDAPI_LIBUSB=0
export SDL_JOYSTICK_IOKIT=0 SDL_JOYSTICK_MFI=0
export SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000
```

A full disc read takes a few minutes. Shell redirection below writes metadata only;
the report command itself writes only stdout.

```sh
build-disc-report/Binaries/Orca.app/Contents/MacOS/Orca \
  --disc-report <path to a Brawl Rev 2 image> \
  > build-disc-report/rsbe01-rev2-report.jsonl

python3 - <<'PY'
import json
from pathlib import Path

directory = Path("build-disc-report")
lines = (directory / "rsbe01-rev2-report.jsonl").read_text().splitlines()
assert lines[-1] == "PASS: files match RSBE01", lines[-1]
header = json.loads(lines[0])
assert header["id"] == "RSBE01" and header["revision"] == 2
reference = {"name": "RSBE01 Rev 2", "partitionOffset": header["partitionOffset"],
             "files": [json.loads(line) for line in lines[1:-1]]}
(directory / "ref-rsbe01-rev2.json").write_text(json.dumps(reference, indent=2) + "\n")
PY

time build-disc-report/Binaries/Orca.app/Contents/MacOS/Orca \
  --disc-report <path to a Brawl Rev 2 image> \
  --against build-disc-report/ref-rsbe01-rev2.json \
  > build-disc-report/rsbe01-rev2-against.jsonl
```

The reference `build-disc-report/ref-rsbe01-rev2.json` contains only paths, sizes, hashes, its name and the partition offset.

Synthetic unit tests use an invented GameCube image. Wii header JSON and optional
offset comparison are tested separately; there is no encrypted Wii unit fixture.
No rebuild or output-image implementation is part of phase 1.
