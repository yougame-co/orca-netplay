#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""P+ release selection in flat bundles and compatibility templates; no game files needed."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bundle = load("pplus_bundle", ROOT / "Tools/orca/mods/make-bundle.py")
compat = load("pplus_compat", ROOT / "Tools/orca/compat-key-template.py")


class PPlus321(unittest.TestCase):
    def test_flat_bundles_pin_the_selected_release(self):
        with tempfile.TemporaryDirectory(prefix="pplus321-") as tmp:
            root = Path(tmp)
            source = root / "run"
            source.mkdir()
            launcher = b"fixture launcher"
            (source / bundle.PPLUS_LAUNCHER).write_bytes(launcher)
            cards = {"PPLUS32": b"320 card", "PPLUS321": b"321 card"}
            pins = {p: hashlib.sha256(b).hexdigest() for p, b in cards.items()}
            with patch.object(bundle, "PPLUS_SD_BYTES", 8), \
                 patch.object(bundle, "PPLUS_LAUNCHER_SHA256", hashlib.sha256(launcher).hexdigest()), \
                 patch.object(bundle, "PPLUS_SD_SHA256_BY_PROFILE", pins), \
                 contextlib.redirect_stdout(io.StringIO()):
                manifests = {}
                for profile, card in cards.items():
                    (source / "sd.raw").write_bytes(card)
                    args = argparse.Namespace(
                        out=str(root / profile), profile=profile, sys=str(ROOT / "Data/Sys"),
                        serial=1, listing=None, min_orca="0.3.76", pplus_run=str(source),
                        pplus_user=None, any_card=False, key=None)
                    bundle.make_flat(args)
                    mod = json.loads((root / profile / "mod.json").read_text())
                    self.assertEqual(mod["builtin"], "brawl-rev2-pplus")
                    self.assertEqual(mod["rulesets"], ["pplus-2024"])
                    self.assertEqual(mod["patches"], [profile + ".patches"])
                    self.assertEqual(mod["listing"], "project-rollback")
                    manifests[profile] = mod
                self.assertNotEqual(manifests["PPLUS32"]["sdImage"]["sha256"],
                                    manifests["PPLUS321"]["sdImage"]["sha256"])
                # A 3.2.1 card cannot be silently packaged as 3.2.0.
                args.profile = "PPLUS32"
                args.out = str(root / "wrong-release")
                with self.assertRaisesRegex(SystemExit, "sd.raw"):
                    bundle.make_flat(args)

    def test_320_template_keys_at_base_are_unchanged(self):
        template = compat.make_template(
            str(ROOT), str(ROOT / "Data/Sys"),
            "647d1aa6c7b6de499b585c4dead3ef6d88dd440b", "0.3.76", [])
        vectors = json.loads((ROOT / "Tools/orca/mods/compat-key-vectors.json").read_text())
        pp = next(v for v in vectors["real"] if v["name"] == "pp-pack")
        old = compat.derive(template, pp["mod"], pp["tree"], pp["sd"])
        self.assertEqual([key for key, _ in old],
                         ["orca2:63355bdbcdf2196a", "orca2:831804d019793054"])
        self.assertEqual(compat.key_code_fingerprint(str(ROOT)), "f86645e20656efe5")
        # Mod mode retains the same builtin family; the card/tree pins separate releases.
        new = compat.derive(template, pp["mod"], pp["tree"], "0" * 64)
        self.assertTrue(set(key for key, _ in old).isdisjoint(key for key, _ in new))


if __name__ == "__main__":
    unittest.main()
