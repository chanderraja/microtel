#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# bench.sh — entry point for the microtel benchmark harness.
#
# Usage:
#   ./bench/bench.sh [options]
#
# Options:
#   --profile <name>      Workload profile to run (default: hot-loop-traces)
#   --reps <n>            Number of repetitions (default: 5)
#   --sink <mode>         Sink mode: blackhole (default) or collector
#   --output <dir>        Output directory (default: bench/results)
#   --ci                  CI mode: non-zero exit on regression vs baseline
#   --baseline <file>     Baseline JSON for --ci regression check
#   --no-build            Skip Docker image build (use cached images)
#   --with-plots          Generate HTML plots alongside results.md
#   --flamegraph <sut>    Run perf + flamegraph for named SUT (requires perf)
#   --allow-smt           Skip SMT/hyperthreading check
#   -h, --help            Show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
PROFILE="hot-loop-traces"
REPS=5
SINK="blackhole"
OUTPUT="${SCRIPT_DIR}/results"
CI_MODE=false
BASELINE=""
NO_BUILD=false
WITH_PLOTS=false
FLAMEGRAPH_SUT=""
ALLOW_SMT=false

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile)    PROFILE="$2";       shift 2 ;;
        --reps)       REPS="$2";          shift 2 ;;
        --sink)       SINK="$2";          shift 2 ;;
        --output)     OUTPUT="$2";        shift 2 ;;
        --ci)         CI_MODE=true;       shift   ;;
        --baseline)   BASELINE="$2";      shift 2 ;;
        --no-build)   NO_BUILD=true;      shift   ;;
        --with-plots) WITH_PLOTS=true;    shift   ;;
        --flamegraph) FLAMEGRAPH_SUT="$2"; shift 2 ;;
        --allow-smt)  ALLOW_SMT=true;     shift   ;;
        -h|--help)
            sed -n '/^# Usage:/,/^[^#]/p' "$0" | head -n -1 | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "bench.sh: unknown option: $1" >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------
if ! command -v docker >/dev/null 2>&1 && ! command -v podman >/dev/null 2>&1; then
    echo "bench.sh: Docker or Podman is required" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "bench.sh: Python 3 is required" >&2
    exit 1
fi

PYTHON_VERSION=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
if [[ "$(echo -e "3.11\n${PYTHON_VERSION}" | sort -V | head -1)" != "3.11" ]]; then
    echo "bench.sh: Python 3.11+ required (found ${PYTHON_VERSION})" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------
echo "bench.sh: microtel benchmark harness — B0 scaffold"
echo "bench.sh: driver implementation pending (B0 milestone)"
echo ""
echo "  Profile:  ${PROFILE}"
echo "  Reps:     ${REPS}"
echo "  Sink:     ${SINK}"
echo "  Output:   ${OUTPUT}"
echo ""
echo "bench.sh: nothing to run yet — add bench/driver/__main__.py in B0"
exit 0
