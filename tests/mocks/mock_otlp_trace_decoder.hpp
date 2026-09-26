// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/expected.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::IOtlpTraceDecoder`.
///
/// Returns a copy of `result_to_return` from every `Decode` call, whatever
/// the payload. Records the call count and the last limits and payload size so
/// a test can check what the receiver asked for. Single-threaded use only.
class MockOtlpTraceDecoder : public internal::IOtlpTraceDecoder
{
public:
    // --- Configurable returns ---
    Expected<std::vector<internal::DecodedResourceSpans>, internal::DecodeFailure> result_to_return{
        std::vector<internal::DecodedResourceSpans>{}};

    // --- Recording ---
    mutable int decode_call_count = 0;
    mutable internal::DecodeLimits last_limits{};
    mutable std::size_t last_payload_size = 0;

    // --- IOtlpTraceDecoder ---

    [[nodiscard]] Expected<std::vector<internal::DecodedResourceSpans>, internal::DecodeFailure>
    Decode(std::span<const std::byte> payload, const internal::DecodeLimits& limits) const override
    {
        ++decode_call_count;
        last_limits = limits;
        last_payload_size = payload.size();
        return result_to_return;
    }
};

}  // namespace microtel::testing
