# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared code for Tools/orca/layer-of-diff and Tools/orca/forbidden-calls.

Reads Tools/orca/layers.txt (format described at its top), classes paths, and reads diffs from
git or from a unified diff. Python 3.8+, standard library only.
"""

import os
import re
import subprocess
import sys

# Least to most risky. A diff's layer is the highest of its paths' layers.
CLASSES = ("game-data", "game-logic", "engine", "engine-sensitive")
RANK = {name: i for i, name in enumerate(CLASSES)}
FLAGS = ("dev", "review")

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_RULES = os.path.join(HERE, "layers.txt")
RULES_PATH_IN_REPO = "Tools/orca/layers.txt"


class RulesError(ValueError):
    pass


class Rule:
    __slots__ = ("cls", "glob", "source", "dev", "review", "line", "literal", "regex")

    def __init__(self, cls, glob, source, dev, line, review=False):
        self.cls = cls
        self.glob = glob  # one alternative after brace expansion
        self.source = source  # the glob as written
        self.dev = dev
        self.review = review
        self.line = line
        self.literal = literal_count(glob)
        self.regex = re.compile(glob_to_regex(glob))

    def __repr__(self):
        return "Rule(%s %s%s%s @%d)" % (self.cls, self.glob, " dev" if self.dev else "",
                                        " review" if self.review else "", self.line)


class Rules:
    def __init__(self, rules, orca_roots, upstream_ref, upstream_base):
        self.rules = rules
        self.orca_roots = orca_roots
        self.upstream_ref = upstream_ref
        self.upstream_base = upstream_base
        self._cache = {}

    def classify(self, path):
        """Returns a Match for path: the winning rule and any rules tied with it."""
        hit = self._cache.get(path)
        if hit is None:
            hit = Match(path, [r for r in self.rules if r.regex.match(path)])
            self._cache[path] = hit
        return hit


class Match:
    __slots__ = ("path", "rule", "ties")

    def __init__(self, path, matching):
        self.path = path
        if not matching:
            raise RulesError("no rule matches %s (layers.txt needs a ** fallback)" % path)
        best = max(r.literal for r in matching)
        top = [r for r in matching if r.literal == best]
        self.rule = top[0]
        # Tied rules that disagree make the map ambiguous for this path.
        self.ties = [r for r in top[1:]
                     if (r.cls, r.dev, r.review) != (top[0].cls, top[0].dev, top[0].review)]

    @property
    def cls(self):
        return self.rule.cls

    @property
    def dev(self):
        return self.rule.dev

    @property
    def review(self):
        return self.rule.review

    @property
    def fallback(self):
        return self.rule.glob == "**"


def literal_count(glob):
    """Number of literal characters: everything but wildcards and character classes."""
    return len(re.sub(r"\[[^\]]*\]|\*|\?", "", glob))


def expand_braces(glob):
    m = re.search(r"\{([^{}]*)\}", glob)
    if not m:
        return [glob]
    out = []
    for alt in m.group(1).split(","):
        out.extend(expand_braces(glob[: m.start()] + alt + glob[m.end() :]))
    return out


def glob_to_regex(glob):
    i, n, out = 0, len(glob), []
    while i < n:
        c = glob[i]
        if glob.startswith("**", i):
            before_ok = i == 0 or glob[i - 1] == "/"
            after = glob[i + 2 : i + 3]
            if before_ok and after == "/":
                out.append("(?:.*/)?")  # **/ : zero or more directories
                i += 3
                continue
            if before_ok and after == "":
                out.append(".*")
                i += 2
                continue
            raise RulesError("`**` must be a whole path segment: %s" % glob)
        if c == "*":
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        elif c == "[":
            j = glob.find("]", i + 1)
            if j < 0:
                raise RulesError("unclosed [ in %s" % glob)
            body = glob[i + 1 : j]
            if body.startswith("!"):
                body = "^" + body[1:]
            out.append("[" + body.replace("\\", "\\\\") + "]")
            i = j
        else:
            out.append(re.escape(c))
        i += 1
    return "(?:" + "".join(out) + r")\Z"


def parse_rules(text, name="layers.txt"):
    rules, roots, ref, base = [], [], None, None
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if fields[0].startswith("%"):
            key, args = fields[0][1:], fields[1:]
            if len(args) != 1:
                raise RulesError("%s:%d: %%%s takes one value" % (name, lineno, key))
            if key == "orca-root":
                roots.append(args[0])
            elif key == "upstream-ref":
                ref = args[0]
            elif key == "upstream-base":
                base = args[0]
            else:
                raise RulesError("%s:%d: unknown directive %%%s" % (name, lineno, key))
            continue
        if len(fields) < 2:
            raise RulesError("%s:%d: expected <class> <glob> [dev] [review]" % (name, lineno))
        cls, glob, flags = fields[0], fields[1], fields[2:]
        if cls not in RANK:
            raise RulesError("%s:%d: unknown class %r (one of %s)" % (name, lineno, cls, ", ".join(CLASSES)))
        for flag in flags:
            if flag not in FLAGS:
                raise RulesError("%s:%d: unknown flag %r" % (name, lineno, flag))
        if glob.startswith("/") or "\\" in glob:
            raise RulesError("%s:%d: globs are relative to the repository root, with /" % (name, lineno))
        for alt in expand_braces(glob):
            rules.append(Rule(cls, alt, glob, "dev" in flags, lineno, "review" in flags))
    if not any(r.glob == "**" for r in rules):
        raise RulesError("%s: no `**` fallback rule" % name)
    return Rules(rules, roots, ref, base)


def load_rules(path=None, repo=None, rev=None):
    """Rules from a file, or from Tools/orca/layers.txt at a git revision."""
    if rev:
        text = git(repo, "show", "%s:%s" % (rev, RULES_PATH_IN_REPO))
        return parse_rules(text, "%s:%s" % (rev, RULES_PATH_IN_REPO))
    path = path or DEFAULT_RULES
    with open(path, encoding="utf-8") as f:
        return parse_rules(f.read(), path)


# --- git -------------------------------------------------------------------------------------


class GitError(RuntimeError):
    pass


def git(repo, *args, check=True, binary=False):
    cmd = ["git"]
    if repo:
        cmd += ["-C", repo]
    cmd += list(args)
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and p.returncode != 0:
        raise GitError("%s: %s" % (" ".join(cmd), p.stderr.decode("utf-8", "replace").strip()))
    return p.stdout if binary else p.stdout.decode("utf-8", "surrogateescape")


def repo_root(start=None):
    return git(start or HERE, "rev-parse", "--show-toplevel").strip()


TOOL_FILES = ("layerlib.py", "layer-of-diff", "forbidden-calls")


def check_tools_at(repo, rev):
    """A gate that trusts `rev`'s map (--rules-rev / --baseline-rev) must also run `rev`'s tools:
    raises GitError naming each running file that differs from its copy at rev. Run them from a
    checkout of rev, with --repo <branch worktree>."""
    differ = []
    for name in TOOL_FILES:
        with open(os.path.join(HERE, name), "rb") as f:
            running = f.read()
        p = subprocess.run(["git", "-C", repo, "show", "%s:Tools/orca/%s" % (rev, name)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0 or p.stdout != running:
            differ.append(name)
    if differ:
        raise GitError("the running %s differ%s from %s's copy: a gate runs %s's tools (a checkout "
                       "of it, with --repo <branch>), or drop --rules-rev/--baseline-rev to try "
                       "this copy" % (", ".join(differ), "s" if len(differ) == 1 else "", rev, rev))


def ref_exists(repo, ref):
    return git(repo, "rev-parse", "--verify", "--quiet", ref + "^{commit}", check=False).strip() != ""


def upstream_base(repo, rules, rev="HEAD"):
    """The upstream Dolphin commit Orca's changes are measured from."""
    if rules.upstream_ref and ref_exists(repo, rules.upstream_ref):
        out = git(repo, "merge-base", rev, rules.upstream_ref, check=False).strip()
        if out:
            return out
    if rules.upstream_base and ref_exists(repo, rules.upstream_base):
        return git(repo, "rev-parse", rules.upstream_base).strip()
    raise GitError("no upstream base: fetch %s or set %%upstream-base" % rules.upstream_ref)


def orca_paths(repo, rules, rev="HEAD"):
    """Orca's own paths at rev (None = the working tree): every file under an %orca-root plus every
    file that differs from the upstream merge-base."""
    base = upstream_base(repo, rules, "HEAD" if rev is None else rev)
    if rev is None:
        tracked = git(repo, "ls-files", "-z").split("\0")
        untracked = git(repo, "ls-files", "-z", "--others", "--exclude-standard").split("\0")
        files = set(p for p in tracked + untracked if p and os.path.lexists(os.path.join(repo, p)))
        changed = git(repo, "diff", "--name-only", "-z", "--no-renames", "--diff-filter=d", base)
        changed = set(p for p in changed.split("\0") if p) | set(p for p in untracked if p)
    else:
        files = set(p for p in git(repo, "ls-tree", "-r", "-z", "--name-only", rev).split("\0") if p)
        changed = git(repo, "diff", "--name-only", "-z", "--no-renames", "--diff-filter=d", base, rev)
        changed = set(p for p in changed.split("\0") if p)
    out = set(p for p in files if any(p.startswith(root) for root in rules.orca_roots))
    out |= changed & files
    return sorted(out), base


class FileChange:
    __slots__ = ("path", "added", "deleted", "binary")

    def __init__(self, path, added=0, deleted=0, binary=False):
        self.path = path
        self.added = added
        self.deleted = deleted
        self.binary = binary


def parse_numstat(text):
    """`git diff --numstat -z --no-renames` output."""
    out = []
    for rec in text.split("\0"):
        rec = rec.strip("\n")
        if not rec:
            continue
        parts = rec.split("\t", 2)
        if len(parts) != 3:
            continue
        a, d, path = parts
        binary = a == "-"
        out.append(FileChange(path, 0 if binary else int(a), 0 if binary else int(d), binary))
    return out


def diff_numstat(repo, spec):
    return parse_numstat(git(repo, "diff", "--numstat", "-z", "--no-renames", *spec))


def unquote_git_path(p):
    if p.startswith('"') and p.endswith('"'):
        body = p[1:-1].encode("latin-1", "backslashreplace").decode("unicode_escape")
        return body.encode("latin-1").decode("utf-8", "replace")
    return p


def parse_unified_diff(text):
    """Per-file added/deleted line counts from a unified diff (git diff, format-patch, or diff -u).
    Both sides of a rename are reported."""
    files, order = {}, []

    def touch(path):
        if path not in files:
            files[path] = FileChange(path)
            order.append(path)
        return files[path]

    def strip_prefix(p):
        p = unquote_git_path(p)
        return p[2:] if p[:2] in ("a/", "b/") else p

    old = new = None  # the hunk's sides
    left = right = 0  # lines still expected in the current hunk
    for no, line in enumerate(text.splitlines(), 1):
        if left > 0 or right > 0:
            if line.startswith("\\"):
                continue
            tag = line[:1]
            if tag == "+" and right > 0:
                touch(new or old).added += 1
                right -= 1
            elif tag == "-" and left > 0:
                touch(old or new).deleted += 1
                left -= 1
            elif tag in (" ", "") and left > 0 and right > 0:
                # Context (a blank line is context with its space trimmed).
                left -= 1
                right -= 1
            else:
                # Fail closed: a hunk that claims more lines than it has would hide the next file.
                raise ValueError("malformed diff at line %d: the hunk ends early" % no)
            continue
        if line.startswith("diff --git "):
            m = re.match(r'diff --git ("a/.*?"|a/\S.*?) ("b/.*"|b/.*)$', line)
            old = new = None
            if m:
                old, new = strip_prefix(m.group(1)), strip_prefix(m.group(2))
                touch(old)
                touch(new)
            continue
        if line.startswith("--- ") or line.startswith("+++ "):
            p = line[4:].split("\t")[0].strip()
            p = None if p == "/dev/null" else strip_prefix(p)
            if line.startswith("--- "):
                old, new = p, None
            else:
                new = p
            if p:
                touch(p)
            continue
        if line.startswith("Binary files "):
            for p in (old, new):
                if p:
                    touch(p).binary = True
            continue
        m = re.match(r"@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@", line)
        if m:
            if not (old or new):
                raise ValueError("malformed diff at line %d: a hunk outside a file" % no)
            left = int(m.group(1)) if m.group(1) is not None else 1
            right = int(m.group(2)) if m.group(2) is not None else 1
    if left > 0 or right > 0:
        raise ValueError("malformed diff: the last hunk ends early")
    return [files[p] for p in order]


def die(msg, code=2):
    sys.stderr.write("%s\n" % msg)
    sys.exit(code)
