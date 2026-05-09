// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/otlp_encoder.hpp"

namespace microtel::wire
{

/// @brief Concrete OTLP protobuf encoder.
///
/// The only compilation unit that includes upb headers is `otlp_encoder.cpp`.
/// No upb type appears here or in any header it includes.
///
/// @threadsafety Not thread-safe (single-caller — exporter worker).
class OtlpEncoder final : public internal::IOtlpEncoder
{
public:
    OtlpEncoder() noexcept                             = default;
    ~OtlpEncoder() noexcept override                   = default;
    OtlpEncoder(const OtlpEncoder&)                    = delete;
    OtlpEncoder& operator=(const OtlpEncoder&)         = delete;
    OtlpEncoder(OtlpEncoder&&) noexcept                = delete;
    OtlpEncoder& operator=(OtlpEncoder&&) noexcept     = delete;

    /// @brief Encode all spans in `batch` into an OTLP `ExportTraceServiceRequest`.
    ///
    /// One upb arena per call; arena freed before returning. Stateless across calls.
    /// Returns an empty payload if `batch` contains no spans.
    [[nodiscard]] internal::EncodedPayload Encode(const internal::BatchHandle& batch) override;
};

}  // namespace microtel::wire
