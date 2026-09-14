#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Convert an lcov tracefile into SonarQube's generic test coverage XML.

Why this exists (issue #211)
----------------------------
``sonar-project.properties`` used to hand ``build/coverage/coverage.filtered.info``
to the C++ analyzer via ``sonar.cfamily.llvm-cov.reportPath``.  That key names a
*llvm-cov* report, not an lcov tracefile.  The sensor opened the file, found
nothing it recognised, and imported zero records without emitting a warning —
the whole project read 0.0% coverage while ``ci/scripts/coverage.sh`` measured
~91% on the same tracefile.  A second hazard sat behind the first: the
tracefile carries **absolute** ``SF:`` paths
(``/home/runner/work/microtel/microtel/src/...``), which do not match the
repo-relative keys SonarQube indexes files under — true of ``lcov --capture
--directory``, which wrote it then, and of ``llvm-cov export``, which writes
it now.

This script removes both failure modes at once.  It emits the
language-agnostic generic format, which is imported by
``sonar.coverageReportPaths`` rather than by the cfamily sensor, and it rebases
every path to be relative to the repository root:

    <coverage version="1">
      <file path="src/sdk/sdk_span.cpp">
        <lineToCover lineNumber="42" covered="true"/>
        <lineToCover lineNumber="43" covered="false"/>
      </file>
    </coverage>

Reference: https://docs.sonarsource.com/sonarqube-cloud/enriching/test-coverage/generic-test-data/

Lines only, no branches
-----------------------
The generic format can carry ``branchesToCover``/``coveredBranches`` per line,
and the tracefile's ``BRDA:`` records would supply them.  They are dropped.

The old reason for dropping them is gone: issue #198 moved the coverage build
to clang source-based instrumentation, so a ``BRDA`` record now corresponds to
a conditional someone wrote rather than to gcov's unwind edge out of every
potentially-throwing call, and ``ci/scripts/coverage.sh`` enforces a branch
floor on it.  What remains is that importing conditions changes what SonarQube
*measures* — it folds them into its single ``coverage`` number, so the
project's coverage and its new-code quality gate would both move — and that is
a separate decision from the CI gate #198 was about.  Emitting them is a small
change to :func:`build_xml` when someone wants to make it; nothing here is
load-bearing against it.

``BRDA`` records are inert rather than special-cased: :func:`parse_lcov`
dispatches on the ``DA:`` prefix, which ``BRDA:`` does not have.

Silence is the bug, so this script is loud
------------------------------------------
Importing nothing is the exact failure #211 is about.  Converting a tracefile
into an empty report is therefore an error, not a zero: the script exits 2 and
says what it saw.  It also prints a one-line summary of what it emitted, so the
CI log carries positive evidence that coverage reached the scanner.

Usage:
    ci/scripts/lcov-to-sonar.py [options] TRACEFILE

Options:
    --base-dir DIR      Repository root that absolute SF: paths are rebased
                        against (default: the current directory).
    --include PREFIX    Only emit files whose repo-relative path starts with
                        PREFIX.  Repeatable.  Defaults to emitting everything.
                        Used in CI to mirror `sonar.sources`, so the importer
                        is never handed a path SonarQube has not indexed.
    -o, --output FILE   Write XML here (default: stdout).

Exit codes:
    0  report written with at least one file
    2  the tracefile was missing, unreadable, or yielded no coverable line
"""

from __future__ import annotations

import argparse
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# SonarQube's generic coverage format version. The only value the importer
# accepts; it is not this script's version number.
_FORMAT_VERSION = "1"

_EXIT_NO_COVERAGE = 2


def _relative_path(source_path: str, base_dir: Path) -> str | None:
    """Rebase one ``SF:`` path to be relative to ``base_dir``.

    Returns ``None`` for a file outside ``base_dir`` — a system header or a
    vendored dependency that survived coverage.sh's ``--remove`` filter.  Those
    are not microtel's code and SonarQube has no file to attach them to.
    """
    candidate = Path(source_path)
    if not candidate.is_absolute():
        # Already relative: normalise and keep it, unless it escapes the root.
        normalised = os.path.normpath(source_path)
        return None if normalised.startswith("..") else normalised

    try:
        resolved = candidate.resolve().relative_to(base_dir)
    except ValueError:
        return None
    return resolved.as_posix()


def parse_lcov(text: str, base_dir: Path) -> tuple[dict[str, dict[int, int]], int]:
    """Parse an lcov tracefile into ``{relative path: {line: hit count}}``.

    Records for the same file may appear more than once (one per translation
    unit that included it).  Hit counts are summed, matching how lcov itself
    merges tracefiles: a line executed in any run is covered.

    Returns the mapping plus the number of file records skipped for sitting
    outside ``base_dir``.
    """
    files: dict[str, dict[int, int]] = {}
    skipped_outside = 0
    current: dict[int, int] | None = None

    for raw_line in text.splitlines():
        line = raw_line.strip()

        if line.startswith("SF:"):
            relative = _relative_path(line[len("SF:") :], base_dir)
            if relative is None:
                skipped_outside += 1
                current = None
            else:
                current = files.setdefault(relative, {})
        elif line.startswith("DA:") and current is not None:
            _record_line_hit(line[len("DA:") :], current)
        elif line == "end_of_record":
            current = None

    return files, skipped_outside


def _record_line_hit(payload: str, into: dict[int, int]) -> None:
    """Fold one ``DA:<line>,<hits>[,<checksum>]`` record into a file's map."""
    fields = payload.split(",")
    if len(fields) < 2:
        return
    try:
        line_number = int(fields[0])
        hits = int(fields[1])
    except ValueError:
        # A malformed record is dropped rather than aborting the conversion;
        # the emitted-vs-parsed counts in the summary make the loss visible.
        return
    if line_number > 0:
        into[line_number] = into.get(line_number, 0) + hits


def build_xml(files: dict[str, dict[int, int]]) -> ET.ElementTree:
    """Render parsed coverage as a generic-format XML tree."""
    root = ET.Element("coverage", {"version": _FORMAT_VERSION})

    for path in sorted(files):
        file_element = ET.SubElement(root, "file", {"path": path})
        for line_number in sorted(files[path]):
            covered = "true" if files[path][line_number] > 0 else "false"
            ET.SubElement(
                file_element,
                "lineToCover",
                {"lineNumber": str(line_number), "covered": covered},
            )

    return ET.ElementTree(root)


def _apply_include_filter(
    files: dict[str, dict[int, int]],
    prefixes: list[str],
) -> tuple[dict[str, dict[int, int]], int]:
    """Keep only files under one of ``prefixes``; return them and the drop count."""
    if not prefixes:
        return files, 0

    normalised = [prefix.rstrip("/") + "/" for prefix in prefixes]
    kept = {
        path: lines
        for path, lines in files.items()
        if any(path.startswith(prefix) for prefix in normalised)
    }
    return kept, len(files) - len(kept)


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert an lcov tracefile to SonarQube generic coverage XML.",
    )
    parser.add_argument("tracefile", help="path to the lcov .info tracefile")
    parser.add_argument(
        "--base-dir",
        default=".",
        help="repository root absolute SF: paths are rebased against",
    )
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        metavar="PREFIX",
        help="only emit files under this repo-relative prefix (repeatable)",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="-",
        help="write XML here (default: stdout)",
    )
    return parser.parse_args(argv)


def _write(tree: ET.ElementTree, destination: str) -> None:
    if destination == "-":
        tree.write(sys.stdout.buffer, encoding="UTF-8", xml_declaration=True)
        sys.stdout.buffer.write(b"\n")
        return

    Path(destination).parent.mkdir(parents=True, exist_ok=True)
    tree.write(destination, encoding="UTF-8", xml_declaration=True)


def main(argv: list[str]) -> int:
    args = _parse_args(argv)

    base_dir = Path(args.base_dir).resolve()
    tracefile = Path(args.tracefile)

    try:
        text = tracefile.read_text(encoding="UTF-8", errors="replace")
    except OSError as error:
        print(f"lcov-to-sonar: cannot read '{tracefile}': {error}", file=sys.stderr)
        return _EXIT_NO_COVERAGE

    files, skipped_outside = parse_lcov(text, base_dir)
    files, skipped_filtered = _apply_include_filter(files, args.include)

    if not files:
        print(
            f"lcov-to-sonar: '{tracefile}' produced no coverable line "
            f"({skipped_outside} record(s) outside {base_dir}, "
            f"{skipped_filtered} filtered out by --include). "
            "Refusing to write an empty report — an empty import is the "
            "silent-zero failure of issue #211.",
            file=sys.stderr,
        )
        return _EXIT_NO_COVERAGE

    total_lines = sum(len(lines) for lines in files.values())
    covered_lines = sum(
        1 for lines in files.values() for hits in lines.values() if hits > 0
    )

    _write(build_xml(files), args.output)

    print(
        f"lcov-to-sonar: {len(files)} file(s), {total_lines} line(s) to cover, "
        f"{covered_lines} covered "
        f"({100.0 * covered_lines / total_lines:.1f}%) -> {args.output}",
        file=sys.stderr,
    )
    if skipped_outside or skipped_filtered:
        print(
            f"lcov-to-sonar: skipped {skipped_outside} record(s) outside "
            f"{base_dir} and {skipped_filtered} outside --include prefixes.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
