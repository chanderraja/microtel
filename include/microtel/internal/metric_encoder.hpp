// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/metric_batch.hpp"

namespace microtel::internal
{

/// @brief Encodes a `MetricBatchHandle` into OTLP metrics protobuf bytes
/// (`ExportMetricsServiceRequest`).
///
/// Implemented by the upb `OtlpEncoder` extension (`metrics-design.md` §10) —
/// the only place that includes upb headers (LOCKED). No upb type appears in
/// this header. One arena per call; stateless across calls.
///
/// @threadsafety Not thread-safe (single-caller — the metrics exporter worker).
/// @see docs/metrics-design.md §10
class IMetricEncoder
{
public:
    virtual ~IMetricEncoder() noexcept = default;

    [[nodiscard]] virtual EncodedPayload Encode(const MetricBatchHandle& batch) = 0;
};

}  // namespace microtel::internal
