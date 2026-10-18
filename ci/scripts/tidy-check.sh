#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# clang-tidy gate. Runs clang-tidy against microtel-owned source files using
# the compile_commands.json from the build tree.
#
# Per docs/coding-standards.md §12, .clang-tidy at the repo root carries
# the SonarQube-aligned ruleset. The script just drives the tool against
# the right TU set.
#
# Usage:
#   ci/scripts/tidy-check.sh <build_dir> [base-ref]
#
# The build directory must already exist and contain compile_commands.json
# (CMAKE_EXPORT_COMPILE_COMMANDS=ON is set in the top-level CMakeLists).
#
# Modes (the one that ran is always printed):
#   full     Every TU in the build is linted. The default, and what pushes to
#            master and the nightly run use.
#   changed  A base ref is given (second argument, or TIDY_BASE). Only TUs
#            whose own file changed since `git merge-base <base> HEAD` are
#            linted. This is what pull requests use.
#
# Changed mode falls back to full whenever the diff touches anything that can
# change the findings of a TU that did not itself change: any header (a header
# is linted through every TU that includes it, and working out which TUs those
# are needs a dependency scan), a .clang-tidy config, CMake files, proto/
# template inputs that generate headers, this script, or ci.yml. It also falls
# back to full if the base cannot be resolved. When in doubt, lint everything.
#
# Environment:
#   CLANG_TIDY  clang-tidy binary (default: clang-tidy)
#   TIDY_BASE   base ref/SHA for changed mode (same as the second argument)
#   TIDY_JOBS   parallel clang-tidy processes (default: nproc)
#
# TUs are linted in parallel. Each TU's output is buffered and printed as one
# block when it finishes, so findings from different TUs never interleave.

set -euo pipefail

BUILD_DIR="${1:-build}"
BASE_REF="${2:-${TIDY_BASE:-}}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
JOBS="${TIDY_JOBS:-$(nproc 2>/dev/null || echo 2)}"

if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    echo "tidy-check: $CLANG_TIDY not found on PATH" >&2
    exit 2
fi

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
    echo "tidy-check: ${BUILD_DIR}/compile_commands.json missing — configure CMake first" >&2
    exit 2
fi

if ! command -v flock >/dev/null 2>&1; then
    echo "tidy-check: flock (util-linux) not found on PATH" >&2
    exit 2
fi

if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "tidy-check: TIDY_JOBS must be a positive integer, got '$JOBS'" >&2
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

# needs_full_lint <path> → 0 if a change to <path> can alter the findings of
# TUs other than <path> itself, so changed mode must lint everything.
needs_full_lint()
{
    case "$1" in
        *.h | *.hpp | *.hh | *.hxx | *.inc | *.ipp | *.tpp) return 0 ;;
        .clang-tidy | */.clang-tidy) return 0 ;;
        CMakeLists.txt | */CMakeLists.txt | *.cmake | *.in) return 0 ;;
        *.proto | *.options) return 0 ;;
        ci/scripts/tidy-check.sh | .github/workflows/ci.yml) return 0 ;;
        *) return 1 ;;
    esac
}

MODE="full"
if [[ -n "$BASE_REF" ]]; then
    merge_base=""
    if merge_base="$(git merge-base "$BASE_REF" HEAD 2>/dev/null)"; then
        # Against the working tree, so local runs see uncommitted edits too; in
        # CI the tree is clean and this is merge-base..HEAD. Deleted files are
        # dropped: there is nothing left to lint.
        mapfile -t CHANGED < <(git diff --name-only --diff-filter=d "$merge_base" --)
        trigger=""
        for path in "${CHANGED[@]}"; do
            if needs_full_lint "$path"; then
                trigger="$path"
                break
            fi
        done
        if [[ -n "$trigger" ]]; then
            echo "tidy-check: mode=full (base ${BASE_REF}: ${trigger} changed, which can affect TUs that did not)"
        else
            MODE="changed"
            echo "tidy-check: mode=changed (base ${BASE_REF}, merge-base ${merge_base:0:12}, ${#CHANGED[@]} changed path(s))"
            declare -A IS_CHANGED=()
            for path in "${CHANGED[@]}"; do
                IS_CHANGED["$path"]=1
            done
            SELECTED=()
            for tu in "${TUS[@]}"; do
                if [[ -n "${IS_CHANGED[$tu]:-}" ]]; then
                    SELECTED+=("$tu")
                fi
            done
            TUS=("${SELECTED[@]+"${SELECTED[@]}"}")
            if [[ ${#TUS[@]} -eq 0 ]]; then
                echo "tidy-check: no lintable TUs changed — clean"
                exit 0
            fi
        fi
    else
        echo "tidy-check: mode=full (base ${BASE_REF} has no merge-base with HEAD — is the history fetched?)"
    fi
else
    echo "tidy-check: mode=full (no base ref given)"
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
    if [[ "$MODE" == "changed" ]]; then
        echo "tidy-check: no changed TUs from this build to lint — clean"
        exit 0
    fi
    echo "tidy-check: no TUs from this build to lint" >&2
    exit 2
fi

echo "tidy-check: linting ${#LINT[@]} TUs with ${JOBS} parallel job(s)..."

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
: > "${WORK_DIR}/failures"

# lint_one <tu>: runs clang-tidy on one TU with its output buffered, then
# prints it as a single block under a lock so parallel TUs never interleave.
# A TU with findings is appended to the failures file.
lint_one()
{
    local tu="$1"
    local start=$SECONDS
    local config=()
    local status="ok"
    local out
    if [[ "$tu" == *.c ]]; then
        config=(--config-file="$TIDY_C_CONFIG")
    fi
    if ! out="$("$TIDY_BIN" --quiet --warnings-as-errors='*' "${config[@]}" -p "$TIDY_BUILD_DIR" "$tu" 2>&1)"; then
        status="FAIL"
    fi
    {
        flock 9
        echo "  -> $tu (${status}, $((SECONDS - start))s)"
        if [[ -n "$out" ]]; then
            printf '%s\n' "$out"
        fi
        if [[ "$status" == "FAIL" ]]; then
            echo "$tu" >> "${TIDY_WORK_DIR}/failures"
        fi
    } 9> "${TIDY_WORK_DIR}/lock"
}
export -f lint_one
export TIDY_BIN="$CLANG_TIDY" TIDY_BUILD_DIR="$BUILD_DIR" TIDY_C_CONFIG="$C_TIDY_CONFIG" TIDY_WORK_DIR="$WORK_DIR"

# lint_one records findings itself, so xargs' own status only matters if
# something went wrong outside it (xargs could not start a worker, etc.).
xargs_status=0
# shellcheck disable=SC2016 # $1 is expanded by the worker shell, not here.
printf '%s\0' "${LINT[@]}" |
    xargs -0 -n 1 -P "$JOBS" bash -c 'lint_one "$1"' _ || xargs_status=$?

mapfile -t FAILED < <(sort "${WORK_DIR}/failures")
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "tidy-check: ${#FAILED[@]} TUs had findings:" >&2
    printf '    %s\n' "${FAILED[@]}" >&2
    exit 1
fi

if [[ $xargs_status -ne 0 ]]; then
    echo "tidy-check: a lint worker failed to run (xargs exit ${xargs_status})" >&2
    exit 1
fi

echo "tidy-check: clean (${MODE} mode)"
