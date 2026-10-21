// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/BrawlModule.h"

#include <cstdlib>
#include <vector>

#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/BrawlStages.h"
#include "Core/Orca/Games/Brawl/CharOrder.h"
#include "Core/Orca/Games/Brawl/CssTitle.h"
#include "Core/Orca/Games/Brawl/MenuText.h"
#include "Core/Orca/Games/Brawl/NameTags.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/Games/Brawl/NativeText.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/OnlineSeats.h"
#include "Core/Orca/Games/Brawl/PPlus/RankedPPlus.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/RankedSet.h"
#include "Core/Orca/Games/Brawl/Relabel.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"
#include "Core/Orca/PanicDiag.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Rumble.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/PowerPC/MMU.h"

namespace Orca::UX
{
namespace
{
// The session's verdicts over Results.h's tracker and OnlineRules' no-show, as it called them.
class BrawlVerdicts final : public Orca::Engine::Verdicts
{
public:
  std::vector<GameResult> Confirm(int confirmed, int plug_frame, std::vector<int>* starts) override
  {
    return Tracker().Confirm(confirmed, plug_frame, starts);
  }
  void Reset() override { Tracker().Reset(); }
  std::optional<Orca::Engine::SceneKind> FinalScene() const override
  {
    return Tracker().FinalScene();
  }
  std::optional<Orca::Engine::SceneKind> LatestScene() const override
  {
    return Tracker().LatestScene();
  }
  std::optional<int> ConfirmNoShow(int confirmed) override
  {
    return Rules::ConfirmNoShow(confirmed);
  }
};

// The session's header over OnlineRules' (Rules::), as it called it.
class BrawlHeader final : public Orca::Engine::HeaderHost
{
public:
  bool SetWanted(Rules::Mode mode, u8 flags, std::string_view code) override
  {
    return Rules::SetWanted(mode, flags, code);
  }
  u64 WantedGeneration() override { return Rules::WantedGeneration(); }
  bool HeaderInPlace() override { return Rules::HeaderInPlace(); }
  void MemoryReplaced() override { Rules::MemoryReplaced(); }
  void SetHeaderFree(bool free) override { Rules::SetHeaderFree(free); }
  void ExpectHeader(Rules::Mode mode, std::string_view code, u8 flags, int known_games) override
  {
    Rules::ExpectHeader(mode, code, flags, known_games);
  }
  std::optional<std::string> TakeMismatch() override { return Rules::TakeMismatch(); }
};

// The session's menu reads over OnlineMenu.h, as it called them.
class BrawlMenuReads final : public Orca::Engine::MenuReads
{
public:
  bool DropInHeld() override { return UX::DropInHeld(); }
  bool OnTheMenus() override { return UX::OnTheMenus(); }
  std::optional<OnlinePick> CssPick() override { return UX::CssPick(); }
  bool PickStands(OnlinePick pick) override { return UX::PickStands(pick); }
  std::optional<OnlinePick> TakeLobbyPick() override { return UX::TakeLobbyPick(); }
  bool FreshSupported() override { return FreshMove::Supported(); }
  Orca::Engine::FreshSeen LastSeen() override { return FreshMove::LastSeen(); }
};

// The host's own pick over Queue.h, as the session called it.
class BrawlCss final : public Orca::Engine::CssAdapter
{
public:
  void ArmOwnPick(const QueueApp::Identity& pick) override { Queue::ArmOwnPick(pick); }
  void DisarmOwnPick() override { Queue::DisarmOwnPick(); }
  bool OwnPickArmed() override { return Queue::OwnPickArmed(); }
  std::optional<GCPadStatus> OwnPickPad() override { return Queue::OwnPickPad(); }
};
}  // namespace

BrawlModule& Builtin()
{
  static BrawlModule s_module;
  return s_module;
}

void BrawlModule::OnBoundary(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                             const std::vector<Orca::Events::PortInfo>& ports, bool alone)
{
  // The main menu's text about going online.
  WriteMenuText(guard);
  // Read only. Tells the app what the player picked in the game's Online menu.
  ReadOnlineMenu(guard, resimulating, alone);
  // A friend who joins while the host is in the menus moves both to the With Friends character
  // select.
  FriendsMove::Frame(guard, frame, resimulating, ports);
  // A fresh start's way out of the main menu, recorded on the replay's first frame; and what the
  // session reads to find its origin and the end of a fresh start (Rollback/OnlineMatch.cpp).
  FreshMove::Frame(guard, frame);
  // Tracks a ranked set in the match block.
  RankedSet::Frame(guard, frame, resimulating);
  // Read only. Reports each game's result in a matchmade room once its frames are final.
  ReadResultsFrame(guard, frame);
  // Project+'s ranked stage flow.
  RankedPPlus::Frame(guard, frame, resimulating, ports, alone);
  // A ranked set's stage flow on vanilla Brawl's stage select.
  BrawlStages::FrameHook(guard, resimulating, ports);
  // Read only, first runs only. Publishes the stage select cursors for the overlay.
  if (!resimulating && Rules::ProfileRuleset() != Rules::Ruleset::None)
  {
    GuardMemory memory(guard);
    StageCursors::View view = Rules::ProfileRuleset() == Rules::Ruleset::Brawl ?
                                  BrawlStages::CursorView(memory, ports) :
                                  RankedPPlus::CursorView(memory, ports, frame);
    StageCursors::Publish(std::move(view));
  }
  ProbeFrame(guard, frame);
  // Read only, first runs only. One log line per scene change, for crash reports
  // (Orca/PanicDiag.h).
  if (!resimulating)
    Orca::PanicDiag::NoteScene(guard, frame);
  // Read only, first runs only. The game's rumble for this machine's own port, to the page.
  if (!resimulating)
  {
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (!p.remote)
        Rumble::Frame(guard, p.port);
    }
  }
  // Local display only. The overlay matches the session's game.
  Kit::SetLook(Rules::ProfileRuleset() == Rules::Ruleset::PPlus ? Kit::Look::ProjectPlus :
                                                                  Kit::Look::Brawl);
  // Test only, ORCA_UX_TEST_LATE_NAME=<frame>. Simulates a friend's name arriving late: first
  // runs before that frame see it empty and re-runs see it, to show the desync the session's
  // synced value channel prevents.
  static const int s_late = [] {
    const char* v = std::getenv("ORCA_UX_TEST_LATE_NAME");
    return v ? std::atoi(v) : -1;
  }();
  // Checked every frame because a host that boots solo can have a friend join later.
  std::vector<Orca::Events::PortInfo> early;
  const bool late = s_late >= 0 && !resimulating && frame < s_late && TestKnobsAllowed();
  if (late)
  {
    early = ports;
    // PortInfo.remote differs per machine: the friend is whoever is not on port 1.
    for (Orca::Events::PortInfo& p : early)
    {
      if (p.port != 0)
      {
        p.name.clear();
        p.controls.clear();
      }
    }
  }
  // The name tags, then (read only, first runs only, while playing alone: it keeps and prints
  // changes) this player's own controls, so the session can carry them into a game it joins or
  // hosts. The read comes after the writes so it never mistakes a tag Orca is about to give
  // their controls for a change back to the game's defaults.
  NameTagsFrameHook(guard, late ? early : ports, ports, alone && !resimulating);
  // The online rules: rule, stage and ready-timer locks for queue rooms.
  u8 plugged = 0;
  for (const Orca::Events::PortInfo& p : ports)
  {
    if (p.port >= 0 && p.port < 4)
      plugged |= static_cast<u8>(1 << p.port);
  }
  Rules::OnFrame(guard, frame, resimulating, alone, plugged);
  // No CPUs online: clears CPUs from the character select's record and panels (OnlineSeats.h).
  OnlineSeats::ClearFrame(guard, frame, resimulating, plugged);
  // A ranked set's character order for later games. The input gate reads its state from the
  // match block.
  CharOrder::Frame(guard, frame, resimulating, ports);
  // The queue's character select: ready flags, timer and the joiner's pick. Runs after the
  // character order, which a ranked later game's ready timer waits for.
  Queue::Frame(guard, frame, resimulating, ports, alone);
  // The character select's title: CASUAL, RANKED or FRIENDS.
  CssTitle::Frame(guard, resimulating, ports);
  // Relabels the game's big titles, such as READY TO FIGHT! and STAGE SELECT.
  const Relabel::Shown labels = Relabel::Frame(guard, resimulating, ports);
  // Orca's text in the character select's own text boxes: player names and the rules bar line.
  NativeText::Frame(guard, resimulating, ports, Relabel::ClockInBand(labels));
  // Only the seats in use on an online character select: hides empty panels nobody plugged into
  // (OnlineSeats.h).
  OnlineSeats::SeatsFrame(guard, frame, resimulating, plugged);
}

Rollback::InputGate::Masks BrawlModule::Masks(const Core::CPUThreadGuard& guard)
{
  Rollback::InputGate::Masks masks{};
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return masks;
  GuardMemory memory(guard);
  // No Z on the results screen in any session. Its replay save is a dead end online.
  Orca::Engine::MergeMasks(masks, Rules::SessionMasks(memory));
  Orca::Engine::MergeMasks(masks, Rules::GateMasks(memory));
  // No CPUs online: where no header locks, A never reaches a player-type or name button, and is
  // pressed for a plugged-in player whose panel a CPU left (OnlineSeats.h).
  Orca::Engine::MergeMasks(masks, OnlineSeats::GateMasks(memory));
  Orca::Engine::MergeMasks(masks, RankedPPlus::Masks(memory));
  BrawlStages::GateMasks(guard, &masks);
  Rollback::InputGate::Masks order = CharOrder::GateFrame(guard);
  // On a queue room's character select the ready timer presses Start, so the character order's
  // own Start is dropped.
  if (Queue::OwnsStart(guard))
  {
    for (Rollback::InputGate::Mask& mask : order)
      mask.press &= static_cast<u16>(~PAD_BUTTON_START);
  }
  Orca::Engine::MergeMasks(masks, order);
  Orca::Engine::MergeMasks(masks, Queue::GateFrame(guard));
  return masks;
}

Orca::Engine::Verdicts& BrawlModule::Results()
{
  static BrawlVerdicts s_verdicts;
  return s_verdicts;
}

Orca::Engine::HeaderHost& BrawlModule::Header()
{
  static BrawlHeader s_header;
  return s_header;
}

Orca::Engine::MenuReads& BrawlModule::Read()
{
  static BrawlMenuReads s_reads;
  return s_reads;
}

Orca::Engine::CssAdapter* BrawlModule::Css()
{
  static BrawlCss s_css;
  return &s_css;
}

void BrawlModule::LoadOwnControls()
{
  UX::LoadOwnControls();
}

void BrawlModule::SetLocalPort(int port)
{
  RankedSet::SetLocalPort(port);
}

Orca::Engine::Display BrawlModule::Show()
{
  Orca::Engine::Display d;
  const u8 native = NativeText::Shown();
  d.plates = native & 0x0F;
  d.rules_bar = (native & NativeText::SHOWN_RULES) != 0;
  const Relabel::Shown words = Relabel::Current();
  d.band_clock = words.band != Relabel::Band::Game && words.band_up;
  d.line_relabelled = words.line != Relabel::LINE_GAME;
  d.legend = words.legend;
  d.tip = Queue::Tip();
  const RankedPPlus::Lines ranked = RankedPPlus::CurrentLines();
  d.ranked_set = ranked.set;
  d.ranked_turn = ranked.turn;
  d.score_line = RankedSet::CurrentScoreLine();
  d.cursors = StageCursors::Current();
  const StageCursors::View& v = d.cursors;
  d.layout = StageCursors::LayoutFor(v.game);
  if (d.layout)
  {
    for (int port = 0; port < 2; ++port)
    {
      const StageCursors::View::Player& p = v.players[port];
      if (!p.present || !Ranked::Named(p.proposal))
        continue;
      const StageCursors::Tile* tile = StageCursors::TileOf(*d.layout, p.proposal, v.page);
      d.proposals[port] = tile ? &tile->rect : nullptr;
    }
    for (int port = 0; port < 2; ++port)
      d.cues[port] = StageCursors::TurnText(v, port);
    d.mk_note = StageCursors::MkClauseNote(v);
  }
  d.picker = CharOrder::CurrentPicker();
  if (d.picker >= 0)
    d.picker_ribbon = CharOrder::CurrentPickerRibbon();
  d.dots = RankedSet::CurrentDots();
  return d;
}

Orca::Engine::Manifest BrawlModule::Describe()
{
  return BrawlGame::Identify(Orca::ActiveProfile());
}

bool BrawlModule::Active()
{
  return BrawlGame::On(Describe());
}

std::span<const Orca::Engine::Region> BrawlModule::Regions()
{
  return BrawlGame::Regions();
}

Orca::Engine::Ruleset BrawlModule::ProfileRuleset()
{
  // A build whose ruleset files don't load plays none (Rulesets.h): fail closed.
  if (!Rulesets::Loaded())
    return Orca::Engine::Ruleset::None;
  const Orca::Engine::Manifest game = Describe();
  return BrawlGame::On(game) ? game.ruleset : Orca::Engine::Ruleset::None;
}

bool BrawlModule::ScenesReadable(std::string_view running_game, u16 running_revision)
{
  return BrawlGame::ScenesReadable(Orca::ActiveProfile(), running_game, running_revision);
}

// Super Smash Bros. Brawl (NTSC-U, revision 2): gfSceneManager* at 0x805a0060, its current scene
// at +0x4, and the scene's name (char*) at +0x0, e.g. scTitle, scSelctCharacter, scMelee.
std::string BrawlModule::SceneName(const Core::CPUThreadGuard& guard)
{
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, 0x805a0060);
  if (manager < 0x80000000u)
    return {};
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (scene < 0x80000000u)
    return {};
  const u32 name_ptr = PowerPC::MMU::HostRead<u32>(guard, scene);
  if (name_ptr < 0x80000000u)
    return {};
  std::string name;
  for (u32 i = 0; i < 32; ++i)
  {
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name_ptr + i));
    if (c == '\0')
      break;
    name.push_back(c);
  }
  return name;
}

void BrawlModule::Latch(
    const Core::CPUThreadGuard& guard,
    const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw)
{
  Queue::LatchFrame(guard, raw);
  // On the stage select, each player's stick moves their own cursor.
  StageCursors::LatchFrame(guard, raw);
}
}  // namespace Orca::UX
