#!/usr/bin/env bash
# shellcheck shell=bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Container-engine detection, shared by up.sh and down.sh. Sourced, never run.
#
# Same policy as ci/scripts/conformance.sh and bench/driver/container.py:
# MICROTEL_CONTAINER_ENGINE wins, else podman if it is installed, else docker.
# podman first because this project's reference dev host is Fedora, where
# rootless podman is what is actually there.
#
# The engine and the compose front-end are two separate questions: podman can
# be driven by `podman-compose` or by `podman compose`, and docker by
# `docker compose` (v2) or the retired `docker-compose`. Both are resolved
# here so the callers never have to care.

# Sets, in the caller's scope:
#   MICROTEL_ENGINE   "podman" | "docker"
#   MICROTEL_COMPOSE  array — the compose command, e.g. (podman-compose)
# Returns non-zero with a diagnostic on stderr if either cannot be resolved.
#
# SC2034: both variables are consumed by the sourcing script, which shellcheck
# cannot see from here.
# shellcheck disable=SC2034
microtel_stack_resolve_engine()
{
    if [[ -n "${MICROTEL_CONTAINER_ENGINE:-}" ]]; then
        MICROTEL_ENGINE="$MICROTEL_CONTAINER_ENGINE"
        if ! command -v "$MICROTEL_ENGINE" >/dev/null 2>&1; then
            echo "stack: MICROTEL_CONTAINER_ENGINE=$MICROTEL_ENGINE is not on PATH" >&2
            return 1
        fi
    elif command -v podman >/dev/null 2>&1; then
        MICROTEL_ENGINE="podman"
    elif command -v docker >/dev/null 2>&1; then
        MICROTEL_ENGINE="docker"
    else
        echo "stack: no container engine found (looked for podman, then docker)" >&2
        echo "stack: install one, or set MICROTEL_CONTAINER_ENGINE" >&2
        return 1
    fi

    case "$MICROTEL_ENGINE" in
        podman)
            # podman-compose directly when it is installed: `podman compose`
            # only shells out to it anyway, and printing its "executing
            # external compose provider" banner on every call is noise.
            if command -v podman-compose >/dev/null 2>&1; then
                MICROTEL_COMPOSE=(podman-compose)
            elif podman compose version >/dev/null 2>&1; then
                MICROTEL_COMPOSE=(podman compose)
            else
                echo "stack: podman is installed but no compose provider is" >&2
                echo "stack: install podman-compose, or 'dnf install podman-compose'" >&2
                return 1
            fi
            ;;
        docker)
            # v2 (the plugin subcommand) first; the hyphenated v1 binary is
            # end-of-life but still the only thing on some hosts.
            if docker compose version >/dev/null 2>&1; then
                MICROTEL_COMPOSE=(docker compose)
            elif command -v docker-compose >/dev/null 2>&1; then
                MICROTEL_COMPOSE=(docker-compose)
            else
                echo "stack: docker is installed but has no compose front-end" >&2
                echo "stack: install the Compose v2 plugin" >&2
                return 1
            fi
            ;;
        *)
            echo "stack: unknown engine '$MICROTEL_ENGINE' (want podman or docker)" >&2
            return 1
            ;;
    esac

    return 0
}
