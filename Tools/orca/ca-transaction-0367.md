# Core Animation transaction audit, 0367

Base: `ff1e554932 YG Dolphin 0.3.66`; ancestry check: `BASE-OK`.
No version change. All line references below refer to the resulting source.

`Core::Init` calls `PrepareWindow` on the host/main thread before starting
`EmuThread` (`Core.cpp:251,255`). Backend `Initialize` runs through
`GetInitializedVideoGuard` on the video/GPU thread in dual-core mode, or on
the emulation/CPU thread in single-core mode (`Core.cpp:466,479,511`). All
Metal rendering and configuration callbacks below use that GPU execution thread.
Sessions select single-core mode, so it is the emulation/CPU thread there.

| File:line under Source/Core | Thread | Write | Disposition |
| --- | --- | --- | --- |
| VideoBackends/Metal/MTLGfx.mm:33 | GPU execution thread | `displaySyncEnabled` at construction | Explicit begin/disable-actions/commit |
| VideoBackends/Metal/MTLGfx.mm:305 | GPU execution thread | `displaySyncEnabled` on config change | Explicit begin/disable-actions/commit |
| VideoBackends/Metal/MTLGfx.mm:716 | GPU execution thread | `drawableSize` at setup, surface replacement, resize | Explicit begin/disable-actions/commit |
| VideoBackends/Metal/MTLMain.mm:124 | GPU execution thread | `device` at initialization | Explicit begin/disable-actions/commit, shared with fallback format |
| VideoBackends/Metal/MTLMain.mm:126 | GPU execution thread | Fallback `pixelFormat` | Same explicit transaction |
| VideoBackends/Metal/MTLMain.mm:170 | Main | Create `CAMetalLayer` in `PrepareWindow` | Unchanged: main thread |
| VideoBackends/Metal/MTLMain.mm:176,177,181 | Main | HDR `wantsExtendedDynamicRangeContent`, `pixelFormat`, `colorspace` | Unchanged: main thread |
| VideoBackends/Metal/MTLMain.mm:185,186 | Main | View `wantsLayer`, `layer` | Unchanged: main thread |
| VideoBackends/Vulkan/VKMain.cpp:273,282,285,296 | Main | Create layer; view `wantsLayer`, `layer`; layer `contentsScale` | Unchanged: `PrepareWindow` runs before emulation starts |
| DolphinNoGUI/PlatformMacos.mm:430 | Main | Content view `wantsLayer` | Unchanged: `InitEmbedded` is called by `main`/`Run` |
| DolphinNoGUI/PlatformMacos.mm:250,391,499,726 | Main | Window initialization, embed style/frame/order/visibility, event handling | Unchanged: init, main loop, main-queue activation observer, and embed commands drained by `UpdateRunningFlag` |
| DolphinNoGUI/PlatformMacos.mm:309 | Main-queue block | Window title | Unchanged: existing asynchronous main dispatch |
| DolphinNoGUI/PlatformMacos.mm:336,351 | Main loop / Cocoa occlusion notification posting thread | No layer writes: reads window occlusion/screen/refresh rate | Unchanged: presenter visibility/refresh-rate setters only store atomics, even if a notification were posted off-main |
| Common/GL/GLInterface/AGL.mm:31,42,161 | Main for view attachment/update | OpenGL view attachment and surface-resolution setting; no direct CA property writes | Unchanged: existing main-thread paths |
| VideoCommon/Present.cpp:647,656,668,683 | Host/main callers, consumed by GPU thread | Surface handle/flags, visibility/refresh-rate atomics, layout data | No Cocoa/layer writes; unchanged |

The audit searched engine Objective-C/Objective-C++ and C++ sources for CA layer
types, layer selectors, Objective-C messages, and property access. `GetSurfaceInfo`
only reads `bounds`, `contentsScale`, and `pixelFormat`. Drawable acquisition and
presentation use Metal drawable APIs, not layer property setters. Other Metal
`setPixelFormat` calls configure texture/render-pass descriptors, not CA layers.
`CommonFuncsObjC.mm` reads the OS version; no additional CA writes were found in
Common or VideoCommon.

Four narrow explicit transactions cover the five off-main setters. Values and
setter order are preserved, each transaction commits immediately after its writes,
and no thread dispatch, wait, shared helper, RTTI, or test changes were added.

Validation artifacts are in ignored `build-0362/ca-transaction-0367*`. All builds,
compiler checks, tests, and boots use heavy-slot, the shared build lock, and nice 10;
build concurrency is two. The smoke uses an isolated user and temporary directory,
Null video, no audio output, local input disabled, all six SDL controller overrides,
and the authorized read-only RSBE01 disc.

Validation completed on 2026-10-21:

| Gate | Result |
| --- | --- |
| `cmake --build build-0362 --target tests dolphin-nogui --parallel 2` | Both targets passed; existing external-library deployment-target linker warnings remain |
| Real compilation-database object builds with `-Werror` | 2/2 passed: `MTLGfx.mm`, `MTLMain.mm`; no diagnostics |
| `python3 Tools/orca/room-fix-tests.py build-0362/Binaries/Tests/tests --gtest_brief=1` | One full run, exit 0: 2,385 tests across 192 suites; 2,362 passed, 23 skipped, zero failures; 629.554 seconds |
| Headless RSBE01 `-p headless -v Null` smoke | Exit 0, frames 0 through 600 hashed, clean frame-600 completion |
| `compat-key-template.py` loaded with `importlib.util.spec_from_file_location`; `key_code_fingerprint('.')` | `f86645e20656efe5`, unchanged |
| `git diff --check` | Passed |

No new pure helper was introduced, so no helper unit test was needed. No window
or real GPU backend was used. The windowed path remains for the later approved
Mac test window. Nothing is blocked.
