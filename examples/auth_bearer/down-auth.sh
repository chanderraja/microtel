#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Stop and remove the auth_bearer overlay. The shared stack is a different
# compose project and is not touched — tear that down with
# examples/stack/down.sh when you are done with it.
#
# Usage:  examples/auth_bearer/down-auth.sh
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)

set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly EXAMPLE_DIR
readonly PROJECT_NAME="microtel-auth-example"
readonly COMPOSE_FILE="${EXAMPLE_DIR}/auth-compose.yaml"

# shellcheck source=../stack/compose-engine.sh
# shellcheck disable=SC1091
source "${EXAMPLE_DIR}/../stack/compose-engine.sh"

microtel_stack_resolve_engine

echo "auth: engine  = ${MICROTEL_ENGINE}"
echo "auth: compose = ${MICROTEL_COMPOSE[*]}"

"${MICROTEL_COMPOSE[@]}" -f "$COMPOSE_FILE" -p "$PROJECT_NAME" down

echo "auth: overlay down"
