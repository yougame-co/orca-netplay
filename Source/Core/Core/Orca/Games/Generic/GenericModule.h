// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/Orca/Bundle.h"
#include "Core/Orca/Engine/Module.h"

namespace Orca
{
struct Profile;
}

namespace Orca::UX
{
inline constexpr int GENERIC_COMPAT = 1;

// Selection uses only the boot's verified bundle and local jail verdict. Tree bundles are
// available to TEST-key builds only; flat bundles always require a jail.
bool GenericModuleEligible(const Profile* profile, Mod::BundleKind kind, bool jailed,
                           bool test_build);

// Engine-only sessions: no game addresses, rules, scene reads, or guest writes.
class GenericModule final : public Engine::Module,
                            private Engine::Verdicts,
                            private Engine::HeaderHost,
                            private Engine::MenuReads
{
public:
  std::string_view Id() const override { return "generic"; }
  Engine::Manifest Describe() override;
  bool Active() override { return true; }
  std::span<const Engine::Region> Regions() override { return {}; }
  void OnBoundary(const Core::CPUThreadGuard&, int, bool,
                  const std::vector<Events::PortInfo>&, bool) override {}
  Rollback::InputGate::Masks Masks(const Core::CPUThreadGuard&) override { return {}; }
  void Latch(const Core::CPUThreadGuard&,
             const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>&) override {}
  Engine::Verdicts& Results() override { return *this; }
  Engine::HeaderHost& Header() override { return *this; }
  Engine::MenuReads& Read() override { return *this; }
  Engine::CssAdapter* Css() override { return &Engine::NoCss(); }
  void LoadOwnControls() override {}
  void SetLocalPort(int) override {}
  Engine::Display Show() override { return {}; }
  Engine::Ruleset ProfileRuleset() override { return Engine::Ruleset::None; }
  bool ScenesReadable(std::string_view, u16) override { return false; }
  std::string SceneName(const Core::CPUThreadGuard&) override { return {}; }

private:
  std::vector<Engine::GameResult> Confirm(int, int, std::vector<int>*) override { return {}; }
  void Reset() override {}
  std::optional<Engine::SceneKind> FinalScene() const override { return std::nullopt; }
  std::optional<Engine::SceneKind> LatestScene() const override { return std::nullopt; }
  std::optional<int> ConfirmNoShow(int) override { return std::nullopt; }

  bool SetWanted(Engine::Mode mode, u8 flags, std::string_view code) override;
  u64 WantedGeneration() override { return m_generation; }
  bool HeaderInPlace() override { return true; }
  void MemoryReplaced() override {}
  void SetHeaderFree(bool) override {}
  void ExpectHeader(Engine::Mode, std::string_view, u8, int) override {}
  std::optional<std::string> TakeMismatch() override { return std::nullopt; }
  Engine::Mode m_mode = Engine::Mode::None;
  u8 m_flags = 0;
  std::string m_code;
  u64 m_generation = 0;

  bool DropInHeld() override { return false; }
  bool OnTheMenus() override { return false; }
  std::optional<OnlinePick> CssPick() override { return std::nullopt; }
  bool PickStands(OnlinePick) override { return false; }
  std::optional<OnlinePick> TakeLobbyPick() override { return std::nullopt; }
  bool FreshSupported() override;
  Engine::FreshSeen LastSeen() override { return {}; }
};

GenericModule& Generic();
}  // namespace Orca::UX
