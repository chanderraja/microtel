#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Report the leaf's own share of a linked firmware image, from a GNU ld map.

Used by ci/scripts/leaf-footprint.sh (docs/leaf-concentrator-design.md §7.6,
ICP 0031 gate 4). The image is examples/leaf/size_probe.c linked with
--gc-sections, so the map lists only the input sections that survived. This
sums them per leaf archive and per section kind, and prints a Markdown report
with one clear line against the flash target.

The targets are not gates in v1.2 (ICP 0031: < 15 KB nanopb, < 30 KB upb;
v2.1 makes the nanopb one a gate), so the exit status is 0 whether the leaf
is over or under. It is 1 only when the map holds no leaf section at all,
which means the probe was not linked against the leaf.

Usage: leaf-footprint.py <map> <label> <target-bytes>
"""

import re
import sys
from collections import defaultdict

# Archive basename -> the part of the leaf it is. The leaf archive holds the
# core and one backend, told apart by object name (see part_of).
PARTS = {
    "libmicrotel_leaf.a": "leaf core",
    "libmicrotel_nanopb.a": "nanopb runtime",
    "libmicrotel_nanopb_gen.a": "nanopb descriptors",
    "libmicrotel_upb_runtime.a": "upb runtime",
    "libmicrotel_upb_gen.a": "upb mini-tables",
    "libmicrotel_utf8_range.a": "utf8_range",
}

KINDS = ("text", "rodata", "data", "bss", "unwind")

# One input section on one line, or its name alone with the rest on the next.
ONE_LINE = re.compile(r"^ (\.\S+|COMMON)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S+)$")
NAME_ONLY = re.compile(r"^ (\.\S+|COMMON)\s*$")
REST = re.compile(r"^\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S+)$")
ARCHIVE = re.compile(r"([^/\\(]+\.a)\(")


def kind_of(section):
    """The section kind an input section counts under, or None."""
    if section.startswith(".text"):
        return "text"
    if section.startswith(".rodata"):
        return "rodata"
    if section.startswith(".data"):
        return "data"
    if section.startswith(".bss") or section == "COMMON":
        return "bss"
    if section.startswith((".eh_frame", ".ARM.exidx", ".ARM.extab")):
        return "unwind"
    return None


def input_sections(lines):
    """Yields (section, size, source) for every kept input section."""
    in_map = False
    pending = None
    for line in lines:
        if line.startswith("Linker script and memory map"):
            in_map = True
            continue
        if not in_map:
            continue
        match = ONE_LINE.match(line)
        if match:
            yield match.group(1), int(match.group(2), 16), match.group(3)
            pending = None
            continue
        if pending is not None:
            rest = REST.match(line)
            if rest:
                yield pending, int(rest.group(1), 16), rest.group(2)
            pending = None
            continue
        name = NAME_ONLY.match(line)
        if name:
            pending = name.group(1)


def part_of(source):
    """The leaf part an input section's source belongs to, or None."""
    archive = ARCHIVE.search(source)
    if archive is None or archive.group(1) not in PARTS:
        return None
    if archive.group(1) == "libmicrotel_leaf.a" and "(backend_" in source:
        return "leaf backend"
    return PARTS[archive.group(1)]


def tally(lines):
    """Bytes per (part, kind) for the leaf archives."""
    sizes = defaultdict(int)
    for section, size, source in input_sections(lines):
        part = part_of(source)
        kind = kind_of(section)
        if part is None or kind is None:
            continue
        sizes[(part, kind)] += size
    return sizes


def report(sizes, label, target):
    """The Markdown report; returns (text, flash_bytes)."""
    parts = sorted({part for part, _ in sizes})
    rows = ["| part | " + " | ".join(f".{k}" if k != "unwind" else "unwind" for k in KINDS) + " |",
            "|---|" + "---:|" * len(KINDS)]
    totals = defaultdict(int)
    for part in parts:
        cells = []
        for kind in KINDS:
            cells.append(f"{sizes.get((part, kind), 0):,}")
            totals[kind] += sizes.get((part, kind), 0)
        rows.append(f"| {part} | " + " | ".join(cells) + " |")
    rows.append("| **total** | " + " | ".join(f"**{totals[k]:,}**" for k in KINDS) + " |")
    flash = totals["text"] + totals["rodata"] + totals["data"]
    ram = totals["data"] + totals["bss"]
    verdict = "UNDER" if flash < target else "OVER"
    text = "\n".join([
        f"### Leaf footprint: {label}",
        "",
        "Input sections from the leaf's archives kept in `size_probe` "
        "(init, one span, one attribute, streaming encode; `-Os`, `--gc-sections`).",
        "",
        *rows,
        "",
        f"- Flash (`.text` + `.rodata` + `.data`): **{flash:,} bytes**",
        f"- Static RAM (`.data` + `.bss`): {ram:,} bytes",
        f"- Unwind tables (not counted; firmware drops them with "
        f"`-fno-asynchronous-unwind-tables`): {totals['unwind']:,} bytes",
        f"- **{verdict} the {target / 1024:g} KB flash target** "
        f"({flash:,} of {target:,} bytes). A target, not a gate, in v1.2.",
        "",
    ])
    return text, flash


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8", errors="replace") as handle:
        sizes = tally(handle)
    if not sizes:
        print(f"leaf-footprint: no leaf input sections in {argv[1]}", file=sys.stderr)
        return 1
    text, _ = report(sizes, argv[2], int(argv[3]))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
