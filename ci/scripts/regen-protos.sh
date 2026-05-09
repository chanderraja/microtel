#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Regenerate the upb C accessors under gen/ from the vendored .proto
# files under proto/. Run this whenever proto/ is updated (i.e. when
# bumping the opentelemetry-proto pin in proto/README.md).
#
# Required tools — all pinned to the same protobuf v29.4 that produced
# the current gen/ files:
#
#   protoc                  v29.4  (download from protobuf GitHub releases)
#   protoc-gen-upb          v29.4  (build from protobuf source; see below)
#   protoc-gen-upb_minitable v29.4  (same)
#
# Quick-start to build the plugins:
#
#   git clone --depth 1 --branch v29.4 \
#     https://github.com/protocolbuffers/protobuf.git /tmp/pb
#   cd /tmp/pb && git submodule update --init third_party/abseil-cpp
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
#     -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_CONFORMANCE=OFF \
#     -Dprotobuf_BUILD_EXAMPLES=OFF -Dprotobuf_WITH_ZLIB=OFF \
#     -Dprotobuf_ABSL_PROVIDER=module
#   cmake --build build --target protoc-gen-upb protoc-gen-upb_minitable
#   # binaries at: build/protoc-gen-upb-29.4.0, build/protoc-gen-upb_minitable-29.4.0
#
# Usage:
#   ci/scripts/regen-protos.sh \
#     --protoc       /path/to/protoc-29.4 \
#     --gen-upb      /path/to/protoc-gen-upb-29.4.0 \
#     --gen-upb-mt   /path/to/protoc-gen-upb_minitable-29.4.0
#
# CI zero-diff check: ci.yml job `regen-check` runs this script and
# asserts `git diff --exit-code gen/` after regeneration.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

PROTOC="${PROTOC:-protoc}"
GEN_UPB="${GEN_UPB:-}"
GEN_UPB_MT="${GEN_UPB_MT:-}"

usage() {
    grep '^# ' "$0" | sed 's/^# //' >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --protoc)       PROTOC="$2";     shift 2 ;;
        --gen-upb)      GEN_UPB="$2";   shift 2 ;;
        --gen-upb-mt)   GEN_UPB_MT="$2"; shift 2 ;;
        *)              echo "Unknown argument: $1" >&2; usage ;;
    esac
done

if [[ -z "$GEN_UPB" || -z "$GEN_UPB_MT" ]]; then
    echo "regen-protos: --gen-upb and --gen-upb-mt are required." >&2
    usage
fi

echo "regen-protos: protoc      = $($PROTOC --version)"
echo "regen-protos: protoc-gen-upb          = $GEN_UPB"
echo "regen-protos: protoc-gen-upb_minitable = $GEN_UPB_MT"

"$PROTOC" \
    --plugin=protoc-gen-upb="$GEN_UPB" \
    --plugin=protoc-gen-upb_minitable="$GEN_UPB_MT" \
    --upb_out="${REPO_ROOT}/gen" \
    --upb_minitable_out="${REPO_ROOT}/gen" \
    --proto_path="${REPO_ROOT}/proto" \
    opentelemetry/proto/common/v1/common.proto \
    opentelemetry/proto/resource/v1/resource.proto \
    opentelemetry/proto/trace/v1/trace.proto \
    opentelemetry/proto/collector/trace/v1/trace_service.proto

echo "regen-protos: done — check 'git diff gen/' for changes"
