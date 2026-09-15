// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/trace.hpp"

#include "adapters/otelcpp/abi_guard.hpp"

#include <algorithm>
#include <cstdint>

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/span.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/trace_state.h>

/// @file
/// Converts span identity between microtel and opentelemetry-cpp: `TraceId`,
/// `SpanId`, `TraceFlags`, and `SpanContext`, in both directions.
///
/// The byte layouts agree (W3C Trace Context: 16-byte trace id, 8-byte span
/// id, 1-byte flags), so every identity conversion is a plain copy.
///
/// **TraceState round-trips through its header form.** Both sides implement
/// the same W3C Trace Context §3.3 grammar but keep their entries in private
/// representations, so the serialised `tracestate` value is the bridge —
/// `ToHeader` on one side, `FromHeader` on the other. An entry either survives
/// exactly or, if the other side's parser rejects it, is absent: ICP 0015's
/// "preserve or omit, never invent" holds here too. microtel gained the
/// storage this needs in issue #208 / ICP 0025 packet 2.3a.
///
/// Both conversions stay `noexcept` because their callers are otel-cpp
/// overrides microtel implements on the span hot path. A non-empty trace state
/// makes them allocate, so allocation failure terminates rather than unwinds —
/// the same bargain hard rule 14 strikes everywhere else in microtel.

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

/// @brief Copy a microtel trace state into otel-cpp's, via its header form.
///
/// An empty state maps to otel-cpp's shared default rather than to a fresh
/// empty instance, so the common case allocates nothing.
[[nodiscard]] inline opentelemetry::nostd::shared_ptr<opentelemetry::trace::TraceState>
ToOtelTraceState(const microtel::TraceState& trace_state) noexcept
{
    if (trace_state.Empty())
    {
        return opentelemetry::trace::TraceState::GetDefault();
    }
    return opentelemetry::trace::TraceState::FromHeader(trace_state.ToHeader());
}

/// @brief Copy an otel-cpp trace state into microtel's, via its header form.
[[nodiscard]] inline microtel::TraceState ToMicrotelTraceState(
    const opentelemetry::nostd::shared_ptr<opentelemetry::trace::TraceState>& trace_state) noexcept
{
    if ((trace_state == nullptr) || trace_state->Empty())
    {
        return {};
    }
    return microtel::TraceState::FromHeader(trace_state->ToHeader());
}

/// @brief Convert a full span context, microtel → otel-cpp.
///
/// Ids, flags, and the remote bit copy exactly; trace state crosses as its
/// `tracestate` header value (see the file comment).
[[nodiscard]] inline opentelemetry::trace::SpanContext ToOtelSpanContext(
    const microtel::SpanContext& context) noexcept
{
    return opentelemetry::trace::SpanContext{
        ToOtelTraceId(context.trace_id),
        ToOtelSpanId(context.span_id),
        opentelemetry::trace::TraceFlags{context.trace_flags.AsByte()},
        context.remote,
        ToOtelTraceState(context.trace_state)};
}

/// @brief Convert a full span context, otel-cpp → microtel.
///
/// Ids, flags, and the remote bit copy exactly; trace state crosses as its
/// `tracestate` header value (see the file comment).
[[nodiscard]] inline microtel::SpanContext ToMicrotelSpanContext(
    const opentelemetry::trace::SpanContext& context) noexcept
{
    return microtel::SpanContext{
        .trace_id = ToMicrotelTraceId(context.trace_id()),
        .span_id = ToMicrotelSpanId(context.span_id()),
        .trace_flags = microtel::TraceFlags{context.trace_flags().flags()},
        .trace_state = ToMicrotelTraceState(context.trace_state()),
        .remote = context.IsRemote(),
    };
}

}  // namespace microtel::adapters::otelcpp
