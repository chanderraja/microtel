// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>

/// @file
/// `ShimOptions` — configuration the shim's entry points (`MakeTracerProvider`,
/// `MakeMeterProvider`, `MakeLoggerProvider`, `RegisterGlobally`) accept and
/// copy into every object they create (ICP 0033).

namespace microtel::adapters::otelcpp
{

/// @brief Configuration for the otel-cpp shim.
///
/// Copied into each provider shim at construction and from there into every
/// tracer, span, meter, instrument, logger and log record it creates. An
/// object keeps the options it was created with for its lifetime (ICP 0033
/// §6).
struct ShimOptions
{
    /// Largest hex rendering, in bytes, of a `span<const uint8_t>` attribute
    /// the shim forwards. A byte span of n bytes renders as 2n characters;
    /// if 2n exceeds this, the attribute is omitted and counted in
    /// `ShimDiagnostics::oversized_byte_attributes_omitted`. Set it to the
    /// same value as `SpanLimitOptions::attribute_value_length_limit`.
    /// `std::nullopt`: no shim limit (the behaviour before ICP 0033).
    std::optional<std::uint32_t> attribute_value_length_limit = 4096;
};

}  // namespace microtel::adapters::otelcpp
