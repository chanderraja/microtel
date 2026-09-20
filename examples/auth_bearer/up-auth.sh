#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Start the auth_bearer example's opt-in overlay: one collector whose OTLP/gRPC
# receiver on :5317 requires a bearer token. Idempotent.
#
# The shared stack (examples/stack/up.sh) is left completely alone — this is a
# separate compose project on separate ports. Start that one too if you want
# the accepted traces to reach Grafana; the overlay forwards to its collector.
#
# Usage:  examples/auth_bearer/up-auth.sh
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)
#   MICROTEL_STACK_TIMEOUT      readiness timeout in seconds (default: 180)
#
# Exit codes:
#   0  the overlay collector answered its health endpoint
#   1  it could not be started, or never came up

set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly EXAMPLE_DIR
readonly PROJECT_NAME="microtel-auth-example"
readonly COMPOSE_FILE="${EXAMPLE_DIR}/auth-compose.yaml"

READY_TIMEOUT_SECS="${MICROTEL_STACK_TIMEOUT:-180}"
readonly READY_TIMEOUT_SECS

readonly HEALTH_URL="http://localhost:15133"
readonly STACK_HEALTH_URL="http://localhost:13133"

# shellcheck source=../stack/compose-engine.sh
# shellcheck disable=SC1091
source "${EXAMPLE_DIR}/../stack/compose-engine.sh"

if ! command -v curl >/dev/null 2>&1; then
    echo "auth: curl is required to wait for the collector to come up" >&2
    exit 1
fi

microtel_stack_resolve_engine

echo "auth: engine  = ${MICROTEL_ENGINE}"
echo "auth: compose = ${MICROTEL_COMPOSE[*]}"
echo

"${MICROTEL_COMPOSE[@]}" -f "$COMPOSE_FILE" -p "$PROJECT_NAME" up -d

echo
echo "auth: waiting for the overlay collector (timeout ${READY_TIMEOUT_SECS}s)"
deadline=$(( SECONDS + READY_TIMEOUT_SECS ))
while (( SECONDS < deadline )); do
    if curl -fs --max-time 2 -o /dev/null "$HEALTH_URL"; then
        ready=1
        break
    fi
    sleep 1
done

if [[ "${ready:-0}" -ne 1 ]]; then
    echo "auth: the overlay collector never became ready (${HEALTH_URL})" >&2
    echo "auth: logs — ${MICROTEL_COMPOSE[*]} -f ${COMPOSE_FILE} -p ${PROJECT_NAME} logs" >&2
    exit 1
fi
echo "auth: overlay collector is ready"

if ! curl -fs --max-time 2 -o /dev/null "$STACK_HEALTH_URL"; then
    echo
    echo "auth: NOTE — the shared stack is not running, so accepted traces stop at"
    echo "auth:        this collector's debug exporter instead of reaching Grafana."
    echo "auth:        Start it with examples/stack/up.sh if you want to see them."
fi

cat <<'BANNER'

  Auth overlay is up.

  Bearer-guarded OTLP/gRPC:  http://localhost:5317
  Token:                     microtel-example-token
                             (configured in auth-collector-config.yaml)

  Run the example:
    ./build/examples/microtel_example_auth_bearer

  Accepted batches are forwarded to the shared stack's collector on 4317 and
  show up in Grafana at http://localhost:3000.

  Tear the overlay down with:  examples/auth_bearer/down-auth.sh
  (The shared stack is untouched by both scripts.)

BANNER
