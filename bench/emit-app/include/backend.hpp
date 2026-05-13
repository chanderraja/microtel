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

/// Counters reported by the backend after a run.
struct BackendStats
{
    uint64_t spans_exported_total{0};
    uint64_t spans_dropped_total{0};
    uint64_t bytes_sent_total{0};
};

/// Options passed to IBackend::Init().
struct BackendOptions
{
    std::string endpoint;        ///< e.g. "http://sink:4318"
    std::string service_name;
    std::string service_version;
    bool compression_gzip{false};
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
