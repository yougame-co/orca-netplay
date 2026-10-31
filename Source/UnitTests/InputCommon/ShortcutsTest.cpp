// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "InputCommon/Shortcuts.h"

namespace
{
using namespace InputCommon::Shortcuts;
TEST(ShortcutsParser, ValidKeysModifiersAndOptionalFields)
{
  EXPECT_EQ(ParseLine("embed shortcuts"), Bindings{});
  for (char key = 'A'; key <= 'Z'; ++key)
    ASSERT_TRUE(ParseLine("embed shortcuts menu=" + std::string(1, key)));
  for (int key = 1; key <= 12; ++key)
    ASSERT_TRUE(ParseLine("embed shortcuts chat=F" + std::to_string(key)));
  for (const auto* key : {"Tab", "Enter", "Backquote"})
    for (const auto* modifiers : {"", "Shift+", "Ctrl+", "Alt+", "Shift+Ctrl+", "Ctrl+Alt+",
                                  "Alt+Shift+", "Shift+Ctrl+Alt+", "Alt+Ctrl+Shift+"})
      ASSERT_TRUE(ParseLine(std::string("embed shortcuts menu=") + modifiers + key));
  const auto parsed = ParseLine("embed shortcuts chat=Shift+Enter menu=Ctrl+Alt+Tab");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->menu, (Combo{Ctrl | Alt, "Tab"}));
  EXPECT_EQ(parsed->chat, (Combo{Shift, "Enter"}));
}
TEST(ShortcutsParser, RejectsMalformedWholeLines)
{
  for (const auto* line : {"",
                           "shortcuts",
                           "embed shortcutsx",
                           "embed shortcuts ",
                           " embed shortcuts",
                           "embed shortcuts  menu=Tab",
                           "embed shortcuts\tmenu=Tab",
                           "embed shortcuts menu=Tab ",
                           "embed shortcuts menu=Tab\n",
                           "embed shortcuts menu=Tab\r",
                           "embed shortcuts menu=Tab chat=Enter extra=A",
                           "embed shortcuts Menu=Tab",
                           "embed shortcuts menu",
                           "embed shortcuts menu=",
                           "embed shortcuts chat=",
                           "embed shortcuts menu=Tab menu=Enter",
                           "embed shortcuts chat=Enter chat=Tab",
                           "embed shortcuts menu=Tab=Enter",
                           "embed shortcuts menu=Shift",
                           "embed shortcuts menu=Shift+",
                           "embed shortcuts menu=+Tab",
                           "embed shortcuts menu=Shift++Tab",
                           "embed shortcuts menu=Shift+Shift+Tab",
                           "embed shortcuts menu=Ctrl+Ctrl+Enter",
                           "embed shortcuts menu=Alt+Alt+A",
                           "embed shortcuts menu=Tab+Shift",
                           "embed shortcuts menu=Meta+Tab",
                           "embed shortcuts menu=Control+Tab",
                           "embed shortcuts menu=shift+Tab",
                           "embed shortcuts menu=tab",
                           "embed shortcuts menu=a",
                           "embed shortcuts menu=F0",
                           "embed shortcuts menu=F13",
                           "embed shortcuts menu=F01",
                           "embed shortcuts menu=Escape",
                           "embed shortcuts menu=Space",
                           "embed shortcuts menu=`",
                           "embed shortcuts menu=Shift +Tab",
                           "embed shortcuts menu=Shift+ Tab",
                           "embed shortcuts menu=Tab,chat=Enter"})
    EXPECT_FALSE(ParseLine(line)) << line;
}
struct ShortcutState : testing::Test
{
  Manager manager;
  std::vector<std::string> lines;
  void SetUp() override
  {
    manager.SetOutput([this](std::string_view line) { lines.emplace_back(line); });
    ASSERT_TRUE(manager.Configure("embed shortcuts menu=Shift+Tab chat=Shift+Enter"));
  }
};
TEST_F(ShortcutState, MenuDownUpRepeatIgnoredAndUnrelatedKeysPass)
{
  EXPECT_FALSE(manager.KeyEvent("Shift", Shift, true));
  EXPECT_TRUE(manager.KeyEvent("Tab", Shift, true));
  EXPECT_TRUE(manager.KeyEvent("Tab", Shift, true, true));
  EXPECT_TRUE(manager.KeyEvent("Tab", Shift, true));  // duplicate press without repeat flag
  EXPECT_TRUE(manager.IsSuppressed("Tab"));
  EXPECT_TRUE(manager.IsSuppressed("Shift"));
  EXPECT_FALSE(manager.KeyEvent("A", Shift, true));
  EXPECT_EQ(manager.GetWindowKeyState("A"), std::nullopt);
  EXPECT_TRUE(manager.KeyEvent("Tab", Shift, false));
  EXPECT_FALSE(manager.IsSuppressed("Shift"));
  manager.KeyEvent("Shift", 0, false);
  EXPECT_FALSE(manager.IsSuppressed("Shift"));
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"overlay\":\"down\"}", "yg {\"overlay\":\"up\"}"}));
}
TEST_F(ShortcutState, StartAndMainStickModifierAreSuppressedAtomically)
{
  // A hardware poll before the window dispatch sees no Start.
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  manager.KeyEvent("Shift", Shift, true);
  EXPECT_EQ(manager.GetWindowKeyState("Shift"), std::nullopt);
  manager.KeyEvent("Enter", Shift, true);
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  EXPECT_EQ(manager.GetWindowKeyState("Shift"), false);
  manager.KeyEvent("Enter", Shift, false);
  manager.KeyEvent("Shift", 0, false);
  EXPECT_EQ(lines, (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"up\"}"}));
}
TEST_F(ShortcutState, CtrlEnterRemapSuppressesStartAndCStickModifier)
{
  ASSERT_TRUE(manager.Configure("embed shortcuts chat=Ctrl+Enter"));
  manager.KeyEvent("Ctrl", Ctrl, true);
  manager.KeyEvent("Enter", Ctrl, true);
  EXPECT_EQ(manager.GetWindowKeyState("Ctrl"), false);
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  manager.KeyEvent("Ctrl", 0, false);
  EXPECT_TRUE(manager.IsSuppressed("Enter"));
  manager.KeyEvent("Enter", 0, true, true);
  manager.KeyEvent("Enter", 0, false);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}"}));
}
TEST_F(ShortcutState, FocusLossCancelsOnceAndDisarmsUntilFocusReturns)
{
  manager.KeyEvent("Enter", Shift, true);
  manager.FocusLost();
  manager.FocusLost();
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  EXPECT_FALSE(manager.KeyEvent("Enter", Shift, true));
  manager.SetFocus(true);
  manager.KeyEvent("Enter", Shift, false);
  manager.KeyEvent("Enter", Shift, true);
  manager.KeyEvent("Enter", Shift, false);
  EXPECT_EQ(lines, (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}",
                                             "yg {\"chat\":\"down\"}", "yg {\"chat\":\"up\"}"}));
}
TEST_F(ShortcutState, InvalidRebindPreservesAndBareClearsWithCancel)
{
  manager.KeyEvent("Enter", Shift, true);
  EXPECT_FALSE(manager.Configure("embed shortcuts chat=Escape"));
  EXPECT_TRUE(manager.IsSuppressed("Enter"));
  EXPECT_EQ(lines.size(), 1u);
  ASSERT_TRUE(manager.Configure("embed shortcuts"));
  EXPECT_FALSE(manager.IsSuppressed("Enter"));  // bare command restores raw input immediately
  manager.KeyEvent("Enter", Shift, false);
  EXPECT_FALSE(manager.KeyEvent("Enter", Shift, true));
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), std::nullopt);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}"}));
}
TEST_F(ShortcutState, RebindingReplacesBothBindingsAndIdenticalConfigKeepsHold)
{
  manager.KeyEvent("Enter", Shift, true);
  ASSERT_TRUE(manager.Configure("embed shortcuts menu=Shift+Tab chat=Shift+Enter"));
  EXPECT_EQ(lines.size(), 1u);
  ASSERT_TRUE(manager.Configure("embed shortcuts menu=A"));
  manager.KeyEvent("Enter", Shift, false);
  EXPECT_FALSE(manager.KeyEvent("Tab", Shift, true));
  manager.KeyEvent("A", 0, true);
  manager.KeyEvent("A", 0, false);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}",
                                      "yg {\"overlay\":\"down\"}", "yg {\"overlay\":\"up\"}"}));
}
TEST_F(ShortcutState, ExtraModifiersDoNotTriggerAndDroppingHoldDoesNotRetrigger)
{
  EXPECT_FALSE(manager.KeyEvent("Enter", Shift | Ctrl, true));
  manager.KeyEvent("Ctrl", Shift, false);
  EXPECT_TRUE(lines.empty());
  manager.KeyEvent("Enter", Shift, false);
  manager.KeyEvent("Enter", Shift, true);
  manager.KeyEvent("Alt", Shift | Alt, true);
  manager.KeyEvent("Alt", Shift, false);
  manager.KeyEvent("Enter", Shift, true, true);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}"}));
}
TEST_F(ShortcutState, SharedBindingRelaysBothActions)
{
  ASSERT_TRUE(manager.Configure("embed shortcuts menu=Enter chat=Enter"));
  manager.KeyEvent("Enter", 0, true);
  manager.KeyEvent("Enter", 0, false);
  EXPECT_EQ(lines, (std::vector<std::string>{"yg {\"overlay\":\"down\"}", "yg {\"chat\":\"down\"}",
                                             "yg {\"overlay\":\"up\"}", "yg {\"chat\":\"up\"}"}));
}
TEST_F(ShortcutState, RepeatsAfterFocusLossAndRebindNeverStartAHold)
{
  manager.KeyEvent("Enter", Shift, true);
  manager.FocusLost();
  manager.SetFocus(true);
  manager.KeyEvent("Enter", Shift, true, true);
  manager.KeyEvent("Enter", Shift, false);
  manager.KeyEvent("Enter", Shift, true);
  ASSERT_TRUE(manager.Configure("embed shortcuts chat=Enter"));
  manager.KeyEvent("Enter", 0, true, true);
  manager.KeyEvent("Enter", 0, true);
  EXPECT_EQ(lines.size(), 4u);
  manager.KeyEvent("Enter", 0, false);
  manager.KeyEvent("Enter", 0, true);
  EXPECT_EQ(lines.size(), 5u);
}
TEST_F(ShortcutState, RebindSwallowsAlreadyHeldNewChordWithoutSyntheticPress)
{
  manager.KeyEvent("A", Ctrl, true);
  EXPECT_EQ(manager.GetWindowKeyState("A"), std::nullopt);
  ASSERT_TRUE(manager.Configure("embed shortcuts chat=Ctrl+A"));
  EXPECT_EQ(manager.GetWindowKeyState("A"), false);
  EXPECT_EQ(manager.GetWindowKeyState("Ctrl"), false);
  EXPECT_TRUE(lines.empty());
  manager.KeyEvent("A", Ctrl, true, true);
  manager.KeyEvent("A", Ctrl, false);
  EXPECT_TRUE(lines.empty());
  manager.KeyEvent("A", Ctrl, true);
  EXPECT_EQ(lines, (std::vector<std::string>{"yg {\"chat\":\"down\"}"}));
}
TEST_F(ShortcutState, ModifierAddedAfterBaseAndRepressedAfterCancelCannotLeak)
{
  ASSERT_TRUE(manager.Configure("embed shortcuts chat=Ctrl+Enter"));
  manager.KeyEvent("Enter", 0, true);
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), true);
  EXPECT_TRUE(manager.KeyEvent("Ctrl", Ctrl, true));
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  EXPECT_EQ(manager.GetWindowKeyState("Ctrl"), false);
  EXPECT_TRUE(lines.empty());
  manager.KeyEvent("Ctrl", 0, false);
  manager.KeyEvent("Ctrl", Ctrl, true);
  EXPECT_EQ(manager.GetWindowKeyState("Ctrl"), false);
  manager.KeyEvent("Enter", Ctrl, false);
  manager.KeyEvent("Enter", Ctrl, true);
  manager.KeyEvent("Ctrl", 0, false);
  manager.KeyEvent("Ctrl", Ctrl, true);
  EXPECT_EQ(manager.GetWindowKeyState("Ctrl"), false);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}"}));
}
TEST_F(ShortcutState, UnknownKeysAlwaysProcessMetaModifierChanges)
{
  manager.KeyEvent("", Shift, true);
  manager.KeyEvent("Enter", Shift, true);
  manager.KeyEvent("", Shift | 8u, true);
  manager.KeyEvent("", Shift | 8u, true);
  manager.KeyEvent("Enter", Shift | 8u, false);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}"}));
}
TEST_F(ShortcutState, RepeatAndDuplicatePressUpdateModifiersWithoutNewDown)
{
  manager.KeyEvent("Enter", Shift, true);
  manager.KeyEvent("Enter", Shift | 8u, true, true);
  manager.KeyEvent("Enter", Shift, true, true);
  manager.KeyEvent("Enter", Shift, true);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"cancel\"}"}));
  manager.KeyEvent("Enter", Shift, false);
  manager.KeyEvent("Enter", Shift, true);
  manager.KeyEvent("Enter", Shift | 8u, true);
  EXPECT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines.back(), "yg {\"chat\":\"cancel\"}");
}
TEST(ShortcutsState, LegacyStateHasNoAuthoritativeOverrideBeforeConfiguration)
{
  Manager manager;
  EXPECT_FALSE(manager.Configured());
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), std::nullopt);
  EXPECT_FALSE(manager.Configure("embed shortcuts chat=bad"));
  EXPECT_FALSE(manager.Configured());
  EXPECT_TRUE(manager.Configure("embed shortcuts"));
  EXPECT_TRUE(manager.Configured());
  EXPECT_EQ(manager.GetWindowKeyState("Mouse 0"), std::nullopt);
}

TEST_F(ShortcutState, OnlyConfiguredBaseKeysAndHeldChordModifiersOverrideRawInput)
{
  for (const auto* key : {"X", "Z", "A", "F1", "Shift", "Ctrl", "Alt"})
    EXPECT_EQ(manager.GetWindowKeyState(key), std::nullopt) << key;
  manager.KeyEvent("Enter", Shift, true);
  EXPECT_EQ(manager.GetWindowKeyState("Shift"), false);
  EXPECT_EQ(manager.GetWindowKeyState("Ctrl"), std::nullopt);
  manager.KeyEvent("Enter", Shift, false);
  EXPECT_EQ(manager.GetWindowKeyState("Shift"), std::nullopt);
  manager.Configure("embed shortcuts");
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), std::nullopt);
}
TEST_F(ShortcutState, ModifierSnapshotBeforeFocusThenEnterSwallowsStart)
{
  manager.Configure("embed shortcuts chat=Ctrl+Enter");
  manager.FocusLost();
  manager.SynchronizeHeldKeys({"Ctrl"});  // Win32 real GetKeyState snapshot on WM_SETFOCUS
  manager.SetFocus(true);
  manager.KeyEvent("Enter", Ctrl, true);
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  EXPECT_EQ(manager.GetWindowKeyState("Ctrl"), false);
  EXPECT_EQ(lines, (std::vector<std::string>{"yg {\"chat\":\"down\"}"}));
}
TEST_F(ShortcutState, FocusRecoveryRepeatWithoutRepeatFlagRequiresPhysicalRelease)
{
  manager.KeyEvent("Enter", Shift, true);
  manager.FocusLost();
  manager.SynchronizeHeldKeys({"Enter", "Shift"});  // XQueryKeymap on FocusIn
  manager.SetFocus(true);
  manager.KeyEvent("Enter", Shift, true);        // detectable repeat press
  manager.KeyEvent("Enter", Shift, true, true);  // legacy release/press pair
  ASSERT_EQ(lines.size(), 2u);
  manager.KeyEvent("Enter", Shift, false);
  manager.KeyEvent("Enter", Shift, true);
  ASSERT_EQ(lines.size(), 3u);
}
TEST_F(ShortcutState, EmbeddedBackendSnapshotsRelayAndSuppressWithoutWindowEvents)
{
  manager.SetPolling(true);
  manager.PollKeys({"X", "Z", "Shift", "Enter"});
  EXPECT_EQ(manager.GetWindowKeyState("X"), std::nullopt);
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  EXPECT_EQ(manager.GetWindowKeyState("Shift"), false);
  manager.PollKeys({"X", "Z", "Shift", "Enter"});
  manager.KeyEvent("Enter", Shift, true);  // same key seen by window must not double-relay
  ASSERT_EQ(lines.size(), 1u);
  manager.PollKeys({"X", "Shift"});
  EXPECT_EQ(manager.GetWindowKeyState("Shift"), std::nullopt);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines.back(), "yg {\"chat\":\"up\"}");
}
TEST_F(ShortcutState, NonUSPhysicalPositionsMatchBackendKeys)
{
  manager.Configure("embed shortcuts chat=Ctrl+Q");
  // AZERTY logical A / Dvorak apostrophe at physical Q: no logical letter is used.
  EXPECT_EQ(ScanCodeKey(16), "Q");
  EXPECT_EQ(XkbPositionKey("AD01"), "Q");
  EXPECT_EQ(XkbPositionKey("AC01"), "A");
  manager.KeyEvent(ScanCodeKey(16), Ctrl, true);
  EXPECT_EQ(manager.GetWindowKeyState(XkbPositionKey("AD01")), false);
  EXPECT_EQ(manager.GetWindowKeyState("A"), std::nullopt);
  ASSERT_EQ(lines.size(), 1u);
}
TEST(ShortcutsDeviceClassification, SparseEvdevKeyboardUsesKeyMetadata)
{
  EXPECT_EQ(EvdevKey(59, "F1"), "F1");  // KEY_F1, no ENTER/SHIFT required
  EXPECT_EQ(EvdevKey(30, "A"), "A");
  EXPECT_EQ(EvdevKey(28, "ENTER"), "Enter");
  EXPECT_EQ(EvdevKey(0x130, "A"), "");   // BTN_SOUTH, even with a letter label
  EXPECT_EQ(EvdevKey(0x100, "F1"), "");  // BTN_MISC is not a keyboard key
}
TEST_F(ShortcutState, EmbeddedSnapshotReleasingBaseAndModifierTogetherEmitsUp)
{
  manager.Configure("embed shortcuts chat=Ctrl+Enter");
  manager.SetPolling(true);
  manager.PollKeys({"Ctrl", "Enter"});
  manager.PollKeys({});
  EXPECT_EQ(lines, (std::vector<std::string>{"yg {\"chat\":\"down\"}", "yg {\"chat\":\"up\"}"}));
}
TEST_F(ShortcutState, EmbeddedPollingStopsWithoutFocusAndRearmsWithoutSyntheticPress)
{
  manager.SetPolling(true);
  manager.PollKeys({"Shift", "Enter"});
  manager.FocusLost();
  EXPECT_FALSE(manager.UsesPolling());
  manager.SetFocus(true);
  EXPECT_TRUE(manager.UsesPolling());
  manager.PollKeys({"Shift", "Enter"});
  EXPECT_EQ(manager.GetWindowKeyState("Enter"), false);
  ASSERT_EQ(lines.size(), 2u);
  manager.PollKeys({});
  manager.PollKeys({"Shift", "Enter"});
  ASSERT_EQ(lines.size(), 3u);
}
}  // namespace
