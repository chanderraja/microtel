#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Dependency-closure gate. Two passes over every shipped microtel artifact:
#
#   1. FORBIDDEN — no artifact defines or references a symbol from gRPC, abseil,
#      or the protobuf C++ runtime.
#   2. UNPREFIXED VENDORED — no artifact defines or references a vendored upb /
#      utf8_range symbol under its upstream name. They ship renamed to
#      `microtel_*` (ICP 0020 Decision 4).
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
# its start. Anchoring is what keeps the upb-generated accessors legal: upb emits
# C symbols like `google_protobuf_FileDescriptorProto_set_name`, which are upb's
# own generated code and must NOT trip `google::protobuf::` (the C++ runtime).
FORBIDDEN_PATTERN='^(absl::|absl_|grpc::|grpc_|GRPC_|google::protobuf::)'

# Vendored upb / utf8_range symbols under their *upstream* names. These are
# members of the dependency closure, so they are not a rule-13 violation — but
# they must not ship unprefixed. ICP 0020 Decision 4: every vendored global is
# renamed to `microtel_<upstream name>` by the force-included rename header
# `third_party/upb/microtel_upb_rename.h`, so that a consumer who also links
# real upb cannot get two definitions of `upb_Arena_Init` and a silent static-
# link selection between them.
#
# The `^` anchor is load-bearing in the other direction too: a correctly renamed
# symbol starts with `microtel_`, so it can never match this pattern. Anything
# that does match escaped the rename header — usually a symbol added by a upb
# pin bump. The fix is to regenerate the header, not to widen this pattern; the
# recipe is in the header's comment block.
UNPREFIXED_VENDORED_PATTERN='^(_?upb_|_?kUpb_|kWyhashSalt$|UPB_linkarr|utf8_range_)'

# This pass looks at *externally visible* symbols only (`nm -g`: `T`/`D`/`R`/`B`,
# weak `W`/`V`, and undefined `U`). That is precisely the collision surface —
# a file-local `t upb_mapsorter_cmpi64` is never a candidate when the linker
# resolves a consumer's reference, so renaming it buys nothing.
#
# Restricting to global linkage also makes the gate deterministic. `UPB_INLINE`
# functions are emitted out-of-line (weak) at -O0 and inlined away at -O2, and
# file-local statics come and go with the compiler's own inlining decisions —
# measured on this pin, the all-linkage symbol set differs between clang and gcc
# at the same -O level, while the global set is byte-identical across
# clang-Debug, gcc-Debug and clang-Release (Release being a strict subset).
# A gate that fires on inlining decisions would be a flake, not a check.
NM_VENDORED_FLAGS=(-A -C -g)

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

# Symbol names in one artifact, one per line, defined and undefined alike.
#
# `nm -A` prefixes every line with `archive:object:`, which removes the
# standalone `object.o:` header lines that would otherwise be mistaken for
# symbols (an archive member named grpc_wire_codec.cpp.o is not a grpc symbol).
# The three seds strip that prefix, then the address column, then the type
# column, leaving the demangled symbol name alone on the line.
#
# $1 is the artifact; remaining arguments are the nm flags to use.
symbols_of() {
    local artifact="$1"
    shift
    "$NM" "$@" "$artifact" 2>/dev/null \
        | sed 's/^[^:]*:[^:]*: *//' \
        | sed 's/^[0-9a-fA-F]* //' \
        | sed 's/^[A-Za-z] //'
}

forbidden_violations=0
unprefixed_violations=0

for artifact in "${ARTIFACTS[@]}"; do
    hits=$(symbols_of "$artifact" -A -C | grep -E "$FORBIDDEN_PATTERN" | sort -u || true)
    if [[ -n "$hits" ]]; then
        echo "symbol-scan: FORBIDDEN symbols in $artifact" >&2
        echo "$hits" | sed 's/^/    /' >&2
        forbidden_violations=$((forbidden_violations + 1))
    fi

    hits=$(
        symbols_of "$artifact" "${NM_VENDORED_FLAGS[@]}" \
            | grep -E "$UNPREFIXED_VENDORED_PATTERN" \
            | sort -u || true
    )
    if [[ -n "$hits" ]]; then
        echo "symbol-scan: UNPREFIXED vendored symbols in $artifact" >&2
        echo "$hits" | sed 's/^/    /' >&2
        unprefixed_violations=$((unprefixed_violations + 1))
    fi
done

if [[ $forbidden_violations -ne 0 ]]; then
    echo >&2
    echo "symbol-scan: $forbidden_violations artifact(s) violate the dependency closure." >&2
    echo "symbol-scan: see CLAUDE.md rule 13 and microtel-spec.md §3." >&2
fi

if [[ $unprefixed_violations -ne 0 ]]; then
    echo >&2
    echo "symbol-scan: $unprefixed_violations artifact(s) ship vendored upb/utf8_range" >&2
    echo "symbol-scan: symbols under their upstream names. Every vendored global must" >&2
    echo "symbol-scan: carry the microtel_ prefix — see ICP 0020 Decision 4 and the" >&2
    echo "symbol-scan: regeneration recipe in third_party/upb/microtel_upb_rename.h." >&2
fi

if [[ $((forbidden_violations + unprefixed_violations)) -ne 0 ]]; then
    exit 1
fi

echo "symbol-scan: clean — no gRPC, abseil, or protobuf-cpp symbols"
echo "symbol-scan: clean — no unprefixed vendored upb/utf8_range symbols"
