// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for the upb trace decoder alone —
// docs/leaf-concentrator-design.md §7.3, so decoder bugs are not hidden behind
// the receiver's validation.
//
// Bytes 0-2 of the input choose the `DecodeLimits`; the rest is the payload:
//
//   byte 0  max_spans            (0-255)
//   byte 1  max_depth            (0-255; 0 is upb's own default)
//   byte 2  max_arena_bytes      in 1 KiB steps (0-255 KiB)
//
// Beyond the standing invariants in tests/fuzz/README.md it asserts that a
// successful decode never returns more spans than `max_spans`, and that every
// span id it returns has the length its type requires (by construction) and
// an end time the clock can represent.
//
// Repro:
//   ./build-fuzz/tests/fuzz/otlp_trace_decoder_fuzz <crash_file>

#include "microtel/internal/otlp_trace_decoder.hpp"

#include "wire/encoder/otlp_trace_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace mti = microtel::internal;

namespace
{

constexpr std::size_t kHeaderBytes = 3;
constexpr std::size_t kArenaStep = 1024;

void Require(bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

bool EndsInRange(const mti::SpanRecord& span) noexcept
{
    return span.end_time.time_since_epoch().count() >= 0;
}

/// Every span a decode returned, checking each end time on the way.
std::size_t CheckedSpanCount(const std::vector<mti::DecodedResourceSpans>& decoded)
{
    std::size_t spans = 0;
    for (const auto& rs : decoded)
    {
        for (const auto& ss : rs.scopes)
        {
            spans += ss.spans.size();
            Require(std::ranges::all_of(ss.spans, EndsInRange));
        }
    }
    return spans;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < kHeaderBytes)
    {
        return 0;
    }
    const mti::DecodeLimits limits{
        .max_spans = data[0],
        .max_depth = data[1],
        .max_arena_bytes = static_cast<std::size_t>(data[2]) * kArenaStep,
    };
    // libFuzzer hands over bytes as uint8_t; the decoder takes std::byte.
    const auto* const bytes = reinterpret_cast<const std::byte*>(data);
    const std::span<const std::byte> payload = std::span{bytes, size}.subspan(kHeaderBytes);

    const microtel::wire::OtlpTraceDecoder decoder;
    const auto decoded = decoder.Decode(payload, limits);
    if (!decoded.has_value())
    {
        return 0;
    }
    const std::size_t spans = CheckedSpanCount(*decoded);
    Require(spans <= limits.max_spans);
    return 0;
}
