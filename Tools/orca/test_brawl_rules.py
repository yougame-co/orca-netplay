#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin casual's ruleset sources while the two rulesets' timers are equal."""
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
BRAWL = ROOT / "Source/Core/Core/Orca/Games/Brawl"


class BrawlRulesTest(unittest.TestCase):
    def test_timer_data_stays_identical(self):
        for year in (2025, 2026):
            rules = json.loads((BRAWL / f"Rulesets/brawl-supernova-{year}.json").read_text())
            self.assertEqual(rules["steps"]["casualSeconds"], 15)
            self.assertEqual(rules["steps"]["casualGraceFrames"], 30)
            self.assertEqual(rules["steps"]["graceFrames"], 180)
            self.assertEqual(rules["queue"], {"readySeconds": 30, "skipFrames": 90})

    def test_casual_stage_consumers_use_the_selected_ruleset(self):
        # Equal values conceal a cross-ruleset read in runtime checks. Keep this source pin
        # alongside the runtime identity/timer tests so future ranked data cannot affect casual.
        source = (BRAWL / "BrawlStages.cpp").read_text()
        self.assertNotRegex(source, r"\bRules\(\)")
        for operation in ("PlanCasual", "PlanGame", "Begin", "Tick", "SecondsShown"):
            calls = re.findall(rf"RS::{operation}\(([^,]+),", source)
            self.assertTrue(calls, operation)
            self.assertTrue(all(call == "RulesFor(casual)" for call in calls), calls)
        for operation in ("Strike", "Skip"):
            self.assertRegex(source, rf"RS::{operation}\(RulesFor\(plan.casual\),")
        self.assertIn("RulesFor(casual).stage_kinds", source)

    def test_queue_and_locks_select_casual_2025(self):
        source = (BRAWL / "Rulesets.cpp").read_text()
        self.assertRegex(source, r"RulesFor\(bool casual\)\s*\{\s*"
                                r"return casual \? Brawl2025\(\) : Brawl2026\(\);")
        for file in ("Queue.cpp", "OnlineRules.cpp"):
            source = (BRAWL / file).read_text()
            self.assertRegex(source, r"h.ruleset == Rules::Ruleset::Brawl\s*\?\s*"
                                    r"&Rulesets::RulesFor\(h.mode == Rules::Mode::Casual\)"
                            if file == "Queue.cpp" else
                                    r"h.ruleset == Ruleset::Brawl\s*\?\s*"
                                    r"&Rulesets::RulesFor\(h.mode == Mode::Casual\)")


if __name__ == "__main__":
    unittest.main()
