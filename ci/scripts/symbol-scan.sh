#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Dependency-closure gate. Asserts that no shipped microtel artifact defines or
# references a symbol from gRPC, abseil, or the protobuf C++ runtime.
#
# This is the mechanical backing for CLAUDE.md rule 13 ("No gRPC library, no
# abseil, no protobuf-cpp runtime. Ever.") and for spec §3's dependency-closure
# claim. The claim is the project's whole reason to exist, so it is tested
# rather than asserted.
#
# Undefined (`U`) references matter as much as defined symbols: a static archive
# carrying `U absl::...` makes abseil a link requirement for every consumer, even
# though the archive itself contains none of abseil's code.
#
# Usage:  ci/scripts/symbol-scan.sh [build-dir]     (default: build)

set -euo pipefail

BUILD_DIR="${1:-build}"
NM="${NM:-nm}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "symbol-scan: build directory '$BUILD_DIR' not found" >&2
    exit 2
fi

if ! command -v "$NM" >/dev/null 2>&1; then
    echo "symbol-scan: $NM not found on PATH" >&2
    exit 2
fi

# Forbidden symbol namespaces, matched against the *demangled* name anchored at
# its start. Anchoring is what keeps the vendored-upb accessors legal: upb emits
# C symbols like `google_protobuf_FileDescriptorProto_set_name`, which are upb's
# own generated code and must NOT trip `google::protobuf::` (the C++ runtime).
# Likewise `upb_*` and `utf8_range_*` are vendored dependencies the closure
# explicitly permits.
FORBIDDEN_PATTERN='^(absl::|absl_|grpc::|grpc_|GRPC_|google::protobuf::)'

# Shipped artifacts. `libmicrotel_*.a` covers every component archive including
# the vendored upb runtime and utf8_range; the preflight binary is the shipped
# CLI from spec §6.4. `microtel_header_check` is the M0 compile check and is
# deliberately excluded — it is never shipped.
mapfile -t ARTIFACTS < <(
    {
        find "$BUILD_DIR" -type f -name "libmicrotel_*.a"
        find "$BUILD_DIR" -type f -perm -u+x -name "microtel-preflight"
    } 2>/dev/null | sort
)

# A scan that finds nothing must fail, not pass. Otherwise a build-layout change
# silently turns this gate into a no-op that still reports green.
if [[ ${#ARTIFACTS[@]} -eq 0 ]]; then
    echo "symbol-scan: no shipped artifacts found under '$BUILD_DIR'" >&2
    echo "symbol-scan: build first, or fix the artifact globs in this script" >&2
    exit 2
fi

echo "symbol-scan: checking ${#ARTIFACTS[@]} shipped artifacts under $BUILD_DIR"

violations=0

for artifact in "${ARTIFACTS[@]}"; do
    # `nm -A` prefixes every line with `archive:object:`, which removes the
    # standalone `object.o:` header lines that would otherwise be mistaken for
    # symbols (an archive member named grpc_wire_codec.cpp.o is not a grpc symbol).
    # The three seds strip that prefix, then the address column, then the type
    # column, leaving the demangled symbol name alone on the line.
    hits=$(
        "$NM" -A -C "$artifact" 2>/dev/null \
            | sed 's/^[^:]*:[^:]*: *//' \
            | sed 's/^[0-9a-fA-F]* //' \
            | sed 's/^[A-Za-z] //' \
            | grep -E "$FORBIDDEN_PATTERN" \
            | sort -u || true
    )

    if [[ -n "$hits" ]]; then
        echo "symbol-scan: FORBIDDEN symbols in $artifact" >&2
        echo "$hits" | sed 's/^/    /' >&2
        violations=$((violations + 1))
    fi
done

if [[ $violations -ne 0 ]]; then
    echo >&2
    echo "symbol-scan: $violations artifact(s) violate the dependency closure." >&2
    echo "symbol-scan: see CLAUDE.md rule 13 and microtel-spec.md §3." >&2
    exit 1
fi

echo "symbol-scan: clean — no gRPC, abseil, or protobuf-cpp symbols"
