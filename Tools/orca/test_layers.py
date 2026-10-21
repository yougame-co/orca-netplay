#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for layers.txt, layer-of-diff and forbidden-calls.

  python3 Tools/orca/test_layers.py        (standard library only; needs git)

The map tests read this checkout: every tracked path must resolve to exactly one class, and every
path in Orca's own code must be classed by a rule other than the `**` fallback.
"""

import importlib.machinery
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import layerlib  # noqa: E402


def load_script(name):
    path = os.path.join(HERE, name)
    loader = importlib.machinery.SourceFileLoader(name.replace("-", "_"), path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


fc = load_script("forbidden-calls")


def rules_of(text):
    return layerlib.parse_rules("engine **\n" + text)


class GlobTests(unittest.TestCase):
    def match(self, glob, path):
        return bool(layerlib.re.match(layerlib.glob_to_regex(glob), path))

    def test_star_stays_in_one_segment(self):
        self.assertTrue(self.match("Source/*.cpp", "Source/a.cpp"))
        self.assertFalse(self.match("Source/*.cpp", "Source/x/a.cpp"))

    def test_double_star_spans_segments(self):
        self.assertTrue(self.match("Source/**", "Source/a/b/c.cpp"))
        self.assertTrue(self.match("**/*.md", "ORCA.md"))
        self.assertTrue(self.match("**/*.md", "docs/a/b.md"))
        self.assertTrue(self.match("a/**/b.h", "a/b.h"))
        self.assertTrue(self.match("a/**/b.h", "a/x/y/b.h"))
        self.assertFalse(self.match("a/**/b.h", "a/xb.h"))
        self.assertTrue(self.match("**", "anything/at/all"))

    def test_question_and_class(self):
        self.assertTrue(self.match("a?.h", "ab.h"))
        self.assertFalse(self.match("a?.h", "a/.h"))
        self.assertTrue(self.match("RSB[EP]01.ini", "RSBE01.ini"))
        self.assertFalse(self.match("RSB[EP]01.ini", "RSBJ01.ini"))

    def test_whole_path_only(self):
        self.assertFalse(self.match("Orca/*", "Source/Orca/a.cpp"))
        self.assertFalse(self.match("a.cpp", "a.cppx"))

    def test_bad_double_star(self):
        with self.assertRaises(layerlib.RulesError):
            layerlib.glob_to_regex("a**b")

    def test_braces(self):
        self.assertEqual(layerlib.expand_braces("UX/{A,B}.{h,cpp}"),
                         ["UX/A.h", "UX/A.cpp", "UX/B.h", "UX/B.cpp"])
        self.assertEqual(layerlib.expand_braces("plain"), ["plain"])

    def test_literal_count(self):
        self.assertEqual(layerlib.literal_count("**"), 0)
        self.assertEqual(layerlib.literal_count("a/*.h"), 4)
        self.assertEqual(layerlib.literal_count("RSB[EP]01"), 5)


class RuleTests(unittest.TestCase):
    def test_most_specific_wins(self):
        r = rules_of("game-logic UX/**\nengine UX/Overlay.*\n")
        self.assertEqual(r.classify("UX/Queue.cpp").cls, "game-logic")
        self.assertEqual(r.classify("UX/Overlay.cpp").cls, "engine")
        self.assertEqual(r.classify("Other/x.cpp").cls, "engine")
        self.assertTrue(r.classify("Other/x.cpp").fallback)
        self.assertFalse(r.classify("UX/Queue.cpp").fallback)

    def test_order_does_not_matter(self):
        a = rules_of("engine UX/Overlay.*\ngame-logic UX/**\n")
        self.assertEqual(a.classify("UX/Overlay.cpp").cls, "engine")

    def test_tie_with_different_class_is_ambiguous(self):
        r = rules_of("game-logic UX/A*\nengine UX/*A\n")
        self.assertTrue(r.classify("UX/AA").ties)
        same = rules_of("engine UX/A*\nengine UX/*A\n")
        self.assertFalse(same.classify("UX/AA").ties)
        flags = rules_of("engine UX/A*\nengine UX/*A dev\n")
        self.assertTrue(flags.classify("UX/AA").ties)

    def test_dev_comes_from_the_winning_rule(self):
        r = rules_of("engine Tools/** dev\nengine-sensitive Tools/release.sh dev\n"
                     "game-data Tools/inputs/**\n")
        self.assertTrue(r.classify("Tools/x.py").dev)
        self.assertEqual(r.classify("Tools/release.sh").cls, "engine-sensitive")
        self.assertFalse(r.classify("Tools/inputs/a.txt").dev)

    def test_parse_errors(self):
        for bad in ["engine\n", "enginee **\n", "engine ** shipped\n", "engine /abs\n",
                    "%nope x\n", "%orca-root\n"]:
            with self.assertRaises(layerlib.RulesError, msg=bad):
                layerlib.parse_rules("engine **\n" + bad)
        with self.assertRaises(layerlib.RulesError):
            layerlib.parse_rules("engine a/**\n")  # no fallback

    def test_review_flag(self):
        r = rules_of("game-logic UX/**\ngame-logic UX/Queue.* review\n")
        self.assertTrue(r.classify("UX/Queue.cpp").review)
        self.assertEqual(r.classify("UX/Queue.cpp").cls, "game-logic")
        self.assertFalse(r.classify("UX/Other.cpp").review)
        tie = rules_of("game-logic UX/A* review\ngame-logic UX/*A\n")
        self.assertTrue(tie.classify("UX/AA").ties)

    def test_comments_and_directives(self):
        r = layerlib.parse_rules("# x\n%orca-root A/\n%upstream-ref up/master\n"
                                 "%upstream-base abc\nengine **   # fallback\n")
        self.assertEqual(r.orca_roots, ["A/"])
        self.assertEqual((r.upstream_ref, r.upstream_base), ("up/master", "abc"))


class RealMapTests(unittest.TestCase):
    """The checked-in layers.txt against this checkout."""

    @classmethod
    def setUpClass(cls):
        cls.rules = layerlib.load_rules()
        cls.repo = layerlib.repo_root()

    def test_every_tracked_path_has_one_class(self):
        tracked = [p for p in layerlib.git(self.repo, "ls-files", "-z").split("\0") if p]
        self.assertGreater(len(tracked), 1000)
        ambiguous = []
        for p in tracked:
            m = self.rules.classify(p)
            if m.ties:
                ambiguous.append("%s: %s vs %s" % (p, m.rule, m.ties))
        self.assertEqual(ambiguous, [])

    def test_every_orca_path_is_classed_on_purpose(self):
        paths, _ = layerlib.orca_paths(self.repo, self.rules, "HEAD")
        self.assertGreater(len(paths), 300)
        for root in self.rules.orca_roots:
            self.assertTrue(any(p.startswith(root) for p in paths), root)
        fallback = [p for p in paths if self.rules.classify(p).fallback]
        self.assertEqual(fallback, [], "Orca paths only the ** fallback classes; add a rule")

    def test_known_paths(self):
        expect = {
            "Data/Sys/Orca/RSBE01.patches": ("game-data", False),
            # Profiles carry forced settings and owner-only fields.
            "Data/Sys/Orca/PPLUS32.ini": ("engine-sensitive", False),
            "Data/Sys/Orca/RSBE01.ini": ("engine-sensitive", False),
            "Data/Sys/Orca/RSBE01.jitwarm": ("game-data", False),
            "Data/Sys/Orca/Textures/RSBE01/x.png": ("game-data", False),
            "Data/Sys/Orca/Fonts/Roboto-Medium.ttf": ("engine", False),
            # A new kind of file under Data/Sys/Orca is engine until it is named.
            "Data/Sys/Orca/mod.json": ("engine", False),
            "Data/Sys/Orca/scripts/x.luau": ("engine", False),
            "Data/Sys/Orca/lib/evil.dylib": ("engine", False),
            "Data/Sys/Orca/Textures/RSBE01/x.dll": ("engine", False),
            "Data/Sys/GameSettings/RSBE01.ini": ("engine-sensitive", False),
            "Data/Sys/GameSettings/GALE01.ini": ("engine", False),
            "Source/Core/Core/Orca/Games/Brawl/OnlineRules.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/Games/Brawl/BrawlStages.h": ("game-logic", False),
            "Source/Core/Core/Orca/Games/Brawl/PPlus/RankedPPlus.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/Games/Brawl/FreeSpace.h": ("game-logic", False),
            "Source/Core/Core/Orca/Games/Brawl/MenuText.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/UX/RankedSteps.cpp": ("engine", False),
            "Source/Core/Core/Orca/UX/Overlay.cpp": ("engine", False),
            "Source/Core/Core/Orca/UX/UX.cpp": ("engine", False),
            "Source/Core/Core/Orca/UX/GamePatches.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/UX/Chat.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/UX/ControllerSource.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/UX/YgOrb.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/UX/YgOrbDraw.cpp": ("engine", False),
            "Source/Core/Core/Orca/Status.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Games/BrawlRev2/Module.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/Games/BrawlRev2/CMakeLists.txt": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Games/BrawlRev2/deps/x.cmake": ("engine-sensitive", False),
            # The engine side of the split: Orca/Engine/, apart from the game modules' Orca/Games/.
            "Source/Core/Core/Orca/Engine/GuestMemory.h": ("engine", False),
            # S2 commit 3: engine files that parse peer or file bytes.
            "Source/Core/Core/Orca/UX/QueueApp.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/LocalStore.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/UX/Search.cpp": ("engine", False),
            "Source/Core/Core/Orca/UX/MenuEvents.cpp": ("engine", False),
            "Source/Core/Core/Orca/Session/YouGameRoom.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Session/Session.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Session/Events.h": ("engine", False),
            "Source/Core/Core/Orca/Profile.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Music.cpp": ("engine", False),
            "Source/Core/Core/Orca/Mod.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Rollback/Rollback.cpp": ("engine", False),
            "Source/Core/Core/Rollback/OnlineMatch.cpp": ("engine-sensitive", False),
            "Source/Core/Core/PowerPC/JitArm64/Jit.cpp": ("engine", False),
            "Source/Core/Core/HLE/HLE.cpp": ("engine", False),
            "Source/Core/Core/State.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Core.cpp": ("engine", False),
            "Source/Core/Core/IOS/Network/SSL.cpp": ("engine-sensitive", False),
            "Source/Core/Core/IOS/IOS.cpp": ("engine", False),
            "Source/Core/Common/WebSocket.cpp": ("engine-sensitive", False),
            "Source/Core/Common/Timer.cpp": ("engine", False),
            "Source/Core/DolphinNoGUI/Orca.entitlements": ("engine-sensitive", False),
            "Source/Core/VideoCommon/HiresTextures.cpp": ("engine-sensitive", False),
            "Source/Core/VideoCommon/Present.cpp": ("engine", False),
            "Source/Core/DolphinQt/MainWindow.cpp": ("engine", True),  # never built into Orca
            "Source/Core/Core/NetPlayClient.cpp": ("engine-sensitive", False),
            "Tools/orca/upstream-sync.py": ("engine-sensitive", True),
            "Externals/libjuice/CMakeLists.txt": ("engine-sensitive", False),
            "Externals/licenses.md": ("engine-sensitive", False),
            "CMakeLists.txt": ("engine-sensitive", False),
            "ORCA.md": ("engine", True),
            "Source/UnitTests/Core/Orca/Games/Brawl/QueueTest.cpp": ("game-logic", True),
            # Tests the engine's RankedSteps.
            "Source/UnitTests/Core/Orca/RankedTest.cpp": ("engine", True),
            "Source/UnitTests/Core/Orca/RoomTest.cpp": ("engine", True),
            "Tools/orca/queue-e2e.mjs": ("engine", True),
            "Tools/orca/inputs/bf-dropin.txt": ("game-data", True),
            "Tools/orca/textures/make-labels.py": ("engine", True),
            "Tools/orca/layers.txt": ("engine-sensitive", True),
            "Tools/orca/layer-of-diff": ("engine-sensitive", True),
            "Tools/orca/layerlib.py": ("engine-sensitive", True),
            "Tools/orca/forbidden-calls": ("engine-sensitive", True),
            "Tools/orca/forbidden-calls.baseline": ("engine-sensitive", True),
            "Tools/orca/firewall": ("engine-sensitive", True),
            "Tools/orca/firewall.baseline": ("engine-sensitive", True),
            "Tools/orca/test_firewall.py": ("engine-sensitive", True),
            "Tools/orca/test_layers.py": ("engine-sensitive", True),
            "Tools/orca-package-macos.sh": ("engine-sensitive", True),
        }
        for path, (cls, dev) in expect.items():
            m = self.rules.classify(path)
            self.assertEqual((m.cls, m.dev), (cls, dev), "%s matched %s" % (path, m.rule))
            self.assertFalse(m.ties, path)
        # Competitive policy in game logic: a person reviews it.
        for path in ("Source/Core/Core/Orca/Games/Brawl/Queue.cpp", "Source/Core/Core/Orca/Games/Brawl/RankedSet.h",
                     "Source/Core/Core/Orca/Games/Brawl/Results.cpp"):
            m = self.rules.classify(path)
            self.assertEqual((m.cls, m.review), ("game-logic", True), path)
        self.assertFalse(self.rules.classify("Source/Core/Core/Orca/Games/Brawl/NameTags.cpp").review)
        # ...and engine-side, once S2 commit 7 moved the machines out (Luis's decision 11).
        for path in ("Source/Core/Core/Orca/Policy/Queue.cpp", "Source/Core/Core/Orca/Policy/CharOrder.h"):
            m = self.rules.classify(path)
            self.assertEqual((m.cls, m.review), ("engine", True), path)

    def test_peer_files_are_engine_sensitive(self):
        """forbidden-calls' PEER_FILES (where peer-parse applies) must all be engine-sensitive, so the
        two lists can't drift (Session/Events is the documented exception: no parsing)."""
        tracked = [p for p in layerlib.git(self.repo, "ls-files", "-z").split("\0") if p]
        for glob, rx in zip(fc.PEER_FILES, fc.PEER_RE):
            matched = [p for p in tracked if rx.match(p)]
            if glob != "Source/Core/Core/Orca/Mod.*":  # mods-p1, not on main yet
                self.assertTrue(matched, glob)
            for p in matched:
                if p.startswith("Source/Core/Core/Orca/Session/Events."):
                    continue
                self.assertEqual(self.rules.classify(p).cls, "engine-sensitive", "%s (%s)" % (p, glob))

    def test_network_and_process_code_is_engine_sensitive(self):
        """Any shipped Orca file with socket, http or process hits must be engine-sensitive (this
        would have caught UX/ControllerSource, a curl client classed engine)."""
        paths, _ = layerlib.orca_paths(self.repo, self.rules, "HEAD")
        paths = [p for p in paths if fc.scannable(p, self.rules)]
        wrong = set()
        for p, text in fc.read_files(self.repo, "HEAD", paths):
            cls = self.rules.classify(p).cls
            for rule, token, no, _ in fc.scan_text(p, text, cls):
                if rule in ("socket", "http", "process") and cls != "engine-sensitive":
                    wrong.add("%s:%d %s %s (%s)" % (p, no, rule, token, cls))
        self.assertEqual(sorted(wrong), [])


DIFF = """diff --git a/A.cpp b/A.cpp
index 1..2 100644
--- a/A.cpp
+++ b/A.cpp
@@ -1,3 +1,4 @@
 keep
--- not a header, a removed line starting with --
+++ not a header, an added line starting with ++
+added
 keep
diff --git a/New.h b/New.h
new file mode 100644
--- /dev/null
+++ b/New.h
@@ -0,0 +1,2 @@
+one
+two
diff --git a/Gone.h b/Gone.h
deleted file mode 100644
--- a/Gone.h
+++ /dev/null
@@ -1 +0,0 @@
-bye
diff --git a/Old.cpp b/Moved.cpp
similarity index 90%
rename from Old.cpp
rename to Moved.cpp
--- a/Old.cpp
+++ b/Moved.cpp
@@ -1 +1 @@
-x
+y
diff --git a/pic.png b/pic.png
Binary files a/pic.png and b/pic.png differ
"""


class DiffParseTests(unittest.TestCase):
    def test_unified_diff(self):
        got = {c.path: (c.added, c.deleted, c.binary) for c in layerlib.parse_unified_diff(DIFF)}
        self.assertEqual(got["A.cpp"], (2, 1, False))
        self.assertEqual(got["New.h"], (2, 0, False))
        self.assertEqual(got["Gone.h"], (0, 1, False))
        self.assertEqual(got["Old.cpp"][1], 1)
        self.assertEqual(got["Moved.cpp"][0], 1)
        self.assertTrue(got["pic.png"][2])
        self.assertEqual(set(got), {"A.cpp", "New.h", "Gone.h", "Old.cpp", "Moved.cpp", "pic.png"})

    def test_plain_diff_u_with_two_files(self):
        text = ("--- a/x.txt\t2026-01-01\n+++ b/x.txt\t2026-01-02\n@@ -1,2 +1,2 @@\n-a\n+b\n c\n"
                "--- a/y.txt\n+++ b/y.txt\n@@ -1 +1,2 @@\n y\n+z\n")
        got = {c.path: (c.added, c.deleted) for c in layerlib.parse_unified_diff(text)}
        self.assertEqual(got, {"x.txt": (1, 1), "y.txt": (1, 0)})

    def test_malformed_diff_fails_closed(self):
        # The first hunk claims more lines than it has: the next file's header would be read as
        # context, hiding an engine-sensitive file.
        hidden = ("diff --git a/Data/Sys/Orca/RSBE01.patches b/Data/Sys/Orca/RSBE01.patches\n"
                  "--- a/Data/Sys/Orca/RSBE01.patches\n+++ b/Data/Sys/Orca/RSBE01.patches\n"
                  "@@ -1,9 +1,9 @@\n-a\n+b\n"
                  "diff --git a/Source/Core/Core/Orca/Session/Room.cpp b/Source/Core/Core/Orca/Session/Room.cpp\n"
                  "--- a/Source/Core/Core/Orca/Session/Room.cpp\n+++ b/Source/Core/Core/Orca/Session/Room.cpp\n"
                  "@@ -1 +1 @@\n-x\n+y\n")
        with self.assertRaises(ValueError):
            layerlib.parse_unified_diff(hidden)
        with self.assertRaises(ValueError):
            layerlib.parse_unified_diff("--- a/x\n+++ b/x\n@@ -1,2 +1,2 @@\n-a\n+b\n")  # EOF
        with self.assertRaises(ValueError):
            layerlib.parse_unified_diff("@@ -1 +1 @@\n-a\n+b\n")  # no file
        p = subprocess.run([sys.executable, os.path.join(HERE, "layer-of-diff"), "--json", "-"],
                           input=hidden.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 2, p.stdout)

    def test_numstat(self):
        got = layerlib.parse_numstat("3\t1\tA.cpp\0-\t-\tpic.png\0")
        self.assertEqual([(c.path, c.added, c.deleted, c.binary) for c in got],
                         [("A.cpp", 3, 1, False), ("pic.png", 0, 0, True)])


def tokens(path, text, cls="engine"):
    return [(rule, token) for rule, token, _, _ in fc.scan_text(path, text, cls)]


def game(text, path="Source/Core/Core/Orca/Games/Brawl/NameTags.cpp"):
    return tokens(path, text, "game-logic")


class ScanTests(unittest.TestCase):
    UX = "Source/Core/Core/Orca/Games/Brawl/Queue.cpp"
    ROOM = "Source/Core/Core/Orca/Session/YouGameRoom.cpp"

    def test_comments_are_blanked_strings_kept(self):
        text = ('a(); // connect(fd)\n/* getenv("X")\n system("y") */ b();\n'
                's = "// not a comment"; c = \'/\'; n = 1\'000;\n')
        out = fc.strip_c_comments(text)
        self.assertNotIn("connect", out)
        self.assertNotIn("getenv", out)
        self.assertIn('"// not a comment"', out)
        self.assertEqual(out.count("\n"), text.count("\n"))

    def test_raw_string(self):
        out = fc.strip_c_comments('x = R"(a // b)"; // c\n')
        self.assertIn("a // b", out)
        self.assertNotIn("// c", out)

    def test_socket(self):
        self.assertIn(("socket", "connect("), tokens(self.UX, "int r = connect(fd, a, n);\n"))
        self.assertIn(("socket", "::bind("), tokens(self.UX, "::bind(fd, a, n);\n"))
        self.assertIn(("socket", "#include sys/socket.h"), tokens(self.UX, "#include <sys/socket.h>\n"))
        self.assertEqual(tokens(self.UX, "auto f = std::bind(g, 1);\nm_ws.send(x);\nroom->connect(y);\n"),
                         [])

    def test_words_inside_strings_are_not_calls(self):
        self.assertEqual(tokens(self.UX, 'Log("character select (frame {})", f);\n'), [])
        self.assertEqual(tokens(self.UX, 'auto s = R"(system("x"))";\n'), [])

    def test_http(self):
        self.assertIn(("http", "curl_easy_init("), tokens(self.UX, "CURL* c = curl_easy_init();\n"))
        self.assertIn(("http", '"https://evil.example/x"'),
                      tokens(self.UX, 'const char* u = "https://evil.example/x";\n'))
        self.assertIn(("http", "Common::HttpRequest"), tokens(self.UX, "Common::HttpRequest req;\n"))

    def test_file_open_outside_user_dir(self):
        self.assertIn(("file-open", "std::fopen("), tokens(self.UX, 'FILE* f = std::fopen("/etc/x", "r");\n'))
        self.assertIn(("file-open", "File::IOFile"), tokens(self.UX, 'File::IOFile f(path, "rb");\n'))
        self.assertIn(("file-open", "std::ofstream"), tokens(self.UX, "std::ofstream out(p);\n"))
        self.assertEqual(tokens(self.UX, "fs.Rename(a, b);\nHostFileSystem::Delete(a);\n"), [])
        self.assertEqual(tokens(self.UX, "v.erase(std::remove(v.begin(), v.end(), x), v.end());\n"), [])

    def test_file_in_the_user_dir(self):
        # Outside game logic: its own baselined rule, never skipped.
        user = 'File::IOFile f(File::GetUserPath(D_USER_IDX) + "x", "wb");\n'
        self.assertEqual(tokens(self.UX, user), [("file-user", "File::IOFile")])
        # One token anywhere on the line used to exempt it.
        self.assertEqual(tokens(self.UX, 'File::IOFile f(p, "wb"); auto unused = D_USER_IDX;\n'),
                         [("file-user", "File::IOFile")])
        self.assertIn(("file-user", "File::WriteStringToFile("), tokens(
            self.UX, 'File::WriteStringToFile(File::GetUserPath(D_CONFIG_IDX) + "Dolphin.ini", s);\n'))
        # Climbing out of it is a plain file-open.
        self.assertIn(("file-open", "File::IOFile"), tokens(
            self.UX, 'File::IOFile f(File::GetUserPath(D_USER_IDX) + "../../../Library/LaunchAgents/x.plist", "wb");\n'))
        # In game logic every file operation is file-open.
        self.assertEqual(game(user), [("file-open", "File::IOFile")])

    def test_app_channel(self):
        # (Tokens drop whitespace, inside strings too.)
        self.assertIn(("app-channel", 'Orca::Status::Line("orcaresultx"'),
                      tokens(self.UX, 'Orca::Status::Line("orca result x");\n'))
        self.assertIn(("app-channel", 'Status::Error("profile"'),
                      tokens(self.UX, 'Status::Error("profile", "bad");\n'))
        for line in ('printf("orca error x\\n");', 'std::puts("orca x");', 'fputs(s, stdout);',
                     'std::cout << "orca x";', 'fmt::print("orca x\\n");', 'write(1, buf, n);',
                     'fprintf(stderr, "x");', 'auto h = GetStdHandle(STD_OUTPUT_HANDLE);'):
            self.assertTrue([t for t in tokens(self.UX, line + "\n") if t[0] == "app-channel"], line)
        self.assertEqual(tokens(self.UX, 'snprintf(b, n, "x"); m.write(1, 2); s.printf();\n'), [])

    def test_include_escape(self):
        self.assertIn(("include-escape", "#include ../../../../../Tools/orca/textures/x.inc"),
                      tokens(self.UX, '#include "../../../../../Tools/orca/textures/x.inc"\n'))
        self.assertEqual(tokens(self.UX, '#include "Core/Orca/Games/Brawl/Queue.h"\n'), [])

    def test_bare_names_in_game_logic(self):
        self.assertIn(("env", "bare std::getenv"), game('auto v = (std::getenv)("X");\n'))
        self.assertIn(("env", "bare std::getenv"), game("#define E std::getenv\n"))
        self.assertIn(("env", "bare std::getenv"), game("auto* g = &std::getenv;\n"))
        self.assertIn(("process", "bare ::system"), game("auto f = ::system;\n"))
        self.assertIn(("dynload", "bare dlsym"), game("auto f = dlsym;\n"))
        # A backslash-newline splitting the name.
        self.assertIn(("env", 'std::getenv("X"'), game('auto v = std::get\\\nenv("X");\n'))
        # Not in engine code, and not a member or a variable that only shares a name.
        self.assertEqual(tokens(self.UX, "auto* g = &std::getenv;\n"), [])
        self.assertEqual(game("auto& system = Core::System::GetInstance(); system.Go(); m.getenv;\n"), [])

    def test_game_only_rules(self):
        self.assertIn(("determinism", "std::thread"), game("std::thread t(f);\n"))
        self.assertIn(("determinism", "steady_clock"), game("auto n = std::chrono::steady_clock::now();\n"))
        self.assertIn(("determinism", "rand("), game("int r = rand();\n"))
        self.assertIn(("determinism", "time(nullptr)"), game("auto t = time(nullptr);\n"))
        self.assertEqual(game("auto s = clock(step);\n"), [])  # a lambda named clock
        self.assertIn(("settings", "Config::SetBase("), game("Config::SetBase(Config::MAIN_CPU_THREAD, true);\n"))
        self.assertIn(("settings", "State::SaveAs("), game('State::SaveAs(guard, "x");\n'))
        self.assertEqual(tokens(self.UX, "std::thread t(f);\nConfig::SetBase(x, 1);\n"), [])
        self.assertIn(("socket", "mbedtls_net_connect("), tokens(self.UX, "mbedtls_net_connect(&c, h, p, 0);\n"))

    def test_scannable(self):
        rules = layerlib.load_rules()
        self.assertTrue(fc.scannable("Source/Core/Core/Orca/Games/Brawl/NameTags.inc", rules))
        self.assertTrue(fc.scannable("Source/Core/Core/Orca/Games/BrawlRev2/table.txt", rules))
        self.assertFalse(fc.scannable("Source/Core/Core/Orca/UX/Overlay.txt", rules))
        self.assertFalse(fc.scannable("Tools/orca/textures/x.inc", rules))  # dev

    def test_process_dynload_env(self):
        self.assertIn(("process", 'std::system("rm-rf"'), tokens(self.UX, 'std::system("rm-rf");\n'))
        self.assertIn(("process", "posix_spawn("), tokens(self.UX, "posix_spawn(&pid, p, 0, 0, a, e);\n"))
        self.assertIn(("dynload", 'dlopen("libx.dylib"'), tokens(self.UX, 'void* h = dlopen("libx.dylib", 0);\n'))
        self.assertIn(("dynload", "LoadLibraryW("), tokens(self.UX, "LoadLibraryW(name);\n"))
        self.assertIn(("env", 'std::getenv("ORCA_X"'), tokens(self.UX, 'auto v = std::getenv("ORCA_X");\n'))
        self.assertIn(("env", 'GetEnv("ORCA_Y"'), tokens(self.UX, 'auto v = GetEnv("ORCA_Y");\n'))

    def test_peer_parse_only_in_peer_files(self):
        line = "std::memcpy(&x, data, 4); auto s = v.get<std::string>();\n"
        self.assertIn(("peer-parse", "std::memcpy("), tokens(self.ROOM, line))
        self.assertIn(("peer-parse", ".get<"), tokens(self.ROOM, line))
        self.assertEqual(tokens(self.UX, line), [])

    def test_cmake(self):
        text = ("# find_package(Commented)\nfind_package(CURL REQUIRED)\n"
                "target_link_libraries(core\n  PUBLIC common\n  PRIVATE juice $<$<BOOL:WIN32>:ws2_32>\n)\n")
        got = tokens("Source/Core/Core/CMakeLists.txt", text)
        self.assertIn(("build-deps", "find_package(CURL)"), got)
        self.assertIn(("build-deps", "target_link_libraries(core juice)"), got)
        self.assertIn(("build-deps", "target_link_libraries(core common)"), got)
        self.assertNotIn(("build-deps", "find_package(Commented)"), got)
        more = ("execute_process(COMMAND curl x)\nadd_custom_command(OUTPUT a COMMAND b)\n"
                "file(DOWNLOAD https://x f)\nfile(GLOB s *.cpp)\ntarget_sources(core PRIVATE x.cpp)\n"
                "target_compile_options(core PRIVATE -include evil.h)\ninclude(Evil)\n")
        got = tokens("Source/Core/Core/Orca/Games/X/CMakeLists.txt", more)
        for t in ("execute_process(COMMAND)", "add_custom_command(OUTPUT)", "file(DOWNLOAD)",
                  "target_sources(core)", "target_compile_options(-include)", "include(Evil)"):
            self.assertIn(("build-deps", t), got)
        self.assertNotIn(("build-deps", "file(GLOB)"), got)

    def test_baseline_round_trip(self):
        from collections import Counter
        counts = Counter({("env", "a.cpp", 'getenv("X"'): 2, ("socket", "b.cpp", "connect("): 1})
        text = fc.format_baseline(counts, "abc")
        self.assertEqual(fc.load_baseline(text), counts)
        with self.assertRaises(ValueError):
            fc.load_baseline("1\tnot-a-rule\ta\tb\n")


class EndToEndTests(unittest.TestCase):
    """Both scripts against a scratch repository with a fake upstream base."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="orca-layers-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def git(self, *args):
        return subprocess.run(["git", "-C", self.tmp, "-c", "user.name=t", "-c", "user.email=t@t",
                               "-c", "commit.gpgsign=false"] + list(args),
                              check=True, stdout=subprocess.PIPE).stdout.decode().strip()

    def write(self, path, text):
        full = os.path.join(self.tmp, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)

    def run_tool(self, name, *args):
        p = subprocess.run([sys.executable, os.path.join(HERE, name), "--repo", self.tmp] + list(args),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout.decode(), p.stderr.decode()

    def test_flow(self):
        self.git("init", "-q")
        self.write("Source/Core/Common/Util.cpp", 'const char* p = getenv("PATH");\n')
        self.git("add", "-A")
        self.git("commit", "-qm", "upstream")
        base = self.git("rev-parse", "HEAD")
        rules = os.path.join(self.tmp, "rules.txt")
        with open(os.path.join(HERE, "layers.txt")) as f:
            text = f.read()
        text = text.replace("%upstream-ref upstream/master", "%upstream-ref nope/master")
        text = layerlib.re.sub(r"%upstream-base \w+", "%upstream-base " + base, text)
        with open(rules, "w") as f:
            f.write(text)
        self.write("Source/Core/Core/Orca/Games/Brawl/Queue.cpp", "void Frame() {}\n")
        self.write("Data/Sys/Orca/RSBE01.patches", "x\n")
        self.git("add", "Source", "Data")
        self.git("commit", "-qm", "orca")
        bl = os.path.join(self.tmp, "bl.txt")
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--update-baseline",
                                       "--baseline", bl)
        self.assertEqual(code, 0, err)

        # A data-only change.
        self.write("Data/Sys/Orca/RSBE01.patches", "x\ny\n")
        self.git("commit", "-qam", "data")
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--json", "HEAD")
        self.assertEqual(code, 0, err)
        s = json.loads(out)
        self.assertEqual((s["highest"], s["highest_shipped"]), ("game-data", "game-data"))

        # Game logic that opens a socket.
        self.write("Source/Core/Core/Orca/Games/Brawl/Queue.cpp",
                   'void Frame() {}\nvoid Leak() { connect(3, 0, 0); getenv("HOME"); }\n')
        self.git("commit", "-qam", "logic")
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--max", "game-data", "HEAD~1..HEAD")
        self.assertEqual(code, 1, out + err)
        self.assertIn("REFUSED", out)
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--max", "game-logic", "HEAD")
        self.assertEqual(code, 0, out + err)
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--baseline", bl,
                                       "--rev", "HEAD", "--since", "HEAD~1", "--json")
        self.assertEqual(code, 1, err)
        new = json.loads(out)["new"]
        self.assertEqual(sorted((n["rule"], n["token"]) for n in new),
                         [("env", 'getenv("HOME"'), ("socket", "connect(")])
        self.assertTrue(all(l["added"] for n in new for l in n["lines"]))

        # Orca's first change to an upstream file: only what Orca adds is new.
        self.write("Source/Core/Common/Util.cpp",
                   'const char* p = getenv("PATH");\nconst char* h = getenv("HOME");\n')
        self.git("commit", "-qam", "upstream-ish")
        bl2 = os.path.join(self.tmp, "bl2.txt")
        self.run_tool("forbidden-calls", "--rules", rules, "--update-baseline", "--baseline", bl2,
                      "--rev", "HEAD~3")
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--baseline", bl2,
                                       "--rev", "HEAD", "--json")
        got = sorted((n["rule"], n["path"], n["token"]) for n in json.loads(out)["new"])
        self.assertIn(("env", "Source/Core/Common/Util.cpp", 'getenv("HOME"'), got)
        self.assertNotIn(("env", "Source/Core/Common/Util.cpp", 'getenv("PATH"'), got)
        self.git("reset", "-q", "--hard", "HEAD~1")

        # The working tree, a stdin diff, and --files.
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--baseline", bl)
        self.assertEqual(code, 1, err)
        diff = self.git("diff", "HEAD~2", "HEAD")
        p = subprocess.run([sys.executable, os.path.join(HERE, "layer-of-diff"), "--repo", self.tmp,
                            "--rules", rules, "--json", "-"], input=diff.encode(), stdout=subprocess.PIPE)
        self.assertEqual(json.loads(p.stdout)["highest"], "game-logic")
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--json", "--files",
                                       "Tools/orca/layers.txt", "ORCA.md")
        s = json.loads(out)
        self.assertEqual((s["highest"], s["highest_shipped"]), ("engine-sensitive", None))

        # Game logic that edits a baselined line (same count, another file) is reported with --since.
        self.write("Source/Core/Core/Orca/Games/Brawl/Queue.cpp",
                   '// baselined\nvoid Frame() {}\nvoid Leak() { connect(3, 0, 0); getenv("HOME"); }\n')
        self.git("commit", "-qam", "baseline it")
        bl3 = os.path.join(self.tmp, "bl3.txt")
        self.run_tool("forbidden-calls", "--rules", rules, "--update-baseline", "--baseline", bl3)
        self.write("Source/Core/Core/Orca/Games/Brawl/Queue.cpp",
                   'void Frame() {}\nvoid Leak() {\n  connect(4, 0, 0); getenv("HOME"); }\n')
        self.git("commit", "-qam", "moved")
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--baseline", bl3,
                                       "--rev", "HEAD", "--since", "HEAD~1", "--json")
        s = json.loads(out)
        self.assertEqual(code, 1, err)
        self.assertEqual(s["new"], [])
        self.assertEqual(sorted(h["rule"] for h in s["edited"]), ["env", "socket"])

    def test_gate_runs_the_trusted_revs_tools(self):
        """With --rules-rev / --baseline-rev, the running tools must be that rev's copies."""
        self.git("init", "-q")
        os.makedirs(os.path.join(self.tmp, "Tools/orca"))
        for name in ("layers.txt", "forbidden-calls.baseline") + layerlib.TOOL_FILES:
            shutil.copy(os.path.join(HERE, name), os.path.join(self.tmp, "Tools/orca", name))
        self.write("README", "x\n")
        self.git("add", "-A")
        self.git("commit", "-qm", "tools")
        code, out, err = self.run_tool("layer-of-diff", "--rules-rev", "HEAD", "--files", "README")
        self.assertEqual(code, 0, err)
        # The trusted rev's tools differ from the running ones (a branch edited them): refused.
        with open(os.path.join(self.tmp, "Tools/orca/layerlib.py"), "a") as f:
            f.write("# lowered\n")
        self.git("commit", "-qam", "edit a tool")
        code, out, err = self.run_tool("layer-of-diff", "--rules-rev", "HEAD", "--files", "README")
        self.assertEqual(code, 2, out)
        self.assertIn("layerlib.py", err)
        code, out, err = self.run_tool("forbidden-calls", "--baseline-rev", "HEAD")
        self.assertEqual(code, 2, out)
        self.assertIn("layerlib.py", err)


if __name__ == "__main__":
    unittest.main()
