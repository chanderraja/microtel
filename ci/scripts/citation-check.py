#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Resolve the code citations attached to LOCKED markers in ``docs/`` (ICP 0021).

Why this exists
---------------
Every LOCKED marker in this repository was written in the M0 commit, before any
code existed to check it against, and nothing has re-checked one since.  The
audit behind issue #134 found roughly 45% of them describing a system that does
not exist — including ``ShutdownState``, a type named as the "single source of
truth" for shutdown in three normative documents and present in **no commit**.

ICP 0021 adopts the cheapest mechanism that would have caught that class of
failure:

    A LOCKED claim that cannot cite code is a claim about intent,
    and must be marked as such.

This script is the CI half.  It is deliberately weak: it proves a *symbol
exists*, not that the surrounding sentence is true.  It would not have caught
"v1 has exactly three thread roles".  It is worth running anyway because it
costs nothing and it catches the failure mode that is **undetectable by
reading** — a named type that no commit ever contained.

The grammar
-----------
A LOCKED marker carries one of two annotations::

    (LOCKED — cites `src/sdk/sdk_provider.cpp:Shutdown`)
    (LOCKED — cites `src/a.cpp:Foo`, `src/b.hpp:m_bar`)
    (LOCKED — intent)

``cites`` names ``path:symbol`` — a **function or member name, never a line
number**, because line numbers rot (a citation added in #149 was already stale
by #144).  ``intent`` says there is no code to point at: the claim is a design
commitment, and saying so is the point of the rule.

Two passes
----------
*Resolution* runs over every scanned document: each ``cites`` citation must
name a file that exists and a symbol that appears in it.  A stale citation
fails the build wherever it is written.

*Coverage* runs only over documents that opt in by carrying the line::

    **Citation policy:** complete — ...

In those, every LOCKED marker must carry ``cites`` or ``intent``; a bare
``(LOCKED)`` is a violation.  Opt-in is per document because the #134 audit of
all 94 markers is open work, not this script's job: a document is flipped to
``complete`` by the pass that reconciles it.  ``docs/threading-model.md`` is the
first, per ICP 0021.

Usage:
    ci/scripts/citation-check.py [--root DIR] [--self-test]

Options:
    --root DIR    Repository root to check (default: the current directory).
                  Documents are read from DIR/docs, citations resolve against
                  paths relative to DIR.
    --self-test   Run the built-in fixtures instead of the repository: one tree
                  that must pass and three that must fail (stale symbol,
                  missing file, uncited marker in a `complete` document).  This
                  is the evidence that the gate can actually fail.

Exit codes:
    0  every citation resolves and every `complete` document is fully annotated
    1  at least one violation (or a self-test expectation was not met)
    2  the check could not run (no docs directory)
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

_EXIT_OK = 0
_EXIT_VIOLATIONS = 1
_EXIT_CANNOT_RUN = 2

# Generated snapshots are excluded: `docs/graph-report.md` is a copy of
# graphify's report, which quotes document headings verbatim and truncates them
# ("(+6 more)").  A truncated quote of a real citation would fail this gate for
# a reason that has nothing to do with the documents it copied.  The originals
# are scanned, so nothing is lost.
_EXCLUDED = frozenset({"docs/graph-report.md"})

# A marker and everything up to its closing paren.  Citations carry no nested
# parentheses, so a non-greedy `[^)]*` is an exact parse rather than a guess.
_MARKER_RE = re.compile(r"\(LOCKED(?P<annotation>[^)]*)\)")

# `— cites` / `- cites` / `-- cites`, then the backticked citations.
_CITES_RE = re.compile(r"^\s*(?:—|-{1,2})\s*cites\s+(?P<body>.+)$")
_INTENT_RE = re.compile(r"^\s*(?:—|-{1,2})\s*intent\s*$")
_BACKTICKED_RE = re.compile(r"`([^`]+)`")

# The per-document opt-in into the coverage pass.
_POLICY_RE = re.compile(r"^\*\*Citation policy:\*\*\s+complete\b")

_FENCE_RE = re.compile(r"^\s*(?:```|~~~)")


class Violation:
    """One failure, with enough context to fix it without opening the file."""

    def __init__(self, path: str, line: int, text: str, detail: str) -> None:
        self.path = path
        self.line = line
        self.text = text
        self.detail = detail

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.detail}\n      {self.text}"


def mask_fenced_blocks(text: str) -> str:
    """Blank out fenced code blocks, preserving every line's position.

    Fenced blocks are where the *grammar itself* is documented — `docs/icps/
    README.md` shows both annotation forms — so scanning them would make
    documenting the rule a violation of it.  Lines are emptied rather than
    dropped so that offsets in the result still map to real line numbers, which
    is what lets a marker whose annotation wraps across lines be parsed as one
    marker instead of a bare one.
    """
    masked: list[str] = []
    in_fence = False
    for line in text.splitlines():
        if _FENCE_RE.match(line):
            in_fence = not in_fence
            masked.append("")
            continue
        masked.append("" if in_fence else line)
    return "\n".join(masked)


def parse_citations(body: str) -> tuple[list[tuple[str, str]], str | None]:
    """Split a ``cites`` annotation body into ``(path, symbol)`` pairs.

    Returns the pairs and, on a malformed annotation, a message saying what is
    wrong with it.
    """
    tokens = _BACKTICKED_RE.findall(body)
    if not tokens:
        return [], "`cites` names no backticked `path:symbol` citation"

    pairs: list[tuple[str, str]] = []
    for token in tokens:
        if token.count(":") != 1:
            return [], f"citation `{token}` is not `path:symbol`"
        path, symbol = token.split(":", 1)
        if not path.strip() or not symbol.strip():
            return [], f"citation `{token}` has an empty path or symbol"
        pairs.append((path.strip(), symbol.strip()))
    return pairs, None


def symbol_present(haystack: str, symbol: str) -> bool:
    """True when ``symbol`` appears in ``haystack`` as a whole identifier."""
    pattern = r"(?<![A-Za-z0-9_])" + re.escape(symbol) + r"(?![A-Za-z0-9_])"
    return re.search(pattern, haystack) is not None


def resolve_citation(root: Path, path: str, symbol: str) -> str | None:
    """Return a failure message, or ``None`` when the citation resolves."""
    target = root / path
    if not target.is_file():
        return f"cited file '{path}' does not exist"
    try:
        text = target.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return f"cited file '{path}' could not be read ({exc})"
    if not symbol_present(text, symbol):
        return f"'{symbol}' does not appear in '{path}' — the citation is stale"
    return None


def check_marker(
    root: Path, rel: str, number: int, marker: str, annotation: str, complete: bool
) -> list[Violation]:
    """Check one LOCKED marker: resolve its citations, or demand one."""
    intent = _INTENT_RE.match(annotation)
    cites = _CITES_RE.match(annotation)

    if cites is None:
        if intent or not complete:
            return []
        return [
            Violation(
                rel,
                number,
                marker,
                "LOCKED marker carries no citation. Add "
                "(LOCKED — cites `path:symbol`), or (LOCKED — intent) "
                "if there is no code to point at (ICP 0021)",
            )
        ]

    pairs, malformed = parse_citations(cites.group("body"))
    if malformed is not None:
        return [Violation(rel, number, marker, malformed)]

    violations = []
    for path, symbol in pairs:
        detail = resolve_citation(root, path, symbol)
        if detail is not None:
            violations.append(Violation(rel, number, marker, detail))
    return violations


def check_document(root: Path, doc: Path) -> tuple[list[Violation], int]:
    """Check one markdown document. Returns its violations and marker count."""
    rel = doc.relative_to(root).as_posix()
    text = doc.read_text(encoding="utf-8", errors="replace")
    complete = any(_POLICY_RE.match(line) for line in text.splitlines())
    masked = mask_fenced_blocks(text)

    violations: list[Violation] = []
    markers = 0
    for match in _MARKER_RE.finditer(masked):
        markers += 1
        number = masked.count("\n", 0, match.start()) + 1
        # A wrapped annotation is one marker, so it is normalised to a single
        # line before it is matched and before it is quoted back.
        quoted = " ".join(match.group(0).split())
        annotation = " ".join(match.group("annotation").split())
        violations.extend(
            check_marker(root, rel, number, quoted, annotation, complete)
        )
    return violations, markers


def check_tree(root: Path) -> tuple[list[Violation], int, int]:
    """Check every document under ``root/docs``.

    Returns the violations, the number of documents scanned, and the number of
    LOCKED markers seen.
    """
    docs_dir = root / "docs"
    violations: list[Violation] = []
    scanned = 0
    markers = 0
    for doc in sorted(docs_dir.rglob("*.md")):
        if doc.relative_to(root).as_posix() in _EXCLUDED:
            continue
        found, count = check_document(root, doc)
        violations.extend(found)
        scanned += 1
        markers += count
    return violations, scanned, markers


def report(violations: list[Violation], scanned: int, markers: int) -> int:
    """Print the verdict and return the process exit code."""
    if not violations:
        print(
            f"citation-check: PASS — {markers} LOCKED marker(s) across "
            f"{scanned} document(s); every citation resolves."
        )
        return _EXIT_OK

    print("", file=sys.stderr)
    print("citation-check: FAIL", file=sys.stderr)
    print("", file=sys.stderr)
    for violation in violations:
        print(f"  {violation}", file=sys.stderr)
    print("", file=sys.stderr)
    print(
        "  A LOCKED claim about code cites the code (ICP 0021). Fix the\n"
        "  citation, or mark the claim (LOCKED — intent) if the code it\n"
        "  described does not exist — which is the finding, not a\n"
        "  formality.\n",
        file=sys.stderr,
    )
    return _EXIT_VIOLATIONS


_FIXTURE_SOURCE = """\
void WorkerLoop() noexcept
{
}
"""

_FIXTURE_GOOD = """\
# Fixture

**Citation policy:** complete — fixture.

The worker drains its queue (LOCKED — cites `src/fixture.cpp:WorkerLoop`).

Multi-profile is a v1.1 feature (LOCKED — intent).

An annotation may wrap, and is still one marker (LOCKED — cites
`src/fixture.cpp:WorkerLoop`).

The grammar is shown, not applied, inside a fence:

```
(LOCKED — cites `src/nowhere.cpp:NoSuchThing`)
```
"""

_FIXTURE_STALE = """\
# Fixture

Shutdown state is a single source of truth
(LOCKED — cites `src/fixture.cpp:ShutdownState`).
"""

_FIXTURE_MISSING = """\
# Fixture

The arena is owned by the encoder (LOCKED — cites `src/gone.cpp:UpbArena`).
"""

_FIXTURE_UNCITED = """\
# Fixture

**Citation policy:** complete — fixture.

There is exactly one worker per process (LOCKED).
"""


def _write_fixture(base: Path, name: str, document: str) -> Path:
    """Materialise one fixture tree: a source file plus one document."""
    root = base / name
    (root / "src").mkdir(parents=True)
    (root / "src" / "fixture.cpp").write_text(_FIXTURE_SOURCE, encoding="utf-8")
    (root / "docs").mkdir()
    (root / "docs" / "fixture.md").write_text(document, encoding="utf-8")
    return root


def _run_case(root: Path, name: str, should_pass: bool) -> bool:
    """Run one fixture and say whether it behaved as the rule requires."""
    violations, _, _ = check_tree(root)
    passed = not violations
    verdict = "PASS" if passed == should_pass else "UNEXPECTED"
    expectation = "must pass" if should_pass else "must fail"
    print(f"  [{verdict}] {name} ({expectation})")
    for violation in violations:
        # Printed for every case: on a must-fail fixture this is the evidence
        # that the gate fires, which is the whole point of the self-test.
        print(f"           {str(violation).splitlines()[0]}")
    return passed == should_pass


def self_test() -> int:
    """Prove the gate passes clean input and fails each way it should."""
    cases = [
        ("good", _FIXTURE_GOOD, True),
        ("stale-symbol", _FIXTURE_STALE, False),
        ("missing-file", _FIXTURE_MISSING, False),
        ("uncited-marker", _FIXTURE_UNCITED, False),
    ]

    print("citation-check: self-test")
    ok = True
    with tempfile.TemporaryDirectory(prefix="citation-check-") as tmp:
        base = Path(tmp)
        for name, document, should_pass in cases:
            root = _write_fixture(base, name, document)
            ok = _run_case(root, name, should_pass) and ok

    if not ok:
        print(
            "citation-check: self-test FAILED — the gate does not behave as "
            "ICP 0021 specifies.",
            file=sys.stderr,
        )
        return _EXIT_VIOLATIONS

    print(f"citation-check: self-test PASS — {len(cases)} fixture(s).")
    return _EXIT_OK


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Resolve LOCKED-marker code citations (ICP 0021)."
    )
    parser.add_argument(
        "--root",
        default=".",
        help="repository root to check (default: the current directory)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the built-in fixtures instead of the repository",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    if not (root / "docs").is_dir():
        print(
            f"citation-check: no docs directory under '{root}'. Run this from "
            "the repository root, or pass --root.",
            file=sys.stderr,
        )
        return _EXIT_CANNOT_RUN

    violations, scanned, markers = check_tree(root)
    return report(violations, scanned, markers)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
