// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Headers smoke test.
//
// Compiles every public + internal header inside a gtest translation unit
// to catch name collisions, missing includes, or template ambiguity that
// only surfaces when gtest's headers are also present. Complementary to
// ci/header_check.cpp, which exercises the same headers without gtest.
//
// This test does no actual behavioural verification — its value is at
// compile time. The single TEST() body just succeeds so the binary
// exits 0 through gtest's main and CTest reports a pass.

#include <gtest/gtest.h>

// --- Public API ---
#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/meter.hpp"
#include "microtel/propagator.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/trace.hpp"
#include "microtel/tracer.hpp"
#include "microtel/version.hpp"

// --- Internal interfaces ---
#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/internal/reactor.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/internal/wire_result.hpp"

namespace
{

TEST(HeadersSmoke, AllPublicAndInternalHeadersCompile)
{
    // The compile passing is the test. The body just confirms gtest itself
    // is wired and microtel::Status (a public type) coexists with gtest's
    // own naming without collision.
    SUCCEED();
    static_assert(static_cast<int>(microtel::Status::Completed) == 0,
                  "lifecycle Status::Completed must be the zero value");
}

}  // namespace
