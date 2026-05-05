#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# clang-format gate. Runs `clang-format --dry-run --Werror` over every
# tracked C++ source/header under include/, src/, tests/, and ci/. Fails
# if any file would be reformatted.
#
# Per docs/coding-standards.md §1: "PRs with formatting drift fail CI."

set -euo pipefail

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "format-check: $CLANG_FORMAT not found on PATH" >&2
    exit 2
fi

echo "format-check: using $($CLANG_FORMAT --version)"

# Globbing — keep paths in step with the directories that hold real C++.
mapfile -t FILES < <(
    find include src tests ci \
        -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) \
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
