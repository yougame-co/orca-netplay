// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The origin and fresh starts (Core/Rollback/OnlineMatch.h, ORCA.md "Drop-in"): every Orca boots
// the same way to its origin at the built main menu, and a host goes back there before a queue
// match, or before a friend joins a game it has played alone for a while, so a join replays only the
// match's own frames.

#include <chrono>
#include <map>
#include <optional>

#include <gtest/gtest.h>

#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/System.h"

using namespace Rollback::OnlineMatch;

namespace
{
// A solo host on the menus with an origin, nobody arriving.
FreshInputs SoloHost(int history_frames = 10 * 60 * 60)
{
  FreshInputs in;
  in.enabled = true;
  in.origin_ready = true;
  in.history_frames = history_frames;
  return in;
}
}  // namespace

TEST(OrcaCanonicalBoot, EveryBootPlaysPort1WithNothingPressedAndNobodyElse)
{
  const Orca::Net::Pads pads = CanonicalBootPads();
  EXPECT_EQ(pads[0], Orca::Net::Pad{});
  const GCPadStatus one = Orca::Net::DecodePad(pads[0]);
  EXPECT_TRUE(one.isConnected);
  EXPECT_EQ(one.button, 0);
  // As values: the header's constants have no definition to bind to.
  EXPECT_EQ(int{one.stickX}, int{GCPadStatus::MAIN_STICK_CENTER_X});
  EXPECT_EQ(int{one.stickY}, int{GCPadStatus::MAIN_STICK_CENTER_Y});
  for (int port = 1; port < Orca::Net::MAX_SEATS; ++port)
  {
    EXPECT_EQ(pads[port], Orca::Net::UNPLUGGED_PAD) << port;
    EXPECT_FALSE(Orca::Net::DecodePad(pads[port]).isConnected) << port;
  }
}

TEST(OrcaCanonicalBoot, TheOriginIsTheMainMenuBuiltForHalfASecond)
{
  // 30 frames of a built main menu make the origin (Brawl Rev 2 measures 1524, Project+ 230).
  EXPECT_EQ(DecideOrigin(1441, 0, std::nullopt), OriginStep::Wait);
  EXPECT_EQ(DecideOrigin(1470, ORIGIN_SETTLE_FRAMES - 1, std::nullopt), OriginStep::Wait);
  EXPECT_EQ(DecideOrigin(1471, ORIGIN_SETTLE_FRAMES, std::nullopt), OriginStep::Capture);
  // Project+: after its JIT clear (frame 2).
  EXPECT_EQ(DecideOrigin(229, ORIGIN_SETTLE_FRAMES, 2u), OriginStep::Capture);
  EXPECT_EQ(DecideOrigin(2, ORIGIN_SETTLE_FRAMES, 2u), OriginStep::Wait);
  // No built menu in time: no origin.
  EXPECT_EQ(DecideOrigin(ORIGIN_DEADLINE - 1, 3, std::nullopt), OriginStep::Wait);
  EXPECT_EQ(DecideOrigin(ORIGIN_DEADLINE, 3, std::nullopt), OriginStep::Fail);
  EXPECT_EQ(DecideOrigin(ORIGIN_DEADLINE, ORIGIN_SETTLE_FRAMES, std::nullopt), OriginStep::Capture);
}

TEST(OrcaFreshStart, AQueueRoomsHostFreshStartsOnceItsWelcomeCame)
{
  FreshInputs in = SoloHost();
  in.queue_armed = true;
  // `host` came, the welcome hasn't: no keyframe yet.
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.waiting = true;  // the opponent's hello came first
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.in_queue_room = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  // However short the host's history.
  in.history_frames = 3;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  // Under way (or a keyframe being made): wait for it.
  in.busy = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  // Done (the arm is spent): the opponent joins in place, never a second fresh start in that room.
  in.busy = false;
  in.queue_armed = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.history_frames = 40 * 60 * 60;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
}

TEST(OrcaFreshStart, AFriendAfterTenSecondsFreshStartsTheHost)
{
  FreshInputs in = SoloHost(FRESH_FRIENDS_AFTER + 1);
  // Nobody arriving: nothing.
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.waiting = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  // A host that just booted or just fresh-started takes the friend in place.
  in.history_frames = FRESH_FRIENDS_AFTER;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.history_frames = 120;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  // The test knob's threshold.
  in.fresh_after = 60;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  in.fresh_after = 0;
  in.history_frames = 1;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaFreshStart, AFriendTakenInPlaceStaysInPlace)
{
  // Arrived at 9 s of history: in place. Its keyframe takes a second to store, and the history
  // passes 10 s meanwhile: still in place, no fresh start under it.
  FreshInputs in = SoloHost(FRESH_FRIENDS_AFTER - 60);
  in.waiting = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.in_place = true;
  in.history_frames = FRESH_FRIENDS_AFTER + 60;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  // The next friend, once nobody waits, decides again.
  in.in_place = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaFreshStart, ASinglePlayerModeHoldsTheFriendFirst)
{
  FreshInputs in = SoloHost();
  in.waiting = true;
  in.single_player = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.single_player = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaFreshStart, NeverInASessionOrWhileJoining)
{
  for (int which = 0; which < 3; ++which)
  {
    FreshInputs in = SoloHost();
    in.waiting = true;
    in.queue_armed = which == 0;
    in.in_queue_room = which == 0;
    in.session = which == 0;
    in.joining = which == 1;
    in.seated_or_plugging = which == 2;
    EXPECT_EQ(DecideFreshStart(in), FreshStep::None) << which;
  }
}

TEST(OrcaFreshStart, NoOriginMeansWaitingAndTheKnobTurnsItOff)
{
  FreshInputs in = SoloHost();
  in.waiting = true;
  in.origin_ready = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.origin_ready = true;
  in.busy = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  // ORCA_TEST_FRESH=off, or a game whose origin is its first frame: joins in place.
  in.busy = false;
  in.enabled = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.queue_armed = true;
  in.in_queue_room = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
}

TEST(OrcaFreshStart, AFriendInAQueueRoomIsTheQueuesBusiness)
{
  // A queue room's opponent never takes the friends path.
  FreshInputs in = SoloHost();
  in.waiting = true;
  in.in_queue_room = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
}

TEST(OrcaFreshStart, AFriendLandsOnTheSelectTheHostIsOn)
{
  EXPECT_EQ(FriendsFreshExit(std::nullopt), Orca::Net::MENU_EXIT_FRIENDS);
  EXPECT_EQ(FriendsFreshExit(false), Orca::Net::MENU_EXIT_CASUAL);
  EXPECT_EQ(FriendsFreshExit(true), Orca::Net::MENU_EXIT_RANKED);
  for (const u8 exit : {Orca::Net::MENU_EXIT_FRIENDS, Orca::Net::MENU_EXIT_CASUAL,
                        Orca::Net::MENU_EXIT_RANKED})
  {
    EXPECT_TRUE(Orca::Net::ValidMenuExit(exit));
  }
  EXPECT_FALSE(Orca::Net::ValidMenuExit(0));
  EXPECT_FALSE(Orca::Net::ValidMenuExit(24));
}

// A queue room's host runs its hold for the room's header and its own pick being put back unseen
// and unthrottled, like the tail, but never longer than FRESH_UNSEEN_LIMIT frames; a friend's fresh
// start has neither, so nothing of it runs unseen past the tail.
TEST(OrcaFreshStart, AQueueHostsHoldAndSteerRunUnseenUpToALimit)
{
  for (int frames = 0; frames < FRESH_UNSEEN_LIMIT; ++frames)
    EXPECT_TRUE(FreshUnseen(true, frames)) << frames;
  EXPECT_FALSE(FreshUnseen(true, FRESH_UNSEEN_LIMIT));
  EXPECT_FALSE(FreshUnseen(true, FRESH_UNSEEN_LIMIT + 1000));
  EXPECT_FALSE(FreshUnseen(false, 0));
  // About two seconds at 60 fps, well past the ~70 frames the steer takes, far short of the
  // steer's own 10 s limit, which would freeze the picture for seconds.
  EXPECT_GE(FRESH_UNSEEN_LIMIT, 100);
  EXPECT_LE(FRESH_UNSEEN_LIMIT, 180);
}

// The header goes in during the unseen hold (it is a first run, so the hook writes and records it),
// never in a tail (a re-run) or a joiner's rebuild.
TEST(OrcaFreshStart, TheHeaderGoesInOnlyDuringTheUnseenHold)
{
  EXPECT_TRUE(HeaderFreeWhileCatchingUp(true, false, false));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(false, false, false));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(true, true, false));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(true, false, true));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(false, true, true));
}

// The queue room's header limit counts only boundaries that run at the game's pace: a tail's and
// an unseen hold's don't count, so the join never fails sooner in real time because they ran
// unthrottled. Once the header is in, the hold is ready at once, unseen or not.
TEST(OrcaFreshStart, UnseenBoundariesNeverCountTowardTheHeaderLimit)
{
  int waited = 0;
  for (int i = 0; i < 10 * HEADER_FAIL_BOUNDARIES; ++i)
  {
    ASSERT_EQ(HeaderWaitAt(&waited, true, false, false, true, true), HeaderWait::Wait);
    ASSERT_EQ(HeaderWaitAt(&waited, false, true, false, true, true), HeaderWait::Wait);
  }
  EXPECT_EQ(waited, 0);
  EXPECT_EQ(HeaderWaitAt(&waited, false, true, true, true, true), HeaderWait::Ready);
  // Shown again with the header still not in: the usual limit, counted from there.
  for (int i = 0; i < HEADER_FAIL_BOUNDARIES; ++i)
    ASSERT_EQ(HeaderWaitAt(&waited, false, false, false, true, true), HeaderWait::Wait) << i;
  EXPECT_EQ(HeaderWaitAt(&waited, false, false, false, true, true), HeaderWait::Fail);
  // Unchanged outside a fresh start: a friends room makes the keyframe anyway after its wait.
  waited = 0;
  for (int i = 0; i < HEADER_WAIT_BOUNDARIES; ++i)
    ASSERT_EQ(HeaderWaitAt(&waited, false, false, false, true, false), HeaderWait::Wait) << i;
  EXPECT_EQ(HeaderWaitAt(&waited, false, false, false, true, false), HeaderWait::Ready);
}

namespace
{
class OriginMemory final : public Orca::UX::GuestMemory
{
public:
  std::map<u32, u32> words;
  bool Valid(u32 address) const override { return words.contains(address); }
  u32 Read32(u32 address) const override { return words.at(address); }
  u16 Read16(u32) const override { ADD_FAILURE(); return 0; }
  u8 Read8(u32) const override { ADD_FAILURE(); return 0; }
  void Write8(u32, u8) override { ADD_FAILURE(); }
  void Write16(u32, u16) override { ADD_FAILURE(); }
  void Write32(u32, u32) override { ADD_FAILURE(); }
};
}

TEST(OrcaDeclaredOrigin, ExactlyTheDeclaredBoundaryWithoutWords)
{
  const Orca::Mod::OriginDecl decl{1801, {}, 30};
  EXPECT_EQ(DecideDeclaredOrigin(1800, 0, decl), OriginStep::Wait);
  EXPECT_EQ(DecideDeclaredOrigin(1801, 0, decl), OriginStep::Capture);
  EXPECT_EQ(DecideDeclaredOrigin(1802, 0, decl), OriginStep::Fail);
  for (const u32 frame : {1u, 36000u})
    EXPECT_EQ(DecideDeclaredOrigin(frame, 0, {frame, {}, 30}), OriginStep::Capture);
}

TEST(OrcaDeclaredOrigin, WordsMustHoldConsecutivelyAndMappedZeroIsDistinct)
{
  const Orca::Mod::OriginDecl decl{100, {{0x80000000, 0}, {0x90000000, 42}}, 3};
  OriginMemory memory;
  memory.words = {{0x80000000, 0}, {0x90000000, 42}};
  EXPECT_EQ(DeclaredOriginStreak(99, 2, decl, memory), 0);
  int streak = DeclaredOriginStreak(100, 0, decl, memory);
  EXPECT_EQ(streak, 1);
  EXPECT_EQ(DecideDeclaredOrigin(100, streak, decl), OriginStep::Wait);
  streak = DeclaredOriginStreak(101, streak, decl, memory);
  EXPECT_EQ(streak, 2);
  memory.words[0x90000000] = 43;
  streak = DeclaredOriginStreak(102, streak, decl, memory);
  EXPECT_EQ(streak, 0);
  memory.words[0x90000000] = 42;
  for (int frame = 103; frame <= 105; ++frame)
    streak = DeclaredOriginStreak(frame, streak, decl, memory);
  EXPECT_EQ(DecideDeclaredOrigin(105, streak, decl), OriginStep::Capture);
  EXPECT_EQ(DeclaredOriginStreak(106, streak, decl, memory), 3);
  memory.words.erase(0x80000000);
  EXPECT_EQ(DeclaredOriginStreak(107, streak, decl, memory), 0);
}

TEST(OrcaDeclaredOrigin, SettleBoundsAndWindow)
{
  for (const u32 settle : {1u, 30u, 600u})
  {
    const Orca::Mod::OriginDecl decl{36000, {{0x80000000, 1}}, settle};
    EXPECT_EQ(DecideDeclaredOrigin(35999, settle, decl), OriginStep::Wait);
    EXPECT_EQ(DecideDeclaredOrigin(36000, settle - 1, decl), OriginStep::Wait);
    EXPECT_EQ(DecideDeclaredOrigin(36000, settle, decl), OriginStep::Capture);
    EXPECT_EQ(DecideDeclaredOrigin(36000 + ORIGIN_WORDS_WINDOW - 1, 0, decl), OriginStep::Wait);
    EXPECT_EQ(DecideDeclaredOrigin(36000 + ORIGIN_WORDS_WINDOW, 0, decl), OriginStep::Fail);
    EXPECT_EQ(DecideDeclaredOrigin(36001 + ORIGIN_WORDS_WINDOW, 0, decl), OriginStep::Fail);
    EXPECT_EQ(DecideDeclaredOrigin(36000 + ORIGIN_WORDS_WINDOW, settle, decl), OriginStep::Capture);
  }
}

TEST(OrcaDeclaredOrigin, RuleSelectionMatrix)
{
  EXPECT_EQ(SelectOriginRule(nullptr, true, false), OriginRule::Menu);
  EXPECT_EQ(SelectOriginRule(nullptr, false, false), OriginRule::Frame0);
  for (const bool builtin : {false, true})
  {
    for (const bool declared : {false, true})
    {
      for (const bool failed : {false, true})
      {
        Orca::Mod::Manifest manifest;
        if (builtin)
          manifest.builtin = Orca::Mod::Builtin::BrawlRev2;
        if (declared)
          manifest.origin = Orca::Mod::OriginDecl{1801, {}, 30};
        const OriginRule expected = builtin && !failed ? OriginRule::Menu :
                                    declared ? OriginRule::Declared :
                                    builtin ? OriginRule::Failed : OriginRule::Frame0;
        for (const bool supported : {false, true})
          EXPECT_EQ(SelectOriginRule(&manifest, supported, failed), expected);
      }
    }
  }
}

TEST(OrcaResetStart, OnlyDeclaredOriginsUseResetAndExitZero)
{
  for (const auto rule : {OriginRule::Menu, OriginRule::Declared, OriginRule::Frame0,
                          OriginRule::Failed})
    EXPECT_EQ(FreshKindForOrigin(rule), rule == OriginRule::Declared ? FreshKind::Reset :
                                                                   FreshKind::Menu);
  for (unsigned exit = 0; exit <= 255; ++exit)
  {
    EXPECT_EQ(FreshExitAllowed(FreshKind::Reset, static_cast<u8>(exit)), exit == 0);
    EXPECT_EQ(FreshExitAllowed(FreshKind::Menu, static_cast<u8>(exit)),
              Orca::Net::ValidMenuExit(exit));
  }
}

TEST(OrcaResetStart, FriendsJoinInPlaceThroughThreeMinutesAtAnyFrameRate)
{
  EXPECT_EQ(FRESH_RESET_FRIENDS_AFTER, std::chrono::seconds(180));
  FreshInputs in = SoloHost();
  in.kind = FreshKind::Reset;
  in.waiting = true;
  for (const int frames_per_second : {30, 50, 60, 120})
  {
    for (const int seconds : {0, 10, 179, 180, 181})
    {
      in.history_frames = seconds * frames_per_second;
      in.history_time = std::chrono::seconds(seconds);
      EXPECT_EQ(DecideFreshStart(in), seconds > 180 ? FreshStep::Now : FreshStep::None);
    }
  }
  // A paused host or a held game frame does not turn wall time into replay history.
  in.history_frames = 100000;
  in.history_time = std::chrono::seconds(1);
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.history_frames = 1;
  in.history_time = std::chrono::seconds(181);
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  in.in_place = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.in_place = false;
  in.history_frames = 2;
  in.fresh_after = 1;  // The explicit test knob still counts frames.
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaResetStart, QueueRestartsOnceAndOnlyASoloHostCanRestart)
{
  FreshInputs in = SoloHost(1);
  in.kind = FreshKind::Reset;
  in.queue_armed = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.in_queue_room = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  for (bool FreshInputs::* blocked : {&FreshInputs::joining, &FreshInputs::session,
                                      &FreshInputs::seated_or_plugging})
  {
    in.*blocked = true;
    EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
    in.*blocked = false;
  }
  in.busy = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.busy = false;
  in.origin_ready = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.origin_ready = true;
  in.enabled = false;  // a frame-0 origin never enables fresh starts
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.enabled = true;
  in.queue_armed = false;
  in.waiting = true;
  in.history_frames = 20 * 60 * 60;
  in.history_time = std::chrono::minutes(20);
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.in_queue_room = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaResetStart, TailSettlesImmediatelyWithoutACharacterSelect)
{
  for (const int length : {0, 1, FRESH_TAIL_LIMIT - 1, FRESH_TAIL_LIMIT})
    EXPECT_TRUE(FreshTailSettled(FreshKind::Reset, 0, length));
  EXPECT_FALSE(FreshTailSettled(FreshKind::Menu, FRESH_CSS_SETTLE - 1,
                               FRESH_TAIL_LIMIT - 1));
  EXPECT_TRUE(FreshTailSettled(FreshKind::Menu, FRESH_CSS_SETTLE, 1));
  EXPECT_TRUE(FreshTailSettled(FreshKind::Menu, 0, FRESH_TAIL_LIMIT));
}

TEST(OrcaResetStart, NewHistoryRecordsNoMenuExitOrSeed)
{
  auto reset = FreshReplayStart(FreshKind::Reset, 1801, 0x12345678, 0, 999);
  EXPECT_EQ(reset.origin_hash, 0x12345678u);
  EXPECT_EQ(reset.first_frame, 1801);
  EXPECT_EQ(reset.EndFrame(), 1801);
  EXPECT_TRUE(reset.frames.empty());
  EXPECT_EQ(reset.boundary.menu_exit, 0);
  EXPECT_EQ(reset.boundary.seed, 0u);
  {
    Orca::Net::ReplayRecordingScope recording(&reset);
    auto* first = Orca::Net::ReplayRecordingScope::Frame(1801);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->menu_exit, 0);
    EXPECT_EQ(first->seed, 0u);
    EXPECT_FALSE(first->header);
    Orca::Net::ReplayRecordingScope::RecordPads(1801, CanonicalBootPads());
  }
  EXPECT_TRUE(Orca::Net::HeadersFitRuleset(reset, 0));
  // Brawl's existing typed events retain their exact first-frame values.
  for (const u8 exit : {Orca::Net::MENU_EXIT_FRIENDS, Orca::Net::MENU_EXIT_CASUAL,
                        Orca::Net::MENU_EXIT_RANKED})
  {
    const auto menu = FreshReplayStart(FreshKind::Menu, 230, 0x12345678, exit, 999);
    ASSERT_EQ(menu.frames.size(), 1u);
    EXPECT_EQ(menu.frames[0].menu_exit, exit);
    EXPECT_EQ(menu.frames[0].seed, 999u);
    EXPECT_EQ(menu.origin_hash, 0x12345678u);
    EXPECT_EQ(menu.first_frame, 230);
  }
}

TEST(OrcaOriginNand, GameCubeNeedsNoIosAndWiiKeepsItsSessionRootRequirement)
{
  // The preflight used by capture, restore, and the joiner, with a real no-IOS system.
  auto& system = Core::System::GetInstance();
  ASSERT_EQ(system.GetIOS(), nullptr);
  struct RestoreBootKind
  {
    Core::System& system;
    bool is_wii;
    ~RestoreBootKind() { system.SetIsWii(is_wii); }
  } restore{system, system.IsWii()};
  system.SetIsWii(false);
  EXPECT_TRUE(LocalNandReady(system.IsWii(), system.GetIOS() != nullptr, false));
  EXPECT_TRUE(LocalNandReady(system.IsWii(), system.GetIOS() != nullptr, true));
  for (const bool has_nand : {false, true})
  {
    for (const bool temporary : {false, true})
    {
      EXPECT_TRUE(LocalNandReady(false, has_nand, temporary));
      EXPECT_EQ(LocalNandReady(true, has_nand, temporary), has_nand && temporary);
    }
  }
  system.SetIsWii(true);
  EXPECT_FALSE(LocalNandReady(system.IsWii(), system.GetIOS() != nullptr, true));
}

TEST(OrcaResetStart, TheHostExplainsTheRestartToItsPlayer)
{
  const auto now = [] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  Orca::UX::ShowRestartForJoin("Ada");
  auto view = Orca::UX::Overlay().View(now(), Orca::UX::Stats::Off);
  ASSERT_FALSE(view.toasts.empty());
  EXPECT_EQ(view.toasts.back().text, "Restarting the game in 3 seconds so Ada can join. Unsaved progress will be lost.");
  Orca::UX::ShowRestartForJoin("");
  view = Orca::UX::Overlay().View(now(), Orca::UX::Stats::Off);
  ASSERT_FALSE(view.toasts.empty());
  EXPECT_EQ(view.toasts.back().text, "Restarting the game in 3 seconds so your friend can join. Unsaved progress will be lost.");
  Orca::UX::ShowRestartForMatch();
  view = Orca::UX::Overlay().View(now(), Orca::UX::Stats::Off);
  ASSERT_FALSE(view.toasts.empty());
  EXPECT_EQ(view.toasts.back().text, "Restarting the game for this match");
}

TEST(OrcaResetStart, WarnsBeforeSchedulingAFriendsReset)
{
  using namespace std::chrono_literals;
  FreshInputs in = SoloHost();
  in.kind = FreshKind::Reset;
  in.waiting = true;
  in.history_time = FRESH_RESET_FRIENDS_AFTER + 1s;
  const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
  std::optional<std::chrono::steady_clock::time_point> warned_at;
  const auto step = [&](auto now) {
    return WarnBeforeResetJoin(DecideFreshStart(in), in.kind, in.queue_armed, &warned_at, now);
  };
  EXPECT_EQ(step(t0), FreshStep::Warn);
  ASSERT_EQ(warned_at, t0);
  EXPECT_EQ(step(t0), FreshStep::Hold);
  EXPECT_EQ(step(t0 + 2999ms), FreshStep::Hold);
  EXPECT_EQ(step(t0 + 3s), FreshStep::Now);
  EXPECT_EQ(step(t0 + 4s), FreshStep::Now);
  // Leaving cancels the pending warning. A later arrival receives the full notice again.
  in.waiting = false;
  EXPECT_EQ(step(t0 + 5s), FreshStep::None);
  EXPECT_FALSE(warned_at);
  in.waiting = true;
  EXPECT_EQ(step(t0 + 6s), FreshStep::Warn);
  // A single-player hold also cancels it: warn anew once resetting becomes possible again.
  in.single_player = true;
  EXPECT_EQ(step(t0 + 7s), FreshStep::Hold);
  EXPECT_FALSE(warned_at);
  in.single_player = false;
  EXPECT_EQ(step(t0 + 8s), FreshStep::Warn);
  in.queue_armed = in.in_queue_room = true;
  EXPECT_EQ(step(t0 + 8s), FreshStep::Now);
  EXPECT_FALSE(warned_at);
  in.queue_armed = in.in_queue_room = false;
  in.kind = FreshKind::Menu;
  EXPECT_EQ(step(t0 + 8s), FreshStep::Now);
  EXPECT_FALSE(warned_at);
  in.kind = FreshKind::Reset;
  in.history_time = 1s;
  EXPECT_EQ(step(t0 + 9s), FreshStep::None);
  EXPECT_FALSE(warned_at);
}

TEST(OrcaFreshStart, SoloCoSeatsKeepSeparatePadsPortsAndNeutralTails)
{
  auto& system = Core::System::GetInstance();
  for (const bool tail : {false, true})
  {
    std::array<int, Orca::Net::MAX_SEATS> reads{};
    std::vector<Orca::Events::PortInfo> ports;
    const auto log = PumpSoloSeatsForTest(system, {2}, 3, tail,
        [&](int seat) {
          ++reads[seat];
          return Orca::Net::Pad{static_cast<u8>(0x10 + seat), 0, 0, 0,
                                static_cast<u8>(seat + 1), 0, 0, 0};
        }, &ports);
    EXPECT_EQ(reads, (std::array<int, Orca::Net::MAX_SEATS>{3, 0, 3, 0}));
    ASSERT_EQ(log.size(), 3u);
    for (const auto& pads : log)
    {
      EXPECT_EQ(pads[0][0], tail ? 0 : 0x10);
      EXPECT_EQ(pads[2][0], tail ? 0 : 0x12);
      EXPECT_EQ(pads[0][4], tail ? 0 : 1);
      EXPECT_EQ(pads[2][4], tail ? 0 : 3);
      EXPECT_EQ(pads[1], Orca::Net::UNPLUGGED_PAD);
      EXPECT_EQ(pads[3], Orca::Net::UNPLUGGED_PAD);
    }
    ASSERT_EQ(ports.size(), 2u);
    EXPECT_EQ(ports[0].port, 0);
    EXPECT_EQ(ports[1].port, 2);
    EXPECT_FALSE(ports[0].remote);
    EXPECT_FALSE(ports[1].remote);
    EXPECT_TRUE(ports[1].controls.empty());
  }
}

TEST(OrcaCoSeats, SetOverButtonsKeepTheLeadSample)
{
  for (const int lead : {0, 1})
  {
    for (const bool lead_b : {false, true})
    {
      const auto seen = PumpSeenLocalPadForTest(lead, {2, 3}, [&](int seat) {
        GCPadStatus pad;
        if ((seat == lead) == lead_b)
          pad.button = PAD_BUTTON_B;
        return Orca::Net::EncodePad(pad);
      });
      ASSERT_TRUE(seen);
      EXPECT_EQ(Orca::Net::DecodePad(*seen).button, lead_b ? PAD_BUTTON_B : 0);
    }
  }
}
