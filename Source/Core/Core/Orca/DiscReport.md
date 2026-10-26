# Standalone disc reports (phase 1)

`dolphin-nogui --disc-report <image> [--against <reference.json>]` opens its own
DiscIO volume and returns before application initialization. It does not boot,
initialize configuration, create a user directory, or write disc data. All
container types supported by DiscIO are read through the existing readers. The report
is read-only; Wii partition reads decrypt through VolumeWii as they do in the player.

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

Phase-1 tests use an invented GameCube image and independent Wii header JSON.
Layout/rebuild tests additionally construct encrypted synthetic Wii images with
invented ticket fields, signatures, certificate bytes and files.

## Standalone layout export and rebuild

```
build-disc-report/Binaries/orca-disc-tool --disc-layout <image> --out <layout.json>
build-disc-report/Binaries/orca-disc-tool --rebuild-standard <image> --layout <layout.json> --out <out.rvz> [--against <reference.json>]
```

Build the separate lab tool explicitly (the option defaults to OFF):

```sh
cmake -S . -B build-disc-report -DORCA_BUILD_DISC_TOOLS=ON
make -C build-disc-report -j2 orca-disc-tool
```

Release kits enable `ORCA_BUILD_DISC_TOOLS=ON` and ship the standalone tool beside
the engine. The player never links the layout,
rebuild or StandardBlob implementations, even when the lab option is ON.
`--disc-report` remains available in the player as before.

Both commands dispatch before application initialization, without configuration,
a user directory or a running session. They accept only the flags shown above
(`--flag=value` is also accepted). Use the same controller guards and shared lock
as for `--disc-report`. The lead runs the reference-disc export and real-disc proof;
unit tests use invented data only.

Orca tools skip loading a user's console dump on their volumes; ordinary emulator
callers retain the default IOSC behavior. This policy still uses IOSC's built-in
cryptographic defaults and does not disable decryption.

`--disc-layout` reads a hashed, encrypted Wii disc and writes version 1 layout JSON
(version 3 when physical-zero gaps require retained subtree digests; older readers
reject layouts with semantics they cannot reproduce)
through a uniquely reserved sibling temporary directory, then publishes it atomically
without replacing an existing destination. Success prints
`PASS: wrote <id> layout` and exits 0. Failure prints `FAIL: <reason>` and exits 1.
Existing output paths are refused; concurrent runs own separate temporary paths.
The schema records byte offsets and sizes, never the on-disc units of four bytes:

- `id`, `discNumber`, `partitionOffset`, and `partitions` in original table order (`offset`, `type`).
- `tmd`, `cert`, `h3`, `data`: partition-relative header ranges (`offset`, `size`).
  `data.size` includes the physical Wii hash headers and encrypted data.
- `systemFiles`: the five system paths with decrypted partition offsets and sizes;
  `fstMaxSize`: boot.bin's FST maximum size. These describe all four boot layout
  fields, the apploader and main.dol locations.
- `fst`: entries in original table order, including the root. Every entry records
  `name`, `directory` and `nameOffset` (relative to the string table). Directories
  have `parent`/`next` indices; files have decrypted `offset`/`size`. Name offsets
  preserve string-table order, duplicate strings and suffix sharing. Unused string
  space is zero. Export checks the regenerated FST against the original byte for
  byte; unrepresented padding or other opaque bytes cause FAIL instead of being
  copied into the JSON.
- `gaps`: the complete complement of files in decrypted partition data, as
  `offset`, `size`, `kind`. Each 0x8000-byte junk unit is compared first with the
  disc-ID/disc-number/offset formula (`junk`, no stored seed), then with a recovered
  LFG seed (`seeded`), then with zeros (`zero`); unmatched bytes are `other`.
  Zero padding before/after junk is split out. Adjacent portions of the same kind
  merge. A zero gap with `physicalZero: true` describes an all-zero physical Wii
  sector, including its hash header and IV. Its decrypted payload repeats a
  title-key-dependent AES block; it is distinct from the default decrypted-zero
  gap. Only the boolean is stored, and sectors are checked directly before using
  it. Regeneration preserves both the decrypted view and the zero physical sectors.
  Version 3 also records top-level `erasedHashes`, containing only `{level,index,digest}`
  subtree digests. Level 1 indexes sectors whose H1 entry survives in a sibling;
  level 2 indexes fully erased eight-sector subgroups whose H2 entry survives;
  level 3 indexes fully erased groups whose H3 entry survives in the table.
  These retain the tree from before erasure. Their positions must exactly cover
  the missing subtrees, without overrides for surviving data. Surviving payloads
  are still hashed normally. Version 2 layouts remain readable; an input with the
  same H3 can supply the missing metadata, otherwise a new standard layout export
  is required.
  A `seeded` entry has `seeds` in unit order, one for each intersected unit;
  each is a 136-character hex string containing 17 consecutive eight-digit,
  big-endian u32 words. These are generator state recovered from the owner's disc.
  Export prints one stderr line with byte totals by kind and the stored seed count.
- `h3Sha1` and `tmdContentSha1`: the same independent data digests as phase 1.

Layouts contain names, offsets, sizes, structure, generator seeds, subtree digests
and the two content digests. They must never contain raw FST, ticket, TMD, certificate,
file or gap bytes, title keys or key hashes. Layout and reference inputs are
limited to 32 MiB. Invalid trees, duplicate file paths, out-of-range fields,
overlaps and incomplete gap coverage are refused before output creation.

`--rebuild-standard` reads any container supported by DiscIO. It requires the same
game ID and file set/sizes, excluding the source FST size because it is regenerated.
With `--against`, it checks phase-1 file hashes on the virtual standard view before
writing: boot.bin's four layout fields come from the layout, title masking remains
as in phase 1, the partition offset is ignored, and the FST is excluded at this
stage. The reopened output must also match the reference's exact FST hash and size.
Without `--against`, the reconstructed partition's computed H3 digest provides
the standard-data check before writing; files are rehashed again after conversion.

The virtual disc retains the input disc header/region and only the game partition
at the requested offset; all remaining nonpartition space is zero. It preserves
the input ticket, certificate chain and TMD bytes, changing only the TMD content
hash to the computed H3 SHA-1. Signature bytes remain exactly as supplied. It uses
the existing Wii hashing/encryption path and the disc's title key, derived without
consulting a user console dump. There is no signing or fake-signing. Decrypted
buffers remain in memory; no extracted-disc temporary files are created.

RVZ output uses zstd level 5 and 128 KiB blocks. Conversion writes a file in a
uniquely reserved sibling temporary directory, then reopens it with DiscIO and verifies the partition offset, H3 SHA-1, TMD content
hash and every reference file before publication. It also compares the reopened
logical disc length and every byte with the generated view, including the outer
header, exact partition table, region, prefix padding, partition header and H3.
A mismatch deletes the partial and names the failed check: H3 SHA-1, TMD content
hash, partitionOffset, a file path, logical length, or a byte offset. SIGTERM cancels
reads/conversion and removes this run's partial. Publication atomically creates a
hard link at the final name and then removes the temporary name. It fails if any
destination appears during conversion, preserving the concurrent writer's file.
Temporary files stay on the destination filesystem; filesystems without hard-link
support are refused. Progress goes to stderr as `PROGRESS <done> 1000`. Success prints exactly
`PASS: rebuilt <id> to the standard layout` and exits 0. A file difference prints
`DIFF: ...`, exits 2 and writes no output. Other failures print `FAIL: <reason>` and
exit 1.

Junk handling mirrors `WIABlob.cpp::RVZPack` and `RVZPackDecompressor`: seeds restart
at multiples of 0x8000 in the input stream, and `LaggedFibonacciGenerator::GetSeed`
receives the offset modulo 0x8000. `SetSeed` followed by `Forward(offset % 0x8000)`
and `GetBytes`/`GetByte` reproduces a fragment. For Wii this stream is decrypted
partition user data, starting at zero: every physical 0x8000-byte block contributes
0x7c00 bytes, without its hash header. For GameCube/raw data it is the absolute
disc stream. Neither the Wii partition's physical offset nor its hash headers
enter the junk offset.

Dolphin has no disc-ID seed formula. The MIT-licensed NKit formula port is the
GameCube/raw form; on Wii it is only a candidate at the decrypted offset and must
match every byte before a portion is called `junk`. There is no assumption that
retail Wii partition junk follows that formula: recovered `seeded` state covers
other LFG output. Invented Wii fixtures verify both paths; the lead runs retail
proof. Unrecoverable fragments remain `other` and rebuilding refuses them.
Byte-exact FST regeneration still requires metadata-representable FSTs; opaque
nonzero FST padding is deliberately refused.

Source preflight bounds the partition counts/table extents before partition parsing,
and checks partition/header/data/FST ranges before filesystem traversal. The shared
Wii parser uses widened table indices and subtraction bounds before raw offset
arithmetic. Valid discs keep the same readers, decryption, metadata and read patches;
only invalid extents/counts are refused. The 256-partition resource bound exceeds
retail tables. Tool preflight additionally bounds FST nesting to 128 directories.
Synthetic compatibility tests cover default factories, both cached factory routes,
directory-backed reads, WAD content integrity, revision alias checks and rebuilt RVZ
reads. Real-disc identity, moved-partition and frame-hash release proofs remain the
engine lead's gates and are not claimed by the synthetic tests.
