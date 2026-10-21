// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The input gate (Rollback/InputGate.h, ORCA.md "Input gate"): what a mask does to a pad, that the
// masks follow the state at every boundary and nothing else, that the game's read is gated while
// the raw read (the wire's, a recording's) is not, and the ORCA_TEST_GATE parser.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Core.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Rollback/InputGate.h"
#include "Core/Rollback/Rollback.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

using namespace Rollback::InputGate;

namespace
{
GCPadStatus Busy()
{
  GCPadStatus pad;
  pad.button = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_START | PAD_TRIGGER_L |
               PAD_TRIGGER_R | PAD_TRIGGER_Z | PAD_BUTTON_UP | PAD_USE_ORIGIN | PAD_GET_ORIGIN;
  pad.stickX = 20;
  pad.stickY = 230;
  pad.substickX = 255;
  pad.substickY = 0;
  pad.triggerLeft = 200;
  pad.triggerRight = 140;
  pad.analogA = 90;
  pad.analogB = 80;
  pad.isConnected = true;
  return pad;
}

bool Same(const GCPadStatus& a, const GCPadStatus& b)
{
  return a.button == b.button && a.stickX == b.stickX && a.stickY == b.stickY &&
         a.substickX == b.substickX && a.substickY == b.substickY &&
         a.triggerLeft == b.triggerLeft && a.triggerRight == b.triggerRight &&
         a.analogA == b.analogA && a.analogB == b.analogB && a.isConnected == b.isConnected;
}

// Leaves no source and no masks behind, whatever the test did.
class OrcaInputGateBoundary : public ::testing::Test
{
protected:
  void TearDown() override
  {
    SetSource({});
    Clear();
  }
  static void Boundary()
  {
    const Core::CPUThreadGuard guard(Core::System::GetInstance());
    OnBoundary(guard);
  }
};
}  // namespace

TEST(OrcaInputGate, EmptyMaskChangesNothing)
{
  const GCPadStatus pad = Busy();
  EXPECT_TRUE(Same(Apply(pad, Mask{}), pad));
}

TEST(OrcaInputGate, ButtonsReleasedTheRestKept)
{
  const GCPadStatus pad = Busy();
  const GCPadStatus out = Apply(pad, Mask{PAD_BUTTON_START | PAD_BUTTON_X, false, false});
  EXPECT_EQ(out.button, pad.button & ~(PAD_BUTTON_START | PAD_BUTTON_X));
  EXPECT_EQ(out.stickX, pad.stickX);
  EXPECT_EQ(out.stickY, pad.stickY);
  EXPECT_EQ(out.substickX, pad.substickX);
  EXPECT_EQ(out.substickY, pad.substickY);
  EXPECT_EQ(out.triggerLeft, pad.triggerLeft);
  EXPECT_EQ(out.triggerRight, pad.triggerRight);
  EXPECT_EQ(out.analogA, pad.analogA);
  EXPECT_TRUE(out.isConnected);
}

TEST(OrcaInputGate, TriggersAndAnalogButtonsGoWithTheirButtons)
{
  const GCPadStatus pad = Busy();
  GCPadStatus out = Apply(pad, Mask{PAD_TRIGGER_L, false, false});
  EXPECT_EQ(out.button & PAD_TRIGGER_L, 0);
  EXPECT_EQ(out.triggerLeft, 0);
  EXPECT_EQ(out.triggerRight, pad.triggerRight);
  out = Apply(pad, Mask{PAD_TRIGGER_R | PAD_BUTTON_A | PAD_BUTTON_B, false, false});
  EXPECT_EQ(out.triggerLeft, pad.triggerLeft);
  EXPECT_EQ(out.triggerRight, 0);
  EXPECT_EQ(out.analogA, 0);
  EXPECT_EQ(out.analogB, 0);
  EXPECT_EQ(out.button & (PAD_TRIGGER_R | PAD_BUTTON_A | PAD_BUTTON_B), 0);
}

TEST(OrcaInputGate, SticksCentredSeparately)
{
  const GCPadStatus pad = Busy();
  GCPadStatus out = Apply(pad, Mask{0, true, false});
  EXPECT_EQ(out.stickX, int{GCPadStatus::MAIN_STICK_CENTER_X});
  EXPECT_EQ(out.stickY, int{GCPadStatus::MAIN_STICK_CENTER_Y});
  EXPECT_EQ(out.substickX, pad.substickX);
  EXPECT_EQ(out.button, pad.button);
  out = Apply(pad, Mask{0, false, true});
  EXPECT_EQ(out.stickX, pad.stickX);
  EXPECT_EQ(out.substickX, int{GCPadStatus::C_STICK_CENTER_X});
  EXPECT_EQ(out.substickY, int{GCPadStatus::C_STICK_CENTER_Y});
}

TEST(OrcaInputGate, AReadsAsAnotherButtonWhileHeld)
{
  // A casual stage pick: A masked, and held A reads as X (the rule sees it, the screen doesn't take
  // a stage); the player's own X masked too, so only A makes the press.
  GCPadStatus pad{};
  pad.isConnected = true;
  pad.button = PAD_BUTTON_A | PAD_BUTTON_X;
  pad.analogA = 0xC0;
  Mask mask{PAD_BUTTON_A | PAD_BUTTON_X, false, false};
  mask.a_as = PAD_BUTTON_X;
  EXPECT_FALSE(mask.Empty());
  GCPadStatus out = Apply(pad, mask);
  EXPECT_EQ(out.button, PAD_BUTTON_X);
  EXPECT_EQ(out.analogA, 0);
  // X alone (no A): nothing.
  pad.button = PAD_BUTTON_X;
  out = Apply(pad, mask);
  EXPECT_EQ(out.button, 0);
  // Without A held the remap adds nothing; with it and nothing masked, both.
  pad.button = PAD_BUTTON_A;
  Mask only;
  only.a_as = PAD_BUTTON_X;
  EXPECT_EQ(Apply(pad, only).button, PAD_BUTTON_A | PAD_BUTTON_X);
  pad.button = 0;
  EXPECT_EQ(Apply(pad, only).button, 0);
}

TEST(OrcaInputGate, TheStickCentresWhileAIsDown)
{
  // The queue's own character select's BACK (OnlineRules.h CssBackTakesA): while the player holds
  // A, or the rule presses it, the main stick reads centred, so the hand stays on BACK through the
  // press; with A up the stick is the player's. The C-stick and the buttons are left alone.
  GCPadStatus pad = Busy();
  pad.button = PAD_BUTTON_A | PAD_BUTTON_Y;
  Mask mask;
  mask.a_centres_stick = true;
  EXPECT_FALSE(mask.Empty());
  GCPadStatus out = Apply(pad, mask);
  EXPECT_EQ(out.stickX, int{GCPadStatus::MAIN_STICK_CENTER_X});
  EXPECT_EQ(out.stickY, int{GCPadStatus::MAIN_STICK_CENTER_Y});
  EXPECT_EQ(out.substickX, pad.substickX);
  EXPECT_EQ(out.button, pad.button);
  // A masked from the game (a ready player's press that stops the search first): still centred,
  // it is the player's A that freezes the hand.
  Mask masked = mask;
  masked.buttons = PAD_BUTTON_A;
  out = Apply(pad, masked);
  EXPECT_EQ(out.button, PAD_BUTTON_Y);
  EXPECT_EQ(out.stickX, int{GCPadStatus::MAIN_STICK_CENTER_X});
  // A up: the stick moves the hand as ever.
  pad.button = PAD_BUTTON_Y;
  out = Apply(pad, mask);
  EXPECT_EQ(out.stickX, pad.stickX);
  EXPECT_EQ(out.stickY, pad.stickY);
  // A pressed by the rule (the frame after that stop): centred too.
  Mask press = mask;
  press.press = PAD_BUTTON_A;
  out = Apply(pad, press);
  EXPECT_EQ(out.button, PAD_BUTTON_A | PAD_BUTTON_Y);
  EXPECT_EQ(out.analogA, 0xFF);
  EXPECT_EQ(out.stickX, int{GCPadStatus::MAIN_STICK_CENTER_X});
  EXPECT_EQ(out.stickY, int{GCPadStatus::MAIN_STICK_CENTER_Y});
  // An unplugged port stays as it is.
  GCPadStatus none{};
  none.isConnected = false;
  none.button = PAD_BUTTON_A;
  none.stickX = 200;
  EXPECT_EQ(Apply(none, mask).stickX, 200);
}

TEST(OrcaInputGate, DownDropsOnlyWhileLAndRAreBothHeld)
{
  // Project+'s Code Menu chord: D-pad Down reads released while L and R are both held, by button or
  // by a quarter press. Shield (L, R, their analog values) and the rest stay the player's.
  Mask mask;
  mask.drop_with_lr = PAD_BUTTON_DOWN;
  EXPECT_FALSE(mask.Empty());
  GCPadStatus pad{};
  pad.isConnected = true;
  pad.button = PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_DOWN | PAD_BUTTON_A;
  pad.triggerLeft = 0xFF;
  pad.triggerRight = 0xFF;
  GCPadStatus out = Apply(pad, mask);
  EXPECT_EQ(out.button, PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_A);
  EXPECT_EQ(out.triggerLeft, 0xFF);
  EXPECT_EQ(out.triggerRight, 0xFF);
  EXPECT_TRUE(Same(Apply(out, mask), out));
  // Analog only, at a quarter press each: still the chord.
  pad.button = PAD_BUTTON_DOWN;
  pad.triggerLeft = TRIGGER_HELD;
  pad.triggerRight = 0xC0;
  EXPECT_EQ(Apply(pad, mask).button, 0);
  // One trigger resting, or released: Down is a taunt again.
  pad.triggerLeft = TRIGGER_HELD - 1;
  EXPECT_EQ(Apply(pad, mask).button, PAD_BUTTON_DOWN);
  pad.button = PAD_TRIGGER_R | PAD_BUTTON_DOWN;
  pad.triggerLeft = 0;
  EXPECT_EQ(Apply(pad, mask).button, PAD_TRIGGER_R | PAD_BUTTON_DOWN);
  pad.button = PAD_TRIGGER_L | PAD_BUTTON_DOWN;
  pad.triggerRight = 0x10;
  EXPECT_EQ(Apply(pad, mask).button, PAD_TRIGGER_L | PAD_BUTTON_DOWN);
  // L and R themselves are never dropped by the chord.
  Mask triggers;
  triggers.drop_with_lr = PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_DOWN;
  pad.button = PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_DOWN;
  pad.triggerLeft = pad.triggerRight = 0xFF;
  out = Apply(pad, triggers);
  EXPECT_EQ(out.button, PAD_TRIGGER_L | PAD_TRIGGER_R);
  EXPECT_EQ(out.triggerLeft, 0xFF);
}

TEST(OrcaInputGate, AllIsANeutralConnectedPadButOriginBitsStay)
{
  const GCPadStatus out = Apply(Busy(), ALL);
  EXPECT_EQ(out.button, PAD_USE_ORIGIN | PAD_GET_ORIGIN);
  // What the wire carries of it: exactly a neutral pad.
  EXPECT_EQ(Orca::Net::EncodePad(out), Orca::Net::Pad{});
  EXPECT_TRUE(out.isConnected);
  EXPECT_EQ(out.triggerLeft, 0);
  EXPECT_EQ(out.triggerRight, 0);
}

TEST(OrcaInputGate, OnlyGameButtonsAreMasked)
{
  // A mask with stray bits (origin, error) touches only the game's buttons.
  const GCPadStatus pad = Busy();
  const GCPadStatus out = Apply(pad, Mask{0xFFFF, false, false});
  EXPECT_EQ(out.button, PAD_USE_ORIGIN | PAD_GET_ORIGIN);
}

TEST(OrcaInputGate, UnpluggedPadLeftAlone)
{
  const GCPadStatus unplugged = Orca::Net::DecodePad(Orca::Net::UNPLUGGED_PAD);
  ASSERT_FALSE(unplugged.isConnected);
  EXPECT_TRUE(Same(Apply(unplugged, ALL), unplugged));
}

TEST(OrcaInputGate, Idempotent)
{
  // A re-run, or a pad that went through the gate already (a recording of gated pads), gives the
  // same bytes again.
  const Mask masks[] = {Mask{PAD_BUTTON_A, true, false}, ALL, Mask{PAD_TRIGGER_R, false, true}};
  for (const Mask& mask : masks)
  {
    const GCPadStatus once = Apply(Busy(), mask);
    EXPECT_TRUE(Same(Apply(once, mask), once));
  }
}

TEST(OrcaInputGate, SameBytesAsTheWireSees)
{
  // Both machines decode every port from the wire, then gate it: the gated pad of a wire pad is a
  // function of the wire bytes and the mask alone.
  Orca::Net::Pad wire{0x11, 0x63, 0xF0, 0x40, 0x7F, 0x81, 0xFF, 0x20};
  const GCPadStatus a = Apply(Orca::Net::DecodePad(wire), Mask{PAD_BUTTON_START, true, false});
  const GCPadStatus b = Apply(Orca::Net::DecodePad(wire), Mask{PAD_BUTTON_START, true, false});
  EXPECT_TRUE(Same(a, b));
  EXPECT_EQ(a.button & PAD_BUTTON_START, 0);
  EXPECT_EQ(a.stickX, int{GCPadStatus::MAIN_STICK_CENTER_X});
}

TEST_F(OrcaInputGateBoundary, NoSourceNoMasks)
{
  Boundary();
  for (const Mask& mask : Current())
    EXPECT_TRUE(mask.Empty());
  const GCPadStatus pad = Busy();
  EXPECT_TRUE(Same(GatePad(0, pad), pad));
}

TEST_F(OrcaInputGateBoundary, MasksFollowTheStateAtEachBoundary)
{
  // The source reads "memory" (here an int): the masks of a boundary depend on it alone, so a
  // rollback that brings back an earlier state brings back that state's masks, and nothing is
  // carried over from the boundary before.
  int memory = 0;
  SetSource([&memory](const Core::CPUThreadGuard&) {
    Masks masks{};
    if (memory % 2)
      masks[1] = Mask{PAD_BUTTON_A, false, false};
    if (memory >= 2)
      masks[0] = ALL;
    return masks;
  });
  const GCPadStatus pad = Busy();
  std::vector<Masks> seen;
  for (memory = 0; memory < 4; ++memory)
  {
    Boundary();
    seen.push_back(Current());
  }
  EXPECT_TRUE(seen[0][0].Empty() && seen[0][1].Empty());
  EXPECT_EQ(seen[1][1], (Mask{PAD_BUTTON_A, false, false}));
  EXPECT_EQ(seen[2][0], ALL);
  EXPECT_TRUE(seen[2][1].Empty());
  // A rollback to the first state, then a re-run.
  for (memory = 0; memory < 4; ++memory)
  {
    Boundary();
    EXPECT_EQ(Current(), seen[memory]) << "state " << memory;
  }
  // The masks in effect are what GatePad applies, per port.
  memory = 3;
  Boundary();
  EXPECT_TRUE(Same(GatePad(0, pad), Apply(pad, ALL)));
  EXPECT_TRUE(Same(GatePad(1, pad), Apply(pad, Mask{PAD_BUTTON_A, false, false})));
  EXPECT_TRUE(Same(GatePad(2, pad), pad));
  EXPECT_TRUE(Same(GatePad(3, pad), pad));
  EXPECT_TRUE(Same(GatePad(4, pad), pad));
  EXPECT_TRUE(Same(GatePad(-1, pad), pad));
}

TEST_F(OrcaInputGateBoundary, ClearedSourceAndClearStopMasking)
{
  SetSource([](const Core::CPUThreadGuard&) {
    Masks masks{};
    masks[0] = ALL;
    return masks;
  });
  Boundary();
  EXPECT_EQ(Current()[0], ALL);
  Clear();
  EXPECT_TRUE(Current()[0].Empty());
  Boundary();
  EXPECT_EQ(Current()[0], ALL);
  SetSource({});
  // Still in effect until the next boundary.
  EXPECT_EQ(Current()[0], ALL);
  Boundary();
  EXPECT_TRUE(Current()[0].Empty());
}

TEST_F(OrcaInputGateBoundary, GameReadsGatedRawReadsDoNot)
{
  // A session's port: the game's read (InputOverride, SI_DeviceGCController) goes through the
  // masks; the raw read (what a recording keeps) and the session's pads themselves do not.
  Rollback::RingPort port(Core::System::GetInstance(), 7);
  GCPadStatus pressed;
  pressed.button = PAD_BUTTON_A | PAD_BUTTON_START;
  pressed.stickX = 255;
  pressed.isConnected = true;
  Orca::Net::Pads pads{};
  pads[0] = Orca::Net::EncodePad(pressed);
  pads[1] = Orca::Net::EncodePad(pressed);
  pads[2] = Orca::Net::UNPLUGGED_PAD;
  pads[3] = Orca::Net::UNPLUGGED_PAD;
  port.SetPads(1, pads);
  SetSource([](const Core::CPUThreadGuard&) {
    Masks masks{};
    masks[0] = Mask{PAD_BUTTON_START, true, false};
    masks[2] = ALL;
    return masks;
  });
  Boundary();

  const std::optional<GCPadStatus> game0 = Rollback::InputOverride(0);
  const std::optional<GCPadStatus> raw0 = Rollback::RawInputOverride(0);
  ASSERT_TRUE(game0 && raw0);
  EXPECT_EQ(game0->button, PAD_BUTTON_A);
  EXPECT_EQ(game0->stickX, int{GCPadStatus::MAIN_STICK_CENTER_X});
  EXPECT_EQ(raw0->button, PAD_BUTTON_A | PAD_BUTTON_START);
  EXPECT_EQ(raw0->stickX, 255);
  EXPECT_EQ(Orca::Net::EncodePad(*raw0), pads[0]);
  EXPECT_EQ(Orca::Net::EncodePad(port.Pad(0)), pads[0]);
  // Port 2 has no mask; port 3 is unplugged whatever its mask.
  EXPECT_EQ(Rollback::InputOverride(1)->button, PAD_BUTTON_A | PAD_BUTTON_START);
  EXPECT_FALSE(Rollback::InputOverride(2)->isConnected);
}

TEST(OrcaInputGateTestKnob, Parses)
{
  std::string error;
  const auto rules = ParseTestSpec("1:A+STICK:mem,2:START,4:ALL:always,3:L+R+CSTICK+UP", &error);
  ASSERT_TRUE(rules) << error;
  ASSERT_EQ(rules->size(), 4u);
  EXPECT_EQ((*rules)[0].port, 0);
  EXPECT_EQ((*rules)[0].mask, (Mask{PAD_BUTTON_A, true, false}));
  EXPECT_EQ((*rules)[0].every, 2);
  EXPECT_EQ((*rules)[1].port, 1);
  EXPECT_EQ((*rules)[1].mask, (Mask{PAD_BUTTON_START, false, false}));
  EXPECT_EQ((*rules)[1].every, 0);
  EXPECT_EQ((*rules)[2].port, 3);
  EXPECT_EQ((*rules)[2].mask, ALL);
  EXPECT_EQ((*rules)[2].every, 0);
  EXPECT_EQ((*rules)[3].mask, (Mask{PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_UP, false, true}));
}

TEST(OrcaInputGateTestKnob, Refuses)
{
  for (const char* bad : {"",           "1",
                          "0:A",        "5:A",
                          "1:Q",        "1:A:sometimes",
                          "1:A:mem:x",  "1:A,",
                          "12:A",       "1:",
                          "1:A+",       "1:A,,2:B",
                          "1:a",        " 1:A",
                          "1:A:mem/1",  "1:A:mem/65",
                          "1:A:mem/",   "1:A:mem/x",
                          "1:A:mem/-2", "1:A:mem/1000",
                          "1:A:mem8"})
  {
    std::string error;
    EXPECT_FALSE(ParseTestSpec(bad, &error)) << bad;
    EXPECT_FALSE(error.empty()) << bad;
  }
}

TEST(OrcaInputGateTestKnob, MemoryRulesFollowTheHash)
{
  std::string error;
  const auto rules = ParseTestSpec("1:A:mem,2:B:mem/8,3:X", &error);
  ASSERT_TRUE(rules) << error;
  EXPECT_EQ((*rules)[0].every, 2);
  EXPECT_EQ((*rules)[1].every, 8);
  // Byte 0 of the hash decides port 1's rule (masked when even), byte 1 port 2's (masked when a
  // multiple of 8); port 3's rule always applies.
  Masks m = TestMasks(*rules, 0x0000);
  EXPECT_EQ(m[0].buttons, PAD_BUTTON_A);
  EXPECT_EQ(m[1].buttons, PAD_BUTTON_B);
  EXPECT_EQ(m[2].buttons, PAD_BUTTON_X);
  m = TestMasks(*rules, 0x0401);
  EXPECT_TRUE(m[0].Empty());
  EXPECT_TRUE(m[1].Empty());
  EXPECT_EQ(m[2].buttons, PAD_BUTTON_X);
  m = TestMasks(*rules, 0x1802);
  EXPECT_EQ(m[0].buttons, PAD_BUTTON_A);
  EXPECT_EQ(m[1].buttons, PAD_BUTTON_B);
  // One frame in n, over every byte value.
  int masked = 0;
  for (u64 byte = 0; byte < 256; ++byte)
    masked += TestMasks(*rules, byte << 8)[1].Empty() ? 0 : 1;
  EXPECT_EQ(masked, 32);
  // Two rules for one port add up.
  const auto two = ParseTestSpec("1:A,1:STICK:mem", &error);
  ASSERT_TRUE(two) << error;
  EXPECT_EQ(TestMasks(*two, 0)[0], (Mask{PAD_BUTTON_A, true, false}));
  EXPECT_EQ(TestMasks(*two, 1)[0], (Mask{PAD_BUTTON_A, false, false}));
}
