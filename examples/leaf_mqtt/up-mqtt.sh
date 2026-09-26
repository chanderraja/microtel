#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Start the leaf_mqtt example's opt-in overlay: one Mosquitto broker on
# 127.0.0.1:1883, plaintext and anonymous. Idempotent.
#
# The shared stack (examples/stack/up.sh) is left completely alone: this is a
# separate compose project. The concentrator exports to that stack's
# collector itself, so start it too if you want the traces in Grafana.
#
# Usage:  examples/leaf_mqtt/up-mqtt.sh
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)
#   MICROTEL_STACK_TIMEOUT      readiness timeout in seconds (default: 180)
#
# Exit codes:
#   0  the broker answered an MQTT CONNECT
#   1  it could not be started, or never came up

set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly EXAMPLE_DIR
readonly PROJECT_NAME="microtel-mqtt-example"
readonly COMPOSE_FILE="${EXAMPLE_DIR}/mqtt-compose.yaml"

READY_TIMEOUT_SECS="${MICROTEL_STACK_TIMEOUT:-180}"
readonly READY_TIMEOUT_SECS

readonly BROKER_PORT=1883
readonly STACK_HEALTH_URL="http://localhost:13133"

# shellcheck source=../stack/compose-engine.sh
# shellcheck disable=SC1091
source "${EXAMPLE_DIR}/../stack/compose-engine.sh"

# An open port is not a ready broker: a rootless port forwarder accepts
# connections before the container listens. So send a real MQTT 3.1.1 CONNECT
# (clean session, empty client id) and expect CONNACK "accepted": 20 02 00 00.
broker_ready()
{
    local reply
    reply="$(timeout 2 bash -c "
        exec 3<>/dev/tcp/127.0.0.1/${BROKER_PORT}
        printf '\x10\x0c\x00\x04MQTT\x04\x02\x00\x3c\x00\x00' >&3
        head -c 4 <&3 | od -An -tx1" 2>/dev/null || true)"
    [[ "$(echo "$reply" | tr -d ' \n')" == "20020000" ]]
}

microtel_stack_resolve_engine

echo "mqtt: engine  = ${MICROTEL_ENGINE}"
echo "mqtt: compose = ${MICROTEL_COMPOSE[*]}"
echo

"${MICROTEL_COMPOSE[@]}" -f "$COMPOSE_FILE" -p "$PROJECT_NAME" up -d

echo
echo "mqtt: waiting for the broker (timeout ${READY_TIMEOUT_SECS}s)"
deadline=$(( SECONDS + READY_TIMEOUT_SECS ))
while (( SECONDS < deadline )); do
    if broker_ready; then
        ready=1
        break
    fi
    sleep 1
done

if [[ "${ready:-0}" -ne 1 ]]; then
    echo "mqtt: the broker never accepted an MQTT connection on :${BROKER_PORT}" >&2
    echo "mqtt: logs — ${MICROTEL_COMPOSE[*]} -f ${COMPOSE_FILE} -p ${PROJECT_NAME} logs" >&2
    exit 1
fi
echo "mqtt: broker is ready"

if ! command -v curl >/dev/null 2>&1 ||
   ! curl -fs --max-time 2 -o /dev/null "$STACK_HEALTH_URL"; then
    echo
    echo "mqtt: NOTE — the shared stack does not answer on ${STACK_HEALTH_URL}, so the"
    echo "mqtt:        concentrator will have no collector to export to."
    echo "mqtt:        Start it with examples/stack/up.sh."
fi

cat <<'BANNER'

  MQTT overlay is up.

  Broker       mqtt://127.0.0.1:1883   (plaintext, anonymous, loopback only)
  Leaves       publish to  microtel/<device-id>/traces
  Concentrator subscribes  microtel/+/traces

  Run the example:
    ./build/examples/microtel_example_leaf_mqtt_concentrator &
    ./build/examples/microtel_example_leaf_mqtt_leaf

  Tear the overlay down with:  examples/leaf_mqtt/down-mqtt.sh
  (The shared stack is untouched by both scripts.)

BANNER
