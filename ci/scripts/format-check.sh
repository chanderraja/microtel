#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# clang-format gate. Runs `clang-format --dry-run --Werror` over every
# tracked C++ source/header under include/, src/, tests/, ci/, and examples/.
# Fails if any file would be reformatted.
#
# Per docs/coding-standards.md §1: "PRs with formatting drift fail CI."
#
# examples/ is in the list because CI compiles it: the `cxx20 / clang` job
# configures with -DMICROTEL_BUILD_EXAMPLES=ON. Examples are also the code a
# reader is most likely to copy, so drift there is worse than drift in a test.
# Issue #281.

set -euo pipefail

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "format-check: $CLANG_FORMAT not found on PATH" >&2
    exit 2
fi

echo "format-check: using $($CLANG_FORMAT --version)"

# Globbing — keep paths in step with the directories that hold real C++, plus
# the C leaf (leaf/) and its C test vectors, which share the same style
# (docs/leaf-concentrator-design.md §7.8).
mapfile -t FILES < <(
    find include src tests ci examples leaf \
        -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.c" \) \
        2>/dev/null | sort
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "format-check: no C++ files found"
    exit 0
fi

echo "format-check: scanning ${#FILES[@]} files..."

# `--dry-run --Werror` exits non-zero on any formatting violation.
"$CLANG_FORMAT" --dry-run --Werror -- "${FILES[@]}"

echo "format-check: clean"
