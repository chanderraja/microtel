# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Load and validate bench/profiles/*.yaml."""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import List


@dataclasses.dataclass(frozen=True)
class Profile:
    name: str
    description: str
    spans_per_sample: int
    samples: int
    warmup_spans: int
    sink_mode: str      # "blackhole" | "collector"
    suts: List[str]     # SUT names enabled for this profile
    metrics: List[str]  # metric keys to include in report


_SINK_MODES = {"blackhole", "collector"}


def load(profiles_dir: Path, name: str) -> Profile:
    """Load and validate a profile YAML by name (without .yaml suffix)."""
    path = profiles_dir / f"{name}.yaml"
    if not path.exists():
        raise FileNotFoundError(f"profile not found: {path}")

    data = parse_yaml(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"profile {name!r}: expected a mapping at top level")

    workload = data.get("workload") or {}
    sink = data.get("sink") or {}
    sink_mode = sink.get("mode", "blackhole") if isinstance(sink, dict) else "blackhole"
    if sink_mode not in _SINK_MODES:
        raise ValueError(f"profile {name!r}: unknown sink.mode {sink_mode!r}")

    raw_suts = data.get("suts") or []
    raw_metrics = data.get("metrics") or []

    return Profile(
        name=str(data.get("profile", name)),
        description=str(data.get("description", "")),
        spans_per_sample=int(workload.get("spans_per_sample", 10_000)),
        samples=int(workload.get("samples", 10)),
        warmup_spans=int(workload.get("warmup_spans", 1_000)),
        sink_mode=sink_mode,
        suts=[str(s) for s in raw_suts] if isinstance(raw_suts, list) else [],
        metrics=[str(m) for m in raw_metrics] if isinstance(raw_metrics, list) else [],
    )


# ---------------------------------------------------------------------------
# Minimal YAML parser — stdlib only, no PyPI deps.
# Supports the subset used by bench/profiles/*.yaml and bench/sut/registry.yaml:
#   scalars (string, int, float, bool, null), block mappings, block sequences,
#   inline values, single/double quoted strings, line comments.
#   Does NOT support: flow style {}, [], multi-line scalars, anchors/aliases.
# ---------------------------------------------------------------------------

def parse_yaml(text: str):
    """Parse a YAML document into plain Python dicts/lists/scalars."""
    tokens = list(_tokenize(text))
    value, pos = _parse_value(tokens, 0, -1)
    return value


# ------------------------------------------------------------------
# Tokenizer: converts raw lines into (indent, kind, text) triples.
# kind: "key", "dash", "scalar", "blank"
# ------------------------------------------------------------------

def _tokenize(text: str):
    for line in text.splitlines():
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#"):
            yield (0, "blank", "")
            continue
        indent = len(line) - len(stripped)
        # Strip inline comment (but not inside quotes)
        content = _strip_comment(stripped)
        if content.startswith("- ") or content == "-":
            yield (indent, "dash", content[2:].strip() if content.startswith("- ") else "")
        elif ":" in content:
            key, _, rest = content.partition(":")
            rest = rest.strip()
            yield (indent, "key", (key.strip(), rest))
        else:
            yield (indent, "scalar", content)


def _strip_comment(s: str) -> str:
    """Remove trailing # comment, respecting quoted strings."""
    in_sq = in_dq = False
    for i, ch in enumerate(s):
        if ch == "'" and not in_dq:
            in_sq = not in_sq
        elif ch == '"' and not in_sq:
            in_dq = not in_dq
        elif ch == "#" and not in_sq and not in_dq:
            return s[:i].rstrip()
    return s


# ------------------------------------------------------------------
# Recursive descent parser over the token stream.
# ------------------------------------------------------------------

def _parse_value(tokens, pos, parent_indent):
    """Parse one value starting at pos.  Returns (value, new_pos)."""
    pos = _skip_blank(tokens, pos)
    if pos >= len(tokens):
        return None, pos

    indent, kind, text = tokens[pos]

    if indent <= parent_indent:
        return None, pos

    if kind == "dash":
        return _parse_sequence(tokens, pos, indent)

    if kind == "key":
        return _parse_mapping(tokens, pos, indent)

    if kind == "scalar":
        return _coerce(text), pos + 1

    return None, pos + 1


def _parse_mapping(tokens, pos, indent):
    """Parse a block mapping whose keys start at `indent`."""
    result = {}
    while pos < len(tokens):
        pos = _skip_blank(tokens, pos)
        if pos >= len(tokens):
            break
        cur_indent, kind, text = tokens[pos]
        if cur_indent < indent or kind != "key":
            break
        if cur_indent > indent:
            break
        key, inline = text
        pos += 1
        if inline:
            # Inline scalar value
            result[key] = _coerce(inline)
        else:
            # Value is on following lines
            pos = _skip_blank(tokens, pos)
            if pos >= len(tokens):
                result[key] = None
                continue
            next_indent, next_kind, _ = tokens[pos]
            if next_indent <= indent:
                result[key] = None
            elif next_kind == "dash":
                val, pos = _parse_sequence(tokens, pos, next_indent)
                result[key] = val
            elif next_kind == "key":
                val, pos = _parse_mapping(tokens, pos, next_indent)
                result[key] = val
            else:
                val, pos = _parse_value(tokens, pos, indent)
                result[key] = val
    return result, pos


def _parse_sequence(tokens, pos, indent):
    """Parse a block sequence whose dashes start at `indent`."""
    result = []
    while pos < len(tokens):
        pos = _skip_blank(tokens, pos)
        if pos >= len(tokens):
            break
        cur_indent, kind, text = tokens[pos]
        if cur_indent < indent or kind != "dash":
            break
        pos += 1
        if text:
            # Check if the inline text is a mapping key
            if ":" in text and not text.startswith(('"', "'")):
                key, _, rest = text.partition(":")
                rest = rest.strip()
                item = {key.strip(): _coerce(rest) if rest else None}
                # Parse remaining key-value pairs at (indent + 2)
                item_indent = cur_indent + 2
                pos = _skip_blank(tokens, pos)
                while pos < len(tokens):
                    pos = _skip_blank(tokens, pos)
                    if pos >= len(tokens):
                        break
                    ni, nk, nt = tokens[pos]
                    if ni < item_indent or nk != "key" or ni <= cur_indent:
                        break
                    k, inline = nt
                    pos += 1
                    if inline:
                        item[k] = _coerce(inline)
                    else:
                        pos = _skip_blank(tokens, pos)
                        if pos < len(tokens):
                            vi, vk, _ = tokens[pos]
                            if vi > ni:
                                if vk == "dash":
                                    val, pos = _parse_sequence(tokens, pos, vi)
                                else:
                                    val, pos = _parse_mapping(tokens, pos, vi)
                                item[k] = val
                            else:
                                item[k] = None
                result.append(item)
            else:
                result.append(_coerce(text))
        else:
            # Value is on next lines
            pos = _skip_blank(tokens, pos)
            if pos < len(tokens):
                next_indent, next_kind, _ = tokens[pos]
                if next_kind == "dash":
                    val, pos = _parse_sequence(tokens, pos, next_indent)
                elif next_kind == "key":
                    val, pos = _parse_mapping(tokens, pos, next_indent)
                else:
                    val, pos = _parse_value(tokens, pos, cur_indent)
                result.append(val)
    return result, pos


def _skip_blank(tokens, pos):
    while pos < len(tokens) and tokens[pos][1] == "blank":
        pos += 1
    return pos


def _coerce(s: str):
    """Convert a YAML scalar string to the appropriate Python type."""
    if not s:
        return None
    if s in ("true", "True", "yes", "Yes"):
        return True
    if s in ("false", "False", "no", "No"):
        return False
    if s in ("null", "~", "Null", "NULL"):
        return None
    if (s.startswith('"') and s.endswith('"')) or \
       (s.startswith("'") and s.endswith("'")):
        return s[1:-1]
    try:
        return int(s)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        pass
    return s
