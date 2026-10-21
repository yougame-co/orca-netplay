// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"
#include "Core/Orca/Games/Generic/GenericModule.h"
#include "Core/Orca/JitWarm.h"
#include "Core/Orca/Music.h"
#include "Core/Orca/PanicDiag.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/UX.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/System.h"

using namespace Orca;
using namespace Orca::Engine;

namespace
{
struct BootDone
{
  ~BootDone()
  {
    UX::SelectModule(nullptr);
    SetActiveProfile(std::nullopt);
  }
};
}  // namespace

TEST(OrcaGenericModule, ContractHasNoGameRulesScenesOrResults)
{
  UX::GenericModule module;
  EXPECT_EQ(module.Id(), "generic");
  const auto manifest = module.Describe();
  EXPECT_EQ(manifest.id, "generic");
  EXPECT_EQ(manifest.ruleset, Ruleset::None);
  EXPECT_FALSE(manifest.results);
  EXPECT_FALSE(manifest.music);
  EXPECT_EQ(manifest.compat, UX::GENERIC_COMPAT);
  EXPECT_EQ(UX::GENERIC_COMPAT, 1);
  EXPECT_TRUE(module.Active());
  EXPECT_TRUE(module.Regions().empty());
  EXPECT_EQ(module.ProfileRuleset(), Ruleset::None);
  EXPECT_FALSE(module.ScenesReadable("GALE01", 0));
  EXPECT_FALSE(module.ScenesReadable("RSBE01", 2));
  std::vector<int> starts{42};
  EXPECT_TRUE(module.Results().Confirm(1000, 0, &starts).empty());
  EXPECT_EQ(starts, std::vector<int>{42});
  EXPECT_TRUE(module.Results().Confirm(1000, 0).empty());
  module.Results().Reset();
  EXPECT_FALSE(module.Results().FinalScene());
  EXPECT_FALSE(module.Results().LatestScene());
  EXPECT_FALSE(module.Results().ConfirmNoShow(1000));
  const auto display = module.Show();
  EXPECT_EQ(display.plates, 0);
  EXPECT_FALSE(display.rules_bar);
  EXPECT_FALSE(display.band_clock);
  EXPECT_FALSE(display.line_relabelled);
  EXPECT_FALSE(display.legend);
  EXPECT_TRUE(display.tip.empty());
  EXPECT_TRUE(display.ranked_set.empty());
  EXPECT_TRUE(display.ranked_turn.empty());
  EXPECT_TRUE(display.score_line.empty());
  EXPECT_FALSE(display.cursors.on);
  EXPECT_EQ(display.cursors.game, 0);
  EXPECT_EQ(display.cursors.picked, -1);
  EXPECT_EQ(display.cursors.turn, -1);
  for (const auto& player : display.cursors.players)
  {
    EXPECT_FALSE(player.present);
    EXPECT_FALSE(player.local);
    EXPECT_EQ(player.proposal, -1);
    EXPECT_TRUE(player.name.empty());
  }
  EXPECT_EQ(display.layout, nullptr);
  for (const auto* proposal : display.proposals)
    EXPECT_EQ(proposal, nullptr);
  for (const auto& cue : display.cues)
    EXPECT_EQ(cue, Cursors::TurnCue{});
  EXPECT_TRUE(display.mk_note.empty());
  EXPECT_EQ(display.picker, -1);
  EXPECT_TRUE(display.picker_ribbon.empty());
  EXPECT_EQ(display.dots, SetDots{});
}

TEST(OrcaGenericModule, HeaderTracksOnlyWantedChanges)
{
  UX::GenericModule module;
  auto& header = module.Header();
  EXPECT_EQ(header.WantedGeneration(), 0u);
  EXPECT_FALSE(header.SetWanted(Mode::None));
  EXPECT_TRUE(header.SetWanted(Mode::Casual, FLAG_SOLO, "room-a"));
  EXPECT_EQ(header.WantedGeneration(), 1u);
  EXPECT_FALSE(header.SetWanted(Mode::Casual, FLAG_SOLO, "room-a"));
  EXPECT_TRUE(header.SetWanted(Mode::Ranked, FLAG_SOLO, "room-a"));
  EXPECT_TRUE(header.SetWanted(Mode::Ranked, 0, "room-a"));
  EXPECT_TRUE(header.SetWanted(Mode::Ranked, 0, "room-b"));
  EXPECT_EQ(header.WantedGeneration(), 4u);
  header.MemoryReplaced();
  for (const bool free : {false, true})
    header.SetHeaderFree(free);
  header.ExpectHeader(Mode::None, "another-room", FLAG_QUEUE2, 99);
  EXPECT_TRUE(header.HeaderInPlace());
  EXPECT_FALSE(header.TakeMismatch());
  EXPECT_FALSE(header.TakeMismatch());
  EXPECT_EQ(header.WantedGeneration(), 4u);
}

TEST(OrcaGenericModule, RankedRoomsRequireResultsFromAnyModule)
{
  UX::GenericModule generic;
  EXPECT_EQ(Rollback::OnlineMatch::RoomRefusal("ranked", generic.Describe().results),
            "This game doesn't report match results, so it can't play Ranked. Try Casual or "
            "With Friends.");
  for (const bool results : {false, true})
  {
    EXPECT_TRUE(Rollback::OnlineMatch::RoomRefusal("private", results).empty());
    EXPECT_TRUE(Rollback::OnlineMatch::RoomRefusal("casual", results).empty());
    EXPECT_EQ(Rollback::OnlineMatch::RoomRefusal("ranked", results).empty(), results);
  }
}

TEST(OrcaGenericModule, NoCssNeverArmsOrSteers)
{
  UX::GenericModule module;
  EXPECT_EQ(module.Css(), &NoCss());
  auto& css = NoCss();
  UX::QueueApp::Identity pick;
  css.ArmOwnPick(pick);
  EXPECT_FALSE(css.OwnPickArmed());
  EXPECT_FALSE(css.OwnPickPad());
  css.DisarmOwnPick();
  EXPECT_FALSE(css.OwnPickArmed());
  EXPECT_FALSE(css.OwnPickPad());
  EXPECT_EQ(&NoCss(), &css);
}

TEST(OrcaGenericModule, BoundariesAndLatchDoNotNeedGuestMemory)
{
  BootDone done;
  UX::GenericModule module;
  SetModule(&module);
  struct CpuThread
  {
    CpuThread() { Core::DeclareAsCPUThread(); }
    ~CpuThread() { Core::UndeclareAsCPUThread(); }
  } cpu;
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS> raw;
  GCPadStatus pad;
  pad.button = PAD_BUTTON_START | PAD_TRIGGER_Z;
  pad.stickX = 255;
  pad.triggerLeft = 200;
  raw[0] = pad;
  for (const bool resimulating : {false, true})
  {
    module.OnBoundary(guard, 1801, resimulating, {}, true);
    module.Latch(guard, raw);
  }
  const auto masks = module.Masks(guard);
  for (const auto& mask : masks)
  {
    EXPECT_EQ(mask.buttons, 0);
    EXPECT_EQ(mask.press, 0);
    EXPECT_EQ(mask.a_as, 0);
    EXPECT_EQ(mask.drop_with_lr, 0);
    EXPECT_FALSE(mask.main_stick);
    EXPECT_FALSE(mask.c_stick);
    EXPECT_FALSE(mask.steer);
    EXPECT_FALSE(mask.a_centres_stick);
    const auto gated = Rollback::InputGate::Apply(pad, mask);
    EXPECT_EQ(gated.button, pad.button);
    EXPECT_EQ(gated.stickX, pad.stickX);
    EXPECT_EQ(gated.triggerLeft, pad.triggerLeft);
  }
  EXPECT_TRUE(module.SceneName(guard).empty());
  module.LoadOwnControls();
  module.SetLocalPort(3);
  EXPECT_EQ(Writes().writes, 0u);
  EXPECT_EQ(Writes().outside, 0u);
}

TEST(OrcaGenericModule, MenuReadsUseOnlyThisBootsDeclaredOriginRule)
{
  BootDone done;
  UX::GenericModule module;
  auto& read = module.Read();
  EXPECT_FALSE(read.DropInHeld());
  EXPECT_FALSE(read.OnTheMenus());
  EXPECT_FALSE(read.CssPick());
  EXPECT_FALSE(read.TakeLobbyPick());
  for (const auto pick : {UX::OnlinePick::Casual, UX::OnlinePick::Ranked})
    EXPECT_FALSE(read.PickStands(pick));
  EXPECT_EQ(read.LastSeen(), FreshSeen{});
  SetActiveProfile(std::nullopt);
  EXPECT_FALSE(read.FreshSupported());
  Profile profile;
  profile.mod.emplace();
  SetActiveProfile(profile);
  EXPECT_FALSE(read.FreshSupported());
  profile.mod->manifest.origin = Mod::OriginDecl{1801, {}, 30};
  SetActiveProfile(profile);
  EXPECT_TRUE(read.FreshSupported());
  profile.mod->manifest.builtin = Mod::Builtin::BrawlRev2;
  SetActiveProfile(profile);
  EXPECT_FALSE(read.FreshSupported());
  Mod::NoteGuards(false, profile.mod->manifest.first_keyframe_frame);
  EXPECT_TRUE(read.FreshSupported());
}

TEST(OrcaGenericModule, SelectionMatrixKeepsFlatPacksJailedInEveryBuild)
{
  Profile profile;
  EXPECT_FALSE(UX::GenericModuleEligible(nullptr, Mod::BundleKind::Flat, true, true));
  EXPECT_FALSE(UX::GenericModuleEligible(&profile, Mod::BundleKind::Flat, true, true));
  profile.mod.emplace();
  for (const auto builtin : {Mod::Builtin::None, Mod::Builtin::BrawlRev2,
                             Mod::Builtin::BrawlRev2PPlus})
  {
    profile.mod->manifest.builtin = builtin;
    for (const auto kind : {Mod::BundleKind::None, Mod::BundleKind::Tree,
                            Mod::BundleKind::Flat, Mod::BundleKind::Refused})
    {
      for (const bool jailed : {false, true})
      {
        for (const bool test_build : {false, true})
        {
          SCOPED_TRACE(static_cast<int>(builtin));
          SCOPED_TRACE(static_cast<int>(kind));
          SCOPED_TRACE(jailed);
          SCOPED_TRACE(test_build);
          const bool expected = builtin == Mod::Builtin::None &&
                                ((kind == Mod::BundleKind::Flat && jailed) ||
                                 (kind == Mod::BundleKind::Tree && test_build));
          EXPECT_EQ(UX::GenericModuleEligible(&profile, kind, jailed, test_build), expected);
        }
      }
    }
  }
}

TEST(OrcaGenericModule, LegacySelectionAndShutdownKeepTheBuiltinFallback)
{
  BootDone done;
  Profile profile;
  UX::SelectModule(&profile);
  EXPECT_EQ(ActiveModule(), &UX::Builtin());
  EXPECT_EQ(&UX::Game(), &UX::Builtin());
  UX::SelectModule(nullptr);
  EXPECT_EQ(ActiveModule(), nullptr);
  EXPECT_EQ(&UX::Game(), &UX::Builtin());
}

TEST(OrcaGenericModule, PackKeyDistinguishesGenericFromAnInactiveBuiltin)
{
  BootDone done;
  Profile profile;
  profile.game_id = "GALE01";
  profile.mod.emplace();
  profile.mod->tree.hash = "fixed-tree";
  SetActiveProfile(profile);
  SetModule(&UX::Builtin());
  ASSERT_EQ(UX::Builtin().Describe().compat, 0);
  const auto inactive_key = Online::CompatibilityKey();
  UX::GenericModule generic;
  SetModule(&generic);
  EXPECT_NE(Online::CompatibilityKey(), inactive_key);
  EXPECT_TRUE(Online::CompatibilityKeyParts().ends_with(
      Online::UxKeyPart(Events::UXCompatVersion(), 1)));
}

TEST(OrcaGenericModule, NoBrawlWarmupOrSceneReadsEvenOnBrawlsDisc)
{
  BootDone done;
  SConfig::Init();
  struct ConfigDone { ~ConfigDone() { SConfig::Shutdown(); } } config;
  UX::GenericModule module;
  SetModule(&module);
  Core::DeclareAsCPUThread();
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  // No guest memory is initialized. All scene entry points must return before any read.
  for (const char* disc : {"GALE01", "RSBE01"})
  {
    Profile profile;
    profile.game_id = disc;
    profile.revision = 2;
    profile.mod.emplace();
    SetActiveProfile(profile);
    SetModule(&module);
    EXPECT_FALSE(JitWarm::ScenesReadable());
    EXPECT_FALSE(JitWarm::SceneIsMatch(guard));
    EXPECT_FALSE(JitWarm::CurrentScene(guard));
    EXPECT_TRUE(JitWarm::SceneName(guard, 0x80000000).empty());
    JitBlock block(false);
    block.effectiveAddress = 0x80000000;
    block.physicalAddress = 0;
    block.feature_flags = {};
    block.originalSize = 1;
    block.physical_addresses.insert(0, 4);
    JitWarm::OnBlockFinalized(block);
    JitWarm::OnBoundary(guard);
    PanicDiag::NoteScene(guard, 100);
    EXPECT_EQ(Music::Supported(), profile.game_id == "RSBE01");
    EXPECT_EQ(Status::OfferedCaps().find(" music") != std::string::npos,
              profile.game_id == "RSBE01");
  }
  Core::UndeclareAsCPUThread();
}
