# Broadcast sink 0.3.67

Private engine-only change. Do not push or package a release from this check.

## PC check (kit agent)

Use the normal Windows MSVC configuration (RTTI disabled) and build **both** D3D and D3D12
backends. The sink uses static casts; it does not require `/GR`. No Windows compiler or real GPU
was run on the Mac. From a Developer PowerShell, in the existing checkout/build directory:

```powershell
cmake --build build --config Release --target tests dolphin-nogui --parallel 2
python Tools/orca/room-fix-tests.py build/Binaries/Tests/Release/tests.exe --gtest_filter="*Broadcast*:*Ring*:*Replay*" --gtest_brief=1
python Tools/orca/broadcast-check.py build/Binaries/Release/DolphinNoGUI.exe 'D:/ROMs/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs' 'D:/fixtures/brawl-current.ygr' --backend D3D --seconds 20 --save-frame broadcast-D3D.rgba
python Tools/orca/broadcast-check.py build/Binaries/Release/DolphinNoGUI.exe 'D:/ROMs/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs' 'D:/fixtures/brawl-current.ygr' --backend D3D12 --seconds 20 --save-frame broadcast-D3D12.rgba
python Tools/orca/broadcast-check.py build/Binaries/Release/DolphinNoGUI.exe 'D:/ROMs/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs' 'D:/fixtures/brawl-current.ygr' --backend Vulkan --seconds 20 --save-frame broadcast-Vulkan.rgba
```

Adjust only the existing build's executable paths and the authorized local disc/replay paths.
Use a compatible RSBE01 replay with at least 20 seconds of audible gameplay. No downloads,
network replay URL, signing, packaging, or live service is needed. Keep the usual PC job lock.
Do not use `--no-audio` for these checks: No Audio Output does not run the mixer.

The helper makes an exclusive, fully zeroed 11,186,624-byte `sink.bin` in its own temporary
folder, with the same header as the app. It starts the engine with:

```text
-p headless -v D3D (or D3D12 / Vulkan) -u <temporary-user> -e <disc>
ORCA_SESSION=1
ORCA_REPLAY_SOURCE=<absolute local .ygr>
ORCA_BROADCAST_SINK=<absolute temporary sink.bin>
ORCA_TEST_COMMANDS=1
ORCA_NO_LOCAL_INPUT=1
SDL_JOYSTICK_HIDAPI=0
SDL_JOYSTICK_HIDAPI_GAMECUBE=0
SDL_HIDAPI_LIBUSB=0
SDL_JOYSTICK_IOKIT=0
SDL_JOYSTICK_MFI=0
SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000
```

Only PATH, temporary directory paths and Windows SystemRoot are otherwise provided. The audio
backend uses its ordinary default and its final stereo mixer feeds the ring. The helper replies
`caps replay broadcast` to the engine's `orca caps ... broadcast ...` handshake, polls every
5 ms, and shuts down only its own child. Headless real backends exercise capture with no window
or swapchain; they must produce real frames and sound. Null alone may produce no media.

Pass criteria: validated 1280x720 RGBA8 top-row-first packets of 3,686,400 bytes, about 30 new
video tickets/second (27–33), audio packets of 480 interleaved LE s16 L/R frames / 1,920 bytes at
48,000 Hz, about 100 audio tickets/second (90–110), monotonic microsecond timestamps, nonzero
audio on an audible replay, and at least 90% of published packets accepted by the reader.
Observe at least five seconds after replay start. The `--save-frame` option retains the last validated packet as a raw `.rgba` file. Inspect it with a normal
image viewer on the PC to confirm channel order, orientation, letterboxing and game content;
header and byte count alone cannot prove the pixels' meaning. Compare stereo sound with the
replay, including a known left/right cue if available.

The reader checks the twelve LE u32 header fields at bytes 0–47; video/audio latest tickets at
64/68; drops at 72/76; state at 80 (0 waiting, 1 active, 2 stopped). Video slots start at 256,
stride 3,686,464; audio starts at 11,059,648, stride 1,984. Slot index is `(ticket - 1) % slots`.
Each 64-byte prefix has u32 guard/bytes, u64 timestamp, u32 count at 0/4/8/16. Accept only
`guard == ticket * 2` both before and after copying the payload, never an odd guard. Tickets
wrap `0x7fffff80 -> 1`. Video selects newest; audio consumes at most ten retained chunks in
order. The checker implements this without mapping producer-controlled file sizes.

Also use the app's existing spectator launch on the PC with its own sink file: start at
`--rect 0 0 0 0`, resize to visible, hide/show, minimize/restore the app, then cover it for ten
seconds. Poll that file using the same layout/guards above throughout each phase. Each hidden
phase must retain the same video/audio cadence, and showing it must resume window drawing.
Repeat D3D, D3D12 and Vulkan. Check a normal played-session launch with the sink variable still
set: caps must omit `broadcast`, and the initialized ring must remain state 0 with both latest
tickets 0. Do not read controller input or touch another process.

## Contract audit

Read-only comparison against `origin/claude/v2:desktop/src/broadcast-ring.ts` and the
`broadcastSink` / `sinkFile` paths in `desktop/src/orca.ts`: **no binary layout differences**.
All twelve fields, reserved header bytes, offsets, strides, slot metadata, ticket rollover,
odd/even publication guards, RGBA dimensions/cadence and s16 stereo rate agree. The engine's
new literal app-format tests pin the entire header and slot metadata independently of engine
constants. No app files were changed. The replay-only capability remains before `record hostui`;
the app requires that word and answers `caps replay broadcast`. Normal starts omit it.

## Window path audit

`Present.cpp` now captures and flushes the XFB before any headless/software return or window
acquisition, so offscreen commands reach their fences even without a present. VI duplicates
still call that hook, with window drawing disabled. Hidden/empty active sinks return after
capture. Live surface dimensions allow an initially zero-size Metal layer to become visible.
Windows now publishes hidden/minimized/empty-client visibility as macOS and X11 already did.
The disabled path does one inline atomic test before any surface/crop/aspect work, and one in
the mixer before calling the audio sink.

The relevant returns are indexed in the final validation notes below. Backend returns stay
local to window work; no backend visibility or device-loss behavior was relaxed.

| Location | Finding / resolution |
| --- | --- |
| `Present.cpp:255` | VI duplicate suppression used to omit the sink hook; capture now runs even when window presentation is skipped. |
| `Present.cpp:272` | Immediate-XFB cap omits extra swaps in a field; the first swap still captures, so this does not suppress a field's output. Unchanged. |
| `Present.cpp:1083` | New active-sink hidden/empty/duplicate return occurs after capture and GPU submission. |
| `Present.cpp:1095` | Headless/no-content return now follows the capture hook. Missing XFB cannot supply a frame. |
| `Present.cpp:1110` | Software/no-utility return now follows the hook; Null/software cannot provide this GPU readback. |
| `Metal/MTLGfx.mm:564,574,622,657` | No layer, hidden surface, background drawable acquisition and no drawable returns only prevent window drawing. Capture is earlier, separately submitted. |
| `D3D/D3DGfx.cpp:175,183` | Lost device / unavailable backbuffer return only prevents window drawing. Capture is earlier; device-loss handling is unchanged. |
| `D3D12/D3D12Gfx.cpp:377,412` | Unavailable backbuffers skip drawing/presenting; offscreen capture is already submitted. |
| `Vulkan/VKGfx.cpp:268,283,296` | Out-of-date resize/acquisition and invalid-image return are after offscreen capture/submission. |
| `DolphinNoGUI/PlatformWin32.cpp:420` | New project-window visibility signal includes hidden windows, minimized root windows and empty client areas. |
| `DolphinNoGUI/PlatformMacos.mm:351`, `PlatformX11.cpp:481` | Existing hidden/occluded/minimized visibility signal retained. |
| `DolphinNoGUI/Embed.cpp:81`, `Platform.cpp:187,193` | A 0x0 app rectangle becomes a hidden minimum-size native view; it still has an emulated XFB. |

## Cherry-pick conflicts

All five conflicted files preserve the later engine behavior alongside the sink:

- `Core/Orca/Status.cpp:195`: replay/broadcast followed by the existing record/hostui tail.
- `Core/Orca/Status.h:112`: retain hostui documentation and update the combined cap count to 23.
- `Core/Rollback/ReplayStream.cpp:32,1153`: retain BootAudit/SafeJson and the detailed comparison
  completion message; add sink startup/stop hooks alongside them.
- `VideoCommon/FrameDumper.cpp:12,47`: retain Core state and screenshot completion handling;
  add the broadcast entry point.
- `VideoCommon/FrameDumper.h:21`: retain the Core::State declaration and screenshot callback API;
  add the BroadcastFrame declaration/owner.

## Mac validation

Base check before edits: `c852ad77ae Commit explicit Core Animation transactions on the GPU
thread`; `BASE-OK` for `ff1e554932`. `git log --oneline ff1e554932..HEAD` showed that prerequisite
commit. Branch: `train-0367`. Reused `build-0362`; no GPU backend or window was opened.

- `tests` and `dolphin-nogui` build successfully. The current configured Mac output is
  `build-0362/Binaries/Orca.app/Contents/MacOS/Orca`; the old flat `dolphin-emu-nogui` is a stale
  artifact and was excluded from the successful smoke.
- All **13** changed C++/Objective-C++ translation units available on macOS compile with their
  build compile commands plus `-Werror -fsyntax-only`. D3D, D3D12 and PlatformWin32 are left to
  the PC compiler. Existing external-library deployment-target linker warnings remain.
- Focused `*Broadcast*:*Ring*:*Replay*`: **76/76 passed**. The five imported BroadcastRing tests
  are unchanged. Six added tests cover hidden/empty/minimum-size surfaces, duplicate frames,
  missing XFB, the disabled path, the literal app header and slot offsets/guards/metadata.
- Full required command:
  `python3 Tools/orca/room-fix-tests.py build-0362/Binaries/Tests/tests --gtest_brief=1`:
  **2,396 tests in 195 suites; 2,373 passed, 23 skipped, zero failures** (629.577 seconds).
  Skips require optional fixture/assets; no broadcast test skipped. Existing tests were not
  edited or weakened.
- `keyCode`: **f86645e20656efe5 True**. No compatibility-key part changed.
- Checker self-check: valid video/audio packets accepted; odd guards and bad counts rejected;
  ticket rollover agrees with the app.
- Normal session with the sink variable set: caps omit `broadcast`; ring stays waiting (state 0),
  video/audio latest 0/0, no writes. The recording fixture used a solo loopback room only.
- Product replay with an app-format temporary ring, `-p headless -v Null`, No Audio Output,
  local input disabled and all six SDL overrides: `caps` includes `replay broadcast`; a complete
  native 601-frame replay prefix reports `done ok`, mismatched=0 and a clean exit. Ring video/audio
  latest **0/0**, accepted packets **0/0**, drops **602/0**, state **2** after completion and shutdown.
  Null cannot supply GPU readbacks, and No Audio Output does not invoke the mixer, so this does
  not validate real video/audio payloads; that is the PC check above.

All builds, checks and boots used the requested heavy-slot + flock + nice wrapper, with at most
2 build workers and the shared ccache directory. Logs and the locally recorded fixture remain in
`build-0362/broadcast-0367/`. No pushes, releases, installs, signing or production operations.
