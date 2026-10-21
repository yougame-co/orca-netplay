#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""S2 commits 1-3 of the Orca engine/pack split: move the game modules into Orca/Games/Brawl/, and
follow commits 2-3's moves of engine state out of them (names other branches still use).

  Tools/orca/split/s2-move.py               git mv the 18 game modules (35 files) and their 11
                                            tests, then rewrite every path that names them
  Tools/orca/split/s2-move.py --rewrite-only
                                            only rewrite paths (no git mv): for a branch that was
                                            rebased onto the move and added a new #include or CMake
                                            line naming an old path
  Tools/orca/split/s2-move.py --check       list code that still names an old path or a moved name
                                            (exit 1 if any); stale comments are listed, not failed

Run from anywhere inside the checkout. The script only moves files and edits path strings, so
`git diff -M HEAD` shows 100% renames apart from #include lines, CMake source lists, layers.txt and
the tests that pin classified paths. It never commits. It is idempotent: a second run changes
nothing.

Replaying on another branch (one that forked from the V2 line before the move):

  git rebase split-s2            # rename detection carries edits to moved files over
  Tools/orca/split/s2-move.py --rewrite-only
  Tools/orca/split/s2-move.py --check
  git commit -am "Follow the S2 move: include paths"   # only if the rewrite changed anything

A branch that adds a whole new game-module file under Orca/UX/ should `git mv` it into
Orca/Games/Brawl/ by hand and add it to MODULES below on that branch.

Commits 2-3 moved code between files, which rename detection can't follow. A branch that edited
these blocks gets a modify/delete or content conflict and ports the edit into the new file by hand:
  - GuardMemory's bodies (Games/Brawl/NameTags.cpp -> Orca/Engine/GuestMemory.cpp);
  - the Search namespace (Games/Brawl/OnlineMenu.* -> UX/Search.*);
  - OnlinePick, MenuEventText, AnnounceOnlinePick, PickSearchable, the "orca menu cancel" lines
    (Games/Brawl/OnlineMenu.* -> UX/MenuEvents.*);
  - the queue's app side: Begin..ResetTimeouts, the identity codec, the s_* app state and the
    timeout tracker (Games/Brawl/Queue.* -> UX/QueueApp.*; Queue::Frame calls NoteTimeout);
  - TipSeenIn/TipSeenWith/TipSeenPath, TipFrame's file code and NameTags' OwnControlsPath
    (-> Orca/LocalStore.*).
The rewrite below renames the callers' uses (Queue::<app function> -> QueueApp::, TipSeen* ->
LocalStore::Seen*); --check flags what it can't fix (a Search:: use without UX/Search.h, the moved
app state or file I/O back in Games/Brawl).

What is rewritten (tracked files only; docs (*.md), ORCA_NOTES and the forbidden-calls baseline are
left alone: the baseline is reviewed by a person, and notes are history):
  - `Orca/UX/<Module>.<ext>` -> `Orca/Games/Brawl/<Module>.<ext>` (RankedPPlus: Games/Brawl/PPlus/)
    in Source/** and Tools/orca/** (includes, CMake lists, layer tests, free-space.py);
  - `Orca/<Game>Test.cpp` -> `Orca/Games/Brawl/<Game>Test.cpp` (tests' CMake list, layer tests);
  - `Orca/Game/<file>` -> `Orca/Engine/<file>` (the engine folder's first name; git mv too);
  - moved tests that find Data/Sys through `__FILE__` get two more `../`;
  - Tools/orca/layers.txt: the UX/ game-logic lines become one review line under Games/Brawl/ (the
    existing `Games/**` rule classes the rest), and the game tests' lines become `Games/**`;
  - in C/C++ files: `Queue::<app function>` -> `QueueApp::<app function>` (the functions commit 3
    moved), `TipSeen{In,With,Path}` -> `Orca::LocalStore::Seen{In,With,Path}`. Include the engine
    header (UX/QueueApp.h, Orca/LocalStore.h) where the compiler asks for it.
"""

import argparse
import os
import re
import subprocess
import sys

UX = "Source/Core/Core/Orca/UX/"
GAMES = "Source/Core/Core/Orca/Games/Brawl/"
TESTS = "Source/UnitTests/Core/Orca/"
GAME_TESTS_DIR = TESTS + "Games/Brawl/"

# The 18 modules (layers.txt's game-logic lines under UX/, modules.md section 2) and where they go,
# relative to Orca/Games/Brawl/.
MODULES = {
    "BrawlStages": "", "CharOrder": "", "CssTitle": "", "NameTags": "", "OnlineMenu": "",
    "OnlineRules": "", "OnlineSeats": "", "RankedPPlus": "PPlus/", "StageCursors": "",
    "Queue": "", "RankedSet": "", "Results": "", "FreeSpace": "", "MatchBlock": "",
    "SetBlock": "", "MenuText": "", "NativeText": "", "Relabel": "",
}
# Their 11 tests (layers.txt's game-logic test lines).
GAME_TESTS = ["BrawlStages", "CharOrder", "CssTitle", "NativeText", "OnlineMenu", "OnlineRules",
              "OnlineSeats", "Queue", "RankedSet", "Relabel", "Results"]

SKIP = {"Tools/orca/forbidden-calls.baseline", "Tools/orca/split/s2-move.py"}
REWRITE_ROOTS = ("Source/", "Tools/orca/")
REWRITE_EXT = (".h", ".hpp", ".cpp", ".inc", ".ipp", ".mm", ".m", ".c", ".txt", ".cmake", ".py",
               ".mjs", ".sh", ".json", ".in")

MOD_RE = re.compile(r"Orca/UX/(" + "|".join(sorted(MODULES, key=len, reverse=True)) +
                    r")\.(\w+)")
TEST_RE = re.compile(r"(?<=Orca/)(" + "|".join(GAME_TESTS) + r")Test\.cpp")
# Engine files of the split live in Orca/Engine/. S2 commit 2 first put GuestMemory in Orca/Game/,
# one letter from the game modules' Orca/Games/; the coordinator moved it before anyone built on it.
OLD_ENGINE = "Source/Core/Core/Orca/Game/"
ENGINE = "Source/Core/Core/Orca/Engine/"
ENGINE_FILES = ["GuestMemory.h", "GuestMemory.cpp"]
ENGINE_RE = re.compile(r"Orca/Game/(\w+\.\w+)")
_MODS = "|".join(sorted(MODULES, key=len, reverse=True))
# Old paths in a glob or brace form (layers.txt), and bare `UX/<Module>.h` mentions in comments.
GLOB_RE = re.compile(r"Orca/UX/(\{[^}]*\b(" + _MODS + r")\b[^}]*\}|(" + _MODS + r")\.\*)")
BARE_RE = re.compile(r"(?<![/\w])UX/(" + _MODS + r")\.")

# Commit 3: the queue's app side moved to UX/QueueApp.h (namespace QueueApp).
QUEUE_APP = ["Begin", "End", "Active", "Ranked", "SetOwnRating", "OwnRating", "SoloReady",
             "OnSoloCss", "OnQueueCss", "AfterRestore", "ClearReady", "ConfirmTimeout",
             "ResetTimeouts", "TakeTimeoutWasMine", "OwnIdentity"]
QUEUE_APP_RE = re.compile(r"\b((?:Orca::)?(?:UX::)?)Queue::(" + "|".join(QUEUE_APP) + r")\b")
TIP_RE = re.compile(r"(?:\b(?:Orca::)?(?:UX::)?Queue::)?\bTipSeen(In|With|Path)\b")
CODE_EXT = (".h", ".hpp", ".cpp", ".inc", ".ipp", ".mm", ".m", ".c")
# What commit 3 took out of the game files; back in Games/Brawl it is a merge that kept old code.
MOVED_STATE_RE = re.compile(r"\b(s_solo_ready|s_on_solo_css|s_on_queue_css|s_clear_ready|s_tip_on|"
                            r"s_last_timeout_mine|TimeoutTracker|File::\w+|OwnControlsPath)\b")

LAYERS_OLD = """\
# Game logic: hardcoded Brawl rev 2 / Project+ memory.
game-logic        Source/Core/Core/Orca/UX/{BrawlStages,CharOrder,CssTitle,NameTags,OnlineMenu}.*
game-logic        Source/Core/Core/Orca/UX/{OnlineRules,OnlineSeats,RankedPPlus,StageCursors}.*
# ...carrying competitive policy until S2 splits it out: the queue's ready/timer/skip machine,
# ranked stocks and minutes (RankedSet), and the winner and set result the room rates (Results).
game-logic        Source/Core/Core/Orca/UX/{Queue,RankedSet,Results}.* review
# Game memory layouts: free space, code caves, the match block and the set block.
game-logic        Source/Core/Core/Orca/UX/{FreeSpace,MatchBlock,SetBlock}.*
# Mixed: an engine mechanism carrying game data (texture names, message buffers, menu strings).
# Classed by the game data, which is what changes; the mechanism moves to the engine in S5.
game-logic        Source/Core/Core/Orca/UX/{MenuText,NativeText,Relabel}.*
"""
LAYERS_NEW = """\
# Game logic (hardcoded Brawl rev 2 / Project+ memory) moved to Orca/Games/Brawl/ in S2 commit 1
# (Tools/orca/split/s2-move.py) and is classed by the Games/** rule above: stage flows, rules, name
# tags, the match block and set block layouts, free space, and the mixed text modules (an engine
# mechanism carrying game data, classed by the data until S5 moves the mechanism to the engine).
# Three of them carry competitive policy until S2 splits it out: the queue's ready/timer/skip
# machine, ranked stocks and minutes (RankedSet), and the winner and set result the room rates
# (Results).
game-logic        Source/Core/Core/Orca/Games/Brawl/{Queue,RankedSet,Results}.* review
"""
LAYERS_TESTS_OLD = """\
game-logic        Source/UnitTests/Core/Orca/{BrawlStages,CharOrder,CssTitle,NativeText}Test.cpp dev
game-logic        Source/UnitTests/Core/Orca/{OnlineMenu,OnlineRules,OnlineSeats,Queue}Test.cpp dev
game-logic        Source/UnitTests/Core/Orca/{RankedSet,Relabel,Results}Test.cpp dev
"""
LAYERS_TESTS_NEW = """\
game-logic        Source/UnitTests/Core/Orca/Games/**     dev     # game-module tests (S2)
"""


def git(root, *args):
    return subprocess.run(["git", "-C", root, *args], check=True, stdout=subprocess.PIPE,
                          text=True).stdout


def moves(root):
    out = []
    for name, sub in MODULES.items():
        for ext in (".h", ".cpp"):
            src = UX + name + ext
            if os.path.exists(os.path.join(root, src)):
                out.append((src, GAMES + sub + name + ext))
    for name in GAME_TESTS:
        src = TESTS + name + "Test.cpp"
        if os.path.exists(os.path.join(root, src)):
            out.append((src, GAME_TESTS_DIR + name + "Test.cpp"))
    for name in ENGINE_FILES:
        if os.path.exists(os.path.join(root, OLD_ENGINE + name)):
            out.append((OLD_ENGINE + name, ENGINE + name))
    return out


def rewrite_text(path, text):
    def mod(m):
        return "Orca/Games/Brawl/" + MODULES[m.group(1)] + m.group(1) + "." + m.group(2)
    new = MOD_RE.sub(mod, text)
    new = ENGINE_RE.sub(r"Orca/Engine/\1", new)
    new = TEST_RE.sub(lambda m: "Games/Brawl/" + m.group(1) + "Test.cpp", new)
    if path.startswith(GAME_TESTS_DIR):
        # Two directories deeper than before: tests that find Data/Sys from __FILE__.
        new = re.sub(r'(__FILE__\)\.parent_path\(\) / ")\.\./\.\./\.\./\.\./(?!\.\./)',
                     r"\1../../../../../../", new)
    if path.endswith(CODE_EXT):
        new = QUEUE_APP_RE.sub(lambda m: m.group(1) + "QueueApp::" + m.group(2), new)
        new = TIP_RE.sub(lambda m: "Orca::LocalStore::Seen" + m.group(1), new)
    if path == "Tools/orca/layers.txt":
        new = new.replace(LAYERS_OLD, LAYERS_NEW).replace(LAYERS_TESTS_OLD, LAYERS_TESTS_NEW)
    return new


def candidates(root):
    for p in git(root, "ls-files", "-z").split("\0"):
        if not p or p in SKIP or not p.startswith(REWRITE_ROOTS):
            continue
        if p.endswith(".md") or not (p.endswith(REWRITE_EXT) or os.path.basename(p) in
                                     ("forbidden-calls", "layer-of-diff")):
            continue
        yield p


def rewrite(root):
    changed = []
    for p in candidates(root):
        full = os.path.join(root, p)
        if not os.path.isfile(full):
            continue
        with open(full, encoding="utf-8", newline="") as f:
            try:
                text = f.read()
            except UnicodeDecodeError:
                continue
        new = rewrite_text(p, text)
        if new != text:
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(new)
            changed.append(p)
    return changed


def check(root):
    left, notes = [], []
    for p in candidates(root):
        full = os.path.join(root, p)
        if not os.path.isfile(full):
            continue
        try:
            text = open(full, encoding="utf-8").read()
        except UnicodeDecodeError:
            continue
        code = p.endswith(CODE_EXT)
        search_ok = "UX/Search.h" in text or p.endswith("/UX/Search.cpp")
        for i, line in enumerate(text.splitlines(), 1):
            where = "%s:%d: %s" % (p, i, line.strip())
            if MOD_RE.search(line) or TEST_RE.search(line) or GLOB_RE.search(line) or \
                    ENGINE_RE.search(line):
                left.append(where)
            elif code and (QUEUE_APP_RE.search(line) or TIP_RE.search(line)):
                left.append(where)
            elif code and not search_ok and re.search(r"\bSearch::\w", line) and \
                    "//" not in line.split("Search::")[0]:
                left.append(where + "   (needs Core/Orca/UX/Search.h)")
            elif code and p.startswith(GAMES) and MOVED_STATE_RE.search(line.split("//")[0]):
                left.append(where + "   (moved to the engine in S2 commit 3)")
            elif code and BARE_RE.search(line):
                notes.append(where)
    stray = [s for s, _ in moves(root)]
    return left, stray, notes
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rewrite-only", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    root = git(os.path.dirname(os.path.abspath(__file__)), "rev-parse", "--show-toplevel").strip()

    if args.check:
        left, stray, notes = check(root)
        for s in stray:
            print("not moved: " + s)
        for l in left:
            print("old path: " + l)
        for n in notes:
            print("comment: " + n)
        print("s2-move check: %d old paths or names, %d files not moved, %d stale comment "
              "mentions (UX/<Module>.h: now Games/Brawl/; not an error)"
              % (len(left), len(stray), len(notes)))
        return 1 if left or stray else 0

    if not args.rewrite_only:
        todo = moves(root)
        for src, dst in todo:
            os.makedirs(os.path.join(root, os.path.dirname(dst)), exist_ok=True)
            git(root, "mv", src, dst)
        print("moved %d files" % len(todo))
    changed = rewrite(root)
    for p in changed:
        print("rewrote " + p)
    if changed:
        git(root, "add", "--", *changed)
    print("rewrote %d files" % len(changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
