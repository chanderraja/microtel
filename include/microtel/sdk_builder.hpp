// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microtel
{

/// @brief Aggregation temporality preference for the metrics pipeline.
///
/// Selects whether metric data points accumulate since process start
/// (`Cumulative`) or reset each export cycle (`Delta`). `LowMemory` approximates
/// delta for all instruments; per-instrument-kind selection is v1.3 work.
///
/// Maps to `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`
/// ("cumulative" | "delta" | "lowmemory"). OTel default: cumulative.
enum class TemporalityPreference : std::uint8_t
{
    Cumulative = 0,  ///< All instruments report cumulative sums.
    Delta = 1,       ///< All instruments reset each collection cycle.
    LowMemory = 2,   ///< Delta for all instruments (per-kind mapping: v1.3).
};

/// @brief Drop policy when the span queue reaches `max_queue_size`.
enum class DropPolicy : std::uint8_t
{
    /// @brief Drop the incoming record (caller's `End()`). FIFO preserved.
    DropNewest = 0,
    /// @brief Evict the oldest queued record to make room.
    DropOldest = 1,
};

/// @brief Batch span processor configuration.
///
/// Designated-initialiser construction is the expected idiom:
/// `WithBatch({.max_queue_size = 8192, .schedule_delay = 5s})`.
struct BatchOptions
{
    std::uint32_t max_queue_size = 8192;
    std::uint32_t max_export_batch_size = 512;
    std::chrono::milliseconds schedule_delay = std::chrono::seconds(5);
    DropPolicy drop_policy = DropPolicy::DropNewest;
};

/// @brief Six-axis timeout taxonomy from `microtel-spec.md` §7.3.
struct TimeoutOptions
{
    std::chrono::milliseconds connect = std::chrono::seconds(10);
    std::chrono::milliseconds tls_handshake = std::chrono::seconds(10);
    std::chrono::milliseconds per_export = std::chrono::seconds(10);
    std::chrono::milliseconds retry_budget = std::chrono::seconds(60);
    std::chrono::milliseconds flush = std::chrono::seconds(5);
    std::chrono::milliseconds shutdown = std::chrono::seconds(5);
};

/// @brief TLS material configuration.
struct TlsOptions
{
    bool insecure = false;
    std::filesystem::path ca_bundle;    ///< empty: use system trust
    std::filesystem::path client_cert;  ///< empty: no mTLS
    std::filesystem::path client_key;   ///< empty: no mTLS
    std::string sni_override;           ///< empty: derive from endpoint host
};

/// @brief Span structural-limit configuration (per `microtel-spec.md` §5.6).
struct SpanLimitOptions
{
    std::uint32_t attribute_count_limit = 128;
    std::uint32_t event_count_limit = 128;
    std::uint32_t link_count_limit = 128;
    std::uint32_t attribute_value_length_limit = 4096;
    std::uint32_t event_attribute_count_limit = 128;
    std::uint32_t link_attribute_count_limit = 128;
};

/// @brief Memory-budget configuration (per `microtel-spec.md` §5.5).
struct MemoryLimitOptions
{
    std::uint64_t max_total_queue_bytes = 16ULL * 1024ULL * 1024ULL;  // 16 MiB
    std::uint32_t max_record_bytes = 64 * 1024;                       // 64 KiB
    std::uint32_t max_response_bytes = 1 * 1024 * 1024;               // 1 MiB
    std::uint32_t max_trailer_bytes = 64 * 1024;                      // 64 KiB
    std::uint32_t max_decompressed_bytes = 4 * 1024 * 1024;           // 4 MiB
};

/// @brief Callback returning the current `Authorization` header value.
///
/// Called per-export-batch with results cached for a configurable TTL.
using AuthCallback = std::function<Expected<std::string, Error>()>;

/// @brief Fluent builder for configuring and constructing a `Provider`.
///
/// Single-shot: `Build()` consumes the builder. A second call returns
/// `ConfigError::Kind::BuildAlreadyConsumed`.
///
/// @threadsafety Externally synchronised — caller serialises chained `WithXxx`
///               calls. The completed `Build()` produces a `Provider` that is
///               itself thread-safe.
class SdkBuilder
{
public:
    SdkBuilder() noexcept;
    ~SdkBuilder() noexcept;

    SdkBuilder(const SdkBuilder&) = delete;
    SdkBuilder& operator=(const SdkBuilder&) = delete;
    SdkBuilder(SdkBuilder&&) noexcept = default;
    SdkBuilder& operator=(SdkBuilder&&) noexcept = default;

    /// @brief Load configuration from a `microtel.toml` file.
    ///
    /// Subsequent `WithXxx` calls override file-resolved values per the
    /// precedence rules in `docs/configuration.md` §1.
    SdkBuilder& FromFile(std::filesystem::path path);

    SdkBuilder& WithEndpoint(std::string endpoint);
    SdkBuilder& WithProtocol(Protocol p);
    SdkBuilder& WithCompressionGzip(bool on);
    SdkBuilder& WithHeaders(std::vector<KeyValue> headers);

    SdkBuilder& WithServiceName(std::string name);
    SdkBuilder& WithServiceVersion(std::string version);
    SdkBuilder& WithResource(std::vector<KeyValue> attrs);

    SdkBuilder& WithSampler(SamplerHandle sampler);
    SdkBuilder& WithBatch(BatchOptions opts);
    SdkBuilder& WithSpanLimits(SpanLimitOptions opts);
    SdkBuilder& WithMemoryLimits(MemoryLimitOptions opts);

    SdkBuilder& WithTimeouts(TimeoutOptions opts);
    SdkBuilder& WithTls(TlsOptions opts);
    SdkBuilder& WithAuthProvider(AuthCallback cb,
                                 std::chrono::milliseconds cache_ttl = std::chrono::seconds(60));

    /// @brief Set the `PeriodicExportingMetricReader` export interval.
    ///
    /// Overrides `OTEL_METRIC_EXPORT_INTERVAL`. OTel default: 60 s.
    SdkBuilder& WithMetricInterval(std::chrono::milliseconds interval);

    /// @brief Set the aggregation temporality preference for the metrics pipeline.
    ///
    /// Overrides `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`.
    /// OTel default: `Cumulative`.
    SdkBuilder& WithMetricTemporality(TemporalityPreference pref);

    /// @brief Validate configuration and construct a `Provider`.
    ///
    /// Eager validation: TOML parsing, env-var parsing, endpoint URL
    /// validation, TLS material readability. Network reachability is **not**
    /// validated — call `Provider::Connect()` if eager network preflight is
    /// needed.
    ///
    /// @return non-null `shared_ptr<Provider>` on success.
    /// @return `ConfigError` on any validation failure.
    [[nodiscard]] Expected<std::shared_ptr<Provider>, ConfigError> Build();

private:
    struct Impl;  ///< pimpl; defined in src/sdk/sdk_builder.cpp
    std::unique_ptr<Impl> m_impl;
};

}  // namespace microtel
