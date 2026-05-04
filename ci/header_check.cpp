// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// M0 header compile check.
//
// Includes every public and internal header to verify they compile in
// isolation under -Werror. This file is the operational realisation of the
// ICP 0001 rule: "Both header trees compile under an INTERFACE CMake target
// with -Werror. No implementations." Implementation begins in M3.
//
// This file declares no symbols and contains no callable code. It is built as
// an executable purely to put the headers through the compiler.

// --- Public API ---
#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/log_sink.hpp"
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

int main()
{
    return 0;
}
