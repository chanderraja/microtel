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

# Translation units to lint: ci/header_check.cpp, src/**/*.cpp, tests/**/*.cpp,
# plus the C of the leaf (leaf/**/*.c) and its C test vectors (tests/**/*.c).
mapfile -t TUS < <(
    {
        find ci src tests -type f -name "*.cpp"
        find leaf tests -type f -name "*.c"
    } 2>/dev/null | sort
)

# C translation units get the leaf's C profile (docs/leaf-concentrator-design.md
# §7.8) instead of the root C++ one, whose cppcoreguidelines / modernize /
# naming checks have no meaning in C.
C_TIDY_CONFIG="leaf/.clang-tidy"

if [[ ${#TUS[@]} -eq 0 ]]; then
    echo "tidy-check: no TUs to lint"
    exit 0
fi

# Only lint TUs the build actually compiles. A file absent from
# compile_commands.json — because it sits behind an off-by-default CMake option,
# such as MICROTEL_BUILD_OTELCPP_SHIM — would otherwise be linted with
# clang-tidy's fallback flags, which cannot resolve its include paths and
# reports a bogus clang-diagnostic-error. Skipping is correct; skipping
# *silently* is not, so the skipped set is always printed.
mapfile -t COMPILED < <(
    python3 -c '
import json, os, sys
with open(sys.argv[1], encoding="utf-8") as fh:
    for entry in json.load(fh):
        print(os.path.realpath(os.path.join(entry.get("directory", ""), entry["file"])))
' "${BUILD_DIR}/compile_commands.json" | sort -u
)

declare -A IN_BUILD=()
for f in "${COMPILED[@]}"; do
    IN_BUILD["$f"]=1
done

LINT=()
SKIPPED=()
for tu in "${TUS[@]}"; do
    if [[ -n "${IN_BUILD[$(realpath "$tu")]:-}" ]]; then
        LINT+=("$tu")
    else
        SKIPPED+=("$tu")
    fi
done

if [[ ${#SKIPPED[@]} -gt 0 ]]; then
    echo "tidy-check: skipping ${#SKIPPED[@]} TU(s) not in this build's compile_commands.json:"
    printf '    %s\n' "${SKIPPED[@]}"
    echo "tidy-check: configure with the relevant CMake option ON to lint them."
fi

if [[ ${#LINT[@]} -eq 0 ]]; then
    echo "tidy-check: no TUs from this build to lint" >&2
    exit 2
fi

echo "tidy-check: linting ${#LINT[@]} TUs..."

failures=0
for tu in "${LINT[@]}"; do
    echo "  -> $tu"
    config=()
    if [[ "$tu" == *.c ]]; then
        config=(--config-file="$C_TIDY_CONFIG")
    fi
    if ! "$CLANG_TIDY" --quiet --warnings-as-errors='*' "${config[@]}" -p "$BUILD_DIR" "$tu"; then
        failures=$((failures + 1))
    fi
done

if [[ $failures -gt 0 ]]; then
    echo "tidy-check: $failures TUs had findings" >&2
    exit 1
fi

echo "tidy-check: clean"
