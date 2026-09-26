// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/batch.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microtel::internal
{

/// @brief Bounds one `IOtlpTraceDecoder::Decode` call enforces
///        (`docs/leaf-concentrator-design.md` §3.7).
struct DecodeLimits
{
    /// Most spans the whole request may carry, across every ResourceSpans.
    std::uint32_t max_spans = 0;
    /// Most nested submessages the wire decoder follows. 0 means the wire
    /// decoder's own default, which is far deeper than any legal OTLP path.
    std::uint16_t max_depth = 0;
    /// Most bytes the per-call decode arena may request from the allocator.
    std::size_t max_arena_bytes = 0;
};

/// @brief Why a `Decode` call produced nothing.
enum class DecodeFailure : std::uint8_t
{
    Malformed = 0,  ///< not a valid ExportTraceServiceRequest, or invalid UTF-8
    TooLarge = 1,   ///< a DecodeLimits bound was hit
};

/// @brief One decoded ScopeSpans. Plain C++ values; nothing upb-derived
///        survives the call.
struct DecodedScopeSpans
{
    InstrumentationScope scope;
    std::vector<SpanRecord> spans;
};

/// @brief One decoded ResourceSpans.
struct DecodedResourceSpans
{
    /// The Resource attributes exactly as the payload carried them, reserved
    /// `microtel.leaf.*` keys included.
    std::vector<KeyValue> resource;
    std::vector<DecodedScopeSpans> scopes;
    /// Span, event and link attributes this ResourceSpans lost because
    /// `AttributeValue` cannot hold them (`kvlist_value`, nested or
    /// mixed-type arrays; design §3.4). The caller counts them as
    /// `span_attribute_limit`.
    std::uint64_t dropped_span_attributes = 0;
    /// Resource attributes lost for the same reason.
    std::uint64_t dropped_resource_attributes = 0;
};

/// @brief Decodes an OTLP `ExportTraceServiceRequest` into plain C++ values.
///
/// The one production implementation, `wire::OtlpTraceDecoder`, uses upb and
/// lives in `src/wire/encoder/` with the encoder (ICP 0031 Decision 5). It
/// follows the encoder's arena rule (`docs/memory-model.md` §3.1): one arena
/// per `Decode` call, destroyed before the call returns.
///
/// Beyond parsing, the decoder rejects as `Malformed` anything a `SpanRecord`
/// cannot represent: a trace id that is not 16 bytes, a span id that is not 8,
/// a parent span id that is neither empty nor 8, a span kind or status code
/// out of range, a timestamp past the range of `system_clock`. Semantic checks
/// (non-zero ids, end after start, the reserved leaf attributes) belong to the
/// caller.
///
/// Stateless across calls.
///
/// @threadsafety Thread-safe: `Decode` is `const` and keeps all state local.
/// @see docs/interfaces.md §4
class IOtlpTraceDecoder
{
public:
    IOtlpTraceDecoder() noexcept = default;
    virtual ~IOtlpTraceDecoder() noexcept = default;

    IOtlpTraceDecoder(const IOtlpTraceDecoder&) = delete;
    IOtlpTraceDecoder& operator=(const IOtlpTraceDecoder&) = delete;
    IOtlpTraceDecoder(IOtlpTraceDecoder&&) = delete;
    IOtlpTraceDecoder& operator=(IOtlpTraceDecoder&&) = delete;

    /// @brief Decode @p payload, enforcing @p limits.
    ///
    /// @param payload the request bytes. Borrowed for the call only.
    /// @param limits  the bounds that turn a hostile payload into `TooLarge`.
    /// @return the decoded ResourceSpans in payload order, or why there are
    ///         none.
    /// @throws std::bad_alloc if building the C++ values fails. The arena's
    ///         own exhaustion is `TooLarge`, not an exception.
    [[nodiscard]] virtual Expected<std::vector<DecodedResourceSpans>, DecodeFailure> Decode(
        std::span<const std::byte> payload, const DecodeLimits& limits) const = 0;
};

}  // namespace microtel::internal
