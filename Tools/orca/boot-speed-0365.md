# Boot speed 0365 validation

Base: `8a0b69be6f`, `BASE-OK`. Private local commits on `train-0362`.

## Startup pacing

`BootThrottle::Begin` temporarily removes host throttling before `BootCore`. The first game
boundary restores it, unless a ring port or unthrottled harness owns pacing. Existing temporary
throttle owners are preserved. No emulated timing settings change.

Built `tests` and `dolphin-nogui` in `build-0362`, with two workers. Four new
`OrcaBootThrottle.*` tests pass. All three changed C++ translation units pass their build's
compile command with `-Werror -fsyntax-only`. Existing external-library deployment-target
linker warnings remain.

Mac M4 Pro, headless `-p headless -v Null`, no audio output, local input disabled and all six SDL
controller overrides from the brief. All builds and boots used heavy-slot, build.lock and nice 10.
The disc was read directly from the authorized WBFS. P+ used the official read-only launcher and
SD card in `/Volumes/YGWorktrees/OSCOO/pplus-3.2/run` (no copy).

Spawn to first game frame boundary, milliseconds, three warm runs:

| Game | Baseline | Startup pacing change |
|---|---|---|
| RSBE01 | 399.413, 385.502, 386.690 | 348.596, 354.223, 346.505 |
| PPLUS32 | 2659.595, 2675.645, 2654.428 | 560.728, 566.741, 568.691 |

The runner timestamps the first `Scene frame 0` / `Watch frame 0` diagnostic on the console;
this measures the first emulated boundary, not a GPU presentation. Runs use `YG_EXIT_AFTER=600`,
`YG_HASHLOG`, `YG_SCENES=1`, `YG_WATCH=80000000`. The harness takes over pacing from frame 0
in both binaries. Baseline boot is throttled, candidate boot is unthrottled.

Each candidate warm run's hashlog frames **0 through 600 inclusive** was compared byte for byte
with baseline: **601 lines per game per run, 3,606 lines total, zero differences**. The harness
requests asynchronous shutdown at frame 600 and may log extra frames while the host services
that request; those frames beyond the agreed N=600 are excluded. Frame 0 / frame 600 hashes:

| Game | Frame 0 | Frame 600 |
|---|---|---|
| RSBE01 | 553da1917d394108 | 8b7577e60071c841 |
| PPLUS32 | 3b7d2fd81e2ef856 | c782713deaeb18c1 |

This is the harness's per-frame RAM hash proof, not a bytewise comparison of device savestates.
Logs, complete hashlogs, the baseline binary and measurements are retained in
`build-0362/boot-speed-0365/` (baseline binary in `build-0362/Binaries/`).

Compatibility code fingerprint: `f86645e20656efe5 True`.


## Launcher verification

The legacy launcher profile checks a cache record under `<user>/Cache/OrcaBoot/` containing
canonical path, size, nanosecond mtime and ctime (NTFS LastWriteTime and ChangeTime), volume,
inode / 128-bit Windows file ID, verified XXH3-128 and engine revision. Every field and the
expected hash must match. Missing metadata or malformed records miss conservatively.

Every start launches a worker which fully hashes both files. Cache misses wait, preserving
verification before guest execution. Hits can boot offline while the worker audits; online
Start and StartRoom both await the audit before taking the room mutex and before any room
constructor, queue or ticket. Watch workers await the same audit before a watch-ticket POST or
socket connection. A mismatch deletes the failed record, reports the existing `profile` refusal and
stops the session on the host thread. Failure completed during core initialization is delivered
when the session watcher is registered. Audit refusal reporting is shared and occurs once.
Shutdown cancels a pending audit before stopping the CPU; early boot failures and destruction
also cancel. The hasher checks the stop flag before every 8 MiB read and before returning its
digest. Cancellation is not a pass, emits no mismatch refusal and leaves all cache records
unchanged; every later start still performs a full audit. Metadata changes during hashing refuse
the boot.

Refusal order remains disc open/identity first, then launcher, then SD. That is the cache
implementation's order: restoring launcher/SD-first order on a warm start would require waiting
for their full audit before a disc refusal, delaying startup failures on slow storage.

CompatibilityKeyParts is unchanged: it still uses the profile's EXPECTED launcher and SD
hashes. No computed or cached value is copied into those fields. Mod mode retains its existing
bundle/SHA-256 and generated-card verification; it is a separate path, not this helper.

The audit checks below live in `Source/UnitTests/Core/Orca/BootAuditTest.cpp`. The 16 draft
checks are retained, with four additional cases. Tests that check the process-wide refusal
status execute in fresh processes, including on macOS after configuration threads have started.

| Required case | Test names (suite prefix omitted) |
|---|---|
| 1. Warm return while hashing is pending | `ColdVerificationPopulatesAWarmRecordAndStillAudits` |
| 2. Full audit on a hit | `AllLauncherFilesAreAuditedOnEveryStart` |
| 3. Online start waits | `StartCannotRequestATicketUntilTheAuditPasses`, `StartRoomCannotOpenARoomUntilTheAuditPasses` |
| 4. Existing refusal, dropped record, no room | `FailedWarmAuditDropsTheRecord`, `FailedAuditRefusesStartAndStartRoomWithoutARoom`, `MismatchCannotRequestATicketOrOpenARoom`, `FailureCompletedDuringInitializationStillEndsTheSession` |
| 5. Changed bytes, matching cache identity | `ChangedBytesWithMatchingCacheIdentityRefuseBeforeOnline` |
| 6. Size / mtime / file ID changes miss | `SizeChangeIsAMiss`, `ModificationTimeChangeIsAMiss`, `ReplacedFileIsAMissEvenWithTheSameSizeAndMtime` |
| 7. Compatibility key unchanged | `CompatibilityKeyUsesExpectedHashesWithOrWithoutTheCache` |
| 8. Damaged / unreadable cache misses | `CorruptCacheRecordsAreMisses`, `UnreadableCacheRecordIsAMiss`, `PermissionDeniedCacheFileIsAMiss` |

Change-2 `tests` and `dolphin-nogui` builds pass. The focused
`*BootAudit*:*Profile*:*Online*:*Compatibility*` run passes **142/142**, including **20/20**
audit tests and all eight required cases above. The five touched translation units pass
`-Werror`. Compatibility code fingerprint: `f86645e20656efe5 True`.

The first full-suite run had one existing TURN classification failure
(`OrcaDirectLink.FourFriendsExerciseAllSixTurnPairs`); ten isolated repetitions then passed
without changing that code or test. The full-suite rerun through
`python3 Tools/orca/room-fix-tests.py build-0362/Binaries/Tests/tests --gtest_brief=1` passes:
**2,370 tests, 2,347 passed, 23 skipped, zero failures** (192 suites, 628.757 s).
The skips are optional fixture/asset checks; none of the required audit cases skipped.

Three warm PPLUS32 runs, using the same headless Null / no-audio / frame-0 diagnostic as the
change-1 measurements above:

| Run | Change 1 alone (ms) | With launcher cache + audit (ms) |
|---|---:|---:|
| 1 | 560.728 | 346.807 |
| 2 | 566.741 | 353.715 |
| 3 | 568.691 | 347.481 |
| Median | 566.741 | 347.481 |

The warm median is **219.260 ms (38.7%) lower** than change 1 alone. The change-1 measurements
are the recorded baseline above. Both persisted cache identities matched the canonical path,
size, nanosecond mtime / ctime, volume and file ID immediately before each timed cache run.
One priming run is excluded from the warm statistics. Every boot used the authorized read-only
disc and official read-only P+ files, local input disabled, and all six SDL overrides.

Each warm run's frames **0 through 600** matched the change-1 baseline byte for byte:
**1,803 lines compared, zero differences**. The priming run also matched all 601 lines. Logs,
hashlogs, timing JSON and gate outputs are in `build-0362/boot-speed-0365/change2-*`.

## Engine follow-up

Base `686af357aa` passed `git merge-base --is-ancestor 686af357aa HEAD` (`BASE-OK`).
All original audit assertions remain. The existing Start/StartRoom fixtures additionally check
that Transport (the room accessor), Seat, SeatName and HelloControls answer during a pending
audit, for both passing and failing starts. Nine additional tests cover:

| Test | Coverage |
|---|---|
| `CancelledHashNeverReturnsADigest` | Cancellation produces no digest, including for an empty file |
| `CancellationFinishesAtTheReadBoundaryAndPreservesAllRecords` | Held in-flight read, no next read after cancellation, no pass/failure callback, unchanged cold/warm cache, full re-audit on restart |
| `DestructionCancelsTheWorker` | Destruction releases the worker without waiting for the held hash |
| `SessionCancellationReleasesOnlineStartWithoutARoom` | Session cancellation releases a waiting StartRoom and creates no room |
| `ProductWatchWaitsBeforeConnecting` | App watch socket starts only after a pass |
| `DevWatchWaitsBeforeRequestingATicket` | Dev watch POST and socket start only after a pass |
| `FailedAuditNeverRequestsAWatchTicketOrConnects` | Both watch paths refuse without contacting the loopback fixture |
| `WatchDestructionDoesNotWaitForTheAudit` | Both watch waits can be abandoned without cancelling the audit |
| `FailedWatchAuditReportsTheProfileRefusalOnce` | Fresh-process exit status and single original profile refusal |

The focused `*BootAudit*:*Profile*:*Online*:*Compatibility*:*ReplayStream*:*OfflineReplay*`
run passes **197/197**, with no skips. This includes all **29 audit tests**. The read-boundary
test holds an injected read so cancellation is deterministic; production HashFile checks the
same atomic stop flag around its 8 MiB reads. Cancellation and destruction release within the
500 ms test bound after an in-flight fixture read is released.

Both `tests` and `dolphin-nogui` build successfully in `build-0362`. All seven touched
translation units pass their build compile commands with `-Werror -fsyntax-only`.
Compatibility code fingerprint: `f86645e20656efe5 True`. Existing external-library
deployment-target linker warnings remain. Builds and gates use heavy-slot, build.lock,
nice 10 and at most two build workers.

The full binary ran once through
`python3 Tools/orca/room-fix-tests.py build-0362/Binaries/Tests/tests --gtest_brief=1`:
**2,379 tests, 2,356 passed, 23 skipped, zero failures** (192 suites, 633.399 s).
The skips remain optional fixture/asset checks; no required audit test skipped.

Headless RSBE01 and PPLUS32 frames **0 through 600** each match the retained change-1
(`5d1dfce6e4`) baseline: **1,202 lines, zero differences**. All subsequent P+ timing runs
also match their 601 baseline lines, including priming runs. Every boot uses `-p headless
-v Null`, no audio, `ORCA_NO_LOCAL_INPUT=1`, all six SDL controller-off overrides, the
authorized read-only WBFS and the official read-only P+ launcher/SD. Warm runs check both
cache identities before launch.

The historical **347.481 ms** warm median was not reproduced. The first three candidate
warm runs measured **392.313, 395.318, 413.242 ms** (median **395.318 ms**). A repeat
measured **1067.137, 355.689, 485.296 ms** with unchanged code. To control for machine
variation, the parent `686af357aa` was built locally in the same build, with the candidate
source and binary backed up and restored byte for byte. After one excluded priming run per
binary, three alternating runs measured:

| Run | Parent 686af357aa (ms) | Candidate (ms) |
|---|---:|---:|
| 1 | 363.689 | 407.935 |
| 2 | 422.879 | 465.881 |
| 3 | 448.245 | 389.381 |
| Median | 422.879 | 407.935 |

The candidate median is 3.5% below the contemporary parent control, but the broad timing
variation prevents confirming the absolute 347.5 ms gate. No timing result is discarded
from this record. The source remains identical to the candidate that passed the tests and
`-Werror`; the build is restored to that source after the control experiment. Artifacts are
retained under `build-0362/boot-speed-0365/change3-*` and `paired-*`, and both local control
binaries under `build-0362/Binaries/`.
