#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Start the tls example's opt-in overlay: one collector serving OTLP over TLS
# on :5327 (gRPC), :5328 (HTTP) and :5337 (mTLS). Generates the certificates
# first if they are not there. Idempotent.
#
# The shared stack (examples/stack/up.sh) is left completely alone — this is a
# separate compose project on separate ports. Start that one too if you want
# the accepted traces to reach Grafana; the overlay forwards to its collector.
#
# Usage:  examples/tls/up-tls.sh
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)
#   MICROTEL_STACK_TIMEOUT      readiness timeout in seconds (default: 180)
#   MICROTEL_CERT_FORCE=1       regenerate certificates even if they exist
#
# Exit codes:
#   0  the overlay collector answered its health endpoint
#   1  it could not be started, or never came up

set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly EXAMPLE_DIR
readonly PROJECT_NAME="microtel-tls-example"
readonly COMPOSE_FILE="${EXAMPLE_DIR}/tls-compose.yaml"

READY_TIMEOUT_SECS="${MICROTEL_STACK_TIMEOUT:-180}"
readonly READY_TIMEOUT_SECS

readonly HEALTH_URL="http://localhost:15134"
readonly STACK_HEALTH_URL="http://localhost:13133"

# shellcheck source=../stack/compose-engine.sh
# shellcheck disable=SC1091
source "${EXAMPLE_DIR}/../stack/compose-engine.sh"

if ! command -v curl >/dev/null 2>&1; then
    echo "tls: curl is required to wait for the collector to come up" >&2
    exit 1
fi

"${EXAMPLE_DIR}/gen-certs.sh"

microtel_stack_resolve_engine

echo "tls: engine  = ${MICROTEL_ENGINE}"
echo "tls: compose = ${MICROTEL_COMPOSE[*]}"
echo

"${MICROTEL_COMPOSE[@]}" -f "$COMPOSE_FILE" -p "$PROJECT_NAME" up -d

echo
echo "tls: waiting for the overlay collector (timeout ${READY_TIMEOUT_SECS}s)"
deadline=$(( SECONDS + READY_TIMEOUT_SECS ))
while (( SECONDS < deadline )); do
    if curl -fs --max-time 2 -o /dev/null "$HEALTH_URL"; then
        ready=1
        break
    fi
    sleep 1
done

if [[ "${ready:-0}" -ne 1 ]]; then
    echo "tls: the overlay collector never became ready (${HEALTH_URL})" >&2
    echo "tls: logs — ${MICROTEL_COMPOSE[*]} -f ${COMPOSE_FILE} -p ${PROJECT_NAME} logs" >&2
    exit 1
fi
echo "tls: overlay collector is ready"

if ! curl -fs --max-time 2 -o /dev/null "$STACK_HEALTH_URL"; then
    echo
    echo "tls: NOTE — the shared stack is not running, so accepted traces stop at"
    echo "tls:        this collector's debug exporter instead of reaching Grafana."
    echo "tls:        Start it with examples/stack/up.sh if you want to see them."
fi

cat <<'BANNER'

  TLS overlay is up.

  OTLP/gRPC over TLS   https://localhost:5327
  OTLP/HTTP over TLS   https://localhost:5328   <- the one configuration in
                                                   which microtel can use
                                                   OTLP/HTTP at all
  OTLP/gRPC over mTLS  https://localhost:5337

  CA to trust:  examples/tls/certs/ca.crt

  Run the example (from the repository root, so the default cert path resolves):
    ./build/examples/microtel_example_tls

  Accepted batches are forwarded to the shared stack's collector on 4317 and
  show up in Grafana at http://localhost:3000.

  Tear the overlay down with:  examples/tls/down-tls.sh
  (The shared stack is untouched by both scripts.)

BANNER
