#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Stop and remove the examples observability stack. Storage is ephemeral (see
# the comment at the top of compose.yaml), so this takes the collected traces
# with it — which is the point: the next up.sh starts clean.
#
# Usage:  examples/stack/down.sh
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)

set -euo pipefail

STACK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly STACK_DIR
readonly PROJECT_NAME="microtel-stack"
readonly COMPOSE_FILE="${STACK_DIR}/compose.yaml"

# shellcheck source=./compose-engine.sh
# shellcheck disable=SC1091
source "${STACK_DIR}/compose-engine.sh"

microtel_stack_resolve_engine

echo "stack: engine  = ${MICROTEL_ENGINE}"
echo "stack: compose = ${MICROTEL_COMPOSE[*]}"

"${MICROTEL_COMPOSE[@]}" -f "$COMPOSE_FILE" -p "$PROJECT_NAME" down

echo "stack: down"
