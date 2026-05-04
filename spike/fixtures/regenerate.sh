#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M1 spike — regenerate single_span.bin from single_span.textproto
# against a pinned opentelemetry-proto release.
#
# Run from any directory; resolves paths relative to this script.
#
# Prerequisites:
#   - protoc on PATH (any 3.x or later release works for proto3 schemas)
#   - curl + tar to fetch the schema bundle
#
# THROWAWAY. Deleted at the end of M1.

set -euo pipefail

# Pinned upstream schema. Bump when the spike needs newer message
# fields and re-encode. The committed binary is the contract; this
# script is the regen recipe.
OTEL_PROTO_TAG="v1.10.0"

HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Fetching opentelemetry-proto $OTEL_PROTO_TAG into $TMP ..."
curl -sSL -o "$TMP/otel-proto.tar.gz" \
    "https://github.com/open-telemetry/opentelemetry-proto/archive/refs/tags/${OTEL_PROTO_TAG}.tar.gz"
tar -C "$TMP" -xzf "$TMP/otel-proto.tar.gz"
OTEL_PROTO_DIR="$TMP/opentelemetry-proto-${OTEL_PROTO_TAG#v}"

echo "Encoding $HERE/single_span.textproto -> $HERE/single_span.bin ..."
protoc \
    --proto_path="$OTEL_PROTO_DIR" \
    --encode=opentelemetry.proto.collector.trace.v1.ExportTraceServiceRequest \
    opentelemetry/proto/collector/trace/v1/trace_service.proto \
    < "$HERE/single_span.textproto" \
    > "$HERE/single_span.bin"

# Sanity-check by round-tripping through --decode. If decoding fails
# the encode is malformed and we abort instead of committing.
echo "Round-trip decode check ..."
protoc \
    --proto_path="$OTEL_PROTO_DIR" \
    --decode=opentelemetry.proto.collector.trace.v1.ExportTraceServiceRequest \
    opentelemetry/proto/collector/trace/v1/trace_service.proto \
    < "$HERE/single_span.bin" \
    > /dev/null

echo
echo "Wrote $HERE/single_span.bin ($(stat -c %s "$HERE/single_span.bin") bytes)."
echo "Pinned schema: opentelemetry-proto $OTEL_PROTO_TAG."
