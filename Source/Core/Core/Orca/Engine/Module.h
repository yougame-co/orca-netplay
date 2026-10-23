// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Orca/Engine/Display.h"
#include "Core/Orca/Engine/MatchTypes.h"
#include "Core/Orca/Engine/Regions.h"

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/MenuEvents.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

// The seam between the engine and a game module (the Orca split, modules.md section 4). The engine
// owns the three hooks into emulation (the frame callback, the input gate's source and its latch)
// and calls the active module from each; the module owns its game's steps and their order, which
// is part of what makes two builds compatible (UX::kCompatVersion and the game's Manifest::compat).
//
// S2 commit 4: one module, the builtin Brawl/Project+ one (Games/Brawl/BrawlModule.h), whose three
// entry points are UX.cpp's three hook bodies moved verbatim. Later commits narrow the interface
// toward API v1 (Read, Css, Display, OnHeader...) and move what the session needs out of the module.
namespace Orca::Engine
{
// Verdicts on confirmed frames (S2 commit 5, group C): the module keeps a reading per frame and
// turns frames into game results once they are final, identically on every machine. CPU thread.
class Verdicts
{
public:
  virtual ~Verdicts() = default;
  // Frames through `confirmed` are final. Returns the games they finish, in order; `starts` gets
  // the ranked set games they begin (the reports' ids). Only games begun after `plug_frame` count.
  virtual std::vector<GameResult> Confirm(int confirmed, int plug_frame,
                                          std::vector<int>* starts = nullptr) = 0;
  // Memory was replaced (a keyframe or snapshot load): everything stored is stale.
  virtual void Reset() = 0;
  // The scene of the last final reading, and of the newest (possibly predicted) one.
  virtual std::optional<SceneKind> FinalScene() const = 0;
  virtual std::optional<SceneKind> LatestScene() const = 0;
  // The ranked no-show port (0-based), once frames through it are `confirmed` final.
  virtual std::optional<int> ConfirmNoShow(int confirmed) = 0;
};

// The match header the session wants in memory (S2 commit 5, group B): the module writes it at a
// solo first-run frame and checks a host's on a joiner. The session says what it wants and asks
// whether it is in place. CPU thread unless noted.
class HeaderHost
{
public:
  virtual ~HeaderHost() = default;
  // Sets the wanted header for this room (`code`: the room code, empty outside a room). Returns
  // whether it changed (WantedGeneration then increments).
  virtual bool SetWanted(Mode mode, u8 flags = 0, std::string_view code = {}) = 0;
  // Counts changes to the wanted header, so a keyframe made under an older count is stale.
  virtual u64 WantedGeneration() = 0;
  // Whether memory carried the wanted header at the last first-run frame hook, with no change.
  virtual bool HeaderInPlace() = 0;
  // Memory was replaced after this frame's hook, so the header read is stale until the next one.
  virtual void MemoryReplaced() = 0;
  // Lets this frame's hook write the header though a peer is joining and waiting for a keyframe.
  virtual void SetHeaderFree(bool free) = 0;
  // A joiner just loaded its keyframe into a room of `mode`: the host's header is checked at the
  // next frame hook (`known_games`: the ranked set's games this player knows of; -1 none).
  virtual void ExpectHeader(Mode mode, std::string_view code = {}, u8 flags = 0,
                            int known_games = -1) = 0;
  // Why the host's header didn't fit the room, once.
  virtual std::optional<std::string> TakeMismatch() = 0;
};

// What the game shows of its menus, for drop-in and fresh starts (S2 commit 5, group D). What the
// frame hook read at its last first run; CPU thread unless noted.
class MenuReads
{
public:
  virtual ~MenuReads() = default;
  // The host's game is in a mode a friend can't drop into (single player, the Vault). Any thread.
  virtual bool DropInHeld() = 0;
  // The main menu or a character select: a former joiner moves to port 1 here.
  virtual bool OnTheMenus() = 0;
  // The Casual or Ranked pick the character select was opened for, if on one.
  virtual std::optional<UX::OnlinePick> CssPick() = 0;
  // Whether a pick the session couldn't take when it was seen still stands.
  virtual bool PickStands(UX::OnlinePick pick) = 0;
  // A Casual or Ranked pick made while the game was not alone, once.
  virtual std::optional<UX::OnlinePick> TakeLobbyPick() = 0;
  // Whether the game's main menu can be read and left (fresh starts). Any thread.
  virtual bool FreshSupported() = 0;
  // What the last frame hook read for the fresh start.
  virtual FreshSeen LastSeen() = 0;
};

// The character select, for the host's own pick after a fresh start (S2 commit 5, group E): the
// session arms it with the player's queue identity, and each first-run frame hook steers port 1
// back onto that pick with this player's own inputs. CPU thread.
class CssAdapter
{
public:
  virtual ~CssAdapter() = default;
  virtual void ArmOwnPick(const UX::QueueApp::Identity& pick) = 0;
  virtual void DisarmOwnPick() = 0;
  virtual bool OwnPickArmed() = 0;
  // The pad the last first-run frame hook chose for port 1 while armed (nullopt: not steering).
  virtual std::optional<GCPadStatus> OwnPickPad() = 0;
};

// What game a session runs, as its module knows it (S3, modules.md section 4.1's Describe): fixed
// for a boot, from the profile (legacy) or mod.json's builtin (mod mode), never from memory. The
// game files and the engine ask the module for this instead of comparing game ids themselves.
struct Manifest
{
  // The game the module's code is for: mod.json's builtin name ("brawl-rev2", "brawl-rev2-pplus"),
  // or a legacy launcher profile's other build on the same executable ("brawl-rev2-launcher").
  // Empty: none of the module's games.
  std::string_view id;
  // The ruleset its online rules play (None: no locks, no queue, no ranked).
  Ruleset ruleset = Ruleset::None;
  // It reads each game's result.
  bool results = false;
  // Host only, about the disc rather than the game (true without an id too, such as for a mod with
  // no builtin on Brawl's disc): the game's music can be switched off (Core/Orca/Music.h).
  bool music = false;
  // The game's own compatibility revision (S3, WP19 a): the key's "ux=" is the engine's
  // UX::kCompatVersion, then ".<compat>" when this is above 0 (Online::UxKeyPart). A change to the
  // module's steps that changes what only this game computes bumps this, not the engine's number.
  int compat = 0;
};

class Module
{
public:
  virtual ~Module() = default;
  // A stable id ("brawl-builtin"), for logs.
  virtual std::string_view Id() const = 0;
  // What game this session runs (S3). Any thread.
  virtual Manifest Describe() = 0;
  // Whether the module's code applies to memory now: a described game whose addresses hold (mod
  // mode: the builtin's guard words held at the last Mod::RefreshGuards; legacy: always, for a
  // described game). A pure function of memory on first runs and re-runs. Any thread.
  virtual bool Active() = 0;
  // The guest memory its steps write (S3, record-only): every write through GuardMemory outside
  // these is counted and logged, never refused (Engine/Regions.h). Static: the engine keeps the
  // span.
  virtual std::span<const Region> Regions() = 0;
  // Every frame boundary, first runs and rollback re-runs, before the session saves state there.
  // Every memory write must be a pure function of emulated memory, the frame number and the synced
  // ports, so a re-run writes exactly what the first run did. Runs after the engine's own
  // ApplyGamePatches and before its ApplyToggledGamePatches.
  virtual void OnBoundary(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                          const std::vector<Events::PortInfo>& ports, bool alone) = 0;
  // The input gate's masks for the frame about to run, from memory alone.
  virtual Rollback::InputGate::Masks Masks(const Core::CPUThreadGuard& guard) = 0;
  // The frame's raw buttons, before it runs (writes the module's latch regions only).
  virtual void Latch(const Core::CPUThreadGuard& guard,
                     const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) = 0;

  // What the session asks of the module (S2 commit 5 routes Rollback/OnlineMatch.cpp through these).
  virtual Verdicts& Results() = 0;
  virtual HeaderHost& Header() = 0;
  virtual MenuReads& Read() = 0;
  // Never null: modules without a character select return NoCss().
  virtual CssAdapter* Css() = 0;
  // This player's own controls, before a launch join's hello carries them (group F).
  virtual void LoadOwnControls() = 0;
  // The port this machine plays (0-based), for the module's local-only display state (group F).
  virtual void SetLocalPort(int port) = 0;
  // What the overlay shows now (host only, any thread the overlay draws on; S2 commit 6).
  virtual Display Show() = 0;
  // The ruleset the running game plays (None: no locks). Part of the compatibility key.
  virtual Ruleset ProfileRuleset() = 0;
  // Host only, for the harness's scene-relative inputs and logs (Rollback/Harness.cpp, S3): whether
  // the running game's scene names can be read, given the game ID and revision Dolphin runs (the
  // engine's settings), and the scene it shows now (empty: none readable).
  virtual bool ScenesReadable(std::string_view running_game, u16 running_revision) = 0;
  virtual std::string SceneName(const Core::CPUThreadGuard& guard) = 0;
};

// A character select adapter for modules that have none. Host-only, with no state or guest writes.
CssAdapter& NoCss();

// The module the engine's hooks call. Selected after the boot profile exists and cleared by
// UX::Shutdown after they are removed; nullptr: the hooks do the engine's part only.
void SetModule(Module* module);
Module* ActiveModule();

// ORs `more` into `masks`: an input any rule takes is taken.
void MergeMasks(Rollback::InputGate::Masks& masks, const Rollback::InputGate::Masks& more);
}  // namespace Orca::Engine
