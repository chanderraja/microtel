#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Builds and runs a consumer of this checkout the way the README's Install
# section tells users to get microtel. consumer-smoke.sh covers
# `cmake --install` + find_package; this covers the other three routes:
#
#   fetchcontent  tests/consumer_fetchcontent, with
#                 FETCHCONTENT_SOURCE_DIR_MICROTEL pointing at this checkout
#   cpm           tests/consumer_cpm, with CPM_microtel_SOURCE pointing at it
#   vcpkg         the overlay port in packaging/vcpkg/ports, building this
#                 checkout (MICROTEL_SOURCE_DIR), then tests/consumer through
#                 vcpkg's toolchain. Needs VCPKG_ROOT.
#
# All three build the same tests/consumer/main.cpp, which prints PASS.
#
# Usage:  ci/scripts/consumer-package.sh <fetchcontent|cpm|vcpkg> [cmake args...]
#
# Extra arguments go to the consumer's configure step (for fetchcontent and
# cpm, they reach microtel too, e.g. -DMICROTEL_WARNINGS_AS_ERRORS=OFF).
# WORK_DIR in the environment keeps the scratch directory; by default a
# temporary one is used and removed on exit.
#
# Exit codes:
#   0  the consumer built and printed PASS
#   1  configure, build, install or run failed
#   2  bad usage, or a tool is missing

set -euo pipefail

METHOD="${1:-}"
shift || true

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly BINARY_NAME="microtel_consumer_smoke"

if [[ -n "${WORK_DIR:-}" ]]; then
    mkdir -p "$WORK_DIR"
else
    WORK_DIR="$(mktemp -d -t microtel-consumer-package-XXXXXX)"
    trap 'rm -rf "$WORK_DIR"' EXIT
fi

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

case "$METHOD" in
    fetchcontent)
        SRC="${REPO_ROOT}/tests/consumer_fetchcontent"
        CONFIGURE_ARGS=("-DFETCHCONTENT_SOURCE_DIR_MICROTEL=${REPO_ROOT}")
        ;;
    cpm)
        SRC="${REPO_ROOT}/tests/consumer_cpm"
        CONFIGURE_ARGS=("-DCPM_microtel_SOURCE=${REPO_ROOT}")
        ;;
    vcpkg)
        if [[ -z "${VCPKG_ROOT:-}" || ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
            echo "consumer-package: set VCPKG_ROOT to a bootstrapped vcpkg" >&2
            exit 2
        fi
        SRC="${REPO_ROOT}/tests/consumer"
        triplet="${VCPKG_TRIPLET:-x64-linux}"
        vcpkg=("${VCPKG_ROOT}/vcpkg" "--triplet=${triplet}"
               "--overlay-ports=${REPO_ROOT}/packaging/vcpkg/ports"
               "--x-install-root=${WORK_DIR}/vcpkg_installed")
        # Dependencies first, through whatever binary cache is configured, so a
        # CI cache can serve them. microtel itself must not come from a cache:
        # vcpkg keys it on the port files, not on the checkout it builds.
        deps=(nghttp2 openssl zlib tomlplusplus spdlog)
        "${vcpkg[@]}" install "${deps[@]}" || exit 1
        MICROTEL_SOURCE_DIR="$REPO_ROOT" "${vcpkg[@]}" install microtel \
            --binarysource=clear || exit 1
        CONFIGURE_ARGS=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
                        "-DVCPKG_INSTALLED_DIR=${WORK_DIR}/vcpkg_installed"
                        "-DVCPKG_TARGET_TRIPLET=${triplet}"
                        "-DVCPKG_MANIFEST_MODE=OFF")
        ;;
    *)
        echo "usage: $0 <fetchcontent|cpm|vcpkg> [cmake args...]" >&2
        exit 2
        ;;
esac

BUILD="${WORK_DIR}/build-${METHOD}"
rm -rf "$BUILD"

echo "consumer-package: ${METHOD}: configuring ${SRC}"
cmake -S "$SRC" -B "$BUILD" "${GENERATOR_ARGS[@]}" "${CONFIGURE_ARGS[@]}" "$@" || exit 1
cmake --build "$BUILD" --target "$BINARY_NAME" || exit 1

if ! "${BUILD}/${BINARY_NAME}"; then
    echo "consumer-package: FAIL — ${METHOD} consumer exited non-zero" >&2
    exit 1
fi
echo "consumer-package: ${METHOD}: clean"
