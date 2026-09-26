#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Leaf footprint and closure (docs/leaf-concentrator-design.md §7.6, ICP 0031
# gate 4). Cross-compiles the standalone leaf for one target and backend, links
# examples/leaf/size_probe.c against it with --gc-sections, reports the leaf's
# own .text / .rodata / .data / .bss (and the whole image) and the worst-case
# stack of every public entry point (ci/scripts/leaf-stack.py, from the call
# graph GCC writes with -fcallgraph-info=su) to stdout and, in CI, to the job
# summary, then runs symbol-scan over the installed archives with the target's
# nm: no C++ runtime, every global prefixed, and for nanopb no heap allocator.
#
# Usage:  ci/scripts/leaf-footprint.sh <cortex-m0plus|cortex-m4|aarch64> [nanopb|upb] [work-dir]
#
#   cortex-m0plus, cortex-m4   arm-none-eabi-gcc with newlib-nano (apt
#                              gcc-arm-none-eabi libnewlib-arm-none-eabi);
#                              nanopb by default
#   aarch64                    aarch64-linux-gnu-gcc (apt gcc-aarch64-linux-gnu);
#                              upb by default
#
# Either backend builds for every target, so the two can be compared on the
# same one (issue #351). The upb probe gets a caller-owned encode scratch.
#
# The flash figures are targets in v1.2, not gates (< 15 KB nanopb, < 30 KB
# upb; v2.1 makes the nanopb one a gate): the script prints OVER or UNDER and
# fails only if the build, the link, the stack analysis or the closure scan
# fails.
set -euo pipefail

TARGET="${1:?usage: leaf-footprint.sh <cortex-m0plus|cortex-m4|aarch64> [nanopb|upb] [work-dir]}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

readonly NANOPB_TARGET_BYTES=$((15 * 1024))
readonly UPB_TARGET_BYTES=$((30 * 1024))

case "$TARGET" in
    cortex-m0plus | cortex-m4)
        ENCODER="${2:-nanopb}"
        PREFIX=arm-none-eabi
        TOOLCHAIN="${REPO_ROOT}/cmake/toolchains/arm-none-eabi.cmake"
        EXTRA_CONFIG=(-DMICROTEL_LEAF_CPU="$TARGET")
        ARCH_FLAGS=(-mcpu="$TARGET" -mthumb)
        # newlib-nano with syscall stubs: a firmware image with no OS.
        LINK_FLAGS=(--specs=nano.specs --specs=nosys.specs)
        ;;
    aarch64)
        ENCODER="${2:-upb}"
        PREFIX=aarch64-linux-gnu
        TOOLCHAIN="${REPO_ROOT}/cmake/toolchains/aarch64-linux-gnu.cmake"
        EXTRA_CONFIG=()
        ARCH_FLAGS=()
        LINK_FLAGS=()
        ;;
    *)
        echo "leaf-footprint: unknown target '$TARGET'" >&2
        exit 2
        ;;
esac

case "$ENCODER" in
    nanopb)
        LIBS=(-lmicrotel_leaf -lmicrotel_nanopb_gen -lmicrotel_nanopb)
        TARGET_BYTES=$NANOPB_TARGET_BYTES
        ;;
    upb)
        # The upb backend encodes in an arena; give it caller-owned scratch.
        ARCH_FLAGS+=(-DPROBE_WITH_SCRATCH)
        LIBS=(-lmicrotel_leaf -lmicrotel_upb_gen -lmicrotel_upb_runtime -lmicrotel_utf8_range)
        TARGET_BYTES=$UPB_TARGET_BYTES
        ;;
    *)
        echo "leaf-footprint: unknown backend '$ENCODER'" >&2
        exit 2
        ;;
esac

WORK_DIR="${3:-build-footprint-${TARGET}-${ENCODER}}"

CC="${PREFIX}-gcc"
for tool in "$CC" "${PREFIX}-size" "${PREFIX}-nm"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "leaf-footprint: $tool not found on PATH" >&2
        exit 2
    fi
done

BUILD_DIR="${WORK_DIR}/build"
INSTALL_DIR="${WORK_DIR}/install"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

echo "leaf-footprint: $TARGET, $ENCODER backend, $($CC --version | head -1)"

# -Os and section-per-function in the library too, or --gc-sections has
# nothing to collect. -fcallgraph-info=su writes each translation unit's frame
# sizes (the -fstack-usage figures) and call edges for leaf-stack.py; it does
# not change the code. Through CFLAGS, which CMake adds to the toolchain file's
# flags; -DCMAKE_C_FLAGS would replace them.
CFLAGS="-fcallgraph-info=su" cmake -S "${REPO_ROOT}/leaf" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DMICROTEL_LEAF_ENCODER="$ENCODER" \
    "${EXTRA_CONFIG[@]}" >/dev/null
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR" --prefix "$INSTALL_DIR" >/dev/null

LIB_DIR="$(dirname "$(find "$INSTALL_DIR" -name libmicrotel_leaf.a | head -1)")"
PROBE="${WORK_DIR}/size_probe.elf"
MAP="${WORK_DIR}/size_probe.map"

"$CC" "${ARCH_FLAGS[@]}" -std=c11 -pedantic-errors -Wall -Wextra -Werror -Os \
    -ffunction-sections -fdata-sections \
    -I "${INSTALL_DIR}/include" \
    "${REPO_ROOT}/examples/leaf/size_probe.c" \
    -L "$LIB_DIR" -Wl,--start-group "${LIBS[@]}" -Wl,--end-group \
    -Wl,--gc-sections -Wl,-Map="$MAP" "${LINK_FLAGS[@]}" \
    -o "$PROBE"

REPORT="$(python3 "${REPO_ROOT}/ci/scripts/leaf-footprint.py" "$MAP" \
    "$TARGET ($ENCODER)" "$TARGET_BYTES")"
STACK="$(python3 "${REPO_ROOT}/ci/scripts/leaf-stack.py" "$BUILD_DIR" "$ENCODER" \
    "$TARGET ($ENCODER)")"
IMAGE="$("${PREFIX}-size" "$PROBE")"
# The caller-owned RAM: leaf state, record buffer and (upb) scratch.
CALLER_RAM="$("${PREFIX}-nm" --print-size --size-sort "$PROBE" |
    while read -r _ size _ name; do
        case "$name" in
            g_leaf | g_records | g_scratch)
                printf -- '- `%s`: %d bytes\n' "$name" "$((16#$size))"
                ;;
        esac
    done)"

SUMMARY="${REPORT}
${STACK}

Caller-owned RAM in the probe (\`sizeof(microtel_leaf_t)\`, the record buffer
its one span needs, and the upb encode scratch):

${CALLER_RAM}

Whole probe image (\`${PREFIX}-size\`, including libc start-up and the probe's
static leaf state, record buffer and scratch):

\`\`\`
${IMAGE}
\`\`\`
"
echo "$SUMMARY"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    echo "$SUMMARY" >>"$GITHUB_STEP_SUMMARY"
fi

# Closure: the same gate the host leaf-standalone job runs, with the target nm.
NM="${PREFIX}-nm" "${REPO_ROOT}/ci/scripts/symbol-scan.sh" --prefix "$INSTALL_DIR"
