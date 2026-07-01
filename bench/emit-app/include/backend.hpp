// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Abstract backend interface for emit-app.
// Each SUT provides a concrete implementation that wraps its SDK.

#pragma once

#include <cstdint>
#include <string>

namespace bench
{

/// Per-reason drop counters.  Four primary DropReason buckets are named;
/// the remaining reasons fold into `other`.  `total` is the sum over all
/// `kDropReasonCount` reasons.
/// otelcpp backends return zeros for all fields (no stable API equivalent).
struct DroppedCounts
{
    uint64_t queue_full{0};
    uint64_t record_too_large{0};
    uint64_t span_attribute_limit{0};
    uint64_t attribute_value_truncated{0};
    uint64_t other{0};
    uint64_t total{0};
};

/// Counters reported by the backend after a run.
struct BackendStats
{
    uint64_t spans_exported_total{0};
    DroppedCounts spans_dropped{};
    uint64_t bytes_sent_total{0};
};

/// Options passed to IBackend::Init().
struct BackendOptions
{
    std::string endpoint;            ///< e.g. "http://sink:4318"
    std::string service_name;
    std::string service_version;
    bool        compression_gzip{false};
    int         attributes_per_span{0};    ///< 0 = no attributes (hot-loop default)
    int         attribute_value_bytes{24}; ///< byte length of each attribute value
    int         metric_interval_ms{0};     ///< 0 = SDK default (60 s); set to 100 for metrics workload
};

/// Abstract tracing backend.
///
/// Implementations are in src/backend_microtel.cpp and src/backend_otelcpp.cpp.
/// Selected at compile time via BENCH_BACKEND_* preprocessor defines.
class IBackend
{
public:
    IBackend() = default;
    IBackend(const IBackend&) = delete;
    IBackend& operator=(const IBackend&) = delete;
    IBackend(IBackend&&) = delete;
    IBackend& operator=(IBackend&&) = delete;

    virtual ~IBackend() = default;

    /// Initialize the SDK and connect to the exporter endpoint.
    /// Called once before any EmitSpan() calls.
    virtual void Init(const BackendOptions& opts) = 0;

    /// Emit one minimal span.  Called in the hot loop; must be non-blocking
    /// on the critical path (batching is handled by the SDK internally).
    virtual void EmitSpan() = 0;

    /// Emit one realistic request: one parent span and two child spans.
    /// Used by the realistic-request workload profile.
    virtual void EmitRequest() = 0;

    /// Emit one metric record: one Counter::Add() + one Histogram::Record().
    /// Used by the hot-loop-metrics workload profile.
    /// Default is a no-op for backends that do not support metrics.
    virtual void EmitRecord() {}

    /// Flush all in-flight spans to the exporter and return elapsed time in ns.
    /// Returns 0 if the backend has no explicit flush API.
    [[nodiscard]] virtual uint64_t ForceFlush() { return 0; }

    /// Flush all pending spans and shut down the SDK.
    /// Called once after the workload completes.
    virtual void Shutdown() = 0;

    /// Return counters accumulated since Init().
    /// Called after Shutdown().
    [[nodiscard]] virtual BackendStats Stats() const = 0;
};

/// Factory function — defined once per backend translation unit.
/// Returns a heap-allocated IBackend; ownership belongs to the caller.
IBackend* CreateBackend();

}  // namespace bench
