// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/trace.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace microtel
{

/// @brief Carrier-agnostic getter callback for `Extract`.
///
/// Given a header name, returns the first value of that header in the carrier,
/// or an empty optional if absent. Carriers (HTTP headers, gRPC metadata,
/// custom maps) adapt to this signature.
using HeaderGetter = std::function<std::optional<std::string_view>(std::string_view header)>;

/// @brief Carrier-agnostic setter callback for `Inject`.
///
/// Sets a header to the given value in the carrier.
using HeaderSetter = std::function<void(std::string_view header, std::string_view value)>;

/// @brief W3C Trace Context propagator.
///
/// Implements `traceparent` and `tracestate` per the W3C Trace Context
/// specification. v1 ships this propagator; W3C Baggage and other propagators
/// land in v1.1+ (see `microtel-roadmap.md` §v1.1).
///
/// @threadsafety Thread-safe (the propagator is stateless).
class W3CTraceContextPropagator
{
public:
    W3CTraceContextPropagator() noexcept = default;

    /// @brief Inject `context` into `setter` as `traceparent` and `tracestate`.
    ///
    /// If `context` is invalid, no headers are set.
    void Inject(const SpanContext& context, const HeaderSetter& setter) const;

    /// @brief Extract a `SpanContext` from `getter`.
    ///
    /// Returns an invalid `SpanContext` (`IsValid() == false`) if extraction
    /// fails for any reason. The `remote` flag of the returned context is
    /// always set to `true` on successful extraction.
    [[nodiscard]] SpanContext Extract(const HeaderGetter& getter) const;
};

}  // namespace microtel
