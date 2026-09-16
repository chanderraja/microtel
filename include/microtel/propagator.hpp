// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/baggage.hpp"
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
/// specification. `W3CBaggagePropagator` below is its sibling for the
/// `baggage` header; the two are independent and write disjoint headers, so a
/// caller uses either or both.
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

/// @brief W3C Baggage propagator.
///
/// Implements the `baggage` header per the [W3C Baggage](https://www.w3.org/TR/baggage/)
/// specification, reusing the same carrier callbacks as
/// `W3CTraceContextPropagator` ([ICP 0025](../../docs/icps/0025-propagation-core.md) §4).
///
/// The surface takes and returns a `Baggage` rather than a `Context`: baggage
/// is the only thing on a `Context` this propagator touches, and reaching the
/// calling thread's context is one expression at the call site —
/// `Inject(CurrentContext().baggage, setter)` on the way out, and
/// `ctx.baggage = Extract(getter)` on the way in.
///
/// @threadsafety Thread-safe (the propagator is stateless).
class W3CBaggagePropagator
{
public:
    W3CBaggagePropagator() noexcept = default;

    /// @brief Inject `baggage` into `setter` as the `baggage` header.
    ///
    /// Sets nothing if `baggage` is empty — an empty `baggage` header value is
    /// not legal, so it is omitted rather than sent blank.
    void Inject(const Baggage& baggage, const HeaderSetter& setter) const;

    /// @brief Extract a `Baggage` from `getter`.
    ///
    /// Returns an empty `Baggage` if the header is absent or if no list-member
    /// survives parsing. A partly malformed header yields its good members:
    /// see `Baggage::FromHeader` for the per-member rule and the limits.
    [[nodiscard]] Baggage Extract(const HeaderGetter& getter) const;
};

}  // namespace microtel
