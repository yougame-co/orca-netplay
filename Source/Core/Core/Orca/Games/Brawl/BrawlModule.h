// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/Orca/Engine/Module.h"

// The builtin game module for Brawl rev 2 and Project+ (Orca split, S2 commit 4): the game steps
// that used to be UX.cpp's three hook bodies, moved verbatim and in the same order. Each step still
// checks whether it applies (BrawlExecutable, ProfileRuleset), so this module runs for every
// session as those bodies did; Masks returns none when no ruleset applies. Since S3 those checks
// ask the module (Describe, Active, ProfileRuleset over BrawlGame.h) instead of comparing game ids.
//
// A few engine calls still sit between the game steps (ProbeFrame, Rumble, Kit::SetLook, the late-
// name test knob): hoisting them out changes the write order and needs its own proof (modules.md
// section 6, after S5).
namespace Orca::UX
{
class BrawlModule final : public Orca::Engine::Module
{
public:
  std::string_view Id() const override { return "brawl-builtin"; }
  Orca::Engine::Manifest Describe() override;
  bool Active() override;
  std::span<const Orca::Engine::Region> Regions() override;
  void OnBoundary(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                  const std::vector<Orca::Events::PortInfo>& ports, bool alone) override;
  Rollback::InputGate::Masks Masks(const Core::CPUThreadGuard& guard) override;
  void Latch(const Core::CPUThreadGuard& guard,
             const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) override;
  Orca::Engine::Verdicts& Results() override;
  Orca::Engine::HeaderHost& Header() override;
  Orca::Engine::MenuReads& Read() override;
  Orca::Engine::CssAdapter* Css() override;
  void LoadOwnControls() override;
  void SetLocalPort(int port) override;
  Orca::Engine::Display Show() override;
  Orca::Engine::Ruleset ProfileRuleset() override;
  bool ScenesReadable(std::string_view running_game, u16 running_revision) override;
  std::string SceneName(const Core::CPUThreadGuard& guard) override;
};

// The one instance UX::Init hands the engine.
BrawlModule& Builtin();
}  // namespace Orca::UX
