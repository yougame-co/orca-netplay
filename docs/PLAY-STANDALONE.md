# Play YG Dolphin without YouGame

YG Dolphin can play Brawl and the official Project+ v3.2 files offline without a YouGame
account or the YouGame app. You need your own Brawl disc image and a keyboard or a controller
mapped as a GameCube pad. No game disc or Project+ files come with the engine.

This guide covers the 0.3.58 release and the standalone source changes. **`--pack` requires a
build containing the standalone changes; the already published 0.3.58 downloads do not have
that option.** The source version number has stayed at 0.3.58.

## Download

Download and extract the whole archive for your machine. Keep the executable and its data
files together.

| Machine | YG Dolphin 0.3.58 |
| --- | --- |
| Windows x64 | [Windows download](https://yougame.co/api/orca/download/0.3.58/windows-x64) |
| Apple Silicon Mac | [macOS download](https://yougame.co/api/orca/download/0.3.58/macos-arm64) |

These use the public production route `/api/orca/download/<ver>/<target>`. Source code and
build instructions are in [yougame-co/orca-netplay](https://github.com/yougame-co/orca-netplay)
and [ORCA.md](../ORCA.md#building). The Mac archive is for Apple Silicon; it is not an Intel
Mac build. The download service still belongs to YouGame; once downloaded, offline play
does not need it.

## First run and your disc

Use **Super Smash Bros. Brawl, USA, revision 2** (game ID RSBE01). The supported original
USA revision 1 dump also plays as revision 2 automatically. Other revisions and modified
revision 1 copies are refused. You can select a supported disc image such as `.iso`, `.wbfs`
or `.rvz`; leave it as a file, without mounting it.

On macOS, open **YG Dolphin.app**. It asks for your disc on first run and remembers the path.
If the remembered file has moved, it asks again. To choose a particular disc from Terminal:

```sh
"./YG Dolphin.app/Contents/MacOS/Orca" -e "/Games/Brawl.wbfs"
```

On Windows, open PowerShell in the extracted folder containing `yg-dolphin.exe` and run:

```powershell
$env:ORCA_SESSION = "1"
& ".\yg-dolphin.exe" -p win32 -u ".\StandaloneUser" -e "D:\Games\Brawl.wbfs"
```

Windows' engine frontend has no standalone disc picker: use `-e`. Save that command in a
PowerShell script or make a shortcut with the same arguments for later runs. `ORCA_SESSION=1`
selects the engine's supported Brawl session behavior; the Mac app selects it automatically.
`-u` chooses a separate settings folder. Quote paths containing spaces.

Choose the game's local Versus modes for offline play. Sessions start with a fresh session
save/NAND. This is a match-oriented engine, so do not depend on it to keep single-player
progress between sessions. Savestates, reset and Wii power operations are restricted in sessions.

## Controls and settings

Without YouGame, input comes from Dolphin's controller mappings. There is no controller setup
window in this engine frontend. Port 1 uses the keyboard by default. On a fresh Mac user folder,
it also maps the first SDL gamepad; plug it in before launching and focus the game window.

| GameCube control | Keyboard |
| --- | --- |
| Main stick | Arrow keys |
| A / B / X / Y | X / Z / C / S |
| Z / Start | D / Return (Enter) |
| L / R | Q / W |
| C-stick | I / K / J / L (up / down / left / right) |
| D-pad | T / G / F / H (up / down / left / right) |

The Mac default gamepad mapping uses the left stick, right stick, south/west/east/north face
buttons for A/B/X/Y, right shoulder for Z, Start, both triggers and the D-pad.

Close the engine before editing settings. Controller mappings live in `Config/GCPadNew.ini`
inside the user folder. You can configure an emulated GameCube pad in Dolphin's normal
controller window using a separate user folder, then copy that mapping file into YG Dolphin's
user folder. Map port 2 separately for a second local player; sessions normally enable two
GameCube ports. Wii Remotes are disabled. Native GameCube adapter handling is disabled in a
session: YouGame's adapter helper is not available standalone. Use a keyboard or a gamepad
that Dolphin can map as an emulated GameCube controller.

For Windows gamepads, a starting mapping is the source file
[`Data/Sys/Orca/Input/GCPadNew.ini`](../Data/Sys/Orca/Input/GCPadNew.ini). Copy it to your user
folder's `Config/GCPadNew.ini`, set `Device = DInput/0/Keyboard Mouse`, and change the keyboard
names `Return`, `Up Arrow`, `Down Arrow`, `Left Arrow`, `Right Arrow` to `RETURN`, `UP`, `DOWN`,
`LEFT`, `RIGHT`. Its explicit `SDL/0/*` expressions also map the first SDL gamepad. Reuse your
own Dolphin mapping instead if your device needs different buttons.

User folders:

- **Mac app:** `~/Library/Application Support/Orca`, unless you pass `-u`. The remembered disc
  is in `Config/Orca.ini`, under `[Disc]`, `Path`.
- **Windows examples above:** `StandaloneUser` beside the executable. Without `-u`, Dolphin's
  usual location rules apply: portable `User` beside the executable if `portable.txt` exists,
  an existing configured Dolphin user folder or Documents folder, otherwise
  `%APPDATA%\Dolphin Emulator` (falling back to `User` beside the executable).
- General settings are `Config/Dolphin.ini`; graphics settings are `Config/GFX.ini`.
  Session-required game settings take precedence over edits that change the game.

## Project+ offline

Download the **official Project+ v3.2 netplay files** from the Project+ project's release.
Keep its `Project+ Netplay Launcher.dol` and matching `sd.raw` side by side. YG Dolphin's
`PPLUS32` profile requires those exact official files and your Brawl disc. A different Project+
version or an edited card needs its own local pack.

On Mac, place this `orca-launch.ini` next to **YG Dolphin.app**, with the official launcher
and card in a `pplus` folder beside the app:

```ini
[Launch]
Profile = PPLUS32
Executable = pplus/Project+ Netplay Launcher.dol
```

Open the app without arguments; it chooses/remembers the Brawl disc and boots the launcher.
The ini requires both keys, allows no extra sections or keys, and the executable path must
stay inside the folder containing it, including after resolving links. It must be relative,
with no `.` or `..` path component. This automatic ini launch is a **Mac app** feature.

For explicit launch on Mac:

```sh
ORCA_PROFILE=PPLUS32 "./YG Dolphin.app/Contents/MacOS/Orca" \
  -C "Dolphin.Core.DefaultISO=/Games/Brawl.wbfs" \
  -e "/Games/pplus/Project+ Netplay Launcher.dol"
```

On Windows, use the profile explicitly:

```powershell
$env:ORCA_SESSION = "1"
$env:ORCA_PROFILE = "PPLUS32"
& ".\yg-dolphin.exe" -p win32 -u ".\StandaloneUser" `
  -C "Dolphin.Core.DefaultISO=D:\Games\Brawl.wbfs" `
  -e "D:\Games\pplus\Project+ Netplay Launcher.dol"
```

The card is read-only during play. Remove the `ORCA_PROFILE` variable before returning to
plain Brawl (`Remove-Item Env:ORCA_PROFILE` in PowerShell). On Mac, remove or rename the ini
to return to a no-argument Brawl launch, or pass the disc with `-e`.

## Local packs (standalone source builds)

`--pack <folder>` selects an **unsigned, offline-only** pack. It accepts a format-1 tree pack
(`mod.json`, `tree.json`, and its files) or a format-2 flat pack (`mod.json` and the files it
names, without `mod.sig`). A folder is resolved from your current working directory; an
absolute path works too. This option selects session mode itself and overrides `ORCA_MOD_DIR`.
It needs no signing service, account or YouGame app.

With Python 3 and a source checkout, make a Brawl tree pack:

```sh
python3 Tools/orca/mods/make-bundle.py RSBE01 /Games/my-brawl-pack --tree
```

The output folder must not exist yet. To regenerate `tree.json` after editing a tree pack:

```sh
python3 Tools/orca/mods/make-bundle.py --tree /Games/my-brawl-pack
```

Run it on Mac:

```sh
"./YG Dolphin.app/Contents/MacOS/Orca" --pack "/Games/my-brawl-pack" -e "/Games/Brawl.wbfs"
```

Or on Windows:

```powershell
& ".\yg-dolphin.exe" -p win32 -u ".\StandaloneUser" `
  --pack "D:\Games\my-brawl-pack" -e "D:\Games\Brawl.wbfs"
```

With a default CMake source build on Mac, use the built executable directly:

```sh
build/Binaries/dolphin-emu-nogui -p macos -u ./StandaloneUser \
  --pack /Games/my-brawl-pack -e /Games/Brawl.wbfs
```

A Windows source build calls the executable `DolphinNoGUI.exe`; use it in place of
`yg-dolphin.exe` in the command above. `--pack` enables the session on both platforms.

For a launcher pack, pass its launcher with `-e` and your Brawl disc with
`-C "Dolphin.Core.DefaultISO=<disc path>"`. A tree pack containing `sd/` also needs
`ORCA_SD_CACHE` set to a writable folder for the generated card. The pack's manifest still
has to meet the engine's format, file and version requirements. Invalid packs stop with a
reason; they do not silently start plain Brawl.

A successful selection prints the folder's name to stdout (the boot log keeps the full path):

```text
orca pack local <folder-name> (unsigned, offline)
```

Local-pack mode refuses **all online rooms**, including Casual, Ranked and friends rooms,
even with two identical packs. It offers no results, locks or queue2 capabilities and cannot
submit rated games. Its compatibility identity also differs from ordinary pack sessions.
Signed community packs with `mod.sig` must still open through YouGame.

Only run local packs you trust. A local pack runs outside the YouGame app's sandbox, like any
mod you load into a plain emulator.

## Limits and bug reports

Online play currently needs YouGame's servers and app integration. Standalone does not provide
an independent matchmaking server, public queues, ratings, accounts or a direct-IP online mode.
Signed community packs need YouGame. This guide does not promise that arbitrary Brawl mods
work: their launcher, manifest and engine support must agree.

Report engine or standalone problems in
[orca-netplay issues](https://github.com/yougame-co/orca-netplay/issues). Include your OS,
engine version/commit, the command you used, the pack type, and the error text or relevant log
lines. Do not attach a disc image or copyrighted mod files.
