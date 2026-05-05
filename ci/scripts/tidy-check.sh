#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# clang-tidy gate. Runs clang-tidy against every microtel-owned source file
# using the compile_commands.json from the build tree.
#
# Per docs/coding-standards.md §12, .clang-tidy at the repo root carries
# the SonarQube-aligned ruleset. The script just drives the tool against
# the right TU set.
#
# Usage:
#   ci/scripts/tidy-check.sh <build_dir>
#
# The build directory must already exist and contain compile_commands.json
# (CMAKE_EXPORT_COMPILE_COMMANDS=ON is set in the top-level CMakeLists).

set -euo pipefail

BUILD_DIR="${1:-build}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"

if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    echo "tidy-check: $CLANG_TIDY not found on PATH" >&2
    exit 2
fi

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
    echo "tidy-check: ${BUILD_DIR}/compile_commands.json missing — configure CMake first" >&2
    exit 2
fi

echo "tidy-check: using $($CLANG_TIDY --version | head -1)"

# Translation units to lint: ci/header_check.cpp + tests/unit/*.cpp.
# When src/ grows real implementations in M3, add a similar find here.
mapfile -t TUS < <(
    find ci tests -type f -name "*.cpp" 2>/dev/null | sort
)

if [[ ${#TUS[@]} -eq 0 ]]; then
    echo "tidy-check: no TUs to lint"
    exit 0
fi

echo "tidy-check: linting ${#TUS[@]} TUs..."

failures=0
for tu in "${TUS[@]}"; do
    echo "  -> $tu"
    if ! "$CLANG_TIDY" --quiet --warnings-as-errors='*' -p "$BUILD_DIR" "$tu"; then
        failures=$((failures + 1))
    fi
done

if [[ $failures -gt 0 ]]; then
    echo "tidy-check: $failures TUs had findings" >&2
    exit 1
fi

echo "tidy-check: clean"
