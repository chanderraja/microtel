#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# CI smoke test for the demo: bring it up, wait for a trace in Tempo that
# spans both services with correct parent links, tear it down.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

status=0
docker compose up -d --build
python3 verify_trace.py http://localhost:3200 180 || status=$?
if [[ $status -ne 0 ]]; then
    docker compose logs --no-color device gateway otel-collector | tail -n 200
fi
docker compose down -v
exit "$status"
