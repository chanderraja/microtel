#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Coverage collection script. Configures + builds + runs tests with
# --coverage on, then produces an lcov report.
#
# Per spec §14.2, the v1.0 thresholds are:
#   ≥ 90% line / ≥ 85% branch on SDK + encoder code (src/sdk/, src/wire/encoder/)
#   ≥ 80% on transport + exporter paths (src/transport/, src/exporter/)
# Plus per-PR diff-coverage thresholds at the same numbers.
#
# In M2 (this milestone) there is no src/ code yet — the script just
# produces the report and exits success regardless. M3+ adds threshold
# enforcement. The diff-coverage gate runs in a separate CI step using
# diff-cover against the lcov report.

set -euo pipefail

BUILD_DIR="${1:-build/coverage}"

echo "coverage: build dir = $BUILD_DIR"

cmake -S . -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_STANDARD=20 \
    -DMICROTEL_BUILD_HEADER_CHECK=ON \
    -DMICROTEL_BUILD_TESTS=ON \
    -DMICROTEL_COVERAGE=ON

cmake --build "$BUILD_DIR" -j

ctest --test-dir "$BUILD_DIR" --output-on-failure

# Capture coverage. lcov reads .gcda files produced by ctest run.
LCOV_FILE="$BUILD_DIR/coverage.info"
LCOV_FILTERED="$BUILD_DIR/coverage.filtered.info"

lcov \
    --capture \
    --directory "$BUILD_DIR" \
    --output-file "$LCOV_FILE" \
    --rc lcov_branch_coverage=1 \
    --ignore-errors mismatch,inconsistent

# Filter out test code, third_party, gen, and system headers — the
# thresholds in spec §14.2 are about microtel's own SDK + encoder +
# transport + exporter code, not about gtest or vendored deps.
lcov \
    --remove "$LCOV_FILE" \
        '/usr/*' \
        '*/third_party/*' \
        '*/gen/*' \
        '*/_deps/*' \
        '*/tests/*' \
        '*/ci/*' \
    --output-file "$LCOV_FILTERED" \
    --rc lcov_branch_coverage=1 \
    --ignore-errors unused

echo
echo "=== Filtered coverage summary (microtel-owned code only) ==="
lcov --summary "$LCOV_FILTERED" --rc lcov_branch_coverage=1 || true
echo
echo "Full report: $LCOV_FILE"
echo "Filtered:    $LCOV_FILTERED"
echo
echo "M2 note: no src/ implementation exists yet; numbers reflect only"
echo "the inline header definitions exercised by the smoke tests. M3+"
echo "adds the threshold enforcement against this same filtered report."
