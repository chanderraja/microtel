#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Runs the leaf's tests on a target other than the x86-64 host (issue #351),
# through tests/leaf/target/:
#
#   cortex-m0plus  bare metal, built -mcpu=cortex-m0plus, run on QEMU's BBC
#                  micro:bit model (-M microbit, a Cortex-M0: QEMU has no M0+,
#                  and both are ARMv6-M, the same instruction set with no
#                  unaligned access). The C runner, nanopb backend.
#   cortex-m4      bare metal, run on QEMU's MPS2 AN386 (-M mps2-an386). The C
#                  runner, nanopb and upb backends.
#   aarch64        Linux user mode under qemu-aarch64: the full gtest leaf suite
#                  (both backends and the byte-identity test) and the C runner.
#   i686           32-bit x86 Linux, run natively: the same as aarch64.
#
# The C runner (tests/leaf/target/leaf_target_test.c) checks the 13 golden
# vectors byte for byte and a subset of the leaf API tests, with every buffer
# also at odd byte offsets, and prints the stack each public entry point used
# (stack painting). Those lines go to stdout and, in CI, the job summary.
#
# Usage:  ci/scripts/leaf-target.sh <cortex-m0plus|cortex-m4|aarch64|i686> [work-dir]
#
# Needs CMake, Ninja and, per target: gcc-arm-none-eabi libnewlib-arm-none-eabi
# qemu-system-arm (Cortex-M); gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# qemu-user (aarch64); gcc-i686-linux-gnu g++-i686-linux-gnu (i686).
set -euo pipefail

TARGET="${1:?usage: leaf-target.sh <cortex-m0plus|cortex-m4|aarch64|i686> [work-dir]}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(mkdir -p "${2:-build-target-${TARGET}}" && cd "${2:-build-target-${TARGET}}" && pwd)"
readonly QEMU_TIMEOUT_SECONDS=300

CONFIG=()
RUNNERS=()
case "$TARGET" in
    cortex-m0plus)
        CONFIG=(-DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/cmake/toolchains/arm-none-eabi.cmake"
            -DMICROTEL_LEAF_CPU=cortex-m0plus -DMICROTEL_LEAF_TARGET_BOARD=microbit
            -DCMAKE_EXE_LINKER_FLAGS="--specs=nano.specs --specs=nosys.specs")
        MACHINE=microbit
        RUNNERS=(leaf_target_nanopb)
        ;;
    cortex-m4)
        CONFIG=(-DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/cmake/toolchains/arm-none-eabi.cmake"
            -DMICROTEL_LEAF_CPU=cortex-m4 -DMICROTEL_LEAF_TARGET_BOARD=mps2-an386
            -DCMAKE_EXE_LINKER_FLAGS="--specs=nano.specs --specs=nosys.specs")
        MACHINE=mps2-an386
        RUNNERS=(leaf_target_nanopb leaf_target_upb)
        ;;
    aarch64)
        CONFIG=(-DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/cmake/toolchains/aarch64-linux-gnu.cmake"
            -DMICROTEL_LEAF_TARGET_GTEST=ON
            "-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-aarch64;-L;/usr/aarch64-linux-gnu")
        ;;
    i686)
        # Static: the x86-64 host has no 32-bit loader, only the cross libc.
        CONFIG=(-DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/cmake/toolchains/i686-linux-gnu.cmake"
            -DMICROTEL_LEAF_TARGET_GTEST=ON -DCMAKE_EXE_LINKER_FLAGS=-static)
        ;;
    *)
        echo "leaf-target: unknown target '$TARGET'" >&2
        exit 2
        ;;
esac

BUILD_DIR="${WORK_DIR}/build"
echo "leaf-target: $TARGET"
# MinSizeRel (-Os): the build the footprint figures are for, so the measured
# stack is the stack of the code whose size is published.
cmake -S "${REPO_ROOT}/tests/leaf/target" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=MinSizeRel "${CONFIG[@]}" >/dev/null
cmake --build "$BUILD_DIR"

LOG="${WORK_DIR}/leaf-target.log"
: >"$LOG"
if [[ ${#RUNNERS[@]} -gt 0 ]]; then
    for runner in "${RUNNERS[@]}"; do
        echo "leaf-target: qemu-system-arm -M $MACHINE $runner"
        # Semihosting carries the output and the exit status; no UART, no
        # monitor. A fault prints the faulting PC and exits 1.
        status=0
        timeout "$QEMU_TIMEOUT_SECONDS" qemu-system-arm -M "$MACHINE" -nographic \
            -monitor none -serial none -semihosting-config enable=on,target=native \
            -kernel "${BUILD_DIR}/${runner}" 2>&1 | tee -a "$LOG" || status=$?
        if [[ $status -ne 0 ]] || ! tail -1 "$LOG" | grep -qx PASS; then
            echo "leaf-target: $runner failed on $MACHINE (exit $status)" >&2
            exit 1
        fi
    done
    if [[ "$TARGET" == cortex-m0plus ]]; then
        # The control: the same core must fault on an unaligned load, or the
        # runner's odd-offset buffers would prove nothing.
        control="$(timeout "$QEMU_TIMEOUT_SECONDS" qemu-system-arm -M "$MACHINE" -nographic \
            -monitor none -serial none -semihosting-config enable=on,target=native \
            -kernel "${BUILD_DIR}/leaf_target_alignment_control" 2>&1 || true)"
        echo "$control"
        if ! grep -q '^FAULT: hard fault' <<<"$control"; then
            echo "leaf-target: $MACHINE did not fault on an unaligned load" >&2
            exit 1
        fi
    fi
else
    ctest --test-dir "$BUILD_DIR" --output-on-failure
    for runner in leaf_target_upb leaf_target_nanopb; do
        # ctest already ran these; again here for their stack lines.
        if [[ "$TARGET" == aarch64 ]]; then
            qemu-aarch64 -L /usr/aarch64-linux-gnu "${BUILD_DIR}/${runner}" | tee -a "$LOG"
        else
            "${BUILD_DIR}/${runner}" | tee -a "$LOG"
        fi
    done
fi

SUMMARY="### leaf-target / ${TARGET}

Stack used per public entry point, measured by painting (bytes; the probe
scenario of \`examples/leaf/size_probe.c\`, and \`encode_to_nested\` for a span
with an event with attributes, the deepest nesting):

\`\`\`
$(grep -E '^(# microtel|stack )' "$LOG")
\`\`\`
"
echo "$SUMMARY"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    echo "$SUMMARY" >>"$GITHUB_STEP_SUMMARY"
fi
