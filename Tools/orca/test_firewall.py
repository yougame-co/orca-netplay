#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for Tools/orca/firewall (the S2 include firewall and float rule)."""

import importlib.machinery
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_loader = importlib.machinery.SourceFileLoader("firewall", os.path.join(HERE, "firewall"))
_spec = importlib.util.spec_from_loader("firewall", _loader)
fw = importlib.util.module_from_spec(_spec)
_loader.exec_module(fw)

GAME = "Source/Core/Core/Orca/Games/Brawl/X.cpp"
ENGINE = "Source/Core/Core/Orca/UX/Overlay.cpp"


def rules(path, text):
    return [(r, t) for r, t, _, _ in fw.scan(path, text)]


class FirewallTests(unittest.TestCase):
    def test_engine_including_a_game_header_is_a_hit(self):
        text = '#include "Core/Orca/Games/Brawl/Queue.h"\n#include "Core/Orca/UX/Kit.h"\n'
        self.assertEqual(rules(ENGINE, text), [("include-game", "Core/Orca/Games/Brawl/Queue.h")])

    def test_the_composition_root_games_and_tests_may_include_games(self):
        text = '#include "Core/Orca/Games/Brawl/BrawlModule.h"\n'
        self.assertEqual(rules("Source/Core/Core/Orca/UX/UX.cpp", text), [])
        self.assertEqual(rules(GAME, text), [])
        self.assertEqual(rules("Source/UnitTests/Core/Orca/Games/Brawl/QueueTest.cpp", text), [])

    def test_relative_spellings_of_a_game_header_are_hits(self):
        text = '#include "Orca/Games/Brawl/Queue.h"\n#include "../Games/Brawl/Queue.h"\n'
        self.assertEqual([r for r, _ in rules(ENGINE, text)], ["include-game", "include-game"])

    def test_policy_machines_follow_the_float_rule(self):
        self.assertEqual(rules("Source/Core/Core/Orca/Policy/Queue.cpp", "float x;\n"),
                         [("float", "float")])

    def test_a_commented_include_is_not_a_hit(self):
        self.assertEqual(rules(ENGINE, '// #include "Core/Orca/Games/Brawl/Queue.h"\n'), [])

    def test_floats_in_game_code(self):
        text = ('float x = 1.5f; double y = .5; int n = 3; s16 z = 16;\n'
                'auto r = std::sqrt(x);\n// 2.0 in a comment\nconst char* s = "3.5";\n')
        self.assertEqual(sorted(rules(GAME, text)),
                         sorted([("float", "float"), ("float", "literal"), ("float", "double"),
                                 ("float", "literal"), ("float", "cmath")]))

    def test_floats_outside_game_code_are_not_checked(self):
        self.assertEqual(rules(ENGINE, "float x = 1.5f;\n"), [])

    def test_versions_and_member_access_are_not_literals(self):
        self.assertEqual(rules(GAME, "int a = v.x; int b = 0x10; int c = 1;\n"), [])


class WarnModeTests(unittest.TestCase):
    """main(): warn vs error vs --strict, the known-file rule, the baseline at a revision."""

    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="firewall-test-")
        self.git("init", "-q")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "test")
        self.write(GAME, "float a;\n")
        self.write(ENGINE, '#include "Core/Orca/Games/Brawl/Queue.h"\n')
        os.makedirs(os.path.join(self.repo, "Tools/orca"))
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")

    def tearDown(self):
        shutil.rmtree(self.repo, ignore_errors=True)

    def git(self, *args):
        subprocess.run(["git", "-C", self.repo, *args], check=True, stdout=subprocess.DEVNULL)

    def write(self, path, text):
        full = os.path.join(self.repo, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)

    def run_fw(self, *args):
        r = subprocess.run([sys.executable, os.path.join(HERE, "firewall"), "--repo", self.repo,
                            *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return r.returncode, r.stdout

    def test_no_baseline_makes_every_hit_an_error(self):
        rc, out = self.run_fw()
        self.assertEqual(rc, 1)
        self.assertEqual(out.count("ERROR"), 2)

    def test_update_baseline_then_clean(self):
        self.assertEqual(self.run_fw("--update-baseline")[0], 0)
        rc, out = self.run_fw()
        self.assertEqual(rc, 0, out)
        self.assertIn("0 over the baseline in known files (warn), 0 in new files (error)", out)

    def test_more_hits_in_a_known_file_warn_and_strict_fails(self):
        self.run_fw("--update-baseline")
        self.write(GAME, "float a;\nfloat b;\n")
        rc, out = self.run_fw()
        self.assertEqual(rc, 0, out)
        self.assertIn("warn  float %s float: 2 > 1" % GAME, out)
        self.assertEqual(self.run_fw("--strict")[0], 1)

    def test_a_hit_in_a_file_the_baseline_doesnt_name_is_an_error(self):
        self.run_fw("--update-baseline")
        self.write("Source/Core/Core/Orca/Games/Brawl/New.cpp", "double d;\n")
        self.git("add", "-A")
        rc, out = self.run_fw()
        self.assertEqual(rc, 1, out)
        self.assertIn("ERROR float Source/Core/Core/Orca/Games/Brawl/New.cpp double", out)

    def test_rev_reads_the_baseline_at_that_revision(self):
        self.run_fw("--update-baseline")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "baseline")
        os.remove(os.path.join(self.repo, "Tools/orca/firewall.baseline"))
        self.assertEqual(self.run_fw("--rev", "HEAD")[0], 0)
        self.assertEqual(self.run_fw("--rev", "HEAD~1")[0], 1)

    def test_a_malformed_baseline_line_is_a_clean_error(self):
        self.write("Tools/orca/firewall.baseline", "not a baseline line\n")
        rc, out = self.run_fw()
        self.assertEqual(rc, 1)
        self.assertIn("line 1 is not", out)
        self.assertNotIn("Traceback", out)


class RepoTests(unittest.TestCase):
    def test_this_tree_has_no_firewall_errors(self):
        r = subprocess.run([sys.executable, os.path.join(HERE, "firewall")], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)


if __name__ == "__main__":
    unittest.main()
