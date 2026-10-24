// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/UX.h"

#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/Orca/Engine/Module.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"
#include "Core/Orca/Games/Generic/GenericModule.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Sandbox.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Rumble.h"
#include "Core/Rollback/InputGate.h"

namespace Orca::UX
{
void SetGameDisc(std::string path)
{
  Kit::SetGameDisc(std::move(path));
}

void Init()
{
  StartControllerStreamFromEnvironment();
  Orca::Events::SetLocalPadSource([](int ordinal, bool for_frame) { return LocalPad(ordinal, for_frame); });
  Orca::Events::SetInputAgeSource([] { return GetInputAge(); });
  InitOverlay();
  // The input gate's masks for the frame about to run: every rule's masks ORed, from memory alone.
  Rollback::InputGate::SetSource([](const Core::CPUThreadGuard& guard) {
    // Mod mode: the builtin's guards, against memory as it is now (Core/Orca/Mod.h).
    Orca::Mod::RefreshGuards(guard);
    Orca::Engine::Module* module = Orca::Engine::ActiveModule();
    return module ? module->Masks(guard) : Rollback::InputGate::Masks{};
  });
  // Each frame's raw buttons go into the match block before it runs, so the queue's character
  // select still sees presses the gate hides from the game (Start, B, Z).
  Rollback::InputGate::SetLatch(
      [](const Core::CPUThreadGuard& guard,
         const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) {
        Orca::Mod::RefreshGuards(guard);
        if (Orca::Engine::Module* module = Orca::Engine::ActiveModule())
          module->Latch(guard, raw);
      });
  Orca::Events::SetUXCompatVersion(kCompatVersion);
  // Runs at every frame boundary, on first runs and rollback re-runs, before the session saves
  // state there. Every memory write below must be a pure function of emulated memory, the frame
  // number and the synced ports, so a re-run writes exactly what the first run did.
  Orca::Events::SetFrameCallback([](const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                                    const std::vector<Orca::Events::PortInfo>& ports, bool alone) {
    static bool s_logged = false;
    if (!std::exchange(s_logged, true))
      NOTICE_LOG_FMT(ROLLBACK, "Orca UX: frame hook live at frame {}", frame);
    // Mod mode: whether the builtin's modules run here, from its guard words in memory now (a pure
    // function of memory, first runs and re-runs; Core/Orca/Mod.h).
    Orca::Mod::RefreshGuards(guard, frame);
    ApplyGamePatches(guard);
    // The game module's steps, in its own order (Games/Brawl/BrawlModule.cpp).
    if (Orca::Engine::Module* module = Orca::Engine::ActiveModule())
      module->OnBoundary(guard, frame, resimulating, ports, alone);
    // Must run last. Toggled patch groups follow the state the writers above left, so a re-run of
    // this boundary decides the same and writes nothing new.
    ApplyToggledGamePatches(guard);
  });
  // While the game waits for a friend, re-present the last frame with "Waiting for <name>...".
  Orca::Events::SetStallCallback([](int stalled_ms) {
    RepresentDuringStall(stalled_ms);
    Rumble::Stall(stalled_ms);
  });
  Orca::Events::SetBoundaryCallback([](bool shown, double game_held_ms, int game_held_frames) {
    FrameBoundary(shown, game_held_ms, game_held_frames);
  });
  // For joined/left toasts only. Never a source for memory writes; those use the frame callback.
  Orca::Events::SetPlugInCallback([](int frame, const std::vector<Orca::Events::PortInfo>& ports) {
    const double now = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    Overlay().OnPorts(frame, ports, now);
  });
}

void SelectModule(const Profile* profile)
{
#ifdef ORCA_BUNDLE_TRUST_TEST_KEY
  constexpr bool test_build = true;
#else
  constexpr bool test_build = false;
#endif
  if (!profile)
    Orca::Engine::SetModule(nullptr);
  else if (GenericModuleEligible(profile, Mod::ResolvedKind().value_or(Mod::BundleKind::None),
                                 Sandbox::Jailed(), test_build))
    Orca::Engine::SetModule(&Generic());
  else
    Orca::Engine::SetModule(&Builtin());
}

Orca::Engine::Module& Game()
{
  if (Orca::Engine::Module* module = Orca::Engine::ActiveModule())
    return *module;
  return Builtin();
}

void Shutdown()
{
  Rumble::Halt();
  Rollback::InputGate::SetSource({});
  Rollback::InputGate::SetLatch({});
  Orca::Events::SetStallCallback({});
  Orca::Events::SetBoundaryCallback({});
  Orca::Events::SetFrameCallback({});
  Orca::Events::SetPlugInCallback({});
  Orca::Events::SetLocalPadSource({});
  Orca::Events::SetInputAgeSource({});
  // What the module wrote outside the regions it declares (record-only, Engine/Regions.h).
  if (Orca::Engine::Module* module = Orca::Engine::ActiveModule())
  {
    const Orca::Engine::WriteCounts writes = Orca::Engine::Writes();
    NOTICE_LOG_FMT(ROLLBACK, "Orca: module {} made {} guest writes, {} outside its regions",
                   module->Id(), writes.writes, writes.outside);
  }
  SelectModule(nullptr);
  ShutdownOverlay();
  StopControllerStream();
}
}  // namespace Orca::UX
