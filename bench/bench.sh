#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# bench.sh — entry point for the microtel benchmark harness.
# Thin wrapper: checks prerequisites then delegates to python3 -m driver.
# Run with -h to see all options.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

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
# Delegate to Python driver
# ---------------------------------------------------------------------------
cd "${SCRIPT_DIR}"
exec python3 -m driver "$@"
