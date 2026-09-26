// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/expected.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"

#include <atomic>
#include <cstddef>
#include <new>
#include <span>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `IOtlpTraceDecoder` that serves a canned decode, applies the
///        span bound to it the way the real decoder does, and can be told to
///        fail allocation.
///
/// A fake rather than a mock because its answer depends on its input: the
/// `max_spans` bound turns the canned result into `TooLarge`, and one chosen
/// call throws `std::bad_alloc` to drive the receiver's out-of-memory path
/// (`docs/leaf-concentrator-design.md` §7.4). Safe to call from several
/// threads at once once configured: the canned result is only read.
class FakeOtlpTraceDecoder : public internal::IOtlpTraceDecoder
{
public:
    /// The decode every call returns, subject to `max_spans`.
    std::vector<internal::DecodedResourceSpans> canned;
    /// Throw `std::bad_alloc` on this call number (1-based); 0 never throws.
    int throw_bad_alloc_on_call = 0;

    mutable std::atomic<int> decode_call_count{0};

    [[nodiscard]] Expected<std::vector<internal::DecodedResourceSpans>, internal::DecodeFailure>
    Decode(std::span<const std::byte> /*payload*/,
           const internal::DecodeLimits& limits) const override
    {
        const int call = ++decode_call_count;
        if (call == throw_bad_alloc_on_call)
        {
            throw std::bad_alloc{};
        }
        std::size_t spans = 0;
        for (const auto& rs : canned)
        {
            for (const auto& ss : rs.scopes)
            {
                spans += ss.spans.size();
            }
        }
        if (spans > limits.max_spans)
        {
            return make_unexpected(internal::DecodeFailure::TooLarge);
        }
        return canned;
    }
};

}  // namespace microtel::testing
