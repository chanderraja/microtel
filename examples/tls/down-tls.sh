#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Stop and remove the tls overlay. The shared stack is a different compose
# project and is not touched — tear that down with examples/stack/down.sh when
# you are done with it.
#
# The generated certificates under ./certs are left in place; they are
# gitignored and the next up-tls.sh reuses them. Remove them by hand, or
# regenerate with MICROTEL_CERT_FORCE=1.
#
# Usage:  examples/tls/down-tls.sh
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)

set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly EXAMPLE_DIR
readonly PROJECT_NAME="microtel-tls-example"
readonly COMPOSE_FILE="${EXAMPLE_DIR}/tls-compose.yaml"

# shellcheck source=../stack/compose-engine.sh
# shellcheck disable=SC1091
source "${EXAMPLE_DIR}/../stack/compose-engine.sh"

microtel_stack_resolve_engine

echo "tls: engine  = ${MICROTEL_ENGINE}"
echo "tls: compose = ${MICROTEL_COMPOSE[*]}"

"${MICROTEL_COMPOSE[@]}" -f "$COMPOSE_FILE" -p "$PROJECT_NAME" down

echo "tls: overlay down"
