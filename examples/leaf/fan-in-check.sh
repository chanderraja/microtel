#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Show how the concentrator's export requests reached the collector: one line
# per request, with the device.id and service.name of every ResourceSpans in
# it. Passes if at least one request carried more than one device.
#
# It reads the shared stack's collector log. Only requests sent to the
# collector's unbatched receiver on :4319 are logged in full (the
# debug/requests exporter in examples/stack/collector-config.yaml), so run the
# concentrator with that endpoint first:
#
#   ./build/examples/microtel_example_leaf_concentrator http://localhost:4319
#
# Requests sent to :4317 go through the collector's batch processor, which
# merges them, so no log there can say how many requests microtel sent.
#
# Usage:  examples/leaf/fan-in-check.sh        # reads the stack's collector log
#         examples/leaf/fan-in-check.sh -      # reads a collector log on stdin
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)
#
# Exit codes:
#   0  a request carried ResourceSpans from two or more distinct device.ids
#   1  no request did, or none was logged at all
#   2  the collector log could not be read

set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly EXAMPLE_DIR
readonly STACK_DIR="${EXAMPLE_DIR}/../stack"
readonly PROJECT_NAME="microtel-stack"

collector_log()
{
    if [[ "${1:-}" == "-" ]]; then
        cat
        return
    fi
    # shellcheck source=../stack/compose-engine.sh
    # shellcheck disable=SC1091
    source "${STACK_DIR}/compose-engine.sh"
    microtel_stack_resolve_engine
    "${MICROTEL_COMPOSE[@]}" -f "${STACK_DIR}/compose.yaml" -p "$PROJECT_NAME" \
        logs otel-collector 2>&1
}

if ! log="$(collector_log "${1:-}")"; then
    echo "fan-in: cannot read the collector log; is examples/stack/up.sh running?" >&2
    exit 2
fi

# The debug exporter at detailed verbosity writes one header line per request
# it exports, tagged with its component id, then a block per ResourceSpans.
# Lines may carry a compose prefix ("otel-collector-1  | "), so nothing below
# is anchored to the start of a line.
printf '%s\n' "$log" | awk '
    function end_resource()
    {
        if (in_rs) {
            printf "  device.id=%s  service.name=%s\n", dev, svc
            if (dev != "" && !(dev in seen)) { seen[dev] = 1; devices++ }
        }
        in_rs = 0; in_attrs = 0; dev = ""; svc = ""
    }
    function end_request()
    {
        end_resource()
        if (in_req) {
            if (devices > 1) { multi++ }
            if (devices > most) { most = devices }
        }
        in_req = 0; devices = 0; delete seen
    }
    /"otelcol.component.id": "/ {
        end_request()
        if ($0 ~ /"otelcol.component.id": "debug\/requests"/ && match($0, /"resource spans": [0-9]+, "spans": [0-9]+/)) {
            split(substr($0, RSTART, RLENGTH), n, /[^0-9]+/)
            requests++
            in_req = 1
            printf "request %d: %d ResourceSpans, %d spans\n", requests, n[2], n[3]
        }
        next
    }
    in_req && /ResourceSpans #[0-9]+/ { end_resource(); in_rs = 1; next }
    in_rs && /Resource attributes:/ { in_attrs = 1; next }
    in_rs && /ScopeSpans #[0-9]+/ { in_attrs = 0; next }
    in_attrs && match($0, /-> device\.id: Str\([^)]*\)/) {
        dev = substr($0, RSTART + 18, RLENGTH - 19); next
    }
    in_attrs && match($0, /-> service\.name: Str\([^)]*\)/) {
        svc = substr($0, RSTART + 21, RLENGTH - 22); next
    }
    END {
        end_request()
        if (requests == 0) {
            print "fan-in: no request logged by debug/requests. Send one to"
            print "fan-in: http://localhost:4319 (the concentrator'"'"'s first argument)."
            exit 1
        }
        printf "fan-in: %d of %d requests carried more than one device (most: %d)\n", \
            multi, requests, most
        exit (multi > 0 ? 0 : 1)
    }'
