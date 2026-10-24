# Orca

> **2026-10-13: YG Baiji is now YG Dolphin.** Players see YG Dolphin from 0.3.42 on. The code,
> files and contracts keep the codename Orca. Packages are `YG Dolphin.app`, `yg-dolphin.exe`
> and `yg-dolphin/yg-dolphin`; the Mac program stays `Contents/MacOS/Orca`. Tags are
> `yg-dolphin-<version>` from 0.3.42; older `yg-baiji-` and `orca-` tags keep their names.
> Earlier notes are left as they were.

> **2026-10-12: players see the name YG Baiji** from 0.3.40 on (Orca up to 0.3.39; the baiji is the
> Yangtze river dolphin). The builds still use neither Dolphin's name nor its logo. This document,
> the code and every contract keep the codename Orca: `namespace Orca`, `Core/Orca`, `Sys/Orca/`,
> the `orca <verb>` lines and caps, `orca2:` keys, `orca-<game>` modes, the `ORCA_*` variables,
> `Config/Orca.ini`, `OrcaControls-<GAMEID>.txt`, the log prefixes and the bundle id
> `co.yougame.orca`. What ships is `YG Baiji.app` (its program still `Contents/MacOS/Orca`),
> `YG Baiji/yg-baiji.exe` and `yg-baiji/yg-baiji` where this document says Orca.app,
> `Orca/Orca.exe` and `Orca/orca`. Public tags are `yg-baiji-<version>` from 0.3.40. Text written
> before this note is left as it was.

Orca is YouGame's GameCube and Wii rollback netcode core: a fork of the
[Dolphin](https://github.com/dolphin-emu/dolphin) emulator. Every Orca source file carries an SPDX
`GPL-2.0-or-later` tag, as Dolphin's own do; in aggregate the repository is GPLv3-compatible (see
[COPYING](COPYING)). Orca is not affiliated with or endorsed by the Dolphin project or Nintendo, and
its builds do not use Dolphin's name or logo.

How Orca's rollback works and how it compares with Slippi and Brawlback: [ORCA_ARCHITECTURE.md](ORCA_ARCHITECTURE.md).

- **Base:** Dolphin release `2609` (`f84df02`), plus the four security fixes of the `2609a` hotfix
  (cherry-picked with `-x` in Orca 0.3.36). The `upstream` remote is `dolphin-emu/dolphin`; Orca is
  rebased onto later releases deliberately.
- **Design:** an exact whole-machine snapshot at each frame boundary; controller input injected at
  the SI device; re-run frames that parse the GPU command stream but skip host rendering; per-game
  profiles. Super Smash Bros. Brawl (`RSBE01` rev 2, and rev 1 played as rev 2) and Project+ v3.2 (a
  launcher profile on top of Brawl) are the supported games.
- **Online:** through YouGame rooms. Friends drop in to a running game; strangers come from
  YouGame's matchmaking queue (casual or ranked).

No disc images, NAND dumps or save data belong in this repository. Bring your own disc.

This document is the engineering reference: how to run and test Orca, and how each subsystem works
and why. Paths are relative to `Source/Core/Core/` unless they start with `Source/`, `Data/` or
`Tools/`.

## Contents

- [Building](#building)
- [Running a session](#running-a-session)
- [Mods (phase 1)](#mods-phase-1)
- [Testing](#testing)
- [Rollback and snapshots](#rollback-and-snapshots)
- [Determinism across hosts](#determinism-across-hosts)
- [Drop-in](#drop-in)
- [Direct links](#direct-links)
- [Input delay](#input-delay)
- [Input latency](#input-latency)
- [Music switch](#music-switch)
- [Match start](#match-start)
- [Graphics](#graphics)
- [Code the JIT doesn't see](#code-the-jit-doesnt-see)
- [Game patches](#game-patches)
- [Free space and the match block](#free-space-and-the-match-block)
- [Input gate](#input-gate)
- [Online menu](#online-menu)
- [Matchmaking and results](#matchmaking-and-results)
- [Online rules](#online-rules)
- [No CPUs online](#no-cpus-online)
- [Ranked sets](#ranked-sets)
- [Stage select](#stage-select)
- [Character select](#character-select)
- [On-screen UI](#on-screen-ui)
- [Embedding](#embedding)
- [The sandbox](#the-sandbox)
- [Changes by release](#changes-by-release)

## Building

Orca builds like Dolphin (CMake). The targets that matter are `dolphin-emu-nogui` (what the YouGame
desktop app runs) and `tests`. On macOS, `Tools/orca-package-macos.sh` makes **Orca.app**, the
bundle the desktop app starts, and zips it as `YG Baiji.app` (from 0.3.40). On Linux (x86-64),
`Tools/orca-package-linux.sh` makes the `yg-baiji/` folder (`Orca/` up to 0.3.39) the app's
`linux-x64` build unpacks: `yg-baiji/yg-baiji` (the NoGUI program), `Sys/` and the licences. It builds with no Qt, Vulkan, ALSA, PulseAudio (Cubeb opens the system's sound
server itself, so its PulseAudio headers must be there at build time), BlueZ, evdev or libudev,
links libstdc++ statically, and refuses a program that needs a library beyond glibc, X11 and
EGL/OpenGL, one that lacks X11, Xi or EGL, or a glibc newer than `ORCA_GLIBC_MAX` (2.35, Ubuntu
22.04's). Build it on a distribution that old, with GCC 12 or newer, so it runs on the ones since.

Differences from a stock Dolphin build:

- **libcurl** is the bundled copy on macOS and Linux (with mbedTLS and the system CA file), because
  the netcode needs libcurl's WebSocket support. That client doesn't use the macOS Keychain's extra
  roots and doesn't do TLS 1.3. On Windows the bundled libcurl uses Schannel and the system store,
  with certificate revocation checked best effort.
- **libjuice** 1.7.4 (`Externals/libjuice`, MPL-2.0, used under its section 3.3) provides ICE for
  direct links.
- `Orca/` and `Rollback/` build with `-ffp-contract=off` on clang and gcc
  ([Determinism across hosts](#determinism-across-hosts)).

## Running a session

A session is `ORCA_SESSION=1` plus a disc whose profile is in `Data/Sys/Orca/`. Boot refuses any
other disc, and anything that would change what the game computes (`Orca/Profile.h`). A session also
refuses whatever would change one player's game alone: pausing (except solo, see
[Embedding](#embedding)), savestates, the reset button and the Wii power button.

Online play goes through a private YouGame room of up to four (`Orca/Session/Online.h`). The host
plays at once; a friend who joins plugs into the next port ([Drop-in](#drop-in)).

Two players meet when both use the same room code and the same **compatibility key**: the Orca
build, the game profile and its data files, every forced setting, the disc and the seeded save. The
host CPU is not in the key: an Apple Silicon Mac and an x86-64 PC compute the same game, so Mac and
PC players meet. An x86-64 CPU without FMA3 has a different key. The host (the Orca that started the
game) is seat 0, controller port 1; a friend's seat is its room slot.

### Environment

| Variable | Meaning |
|---|---|
| `YOUGAME_BRIDGE`, `YOUGAME_TOKEN` | Set by the YouGame desktop app: tickets come from its loopback bridge. |
| `YOUGAME_STDIO` | `1`, set by the app instead: its bridge is Orca's stdin and stdout ([The stdio bridge](#the-stdio-bridge)). |
| `ORCA_NET` | Set by the app when it sandboxes Orca: `internet` or `internet-lan` ([Network](#network)). |
| `ORCA_ROOM` | Room code (6-12 of `a-z0-9`). Default: the app's invite, else a new code. |
| `ORCA_JOIN` | `1`: join the friend's game in that room. |
| `ORCA_NAME` | Name shown in the room. |
| `ORCA_PROFILE` | A launcher profile (below), e.g. `PPLUS32`. |
| `ORCA_DIRECT` | `0`: no direct links, the relay only. Default on. |
| `ORCA_THREAD_QOS` | `0`: no raised thread priority; `1`: also for a scripted harness run. |
| `ORCA_JITWARM` | `0`: no JIT warm-up ([Match start](#match-start)). |
| `ORCA_MUSIC` | `off`: start with the music off ([Music switch](#music-switch)), for harness runs. |
| `ORCA_SHADER_WAIT_S` | Set by the app: how long boot may wait for shaders (60-900 s). |
| `ORCA_CONTROLS` | Set by the app: the player's own controls, 92 hex ([Each player's own controls](#each-players-own-controls)). |

Test-only variables (most are "test overrides": they change the compatibility key, so only two Orcas
with the same value meet):

| Variable | Meaning |
|---|---|
| `ORCA_TEST_DEV_GAME`, `ORCA_SITE` | An anonymous dev ticket from the site (default `https://yougame.co`) instead of the app. No ratings, no account. |
| `ORCA_TEST_KEYFRAME_DIR` | Keyframes go through this folder instead of YouGame's store. |
| `ORCA_TEST_LEAVE_AT` | A joined friend leaves at this frame. |
| `ORCA_TEST_COMMANDS` | `1`: take the embed protocol's stdin commands without embedding, plus test commands: `test-drop-room`, `test-direct off\|on\|in\|out\|rebuild`, `test-shot <name>`. |
| `ORCA_TEST_DROP_SILENT` | `1`: `test-drop-room` drops the room connection the way a network failure does: no close, the socket stays open and unread, and the server only notices when the player's next connection replaces it. |
| `ORCA_TEST_PRESENT` | Headless only: `WxH` presents into an offscreen image with the OSD and overlay, so screenshots show what a player sees. |
| `ORCA_TEST_PADS`, `ORCA_TEST_SKIP_RENDER` | Harness overrides. |
| `ORCA_TEST_FULL_SNAPSHOTS` | `1`: snapshots copy all of RAM instead of copy-on-write. |
| `ORCA_TEST_NO_AFP` | `1`: an ARM CPU with FEAT_AFP behaves as one without (Apple M4 as M1-M3). |
| `ORCA_TEST_CPUCORE` | `0` (interpreter) or `5` (cached interpreter) as a reference. |
| `ORCA_TEST_NET_DELAY_MS`, `ORCA_TEST_NET_SPIKES`, `ORCA_TEST_NET_UPLINK` | A simulated bad link (`Orca/Profile.h`). |
| `ORCA_DIRECT_FORCE` | `turn`: relayed candidates only; `srflx`: nothing on a host interface. |
| `ORCA_DIRECT_TURN` | `0` never use TURN, `1` from the first link. Default: only after a link found no path. |
| `ORCA_DIRECT_STUN`, `ORCA_DIRECT_ICE_LOG`, `ORCA_DIRECT_LOOPBACK` | Leave STUN out; log every libjuice line (addresses masked); offer loopback candidates for two Orcas on one machine. |
| `ORCA_DIRECT_LOSS`, `ORCA_DIRECT_BLOCK` | Drop this percentage of datagrams; start with every datagram dropped. |
| `ORCA_TEST_GATE` | Input-gate masks ([Input gate](#input-gate)). |
| `ORCA_TEST_QUEUE`, `ORCA_TEST_QUEUE_PICK` | A queue room's header, and a matched joiner's pick ([Character select](#character-select)). |
| `ORCA_TEST_CHAR_ORDER`, `ORCA_TEST_SET_RULES` | Run the ranked character order, or set the stock/time rules, without a real room. |
| `ORCA_UX_TEST_NAMES`, `ORCA_UX_TEST_CONTROLS`, `ORCA_UX_TEST_PLUG_AT` | Names, controls and a plug-in frame for harness ports. |
| `ORCA_TEST_RNG_SEED` | The seed every history's game starts from in a dev game's room ([Random numbers](#random-numbers)). |
| `ORCA_TEST_HOLD`, `ORCA_TEST_SHADER_WAIT_MS`, `ORCA_TEST_FXC_FLAGS`, `ORCA_TEST_JITWARM_SLOW_US`, `ORCA_JITWARM_SEED`, `ORCA_JITWARM_LOG`, `ORCA_JITWARM_BUDGET_US` | Knobs for the sections that describe them. |
| `ORCA_UX_PROBE` | Memory watches, dumps and writes for reverse-engineering game state. |
| `ORCA_UX_KIT_DEMO`, `ORCA_UX_SETEND_DEMO`, `ORCA_UX_OVERLAY_DEMO` | Draw overlay elements for design checks. |

### Orca.app and the disc

Orca.app is a session by default (`ORCA_SESSION=0` turns that off). Its user directory is
`~/Library/Application Support/Orca` unless `-u` says otherwise. Without `-e` it asks for the
player's own Brawl disc image, checks the game ID and revision before boot, and remembers the path
in `Config/Orca.ini` `[Disc] Path`. A file given with `-e` is checked before anything else starts:
a missing file prints `orca error disc_missing <path>`, an unreadable one `orca error
disc_unreadable <path>`, and Orca exits 1 within a second. Orca's alert handler is registered before
the boot file is read, so no hidden message box can block a run.

Brawl (USA) (Rev 1) plays as Rev 2. The two dumps differ only in the revision byte of both disc
headers and in one main.dol instruction (`0x8001BC9C`, the version in the disc ID the game builds at
boot); every file, the file table, the partition layout, the IOS and the title version are the same.
Every disc a session boots, and the launcher profile's disc, goes through `Orca::AliasRevision`
(`Orca/Disc.h`): for that exact dump (the content hash in its TMD) reads of those three bytes return
Rev 2's (`VolumeDisc::SetReadPatches`), so the console reads a Rev 2 disc and Rev 1 and Rev 2 players
meet in the same rooms. The TMD and ticket still differ in hashes, signatures and the ticket ID; the
game never reads them: a Rev 1 and a Rev 2 run of the default inputs (Brawl and Project+) give the
same hashlog. Other revisions and modified Rev 1 copies are refused, and so is a launcher's disc that
doesn't read as Rev 2 at boot (`Orca::SessionDiscReady`, e.g. after a read error).

`Orca --verify <disc> [-u <user>]` checks a disc image and exits, without booting, a window, a
session or the app's bridge (`Orca/DiscVerify.h`). It runs Dolphin's integrity check (the Wii hash
tree; no Redump lookup, no whole-disc hashes) and prints `orca verify progress <done> <total>` in
bytes at most once a second and one `orca verify problem <low|medium|high> <text>` per problem, with
Dolphin's severity. A Medium or High problem counts as damage unless it is only in Brawl's
Masterpiece partitions, which Orca and Project+ never read (some WBFS tools wipe them); each one
that counts is followed by `orca verify damage <text>`. Last comes `orca verify ok` (exit 0) or
`orca verify damaged <n>`, n damage lines (exit 2). A file that isn't a disc image prints `orca
verify unreadable <path>` (exit 1). The signature checks use a temporary NAND, never the player's:
a folder in the system's temp folder named for the process (`orca-verify-nand-<pid>-<n>`). A verify
that is killed (the app's SIGTERM, TerminateProcess on Windows) leaves its folder behind, so each
verify first removes those whose process is gone.
A GameCube disc has no hash tree, so only its structure is checked. A good scrubbed Brawl Rev 2
`.wbfs` reports 9 Low problems: no update partition, no stored image size, and errors in unused
blocks (8088 in DATA, 1 in each Masterpiece partition).

Sessions start from an empty NAND (save hash 0); the game makes its save in-session on both
machines. A session with no `Config/GCPadNew.ini` gets `Sys/Orca/Input/GCPadNew.ini`: port 1 on the
keyboard and the first SDL gamepad of any model.

### Launcher profiles (Project+)

A mod that boots through its own loader runs from a launcher profile (`ORCA_PROFILE`;
`Data/Sys/Orca/PPLUS32.ini` is Project+ v3.2). The session boots the loader (`-e <launcher.dol>`)
with the disc as Dolphin's default disc, and refuses unless the loader, the disc's ID and revision
and the SD card image next to the loader match the profile (XXH3-128). The SD image is opened
read-only. The profile also names the save that seeds the session NAND, the game's title, and its
game patches. None of a mod's files are in this repository; the player downloads the official
release.

```bash
ORCA_SESSION=1 ORCA_PROFILE=PPLUS32 dolphin-emu-nogui -p headless -u <user> -v Null \
  -C "Dolphin.Core.DefaultISO=<your Brawl (USA, Rev 2) disc>" \
  -e "<dir>/Project+ Netplay Launcher.dol"
```

Orca.app started with no arguments reads `orca-launch.ini` next to the app if the build ships one:
`[Launch]` with `Profile` and `Executable` (relative to that folder). The parser refuses unknown keys
and sections, and any path that is absolute, has a `.` or `..` component, or resolves outside the
folder (`Orca/Launch.h`). A mod's native build is Orca.app plus the mod's files and that ini.

### The local controller

Under the YouGame app, the app's controller helper reads GameCube adapters (libusb, raw reports) and
gamepads, and Orca takes the newest snapshot of its stream at each frame boundary
(`Orca/UX/ControllerSource.*`). Without the app, it is port 1 as mapped in Dolphin's controller
settings. A session never runs Dolphin's own adapter code.

`orca stats` reports the input age: `pa`, the mean age of the newest adapter report when a frame
reads it (only reads whose pad changed), and `rpf`, adapter reports per frame. Expect a little over
4 ms at the official adapter's 125 Hz and a little over 0.5 ms at 1 kHz. The age is measured on one
clock per span (the helper's, then Orca's); the hop between processes is left out because their wall
clocks drift and step, and its jitter is logged separately. `Tools/orca/controller-latency.mjs`
measures the whole transport (0.1-0.3 ms on loopback); `Tools/orca/fake_bridge.py` with
`FAKE_PAD_HZ` stands in for an adapter.

## Mods (phase 1)

Side branches `mods-0334` (`mods-p1` on `release-0.3.34`) and `bundle-flat` (flat bundles, below,
on top of it), not on main: nothing ships until YouGame switches to this architecture. Design:
YouGame `docs/ORCA_MODS.md` ("Phase 1 contract" and its adjustments). A session reads its game from a
mod bundle instead of `Data/Sys/Orca/<profile>.*`: a tree bundle laid out as a run folder, chosen by
`ORCA_MOD_DIR`, or a signed flat bundle that Orca finds by itself ("Flat bundles"). Without either
every legacy code path runs exactly as on the release (each mod branch keys on `Profile::mod`, set
only by `LoadModProfile` and `LoadFlatProfile`, or on `Mod::Requested()`).

| Variable | Meaning |
|---|---|
| `ORCA_MOD_DIR` | The run folder: `mod.json`, `tree.json`, the launcher, `sd/...`, `orca/patches/*.patches`, `orca/textures/`, `orca/jitwarm`. Mod mode. `ORCA_PROFILE` is ignored. |
| `ORCA_SD_CACHE` | Where a mod's SD image is built (required when `mod.json` has `sd`). |
| `ORCA_DISC_SHA1` | The dump's canonical SHA-1, in the key (`-` when unset in mod mode; legacy's key mixes it in too). |

- **`mod.json`** (`Core/Orca/Mod.cpp` `ParseManifest`, `Tools/orca/mods/mod.schema.json` for the
  site): every field of the design's example, unknown keys and malformed values refuse the session,
  so do trailing text after the JSON value. `forced` takes only `Dolphin.Core.CustomRTCValue`
  (what PPLUS32.ini forces besides the card); the card itself follows `sd`. `saveTitle` only the
  disc's own. `firstKeyframeFrame` (default and least 300) replaces `FIRST_KEYFRAME_FRAME` and
  `FriendsMove::FIRST_FRIEND_FRAME`. `engine.api` above `Mod::ENGINE_API` (1) refuses. `minOrca`
  above the running YG Dolphin (`Mod::EngineVersion`, CMake's `ORCA_VERSION`, set in
  `Source/Core/CMakeLists.txt`; X.Y.Z compared number by number) refuses before any other check,
  with the player's message "This pack needs YG Dolphin <minOrca> or newer. Update the YouGame app
  and try again." as the session's error (`OrcaMod.MinOrcaAboveThisBuildRefuses`).
- **`tree.json`** (written by the app): sorted, unique also without case, no file that is also a
  folder, paths of at most 100 bytes of printable ASCII with no `..`, leading dots, trailing dots or
  spaces, Windows device names or `<>:"\|?*`. `tree` must be the contract's hash of the list
  (`Tools/orca/mods/tree-hash-vector.json`, the vector the app and site share). Every file must be
  there at its size; `mod.json`, the launcher and `orca/patches/*` also at their sha256.
- **The SD card** is built from exactly the tree's `sd/` files (`Common::BuildSDImageFromFiles`:
  FatFsUtil's FAT32 format, the tree sorted, FAT times zero) at `sd.bytes` (a multiple of 32 KiB, 64
  MiB to 2 GiB, so SDSC), into `ORCA_SD_CACHE/<sdTreeHash>-fat2.raw` (sha256 of `bytes=<n> cluster=<c> fats=<f>` and the
  `sd/` lines) under a temporary name, then renamed, with its XXH3-128 in a `.xxh3` note. It is
  reused while it hashes as its note says (a damaged one is rebuilt), the newest two are kept, and
  it is mounted exactly like a legacy `sd.raw` (read-only, writes and folder sync forced off). Its
  hash is `sd=` in the key.
- **The key**: `orca2:<xxh3>` over `orca2|<commit>|api=1|tree=<treeHash>|sd=<image or ->|disc=<sha1 or
  ->|`, the forced settings (the card's switch, then `forced`), the universal settings, `|ux=`. The
  player's `User/GameSettings` INIs never load in mod mode (`GameConfigLoader`). `orca2:` is the
  room protocol's prefix; legacy hashes start `orca|`, mod hashes `orca2|`, so the two modes never
  share a key.
- **Builtins** replace the game-id checks (`ProfileRuleset`, `ResultsVerified`, `BrawlExecutable`,
  NameTags, MenuText, `VanillaBrawl`, `RequestQueueRules`; JitWarm's `KnownGame` and the harness's
  scene reader take the declared builtin, host-only). `brawl-rev2`, `brawl-rev2-pplus` and
  `brawl-rev2-results` run only while their guard words hold (`Mod::BuiltinGuards`: the frame loop's word, r13's base, the scene
  manager / g_GameGlobal / pad system loads, the message buffer's vtable; for `brawl-rev2` Brawl's
  word at 0x8001CD24, which Project+'s codes rewrite (not the v-sync call any more: since 0.3.33
  RSBE01.patches moves it as Project+ does; `OrcaMod.GuardsAreNotOrcasOwnPatches`); Project+'s stage select routines at 0x80553594, 0x80554320, 0x805518D0 for
  `brawl-rev2-pplus`, or where its `pplusGuards` puts them), checked against memory at every entry into the UX (frame callback, input gate
  source and latch). The modules run exactly while the words hold in memory, a pure function of
  memory on first runs and re-runs alike: Project+'s codes land in frame 1, so its guards hold from
  frame 2, and a joiner re-executing frame 1 from its own origin runs it with the modules off, as
  the host did (`OrcaMod.GuardsFollowMemoryOnReRuns`). Only the caps keep history: they wait until
  the guards held once, and not holding at or after `firstKeyframeFrame` is a failure for the rest of
  the boot (a warning, the gated caps withdrawn). Caps: `mod.json` `offers` ∩ the builtin, unless failed; the
  pick caps (`pick-casual pick-ranked`) go with `locks`, and `music` follows the same rule as legacy
  (`Music::Supported`: the disc is RSBE01; host-only, so no guards). Whether a game's main menu is
  read for a fresh start's origin ("Fresh starts", `FreshMove::Supported`) is asked once, as the
  match is set up and before any guard is read: in mod mode it is the declared builtin, so a mod
  boots to the same origin as legacy; the menu itself is still read only while the guards hold, so
  a mod that moved what its builtin relies on gets no origin and takes no players, on every machine
  alike.
- **Results without a ruleset (`brawl-rev2-results`)**, for Brawl-engine mods without Project+'s
  code (PM 3.6, Brawl Minus, Legacy): RSBE01 revision 2 only, profile game id `RSBE01`, its own game
  compat 1 (the key's `ux=<engine>.1`; Brawl and Project+ also use game revision 1 from 0.3.48).
  Its guards are the eight Brawl words alone. `offers` may name only `results`, and only with `rankedRules`: the
  parser refuses `locks` and `queue2`, and `results` without declared rules (Brawl's own defaults, a
  2-minute timed match with items, would count), and `Mod::Offers` offers nothing else for it, so no
  pick caps either. Without `rankedRules` it reads no results (`Describe().results` is false) and
  plays no Ranked, so friends rooms only. It is the legacy
  `brawl-rev2-launcher` game in mod mode: results (`ResultsVerified`: the vanilla scene manager and
  `g_GameGlobal`'s result info, never the match block), the Online menu's Casual/Ranked picks, fresh
  starts, name tags, menu text and music. Its ruleset is `None`, so nothing that needs one runs: no
  header, rule or stage writes (`Rules::OnFrame`), no queue character select or latch (`Queue`), no
  stage flows (`BrawlStages`, `RankedPPlus`), ranked set (`RankedSet`), character order
  (`CharOrder`), CPU and seat rules (`OnlineSeats`), stage cursors, CSS title, relabels or native
  text, and no input masks (`BrawlModule::Masks`). It never writes sora_scene's free space
  (`BrawlGame::UsesFreeSpace`): there the match block and code caves may be the mod's own code, so
  the friends' drop-in move (`FriendsMove`) is off too and the host picks With Friends itself. Ranked
  admits it (`RoomRefusal` asks only for `results`), unlocked: nothing in the game enforces stages
  or rules, and each game is reported as its results screen shows it. The legacy launcher game is
  unchanged. `OrcaMod.ResultsOnlyAdmitsRankedWithoutRulesetGates`.
- **`rankedRules`** (`brawl-rev2-results` only, either bundle format, owner-only like `builtin`;
  required to offer `results`): `"rankedRules": {"stocks": 3, "timeMinutes": 8, "items": "off"}`,
  all three required, `stocks` and `timeMinutes` integers 1-99, `items` only `"off"`. On the results
  screen the reader compares the rules the fight started with, which only the Rules menu changes:
  `g_GameGlobal +0x1C` (gmSetRule, the block the online rules lock: `+0x02` low 3 bits 1 for a stock
  match, `+0x04` stocks, `+0x08` the stock match's time limit in minutes) and the item frequency at
  `0x9017BE50` (gmGlobalRecord's menu data, RSBE01.patches' items group; 0 None), then two rules the
  engine fixes for every such game, not pack fields: `+0x05` handicap off (0) and `+0x06` damage
  ratio 1.0 (10), as the locks set them. The first rule that differs, or can't be read, makes the
  game void `rules-stocks`, `rules-timeMinutes`, `rules-items`, `rules-handicap` or
  `rules-damageRatio` (the room gets `void`, as for `nocontest`), never a win or a loss, with one
  line `Orca result: frame <n>: not rated, its <rule> is not the ranked rules'`. Casual and ranked
  rooms alike; nothing is written or locked. Both layouts are Brawl rev 2's; a mod that moved them
  voids every game, and one real 1v1 on the mod proves them. `OrcaResults.RankedRules*`.
- **P+ fork guards** (flat format 2 only): the listing owner may add
  `"pplusGuards": {"rssExdata": "0x80551DD4", "pageIndex": "0x80552B70", "currentPage": "0x80552F90"}`
  with `builtin: "brawl-rev2-pplus"` (Project+Ex 1.7's NETBOOST.GCT holds Project+ v3.2's three
  lis/ori pairs there). Each address is the first word of its 8-byte pair, 4-byte aligned, in
  `[0x80550010, 0x805A0000)` (`Mod::PPLUS_CODESET_REGION`: the codeset is read to 0x80550010 and
  nothing in the repo bounds its end, so the window stops below Brawl's globals), and the pairs
  don't overlap. The engine reads exactly those six words, no search; the words stay its own
  constants. Without the key the v3.2 addresses apply, so Project+ v3.2's key and boot are
  unchanged. The boot's `Orca mod: builtin ... on` / `failed` lines name the declared addresses,
  and with the key a second line names the known limits below. The key moves the guards only:
  - Code addresses in Project+ v3.2's own files: a pack's `.patches` replace PPLUS32.patches in mod
    mode, and v3.2's BootToCSS group (`PPLUS32.patches:41-44`, `0x80559C20`-`0x80559C2C`, reached from
    the hook at `0x806DD5F8`) and menu groups (`:62-168`, from `0x81179ED8`: P+'s menu module at
    `0x81164C00`, Brawl's addresses + `0x1480`) are v3.2's; a fork's pack adapts its own copy (the
    guarded groups simply don't land on other code). PPLUS32.jitwarm names v3.2's blocks, codeset
    ones included (hash-checked, so a fork's pack ships its own seed or a stale one only warms less). No other engine code names a codeset address
    (`PanicDiag.h`'s `0x80558678` is a crash example in a comment).
  - Data stays v3.2's: RankedPPlus and the online rules read and write RSS_EXDATA (`0x8042C4E8`), its
    page index (`0x8042C821`), the strike table, the current page (`0x80496000`) and the Code Menu
    (`0x804E0000`-`0x804E2520`) at v3.2's layout. Known limits on a fork: the online rules write
    v3.2's 2024 stage preset (`PPLUS_2024_RSS`) over the fork's RSS_EXDATA on a locked header's
    character select, and lock the Code Menu only where v3.2's layout is found (`CodeMenuPresent`):
    a fork with another Code Menu is not locked.
- **Bundles**: `Tools/orca/mods/make-bundle.py RSBE01|PPLUS32 <out> [--pplus-user <release user/>]
  [--tree]` (no `tree.json` in a bundle; `--tree` makes a local run folder, `--tree <folder>` alone
  rewrites one). Brawl's run folder is 153 files, Project+'s 4,033 (3,878 on the card).
- **Limits** (`Mod.h`): what the engine reads whole is refused above its size before it is read:
  `mod.json` 64 KiB, `tree.json` 16 MiB, each `orca/patches/` file 1 MiB, a `.dol` or `.elf` at the
  root (any case; all hash-checked) 64 MiB, `orca/jitwarm` 16 MiB. JSON nests at most 16 levels
  (picojson recurses per level, so a deep document would overflow the stack).

Launch (what the desktop app does with `YOUGAME_ORCA_DEV_MOD=<folder>`):

```bash
ORCA_SESSION=1 ORCA_MOD_DIR=<run folder> ORCA_SD_CACHE=<cache> [ORCA_DISC_SHA1=<sha1>] \
  dolphin-emu-nogui -p headless -u <user> -v Null -e <disc>                       # no launcher
  ... -C Dolphin.Core.DefaultISO=<disc> -e <run folder>/launcher.dol                # mod.json "launcher"
```

**Follow-ups, 2026-10-05:**
- **Caps wait for the guards.** In mod mode "ready" and the `orca caps` line before it wait until
  the builtin's guards are decided (held, or failed by `firstKeyframeFrame`; at most 20 s, then the
  line goes out without them), so a builtin cap is never offered before its guards hold; Project+'s
  "ready" moves from the launcher's first frame to the game's frame 2. A failure after they held
  withdraws them with a second `orca caps` line: the app already takes a later line as the new offer
  (desktop `orca.ts` recomputes offered and agreed; the page answers it again), so no app change.
  What is on follows each offer at once (`Status::UpdateCaps`). Legacy never waits.
  `OrcaMod.ModeGates`, `OrcaMod.CapsWaitForTheGuards`. Checked headless: the one `orca caps` line
  of both bundles and of legacy Project+ is `… results locks queue2`.
- **SD geometry.** `sd.clusterBytes` and `sd.fats` pin the card's FAT32 geometry; make-bundle.py gives
  Project+ its official image's (boot sector: 2 KiB clusters, two FATs, 32 reserved sectors), and the
  builder (`fat2`) lays each folder out files first, by upper-cased name, as the official image does
  195 of its 204 folders. Card `0615853dd38b01c51a3608d0199fa966` (tree `830384ca…34af4`), built
  twice byte-identical.

### Reaching Casual/Ranked from a non-P+ Brawl build

For a `brawl-rev2-results` pack (PM 3.6, Brawl Minus, Legacy), whose mod has no Project+ codeset,
offering `results` with its `rankedRules` (without them it plays friends rooms only). What Orca
reads needs a builtin whose guards hold, never a ruleset ([Online menu](#online-menu)):

- **The pick.** The Online menu reader (`OnlineMenu.cpp` `ReadMenuState`,
  `OnlineMenuReader::Boundary`) follows the scene manager (`[0x805A0060]`: `+0x4` the scene, whose
  name is at `+0`) from the scene named `muMenuMain` into the one named `scMemoryChange`, and reads
  the main menu's exit code at `[0x805A0060] +0x284`: **30** is Casual, **31** Ranked (24-27 With
  Friends). The menu writes the code itself as it leaves (then its step, 2, at `+0x288`); 30 and 31
  are Brawl's own With Anyone codes (Basic Brawl, Team Battle), so a menu that keeps those buttons
  needs no patch for them. Picked with the game alone and the app's `host`, the search starts at
  the pick (no `queue2`: `UX/MenuEvents.cpp` `AnnounceOnlinePick`). With friends in the game the
  pick is kept, unarmed (no pick caps without `locks`).
- **What the pack's `.patches` must do**, after Project+'s groups in `Data/Sys/Orca/PPLUS32.patches`
  (RSBE01.patches has the same groups for Brawl's own menu; Project+'s menu module is Brawl's
  `+0x1480`). Read every original word from the mod's own RAM on its main menu and guard each group
  with them, so a group never lands on other code:
  1. Reach the ONLINE page without the connection window (its save check and network task):
     `:60-68`, the top page's two Play Online press checks (`bne` -> `b`).
  2. Leave the ONLINE page without the disconnect window and keep With Friends local (exit 25):
     `:70-155`; never enable Options or Spectator: `:157-168`.
  3. Send exit codes 30 and 31 to the local Versus sequence instead of the network ones, leaving
     the code where the reader saw it: `:170-181`, `sqMenuMain::setNext`'s table (in sora_scene,
     which Project+ keeps as Brawl's: codes 24-27 and 29-31 to the block at `0x806DC8C8`), and
     `:183-211`, that block (`sqVsMelee` as VERSUS, the code kept at `sqVsMelee +0x18`), with the
     back out at `:213-217`. A mod that keeps Brawl's sora_scene can take these groups as they are.
  4. Boot to the main menu: a build that boots straight to its character select needs its own
     group like `:31-44` (Project+'s BootToCSS), and `:46-50` skips the title.
- **Not for this pack:** the toggled group at `:219-234` acts on the match block's header
  (`0x806F2380`), which a results-only pack never has, and nothing else here needs the match
  block, a header or the Code Menu.
- Proof is the pack author's: on the mod's disc, Play Online > With Anyone > Casual must print
  `orca menu online casual` and land on the local character select (`Tools/orca/online-menu.py`
  is the legacy flow's check), and one real 1v1 in a matchmade room must log `Matchmaking: a game
  ended at frame ...` with its winner.

### Flat bundles

A flat bundle is a game's data as at most eight files in one folder, signed, so it can ride as a
YouGame listing's runtime files: the app lays those out in `userData/orca/run/<slug>/` beside
`userData/orca/user/` (the `-u` folder), and a Project+ or Brawl change becomes a file update instead
of an Orca release (`Core/Orca/Bundle.h`).

| File | What |
|---|---|
| `mod.json` | format 2: as format 1, plus `listing` (the slug it is for) and `serial`, optional `pplusGuards` for P+ forks and `rankedRules` for results-only builds, with `sdImage` `{file, bytes, sha256}` (the card used as it is), `patches` (file names, applied in order), `jitwarm`, `textures: "engine"` instead of `sd`. |
| `mod.sig` | `ed25519 <public key> <signature>`, lowercase hex, one line. |
| the rest | exactly the files `mod.json` names: a launcher, the SD image, up to four `.patches`, a JIT seed. |

- **Where Orca looks**, once per process at boot, before any game setting is read
  (`Mod::ResolveBundle`): `ORCA_MOD_DIR` when set (with a `tree.json` it is a tree bundle, as
  above); else a launcher boot's own folder, where Project+'s `sd.raw` already lies; else, for a disc
  boot, `<-u folder>/../run/<YOUGAME_GAME>/`. No `mod.json` there: legacy, exactly as before. A
  launcher boot must run the bundle's launcher, and a disc boot needs a bundle without one.
- **The signature** is over `orca-flat-bundle 1\n` and one `<name>\t<sha256>\t<bytes>\n` line for
  `mod.json` and every file it names but the SD image, sorted by name; `mod.json` pins the card by
  size and chunked SHA-256 (SHA-256 over the SHA-256 of each 64 MiB piece, which hash on every core:
  about 0.15 s for Project+'s 2 GB card on an M-series Mac; `sdhash.py`). The launcher's line is
  checked on the bytes the boot read, and the patches and JIT seed are used as they were verified,
  so nothing is read again after the check. Verified with `Externals/ed25519`.
- **Keys.** Only a key the build pins may sign: CMake `-DORCA_BUNDLE_PUBLIC_KEY=<64 hex digits>`. A
  build without one trusts no key, and every flat bundle is refused. `-DORCA_BUNDLE_TRUST_TEST_KEY=ON`
  adds the bundle-flat TEST key for tests and gates only; `orca-package-macos.sh` turns it off and
  fails if the executable holds it. `OrcaBundle.PinnedKey` checks the build trusts exactly what
  CMake was given.
- **Listing and serial.** Under the app (`YOUGAME_GAME` set), a bundle for another listing is
  refused. `MIN_FLAT_SERIAL` (`Bundle.h`) refuses older bundles: a release raises it to retire a
  signed bundle, which can't otherwise be taken back.
- **Hostile input.** The folder must hold exactly the named files (hidden files such as the app's
  `.copies.json` aside), at most eight, no folders or links; names are the app's runtime file names
  (no `/`, no `..`, no device names); `mod.sig` is read first and must be by a trusted key before
  `mod.json` is parsed; every read is bounded as in a tree bundle (`mod.sig` 512 bytes); the small
  files are hashed and the signature checked before the card is hashed. Anything wrong refuses the
  whole bundle with an `Orca bundle: <folder> refused: <why>. The session is refused.` line, and the
  session ends with `orca error profile YG Baiji refused this game's pack: <why>` (no folder path),
  whether Orca found the bundle by itself or `ORCA_MOD_DIR` named it: a refused pack never plays the
  built-in game in its place.
- **Mod mode** then runs as for a tree bundle (builtins, guards, caps). Textures stay the engine's.
  The key is `orca2|...|tree=<hash of the signed lines but the JIT seed's>|sd=<the card's pin>|...
  |sys=<the Sys game INIs>|...`: a bundle player never meets a legacy one, a JIT seed change keeps
  the pool, anything that reaches the game moves it.
- **Signing.** Sign only a bundle that played on two Orcas: a builtin whose guards never hold gets no
  origin, so its players meet nobody. `sign-bundle.py sign` checks the folder as Orca does, the
  card's pin included, before it signs.
- **Tools**: `make-bundle.py RSBE01|PPLUS32 <out> --flat --serial <n> [--listing <slug>]
  [--pplus-user <user/> | --pplus-run <folder>] [--key <private key>]` (Project+'s launcher and card
  must be v3.2's, by full SHA-256); `sign-bundle.py keygen|sign|verify|message`
  (standard library Ed25519, `ed25519ref.py`); `sdhash.py` (the card's pin); `flat-sig-vector.json`
  and `sd-hash-vector.json` tie the Python tools to the engine (`OrcaBundle.SignatureVector`,
  `OrcaBundle.ChunkedSha256`); `mod.schema.json` states format 2. Brawl's bundle is 4 files,
  Project+'s 6.
- **Limits**: a listing change moves ranked at once (no pinned versions). The run-folder layout is a
  contract with the app (`orca.ts`: `userData/orca/{user,run/<slug>}`); if it moves, Brawl plays
  legacy (Project+ follows its launcher). `minOrca` and `engine.api` are enforced. A refused bundle
  ends the session with its reason (above).

Open: a P+ fork whose codeset moves the stage select routines loses `brawl-rev2-pplus` until phase
3's labels; a bundle's own patches are not yet checked against the guard words (the validator's
job); duplicate JSON keys are last-wins (as in JavaScript; the site refuses them).

What was checked (2026-10-05, then on 0.3.33 and in flat form on 0.3.34 on 2026-10-08, with the
runs): ORCA_NOTES.md, "Mods (phase 1)" and "Flat bundles".

## Testing

Run tests headless (`-v Null`), muted, and at low priority on a shared machine. Two-instance runs
each start two emulators.

### Unit tests

```bash
ninja tests && ./Binaries/Tests/tests --gtest_filter='Orca*'
```

Live, network and disc tests skip without their switches (`ORCA_TEST_DISC` for a Rev 2 image,
`ORCA_TEST_DISC_REV1` for Rev 1). `ORCA_LIVE_TEST=1` runs `OrcaLive.*`
against real rooms on yougame.co.

### The harness

`Rollback/Harness.h` is a test harness driven by environment variables (it needs `ORCA_SESSION=1`).
The header documents every knob; the main ones:

| Variable | What it does |
|---|---|
| `YG_INPUT=<file>` | Scripted pads (`Tools/orca/inputs/`): hold, mash and fuzz rules, counted from scene entry with `@<scene>`. |
| `YG_EXIT_AFTER=<n>` | Stop after first-pass frame n. |
| `YG_HASHLOG=<file>` | A RAM hash per frame. Two builds that should compute the same game must produce the same log. |
| `YG_SYNCTEST=<k>` | Save every frame, rewind k, re-run, compare RAM and device-state hashes. `0 RAM mismatches` is the bar. `_FROM`, `_DIAG`, `_SECTIONS`, `_CLEARJIT` refine it. |
| `YG_KEYFRAME_TEST` | Capture and load a drop-in keyframe, then check 30 re-run frames. |
| `YG_PADREC=<file>`, `YG_LOOPBACK=<file>` | Record pads and checksums, then replay them through a real session with a late virtual remote (`_LAG`, `_JITTER`, `_SPIKE`, ...). |
| `YG_THROTTLE=1` | Run in real time instead of unthrottled. |
| `YG_SCENES=1`, `YG_RESULTS=1` | Log scene changes and each game's result. |
| `YG_SHOT_EVERY`, `YG_SHOT_AT`, `YG_DUMP_FRAMES_FROM/_TO` | Screenshots and frame dumps. |
| `YG_CLEARJIT_AT`, `YG_JITCODE_LOG`, `YG_JITCODE_CENSUS`, `YG_WATCH` | JIT history diagnostics ([Code the JIT doesn't see](#code-the-jit-doesnt-see)). |

A device-state-only mismatch in a sync test is expected (the emulated icache, GPU dirty flags, host
counters); only RAM mismatches are bugs. Settings go through Dolphin's config system names:
`Graphics.*`, never `GFX.*` (silently ignored). A forced session setting (`Orca/Profile.cpp`) beats
`-C`.

Example: the reference Brawl run.

```bash
ORCA_SESSION=1 YG_INPUT=Tools/orca/inputs/bf-mario-link-results.txt YG_EXIT_AFTER=10800 \
  YG_HASHLOG=brawl.hash dolphin-emu-nogui -p headless -v Null -e <your Brawl (USA, Rev 2) disc>
```

### Two-instance tests

These start two Orca processes and steer them as the desktop app does, over real rooms on yougame.co
with dev tickets (nothing is recorded). Each takes one to three minutes.

| Tool | What it covers |
|---|---|
| `Tools/orca/dropin-commands.py` | Join, leave, rejoin, the host leaving. |
| `Tools/orca/dropin-rooms.py` | A room with no host, a friend first in the host's room, the host's connection dropping; a guest going home, leaving the friends lobby for the queue, a kept pick. |
| `Tools/orca/dropin-delay.py` | The input delay for five minutes after a join, optionally on a simulated bad link. |
| `Tools/orca/dropin-identity.py` | Each player's own name-tag controls in the other's game. |
| `Tools/orca/direct-link.py` | Direct links: off/in/on, rebuilds, TURN, the ticket's switch. |
| `Tools/orca/versus-cpu-dropin.py` | A friend dropping into Group > Brawl's character select with CPUs on it: both machines clear them and seat the friend at the same frame. |
| `Tools/orca/online-menu.py`, `online-menu-dropin.py` | Every path through the Online menu (one Orca, offline, no network access allowed); a friend dropping in on it. |
| `Tools/orca/queue-e2e.mjs` | The matchmaking flow: casual and ranked, rematch, skip, timeout, kill. Plays the page's half of the protocol. |
| `Tools/orca/free-space.py` | That the match block and code caves are never touched by the game. |
| `Tools/orca/jit-history.py` | Code the JIT compiled that RAM no longer holds. |
| `Tools/orca/occluded-test.py` | Frame pacing with the window covered, with a friend, or with the overlay opened. |
| `Tools/orca/xplat.py` | One Orca on each of two machines (`--role join` on the second). The only test that can show a Mac and a PC disagreeing. |

All take `<dolphin-emu-nogui> <disc>`, and `--pplus "<dir>/Project+ Netplay Launcher.dol"` for
Project+.

### Match start timing

How long a queue match takes from "matched" (the page's `/match` socket) to both players in control
on the room's character select, phase by phase. `Tools/orca/match-latency.mjs` drives two headless
Orcas through YouGame's real queue, rooms and keyframe store as the desktop app's page does (the
in-process bridge of `queue-e2e.mjs`), as two `/sandbox` roster players, N matches in a row; it
records every line both print, with times. `match-latency-report.mjs` cuts each match into phases
(the matched room's ticket, the room connect, the host's fresh start and unseen hold, the replay's
way over, the joiner's rebuild and catch-up, both picks) and prints p50/p90 per phase, the replay's
size and whether it went inline or through the store, and fails the run (exit 1) on any broken join:
a replay that didn't reproduce the host's game, `orca error mismatch`, `not_seated`, the host's header
never landing, a live frame moved, or a desync. `--max-p50`/`--max-p90`/`--min-ok` gate the total.

```bash
node Tools/orca/match-latency-session.mjs <YouGame checkout> <sess> cy di   # roster cookies, mode 600
ML_BIN=<Orca> ML_BUILD=<version>:<commit> ML_DISC=<Brawl> ML_PPLUS=<dol> ML_SESS=<sess> \
  Tools/orca/match-latency-run.sh perf cb10 casual brawl 10 [--gap 90]
node Tools/orca/match-latency-report.mjs [--detail] match-latency-runs/cb10
```

- `match-latency-run.sh <check|perf> <tag> <casual|ranked|friends> <brawl|pplus> <n>`: both Orcas on
  this Mac, through `run-lock.py` (`perf` for timing, on AC power), SDL HIDAPI off, and the test mark
  `ORCA_TEST_NET_DELAY_MS=0`, so test Orcas only meet each other.
- `match-latency-xplat.sh`: one Orca on the Mac and one on a PC over SSH (`match-latency-pc.ps1`
  there); `MACLAG=<s>` makes the PC host.
- `--gap <s>` idles between matches so the rooms Worker's isolates go cold, as between real
  players' matches; `--video Metal --shots 100` adds a screenshot request every 100 ms, and
  `match-latency-shots.py <run>` turns them into the frozen-picture gaps and contact sheets.
- Both Mac sides share one machine and one network (a direct link of ~0 ms); a PC on the same LAN
  adds a slower machine, not a longer path.

### Release checklist

Every release candidate passes, for Brawl and for Project+:

0. **Nobody playing** on a shared Mac: before each step that starts Orca, `ps -axo comm=` lists
   neither `/Applications/YouGame.app/Contents/Resources/runtimes/orca/Orca.app/Contents/MacOS/Orca`
   nor `~/Library/Application Support/yougame-desktop/orca/apps/<version>/Orca.app/Contents/MacOS/Orca`
   (whole paths: a packaged test copy's own bundled Orca isn't a player's). Test Orcas beside a game
   in play make it stutter; wait until it closes.

1. **Build** `dolphin-emu-nogui` and `tests` in Release: no new warnings.
2. **Unit tests:** `Orca*`, none failing.
3. **Hashlogs** of the default inputs (`bf-mario-link-results.txt` to frame 10800,
   `pplus-1v1.txt` to 9000) equal to the live release's, frame by frame. A release that changes
   emulation on purpose says which frames move and why.
4. **Sync tests** (`YG_SYNCTEST=7`): 0 RAM mismatches over the whole casual flows.
5. **Two-Orca checks** on one machine: `queue-e2e.mjs --queue casual --q2` (and `--scenario
   rematch`) and `online-menu-dropin.py`. Each must pass with every session's checksums matched
   (both Orcas print `K checksums matched` with K > 0), no `Desync at frame`, no `orca error`. A
   sync test runs one game against itself, so only this shows races between two peers' messages.
6. **Mac and PC:** `xplat.py` with one Orca on each. Two Orcas on one machine share a compiler, an
   architecture and a JIT; only this run sees two builds disagree. A release with a Linux build also
   runs `xplat.py` between Linux and the PC: GCC builds that one, MSVC the PC's.

Releases that could affect netcode performance also run a two-Orca performance comparison against
the live build.

## Rollback and snapshots

`Rollback/Rollback.*`, `Rollback/OnlineMatch.cpp`, `Orca/Session/Session.*`.

- **The frame hook.** Each profile names an instruction in the game's main loop that ends a frame.
  The hook runs there, at every frame boundary, on first runs and re-runs: it saves or loads
  snapshots, sets the pads for the next frame, and runs Orca's game-memory logic (patches, rules,
  readers). Everything it writes to emulated memory is a pure function of memory, the frame number
  and the session's synced inputs, and is idempotent at its boundary, so a rollback that runs the
  hook again at the same boundary writes nothing new. Anything that depends on one machine (the
  overlay, stdout lines) happens on first runs only.
- **Snapshots** hold device state and MEM1/MEM2, copy-on-write (`Rollback/Cow.h`). The session NAND
  is kept as a journal mark, not bytes. A load restores only the 4 KB pages that changed, and drops
  the JIT blocks in those pages; the rest of the JIT is kept across loads. The EFB is left alone:
  re-run frames draw nothing.
- **Window:** 7 frames of rollback, 3 frames of input delay by default, fixed
  ([Input delay](#input-delay)). Peers exchange a checksum every 60 frames; a mismatch is a desync,
  which unplugs the friend rather than letting the games drift.
- **Ports.** Online, SI is forced to four GameCube pads, and a port without a player reports no
  controller (bit `0x80` of byte 0, `UNPLUGGED_PAD`). Pads travel as 8 bytes (`PadCodec.h`).
- **Inputs read where the game reads them.** `Rollback::InputOverride` supplies each poll; the
  [SI relatch](#input-latency) makes the game read the pads the hook just set.

## Determinism across hosts

Mac and PC players share a session, so everything that reaches emulated memory must compute the same
on Apple clang/ARM64 and MSVC/x86-64.

**Orca's own arithmetic is integer.** What Orca's C++ works out from emulated memory on every
machine (input-gate masks, steered sticks, whatever a hook writes) is integer math. Apple clang
contracts `a * b + c` into one fused multiply-add on ARM64 by default, which rounds once where MSVC
rounds twice: a float `sqrt(dx * dx + dy * dy)` in the queue's stick steering gave a Mac and a PC
sticks a unit apart for 12 of 20 million inputs. The steering is now fixed point
(`Queue::ToFixed`, an exact `IntSqrt`), pinned by `OrcaQueue.SteeringIsTheSameOnEveryHost`. `Orca/`
and `Rollback/` also build with `-ffp-contract=off`; MSVC's `/fp:precise` never contracts. Floats
that remain on shared values are only compares, truncations of in-range values, bit copies and
constant writes, none of which round differently.

**The frame hook runs in the host's floating-point mode** (`HostFloatScope`). The CPU thread
otherwise runs in the guest's FP mode; under ARM's FPCR.AH, FNEG and FABS leave a NaN's sign alone,
and clang builds some constants with MOVI + FNEG, which turned a `{0, INT_MAX}` into `{0, -1}`. The
hook switches to the host default on entry and restores the guest's mode from FPSCR on exit.

**Floating point on every host.** Brawl and Project+ run in PowerPC's non-IEEE mode (FPSCR.NI).
Three hosts each differ from PowerPC there:

| | x86-64 (Jit64) | ARM with FEAT_AFP (Apple M4+) | ARM without (Apple M1-M3) |
|---|---|---|---|
| Mode Orca sets | MXCSR FTZ | FPCR FZ, AH | FPCR FZ |
| NaN made from no NaN (inf * 0) | negative | negative | positive (PowerPC: positive) |
| Denormal inputs | kept (as PowerPC) | kept | flushed to zero |
| Denormal outputs | flushed after rounding | flushed after rounding | flushed before rounding (as PowerPC) |

What a session does about it:

- **PowerPC's NaN rules** (accurate NaNs forced): input NaNs picked in PowerPC's order and quieted,
  a new NaN is `0x7FF8000000000000`, a NaN result is never negated. Both JITs' accurate-NaN paths
  now cover every arithmetic op, and fctiw, frsqrte and psq_st of a NaN match PowerPC.
- **Denormal inputs kept** on CPUs that flush them: a single that may be a denormal never goes into
  a single-precision op; the op runs in double precision, as Jit64 computes everything.
- **Paired loads and stores** quiet SNaNs as Jit64 does, and fall back to the interpreter exactly
  where Jit64 does (Project+'s codes use `rA = 0` forms).
- **The interpreter under the host's mode** uses integer sign operations, so fabs, fres and fctiw
  are right under FPCR.AH.
- FEAT_AFP is detected on macOS, and FPCR.AH is set only where the CPU has it.

These fixes also apply outside a session wherever Dolphin's accurate-NaN setting is on. Forcing
accurate NaNs means an input recording from a build without it may not reproduce a run that makes a
NaN.

Remaining differences are detected (the checksum exchange reports a desync), not silent, and none
has been seen: double denormal inputs on M1-M3 (none in about 80,000 scripted frames), tininess
before rounding on M1-M3 (a 2^-25-wide window below FLT_MIN), and fctiw of a value rounding to -0
in an Rc = 1 form. Cost: about 3-5% slower emulation, 10-11% on an M1-M3 path (measured on an M4
with `ORCA_TEST_NO_AFP=1`).

Considered and not done: FZ without AH on every ARM CPU, which would remove the M1-M3 residuals
between Macs at about 5% speed on an M4, but would make Mac-to-PC parity depend on a matching x86
change.

## Drop-in

The host plays at once. An invited friend joins as if they had plugged a controller into the next
port. Since the input-replay protocol, a join rebuilds the host's game from the local origin and its
controller history. Longer histories take longer to rebuild, so since 0.3.34 a match's history
starts with the match: before a queue match, and before a friend joins a host that has played on its
own for a while, the host goes back to its origin (a [fresh start](#fresh-starts)), and the join
replays only the match's own frames. No peer supplies Dolphin device state, RAM images, NAND files,
pointers, or arbitrary memory writes.

`Rollback/OnlineMatch.cpp`, `Orca/Session/Session.*`, `Orca/Session/Replay.h`,
`Orca/Session/Keyframe.*`.

### How it works

1. **Local origin.** Every Orca boots the same way up to its origin: port 1 plugged in with
   nothing pressed, ports 2-4 unplugged, whatever its seat or controller, and no port values. The
   origin is the first boundary at which the main menu has been built for 30 frames
   (`OnlineMenu.h` `FreshMove`, `OnlineMatch.h` `DecideOrigin`), captured before that boundary's
   hook: the process's own machine, temporary NAND and RAM checksum. These trusted local snapshots
   support replay, fresh starts and returning to the player's own game. Nobody can act during the
   boot anyway; the main menu's first half second ignores input. A game whose main menu Orca doesn't
   read takes its origin at its first boundary, with no fresh starts.
2. **Recording.** The host retains the final controller pads and bounded port metadata for each
   frame. Rollbacks replace predicted inputs with the winning inputs. Typed rules-header and
   clear-ready events run through the same trusted UI handlers at their original phase. The first
   frame also carries the history's seed ([Random numbers](#random-numbers)).
3. **Offer.** A background thread compresses and encrypts a replay through boundary K. The
   canonical id is `replay-<frame>-<first eight hash digits>`. AES-GCM and the transport checksum
   detect corruption; the decoder independently validates every peer-supplied field.
4. **Rebuild.** The friend restores only its own origin and re-executes frames [M, K) from it (M:
   the origin's frame, which the replay names), unthrottled and unrendered. Another origin frame or
   RAM checksum refuses the join (`room_mismatch`). A launch joiner first runs its own boot to its
   origin, unseen and unthrottled, taking the host's offer meanwhile. At K it applies the recorded
   boundary hook and checks its complete MEM1/MEM2 checksum against the host. A mismatch or
   cancellation returns to its own local game. Historical result and timeout notifications are
   rebased without reporting old events.
5. **Catch up and plug in.** The normal rollback session starts at K, catches up from live history,
   and both machines plug in the friend's controller at the same agreed future frame. Heartbeats
   keep the pending seat alive while the replay runs. The host's packets reach the friend from the
   moment it is in the room, before the host takes it in; a roster from then that names no live
   frame for the friend's seat, or one before K, is not about this stay and the friend's session
   skips that seat (`Session::ApplyRoster`). It matters when a player comes back to the seat it had: until the host
   takes it in again, the host's roster still names that seat's earlier plug-in.
6. **Leave.** The existing agreed unplug and solo-return behavior continues. A failed replay does
   not leave the player's machine on partially reconstructed peer history.

Replays permit frames up to 216,000 (one hour at 60 fps), 64 MiB decompressed data, and a separate
64 MiB decoded-allocation budget. A replay carries its first frame and only the frames from it.
Port lists have at most four unique seats, names at most 128 bytes, controls at most 64 bytes, and
queue labels at most 32 bytes. Rebuilding has a three-minute deadline.
An over-limit history refuses new joins; existing players keep playing. The `orca2:` compatibility
prefix isolates the replay protocol from old `orca1:` clients and legacy machine-image payloads.

A host takes one friend at a time: a friend who arrives while another is in the game, rebuilding
or playing, is dropped with `room_full` (`HostEvents`). A rebuilding friend acknowledges nothing past
its K, which holds what the host sends everyone (`Session::SendPacket`), so a friend already playing
would stall for the whole rebuild and both would be dropped (`stalled`). A rebuild that ends because
the room ended reports the room's own code, and one whose host left reports `peer_left`.

Only two-player joins have been tested. A Brawl loopback join at frame 2,039 rebuilt the same RAM,
joined live play in about nine seconds, and then matched 24 live checksums on the test machine.

### Fresh starts

Without them a join rebuilds everything the host played since its Orca booted: about 600 frames a
second on an M4 Pro, so 25 minutes of play took close to three minutes to join, and slower machines
ran out of time after 10-12. A fresh start (`OnlineMatch.cpp` `FreshStart`) puts the host back at
its own origin and leaves the main menu for the match's character select with a recorded exit, the
way the player's own press would (`OnlineMenu.h` `FreshMove`). The exit is a typed event on the
replay's first frame (`menu_exit`: 25 With Friends, 30 Casual, 31 Ranked), so a joiner replays the
same move from its own origin. So is a new seed for the game's random numbers
([Random numbers](#random-numbers)). Frame numbers start again at the origin's.

- **When.** A queue room's host always fresh-starts, once per room, at the first boundary after the
  room's welcome named the queue (`host` arms it). A friend arriving at a host with no session
  fresh-starts it when the host's history since its last start is over 10 s
  (`FRESH_FRIENDS_AFTER`); a shorter one, a host that just booted or just fresh-started, joins in
  place. A host in a single-player mode holds the friend first, as before; anywhere else (the
  menus, a select, a Versus fight against CPUs, results) the fresh start is immediate. The decision
  is `DecideFreshStart`, a pure function. Never in a session.
- **Where to.** A queue room: its Casual or Ranked select. A friend: the Casual or Ranked select the
  host is on, so Play again re-arms there once the friend leaves; otherwise With Friends.
- **Declared origins.** These packs restore the declared start point without a menu exit or seed.
  Friends join in place through three minutes of emulated play; after that the solo host resets
  before the join. This bounds the replay while avoiding frequent restarts in games without a
  menu shortcut. The threshold uses emulated seconds, so it works at any frame rate and does not
  count paused time. A queue host resets once per room and sees a restart toast too.
- **The tail.** From the origin until the character select has been up 30 frames (600 at most),
  the host runs unthrottled, unseen and muted, like a rebuild of the same frames: the hook re-runs
  and plays the replay's typed events, and nothing is announced. Then the game shows as usual; the
  readers start over on the select.
- **A queue room's host** then plays nothing until the room's header is in, and its own pick goes
  back on the room's select by its own inputs (`Queue.h` `StepOwnPick`): the hand to where its token
  was placed on its own select, A once still over that character, B if it landed on another, X
  until the costume matches; at most 10 s, and the player's controller takes over once a session
  starts. These are ordinary inputs, recorded and sent as such. The opponent's pick is steered in as
  before.
- **Unseen hold.** That hold and steer run unseen and unthrottled too, like the tail (`FreshUnseen`,
  at most `FRESH_UNSEEN_LIMIT` = 120 frames past the tail, then the rest shows as before): the
  host's picture comes back with its pick already in, about a second sooner on a Mac, and its
  keyframe follows at the next boundary. Unlike the tail these are first runs: the hook writes and
  records the room's header and the steer's inputs as always (`SoloQuiet` lets the header in while
  the port catches up only here, `HeaderFreeWhileCatchingUp`), so the joiner replays exactly what it
  did before, only sooner. Its boundaries don't count toward the header's limit until the header
  is in (`HeaderWaitAt`), so a queue room never fails sooner in real time, and no stats line counts
  them. A friend's fresh start has no hold.
- **What it costs.** The host's picture holds for the restore and the tail (a few hundred frames,
  unthrottled). Its history restarts, so a join replays only the tail, the header and the seconds
  since: how long either player played before no longer matters. After a queue room the player's own
  game comes back as before (the queue image).
- **What it resets.** A friends fresh start resets what the host set up alone: Versus rules, items,
  the stage list, Project+'s Code Menu, a fight against CPUs in progress, tags made in the game. Each
  player's YouGame tag and controls carry over as port values.
- **NAND.** A restore rewrites only the files that differ and removes the rest
  (`ReplaceNandTree`), so a fresh start doesn't write Brawl's 39 MB of boot files again.

Test knobs (`TestKnobsAllowed`): `ORCA_TEST_FRESH=off` (no fresh starts: every join replays from the
origin in place), `ORCA_TEST_FRESH_AFTER=<frames>` (the friends threshold),
`ORCA_TEST_FAST_UNTIL=<frame>` (solo frames before it run unthrottled, to age a host quickly, until
its first fresh start or session).

### Random numbers

Brawl and Project+ draw from two generators (`mtRand`, a 31-bit LCG: `seed * 0x41C64E6D + 12345`):
`g_mtRandDefault` (`0x805A00B8`, everything a fight draws) and the menus' own (`0x805A0420`). The
character select draws each panel's RANDOM from the menus' generator as it starts (three draws per
panel, `0x806857F0`; Project+ maps the first through its 42-entry table at `0x80680E80`), and A on
RANDOM takes that draw. In the menus nothing else draws from the menus' generator, and
`g_mtRandDefault` moves once per scene change; how long anyone stays on a screen changes neither, and
nothing reseeds them from the clock after boot (probe watches, 2026-10-09).

Orca pins the clock and boots the same way every time, so both generators start every boot, and
every fresh start's origin, from the same numbers. Through 0.3.35 that made RANDOM the same
character in every match: since 0.3.34's fresh starts, game 1 of every Project+ queue match gave
the host R.O.B. and the joiner King Dedede, and game 2 one of a few characters set by game 1's
length (the menus' generator moves once every ~5 s of a fight). Before 0.3.34, the same for a given
player's own image. A fight also began from the same `g_mtRandDefault` state every match.

So every history starts from a seed of its own: a typed event on the replay's first frame
(`ReplayFrame::seed`, with `menu_exit` on a fresh start), drawn by the host from the system's random
source for every boot origin and every fresh start (`OnlineMatch.cpp` `NewHistorySeed`). The frame
hook writes both generators' seed words from it at that frame (`OnlineMenu.h`
`FreshMove::ApplySeed`), after the origin is captured, so every Orca's origin stays the canonical
one, and a joiner's rebuild plays the same event from its own origin. The origin's checksum and the
rebuild's checksum at K still refuse any difference. A seed rides only on a replay's first frame;
the decoder refuses it anywhere else. With no YouGame room there is no history and nothing is
written, so harness runs and hash logs repeat; in a dev game's room `ORCA_TEST_RNG_SEED=<n>` (a
test knob) fixes the seed.

### Port values: each player's name and controls

A joiner reconstructs the host's input history, so its own name tags are not there.
Each port carries its player's values instead (`Orca/UX/NameTags.h`): the YouGame username as the
tag's name, the tag's rumble byte and its whole button layout. The frame hook writes them into the
port's name tag on the character select, and the game copies the layout into its per-port table
(`0x805B7480`) as a match starts, so each port plays with its player's own buttons.

The host decides each port's values and the frame they apply from; every machine holds them before
it runs that frame (`Session::RequireValues`). Values from another machine reach the tag only as the
game's own menus could have set them, at their exact size (64 bytes at most on the wire).

### Each player's own controls

A player's own controls are 46 bytes: rumble, then a tag's layout (`NameTags.h`), written as 92 hex.
They come from `ORCA_CONTROLS` at boot, else the file Orca kept for this game
(`<user>/Config/OrcaControls-<game>.txt`), and from the app's `controls <hex>` mid-run. While the
player is alone, a change they make to the tag they wear (the controls menu, Project+'s name list)
becomes their own controls: Orca prints `orca controls <hex>` and keeps the file. Orca's own writes,
a save that isn't theirs (after a resync), a switch to a tag at the game's defaults and re-runs never
count. Controls loaded from the file (no `ORCA_CONTROLS`) are printed the same way once at boot, so
the page shows what Orca plays.

- **Tags made in the game** last only until Orca closes: every run boots an empty session NAND, so a
  player who wants their own tag makes it again. A port with controls that wears a tag holding
  exactly what the game puts in a new one (rumble on, the default layout) gets its controls written
  into it on the character select, in the same frame (`ApplyNameTags`), and the read of the
  player's own controls comes after the writes (`NameTagsFrame`), so a tag made again plays with
  their controls and never replaces them with the defaults. The tags themselves are not kept.
- **A tag the player typed is theirs.** The game types names in full-width characters (`A` is
  `0xFF21`) and Orca's own tags are ASCII, so names match as they read: full-width folded to ASCII,
  one case (`NameTags.h`). A tag the player named like their YouGame username in Options >
  Controls is found as theirs, preferred over Orca's own copy (`FindPlayerTag`), and is the one their
  port wears on the character select and their own controls are read from. Since 0.3.37; before, Orca
  made a second tag of that name at the game's defaults and put the player in it, so controls set in
  the game (tap jump off) never reached an online game.

- **Solo**, the own port's values carry them from the frame they change, so the YouGame tag gets them
  on every character select: Training, the queue's own select, With Friends. A friend who joins
  later gets those entries with the keyframe. A session's host and joiner carry them as before
  (`RefreshOwnValues`, the hello).
- **The name button** takes A on With Friends' selects (the hand's own panel) and on the queue's own
  select while not searching, with the stick centred while A is down; its list takes A and the stick,
  never L (Project+'s hold-L leaves for its controls scene). Start never readies with the list open.
  A queue room's select keeps A in the grid: there each player wears their YouGame tag with their
  own controls.

### Rooms

- Orca asks for 4-seat private rooms with a game-owned lobby. Seats are lobby slots. A hello carries
  the compatibility key and whether the sender is the host. The host must hold slot 0; one that
  finds itself elsewhere opens a fresh room.
- The room's ticket (`mpTicket`, minted by the page) waits up to 30 s for a reply and is asked for
  once more if none came; then the room ends with `network`.
- A joiner that hears no host hello within 8 s of its welcome ends with `peer_left`.
- The server answers a ping every second; 8 s without any message ends the room as a dropped
  connection (close code 4001), so the server holds the seat for 90 s. Not much less than 8 s: a
  keyframe upload can fill a home uplink and delay the pings.
- A host whose room ends without it leaving prints `orca error network <why>` and reopens, backing
  off from 2 s to 30 s, trying the same code first. A socket the server replaced (close code 4000:
  another run on the same account) is never taken back. During the player's pause the host loop
  does the same every 2 s on the CPU thread (`WatchRoomWhilePaused`), so a paused game keeps a room.
- A host that leaves always opens a new room. A player on a port other than 1 after a session (it
  joined someone and went solo) goes home ([Going home](#going-home)).
- A rollback the joiner can't make (an engine fault, not a desync) ends its session as `network`.

### Going home

A player on a port other than 1 after a session (it joined a friend's game, then it left, its host
did, or the session failed) goes home (`OnlineMatch.cpp` `ComeHome`). At its first alone boundary on
the menus (the main menu or a character select; never a fight, a stage select, a results screen, a
scene change or a single-player mode: `OnlineMenu.h` `MenusScene`), with no session, nothing of
drop-in under way and the old room gone, it moves to port 1 and opens a room of its own a second
later, as after a queue room. Solo with no session, that boundary is a first run nothing re-runs,
and only this game changes; the next friend's keyframe carries it.

- Home on the character select a With Anyone pick opened, it announces that pick too ([Casual or
  Ranked with friends in the game](#casual-or-ranked-with-friends-in-the-game)), so its Start
  readies it on the queue's own character select.
- On a character select the move comes a frame after the old host's port unplugged, so port 1's
  panel keeps what the old host put there and the guest's hand now drives it, while its own old
  panel closes and it picks again. Cosmetic: a With Anyone select starts with nobody's token down.
- A player in a fight when its host goes prints `orca state no-room` once, after the old room has
  told the page it is empty (so the page never shows that room again), and goes home once back on
  the menus. The app can still send `join` meanwhile, which wins over going home.

Harness knob: `ORCA_UX_TEST_RESEAT_AT=<frame>`: the player plays port 2 until that frame and port 1
from it, as a guest going home does, for synctests of the move (`Tools/orca/inputs/bf-reseat.txt`).

### Play again after a ranked set

After a set both games are back on their own Ranked character select, each hosting a friends room.
The page's Play again invites the opponent (`prepare-join` to the inviter, `join <code>` to the
invitee). Through 0.3.27 that join failed: the inviter's game still carried the queue select's header,
its keyframe carried it, and the friend refused it (`orca error mismatch`). The same happened to any
invite from a With Anyone character select. Now the newest intent wins
(`OnlineMatch.cpp` `EndQueueForFriend`, `EndQueueForJoin`, `RearmFriendsPick`):

- **The host** (port 1, not joining, its room's welcome said `private`): a friend arriving, or a
  `prepare-join`, while the player is on the queue (its own select, or searching) ends the queue and
  the search and prints `orca menu cancel`. The header goes back to none at the next boundary, a
  keyframe made under the old one is dropped, and the friend's keyframe has none. Never in a matched
  room, nor before a room's welcome.
- **Once the friends are gone** (one came in, all left, and the host is alone again on that pick's
  character select), the pick is armed again if the page can search it (`orca menu online ranked`),
  so Start searches. Never for an invite nobody took yet.
- **The friend** whose keyframe loads into a friends room ends its own queue the same way and drops
  the game it kept for after a queue room, which would otherwise block going home. A join that fails
  before the keyframe loads keeps all of it.

Local: only the host's own solo boundaries and process state change; nothing a session runs on two
machines, so no compatibility change. Tests: `OrcaOnlineMenuLobby.*` (the three play-again tests).

### Keyframe store

`HttpKeyframeStore` in `Orca/Session/Keyframe.cpp`. It holds the host's encrypted replays (the
store and its route keep the keyframe name). Tests use `ORCA_TEST_KEYFRAME_DIR`: the store refuses
dev tickets.

- `https://yougame.co/api/orca/keyframes/<room>/<id>`, `id` = `replay-<frame>-<8 hex of XXH3-64>`
  (frame 0 to 216,000, no leading zeros; `ValidReplayId`). Orca sends and accepts no other id.
  Auth: `Authorization: Ticket <ticket>` with the room's ticket (accepted up to 6 h past expiry; on
  401 `ticket` or 403 `signed_out` Orca mints a fresh one once).
- **PUT** with `Content-Length`, `X-Orca-Frame`, `X-Orca-Hash`. A replay is at most 64 MiB
  before compression, so at most `ZSTD_compressBound(64 MiB)` + 28 = 67,371,036 bytes; Orca's own
  cap on a download is 128 MB. 409 `exists` counts as done. 6 PUTs a minute per account.
- **GET** streams with `Content-Length` (for join progress); Orca checks size and hash, then
  decrypts. 404 `gone` means expired.
- **DELETE** by the joiner after rebuilding, best effort; by the host when replacing or leaving.
- **Inline replays.** A sealed replay of at most `INLINE_KEYFRAME_MAX` (32 KB) never goes to the store:
  it travels in the room's `kf` offer itself (`b`, base64, `KeyframeOfferToJson`), so a queue match
  (a few KB) makes no PUT, GET or DELETE. The joiner refuses an offer whose `b` is longer than
  `INLINE_KEYFRAME_MAX_TEXT`, isn't the canonical base64 of exactly `n` bytes, or comes with `n` over
  the cap, before decoding anything (`KeyframeOfferFromJson`), and checks the bytes exactly as a
  download (`OpenKeyframe`: size, hash, decryption, unpacking). Only the room's host sends a `kf` a
  joiner reads (`drop_in_host`), as before; the relay delivers it only to sockets the room admitted,
  and the room's pause still stops it. A larger replay (a host with a long history since its last
  start) goes through the store as before. `ORCA_TEST_INLINE=off` (a test knob) sends every replay
  through the store.
- Network errors, 429 and 5xx retry 4 times with 0.5/1/2 s backoff; anything else is final. A host
  whose keyframe is refused for good (a refusal with YouGame's code; a bare 4xx from the edge only
  fails that keyframe) makes no more for the friends waiting: it drops them with
  `signed_out` (YouGame's code) or `refused`, prints `orca state friend-left <that>`, and each of
  them ends with `orca error signed_out` or `network`. A joiner whose own download is refused with
  `signed_out` reports that code too.

### Room messages

Relayed `msg` (`Orca/Session/YouGameRoom.cpp`):

- `hello {c, r, h, dl}`: key, heard-you, is-host, offers direct links. Sent until answered.
- `dl`, `dlc`, `dlr`: direct-link signalling, only between two Orcas that both offered links.
- `kf {f, id, n, x, key, nm, nv, b?}`: the keyframe offer, every second until the joiner's first
  packet; `b` is the sealed replay itself when it is small enough (Keyframe store, "Inline replays").
- `drop {r}`: the host unplugged this joiner (`desync`, `stalled`, `network`).
- `p`: session packets. `q` is the sender's packet count; a stale copy still contributes inputs,
  acknowledgements and checksums, never roster or timing. Drop-in adds `ro` (roster), `ha`
  (history ack), `ht`/`hf`/`hp` (history runs) and `lv` (leaving).
- `bye {r}`: why a player leaves, broadcast before the leave.

## Direct links

`Orca/Session/DirectLink.*`. Every session packet also goes straight to each other Orca in the room
over UDP, beside the relay. The relay costs about 15 ms each way (machine, Cloudflare edge, the
room's Durable Object, edge, machine, over TCP); two machines on one LAN see a 40-50 ms round trip
through it where a direct one takes about 1 ms.

- **ICE** is libjuice: host candidates on every interface (IPv4 and IPv6), the public address from
  STUN, and Cloudflare TURN over UDP from the ticket's `ice` credentials. TURN over TCP or TLS isn't
  used: on a network that blocks UDP the room's relay is the path.
- **Signalling** is room messages. The lower connection id offers; the other answers; candidates
  trickle in batches (at most 10 messages a second).
- **Keys.** Each side sends 32 random bytes in its `dl`. HKDF-SHA256 over both, salted with the room
  code, gives one key per direction. Every datagram is ChaCha20-Poly1305 with a counter nonce and a
  64-counter replay window. Only the two Orcas of a pair can make a datagram the other takes; the
  room sees both halves, so the trust is the relay's, no more.
- **Datagrams** are at most 1200 bytes (IPv6's minimum MTU less headers) and carry the packet's
  JSON without drop-in history (the relay alone carries that, in order). Each echoes the peer's
  newest counter, giving a round trip without a shared clock. Keepalive after 200 ms idle. Marked
  DSCP EF (not on Windows).
- **Sending** happens at once on the emulator thread under the link's own lock: 70-100 us on average
  on an M-series Mac.
- **Both paths, always.** The relay still carries every packet; whichever copy arrives first counts,
  so a link that fails costs only the relay's time.
- **Rebuilds.** libjuice calls a pair failed only after 30 s without consent, so Orca rebuilds a
  link when the agent fails, when no answer comes within 5 s or no path within 10 s, when a link
  that was up hears nothing for 3 s while the relay still brings the peer's packets (a network
  switch, an expired NAT mapping), and once, 10 s in, on a TURN or server-reflexive pair between two
  machines that share both a host subnet and a public address (`OneLan`). A shared /24 alone isn't
  one LAN: two homes on the same private range would otherwise tear down a working link. Failure
  rebuilds back off 2 s to 30 s. Each build is a new generation with fresh keys.
- **Flood limit:** at most 300 packets a second per link after authentication (an honest Orca sends
  one a frame); the rest are dropped (their copy still comes through the relay).
- **Switching off:** YouGame can turn links off for everyone with `"direct": false` in the ticket
  reply; the player can with the embed command `direct off`; `ORCA_DIRECT=0` is a test switch.
- **Logs** never carry addresses, candidates or credentials. `orca stats` gains `tx` (0 relay, 1
  direct, 2 TURN) and `lrtt` (the link's median round trip).
- **Firewall.** The room binds a throwaway UDP socket on every interface as soon as it is in the
  room, so a Windows firewall prompt comes in the lobby rather than mid-game. Windows ties the rule
  to the executable's path, so each new version folder asks again. Orca.app is signed, so the macOS
  firewall lets it be.
- **Privacy.** ICE shows each player's LAN and public addresses to the other players in the room.
  A TURN-only option could hide them later.

**TURN only when needed.** A link starts with host and STUN candidates only. A link that never came
up marks the pair `with_turn`, and the next link to that peer gathers TURN too. A TURN allocation
beside a working direct path made one Wi-Fi card (Realtek RTL8822BE) drop off the air for 100-400 ms
every 5 s, stalling the other player each time (42-57 stalls in 160 s against 4 without TURN). Most
pairs connect host-to-host or through STUN anyway.

**Measured** (two Orcas, one machine, prod rooms): direct links gave 0 rollbacks and 0 stalls over a
two-minute match where the relay alone gave 7 and 65 rollbacks and 4 stalls. With 20% datagram loss
the link still carried the match; forced TURN gave a 23-24 ms round trip.

**Wi-Fi.** A machine whose radio leaves the air loses everything sent to or from it for that long.
Rollback hides up to the input delay plus `max_rollback` frames of it (10 frames, about 167 ms, at
the default 3 + 7); past that the
other side stalls. No build fixes a radio's dead time. The fix is the player's: a cable on the
machine whose radio drops out, or Bluetooth off, or the adapter's power saving off, or another
channel. To find whose radio it is, run a 10 Hz router ping on each machine during a match and line
the stall seconds (`stats.jsonl` from `xplat.py`) up against each ping's spikes.

Not yet: playing on through a room disconnect while direct links are up; using the link's round trip
in the delay rule (it uses the relay's); a binary packet codec.

Tests: `OrcaDirectLink*`, `OrcaRoom.TicketSwitchesDirectLinks`, `OrcaSessionDirect*`,
`OrcaLive.DirectLinkBetweenTwoRooms`, `Tools/orca/direct-link.py`.

## Input delay

Fixed, never adaptive: players want a delay that never moves with the link. The page sends `delay N`
(1-6; YouGame's `ORCA_DELAY_DEFAULT`, 3 since Orca 0.3.32's v-sync move); with no page, no `caps
delay` or an old page's `delay auto`, Orca plays at its own fixed 3 (`Rollback/OnlineMatch.cpp`
`DEFAULT_DELAY`, Orca 0.3.33+; before, the adaptive delay below from 2). Rollback hides the rest of
the link, jitter and spikes included: a spike past the rollback window costs a stall there, not a
frame of delay on every input all match long. Each player's delay is their own: a 2 and a 3 play
together.

The session's adaptive delay (`Orca/Session/Session.cpp`, "Input delay"; `Config::input_delay` 2 is
its floor) is kept but no online match uses it. It rises only for what rollback can't cover, and only
where a little delay cures it:

- **The typical link.** The median of this player's latest 15 round trips to the relay (one a
  second) plus the slowest friend's median, halved for one way. The delay covers what the 7-frame
  window leaves of that, keeping a frame for the boundary and one for jitter: two frames cover a
  round trip of about 240 ms between the players. A 4 ms hysteresis around each frame boundary keeps
  it from flipping.
- **Stalls a little more delay would have spared.** A friend stalls when this player's inputs outran
  its rollback window, so only this player's delay can help. Each side times its own stalls and
  reports how many were over within one, two, three and four frames (`"sp"`). A raise of k frames is
  worth it once it would have spared stalls in 8 seconds per frame of the last 45: a frame on every
  input, against a stall saved every five or six seconds.
  - Lateness a frame or two cures (a slow, bursty uplink; dropouts just past the window every few
    seconds) raises the delay within half a minute or so.
  - Random spikes that outlast the window by anything up to six frames raise nothing: a frame more
    would spare only the shortest. They cost their stalls, as they would in Slippi.
  - Stalls don't count around a hitch on either machine (a frame that took this machine over a
    frame period more than the game itself took) or a frame a game load slowed here, when this
    player's own stall set them off, while a controller is catching up, or in the first 10 s after
    a plug-in.
- Rollbacks and time-sync waits never move the delay.
- **Coming down.** A link-raised delay follows the link down after 3 s. A stall-raised one steps
  down after 5 quiet seconds, then a frame each further quiet second while each packet's reach (how
  far the friend ran past this player's inputs, plus the delay) shows two frames of room. Stalls
  that come back within a minute undo the step and double the next wait (up to 40 s).
- Every change is logged with its reason, e.g. `input delay 2 -> 3 at frame 5400: 1 frame more
  would have spared the others' stalls on this player's inputs in 8 of the last 45 s`.

Why this rule: an earlier rule raised both players on any burst of re-run frames or two stalls in
three seconds, and needed ten quiet seconds to come down, which a match never has. A Wi-Fi link with
spikes every ~11 s sat at delay 4-5 all match with barely fewer rollbacks than at 2. Counting
stalled seconds can't tell a bursty uplink from random spikes (both stall in 5-11 of every 30 s);
how long each stall lasts can. `SessionTest` covers each kind of link, simulated over 16 draws.

`ping` in `orca stats` is this machine's own round trip to the relay, not between the players; one
way between them is about half the sum of the two. `pmed` and `fpmed` are the medians the rule uses.

## Input latency

Between a press and the frame that shows it, and what Orca does about each part. Online adds the
input delay on top (3 frames of 16.68 ms).

| Change | Where | Changes emulation | What it does |
|---|---|---|---|
| Immediate XFB | `Orca/Profile.cpp` | no | Presents each frame at its XFB copy instead of 18 ms later at the VI field that scans it out. |
| VSync off | `Orca/Profile.cpp` | no | Forced: VSync only adds 2-19 ms in a composited window and could let the screen pace the game. |
| Steady presentation | `Source/Core/VideoCommon/PresentPacing.h` | no | Shows each frame at a steady offset from its VI time that covers the usual rollback re-run, so re-runs don't move the picture. |
| No re-present on rollback loads | `Source/Core/VideoCommon/Present.cpp` | no | A rollback no longer flashes the snapshot's stale frame. |
| Slow screens drop frames | `Source/Core/VideoBackends/Metal/MTLGfx.mm` | no | A screen under 59 Hz no longer slows the game to its rate. |
| Thread priority | `Orca/ThreadPriority.cpp` | no | The CPU, room and controller threads run at interactive priority. |
| Brawl's input lag fix | `Data/Sys/Orca/RSBE01.patches` | yes (Brawl) | Brawl copies this frame's pads, not last frame's. |
| SI relatch | `HW/SI/SI.cpp`, `Rollback/Rollback.cpp` | yes | The game reads the pads the frame hook just set, not the previous hook's. |
| V-sync move | `Data/Sys/Orca/RSBE01.patches`, `PPLUS32.patches` | yes | Both games draw and copy each frame right after its logic instead of after a retrace wait: 13 ms sooner. |

The three that change emulation change the compatibility key, so builds with and without them never
meet.

**Result** (median of 82 jumps, 120 Hz screen, solo): Brawl's press-to-glass went from 104 ms to
53 ms, Project+'s from 87 ms to 53 ms, with the image at press+2 frames in both; the v-sync move
then took 13 ms more off both (the sampling boundary to the present call: 48.8 to 35.6 ms in Brawl,
48.8 to 35.2 ms in Project+). Online, with frequent 1-frame rollbacks (on 6-17% of the frames), it
keeps 6-7 ms of that; with 4-7 frame rollbacks it keeps none, and the picture is as steady as
before.

### Presentation (host only)

- **Immediate XFB.** The VI field that would scan a copy out starts up to a frame after it. It can't
  desync: re-run frames skip `ImmediateSwap`, the throttle it bypasses only sleeps, and per-frame
  RAM hashes are identical with it on and off. It also shows the corrected frame after a rollback at
  once.
- **Steady presentation** (`PresentPacer`, under Smooth Early Presentation, forced on). With the
  v-sync move a frame's copy lands about 2 ms after the frame boundary, 14 ms before the VI field
  after it. Each frame is due at the throttle's host time for that field, computed at the copy (the
  estimate taken at the previous field is stale after a re-run, whose fields run unthrottled, or a
  stall's pacing reset), and is shown at due plus an offset: all but the latest 4 of the last 180
  arrivals, moving at most 0.5 ms a frame up and 0.2 ms down, never past due. Without rollbacks the
  offset sits just above the copies. When re-runs come often it grows to cover them, so a re-run
  doesn't move the picture; a rare one is late on its own frame only. Dolphin's own rule moved half
  the way to any late frame at once, so with the early copy every re-run dragged the presents after
  it. Loads and hitches raise the offset the same way: a match's first seconds are presented as
  before the move, until 3 s after the last slow frame.
- **Precise waits** (`Common/Timer.cpp`, macOS). A present now waits up to 14 ms, and macOS woke such
  sleeps 1-3 ms late, which no spin takes back; long waits sleep in 1 ms steps. Precision frame
  timing is forced on.
- **No re-present on rollback loads.** Rollback snapshots skip `Presenter::DoState`'s redisplay;
  savestates and keyframes still show their frame.
- **Rollback and the vertex buffer.** Before the v-sync move, the GPU was mid-frame at the frame
  boundary (Brawl and Project+ 170-350 draws past their last XFB copy), so the frame shown after a
  rollback mixed the old timeline's draws and the new one's. But a batch still buffered in the vertex manager
  was flushed after the snapshot's XF and TEV state had been restored, so it was drawn through the
  wrong matrices (one-frame triangle spikes from a character as hits land). Loads now flush it first.
- **Slow screens** (Metal). On a 50 Hz screen `nextDrawable` held the game at 50 fps, dragging the
  friend down too. Drawables now come from a background acquire and a frame without one isn't shown,
  while the screen runs under 59 Hz or `nextDrawable` keeps waiting.
- **Thread priority.** `QOS_CLASS_USER_INTERACTIVE` on macOS; on Windows a raised priority with power
  throttling off. Only in a session. It cut the worst frame under heavy load (98-106 ms against
  121-158 ms) but showed no consistent p99 gain on a fast Mac.

### Audio (host only)

The buffer stays at Dolphin's 80 ms. The game hands the mixer a frame's audio in one burst and a
rollback's re-run holds the next burst back by up to ~15 ms. At 80 ms a minute of match with
rollbacks had 0-1 underflows; at 40 ms 5-32, at 32 ms 174. Sound trails the picture by about
50-65 ms.

### Emulation

- **Brawl's Controller Input Lag Fix** (the community code by Magus, also in Dolphin's
  `GameSettings/RSBE01.ini` netplay set and in Project+'s codeset). Brawl's pad code at `0x8002AD88`
  copies each pad's status from a frame-old array; the fix reads the current one. Orca writes it as
  one guarded group of 45 words. Project+ already runs it.
- **SI relatch.** The game reads its pads from the SI input buffers, which the SI's last poll filled
  just before the frame hook, so every frame played the previous hook's pads: "delay 2" acted like 3.
  At the end of the frame hook, `SerialInterfaceManager::RelatchInputs` writes each channel's input
  buffer from the pads just set, leaving the status register alone. It is deterministic: a pure
  function of the session's pads, applied after the boundary's snapshot was saved or loaded, so every
  load relatches.
- **V-sync move** (Project+'s "Move v-sync call for Brawl/PM" by hannesmann, Dan Salvato and
  Achilles, in its netplay codeset). Brawl's frame function (`0x80023AE4`) waits for a retrace
  before it draws, looping back to `VIWaitForRetrace` (`0x801E892C`) from the `beq` at `0x80023B88`
  while no XFB slot is free. The move makes that `beq` a `nop` and its `blr` at `0x80024028` a tail
  call to `VIWaitForRetrace`, so each frame is drawn and copied right after the game's logic (the
  copy 2.1 frames after the sampling boundary instead of 2.9) and waits once at the end. Both
  games write the same two words at the first frame boundary, each in a guarded group of three
  (`RSBE01.patches`, `PPLUS32.patches`; Project+'s codes then write the same words every frame, see
  [Code the JIT doesn't see](#code-the-jit-doesnt-see)). Every frame's RAM differs from earlier
  builds from frame 0 on; the scripts still reach the same scenes, a few frames sooner. A frame
  that runs over a field now waits for the next retrace, so the game's slow load frames hold whole
  fields; a 2-field hold (33.37 ms) counts as a slow frame in a game load, where a 1.5-field one
  didn't.

Measure with a latency-probe build: a scripted press, frame dumps to find the first changed image,
and timestamps from the sampling boundary to the present call and to CoreAnimation's
`presentedTime`. Presentation must be measured in a `-p macos` window (headless Metal presents
nothing), with a warm shader cache.

## Music switch

`music on|off` on stdin (cap `music`), or `ORCA_MUSIC=off` for a harness run: the player's own
Music switch. Off leaves the game's music out of what this machine plays and changes nothing the
game sees, so two players with different switches stay in sync. It is not in the compatibility key.
Offered for Brawl (any revision) and Project+; without the cap the line answers `unsupported music`.

`Orca/Music.*`, `HW/DSPHLE/UCodes/AXWii.cpp`, `HW/DSP.cpp`:

- **Music is the stream voices.** Brawl and Project+ play their music (BRSTM) on AX voices of type
  stream (`AXPBWii::is_stream` 1): a looping stereo pair over a ring buffer in MEM2, one voice on
  main left, one on main right, no AUX sends. Sound effects are normal voices (type 0), looping ones
  too. Seen on every voice of the default Brawl and Project+ runs (menus, character and stage
  select, a match).
- **Through a song's loop.** At each loop the stream player makes the pair normal voices for 0.5-5
  s (loop start and end moved inside the ring buffer, so the DSP takes the loop's ADPCM context at
  the jump), then stream voices again. A voice that played as a stream stays music while it plays
  inside the ring buffer it had then (`Orca::Music::StreamVoices`); an effect that reuses its
  parameter block plays from elsewhere and is an effect. Before this (0.3.30-0.3.35) the music came
  back at full volume for those seconds at every loop.
- **RAM gets the same mix.** AX HLE mixes every voice as before and writes the frame (96 samples,
  3 ms) to RAM as before. With the switch off it also keeps what the stream voices added to the
  main bus (a voice's share is additive, `MixAdd`), gives that the compressor's gain too, and mixes
  the frame again without it: same volume ramp, same clamp.
- **Only the speakers get the second mix.** The AI DMA reads the frame from RAM for the host mixer.
  While RAM still holds the bytes the mixer wrote there (Brawl: `0x804E84C0` and `0x804E8640`,
  read by the DMA as `0x004E84C0`), it plays the quiet copy instead; anything else (a rollback
  load, a game write) plays RAM. The last 32 frames are kept.
- **Not RAM.** The game's own volume or options would change memory every peer hashes, and a
  session starts from an empty NAND where a joiner plays on the host's save.

**Checked** (2026-10-06, Mac):

- Hashlogs of the default inputs (Brawl to 10800, Project+ to 9000) with the music on, with it off,
  and of 0.3.27: equal on every frame.
- Sync tests (`YG_SYNCTEST=7`) with it off: 0 RAM mismatches (Brawl 73451 re-run frames, Project+
  60851).
- What Orca played (`Dolphin.DSP.DumpAudio`, the same run on and off, sample-aligned): where only
  music plays (character select after the match) RMS 2000-4000 drops to 0.7, the floor of a silent
  second; in the matches the effects stay (off/on RMS median 0.77 Brawl, 0.84 Project+); with no
  music playing the two dumps are identical. 99.99% of AI blocks went out quiet.
- Two Orcas, one with the music off (`queue-e2e.mjs --q2 --queue casual --music-off ada
  --dump-audio`): Brawl passed with 102 checksums matched on each side, Project+ with 103, no
  desync; the music-off side's dump was silent for 71 of 157 s (Brawl) against 25 for the other.
- Again on the 0.3.29 candidate: hashlogs on = off (Brawl Rev 2 frames 0-10807, Project+ 0-9003),
  Rev 1 off = Rev 2 on (0-10819); a sync test off, 0 RAM mismatches; two Orcas, Project+, one off:
  104 checksums each. A voice probe of the Brawl run with it off: the results screen's music is the
  stream pair too (the announcer and the crowd are normal voices), and no stream voice of the run
  sends to an AUX bus. Cost: instructions retired by the whole run to frame 9000 (a snapshot every
  frame from 5900) are the same on and off, within the 0.1% between runs.

Through loops (2026-10-09, Mac, branch `music-loop-0335`): idle on the main menu with it off, the
menu song loops every 91 s and 0.3.35's dump has the music back for 3.8, 5.2 and 0.7 s (Brawl) and
3.8, 5.2, 0.7 and 2.1 s (Project+), exactly while a RAM watch shows the pair as type 0; with the fix
the dump is silent the whole 6-7 minutes. Over the default inputs it is off the same as 0.3.35
except at a loop (Project+ 136 s: 0.3.35 played the music, the fix doesn't; Brawl 131 s, in the
match: the effects only), on it is sample-identical, and hashlogs (fix off, 0.3.35 on) are equal on
every frame (Brawl 0-10813, Project+ 0-9001). Sync tests off: 0 RAM mismatches (Brawl 29351 re-run
frames, Project+ 19901).

### The game's Sound slider

Options > Sound's balance between music and effects (gmGlobalRecord, `0x9017BE68`, 0-100) is game
memory, so in a room it was the host's: a host who had pushed it to the end labelled MUSIC silenced
the joiner's music, and to the other end their effects. Since Orca 0.3.32 it stays at 50, where
`GameGlobal::init` puts it, in every session, solo included: both limits of the page's slider
handler are 50 (`RSBE01.patches` and `PPLUS32.patches`, "Options > Sound"), so the cursor never
moves and the sound manager is never called. Solo can't keep it: every Orca is the host a friend
can drop into, and a balance set alone reaches the friend with the keyframe. Music on or off for one
player is the Music switch. Clamping to 10-90 instead is two words per file, named there.

Checked 2026-10-07, Brawl and Project+: two Orcas in a friends room, the host's slider held toward
MUSIC before the friend joined. With 0.3.30 the friend's audio after the join was silent (RMS 1-20 a
second); with the lock it had its music (RMS 2700-6100), every checksum matched. Hashlogs of the
default inputs equal 0.3.30's without the two groups and differ with them only in the three changed
words (RAM compared at the character select, in the match, on the results screen and back on the
character select). `YG_SYNCTEST=7` with the slider held both ways on the Sound page, and over the
default inputs: 0 RAM mismatches. Test: `OrcaOnlineMenuSound.*`.

## Match start

The first seconds of a match (the countdown and just after) used to drop frames: two things compiled
on the one thread that runs the game in a session, the GPU pipelines the match draws first and the
match's PowerPC code. Both now compile before the match is on screen.

### Shaders

Sessions force or allow (`Orca/Profile.cpp`, `Source/Core/VideoCommon/ShaderCache.cpp`):

- **Asynchronous shaders only:** a pipeline never drawn compiles in the background and is drawn with
  an ubershader meanwhile (AsyncUber, the default), or not drawn until it is ready (AsyncSkip, the
  player's choice since 0.3.39: lightest on the graphics card, and a new effect may be missing for a
  moment on that player's screen only). Dolphin's default compiles it on the emulation thread,
  which in a session is also the GPU thread, so a session refuses it ([Graphics](#graphics)).
- **Compile before starting:** the ubershaders and every pipeline in `Cache/<game>.uidcache` compile
  on all cores before the first frame, the UID cache first (it is what the match draws and is far
  quicker than the ubershaders).
- **EFB copy pipelines before the first frame:** no Dolphin cache or ubershader covers them, so Orca
  keeps their UIDs in `Cache/<game>.efbcopy.uidcache`.
- **A limit on the wait:** 60 s, or the app's `ORCA_SHADER_WAIT_S`. While it waits Orca prints
  `orca shaders <done> <total>` once a second, so the app can tell a slow boot from a hung one.
- **No ubershader compiled on the emulation thread:** a draw whose ubershader is itself still
  compiling (a boot that hit its limit) is skipped until something is ready, instead of a
  multi-second freeze.
- **FXC output kept across builds** (`Source/Core/VideoBackends/D3DCommon/FxcCache.*`, D3D11 and
  D3D12). Dolphin's compiled shader files are emptied by any other build. `Cache/Shaders/D3D-fxc.cache` maps a SHA-1 of
  exactly what FXC is given (compiler DLL hash, entry point, target, flags, macros, HLSL text) to
  the DXBC it returned. FXC is a pure function of that input, so a binary is only ever reused for
  the compile that made it. Records carry their own checksum; a torn write ends the read and the file
  is rewritten from the records before it. The file is opened shared, so two Orcas on one user folder
  can both use it.

Rendering only: nothing drawn reaches RAM, so none of this can make two players' games differ.

Measured on an M4, Metal: a cold synchronous match start had a worst frame of 1.5 s (Brawl); with
these settings and a warm cache the worst is about 23 ms. On Windows (D3D11) the 63 ubershader
stages take about 30-37 s of FXC on every thread after an update, which is why the boot waits and
why FXC's output is cached.

### The match's code (JIT warm-up)

`Orca/JitWarm.*`. The first match in a process compiles the match's code at first use: in Brawl
~16,500 blocks in the two frames where the stage and the first fighter appear (64-105 ms each on an
M4) and ~4,100 more when the second fighter enters during the countdown.

- **Recording.** In a match's first 600 frames, every block the game compiles is noted (its start,
  physical ranges and a hash of its words). Blocks whose code is rewritten within a second are never
  kept. The list is merged into `Cache/<profile>.jitwarm` (Brawl ~30,000 blocks, 1.2 MB); blocks
  unused for 24 matches age out.
- **Warm-up.** From the next match's first frame, at each boundary, up to 8 ms
  (`ORCA_JITWARM_BUDGET_US`) of compiling listed blocks whose words are in RAM again. Both games
  show a still loading image for a while after the match scene starts (Brawl to frame ~169,
  Project+ to frame 27), which hides the work.
- **Paced by when each block first ran.** Each block is due 3 frames before the recorded match first
  ran it, and a sweep goes past the 8 ms as far as the nearest deadline needs (never more than
  100 ms in one boundary). Without pacing, a PC's Jit64 (about 40 us a block) had compiled only
  ~5,000 blocks by the frame where Project+ runs ~12,600 more, which froze that frame for 374 ms.
- **A shipped list for players without one** (`Data/Sys/Orca/RSBE01.jitwarm`, `PPLUS32.jitwarm`):
  used only when the player's own file is missing, then replaced by it. Re-record both after a change
  to the games' patches, scene detection or how the JIT splits blocks: run the reference inputs
  (`bf-two-new.txt`, `pplus-1v1.txt`) with `ORCA_JITWARM_SEED=0 YG_SCENES=1` on a fresh user folder
  and copy `Cache/*.jitwarm` into `Data/Sys/Orca`. ARM64 and x86-64 record the same lists.
- **JIT tables grow early:** Jit64's backpatch map rehashes inside a compile as it doubles (up to
  26 ms), so `JitBase::ReserveForBlocks` makes room at the match's first frame.
- **Why it can't change the game.** A block compiled early is the block the game's first run would
  compile: the same words at the same addresses (the hash), the same flags, in the guest's FP mode,
  kept only if its instructions span the recorded ranges. Only BAT-mapped code is touched, and the
  JIT never compiles from the hook where it could clear its cache or reuse code memory under the
  running block (`CanCompileFromHook`). Per-frame RAM hashes, sync tests and loopbacks are unchanged
  with it on, on both JITs.

Result: on an M4 every match start is smooth from the first launch after an update (worst frame
22-25 ms in Brawl and about 31 ms in Project+, against 88-102 ms before). `ORCA_JITWARM_LOG=1` logs
the first 600 frames' compiles; `node Tools/orca/match-start.mjs --from-frame 28 <run>` (Project+;
Brawl 170) counts the frame times a player sees after the loading image.

A rollback in Project+ recompiles 40-120 blocks (pages with code also hold data that changes every
frame, and a restore drops every block in a restored page). Invalidating only changed 32-byte lines
would keep most of them (not done).

## Graphics

Since 0.3.39 the player picks every graphics setting that only changes what their own machine
draws, and how fast. The YouGame app keeps them per computer and passes them on every start
(`-C Graphics.*` and `-C Dolphin.Core.GFXBackend`, the CommandLine layer). Code:
`Orca/Profile.cpp` (what a session forces), `Orca/Perf.*` (live changes, speed, the switch to
Performance).

**Why they can differ between players.** In a session nothing drawn reaches the game: EFB and XFB
copies stay on the GPU and RAM only gets a placeholder whose size comes from the game's own
registers; EFB peeks, bounding box and perf queries return constants; single core is forced, so the
host GPU's speed changes when a frame runs, never what it computes. Rollback snapshots skip the
framebuffer and texture cache, and the desync check hashes MEM1 and MEM2 only. So a setting that
only changes drawing can only make this machine slower or faster, and a slower machine is what the
speed lines below are for.

**What the player picks** (the app's Graphics panel): backend and graphics card (next start),
internal resolution (Auto, Native, 2x-4x; `MaxInternalResolution` is 4, so Auto never picks 5x or
6x on a 4K screen), anti-aliasing (next start: it reloads every pipeline, and with the shader wait
forced that would freeze both players), texture filtering, the scaling filter, scaled EFB copies,
the shader compilation mode (AsyncUber or AsyncSkip) and compile threads (next start), GPU texture
decoding, CPU culling and backend multithreading. The app offers the backends each package carries:
Direct3D 11 and 12 and OpenGL on Windows (its build has `ENABLE_VULKAN` off), Metal on macOS, OpenGL
on Linux. On Windows the adapter list leaves out trailing software adapters (WARP, "Microsoft Basic
Render Driver"), so a one-card PC lists one card.

**What stays forced** (`ForEachUniversalSetting`, checked by `VerifyForcedSettings` at boot):

- everything that changes what the game computes or shows only some players: the CPU and timing
  settings, EFB/XFB copies to RAM, EFB access, bounding box, perf queries, graphics mods, VSync off,
  Immediate XFB and the presentation pacing, the shader wait;
- since 0.3.39, also the settings a player's Dolphin.ini or GFX.ini could change before: the GDB
  stub (off: it would serve the game's memory to anyone), debugging and the 14 JIT debug switches,
  the low DCBZ hack, fastmem internals, the boot IOS, the HSP device, emulation speed (1.0) and
  Dolphin's pacing settings, Cap Immediate XFB, and, for fairness, the widescreen hack, Disable Fog,
  wireframe, the texture-format overlay and Free Look; from the review, PauseOnPanic (it turns on
  the JIT's memory checks, so an unresolved access breaks into a DSI instead of reading 0), the
  audio buffer size (80 ms: it bounds the throttle's fallback like the other pacing settings) and
  VertexRounding (a shader host-config bit only away from native resolution: with it on, the switch to
  Performance or a live `gfx res` would reload every shader on the CPU thread mid-match with no time
  limit). Each is Dolphin's default, so nothing a normal player computes changes and the
  compatibility key needs nothing new;
- the shader compilation mode within a range: a session takes the player's value only if it is
  AsyncUber or AsyncSkip, else forces AsyncUber (Synchronous and SyncUber compile on the emulation
  thread). A compile thread count outside -1 and 1-4 (0 is the emulation thread; hundreds from a
  hand-edited GFX.ini) becomes -1, Dolphin's Auto. With no
  value from anywhere, a session uses what it forced before 0.3.39 (AsyncUber and Auto), not
  Dolphin's defaults (Synchronous and one thread). Both are pinned at boot, so a game INI layer a
  launcher's disc brings in later can't change them.

**Live changes:** `gfx <key> <value>` on stdin (cap `gfx`), applied at the next frame boundary on
the CPU thread, for this run only (the app passes the stored choice at the next start). The keys are
a fixed list, because the CurrentRun layer they write outranks the forced one and
`VerifyForcedSettings` runs only at boot:

| Key | Setting | Values |
|---|---|---|
| `res` | `Graphics.Settings.InternalResolution` | 0 Auto, 1 native, 2-4 |
| `aniso` | `Graphics.Enhancements.MaxAnisotropy` | -1 the game's own, 0-4 (1x-16x) |
| `filter` | `Graphics.Enhancements.ForceTextureFiltering` | 0 the game's own, 1 nearest, 2 linear |
| `resample` | `Graphics.Enhancements.OutputResampling` | 0-6 |
| `efb` | `Graphics.Hacks.EFBScaledCopy` | 0, 1 |
| `gpudecode` | `Graphics.Settings.EnableGPUTextureDecoding` | 0, 1 |
| `cpucull` | `Graphics.Settings.CPUCull` | 0, 1 |

`res` and `gpudecode` cost one short hitch (the EFB is rebuilt or the texture cache dropped), which
the other player sees as one stall. Only the last value queued for each key is kept, so a burst of
lines (a page while the game is paused) applies as at most one change per key. No `gfx` key, and
nothing the switch to Performance writes, changes the shader host config (the
`OrcaGraphicsSettings.ResolutionChangesNeverReloadTheShaders` test).

### This machine's speed

Orca measures its own speed (`Orca/Perf.h`) and reports it as `orca perf {json}` on stdout,
embedded only, each line at most 300 bytes, versioned with `"v":1`:

- `{"v":1,"k":"info","be":"D3D","gpu":"...","ad":[...],"ai":0,"cpu":"...","th":12,"os":"win","rev":"..."}`
  once after `ready`: the backend in use, the graphics card it uses, up to 3 adapter names and the
  adapter index, the CPU's name (dropped if the line would pass 300 bytes), logical threads, the OS
  and this build's source revision. Names are printable ASCII, at most 40 characters.
- `{"v":1,"k":"win","m":"1v1","n":858,"cap":51.7,"fc":58.0,"rw":12.4,"rv":180,"w50":18.9,"w95":24.1,"g95":3.2,"fps":51.6,"rbf":212,"res":1,"pf":1,"as":1}`
  every 15 s of running time (a pause doesn't count), menus included (`n` is 0 there), with
  `"t":1` at the end when a test switch shaped the run.
- `{"v":1,"k":"auto","cap":44.3}` once, when the run switched itself to Performance (`cap`: the
  median speed of the windows that switched it, at the reference re-run load below).

How it is measured. A forward frame is a boundary whose frame ran for the first time in real time.
Its time since the previous forward boundary includes any rollback re-runs (they are this machine's
cost); taken out are the CPU thread's sleeps in CoreTiming (the throttle and the present pacing)
and the session's stall and time-sync waits (`StepSession`). What is left is the frame's work. A
frame counts once the game has been in a match for 900 frames (Brawl's scene, as the JIT warm-up
reads it), and not when the game was paused, hidden by the page, held for a load, caught up after a
join or run unseen. The work is also split at the frame's last re-run boundary: before it is re-run
work (the rollback's load and its re-run frames, waits out), after it forward work. Re-runs depend on
the opponent (their distance, how often their inputs change), forward work only on this machine, so
the page can read the machine at a reference re-run load whoever it played. In a window, `n` is the
counted frames, `cap` is `n` over their total work (the frames per second this machine could run,
not capped at 60), `fc` the same over forward work only, `rw` the re-run work per re-run frame (ms)
over the `rv` re-run frames inside the counted frames, `w50`/`w95` the median and 95th
percentile work in ms, `g95` the 95th percentile time blocked binding the back buffer and presenting
(a GPU-bound machine has most of its work there), `fps` frames shown per running second, `rbf`
re-run frames, `m` the peers plugged in (`solo`, `1v1`, `multi`), `res` the EFB scale in use (Auto
resolved), `pf` 1 when the picture settings are Performance's with anti-aliasing off, `as` 1 when
the switch fired this run and the picture is still Performance's (anti-aliasing aside, which waits
for the next start; a player who raised the picture again is measured on their own settings).

**The switch to Performance.** A window counts when it has at least 600 frames, solo or 1v1 (3-4
player rooms dip normally). It is read at the page's reference re-run load (`CapAtRef`, the page's
`perfCapAtRef`): its forward work per frame plus 0.1 re-run frames at the re-run cost it measured
(`rw`, once `rv` is 30 or more; else the last cost the run measured; with none, `fc` alone). The
raw `cap` grows with the distance to the opponent: an i7-8700 / RTX 2080 at 40 ms each way read
51-56 with 63-71 forward frames a second, and switched in every match before this rule. After 4
counted windows in a row under 58 fps at that load, with the picture settings not already
Performance's, Orca switches once per run: native resolution, unscaled EFB
copies, the game's own texture filtering, the default scaling filter and no post-processing shader,
at once and mid-match too (one hitch beats a match of stalls). It shows "Graphics set to Performance
to hold 60 fps" and prints the `auto` line; the page keeps Performance (and anti-aliasing off) for
the next start. Menus and big rooms in between don't break the run; a counted window at 58 or more,
one already on Performance, or a `gfx` change starts it over. Why 58: time sync gives back at most
one frame in 30, so the faster side can only slow to about 59.94 x 30/31 = 58.0 fps; under that the
other player stalls.

Test switches, read only with `ORCA_TEST_COMMANDS=1` in a session (a harness run; the app passes
Orca neither) and marked `"t":1` on every window line: `ORCA_TEST_PERF=<window_s>[,<warm_frames>]`
shortens the windows (and starts the lines without the app's `ready`), `ORCA_PERF_AUTO=0` turns the
switch off, and `ORCA_TEST_SLOW=<ms>[,<ms_on_performance>]` makes a slow machine: each forward frame
spins the CPU thread that long (the second value while the picture is Performance's), so the switch
fires and helps.

Observation only: wall-clock time and drawing settings never reach emulation, the replay, the
checksums or the compatibility key.

## Code the JIT doesn't see

Project+'s Gecko code handler applies `NETBOOST.GCT`'s RAM writes with plain stores (no `icbi`), and
the first time it writes them (frame 1) seven words of Brawl's code change, in functions the boot
already ran and the JIT already compiled:

| Words | Code | Effect of a JIT clear |
|---|---|---|
| `80023b88`, `80024028` | Move v-sync call for Brawl/PM | RAM differs the next frame |
| `8001cd24`, `8001cd2c`, `800266b8` | Fixed OSReport Syntax on File Reading | RAM differs at the next file read |
| `8018cfc4`, `8018cfc8` | Extra null pointer catch | none seen |

Nothing invalidates those blocks afterwards, and Dolphin's workaround for such code handlers is
spent during the boot. So a warm JIT runs Brawl's words for the whole process, and the first
whole-cache clear switches to Project+'s: a desync from the next frame. Orca never clears the JIT in
a session itself, but Dolphin does when its code space fills, rarely, and at different times on a Mac
and a PC.

**The fix:** the v-sync move's two words are written by `PPLUS32.patches` at the first frame
boundary, before the codes first write them, through the memory-patch path, which drops the JIT's
copies; every machine runs the move from frame 1 and the codes then write the same words ([Input
latency](#input-latency)). The other five are `KeepGameCode = <address> <Brawl's word> <Project+'s
word>` lines in `PPLUS32.ini`: every instruction fetch there (both JITs and the interpreters) gets
Brawl's word while RAM holds Project+'s, so any JIT clear compiles what ran before, and RAM, its
hashes and the codes' own reads are unchanged. `JitClearFrame = 2` adds one clear at the end of
frame 2, so every machine compiles from then on whatever a fetch reads, whatever its boot compiled.

Through 0.3.30 the v-sync move was kept as Brawl's words too: run for real, its early copy let each
rollback's re-run land on that frame's present and drag the presents after it (twice the 60 Hz
repeats and drops with rollbacks). The steady presentation in [Input latency](#input-latency) fixed
that.

**Checking a codeset** (a Project+ update, another launcher profile): `Tools/orca/jit-history.py
<nogui> <disc> --pplus <dol>` runs the boot twice (as it comes, and with JIT clears), compares the
hash logs and lists every word a live block runs that a fetch no longer reads. List such words as
`KeepGameCode`.

## Game patches

`Data/Sys/Orca/<profile>.patches` (`Orca/UX/GamePatches.h`): Orca's own changes to the game's code
and data, forced on in every session (the player's own codes are dropped) and hashed into the
compatibility key. They are applied at every frame boundary from the frame hook, first runs and
re-runs, as a pure function of emulated memory. One 32-bit word per line, in hex:

| Kind | Lines | Written |
|---|---|---|
| Guarded group | `<address> <original> <value>`, consecutive words | once, while every word is its original; a line whose original equals its value only guards |
| Data | `<address> * <value>` | every frame |
| Toggled group | guarded lines between `when` lines and `end` | its values while the conditions hold, its originals while they don't |

```
when 806F2380 == 59474D31               # the match block's first word is 'YGM1'
when 806F2384 & FF000000 != 00000000    # and its masked word isn't 0
8119A120 2C030000 2C030000              # cmpwi r3, 0 (guard)
8119A124 4082000C 4800000C              # bne -> b while both hold
end
```

- A `when` reads one aligned word of MEM1 or MEM2 at fixed address; an unmapped address never holds.
- A group whose words are neither all originals nor all values (its module not loaded) is left alone.
- Every write goes through Dolphin's memory-patch path, which drops the JIT's copy.
- Toggled groups are applied last in the frame hook, so a condition set at a boundary is followed
  at that boundary.
- The loader refuses: a `*` line in a block, nesting, an `end` without `when`, a mask of 0, a
  condition that reads a word the file writes, and a toggled group of one word (a lone word matches
  other code by chance too easily: add guard words).
- Never patch a word that game code also writes. Project+'s code handler rewrites its hooks every
  frame, so a toggle there would land and be undone within each frame, and what the JIT runs would
  depend on its history.

Real code goes into a code cave ([Free space](#free-space-and-the-match-block)). Tests:
`OrcaGamePatches*`.

## Free space and the match block

The online rules need room in the game's own memory: a **match block** that every rollback snapshot
and drop-in keyframe carries, and **code caves** for patches. Both come from one place, the same in
Brawl rev 2 and Project+ v3.2: the dead code of Brawl's online sequences in `sora_scene.rel`
(`Orca/UX/FreeSpace.h`).

| What | Where | Size |
|---|---|---|
| Match block (data only) | `0x806F2380`-`0x806F2618`, in sqNetAnyOkiraku::setNext | 664 bytes |
| Code caves | 12 runs from `0x806F2674` to `0x806F82A4` (listed in the header) | 16,908 bytes |

**Why it is free.** sora_scene is loaded once at boot (`0x806BB480`) and never reloaded, and
Project+ ships no copy of its own. The online sequences start only from the main menu's exit codes
24-27 and 29-31, which Orca's patches redirect ([Online menu](#online-menu)). Every reference into
these runs, in every module on the disc and in Project+'s modules and plugins, comes from those
sequences' own tables. Left out: each sequence's create function (it runs at boot), 25 words the
loader relocates against `sora_melee`, and `0x806F3DA4`, which Project+'s codeset nops every frame.

**Proven** by `Tools/orca/free-space.py`, which runs one loop through every scene online play
visits, twice: once watching the block and caves (they keep the game's own bytes, and nothing writes
them), once with them filled with illegal instructions (none ever runs, the fill survives, and every
RAM dump is byte-identical to the first run's outside the block and caves). Sync tests and keyframe
tests pass with the fill. The tool reads the ranges from the header: until S5, FreeSpace.h's
`kMatchBlock` was only the block's first 512 bytes (`kMatchBlockHead` now), so the last 152
(`0x806F2580`-`0x806F2618`: character order, ranked set, friend drop-in) were not in the proof.

**Using it.** Before Orca writes it, the block holds code (its first word is `li r17, 0`), so
readers check the magic. Only Orca's frame hook writes the block, never a patch group. A cave is a
guarded patch group whose originals are the dead code there; any MEM1 address is a relative branch
away.

**Layout** (offsets from `0x806F2380`, big-endian; `Orca/UX/MatchBlock.h`):

| Offset | What |
|---|---|
| `+0x00` | Header: `YGM1`, version 1, mode (0 none, 1 casual, 2 ranked), ruleset (1 Brawl Supernova 2025, 2 Project+ 2024), `+0x07` coin, `+0x08` room hash, `+0x0C` queue flags |
| `+0x10`-`+0x8F` | Project+'s stage flow (`MatchBlock::State`) |
| `+0x80`-`+0xBF` | Brawl's stage flow (`BrawlStages`; each game uses its own range) |
| `+0xC0`-`+0xCF` | Ranked ready timer, no-show, plugged ports |
| `+0xD0`-`+0x1A7` | The player's own rules, saved when the header was written (Project+'s stage switch data from `+0x100`); `+0xF4`-`+0xFF` are the stage cursors' shared bytes |
| `+0x1B4`-`+0x1BF` | The two stage cursors |
| `+0x1C0`-`+0x1EF` | The queue's character select |
| `+0x1F0` | Saved rules' flags |
| `+0x1F4`-`+0x1FF` | Character order: where each hand last held its token |
| `+0x200`-`+0x20F` | Character order |
| `+0x210`-`+0x28F` | The ranked set (`SetBlock`, authoritative) |
| `+0x290`-`+0x297` | A friend's move from the menus |

Any new header (another mode, ruleset, coin or room) starts every track's state fresh.
Compatibility: a change to one game's module that changes only what that game computes bumps that
game's own revision (`Games/Brawl/BrawlGame.h`, the key's `ux=<n>.<game>`; a game revision of 0
omits the suffix). Engine compatibility (`UX::kCompatVersion`) is currently **31**. From 0.3.48,
Brawl and Project+ use game revision **1** for returning a held token to the player's own panel
to sit out, so their keys say `ux=31.1`. Results-only and GenericModule also use revision **1**;
the legacy Brawl launcher stays at **0** (`ux=31`). Anything else that changes what the game
computes bumps the engine compatibility. Engine 31 keeps player-selected tag controls, including
the C-stick. Earlier engine changes: `ux=29`: a casual stage select's Random, the two cursors start on
nothing, and a button held into the stage select counts once it is let go, so an A held into a
ranked stage select no longer proposes until released, and Project+'s auto-pick waits for the hover
to leave Random; also leaving is a held B and Z reaches the game on the queue's selects (hold B to
leave); `ux=28`: a friend's move from the menus waits while Options > Controls is open; `ux=27`: how
a turn ends in the [Character order](#character-order), and who turns Brawl's stage select page;
`ux=26` since 0.3.37: the history's seed, [Random numbers](#random-numbers), and tags typed in the
game, [Port values](#port-values-each-players-name-and-controls). These revisions are in the
compatibility key.

## Input gate

The queue rules take input away from a player: the port whose turn it isn't, B on the stage select,
a locked character. The gate does that per port and per frame where the game reads its controllers,
so no game code has to be patched per control (`Rollback/InputGate.*`).

- **A mask** makes buttons read released (L and R also zero their analog value) and sticks centred.
  An unplugged port stays unplugged. A mask can also **press** buttons for the game
  (`Mask::press`), **steer** a stick (`Mask::steer`), or read the stick centred while A is down
  (`a_centres_stick`), so an A acts where the hand stood. `drop_with_lr` releases buttons only
  while L and R are both held (pressed, or a quarter press): Project+'s Code Menu chord.
- **Where.** `Rollback::InputOverride`, called for every SI poll and for the relatch. Everything else
  stays raw: the local pad sampled for the wire, the session's pads, the harness's recording. Both
  machines apply the same masks to the same pads, and a re-run applies them again.
- **When.** One source (`InputGate::SetSource`, `UX.cpp`), a pure function of emulated memory,
  evaluated as the frame hook's last step, after any load at that boundary. So a frame's masks
  always come from the state that frame starts from, and nothing of them needs to be in snapshots.
  The source may not write memory; what it needs from the session (ports, names) the frame hook
  writes into memory earlier at the same boundary.
- **The latch** (`InputGate::SetLatch`): the raw pads of ports 1-2 for the frame about to run,
  written into the match block after the masks, so the rules can see presses the game didn't (Start
  to lock in, B, Z, the stage cursors' sticks). It runs after the boundary's snapshot or keyframe was
  saved or loaded, on every pass, so the next boundary's hook reads the same bytes everywhere.
- Every session masks Z on the results screen (below); everything else comes from a queue header.

Test knob: `ORCA_TEST_GATE=<port>:<buttons>[:always|mem/<n>],...`, e.g.
`ORCA_TEST_GATE=1:A+STICK+CSTICK+X+Y+Z+R:mem/8,2:START`. `mem/n` masks on one frame in n chosen from
a hash of game memory, so masks computed from the wrong frame show up in a sync test. Tests:
`OrcaInputGate*`. Evaluating the masks where the frame callback runs (before a load) failed the sync
test with 385 RAM mismatches; where they are now, 0.

## Online menu

Play Online works in both games, through the game's own menus and never a new screen; no Nintendo
server is ever asked for anything; nothing about connecting looks like Nintendo's.

- **The top page.** Brawl's Wi-Fi button reads PLAY ONLINE (Project+'s already does), with "Play
  different modes online." under it.
- **A opens the game's own ONLINE page** (With Friends / With Anyone) with no connection window: no
  save check, no NAND access, no network task. B goes straight back. Options (WiiConnect24) and
  Spectator never show.
- **With Anyone** shows CASUAL and RANKED. Either goes to the local Versus character select and Orca
  prints `orca menu online casual|ranked` (with the app's `host` cap; `... local` without).
- **With Friends** goes straight to the local Versus character select, where friends drop in, and
  prints `orca menu online friends`. ("Play with friends you invite! (2-4 players)".)
- **B out of that character select** goes back to the page and button the pick was made on, as the
  game does after Group > Brawl. It holds after matches too.

**How** (`Data/Sys/Orca/RSBE01.patches`, `PPLUS32.patches`, `Orca/UX/MenuText.h`, `OnlineMenu.h`):

- **Game patches** (guarded groups; Project+'s menu module sits `0x1480` above Brawl's): the top
  page's press takes the path every other button takes, never creating the connection window; the
  ONLINE page's B does what the game does after its disconnect window says OK; A on With Friends
  leaves the menu with exit code 25; Options and Spectator are never enabled; the main menu's exit
  table sends codes 24-27, 29, 30 (Casual) and 31 (Ranked) to Orca's block, which starts the local
  Versus scene. No online scene, network thread or DWC is ever entered.
- **The back out.** Orca's block keeps the exit code in sqVsMelee's object (`+0x18`, slack in its
  heap block); on the way back a helper turns it into the main menu's argument for that page and
  button. It is game memory, so rollback and keyframes carry it.
- **Text** (`MenuText.h`): the main menu's messages that mention Nintendo WFC, Project+'s
  "(Discontinued)" and the rewritten descriptions are replaced in emulated memory, only while the
  bytes are exactly the game's own, into the same slot.
- **Textures** (`Data/Sys/Orca/Textures/RSBE01`, `Tools/orca/textures/`): Orca's own label art
  (PLAY ONLINE, ONLINE, CASUAL, RANKED), drawn by `make-labels.py` with SIL OFL fonts. A session
  forces texture replacement on and loads Orca's pack with the player's own; Orca's names win, so a
  player's HD pack can never bring back the Nintendo WFC labels. Textures only change what is drawn,
  so the pack is not in the compatibility key.
- **Events** (`OnlineMenu.h`): a reader in the frame hook follows the menu at every first-run
  boundary and reads the exit code. It prints an event only for a pick made while the player's game
  is alone (no friend plugged in or on the way, nothing of drop-in pending); a Casual or Ranked pick
  made with friends goes to the session instead (below). Going solo or loading a keyframe starts it
  over, so nothing stale is printed.
- **No IOS network access:** NET_KD_REQ, IP/Top and SSL refuse in a session, and
  `Tools/orca/online-menu.py` fails on any `/dev/net` open, network request or NAND access to the
  save, WiiConnect24 or network folders after boot.

| stdout | When |
|---|---|
| `orca menu online casual` / `ranked` | With Anyone > Casual or Ranked, with the app's `host`. |
| `orca menu online casual kept` / `ranked kept` | The same with friends in a host's game, kept because the app can't search that queue now: nothing armed, never a search. Printed again unmarked once the friends are gone. |
| `orca menu online casual local` / `ranked local` | The same without `host`: nothing searches. |
| `orca menu online friends` | With Friends. |
| `orca menu cancel` | The player backed out of searching, alone. Once. Also when a friend on the way, or a friends room joined, ends the queue ([Play again](#play-again-after-a-ranked-set)). |

### Casual or Ranked with friends in the game

With Anyone > Casual or Ranked leaves the friends lobby for a 2-player room the matchmaker fills
(`OnlineMatch.cpp` `TakeLobbyPickNow`, `DecideLobbyPick`, `ArmKeptPick`). The reader hands such a
pick (never With Friends) to the session at the boundary that saw it, after the app's commands
(`OnlineMenu.h` `TakeLobbyPick`). Either player's press picks in a shared menu, and every game sees
it at the same frame and goes to the same Versus character select.

- **The host** (port 1, with the app's `host` and the queue's pick cap, `pick-casual` or
  `pick-ranked`), with friends in its game, waiting for a keyframe or arriving, leaves its room as
  the app's Leave does, prints `orca state left lobby`, shows "Left the lobby for Ranked" (or
  Casual), then announces the pick (`orca menu online casual|ranked`). From the next boundary it
  plays alone on port 1, so the queue's own character select gets its header and Start readies. An
  arrival not taken in yet counts as a friend: taken in later, it would refuse the queue's header.
- **Each friend** lets the pick go (the game is its host's), hears `orca state host-left` and goes
  home on that character select ([Going home](#going-home)). There it reads the pick the select was
  opened for (`CssTitle::ReadPick`, 30 or 31: `OnlineMenu.h` `CssPick`) and announces it too,
  whatever its own app can search, so its Start readies it without backing out.
- **A queue the app can't search now** (it answers `pick-casual` / `pick-ranked` in its `caps` only
  for a queue it can search; Ranked needs a signed-in player): leaving would drop the friends for
  nothing. The host keeps them and prints the pick in place, unarmed (`orca menu online ranked
  kept`): no queue select, no search, no queue header. The app answers with its reason and
  `queue-cancel`, which changes nothing. Once the friends are gone and the host's game is alone on
  that select (`DecideKeptPick`), the pick is armed as an alone pick (`orca menu online ranked`);
  it is dropped once the game moves on from that select (`PickStands`) or a queue or search starts
  anyway. The app sends `caps` again when what it can search changes; a later `caps` line replaces
  the whole answer (`Status::SetAppCaps`).
- **Nobody to leave by now** (the app's own leave went first, the last friend has gone, or only a
  keyframe is kept for an invite): the pick is announced in place with no `left lobby`, and a kept
  keyframe is dropped (never while a session still runs). A friend who comes later ends that queue
  first ([Play again](#play-again-after-a-ranked-set)).
- Without the app's `host` nothing would search, so the friends' game stays theirs and nothing is
  printed. A queue room's menus are locked.
- A pick seen at a boundary the commands can't take (mid re-run) waits for the next while it still
  stands (`PickStands`), and is dropped once the game has moved on from it.

Local: the session ends at the pick's boundary, so only the room's leave reaches the friends' games,
and the compatibility key doesn't change. Tests: `OrcaOnlineMenuLobby.*`, `Tools/orca/dropin-rooms.py`
steps 6-10.

### Boot and friends from the menus

Booting either game lands on the main menu. Brawl's patches skip the strap, the save prompt and the
title. Project+'s codeset has "Boot Directly to CSS"; one guarded group over `NETBOOST.GCT`'s words in
memory (`0x80559C20`) makes its default case go to the title and main menu as vanilla Brawl does.
Project+ reaches its main menu at frame 197. Online, the boot plays nobody's inputs and ends at the
origin, the built main menu 30 frames on ([How it works](#how-it-works)).

**A friend who drops in while the host is anywhere but Versus** (`OnlineMenu.h` `FriendsMove`): once
the main menu has built its pages and run 30 frames, the frame hook leaves the menu with exit code 25,
as With Friends does, so both games go to the character select at the same boundary. It counts from
the menu's build (`+0xAC8` of muMenuMain), not its start: moving before the build made the menu's
exit call destructors through uninitialised page slots and froze both games. Never while Options >
Controls is open: its name list and keypad run as a gfTask of their own (`MuName`, linked after the
menu's task), and leaving the menu under it froze (the list) or crashed (the keypad, `0x80677f54`)
both games, so the friend waits on the host's menu until the host backs out of it, then 30 frames
more. Never in a queue room.
That character select is online, so its CPUs go and a friend whose panel had one takes it at once
([No CPUs online](#no-cpus-online)).

**A join held in a single-player mode:** while the host plays Classic, All-Star, Events, Training,
the Subspace Emissary and the like (`SequenceHoldsDropIn`), a friend who arrives waits: no keyframe
is made. The host prints `orca state friend-holding`, the joiner `orca state friend-waiting` and
shows that the host is in a single-player mode. The join's 90 s limit doesn't run meanwhile.

## Matchmaking and results

With Anyone > Casual or Ranked plays a stranger YouGame's queue found (Elo and distance windows, the
same Orca build and disc), then the existing drop-in into the matched room. Ranked is a best of 3,
rated once per set.

### The flow

1. The pick prints `orca menu online casual|ranked`. The game is on the queue's own character select
   ([Character select](#character-select)); the page searches once the player is ready.
2. The page queues a ticket in the queue's mode with Orca's compatibility key. On a match, slot 0 gets
   `host <code>` and the other `join <code>` 1.5 s later.
3. `host <code>` works only while the game is alone (else `unsupported host`). The game hosts the
   room where it is. Once the room's welcome names its queue, the next safe boundary writes the
   match block's header; the opponent's keyframe waits for it.
4. `join <code>` is the ordinary drop-in: the joiner lands on port 2 of the host's character select.
5. Once the opponent has plugged in, the host begins the room's match (`{"t":"begin"}`); both learn
   the match id from `start`.
6. Each game's result is read from memory once its frames are final and reported by both Orcas, the
   same bytes. Casual: each game is a `finish`, the room stays. Ranked: each game is a `game-report`;
   when one player has 2 agreed wins both send `finish {winner}`; 7 games without that, `finish
   {void}`. The room settles each report by agreement on the outcome; different outcomes void the
   game. Ranked also says when each game began: `game-start` once the first frame with the fight's
   fighters in is final (below).
7. After a ranked set's verdict the room stays open for up to five minutes (`set-over`) so the
   players can talk; each Orca leaves when its player moves on ([On-screen UI](#on-screen-ui)).

Leaving a ranked set once game 1 began is a forfeit (the room's 15 s grace); a set left before
game 1 began (a no-show, a leave or a crash while game 1's stage loads) is void and rates nobody.
The room knows game 1 began from the leaver's own `game-start` (Orca 0.3.33+) or from a game it
settled (rooms `game-lobby.ts` `reached`; YouGame `docs/ORCA_ONLINE_UX.md` "3B leaving mid-game").
A desync voids the match in progress before the players part.

`game-start`: each Orca sends `{"t":"game-start","matchId":M,"id":"g<start>"}` once per ranked game,
when its fighters are in: the first frame of a fight with `SET_FIGHT_FLAGS`' `FIGHT_SEEN`, which
`RankedSet::Step` sets when `ReadLiveFight` reads both fighters (Brawl and Project+). That is after
the stage's loading image (`scMelee` starts under it: Brawl to frame ~169, Project+ to 27): headless
over `bf-ranked-bo3.txt` and `pplus-ranked.txt` the fighters were in 283 frames (Brawl) and 140
frames (Project+) after the fight's first frame. So a crash or a 15 s stall while the stage loads
leaves the set void, as the rule says. `<start>` is still `SET_FIGHT_START`, the fight's first
frame, the same on both machines and the id that game's `game-report` carries (the room doesn't need
the two to match). The tracker names it only once the fighters' frame is final
(`ResultsTracker::Confirm`'s `starts`), so a rollback re-run never sends one for a fight that didn't
happen, and only for a fight begun after the opponent's plug frame, as the reports. It goes with the
reports' queue (16 held at most, each up to 60 s until there is a room match) and only in a ranked
queue room with its match current and the set still open (not after this side's `finish` or the
verdict); once per game per match. A sudden death is the same game; a fight left without its results
screen began all the same. The room answers nothing; a refusal (`command-error`, `action:
"game-start"`) is logged and ignored. Project+ reads the live fight only for this: its records count
no ledge grabs or time-up snapshot, as before. Tests:
`OrcaRankedSet.AGameBeginsWhenItsFightersAreIn`, `TrackerSaysWhenEachGameBegan`,
`AGameBeginsOnlyOnFinalFramesAfterThePlugFrame`, `OrcaRoom.GameStartIsTheRoomsMessage`;
`Tools/orca/queue-e2e.mjs --q2 --scenario leave --in-game 1`.

On screen the same rule holds (`SetEnd::Model::GameBegan`, from the tracker's starts and results):
before the set's first game the leaver reads "You left · This set doesn't count" (a no-show "Time's
up · You didn't pick in time · This set doesn't count") and the other player's countdown runs to "No
contest"; from it on, "This set counts as a loss" and "They forfeit in". Tests:
`OrcaSetEndCountdown.BeforeTheFirstGameTheCountdownIsToNoContest`,
`OrcaSetEndNotice.LeavingBeforeTheFirstGameSaysItDoesNotCount`.

| stdin | |
|---|---|
| `host <code>` | Host the matched room. Refused: `unsupported host`. |
| `queue-cancel` | The page's search ended. |

| stdout | |
|---|---|
| `orca caps ... host results locks queue2 pick-casual pick-ranked` | `results` where the result reader is verified; `locks`, `queue2` and the pick caps where the profile has a ruleset (Brawl rev 2 and Project+). The app answers a pick cap only for a queue it can search now. With `music` that is 16 caps, the most an app before the Music switch's release passes (that release takes 24); more needs it as the minimum app (`OrcaOnlineMenuLobby.EveryCapFitsTheAppsCapsLine`). `talk`, `rumble`, `record` and then `hostui` come after them, so such an app drops only those. |
| `orca result <json>` | The room's verdict, when it arrives. At most 300 bytes. |
| `orca state friend-left match-over` / `host-left match-over` | A ranked room closed after its set. |

`orca result` (the local player first; `d` the settled detail):

- casual game: `{"q":"casual","k":"game","n":3,"out":"won"|"lost"|"draw"|"void","why"?:...,"d"?:{"st":2,"c":[mine,theirs],"s":[mine,theirs],"to":0|1}}`
- ranked game: `{"q":"ranked","k":"game","n":2,"out":...,"score":[mine,theirs],"d"?:{...}}`
- ranked set: `{"q":"ranked","k":"set","out":"won"|"lost"|"void","by":"games"|"forfeit","score":[...],"r"?:[before,after],"rank"?:"..."}`

Room messages sent (queue rooms only; friends rooms never begin or report): ranked
`{"t":"game-start","matchId":M,"id":"g<start>"}` and
`{"t":"game-report","matchId":M,"id":"g<n>","winner":"<participant id>","detail":D}` (or `"draw"` or
`"void"`), casual `{"t":"finish","matchId":M,"winner":...,"detail":D}`, a set's `finish`. D is
`{"f","st","c","s","to"}`: the session frame of the first final results-screen reading, the stage,
characters, stocks left and whether time ran out, in port order. Handled: `start`, `game-result`,
`result`, `set-over`, `command-error`, and the socket's 4004 close.

### Reading results

`Orca/UX/Results.*`, read only. At every boundary the frame hook stores a small reading of the frame
just run: the scene (`scMelee` the fight, `scMemoryChange` between, `scVsResult` the results) and, on
the results screen, the result info. OnlineMatch consumes readings in frame order once their frames
are final (`Session::ConfirmedFrame`), so both machines see the same readings. A game is a fight
entered after the opponent's plug frame, ended by the results screen or by any other scene (void
`nocontest`). The verdict needs exactly two humans on ports 1 and 2 (else void `ports`) and a fight
that ended normally.

Where (Brawl rev 2; Project+ runs the same executable): `g_GameGlobal` `0x805A00E0` points to the
mode data (`+0x08`: stage, time limit, per-port init data of `0x5C` bytes), the result info (`+0x18`:
per-port records of `0x2AC` bytes at `+0x24` with character, stocks left, place, KOs, falls; the
decision at `+0x1378`: 9 during a fight, 1 when time runs out, 2 at a deciding KO), and the rules
(`+0x1C`, gmSetRule: mode, minutes, stocks, items).

## Online rules

`Orca/UX/OnlineRules.*`, `MatchBlock.h`. Nothing here acts without a match block header, and only a
queue room's host (or the queue's own character select) writes one. Friends rooms are unchanged.

**The header** is written only at a boundary no other game runs from a state before it: one where
the game is its player's alone, or one where an arriving opponent still waits for a keyframe not yet
made (`HeaderFreeAt`). A keyframe for an arrival is made only once memory carries the header the
room calls for (`Rules::HeaderInPlace`); if that takes over 5 s the host leaves with `orca error
mismatch`. The joiner checks the header after its keyframe loads (`Rules::HeaderFitsRoom`: mode,
ruleset, coin, room hash, queue flags) and leaves on any difference. Casual unlocks when the
opponent unplugs; ranked stays locked while the set is open.

**The locks** (first runs and re-runs, only bytes that differ):

| | Brawl (Supernova 2025) | Project+ (2024) |
|---|---|---|
| Rules | stock, 3 stocks, 8:00, handicap off, team attack on, pause off | the same with 4 stocks |
| Items | none | none |
| Legal stages | Battlefield, Final Destination, Delfino, Yoshi's Island, Lylat, Smashville, Pokemon Stadium; FD off while a human picked Ice Climbers | the 2024 Proposed list (preset 3, `Switch03.rss`): 9 stages, hazards off |
| Input gate | character select: A only in the grid, B only to take the token back up; stage select: every port while the two cursors run (they act instead); fight: Start | the same, plus the D-pad on the character select, and D-pad Down while L and R are held in every scene (the Code Menu's chord) |
| Code Menu | (none) | off, closed, every cheat and gameplay value at its default (below) |
| Fighters | any | Giga Bowser and Wario-Man play as Bowser and Wario |

- **No way back to the menus.** A toggled patch group conditioned on the header flips the character
  select's "back to the menu" exit to "character select again", so BACK reloads the select.
- **Ranked's no-ready timer:** before the set's first fight, once one player has picked and the
  other hasn't, 60 s; then that port is the no-show, whose own Orca leaves. No game began, so the
  room voids the set and rates nobody.
- **Stalls:** a ranked player whose game has waited 15 s on the opponent's inputs, or whose opponent
  went silent, claims the set (`Online::ReportStall`); the room lets it stand when the other side has
  been silent too, and voids it when the stalled player's game never said game 1 began.

Test knob: `ORCA_TEST_QUEUE=casual|ranked[:<coin>][:q2|:solo]` writes the header as a queue room's
host would, in a harness run or a dev room. Tests: `OrcaOnlineRules*`.

### Project+'s Code Menu and banned fighters

Project+'s netplay codeset carries its Code Menu (`pf/menu3/dnet.cmnu`, loaded as is to
`0x804E0000`). Any port opens it with L+R+D-pad Down in any scene, mid-fight too, and it pauses both
games while open; from it a player could turn on flight, infinite shield and debug displays, swap a
port's character (Giga Bowser, Wario-Man) and change hitstun, shield and other constants, all in
sync. Its character select also turns a Bowser or Wario pick into Giga Bowser or Wario-Man, both
banned, when the player holds shield (L) as the select ends. Under any locking header (casual,
ranked, the queue's own character select), since `ux=20`:

- **The menu is off and closed.** Code Menu Activation is OFF, its state is closed (an open menu
  closes as its own Start does and the game's pause word comes back), and 73 cheat and gameplay
  values are held at the file's defaults at every boundary. The four Character Select lines follow
  the fighters and are left alone; so are the replay, tag, costume, crowd and HUD colour lines.
  Nothing is written unless the memory has the pinned release's layout (`Rules::CodeMenuPresent`).
- **The chord is dropped:** the gate drops D-pad Down while L and R both read held (button, or
  analog at a quarter press), in every scene (`drop_with_lr`). That also stops the debug codes' own
  L+R+Down toggle. Activation OFF is what keeps the menu shut; the drop is a backup.
- **Banned fighters** become Bowser (`0x0C`) and Wario (`0x17`) on the character select, the stage
  select and between scenes, in the match's init data and in the character select's record
  (gmSelCharData), which the way to the fight copies into the init data again.
- The header saves the player's own activation and its clear gives it back; the values stay at
  Project+'s defaults afterwards. Friends rooms keep the menu as it is.

Tests: `OrcaOnlineRules.*CodeMenu*`, `OrcaOnlineRules.ProjectPlus*`,
`OrcaInputGate.DownDropsOnlyWhileLAndRAreBothHeld`. Two Orcas: `queue-e2e.mjs --pplus <dol> --q2
--queue ranked --pick bowser --giga --chord`. More in ORCA_NOTES.md, "Project+'s Code Menu".

## No CPUs online

No CPU exists in any online mode, and online character selects show only the seats in use
(`ux=19`, `Orca/UX/OnlineSeats.*`). Local Versus (Group > Brawl, offline) keeps its CPUs.

**Where a CPU came from** (found under the harness with `ORCA_UX_PROBE` watches, both games):

- The Versus character select builds its panels from a record the game keeps between visits,
  gmSelCharData (`[0x805A00E0]+0x10`, `0x90180B40` in both games; each port at `+0xB8 + port x
  0x5C`: `+0` the character, `+1` the player's state, 0 a human, 1 a CPU, 3 nobody). While building,
  the select reads each port's state (`0x80685AFC`) into the player area's kind (`+0x1B4`: 0 empty,
  1 human, 2 CPU). It saves its panels back into the record only as it goes on to the stage select
  (`0x806878D4`), never when B leaves to the menu.
- Vanilla Brawl resets the record's players whenever Versus starts from the main menu; Project+
  doesn't. So in Project+ a CPU from a local game that reached the stage select came back on the
  online selects and into the match (a three-way game 1, or a CPU on panel 2 the joiner could never
  take). In Brawl, A on an empty panel's player-type button made a CPU on With Friends' select.
- The stage select's end copies the record into the fight's players (sqVsMelee, `0x806DCF6C`, every
  port whose state isn't 3), overwriting the fight's own data.

**What is online** (`OnlineSeats::Online`, a pure function of memory and the synced ports): the match
block's header locks, or the main menu's pick kept in sqVsMelee (`+0x18`) is With Friends (24-27),
Casual (30) or Ranked (31), or two or more ports are plugged in (a friend dropped in, wherever the
host was).

**What Orca writes there** (the frame hook, first runs and re-runs, only bytes that differ):

- **The record**, only between two scenes (`scMemoryChange`): a CPU's state becomes nobody's (3),
  and so does a human's on a port nobody plugged in. The select then starts with only the players
  who are there. Never on the stage select or in a fight: by then the copy into the fight is made,
  so no fight loses a player to this. Characters stay; only states change.
- **The panels**, on the character select: a CPU's (kind 2) becomes empty (0), which the select
  saves as it leaves, so the CPU plays no fight. A human's panel whose port nobody plugged in (a
  friend who left) is left to the game, which empties it 17 frames after the controller goes.
- **The join** (the input gate, `OnlineSeats::PressesJoin`): a panel a CPU had can't be joined by
  moving the hand into the grid, since its token went with the CPU. The game's own way back is A on
  the panel's player-type button with that port's controller in. So on an online select without a
  header, Orca presses A for a plugged-in port whose own panel is empty, its token neither in the
  hand nor flying, while its hand rests on that panel's player-type button (hand `+0xB0`). A friend
  who drops into a select whose CPU was on their panel takes it at once.
- **The buttons** (the input gate): where no header locks (With Friends, a friend's drop-in, Casual
  or Ranked before the queue's header lands), A never reaches a player-type button (`0x1D`) or
  another panel's name button (`0x1C`), nor any hand below y = -16.4 (the buttons start between
  -17.4 and -18.3 in both games, and a hand moves at most 1.0 a frame). An unreadable hand gets no A
  either. The hand's own name button and its list take A (`ux=21`, [Each player's own
  controls](#each-players-own-controls)). A fresh
  panel needs no A: a hand moved into the grid joins it. The gate reads the plugged ports from the
  friends bytes the frame hook keeps (`MatchBlock::FRIENDS_SEEN`).
- **The seats**, on any character select: an empty panel (kind 0) whose port nobody plugged in is
  hidden while the select is online, except the opponent's seat of a queue select (panel 2: always
  in a queue room, and on the queue's own select while it searches). Each panel is player area i's
  39 models (MuObjects at area `+0xB0..+0x14C`; each one's nw4r `ScnMdl` at `+0xC`, its `ScnObj`
  flags at `+0xCC`, `0xA0000000` in both games). Orca sets `0x60` (draw neither opaque nor
  translucent) on the hidden ones and clears it on the others, only on a model whose flags are
  exactly the game's (with or without those bits) and whose first word is a vtable in the
  executable. The select never writes those flags and rebuilds them with itself, so nothing
  outlives it. Hiding is drawing only: a hidden panel's logic and buttons stay, and the gates keep
  them dead.

So the queue's own select shows the player alone while picking, then the opponent's seat
(SEARCHING) once it searches; a queue room shows both players; With Friends shows the players
plugged in. Forcing the game's own 2-player layout (task `+0x648`, `0x806834D8`) was rejected: it
changes game logic, is fixed when the select starts, and can't serve a 3-4 player friends room.

**Determinism:** every write is a pure, idempotent function of emulated memory and the session's
synced ports, at the frame hook (the record and panels after the online rules, the seats after the
native text); the gate's masks and its join press read memory alone. A joiner loads the host's
keyframe, which carries all of it.

**Test inputs.** The default two-player inputs (`bf-mario-link-results.txt`, `pplus-1v1.txt`) join
with A on the player's own player-type button. In a session two ports are plugged in, so the select
is online and that A is masked: each player joins when the stick takes the hand into the grid.
Two-Orca runs of these inputs therefore play differently from a pre-`ux=19` build's from the first
character select on. Harness-only runs (one port plugged in, a local select) are unchanged, so the
release hashlogs still compare. Harness knobs: a walk that plays ports 2-4 on an online select needs
them plugged in (`ORCA_UX_TEST_NAMES`, and `ORCA_TEST_PADS=4` for ports 3 and 4);
`ORCA_UX_TEST_PADS_FOLLOW_PLUGS=1` gives a port nobody plugged in no controller, as a session does;
`ORCA_UX_TEST_UNPLUG_AT=<frame>:<port 2-4>` unplugs a port from that frame, as a friend's leave
does.

Tests: `OrcaOnlineSeats.*`; `Tools/orca/versus-cpu-dropin.py`; `queue-e2e.mjs --probe
Tools/orca/inputs/pplus-cpu-record-probe.txt` (a queue match with CPUs in both players' records);
inputs `pplus-cpu-online.txt`, `bf-cpu-friends.txt`, `*-cpu-dropin-live.txt`,
`*-friend-leaves.txt`.

## Ranked sets

A ranked set is followed in the game's own memory, so both Orcas, every re-run and every keyframe see
the same score (`Orca/UX/RankedSet.*`, `SetBlock.*`). The room hears each game from there once its
frames are final.

| Offset | |
|---|---|
| `+0x210` | Games recorded, wins per port, done (1 won, 2 void), winner, last game's winner, tiebreak next, ties in a row |
| `+0x218` | The fight being followed: flags (between scenes, sudden death, time ran out), ledge grabs, first frame (the game's id), stocks and percent at time-up |
| `+0x230` | 8 game records of 12 bytes: first frame, winner, how, stage, characters, stocks left, ledge grabs |

**Following a fight** (Brawl): `RankedSet::ReadLiveFight` reads each fighter through `sora_melee`'s
`g_ftEntryManager` (stocks, damage, status; statuses `0x73`-`0x75` are catching or holding the
ledge) and counts ledge grabs; stocks and percent are kept as they were when time ran out. Brawl's
sudden death stays in `scMelee` (fighters vanish and come back with 1 stock each).

**The verdict:**

- Not exactly two humans on ports 1 and 2, or a no contest: no game.
- **Brawl (Supernova 2025):** a KO is the results screen's winner. Out of time: the ledge-grab limit
  first (over 35 loses, Meta Knight over 20; 11 and 6 in a tiebreak game), then more stocks, then
  lower percent. A tie (a simultaneous last stock, or a level time-out) makes the next game a
  tiebreak with 1 stock and 3:00. Five ties in a row void the set.
- **Project+ (2024):** the results screen's winner (its codeset already handles time-outs and
  overtime).

Two wins end the set; 8 games without that void it. Other tracks read the score, the last winner
(the loser picks next), the stages each port won on (for Dave's Stupid Rule) and the tiebreak flag.

Tests: `OrcaSetBlock*`, `OrcaRankedSet*`, `OrcaResults*`. Not checked live: a real ledge-grab count.

### Rulesets are data

A ruleset's numbers and lists are a data file, not C++ (Orca split S5): the legal stages (name, the
game's stage id, starter or counterpick), the stage steps' timers and the winner's later step, DSR,
the Ice Climbers and Meta Knight clauses, the set (wins, stocks and minutes, who decides a time-out,
the ledge-grab limits, the tiebreak game, how many ties void the set), the queue's character
select (its ready timer, the B hold that skips), the ranked character order's turn time and the
ranked character select's no-show timer. The builtin ones are `Orca/Games/Brawl/Rulesets/brawl-supernova-2025.json`
(the match block's ruleset 1) and `pplus-2024.json` (2), compiled in byte for byte
(`Games/Brawl/Rulesets.*`).

Every ruleset is read the same way (`Orca/Engine/Ruleset.*`): a strict JSON reader
(`Engine/StrictJson.*`: one value, valid UTF-8, JSON's own number grammar with only exact integers
as written, no duplicate keys, at most 16 KiB and 8 levels), then the one schema, `Tools/orca/mods/ruleset.schema.json`, compiled in
and interpreted by `Engine/Schema.*`, then the few rules a schema can't state (its `x-orca-rules`).
Anything unknown, out of range or of the wrong type refuses the file; nothing is defaulted or
clamped. The schema uses a small subset of JSON Schema in which every value is typed and bounded,
and the interpreter refuses a schema outside it, so the site's validator (YouGame
`web/src/lib/orca-data-schema.ts`, `orca-ruleset-validate.ts`, with a byte copy of the schema) and
Orca can't drift: `Tools/orca/mods/ruleset-vectors.json` holds the cases both must agree on
(`OrcaRuleset.Vectors` and the site's `orca-ruleset-validate.test.mjs`). A libFuzzer target for
the reader is `Tools/orca/fuzz/build-ruleset-fuzz.sh`.

Changing a builtin file changes what both games write, like a code change: it needs the hashlogs
and, for a ruleset already played online, a compat bump. A build whose builtin files don't load
plays no ruleset at all (no locks, no queue or ranked caps), and logs why. A pack can't carry a ruleset file yet (the
pack format names none).

## Stage select

### Ranked steps

`Orca/UX/RankedSteps.*` (namespace `Ranked`): pure functions of a ruleset and the set so far, shared
by both games. Stages are the ruleset's legal list, starters first, as bit masks.

- **Game 1:** the coin's port strikes 1 starter, the other strikes 2, then the coin's port picks one
  of the last two. The coin is one bit of an FNV-1a hash of the room code (the server makes the code,
  neither player picks it).
- **Later games:** the last game's winner strikes 2 (Project+) or may ban 1 (Brawl, optional: Y
  skips), then the loser picks, never a stage they already won on (full DSR; dropped if it leaves
  nothing).
- **Brawl only:** the Meta Knight clause (exactly one Meta Knight, character `0x18`: the other player
  picks any legal stage, over strikes, bans and DSR) and Ice Climbers never on Final Destination.
- **Timers:** game 1's steps 30, 30 and 10 s, a later strike, ban or pick 30 s, each with 3 s of
  grace at 0:00. When one runs out, the remaining strikes fall on the first allowed stages, the ban
  is skipped, a pick takes the first allowed stage.

**Brawl** (`Orca/UX/BrawlStages.*`): stages the current step doesn't allow are drawn as plain grey
tiles. The page (Brawl or Melee stages) is both players' view, so only the player whose step it is
turns it with A on the page button (either, on a casual game's step both take at once). The flow's
own picks (the last starter, a timer's default) move the game's cursor there (via the page button
for Pokemon Stadium on the Melee page) and press A until the game takes it. A stage
taken against the steps is replaced before the screen copies it out. The stage select task is the
scene's `+0x3AC`: `+0x200` the cursor (`+0x3C`/`+0x40` x and y), `+0x224` the state, `+0x228` the
page, `+0x244` the hovered item, `+0x258` the taken stage (a write before the screen leaves changes
the match's stage).

**Project+** (`Orca/UX/RankedPPlus.*`): outside the stage select, `RSS_EXDATA` (`0x8042C4E8`) holds
the 2024 Proposed preset. On the stage select the hook writes Project+'s own Stage Striking table
(`0x8042C822`) as the block calls for, and sets `PAGE_INDEX` to `0xFF` so Project+ redraws the struck
art; Project+'s own check then refuses a struck stage. In every Project+ session the character
select clears a My Music step (`0x80002810`, Project+'s `MusicSelect.asm`) left from an earlier stage
select: step 1 hid every tile and turned the timer's pick into My Music, a softlock. The log says
`cleared a leftover My Music step` when it does.

### Casual stage pick

Both players at once, 15 s: each player's A on a legal stage under their own cursor names it, B
takes it back. The same stage plays at once; two different ones, a coin takes one the moment the
second player names theirs; a player who named nothing gets a random legal stage. The coin and random
stages come from `CasualSeed(room hash, coin, game number)` through murmur3's finalizer, so every
machine and re-run computes the same. Tests: `OrcaRankedSteps.*Casual*`, `OrcaBrawlCasual.*`,
`OrcaRankedPPlus.Casual*`.

**Random.** A on the game's own Random button names Random (`Ranked::PREF_RANDOM`, Project+'s block
`0xFE`): that player's random legal stage, the one a silent player gets at the timer
(`CasualDefault`, never FD with an Ice Climbers), but at once. Random against a stage, the coin
takes one of the two; Random against Random, the stage is picked at once. The block keeps the
marker, not the stage it draws, so the other player only sees "bo wants Random"; the result says
"Smashville (random)". The flow then takes that stage's own tile; the game's Random and its random
number generator are never used: Project+'s A waits until the game's hover has left its Random
button (the hover follows the cursor a frame late), as Brawl's waits for the hover to read the
stage. The cursors start just above the Random button, on nothing (no tile, Random or page button),
so an A mashed into the stage select names nothing, as before Random; every button starts as held,
so one held through the screen change counts once it is let go. Ranked has no Random (neither
ruleset has a random counterpick): A there does nothing, and the hint line says "Random is casual
only". The Random buttons' rectangles (`StageCursors::Layout::random_button`) were measured from
screenshots, not swept like the tiles. Friends rooms keep the game's own stage select, Random
included.

### Two cursors

In queue matches each player has a cursor of their own on the stage select, moved by their own stick
and drawn by Orca's overlay in the player's colour (`Orca/UX/StageCursors.*`). Friends rooms and solo
play keep the game's own stage select.

- **A on a stage proposes it**; B takes the proposal back. When both propose the same legal stage in
  play this game, the match starts there at once, whatever steps are left.
- The steps work on the turn's player's own cursor: X strikes or bans, Y skips Brawl's ban, the
  picker's A picks.
- The game's own cursor is nobody's: the gate masks every port on the stage select, and the flow
  writes the cursor's position (where the cursor that moved last is) and is the only one to press A.
- **Determinism.** The cursors are match block bytes (`+0xF4`-`+0xFF`, `+0x1B4`-`+0x1BF`; positions
  in 1/16 of the game's cursor units). The latch moves them from the raw sticks (dead zone 24 of 128,
  integer math) at every pass over a boundary; the frame hook takes presses as edges of "now" against
  "seen", so a re-run sees none.
- Tile rectangles and the cursor-to-screen map were probed in each game (`StageCursors::Layout`).
  The game clamps its own cursor to its range, so it can't be hidden; the overlay draws the
  follower's crosshair over it.

**Whose turn it is** must be obvious. The acting player's cursor blinks (2.5 beats a second, under
the 3-flashes-a-second limit) with a tag ("YOUR TURN", "X STRIKES A STAGE · 2 LEFT", or "BO'S TURN"
on the other screen) and the turn's timer; a waiting player's cursor goes grey. Under the Meta Knight
clause the stage select's first 3 s say so ("META KNIGHT CLAUSE"). Overlay only.

Not yet: the two cursors over a real two-Orca room and between a Mac and a PC; a keyframe mid-select.

## Character select

### A only in the grid

On a later game's character select the hands come back on the bottom panels, each on its own
player-type button, where A turns the player into a CPU. So while a header locks, the gate passes A
only when the hand is in the character grid or on its own token (`OnlineRules.h`). Without a
header, With Friends and a friend's drop-in keep A off the player-type buttons and other panels'
name buttons ([No CPUs online](#no-cpus-online)).

- Player area i is the select task's (scene `+0x400`) `+0x44 + 4i`. `+0x1A8` is its hand: `+0x80`
  what it points at (2 or 7 the grid, 3 its own token, 6 and 8 the token moving, 4 a leave button,
  1 another button), `+0x90`/`+0x94` its x and y.
- A acts where the stick has just moved the hand, and the gate decides from the state before the
  frame, so it passes A only for y strictly inside the grid less one frame's travel: -3.0 to 15.0 in
  Project+, -4.2 to 15.0 in Brawl (Brawl's bottom row, with RANDOM, reaches -4.07).
- **A on BACK** on the queue's own character select backs out, as held B does there; while the
  player is searching, the first frame of that press un-readies and the next is the game's A.

### B only takes the token back up

Both games tell the two B's apart by the token: a B pressed with the token down puts it back in the
hand at once; B held from there backs out to the menus after 31 frames. So while a header locks the
gate passes B only on the first frame of a press, with that player's token down (`CssBUnpicks`): it
takes the token up, and a held B never backs out.

### Character order

Later games of a ranked set pick in order (`Orca/UX/CharOrder.*`). `CharOrder::kLaterGameOrder` is
one constant:

- **`WinnerFirst`** (the default, as in the standard rulesets): the previous game's winner picks
  first, the loser sees that pick and picks second, 45 s each.
- **`Free`**: the loser is asked first; the winner may lock in at any time.

A picks (puts the token down), B takes it back up, **Start locks the pick in** (read from the raw
latch; the game never sees that Start). Off-turn players' controllers do nothing. When the second
player locks in, the stage select comes at once. The tokens stay down between games, so Start alone
keeps last game's character. A player whose turn ends with the token in hand has no input until it
is down: Orca holds their stick, brings the hand back to where it last held the token over a
character this order (`+0x1F4`-`+0x1FF`), and puts it down there. The next turn starts only once it
is down, so the second picker always sees the first pick, and nobody shows as locked in with a token
in hand. A token still in hand 45 s after both turns makes its player the ranked no-show, who leaves
the set. Game 1 has no order (both pick
at once). L or R held as you lock in
carries into the game (Zero Suit Samus, Sheik, Project+'s Nana-led Ice Climbers): kept as the turn
ends and held for that player while the select ends. State: 16 bytes at `+0x200` (magic `YGCO`, the
step, the first picker, the lock-ins, the step's start frame, the kept L and R at `+0x20E`).

Not yet: Supernova 2025's exact order (ban, the loser's stage, then characters).

### The queue's character select

Casual and Ranked land on a character select where the player sees only their own panel, picks and
presses Start; only then does the page search (`Orca/UX/Queue.*`, the session's part in
`Rollback/OnlineMatch.cpp`).

| stdout (with `caps queue2`) | When |
|---|---|
| `orca queue ready <casual\|ranked> <char> <costume>` | A character down and Start pressed: search with this pick. Printed again with a new pick or when back after a room. `<char>` is the select's id (`0x28` none, `0x29` Random). |
| `orca queue unready` | B, the token taken up, or `queue-cancel` while ready. |
| `orca menu cancel` | Backed out to the menus (held B, or A on BACK; while ready, the B that un-readies backs out once held about 1 s). |
| `orca queue skip` | Casual only: B held 1.5 s on the room's select (its first frame does what B does there). The page leaves and searches again. |
| `orca queue timeout me\|them` | Casual's first game: the 30 s ready timer ran out. |

The page owns the room: Orca never leaves on a skip or a timeout by itself.

- **The own select.** The header carries `FLAG_SOLO`. Ports 2-4 are masked; Start never reaches the
  game (it readies through the latch); while ready, a B tap only un-readies, and the same B held
  `SOLO_LEAVE_FRAMES` (30) reaches the game, which backs out as for any held B (`TIMER_START` holds
  when that B began, its solo meaning). So hold B leaves the own select, searching or not.
- **Hold B to leave** (2026-10-11, `ux=29`). Leaving moved from Z to B: casual's skip, set over's
  leave and the own select's back-out are all a held B. Z is the game's on every queue character
  select, so Project+'s AltZ costume works (hold Z as the select ends: in a room, through Orca's
  Start at the go). Ranked's results screen keeps B from the game (`GateMasks`). The first time a
  player reaches the own select, the control guide shows "New · Hold B to leave" for 6 s (gone on
  Start or a held B); once on screen 2 s, or once B is held, `hold-b-leave` goes into
  `<User>/Config/OrcaSeen.txt`, beside the kept controls, so it never shows again on that machine.
- **The pick's identity.** At ready, Orca keeps the pick (character, costume, where the hand placed
  it) and the page's `queue rating`, as a 13-byte queue identity that rides the session's value
  channel, so every machine has the joiner's at its plug frame. The place is learned from a hand
  over that same character (`Queue::PickPlaces`, kept per character for the run); a token that was
  already down (the queue image after a ranked set) teaches nothing, and a pick with no known place
  goes out with no pick, so the player picks by hand rather than having the hand parked on another
  square with their controller held for 10 s.
- **The room's select.** Game 1: the host's pick goes back on it after its fresh start, by its own
  inputs ([Fresh starts](#fresh-starts)), and locks once down; the joiner's pick is put in from its
  identity by the input gate (its stick held centred for 20 frames, since the game takes a newly
  plugged pad's stick as its origin, then steered to the place, A once the hand has stopped, X until
  the costume matches; about 1.5-2 s, 10 s at most). Then a 30 s ready timer; both ready, Orca
  presses Start. Casual's later games have no timer: players can talk; ranked's go through the
  character order.
- **Random** is a pick like any character (`0x29`); the game resolves it from its own RNG, which is
  emulated state, so both machines resolve the same character, and starts from each history's own
  seed, so it differs from match to match ([Random numbers](#random-numbers)).
- **Ranked has no skip** (`Queue::MaySkip`): a ranked match must be played.
- **After the room** (a skip, a timeout, the opponent gone, a set over), the session loads the
  **queue image**, the player's own game as it was when `host` or `join` came (a machine image and
  the NAND, 20-30 ms to capture), as the state of the next frame, and opens a room of its own. The
  player is ready again unless their own timer ran out or a ranked set was played.

Determinism: the region `+0x1C0`-`+0x1EF` (ready flags, timer, game 1's locks, the raw latch's
buttons, the joiner's pick and steering), advanced at every boundary by `Queue::Advance` as a pure,
idempotent function of memory, the frame number and the synced ports. All steering arithmetic is
integer ([Determinism across hosts](#determinism-across-hosts)).

### The results screen's replay

The results screen's Z saves a replay, which a session can't (no save file, no writable SD card). Z
opened a picker that A and Start never leave, so a player mashing A with Z after a game was stuck on
it for good. Every session's input gate masks Z on the results screen (`Rules::SessionMasks`).

## On-screen UI

Everything Orca shows should look like the game drew it. Two ways:

- **Native text** (`Orca/UX/NativeText.*`): where the game has a text box on screen and the words
  are the same for both players, Orca writes them into the game's own message buffer, so they are
  drawn in the game's font and place. A buffer is a window's set-up codes, then UTF-8 words between
  inline codes; Orca replaces only the words (ASCII only: the font lacks the middle dot and en
  dash). Built for Brawl's name plates and rules bar on the Versus character select (task `+0x538`:
  "Game 1: ADA locked in, BO to press Start 0:24"), and Project+'s name plates. Always third person,
  so both machines write the same bytes; this is game memory, so it is in the compatibility key.
- **The kit** (`Orca/UX/Kit.*`, `Widgets.*`): everything else is drawn by the host over the picture,
  local display only, in the game's own fonts (read at run time from the player's disc,
  `system/font/`, never shipped) and the look of that game's menus. Without the disc it falls back to
  ImGui's font. The fonts' two textures are freed as the video backend shuts down
  (`OSD::SetHostOverlay`'s release function, called first in `VideoBackendBase::ShutdownShared`).
  Up to 0.3.38 they lived until process exit: on D3D11 they held 4 device references ("Unreleased
  references: 4"), so the device and the graphics driver outlived `D3D::Destroy`, which unloads
  d3d11.dll, and a driver thread calling back into it ended Orca with `0xC0000005` a moment after
  the game (12 of 132 embedded or jailed exits on an NVIDIA PC); a standalone window, which exits
  through the CRT, crashed every time in their destructors. `D3D::Destroy` now also keeps d3d11.dll
  loaded if anything still holds the device.
- **Relabelled textures** (`Orca/UX/Relabel.*`, `CssTitle.*`): the game's big words (READY TO
  FIGHT!, STAGE SELECT, the character select's BRAWL / VERSUS title) are textures. Orca changes a
  value nobody sees (a transparent palette entry's blue, or one texel's high nibble) so the texture
  hashes to another name, and Orca's texture pack has a label for each: PRESS START TO LOCK IN!,
  WINNER PICKS FIRST!, P1 STRIKES 2, CASUAL, RANKED, FRIENDS. Without the pack the game's own words
  show, unchanged to the eye.
- **YouGame's layer** (`Orca/UX/YgCard.*`, `YgOrb*`): chat and room notices in YouGame's own look and
  Roboto (`Data/Sys/Orca/Fonts`, Apache 2.0), deliberately not the game's.

**Set end** (`Orca/UX/SetEnd.*`): VICTORY or DEFEAT with the set's score and the rating counting from
the old number to the new one; casual's per-game YOU WON / YOU LOST; "<name> disconnected · They
forfeit in" with a draining ring that follows the room's 15 s grace (or the 10 s silence limit); "You
left · This set counts as a loss". Before the set's first game began (its fighters in) the room voids
a leave instead: "No contest in" and "This set doesn't count". After a ranked verdict the room stays
open (**set over**): both games keep the session on the results screen with the page's chat; each
Orca leaves the room when its results screen ends, when the player holds B for 1.5 s, or, with the
opponent gone, on Start. A
ranked set's player comes back to their own character select not ready, so the page doesn't search
by itself; a Start still held from the results screen readies nobody until it is let go.

`ORCA_UX_KIT_DEMO=1`, `ORCA_UX_SETEND_DEMO=victory|defeat|countdown|casual|left|stay` and
`ORCA_UX_OVERLAY_DEMO=1` draw these for design checks; with `ORCA_UX_SETEND_DEMO_AT=<frame>` a harness
run shows the same moment at the same frame.

Not yet: Project+'s rules bar (its message has no width code, so a longer line isn't drawn; the
overlay docks its lines on the bar instead), native words on the results screen, a native LOCKED IN.

## Embedding

**Private live broadcast sink (v1).** The desktop app may set `ORCA_BROADCAST_SINK=<file>` on a
product replay (`ORCA_REPLAY_SOURCE`, a file or ticketed watch URL). Only a product replay player's
lifetime enables the sink; setting the sink on a playing machine exports nothing. Finish and
destruction disable it. The app creates the private file in its run directory, initializes every
page, grants this single file read-write, then launches YG Dolphin. No listener, loopback, screen,
microphone, or system audio source is used. The sink never records media for later playback.

`Common/BroadcastRing.h` defines the little-endian file ABI, 11,186,624 bytes. Its 256-byte header
has u32 magic `0x31424759` (YGB1), version 1, width 1280, height 720, fps 30, rate 48000,
channels 2, video slots 3, audio slots 64, video bytes 3686400, audio frames 480, total size, at
offsets 0 through 44. Video/audio latest tickets, video/audio drop counters and state are u32 at
64/68/72/76/80. State is 0 waiting, 1 active replay, 2 stopped. Other header words are reserved.

Three video slots start at 256, stride 3,686,464. Audio starts at 11,059,648, with 64 slots of
stride 1,984 (10ms each). Each slot has a 64-byte prefix: u32 sequence guard, payload byte count,
timestamp low/high words, and sample count at 0/4/8/12/16. Timestamp is steady-clock microseconds,
shared by video and audio. Count is 1 for video, 480 for audio. Video is top-first RGBA8, audio is
interleaved little-endian signed 16-bit L/R, tapped after every mixer source and resampled to 48k.
Both rings are single producer; they never wait for the reader. Nonzero u31 tickets wrap from
0x7fffff80 to 1, slot `(ticket-1)%slots`. Writers mark the guard odd (`ticket*2+1`), write, mark it
even (`ticket*2`), then publish latest. Readers require that same even guard before and after the
copy; a mismatched guard drops the copy. Video consumes newest only, audio consumes retained
chunks in order (desktop caps catch-up at 100ms). Independent rings avoid a mixer/render lock.

Video uses three GPU staging textures and one bounded CPU writer job beside FrameDumper. It
polls `TryFlush`, scales the game XFB to 720p, and drops busy/stale work (100ms) without waiting
on the GPU or writer. Metal, D3D11, D3D12, Vulkan and persistent-buffer OpenGL supply nonblocking
readback. Other backends drop frames. Normal disk frame dumping retains its original behavior.
The lead must measure CPU/GPU load, output fps and drops, A/V drift over a long match, and output
with the window minimized. Render/audio shutdown and readback orientation on each backend need
runtime validation; none of these code changes are a substitute for those checks.

The YouGame desktop app shows Orca inside its own window, at the page's player box, using the same
line protocol as its libretro host. Code: `Source/Core/DolphinNoGUI/Embed.*`, `Platform.cpp`
(commands), `PlatformWin32.cpp`, `PlatformMacos.mm` and `PlatformX11.cpp` (the window). Test hosts:
`Tools/orca/embed-test-host.m` (macOS) and `embed-test-host-win.cpp`.

```
Orca --embed --parent <handle> --rect X Y W H [--embed-adopted] [-u <user>] -e <disc> [-C ...]
```

`--embed-adopted` is the sandbox's way to embed on Windows ([Adopted window](#adopted-window)).

Self-embedding (`--embed --parent`, macOS, X11 and unjailed Windows) prints one decisive stdout
line: `orca embed ok` after the first applied rectangle is visibly in place, or
`orca embed failed <reason>` when embedding fails and it falls back to its own window. The reason
is exactly one token: `parent-gone`, `setparent`, `no-parent`, or `track-failed`. Only the first
result is sent. A result decided before `ready` is held and printed immediately after `ready`;
a later result is sent on the next main-loop pass. `--embed-adopted` never sends these lines:
the jailed Windows launcher reports adoption itself. Parent loss after successful embedding
still stops the run as described below.

**Replay playback.** Use the same game, disc/profile, session (`ORCA_SESSION=1`) and embed launch,
with `ORCA_REPLAY_SOURCE=<local file path or ws(s) watch URL with ticket>`. YG Dolphin boots to its
local canonical origin and plays the stream instead of joining or creating a room. A saved regular
file (at most 1 MiB) plays at real speed. For watch playback the app also sets
`ORCA_REPLAY_ROOMS_ORIGIN=https://<rooms host>[:port]` for its selected environment. Replay has no
room-ticket reply from which to learn that origin. Watch URLs use `wss://`, have no userinfo, and
match that host (case-insensitively) with either the default port or the origin's port. No origin
means no watch connection. Test-knob builds may also use `ws://` for `localhost` or `127.0.0.1`
with a matching explicit origin. Pending watch bytes and reset offsets share an 8 MiB limit.
A watch URL follows live, 12 frames behind the newest final
frame, catches up after joining late, and drains the tail when the stream ends. The
`ORCA_TEST_REPLAY_*` knobs still work when no product source is set and test knobs are allowed;
they do not alter product playback.

`ORCA_REPLAY_NAMES` is the complete view-time name map: alternating zero-based GameCube port and
`encodeURIComponent` UTF-8 name, e.g. `0 Alice%20Smith 2 Ren%C3%A9`. At most four entries (ports
0..3, no duplicates), each 1..24 code points, with chat's ban on controls and line separators.
An empty map is valid. Every loaded port's name is replaced; a missing port shows `Player N`
(one-based N). The reader keeps its original metadata for reconnect overlap checks. Brawl's
in-game tags retain their existing five-character ASCII conversion; unsupported glyphs use its
port label. Worn tags are renamed on the character select when the view map changes. Controls
and queue metadata remain those of the recording. Names change guest RAM, so recorded full-RAM
checksum comparisons are retained in test playback and in a player's own product replay without
a view-time name map. Playback with a view-time map still validates stream hashes, compatibility,
the local origin, and reconnect overlap.

The desktop bridge is `window.yougameDesktop.orca.watchReplay({ bytes?, url?, names, launch? })`.
Supply exactly one source: the page fetches a saved replay with its own session and passes an
`ArrayBuffer` of at most 1 MiB, or supplies the live ticketed watch URL. Main creates a private,
unique file in the app's temporary replay folder, grants that one file read-only in the sandbox,
and removes it after exit or failure. `launch` supplies the usual play metadata (game slug, disc
key, rectangle and optional graphics); without it, the last Play's metadata is reused. Every
replay has a fresh session. The promise resolves on exit, as `play` does. Main automatically
answers `caps replay` for this API; a page may answer the caps again through `send`.
`on("replay-time", cb)` and `on("replay-done", cb)` return unsubscribe functions. Time events carry
`{ frame, ms, slug, session }`; done events carry `{ ok, frames, why, slug, session }`.
`pauseReplay()`, `resumeReplay()`, `seekReplay(frame)` and `replayNames(names)` use the same filtered
stdin as `send(slug, session, line)`. The page-side `fetchReplayBytes(url, init)` helper preserves
session request headers/cookies and bounds the streamed response before passing bytes to main.

- `--parent`: on Windows the HWND as a decimal number; on macOS the window's **CGWindowID** (in
  Electron, the middle field of `win.getMediaSourceId()`), since an `NSView*` means nothing to
  another process; on Linux the X11 window id (Electron's `getNativeWindowHandle()` when the app
  runs as an X11 client: the YouGame app always does, through XWayland on a Wayland desktop).
- Orca never shows dialogs while embedded: alerts go to stderr. stdin must stay open; a closed stdin
  means the app is gone, and Orca quits. When the app's window goes away without a `quit`, Orca
  stops too.
- Fd 1 is kept for the protocol; everything else Orca or Dolphin prints goes to stderr.
- The netplay and rollback logs are on at INFO. The boot logs the app turns on with `-C` after boots
  that died before their first frame (OSREPORT, IOS, IOS_SD and the memory map's MI) go off at the
  game's first frame, saying `Orca: boot logs off at the first frame: <names>` (`Orca/BootLogs.h`):
  past it they tell nothing about the boot, and Project+ reads its SD card all game, so they logged
  about 0.7 MB a minute of play (Brawl about 50 KB). A category the player's Logger.ini turns on
  stays on.

**Coordinates** are physical pixels (the box's CSS pixels times `devicePixelRatio`). On Windows
`rect` is relative to the parent's client area and Orca's window is a `WS_CHILD`, so it moves and
clips with the parent. On macOS `rect` is relative to the parent's frame (title bar included; the app
adds the title bar's height to Y), and Orca polls the window's bounds at 120 Hz. On Linux `rect` is
relative to the parent X window (the app adds its menu bar's height to Y) and Orca's window is its
child, so it moves, clips and hides with it as on Windows. The app sends `rect` only when the box
moves inside the window.

`ORCA_EMBED_TRACE=1` is a diagnostic switch: each applied `rect` or `view` command logs
`embed-trace t=<steady-clock ms> req=<x,y,w,h> real=<x,y,w,h> scale=<s>` to the normal log.
`req` is the received rectangle (`view off` uses the current window rectangle); `real` is our own
window's actual screen bounds immediately afterward: macOS screen points (Cocoa origin), Windows
physical pixels, X11 pixels. `scale` is the window's backing scale (1 on X11). Small `rect` commands
log their raw request and the unchanged bounds after hiding the window. Unset, the switch adds no queries or logs.

### stdin

| Command | Effect |
|---|---|
| `rect X Y W H` | Move and resize Orca's window. W or H under 32 hides it until a big enough rect. |
| `view X Y W H` / `view off` | The box the game's picture is laid out in, when it differs from the window: the page cuts the window (`rect`) to make room for its menu panel and keeps `view` the whole box, so the game neither shrinks nor moves. Send `view` before every `rect` once used. Orca's own overlay stays inside the visible part. |
| `dim N` | N% black over the whole frame, eased (the page sends `dim 50` while its menu is open). `focus` clears it. |
| `hide` / `show` | Take the window off screen and back. Hidden means no input. |
| `focus` / `blur` | Give the game the input, or give it back to the page. |
| `pause` / `resume` | Answers `state paused` / `state running`. In a session it pauses only while the player's game is alone, and ends by itself when drop-in needs the game; otherwise `unsupported pause`. |
| `volume V` | 0 to 1, this run only. |
| `perf off\|fps\|detailed` | The overlay's frame meter (frames shown in the last second and the longest; yellow under 59 fps or past 20 ms, red under 50 fps or past two frames). A frame the game itself holds in emulated time (Project+ loading between scenes) counts as the video frames it held, so the meter, `fps` and the `hi` hitch count in `orca stats` show only this machine falling behind; `hil` counts the frames a load alone made slow (`hi` + `hil` is the older builds' `hi`). |
| `music on\|off` | The player's Music switch ([Music switch](#music-switch)), cap `music`. |
| `gfx <key> <value>` | A live change to one of the player's drawing-only graphics settings ([Graphics](#graphics)), cap `gfx`. |
| `save N` / `load N` / `reset` | Refused in a session. |
| `prepare-join` | The host invited a friend: a queue or search under way ends (`orca menu cancel`). Since 0.3.34 no keyframe is made ahead: the friend's arrival may [fresh-start](#fresh-starts) the game first. |
| `join <code>` / `leave` | Into or out of a friend's game. |
| `host <code>` / `queue-cancel` / `queue rating <n\|->` | Matchmaking ([Matchmaking](#matchmaking-and-results)). |
| `direct on\|off` | The player's switch for direct links. |
| `record on\|off\|auto` | Whether this player's match streams to the room for watchers and the saved replay in the players' accounts, cap `record`. `auto` (the default) records ranked rooms only; a tournament or friends room records after `record on`. See `Rollback/ReplayStream.h`. |
| `replay pause` / `replay resume` | Product playback only, cap `replay`: hold the displayed replay position or continue. Prints `state paused` / `state running`. The window heartbeat and replay clock keep reporting while paused. |
| `replay seek <frame>` | Cap `replay`: absolute recorded frame in the history received so far, including its first frame and excluding the end. Restores the local origin and replays unseen to that frame, which is rendered. Keeps the pause choice. Out-of-range targets answer `unsupported replay`. |
| `replay names [<port> <name> ...]` | Cap `replay`: replace the complete view-time map, in the same format as `ORCA_REPLAY_NAMES`. No pairs clears it to the `Player N` defaults. |
| `delay auto` / `delay N` | Orca's default input delay (a fixed 3), or a fixed 1-6 frames. |
| `chat <name> <text>` | A line from the page's chat, shown while Orca fills the screen. Fields are `encodeURIComponent` of UTF-8 (name at most 24 code points, text 140). Never logged. |
| `controls <hex>` | The player's own controls ([Each player's own controls](#each-players-own-controls)); the tag gets them at the next character select. A bad profile answers `unsupported controls`. |
| `orb <x> <y> <size> <lit> <away> <badge>` / `orb blink` / `orb off` | YouGame's overlay button, drawn where the page's own stands under Orca's window. |
| `notice <kind> <name> <text> [<key> <verb>]` | One of the YouGame overlay's lines (join, leave, invite, ...). Never logged. |
| `talk listen\|send\|sent\|error <name> <text>` / `talk off` | Hold-to-talk's row, cap `talk`: one row after the notices, replaced by each line. Fields as in `chat`. Never logged. |
| `feedback` / `host-menu` | Cap `hostui`: request feedback with a game screenshot, or toggle the native host menu. Ignored without the agreed cap. The app relays these shortcuts only from its focused view. |
| `quit` | Stop at once and exit. |

Most commands are gated by capabilities (`caps`) that Orca and the app agree on; a later `caps` line
replaces the whole answer. A command without its cap, or a malformed one, answers
`unsupported <command>` (except unagreed `feedback` and `host-menu`, which are ignored). Any other line goes to stderr. Drawing
commands (`view`, `dim`, `perf`, `chat`, `orb`, `notice`, `talk`) are host-only: never in game
memory or the compatibility key.

`replay` is offered only with `ORCA_REPLAY_SOURCE`, after `talk` and `rumble` and before `record`
and `hostui`. The newest word, `hostui`, stays last so apps that read fewer words drop it first.
A ruleset profile normally offers 21 words (22 with `replay`), within the app's 24-word limit. Replay
mode withdraws `join`, `host` and `direct`; it never opens a seated room.

**Native host menu** (cap `hostui`). The app may handle `orca host ...` and `orca feedback ...`
and wants YG Dolphin's native host menu only when both sides list `hostui`. Desktop 0.13.0 / V2 web
will add this agreement; desktop 0.12.100, 0.12.43/44 and V1 web keep the 0.3.49 orb and overlay
behavior without it. With the cap, Δ appears beside the orb; an orb click or Up+Start opens
Feedback, Volume up/down, Mute, Exit fullscreen, Leave and Controls. Feedback captures at most
one screenshot per five seconds after completion. A later `caps` answer can enable or withdraw
the menu immediately; withdrawal closes an open menu and suppresses pending feedback output.

Without `hostui`, overlay and talk input follow these paths:

**Opening YouGame's overlay** prints `orca orb`: a left press on the button (Windows), Shift+Tab
while Orca has the keyboard, or, on a controller, holding the D-pad's Up and pressing Start
(`Orca/UX/OrbCombo.h`). From that Start until Start and the D-pad are let go the game gets neither,
and the masked pad is what is recorded and sent, so every machine plays it. Opening the overlay
never pauses Orca: the page sends `blur`, the player's pad goes neutral, and the match goes on.

**Hold-to-talk** (cap `talk`, offered before `rumble`, `record` and `hostui`). Without `hostui`, those two presses print their press and
release instead of `orca orb` (`Orca/UX/OverlayKey.h`): `orca overlay down keys|pad`, then exactly
one `orca overlay up keys|pad` (let go: Tab or Shift; Start, even with Up still held) or
`orca overlay cancel keys|pad` (no longer read: Orca's window lost the keyboard or was hidden; the
game lost focus, paused or is stopping). The page times them: let go within 250 ms is a tap (the
overlay), longer is a hold (it talks). A click on the button stays `orca orb`. The pad is read once
a frame, and while a frame waits on a friend (a stall, up to the 10 s silence limit) no frame reads
it, so each stall tick reads it again for the press alone (`UX::LocalPadWaiting`): Start let go
prints `up` within a tick. That read is not recorded, sent or timed, and the combo's mask advances
on frames only, so the bytes every machine plays are unchanged. Focus lost during a stall is handled
when frames resume (the input gate moves with the frames' pad poll), and the page's own blur
already ends its side. A release found more than 100 ms after the last read that saw Start down,
once the press is past 250 ms, prints `cancel`, not `up`: Orca cannot say when the player let go, so
a tap could read as a hold or a talk run on. On macOS and
Linux Orca's window never has the keyboard, so the page sees Shift+Tab itself. The masking is the
same as without the cap, so the compatibility key does not change. While the player talks the page
sends `talk listen <name> <text>` (to whom, then the live words, at most 4 a second), `talk send`,
`talk sent` or `talk error`, and `talk off`. Orca draws one row after the three notice rows,
replaced in place: listen and send keep the end of the words and stay until the next line (60 s at
most), sent fades after 3 s and error after 4 s, and while listening the button sends out a green
ring every 1.2 s.

### stdout

| Line | Meaning |
|---|---|
| `ready W H FPS` | The first frame is on screen (Brawl: `640 480 59.9401`). A session may wait in its lobby after this. |
| `state running` / `state paused` | The answer to `pause` and `resume`. |
| `unsupported <command> [N]` | A refused command. Not an error. |
| `key escape` / `key fullscreen` | Windows only, if Orca's window somehow has the keyboard. |
| `orca click` | Windows only: a mouse press on Orca's window, which the page never sees; the app focuses its page and sends `focus`. On macOS and Linux the press reaches the page. |
| `orca orb` / `orca overlay down\|up\|cancel keys\|pad` | The overlay key, as a toggle, or (cap `talk`) its press and release: see Hold-to-talk above. |
| `orca host volume-up\|volume-down\|mute\|exit-fullscreen\|leave` | Cap `hostui`: the native host menu's requested app action. Controls uses `orca orb`. |
| `orca feedback <filename>.png` | Cap `hostui`: a completed screenshot in the run's user directory, ready for the app's feedback UI. |
| `error <sentence>` | Something failed. Each `orca error` also gets an `error` twin. |
| `orca state <state>` / `orca error <code> <sentence>` | Session status (`Orca/Status.h`). Forward every line that starts with `orca `. |
| `orca menu ...`, `orca result ...`, `orca queue ...` | See [Online menu](#online-menu) and [Matchmaking](#matchmaking-and-results). |
| `orca controls <hex>` | The player's own controls: changed in the game, or loaded from Orca's file at boot. The page keeps them. |
| `orca active <interval_s> <active_s> <inputs> <mode> <pads>` | How much the player played: every 60 s while the game runs, and once more when it stops ([Activity lines](#activity-lines)). |
| `orca perf <json>` | This machine's speed: `info` once after `ready`, `win` every 15 s of running time, `auto` when the run switched its graphics to Performance ([This machine's speed](#this-machines-speed)). |
| `orca rumble <port> <0\|1> <adapter\|pad\|none> <index>` | The game's motor for the local player's controller, on change and every 30 frames while on, cap `rumble` ([Rumble](#rumble)). |
| `orca replay time <frame> <ms>` | Product playback position, about four times a second, including paused/live waits, plus initial and final positions. Absolute replay frame and whole milliseconds from its first frame, `max(0, frame - first) * 1000 / 60` rounded down. A seek may move both backward. For chat synchronization, not wall-clock time. |
| `orca replay done ok\|fail frames=<n> ... why=<text>` | Replay completion or failure; the existing diagnostic fields stay intact. `frames` counts executed replay frames, including seeks; `why=-` means normal saved-file completion. Main forwards this as `replay-done`. |
| `exit <code>` | Always last: 0 normal, 1 a reported error, 2 bad arguments. |

### Activity lines

`orca active 60 42 118 ranked 1`, every 60 s of wall-clock time on its own thread (stalls, loads and
a paused game don't hold it up), and once more for the rest of the interval when the game stops,
unless that rest is empty (under half a second, no input) (`Orca/Activity.h`). Since 0.3.35.

- When it starts: embedded, at the first frame after `ready`. Until `ready` the app takes every line
  from Orca for boot progress (`bootWatch.line()` in `desktop/src/orca.ts`), so a line printed by a
  boot that hangs before its first picture would keep its boot watchdog from ever stopping it.
  Without the app (a harness run), at the game's first frame.
- An app without a handler for it (desktop on 2026-10-08): `parseOrcaLine` makes it a status line,
  written to the run log and sent to the page as a `running` message that `OrcaEmbed` doesn't act
  on. It changes neither the company, `orcaError` nor the `orca state left` hook.
- For the app's handler: the last line comes after the game stopped, so after `orca state left` when
  the app stopped it, and today's app no longer passes a stopped session's lines to the page (it
  only logs them): read `orca active` before that check. Keep it out of `bootWatch.line()`.
  `interval_s` is wall time, pauses and a sleeping Mac included: count `active_s`. Like the page's
  own clock, the 30 s after an input run on when the game loses the input; the page only counts
  them while it is visible.

- `interval_s`: the seconds the line covers. `active_s`: those within 30 s after a real local input,
  the page's own rule (`web/src/lib/play-activity.ts`), never more than `interval_s`. Both are
  rounded to the nearest second.
- `inputs`: real input edges in the interval, on every local controller together. A button pressed
  or released, a trigger past half its travel (or its click) and back under a quarter, a stick out
  past 48 of its 127 and back under 28: analog noise, drift, a stick held still and a gamepad's
  stream of identical reports never count.
- `mode`: `ranked` or `casual` in a queue room, `friends` with a friend plugged in, `training` in the
  game's Training, else `solo`; the one with the most active time in the interval.
- `pads`: local controllers with at least one edge in the interval. Several (couch play, the
  keyboard pad) never multiply the seconds: one counts while any of them had an edge in the 30 s
  before it.

Only this machine's controllers count (every adapter port and gamepad in the app's stream, Dolphin's
own port 1, a harness script), read at the boundary of each frame that runs for the first time in
real time and only while the game has the input. Never a peer's input, a rollback re-run, a
joiner's rebuild or catch-up, a fresh start's tail or replayed pads. Observation only: nothing it
reads or keeps reaches emulation, the replay, checksums or the compatibility key.

### Rumble

Orca drives no motor: the app's controller helper owns the devices. With cap `rumble`, Orca tells
the page when the game turns the local player's motor on or off, and the page sends the helper its
`rumble` commands (`Orca/UX/Rumble.h`).

- **What it reads:** the local port's SI channel output, the command each poll sends, where
  PADControlMotor leaves `0x0040_03_0m` (m = 1 is on). Read at frame boundaries that are not re-runs
  or a joiner's catch-up. The register is in every snapshot, so a rollback's correction shows at the
  next live boundary and a re-run never buzzes the pad again. Nothing is written to emulated memory
  and no input changes: no `ux` or `kCompat` bump. Dolphin's own motor output (`Pad::Rumble`) is off
  in a session: it would follow the in-game port, not this machine's, and fire on every re-run.
- **Line:** `orca rumble <port 1-4> <0|1> <adapter|pad|none> <index>`. The device is the one Orca
  read the local pad from at the last first-run frame: the adapter's physical port (0-3) or the
  standard pad's index in the app's snapshot (as the app arranged it). `none` (Dolphin's own input, a
  test script) is never on.
- **Timing:** on at once, off after 3 live frames off, so the game's on/off patterns read as one
  buzz. The off goes out as soon as it is due; on lines are at least 6 frames apart, so at most 10
  on and 10 off lines a second. While the game holds the motor on, the same `1` line is said again
  every 30 live frames: the page holds the motor about a second after any `1`, so a long rumble
  lasts and a frozen Orca stops buzzing soon. Off at once when the game has stalled 100 ms on a
  friend and at the exit; off after the same 3 frames when Orca loses the focus.
- The player's switch is the game's own: each name tag's Rumble in Options > Controls.

### Focus

Dolphin reads input only while `Host_RendererHasFocus()` holds, and embedded Orca turns off
background input, so that rule always applies.

- **Windows:** Orca never takes the keyboard; the page sees every key and sends the keyboard as a pad
  in the controller stream. A click on the picture lands in Orca's child window, so Orca prints
  `orca click`. Input counts while the last command was `focus`, the view is shown and the app's
  window is in front. A child window in another process shares the app's input queue, so a stall on
  Orca's main thread stalls the app's UI.
- **macOS:** Orca never takes the keyboard or the mouse. Its window can't become key and lets clicks
  through (`ignoresMouseEvents`). Orca reads the keyboard from the HID state and pads through SDL;
  input counts while the last command was `focus`, the view is shown and the app is frontmost. The
  page sends `blur` when it wants keys for itself.
- **Linux:** as on macOS, Orca never takes the keyboard or the mouse: its child window has an empty
  input shape (the pointer passes through it, and through the GL child inside it, to the app's
  window) and selects no key or button events, so the page sees clicks on the picture itself (no
  `orca click`). The page sends the keyboard as a pad, as
  on Windows. Input counts while the last command was `focus`, the view is shown and the X server
  has the keyboard in the app's window (its focus events, checked twice a second too).

### Linux window behaviour

Orca's window is an X11 child of the app's window, in a 24-bit visual of its own (so the app's
window may have any depth), black until the first frame and raised above the app's other children
on every `rect` and `show`; Dolphin's GL context makes its own child inside it
(`Common/GL/GLX11Window.cpp`, which no longer discards the connection's queued events: it shares
the connection with the main loop). The app forces
itself onto X11 (`--ozone-platform=x11`), so a Wayland desktop runs both through XWayland. X errors
are logged to stderr instead of ending Orca, because the app's window can go away between two
requests that name it (the first 20, then one in a thousand); Orca stops when that window is
destroyed, as on Windows. While Orca's window is hidden or the app's window is not viewable
(minimized, another workspace: checked twice a second too), the presenter is told the surface is
out of sight. With `ORCA_TEST_COMMANDS` and no embedding, the window commands do nothing.

### macOS window behaviour

Orca's view is a borderless window ordered just above the app's window, so the app's menus and other
apps' windows still cover it. The process is an accessory app (no Dock icon), its window follows the
app into full screen and hides when the app's window is minimized or on another Space.

**Out of sight, nothing is presented.** While Orca's window is hidden or fully covered
(`NSWindow.occlusionState`), Metal takes no drawable: a covered layer gets drawables back late, and
`nextDrawable` blocked the CPU thread (which a session shares with the GPU) for up to a second, down
to 14 fps, dragging the friend down too. After any wait over 50 ms, drawables come from a background
acquire until the window is next out of sight. `Tools/orca/occluded-test.py` measures it.

Nothing in the page can draw over Orca's window, so a sheet over the box must `hide` it; the play
menu's side panel instead cuts `rect`, keeps `view` and sends `dim 50`.

**Not built:** rendering into IOSurfaces shown in the app's own view. A spike
(`Tools/orca/iosurface-spike.c`) passed three 1920x1080 IOSurfaces to another process in about 13 ms
of setup, with per-frame messages in a median 12 us. It would need Metal to present into an
IOSurface ring and a small N-API module in the app.

### Windows window resizes

When the app resizes Orca's window and the graphics driver can't make the picture at the new size
(D3D11 or D3D12 `ResizeBuffers` fails: out of memory, a window past 16384 px), Orca skips the picture
instead of closing, and asks for the resize again on the next frame, then every 30 frames (a new
size asks at once). The game, and an online match, carry on. stderr says so once per change
(`Orca: the picture's swap chain has no buffers after a resize ...`, with the window's size and the
error code) and again when the picture works. A lost graphics device still ends Orca, after saying
so. Since 0.3.35; before, a refused resize ended Orca (exit `0x80000003`).

## The sandbox

The YouGame desktop app can run Orca inside its sandbox: on Windows an AppContainer started by
`yougame-sandbox.exe`, inside a job (low integrity, no loopback, files only where the app grants
them), and later on macOS a `sandbox-exec` profile. Orca's side of it is a handful of switches, all
off unless the app sets them, so an Orca with them runs exactly as before under an app without the
sandbox. The app jails only an Orca whose manifest entry (and the installer's `orca.json`) says
`"sandbox": 1`. The build says so itself in `Sys/Orca/features.json` (`{"sandbox": 1}`: it speaks
`YOUGAME_STDIO`, `--embed-adopted` and `ORCA_NET`), which the release scripts copy into the
manifest entry, so a build without the mode can never be marked as having it. Code: `Orca/Sandbox.*`, `Orca/Bridge.*`, `DolphinNoGUI/Embed.*`,
`PlatformWin32.cpp`; tests: `OrcaBridge*`, `OrcaSandbox.*` (`SandboxTest.cpp`) and
`OrcaLive.StdioBridgeMirrorsTheRoomAndTakesLeave`.

| What | How |
|---|---|
| `YOUGAME_STDIO=1` | The app's bridge on stdin and stdout instead of a loopback server ([The stdio bridge](#the-stdio-bridge)). |
| `--embed-adopted` | Windows: the window waits to be made a child of the app's ([Adopted window](#adopted-window)). |
| `ORCA_NET` | The network the sandbox gives Orca ([Network](#network)). |
| Every session | Dolphin's own GameCube adapter code never starts (`GCAdapter::Init`): the app's controller helper owns the adapter and sends its ports in the snapshots. No libusb or UsbDk is loaded, and a planted `Dolphin.ini` can't reach the adapter. |
| macOS session on the stdio bridge | No Quartz keyboard device, so nothing reads the global key state (`CGEventSourceKeyState`): the page sends the keyboard as a pad. |
| Windows, D3D | `d3dcompiler_47.dll` is loaded by its full path beside `Orca.exe` (the name is the fallback when there is none, as in a dev build), so neither the sandbox's prefer-System32 rule nor a DLL elsewhere on the search path swaps in another compiler, whose bytes would miss every cached shader (`FxcCache.cpp`). |
| Inside the sandbox | stderr says `Orca: in the app's sandbox (network <mode>[, stdio bridge])`. A user folder Orca may not write ends the run at once with `orca error sandbox_denied`. |
| Embedded (or `ORCA_TEST_COMMANDS`), on the stdio bridge | `orca alive` on stdout about once a second from the thread that owns the window (`Bridge::Heartbeat`, every main-loop pass). The app stops a running Orca that said it once and then goes quiet for 10 s: an adopted window shares the app window's input queue, so a hung window thread would hang the app too. Never on the loopback bridge, so an older app never sees it. `Tools/orca/stdio-app.py` fails a run without one at least every 3 s. |
| `--sandbox-probe -u <folder>` | No boot: what a boot asks of the system before its first frame (UICommon, the GPU backend's info with no window, the controllers without the Mac keyboard, the sound out), then `orca probe ok` and exit 0. The app runs it under its macOS sandbox once per macOS and Orca version, with an empty folder and no disc, to choose the strict or relaxed profile from this run alone (never from a player's run, which a crafted disc or setting could end on purpose). Not with `--embed` or `--verify`. |

**Error codes** (`orca error <code> <sentence>`):

| Code | When |
|---|---|
| `sandbox_disc` | Inside the sandbox, the disc can't be opened for lack of permission: the app didn't grant it (a network share, a file an admin tool made). "Orca can't read this disc where it is. Copy it into a folder of yours, such as Documents, and choose the copy." (the app's words for the same refusal) |
| `sandbox_embed` | `--embed-adopted`: nothing adopted the window within 10 s. "Orca couldn't open in the YouGame window. Try again." |
| `sandbox_denied` | Inside the sandbox, Orca may not write its user folder. "Orca isn't allowed into its own folder, so it didn't start. Reset Orca's files from YouGame's Help menu, then press Play again." |
| `gpu_lost` | Direct3D: the graphics device was lost (a GPU hang the driver reset, a driver update or crash; `Orca/GpuLost.h`). stderr says `Orca: [VIDEO] the graphics device is lost (D3D11: device hung, 0x887A0006) at <what failed>; Orca stops`, nothing more is drawn, and the run stops like any other stop (the room is left: an online opponent sees this player leave, not a desync), exit 1. "Your graphics driver stopped responding. Try the Performance preset or a lower resolution in Graphics, and update your GPU driver." |

**Windows zip layout.** A release zip for the sandbox carries, beside `Orca/Orca.exe`, the pinned
`d3dcompiler_47.dll` (10.0.20348 from the Windows SDK's `Redist/D3D/x64`, SHA-256
`a05f99734f7c4822fefc12b367af21fd0976ed6608752fb1e1e80b6ece7ecbbb`, the one YouGame's installer ships) and `Orca/shaders/D3D-fxc.cache`, the
compiled-shader cache made with that compiler and that build (the app's `orca.ts`
`downloadedShaders` merges it into the player's). The 0.3.30-0.3.38 zips have neither.

### The stdio bridge

With `YOUGAME_STDIO=1` the app's bridge is Orca's own stdin and stdout, the pipes embedding already
uses, so a sandboxed Orca needs no loopback (an AppContainer can't reach it) and no token. Each
bridge line starts with `yg `; every other line keeps its embed meaning.

| Direction | Line |
|---|---|
| Orca to app (stdout) | `yg {"id":<n>,"type":"hello"\|"mpTicket"\|"mpRoom",...}`: the only three requests Orca makes, at most 32 KB. `mpTicket` carries `players`, `mode`, `ranked` (always `false`), `room` and `lobby`, nothing else. |
| App to Orca (stdin) | `yg {"re":<n>,"ok":true,"result":{...}}` or `yg {"re":<n>,"ok":false,"error":"..."}`: the reply to request `n`. |
| | `yg {"push":{"type":"controllers","snapshot":{...}}}`: a controller snapshot, the same schema-1 object the loopback stream's `data:` lines carry. |
| | `yg {"push":{"slug":"<game>","cmd":"leave"\|"kick",...}}`: the page's lobby commands, as the loopback event stream sent them. |

- The stdin reader reads 64 KB at a time and routes bridge lines on its own thread, so neither a
  reply nor a pad waits for the frame loop: a reply wakes the request waiting for it (any thread,
  with a timeout), a snapshot goes straight into the controller cache the CPU thread reads once per
  frame, a lobby command to the current room.
- Only the newest controller snapshot of one read is parsed; the older ones are stale already
  (counted as `coalesced`).
- A stdin line over 128 KB is dropped whole, through its newline; a bridge line over 64 KB, one that
  isn't JSON, nests deeper than 8 or has a shape Orca doesn't know is dropped and counted, never
  acted on. A reply nobody waits for is dropped. A closed stdin fails every waiting request at once.
- Bridge lines are dropped unless `YOUGAME_STDIO=1`, and they never reach the embed commands.
- Requests need stdout as the protocol channel and stdin read: `--embed`, or `ORCA_TEST_COMMANDS=1`
  for tests. Without either the bridge is off (stderr says so) and the session plays offline.
- The loopback bridge (`YOUGAME_BRIDGE`, `YOUGAME_TOKEN`) stays for an app without the stdio
  bridge and for tests; dev tickets (`ORCA_TEST_DEV_GAME`) work as before.

### Adopted window

A process in an AppContainer (low integrity) may not create a window under the app's (medium
integrity), so with `--embed-adopted` Orca:

1. creates a hidden top-level `WS_POPUP` window (`WS_EX_NOPARENTNOTIFY | WS_EX_TOOLWINDOW`, so it
   never shows on the taskbar), class and caption both `Orca`, and never retitles it. It is made at
   the app's rect: `SetParent` keeps a window's coordinates and reads them in the parent's client
   area, so the window lands at its rect even when the launcher's `SetParent` moves it after Orca
   placed it (made at 0,0, it covered the app's header for the rest of the boot);
2. answers messages until `GetParent` is the `--parent` window: the launcher (outside the
   sandbox, `--embed-parent <hwnd> --embed-class Orca --embed-title Orca --embed-keep-style`) makes
   it a `WS_CHILD` of the app's window. After 10 s it gives up with `orca error sandbox_embed`;
3. then places it at the last `rect` and shows it, checks the place once more 50 ms later, and
   from the main loop on puts it back at the app's rect whenever it is found elsewhere.

Everything else is embed mode as before: the swap chain is made on the adopted child, `rect`,
`view`, `hide`, `show` and focus work the same, and Orca stops when the app's window is destroyed.
It also exits like embed mode (`exit <code>`, then `_exit` without the CRT's exit-time work), not
like a standalone window, whose exit crashed with `0xC0000005` up to 0.3.38 (the kit's font
textures, [On-screen UI](#on-screen-ui)). Elsewhere the flag changes nothing: a Mac's Orca window is
top-level anyway.

### Network

`ORCA_NET` says which network the sandbox gives Orca, for the [direct links](#direct-links):

| Value | Direct links |
|---|---|
| unset | Every candidate, as before. In a Windows AppContainer started without it: what the token allows (`internet-lan` with the `privateNetworkClientServer` capability, else `internet`). |
| `internet` | No loopback and no LAN candidate (RFC 1918, link-local, IPv6 unique-local) is offered or tried, and there is no upgrade to the LAN. |
| `internet-lan` | LAN candidates too (the player's setting, on by default on Windows), never loopback. |
| anything else | `internet`, the strictest. |

`ORCA_DIRECT_LOOPBACK` has no effect when `ORCA_NET` is set. The room's WebSocket, the site's HTTPS,
STUN and TURN are internet traffic and work in every mode.

## Changes by release

What each release changed for players, in plain words.

### 0.3.81

- More groundwork for two players on one computer in online matches (switched off; nothing changes yet).

### 0.3.80

- Windows: games start about 0.1-0.2 s sooner (no throwaway graphics device before the real one).
- Groundwork for two players on one computer in online matches (switched off; nothing changes yet).

### 0.3.79

- The game's log times the start of a game in finer steps (system setup, Wii storage, settings file,
  graphics device and window, the game's first frame), so slow starts can be found. Players see no change.

### 0.3.77

- Project+ 3.2.1b: YG Dolphin plays the new Project+ release (profile PPLUS321) next to 3.2.
  Players on 3.2 and 3.2.1b are matched only with players on the same version.

### 0.3.76

- The game's log times each step of starting a game, so slow starts can be found and fixed.
- Test tools: scripted button presses keep pace in online matches, and test copies of the app can
  drive the game with the on-screen controller while its window isn't focused. Players see no change.

### 0.3.74

- Games no longer stop when a match starts on older Windows graphics (for example Intel HD
  Graphics 4000) that can't do one kind of blending.

### 0.3.73

- If the game ever stops on an internal check, its message now says exactly which check, so a
  crash report shows what went wrong.

### 0.3.72

- The game's messages say "YG Dolphin" everywhere, also before YouGame shows them.
- A broadcast from a muted game keeps a silent sound track, so the video never waits for sound.

### 0.3.70

- If a friends room keeps breaking right after you get back in, the game opens a fresh room for
  you instead of trying the broken one again; send your friend a new invite.
- Opening an invite whose host already left shows "Your friend left that game" instead of
  closing the game.

### 0.3.68

- If you drop out of a long friends room, you get back in: the game asks YouGame for a fresh pass
  when the old one has expired and gives a waking room more time to answer. If the room can't be
  reached, it stops trying after a few minutes and says so, instead of retrying for ever.

### 0.3.67

- Broadcast a replay or a live match you're watching through YouGame, with game sound.
  The broadcast keeps going when the game view is hidden or has no room on screen.
- On Mac, the game finishes every change to its picture at once, so the screen can't be left
  waiting on it.

### 0.3.66

- On Mac, the game stays with YouGame when entering or leaving full screen, and waits for the
  app's window to settle before appearing again.
- The game keeps full priority on Mac while it runs.
- On Linux, Vulkan graphics are included (fixes a crash on the character screen with some AMD graphics drivers).

### 0.3.65

- Faster start: the game boots at full speed until its first frame, and a warm start no longer
  waits for the full check of Project+'s files (the check still runs in the background and must
  pass before anything goes online). A warm Project+ start reaches its first frame several
  seconds sooner.

### 0.3.64

- Clicking the YouGame logo inside a game (full screen or theater) opens the same menu as
  Shift+Tab. YG Dolphin's own small dropdown is gone; the Δ feedback button stays.
- While you search for an opponent on your own, the screen says "Waiting for another player…"
  with how long you have waited.
- Windows graphics-crash diagnostics no longer read the Windows graphics timeout settings (as in
  production's 0.3.63).

### 0.3.62

- Windows: when the graphics driver stops responding, YG Dolphin's log now records memory use,
  the graphics card's video memory budget, the driver version and the render settings, so these
  crashes can be traced. Nothing changes in play.

### 0.3.61

- Ranked sets on results-only packs (Brawl Minus, Project M 3.6) end as soon as a player wins two
  games, with the result and rating shown right away (0.3.59 only settled them when the players
  left).
- Offline local packs (`--pack`): a refused pack ends the program with an error instead of waiting,
  and the terminal shows only the pack folder's name.

### 0.3.59

- When your friend gets back to a friends room before you do, YG Dolphin joins their game instead
  of reporting a network error.
- After a dropped connection, rejoining a game that just finished shows its result like any other
  finish, instead of a network error.
- Trying to join a friend who already left that game says so ("Your friend left that game. Ask
  them for a new invite.") and stops retrying.
- Ranked sets on results-only packs (Brawl Minus, Project M 3.6): leaving during the first game
  now counts as a loss, as it does on Project+.
- Offline play without YouGame: `--pack <folder>` plays your own unsigned pack offline only
  (docs/PLAY-STANDALONE.md).

### 0.3.58

- The feedback button inside a game is the same red triangle as everywhere else on YouGame, and it
  and the YouGame button no longer react to each other.
- After a lost match, YG Dolphin opens a fresh room of its own and says the match ended, instead of
  retrying the old room and blaming a version or disc mismatch.

### 0.3.57

- A tournament opponent can now join the host's game at character select: in rooms the page opens
  with `host` outside the matchmaking queue, the host sends its game state to the arriving player.

### 0.3.56

- Returning to a public match after the reconnect time runs out now says why you couldn't rejoin.
- On a Mac, YG Dolphin inside the YouGame app is much lighter on macOS's window system and never takes
  keyboard focus from the app. This follows a Mac that froze when a game started in full screen.

### 0.3.55

- Public casual and ranked matches can ride through a dropped connection: when the server supports
  it, YG Dolphin keeps the match, shows "Reconnecting…" with a countdown, and rejoins by itself.
  If a player doesn't come back in time, today's rules decide the result.
- Fixes the game window sometimes not starting on Windows 10 with the YouGame app.

### 0.3.54

- Groundwork for public casual and ranked matches to ride through a dropped connection, the way
  friends rooms already do. It switches on later, from the server side.
- Safer handling of saved and live match replays.

### 0.3.53

- If your PC runs out of memory while a game starts, YG Dolphin now says so plainly instead of closing.
- The YouGame menu's feedback and volume buttons over the game appear only with a YouGame app that
  supports them; on older apps the YouGame button works as before.
- Behind the scenes: diagnostics for the game window inside the page.

### 0.3.52

- Everything in 0.3.50, under a new number. 0.3.51 was a networking fix for Windows on the previous
  release line; 0.3.52 includes it.

### 0.3.50

- In full screen, the YouGame menu over the game can now send feedback with a screenshot of the
  game, change the volume, mute, leave full screen and leave the match (with YouGame 0.12.52).
- A networking fix for Windows.
- Behind the scenes: test copies mark their results as tests, and safer checks on the game's own menus.

### 0.3.49

- Saved replays play back completely offline: watching one never connects to anything.
- Behind the scenes: safer test copies (they never touch a player's controllers) and more tests for dropped connections.

### 0.3.48

- In a friends room with three or four people, you can now sit out after a fight: pick up your
  token with B, then press A on your own player button. Whoever doesn't pick a character sits out;
  they can pick at the next character select.
- A clearer message tells you when a friend's YG Dolphin is older or newer. Ask them to press Play
  to update.
- Fewer "room is full" retries after a dropped connection.
- A clear message tells you when a friend's game is too far behind to send you the match.
- Packs that need a newer YG Dolphin now give a clear message telling you to update the YouGame app.
- Known limit: a friend who is sitting out is still connected. Their bad connection can briefly
  stall the fight.

### 0.3.47

- Recorded matches reach the site again: 0.3.46 never uploaded them. Fixes a rare crash at start on
  Windows when the time zone can't be read.

### 0.3.46

- Play doubles with friends: up to four in a friends room. Tournament matches report their own results.

### 0.3.45

- Watch your recorded matches again from the site.

### 0.3.44

- **Friends rooms ride through short connection drops.** If your connection drops for a moment, the room
  keeps going and keeps the same room code, so nobody has to make a new room or share a new code.

### 0.3.43

- **Saved replays.** Matches in ranked and tournament rooms can be saved as replays.
- **A warning before restarting.** If a friend's join needs to restart your pack's game, you get
  a three-second warning that unsaved progress will be lost.

### 0.3.42

- **YG Baiji is now YG Dolphin.** The window, the Mac menu and the messages use the new name.
  Your controls and settings carry over.
- **Name tags keep the buttons you saved.**
- **More community packs.** Community mods of other GameCube games can run (casual).

### 0.3.41

- **Clearer "couldn't open the disc" reports.** When Project+ (or a pack's launcher) can't open your
  game disc, the report now says why: the file won't open or read, is cut short, or isn't a disc
  image. A disc whose folder or file name contains `=` now opens.
- **A refused pack no longer starts the plain game.** If a game's pack fails its checks, the game
  doesn't start, instead of the plain game starting in its place; the reason is in the report.
- **Better crash reports** for crashes inside the game.
- **Stability fixes.**

### 0.3.40

- **Orca is now YG Baiji.** The name was picked in a YouGame community poll; the baiji is the
  Yangtze river dolphin. The window, the Mac menu and the messages say YG Baiji. Your controls,
  settings and ranked ratings carry over. On Windows the firewall may ask once more, because the
  program has a new name.
- **Hold to talk.** Hold YouGame's overlay key (or the controller shortcut) to talk to friends in a
  match; a tap still opens the menu.
- **Rumble.** Your controller can rumble when the game rumbles, with the app's switch on.
- **Random stage in casual.** Both players can pick Random on a casual stage select.
- **Hold B to leave.** Skipping a casual opponent, leaving after a ranked set and backing out of the
  character select are now a held B. Z goes to the game, so Project+'s Z costumes work.

### 0.3.39

- **Your graphics, your choice.** Every graphics setting that only changes what your own screen
  shows is now yours to pick in the YouGame app: resolution, anti-aliasing, texture filtering, the
  backend and graphics card, and more. Settings that would change the game or show you more than
  your opponent stay the same for everyone.
- **Orca keeps up on slower computers.** If your computer can't hold 60 frames a second in a match,
  Orca switches its graphics to Performance by itself, once, and tells you. Your opponent stops
  freezing.
- **The app sees how fast your computer is,** so it can tell you when a setting helps.
- **Stability and ranked fixes.**
- **A friend joining while you set up your controls no longer crashes both games.** If a friend
  dropped in while you were in Options > Controls (making a name or typing it), both games crashed
  or froze. Now your friend waits on your menu until you leave Controls, then you both go to the
  character select.
- **Stopping a game while it loads** no longer counts as a game that couldn't start.
- **Windows: no more crash after a game ends.** Orca could close with an error a moment after a
  game had ended, which sent a crash report. Games themselves were never affected.
- **Options > Controls no longer freezes the game.** After any character select, opening the
  game's Controls menu stopped the game for good: Orca put your name tag in the last of the
  game's 120 slots, and the menu can't list tags while the first slot is empty. Your tag now goes
  in the first free slot, as the game puts its own (`NameTags.cpp` `MakeTag`), and making it no
  longer clears memory past the tag table.

### 0.3.38

- **Matches start about 2 seconds sooner.** Once the queue finds you an opponent, you are both in
  control of your fighters about 2 seconds earlier, and the player joining sees "Opponent found ·
  joining" and the opponent's name while the match loads.
- **Linux: an experimental build.** The YouGame app on Linux (x86-64, X11 or XWayland) can now run
  Orca inside its window. It is an early test and may still have problems.
- **Small fixes from upstream Dolphin.** A crash when the system's language setting is invalid, a
  counter overflow in the graphics statistics, and extra checks in the GameCube microphone.

### 0.3.37

- **Music stays off.** With the Music switch off, the music no longer comes back for a few seconds
  each time a song loops.
- **Random is random again.** Picking Random on the character select gives a different fighter from
  match to match, instead of the same ones every time.
- **Reconnecting to a ranked set no longer closes Orca.** If your connection drops mid-set and you
  come back to the same room, you rejoin the set instead of Orca stopping.
- **Your in-game name tag and controls carry into online games.** A name tag you make in the game's
  Controls menu, named like your YouGame name, is now used as yours, so its controls (tap jump off,
  your C-stick) play online instead of the defaults, and the name no longer shows up twice.
