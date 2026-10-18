#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Builds the release source archive for one tag, and its SHA-512 file.
# Run by .github/workflows/release.yml; RELEASING.md §4 describes the release.
#
#   dist/microtel-X.Y.Z.tar.gz          git archive of the tag, under a
#                                       microtel-X.Y.Z/ top-level directory
#   dist/microtel-X.Y.Z.tar.gz.sha512   `sha512sum` output for it
#
# microtel has no submodules: upb, utf8_range, nanopb, tl::expected and the
# generated protos are committed, so `git archive` alone is complete. Paths
# marked export-ignore in .gitattributes (ci/, .github/, dotfiles) are left
# out, as they are from GitHub's own tag archives.
#
# The tag must name the project version: v<PROJECT_VERSION>.
#
# Usage:  ci/scripts/source-archive.sh <tag> [out-dir]
#
#   tag      an existing tag, e.g. v1.2.2
#   out-dir  where to write the two files (default: dist)
#
# Exit codes:
#   0  both files written
#   1  the tag does not match the project version at that tag
#   2  bad usage, or the tag does not exist

set -euo pipefail

TAG="${1:-}"
OUT_DIR="${2:-dist}"

if [[ -z "$TAG" ]]; then
    echo "usage: $0 <tag> [out-dir]" >&2
    exit 2
fi

if ! git rev-parse -q --verify "refs/tags/${TAG}^{commit}" >/dev/null; then
    echo "source-archive: no tag '${TAG}' in this repository" >&2
    exit 2
fi

# project(microtel VERSION …) at the tag, read the way version-drift-check.sh
# reads it.
version="$(git show "${TAG}:CMakeLists.txt" | awk '
    /^[[:space:]]*project[[:space:]]*\(/ { in_project = 1 }
    in_project && $1 == "VERSION"        { print $2; exit }')"

if [[ "$TAG" != "v${version}" ]]; then
    echo "source-archive: tag '${TAG}' does not match project VERSION '${version}'" >&2
    exit 1
fi

name="microtel-${version}"
mkdir -p "$OUT_DIR"

git archive --format=tar.gz --prefix="${name}/" -o "${OUT_DIR}/${name}.tar.gz" "refs/tags/${TAG}"
(cd "$OUT_DIR" && sha512sum "${name}.tar.gz" >"${name}.tar.gz.sha512")

echo "source-archive: ${OUT_DIR}/${name}.tar.gz"
cat "${OUT_DIR}/${name}.tar.gz.sha512"
