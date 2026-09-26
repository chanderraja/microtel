// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Differential fuzz target for the leaf's two encoder backends
// (docs/leaf-concentrator-design.md §2.3, §7.2).
//
// ICP 0031 requires the upb and nanopb backends to produce the same bytes for
// the same spans. The input is a program, not a payload: four configuration
// bytes (time mode, clock, record buffer size, per-span caps, Resource, scope),
// then a stream of builder calls — span start (local and remote parent),
// attributes of every type, events, status, end, clock sync, clock advance,
// buffer encode (size query, exact, one byte short, ample) and streaming
// encode (with a sink that may refuse). tests/leaf/diff/ runs it on two leaves
// built from identical inputs and linked through microtel_leaf_dual; at every
// encode the first leaf is encoded with upb and the second with nanopb.
//
// Asserted, beyond crash-freedom: every builder call returns the same status
// on both leaves, and every encode returns the same status, the same size and
// the same bytes, which decode as an ExportTraceServiceRequest.
//
// Both backends are linked into this one binary: their entry points have
// distinct names (§2.1) and every vendored symbol is renamed (microtel_upb_*,
// microtel_pb_*), so nothing collides.
//
// Repro:
//   ./build-fuzz/tests/fuzz/leaf_backend_diff_fuzz <crash_file>

#include "leaf/diff/leaf_diff_program.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const microtel::leaf_diff::Outcome out = microtel::leaf_diff::RunProgram(data, size);
    if (!out.mismatch.empty())
    {
        (void)std::fprintf(stderr, "leaf_backend_diff_fuzz: %s\n", out.mismatch.c_str());
        std::abort();
    }
    return 0;
}
