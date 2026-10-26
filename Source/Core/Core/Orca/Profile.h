// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/Orca/Mod.h"

namespace PowerPC
{
enum class CPUCore;
}
enum class ShaderCompilationMode : int;

struct Sram;
namespace Core
{
class CPUThreadGuard;
}

// Orca per-game profiles and the settings every rollback session forces. Both players must run
// identical emulated machines, so every setting that can change what the game sees is pinned: the
// universal ones in code (ForcedConfigLoader), the game-specific ones in
// Data/Sys/Orca/<GameID>.ini, which also names the disc revision and the frame boundary.
namespace Orca
{
namespace BootAudit
{
class Audit;
}
struct Profile
{
  // The profile's name: the disc's game ID for a disc profile (RSBE01), or the ORCA_PROFILE name
  // for a launcher profile (PPLUS32).
  std::string game_id;
  // The name players know it by, for the window title (a launcher profile's is the mod's). Empty
  // means the disc's own title.
  std::string title;
  // The disc's revision (for a launcher profile, that of the disc the launcher boots).
  u16 revision = 0;
  std::optional<u32> frame_hook;
  // Instruction word expected at frame_hook. A disc profile checks it before installing the hook; a
  // launcher profile installs the hook at boot (the game isn't in RAM yet) and ignores every pass
  // while the word differs.
  std::optional<u32> frame_hook_word;
  // Drop-in: by this frame the game's boot has written its NAND /tmp files, identically whatever
  // the player does, so a joiner recreates them by running its own boot this far and the keyframe
  // carries only their hashes. Unset: the keyframe carries every file.
  std::optional<u32> boot_nand_frame;
  // Some loaders (Project+'s Gecko codes) rewrite game code with plain stores and no icbi in the
  // first frame. The JIT then keeps running stale blocks until a full cache clear, which happens at
  // a different moment on each machine and desyncs them. So Orca drops every JIT block once, at the
  // end of this frame, after the codes have landed. Unset: no clear.
  std::optional<u32> jit_clear_frame;
  // Game code a loader rewrites that Orca keeps running as the game's own (KeepGameCode lines):
  // every instruction fetch at `address` (MMU::TryReadInstruction, used by the JITs and
  // interpreters) gets `game` while memory there holds `loader`. Memory itself keeps the loader's
  // words, so RAM hashes and the codes' own reads are unaffected; any other word there is fetched
  // as is. This makes a JIT clear recompile the same code a warm JIT would have kept running.
  struct KeptCode
  {
    u32 address = 0;  // effective address in MEM1 (0x80000000-0x817FFFFC)
    u32 game = 0;
    u32 loader = 0;
    bool operator==(const KeptCode&) const = default;
  };
  std::vector<KeptCode> kept_code;
  // Game-specific additions to the universal settings, as <System>.<Section>.<Key> = <Value>.
  std::vector<std::pair<Config::Location, std::string>> forced;

  // Launcher profiles (a mod that boots through its own loader, such as Project+): the session
  // boots a loader executable with exactly this XXH3-128 hash (32 hex digits), and the loader boots
  // the disc named by `disc` and `revision` from the DVD drive.
  std::string launcher_hash;
  std::string disc;
  // The SD card image the loader reads its files from, next to the loader, and its XXH3-128 hash. A
  // session opens it read-only.
  std::string sd_image;
  std::string sd_image_hash;
  // Set at boot: the image's full path, which the session forces as the read-only SD card path.
  // A legacy launcher's audit must pass before going online. The key uses the expected hash.
  std::string sd_image_path;
  // Host-only verification of a legacy launcher's files. Expected hashes above remain the key.
  std::shared_ptr<BootAudit::Audit> boot_audit;
  // The title whose save seeds the session NAND (default: the booted title's own).
  std::optional<u64> save_title;
  // Set at boot: the game ID Dolphin gave the booted file (a loader's comes from its file name).
  // Its game settings INIs apply while the loader runs, the disc's after.
  std::string boot_game_id;
  // Mod mode (ORCA_MOD_DIR, Core/Orca/Mod.h): the bundle this profile was read from. Unset for a
  // legacy profile (Data/Sys/Orca/<name>.ini), whose sessions run exactly as before mods.
  std::optional<Mod::ModInfo> mod;

  bool IsLauncher() const { return !launcher_hash.empty(); }
};

// An environment variable as UTF-8; empty when unset. On Windows std::getenv uses the ANSI code
// page, which can't represent every path or name, and Dolphin's file functions expect UTF-8.
std::string GetEnv(const char* name);

// True when this process runs an Orca rollback session: ORCA_SESSION=1, which Orca.app sets for
// itself unless the environment already sets it (ORCA_SESSION=0 turns it off), or --pack is selected.
bool SessionActive();

// Gives the calling thread, which is on the input path, interactive scheduling so a busy machine
// neither delays it nor moves it to an efficiency core: QOS_CLASS_USER_INTERACTIVE on macOS, raised
// priority and no EcoQoS on Windows. Sessions only. See ORCA.md, "Input latency". ORCA_THREAD_QOS=0
// disables it; =1 also raises the CPU thread in a scripted harness run. Scheduling never changes
// what a thread computes, so this is host-side only.
enum class LatencyThread
{
  Cpu,          // emulation, the frame hook, the session
  Room,         // the YouGame room's network thread
  Controllers,  // the app's controller stream
};
void PrioritizeThread(LatencyThread thread);

// Test-only overrides for harness runs, honoured only inside a session. A process with any of them
// active must never offer or join a lobby, and its compatibility key differs.
//   ORCA_TEST_PADS=4         ports 3 and 4 connected too (4-player desync coverage)
//   ORCA_TEST_SKIP_RENDER=1  skip host rendering for the whole run
//   ORCA_TEST_NET_DELAY_MS=N the room holds every message N ms (0-1000) each way, plus
//   ORCA_TEST_NET_JITTER_MS=J a deterministic 0..J ms more per message, order kept. Set both
//                            processes (0 on the undelayed side) so their keys match.
//   ORCA_TEST_NET_SPIKES=G1-G2/L1-L2[/S1-S2]  the connection goes dark both ways for L1-L2 ms
//                            after each G1-G2 ms gap, from S1 to S2 s after the room opened
//                            (default: always); queued traffic is delivered after, in order.
//                            ~M for G1-G2 makes gaps random with mean M ms.
//                            Implies ORCA_TEST_NET_DELAY_MS.
//   ORCA_TEST_BLACKOUT=S1-S2  drop direct packets both ways from S1 to S2 s after the room
//                            opened; silently detach the room socket and fail every reconnect
//                            as a network error until S2. No traffic is queued by this hook.
//   ORCA_TEST_BLACKOUT_ROOM_AFTER=N  restore the room N s BEFORE the direct path (0 by default).
//                            For a 15 s blackout, N=12 brings the room back after 3 s.
//                            Both hooks require ORCA_SESSION=1; BLACKOUT marks the test key.
//   ORCA_TEST_NET_UPLINK=D+J[/S1-S2]  outgoing messages wait D ms plus a deterministic 0..J ms,
//                            order kept (a slow, bursty uplink), from S1 to S2 s after the room
//                            opened. Implies ORCA_TEST_NET_DELAY_MS.
//   ORCA_TEST_SNAPSHOT_EVERY=K  snapshot every K-th predicted frame (1-4) instead of choosing from
//                            the measured save time; the synctest (YG_SYNCTEST) then loads only
//                            multiples of K. Peers may differ in K.
//   ORCA_TEST_FULL_SNAPSHOTS=1  snapshots copy all of RAM instead of copy-on-write
//                            (Rollback/Cow.h), for comparison.
//   ORCA_TEST_NO_AFP=1       an ARM CPU with FEAT_AFP (M4 and later) behaves as one without:
//                            FPCR.AH is never set and the JIT takes its no-AFP paths. Read at CPU
//                            detection (Common/ArmCPUDetect.cpp), so it applies outside sessions
//                            too; on x86-64 it only marks the key.
//   ORCA_TEST_CPUCORE=N      run CPU core N instead of the JIT: 0 interpreter, 5 cached interpreter
//   ORCA_TEST_GATE=<rules>   input-gate test masks (Rollback/InputGate.h): ports whose buttons or
//                            sticks the game doesn't see, always or on chosen frames. Same value in
//                            both processes.
//   ORCA_TEST_QUEUE=casual|ranked[:<coin 0|1>]  the online rules header (UX/OnlineRules.h) a queue
//                            room's host writes, to test locks and ranked flows without the queue;
//                            `ranked:1` makes port 2 the first striker. Same value in both
//                            processes.
//   ORCA_TEST_CHAR_ORDER=<rules>:<game>:<winner>  a ranked set's character order (UX/CharOrder.h)
//                            without a match behind it: `pplus:2:1` is Project+'s game 2 after
//                            port 1 won. Same value in both processes.
// Not in TestOverridesActive, since it can't change the game:
//   ORCA_TEST_SHADER_WAIT_MS=N  the boot's shader wait (WaitForBootCompile) gives up after N ms.
// Scripted pads, including the stdio test driver before its first snapshot arrives.
bool HarnessInputActive();
// Adds the optional room/app report marker; normal play preserves every byte.
std::string HarnessReport(std::string json);
int TestPads();
bool TestSkipRender();
std::optional<int> TestNetDelayMs();
int TestNetJitterMs();
struct TestBlackout
{
  int from_s = 0;
  int to_s = 0;
  int room_before_s = 0;
  bool DirectBlocked(std::chrono::steady_clock::duration elapsed) const
  {
    return elapsed >= std::chrono::seconds(from_s) && elapsed < std::chrono::seconds(to_s);
  }
  bool RoomBlocked(std::chrono::steady_clock::duration elapsed) const
  {
    return elapsed >= std::chrono::seconds(from_s) &&
           elapsed < std::chrono::seconds(to_s - room_before_s);
  }
};
std::optional<TestBlackout> TestBlackoutConfig();
struct TestNetSpikes
{
  int gap_min_ms = 0;
  int gap_max_ms = 0;
  int length_min_ms = 0;
  int length_max_ms = 0;
  int from_s = 0;
  int to_s = 0x3fffffff;
  // Gaps are exponential around gap_min_ms on average, not between the bounds.
  bool poisson = false;
};
std::optional<TestNetSpikes> TestNetSpikesConfig();
struct TestNetUplink
{
  int delay_ms = 0;
  int jitter_ms = 0;
  int from_s = 0;
  int to_s = 0x3fffffff;
};
std::optional<TestNetUplink> TestNetUplinkConfig();
int TestSnapshotEvery();  // 0: unset
bool TestFullSnapshots();
bool TestNoAfp();
std::optional<PowerPC::CPUCore> TestCpuCore();
std::optional<int> TestShaderWaitMs();
std::string TestGateSpec();  // ORCA_TEST_GATE in a session, else empty
std::string TestQueueSpec();      // ORCA_TEST_QUEUE in a session, else empty
// ORCA_TEST_QUEUE_PICK in a session, else empty: <frame>:<char>:<costume>:<x>:<y>[:<rating>]. Port
// 2 plugs in at that frame with that queue identity (UX/Queue.h), in a harness run.
std::string TestQueuePickSpec();
std::string TestCharOrderSpec();  // ORCA_TEST_CHAR_ORDER in a session, else empty
// ORCA_SHADER_WAIT_S: how long the app lets a session's boot wait for shaders, in seconds (60-900).
// Set by an app that keeps a boot alive while `orca shaders` lines arrive; otherwise the boot waits
// at most 60 s.
std::optional<int> ShaderWaitLimitS();
// ORCA_TEST_FXC_FLAGS (hex): the D3DCOMPILE_* flags FXC compiles with (D3DCommon/Shader.cpp), for
// timing other optimization levels. Rendering only, so not in the lobby key.
std::optional<u32> TestFxcFlags();
bool TestOverridesActive();

// Loads a game's profile; nullopt and a reason when there is none, the revision differs or a line
// is malformed. Without a revision (a launcher boot) the caller checks the disc itself.
std::optional<Profile> LoadProfile(const std::string& game_id, std::optional<u16> revision,
                                   std::string* error);
// The same, from another Sys folder (with trailing slash; tests read the source tree's).
std::optional<Profile> LoadProfileFrom(const std::string& sys_directory, const std::string& game_id,
                                       std::optional<u16> revision, std::string* error);

// Mod mode: the profile a bundle's mod.json makes (Core/Orca/Mod.h), after checking its tree.json
// (the hash of the list, every file there at its size). Anything malformed refuses (nullopt).
std::optional<Profile> LoadModProfile(const std::string& dir, std::string* error);
// Mod mode, a flat bundle: the profile its verified mod.json makes (Core/Orca/Bundle.h).
std::optional<Profile> LoadFlatProfile(const Mod::FlatBundle& bundle, std::string* error);
// Mod mode's boot checks: a loader boot runs exactly the bundle's launcher with the mod's disc in the
// drive (`default_disc`); a disc boot (`executable_path` null) runs the mod's disc (`boot_game_id`,
// `boot_revision`); `wrong_disc` says a refusal was the disc's. A mod with an SD card gets its image built or reused (ORCA_SD_CACHE), and
// `profile->sd_image_path` names it.
bool PrepareModBoot(Profile* profile, const std::string* executable_path,
                    const std::string& default_disc, const std::string& boot_game_id,
                    u16 boot_revision, std::string* error, bool* wrong_disc);

// ORCA_PROFILE: the launcher profile for a session that boots an executable; empty when unset.
std::string LauncherProfileName();

// XXH3-128 of a file as 32 lowercase hex digits; empty on a read failure or cancellation.
// Cancellation is checked before each 8 MiB read and before returning the digest.
std::string HashFile(const std::string& path, const std::atomic<bool>* stop = nullptr);

// Why DiscIO::CreateDisc gave nothing for `disc_path`, as the end of an error line: no path, a file
// that won't open or read (with the system's own reason), one cut short, or one that isn't a disc
// image. The app opens the disc before every run but never reads it, so a crash report needs this.
std::string DiscOpenProblem(const std::string& disc_path);

// A launcher profile's boot checks: the disc must match, and a worker verifies the loader and SD
// image. Cache misses wait for verification; hits can boot offline while the full audit runs.
// Sets sd_image_path and boot_audit. Online start must await the audit; failures retain the old
// profile refusal. On a synchronous failure, error says why.
bool PrepareLauncherBoot(Profile* profile, const std::string& executable_path,
                         const std::string& disc_path, std::string* error);

// The frame boundary is live: for a launcher profile, its instruction word is at the hook (a disc
// profile's hook was checked before install).
bool FrameHookLive(const Core::CPUThreadGuard& guard, const Profile& profile);

// The running session's profile, set at boot; null outside a session. The rollback core reads the
// frame boundary from it.
void SetActiveProfile(std::optional<Profile> profile);
const Profile* ActiveProfile();
// Called at the end of each frame's first run once the hook is live (catch-up frames count;
// rollback re-runs don't). True exactly once per boot, at the end of jit_clear_frame.
bool JitClearDue();
// What an instruction fetch at a physical address sees when memory holds `hex`: the game's word
// from kept_code while memory holds the loader's, else `hex`. A pure function of memory, so it is
// the same on every machine, first runs and re-runs.
u32 KeptInstruction(u32 physical_address, u32 hex);

// A config layer forcing the universal session settings plus the profile's. Uses the Netplay layer
// type: Orca never runs Dolphin's own netplay, and refuses to start a session while it does.
std::unique_ptr<Config::ConfigLayerLoader> GenerateConfigLoader(const Profile& profile);

// The SRAM every session boots with (Dolphin's built-in default), instead of each player's own.
const Sram* SessionSram();

// Hash of the save the session NAND was seeded with (0 if none). Both players must match.
u64 HashSessionSave(u64 title_id);
void SetSeedSaveHash(u64 hash);
u64 GetSeedSaveHash();

// Whether a session copies the title's save from the configured NAND into the session NAND (on by
// default). Orca.app turns it off, so every app session starts from an empty NAND (save hash 0) and
// the game creates its save in-session on both machines.
void SetSeedSessionSave(bool seed);
bool SeedSessionSave();

// After every other boot-time config change: an error if anything outranks a forced setting, or
// if the shader settings (the player's, see below) are outside what a session allows.
std::optional<std::string> VerifyForcedSettings(const Profile& profile);

// One line naming the effective values of the main forced settings, for the boot log.
std::string DescribeForcedSettings(const Profile& profile);

// Graphics settings are the player's (the app passes them with -C), except the few the universal
// list forces: see ORCA.md, "Graphics". Two have a range a session allows. The shader compilation
// mode must be one of the asynchronous ones (AsyncUber or AsyncSkip): the others compile on the
// emulation thread, which stalls the other player. The compile thread count is -1 (Dolphin's
// Auto) or 1 to 4: never 0 (the emulation thread). ForcedConfigLoader pins the player's value for
// the run, or AsyncUber or -1 in place of one outside the range or of none, and
// VerifyForcedSettings checks the effective values.
bool SessionShaderModeAllowed(ShaderCompilationMode mode);
bool SessionShaderThreadsAllowed(int threads);
// Every setting the universal list forces, for tests and the `gfx` command's check.
std::vector<Config::Location> UniversalForcedLocations();
// One line naming the graphics settings in use, for the boot log.
std::string DescribeGraphics();
}  // namespace Orca
