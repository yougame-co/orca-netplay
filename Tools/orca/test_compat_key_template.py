#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for compat-key-template.py (the lobby compatibility key template of a build).

  python3 Tools/orca/test_compat_key_template.py        (standard library only)

The shared vectors (mods/compat-key-vectors.json) hold keys a real Orca logged for the TEST packs;
YouGame's TS port (web/src/lib/orca-compat-keys.ts) runs the same file. When this checkout's key code
no longer has a known fingerprint, the template refuses, and so does this test: update the tool,
the TS port and the vectors together.
"""

import importlib.machinery
import importlib.util
import json
import os
import shutil
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
VECTORS = os.path.join(HERE, "mods", "compat-key-vectors.json")


def load_tool():
    path = os.path.join(HERE, "compat-key-template.py")
    loader = importlib.machinery.SourceFileLoader("compat_key_template", path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


ck = load_tool()


class CompatKeyTemplate(unittest.TestCase):
    def setUp(self):
        with open(VECTORS, encoding="utf-8") as f:
            self.v = json.load(f)

    def test_vectors(self):
        self.assertEqual(ck.selftest(self.v), 0)

    def test_real_keys_are_xxh3_of_their_parts(self):
        for r in self.v["real"]:
            self.assertEqual(f"orca2:{ck.xxh3_64(r['parts'].encode()):016x}", r["key"], r["name"])

    def test_key_code_is_known(self):
        self.assertIn(ck.key_code_fingerprint(ROOT), ck.KEY_CODE)

    def test_template_is_deterministic(self):
        sys_dir = os.path.join(ROOT, "Data", "Sys")
        commit = "0" * 40
        a = ck.make_template(ROOT, sys_dir, commit, "9.9.9", [])
        b = ck.make_template(ROOT, sys_dir, commit, "9.9.9", [])
        self.assertEqual(json.dumps(a, sort_keys=True), json.dumps(b, sort_keys=True))
        self.assertTrue(a["parts"].startswith(f"orca2|{commit}|api="))
        # The Brawl and Project+ games are always covered, whatever --cover says.
        self.assertIn("RSBE01", a["fill"]["sysIni"]["covers"])
        self.assertIn("PPLUS32", a["fill"]["sysIni"]["covers"])

    def test_a_changed_key_function_refuses(self):
        tmp = tempfile.mkdtemp(prefix="ck-")
        try:
            for path, _ in ck.KEY_FUNCTIONS:
                dst = os.path.join(tmp, path)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(os.path.join(ROOT, path), dst)
            online = os.path.join(tmp, "Source/Core/Core/Orca/Session/Online.cpp")
            with open(online, encoding="utf-8") as f:
                text = f.read()
            with open(online, "w", encoding="utf-8") as f:
                f.write(text.replace('"|ux={}.{}"', '"|ux={}+{}"'))
            self.assertNotIn(ck.key_code_fingerprint(tmp), ck.KEY_CODE)
        finally:
            shutil.rmtree(tmp)

    def test_results_builtin_differs_from_brawl_only_in_ux(self):
        t = ck.make_template(ROOT, os.path.join(ROOT, "Data", "Sys"), "0" * 40, "9.9.9", [])
        games = t["fill"]["ux"]["games"]
        self.assertEqual(games["brawl-rev2"], 0)
        self.assertEqual(games["brawl-rev2-results"], 1)
        bf = next(r for r in self.v["real"] if "launcher" not in r["mod"])
        self.assertEqual(bf["mod"]["builtin"], "brawl-rev2")
        results = dict(bf["mod"], builtin="brawl-rev2-results")
        brawl_parts = ck.pack_parts(t, bf["mod"], bf["tree"], bf["sd"])
        results_parts = ck.pack_parts(t, results, bf["tree"], bf["sd"])
        engine = t["fill"]["ux"]["engine"]
        self.assertEqual(len(brawl_parts), len(results_parts))
        for a, b in zip(brawl_parts, results_parts):
            self.assertTrue(a.endswith(f"|ux={engine}"), a)
            self.assertEqual(a + ".1", b)

    def test_profile_game_ids_follow_falling_through_cases(self):
        def mod_cpp(cases):
            return (ck.PROFILE_FROM_MANIFEST + " Mod::BundleKind kind)\n{\n  Profile profile;\n"
                    "  switch (manifest.builtin)\n  {\n" + cases + "  }\n  return profile;\n}\n")
        old = ('  case Mod::Builtin::BrawlRev2:\n    profile.game_id = "RSBE01";\n    break;\n'
               '  case Mod::Builtin::BrawlRev2PPlus:\n    profile.game_id = "PPLUS32";\n    break;\n'
               '  default:\n    profile.game_id = manifest.disc_id;\n    break;\n')
        self.assertEqual(ck.builtin_game_ids(mod_cpp(old)),
                         {"BrawlRev2": "RSBE01", "BrawlRev2PPlus": "PPLUS32"})
        both = old.replace('  case Mod::Builtin::BrawlRev2:\n',
                           '  case Mod::Builtin::BrawlRev2:\n  case Mod::Builtin::BrawlRev2Results:\n')
        self.assertEqual(ck.builtin_game_ids(mod_cpp(both)),
                         {"BrawlRev2": "RSBE01", "BrawlRev2Results": "RSBE01", "BrawlRev2PPlus": "PPLUS32"})
        # A label that falls into the default gets no builtin game id; anything else in the switch refuses.
        into_default = old.replace("  default:\n", "  case Mod::Builtin::BrawlRev2Results:\n  default:\n")
        self.assertNotIn("BrawlRev2Results", ck.builtin_game_ids(mod_cpp(into_default)))
        with self.assertRaises(SystemExit):
            ck.builtin_game_ids(mod_cpp(old.replace("    break;\n", "    profile.revision = 2;\n", 1)))
        # And this checkout's: every builtin the tool knows, with its game id.
        ids = ck.builtin_game_ids(ck.mod_text(ROOT))
        for builtin, game_id in ck.BUILTIN_GAME_IDS.items():
            self.assertEqual(ids.get(ck.BUILTIN_ENUMS[builtin]), game_id, builtin)

    def test_a_tampered_template_refuses(self):
        t = json.loads(json.dumps(self.v["templates"]["train-fri-005be8ff1f"]))
        r = self.v["real"][0]
        for parts in (t["parts"].replace("{tree}", "x"), t["parts"] + "{sd}",
                      t["parts"].replace("|ux=", "|ux={"), " " + t["parts"]):
            bad = dict(t, parts=parts)
            with self.assertRaises(ck.Refused):
                ck.derive(bad, r["mod"], r["tree"], r["sd"])


if __name__ == "__main__":
    unittest.main()
