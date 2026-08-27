// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/trace.hpp"

#include "adapters/otelcpp/abi_guard.hpp"

#include <algorithm>
#include <cstdint>

#include <opentelemetry/nostd/span.h>
#include <opentelemetry/trace/span_context.h>

/// @file
/// Converts span identity between microtel and opentelemetry-cpp: `TraceId`,
/// `SpanId`, `TraceFlags`, and `SpanContext`, in both directions.
///
/// The byte layouts agree (W3C Trace Context: 16-byte trace id, 8-byte span
/// id, 1-byte flags), so every conversion is a plain copy.
///
/// **TraceState does not round-trip.** `microtel::TraceState` carries no
/// storage and its `FromHeader`/`ToHeader` are unimplemented declarations, so
/// bridging it would link-fail; both directions produce the empty default
/// instead. Revisit when microtel's TraceState gains an implementation.

namespace microtel::adapters::otelcpp
{

static_assert(microtel::TraceId::kSizeBytes == opentelemetry::trace::TraceId::kSize,
              "trace id width diverged between microtel and otel-cpp");
static_assert(microtel::SpanId::kSizeBytes == opentelemetry::trace::SpanId::kSize,
              "span id width diverged between microtel and otel-cpp");

/// @brief Copy a microtel trace id into otel-cpp's.
[[nodiscard]] inline opentelemetry::trace::TraceId ToOtelTraceId(
    const microtel::TraceId& trace_id) noexcept
{
    return opentelemetry::trace::TraceId{
        opentelemetry::nostd::span<const std::uint8_t, opentelemetry::trace::TraceId::kSize>{
            trace_id.AsBytes().data(), opentelemetry::trace::TraceId::kSize}};
}

/// @brief Copy a microtel span id into otel-cpp's.
[[nodiscard]] inline opentelemetry::trace::SpanId ToOtelSpanId(
    const microtel::SpanId& span_id) noexcept
{
    return opentelemetry::trace::SpanId{
        opentelemetry::nostd::span<const std::uint8_t, opentelemetry::trace::SpanId::kSize>{
            span_id.AsBytes().data(), opentelemetry::trace::SpanId::kSize}};
}

/// @brief Copy an otel-cpp trace id into microtel's.
[[nodiscard]] inline microtel::TraceId ToMicrotelTraceId(
    const opentelemetry::trace::TraceId& trace_id) noexcept
{
    microtel::TraceId::Bytes bytes{};
    const auto source = trace_id.Id();
    std::copy(source.begin(), source.end(), bytes.begin());
    return microtel::TraceId{bytes};
}

/// @brief Copy an otel-cpp span id into microtel's.
[[nodiscard]] inline microtel::SpanId ToMicrotelSpanId(
    const opentelemetry::trace::SpanId& span_id) noexcept
{
    microtel::SpanId::Bytes bytes{};
    const auto source = span_id.Id();
    std::copy(source.begin(), source.end(), bytes.begin());
    return microtel::SpanId{bytes};
}

/// @brief Convert a full span context, microtel → otel-cpp.
///
/// Ids, flags, and the remote bit copy exactly; trace state is the empty
/// default (see the file comment).
[[nodiscard]] inline opentelemetry::trace::SpanContext ToOtelSpanContext(
    const microtel::SpanContext& context) noexcept
{
    return opentelemetry::trace::SpanContext{
        ToOtelTraceId(context.trace_id),
        ToOtelSpanId(context.span_id),
        opentelemetry::trace::TraceFlags{context.trace_flags.AsByte()},
        context.remote};
}

/// @brief Convert a full span context, otel-cpp → microtel.
///
/// Ids, flags, and the remote bit copy exactly; trace state is the empty
/// default (see the file comment).
[[nodiscard]] inline microtel::SpanContext ToMicrotelSpanContext(
    const opentelemetry::trace::SpanContext& context) noexcept
{
    return microtel::SpanContext{
        .trace_id = ToMicrotelTraceId(context.trace_id()),
        .span_id = ToMicrotelSpanId(context.span_id()),
        .trace_flags = microtel::TraceFlags{context.trace_flags().flags()},
        .trace_state = {},
        .remote = context.IsRemote(),
    };
}

}  // namespace microtel::adapters::otelcpp
