#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Consumer gate — ICP 0020 Decision 6. Installs a build tree to a prefix, then
# configures, builds and *runs* tests/consumer against that prefix as an
# ordinary external CMake project.
#
# Export-set breakage is invisible to every in-tree test: everything builds
# because everything is a subdirectory. A missing `install(FILES …)` for a
# header, an export set naming a target it never installed, or a
# `find_dependency` the config forgot shows up only in a consumer that is not
# part of this build — and only against a real install prefix, never against a
# build directory that still has every source file in place.
#
# The consumer is configured with `-Dmicrotel_DIR=<prefix>/<libdir>/cmake/microtel`
# and nothing else, so it can resolve microtel from the install tree alone.
# `<libdir>` is `lib64` on Fedora and `lib` on Debian/Ubuntu (and can be a
# multiarch path), so it is located rather than assumed: the package config is
# the authority on where it put itself.
#
# `symbol-scan.sh --prefix` runs on the same prefix at the end. The dedicated
# `symbol-scan` CI job already covers it; repeating it here costs a second and
# means the artifacts this gate actually linked against are the ones scanned.
#
# Usage:  ci/scripts/consumer-smoke.sh [build-dir] [prefix]
#
#   build-dir  a configured, built microtel build directory (default: build)
#   prefix     install prefix to stage into (default: a scratch directory,
#              removed on exit along with the consumer build beside it)
#
# The consumer's own build directory is `<prefix>.consumer-build`, wiped at the
# start of every run so a configure is never served from cache. An existing
# prefix is *not* cleaned — pass a fresh one, or none, when what you want to
# check is a clean install tree.
#
# Exit codes:
#   0  the consumer configured, linked, ran and reported success
#   1  the gate failed — install, configure, build, run, or the symbol scan
#   2  the gate could not run (no cmake, build-dir not configured, no consumer
#      project on disk)

set -euo pipefail

BUILD_DIR="${1:-build}"
PREFIX="${2:-}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSUMER_SRC="${REPO_ROOT}/tests/consumer"
SYMBOL_SCAN="${REPO_ROOT}/ci/scripts/symbol-scan.sh"

readonly BINARY_NAME="microtel_consumer_smoke"

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

if ! command -v cmake >/dev/null 2>&1; then
    echo "consumer-smoke: cmake not found on PATH" >&2
    exit 2
fi

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "consumer-smoke: '${BUILD_DIR}' is not a configured CMake build directory" >&2
    echo "consumer-smoke: configure and build first, e.g." >&2
    echo "consumer-smoke:   cmake -S . -B ${BUILD_DIR} -G Ninja -DMICROTEL_BUILD_TESTS=OFF" >&2
    echo "consumer-smoke:   cmake --build ${BUILD_DIR}" >&2
    exit 2
fi

if [[ ! -f "${CONSUMER_SRC}/CMakeLists.txt" ]]; then
    echo "consumer-smoke: no consumer project at ${CONSUMER_SRC}" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Prefix
# ---------------------------------------------------------------------------

OWN_PREFIX=0
if [[ -z "$PREFIX" ]]; then
    PREFIX="$(mktemp -d -t microtel-consumer-prefix-XXXXXX)"
    OWN_PREFIX=1
fi

mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
CONSUMER_BUILD="${PREFIX}.consumer-build"

cleanup()
{
    local status=$?
    if [[ $OWN_PREFIX -eq 1 ]]; then
        rm -rf "$PREFIX" "$CONSUMER_BUILD"
    fi
    return $status
}
trap cleanup EXIT

echo "consumer-smoke: build    = $BUILD_DIR"
echo "consumer-smoke: prefix   = $PREFIX"
echo "consumer-smoke: consumer = $CONSUMER_BUILD"

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

if ! cmake --install "$BUILD_DIR" --prefix "$PREFIX"; then
    echo "consumer-smoke: FAIL — 'cmake --install' failed" >&2
    exit 1
fi

# The package config is the authority on libdir. Its absence is a gate failure,
# not a lookup problem: an install that produces no microtelConfig.cmake is an
# install no consumer can use.
mapfile -t CONFIG_DIRS < <(
    find "$PREFIX" -maxdepth 5 -type f -name "microtelConfig.cmake" -printf '%h\n' 2>/dev/null | sort -u
)

if [[ ${#CONFIG_DIRS[@]} -eq 0 ]]; then
    echo "consumer-smoke: FAIL — no microtelConfig.cmake anywhere under $PREFIX" >&2
    echo "consumer-smoke: find_package(microtel) cannot resolve an install without one." >&2
    exit 1
fi

if [[ ${#CONFIG_DIRS[@]} -gt 1 ]]; then
    echo "consumer-smoke: warning — ${#CONFIG_DIRS[@]} package configs under $PREFIX:" >&2
    printf '    %s\n' "${CONFIG_DIRS[@]}" >&2
    echo "consumer-smoke: using the first." >&2
fi

CONFIG_DIR="${CONFIG_DIRS[0]}"
echo "consumer-smoke: microtel_DIR = $CONFIG_DIR"

# ---------------------------------------------------------------------------
# Configure, build and run the consumer
# ---------------------------------------------------------------------------

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

rm -rf "$CONSUMER_BUILD"

if ! cmake -S "$CONSUMER_SRC" -B "$CONSUMER_BUILD" "${GENERATOR_ARGS[@]}" \
    -Dmicrotel_DIR="$CONFIG_DIR"; then
    echo "consumer-smoke: FAIL — the consumer project did not configure" >&2
    echo "consumer-smoke: find_package(microtel) resolves the export set and its" >&2
    echo "consumer-smoke: find_dependency calls; one of them did not." >&2
    exit 1
fi

if ! cmake --build "$CONSUMER_BUILD"; then
    echo "consumer-smoke: FAIL — the consumer project did not build" >&2
    echo "consumer-smoke: a missing installed header or an unresolved symbol from a" >&2
    echo "consumer-smoke: component the export set does not carry both land here." >&2
    exit 1
fi

BINARY="${CONSUMER_BUILD}/${BINARY_NAME}"
if [[ ! -x "$BINARY" ]]; then
    BINARY="$(find "$CONSUMER_BUILD" -maxdepth 3 -type f -perm -u+x -name "$BINARY_NAME" | sort | head -1)"
fi

if [[ -z "$BINARY" || ! -x "$BINARY" ]]; then
    echo "consumer-smoke: FAIL — '$BINARY_NAME' not found under $CONSUMER_BUILD" >&2
    exit 1
fi

echo "consumer-smoke: running $BINARY"

set +e
"$BINARY"
rc=$?
set -e

if [[ $rc -ne 0 ]]; then
    echo "consumer-smoke: FAIL — the consumer binary exited $rc" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Dependency closure over the same prefix (CLAUDE.md rule 13, ICP 0020 D5)
# ---------------------------------------------------------------------------

set +e
"$SYMBOL_SCAN" --prefix "$PREFIX"
scan_rc=$?
set -e

if [[ $scan_rc -eq 2 ]]; then
    echo "consumer-smoke: symbol-scan could not run over $PREFIX" >&2
    exit 2
fi

if [[ $scan_rc -ne 0 ]]; then
    echo "consumer-smoke: FAIL — symbol-scan rejected the installed tree" >&2
    exit 1
fi

echo "consumer-smoke: clean — installed tree configures, links, runs"
