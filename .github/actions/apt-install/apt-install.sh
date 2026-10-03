#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# The two halves of .github/actions/apt-install:
#
#   apt-install.sh offline <cache-dir> <package...>
#       Installs from a restored cache with no network at all: the package
#       lists and .deb files saved by `online` are copied into apt's own
#       directories and `apt-get install --no-download` does the rest. Exits
#       non-zero if anything is missing, so the caller can fall back.
#
#   apt-install.sh online <cache-dir> <package...>
#       The networked install (apt-get update + install, with retries and
#       timeouts), then saves the package lists and every .deb it downloaded
#       into <cache-dir> for the next run.
#
# Why both halves: CI's installs stalled in `apt-get update` (fetching
# InRelease from the Ubuntu mirror), so caching only the .deb files would
# still hit the mirror. With the lists cached too, a cache hit never touches
# the network.
#
# SUDO defaults to `sudo`; tests run as root with SUDO="".

set -euo pipefail

readonly SUDO="${SUDO-sudo}"
readonly APT_NET_OPTS=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30
                       -o Acquire::https::Timeout=30)
# Keep what is downloaded (docker images delete it by default) and store the
# lists gzipped, which makes the cache several times smaller.
readonly APT_KEEP_OPTS=(-o APT::Keep-Downloaded-Packages=true
                        -o Binary::apt-get::APT::Keep-Downloaded-Packages=true
                        -o Acquire::GzipIndexes=true)

usage()
{
    echo "usage: $0 offline|online <cache-dir> <package...>" >&2
    exit 2
}

[[ $# -ge 3 ]] || usage
readonly MODE="$1"
readonly CACHE="$2"
shift 2

offline()
{
    # The lists are required. The archives can be empty: when every package
    # was already on the runner image, nothing was downloaded.
    if [[ ! -d "$CACHE/lists" ]]; then
        echo "apt-install: no package lists in $CACHE" >&2
        return 1
    fi
    $SUDO cp -a "$CACHE/lists/." /var/lib/apt/lists/
    if compgen -G "$CACHE/archives/*.deb" >/dev/null; then
        $SUDO cp "$CACHE"/archives/*.deb /var/cache/apt/archives/
    fi
    $SUDO apt-get "${APT_KEEP_OPTS[@]}" install -y --no-download "$@"
}

online()
{
    $SUDO apt-get "${APT_NET_OPTS[@]}" "${APT_KEEP_OPTS[@]}" update
    $SUDO apt-get "${APT_NET_OPTS[@]}" "${APT_KEEP_OPTS[@]}" install -y "$@"

    mkdir -p "$CACHE"
    find "$CACHE" -mindepth 1 -delete
    mkdir -p "$CACHE/lists" "$CACHE/archives"
    $SUDO cp -a /var/lib/apt/lists/. "$CACHE/lists/"
    $SUDO rm -rf "$CACHE/lists/partial" "$CACHE/lists/lock"
    if compgen -G "/var/cache/apt/archives/*.deb" >/dev/null; then
        $SUDO cp /var/cache/apt/archives/*.deb "$CACHE/archives/"
    fi
    $SUDO chown -R "$(id -u):$(id -g)" "$CACHE"
    echo "apt-install: saved $(find "$CACHE/archives" -name '*.deb' | wc -l) packages," \
         "$(du -sh "$CACHE" | cut -f1) in total"
}

case "$MODE" in
    offline) offline "$@" ;;
    online) online "$@" ;;
    *) usage ;;
esac
