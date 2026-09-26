// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/view.hpp"

#include <chrono>
#include <cstddef>
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

/// @brief Per-instrument cardinality cap for the metrics pipeline.
///
/// When the number of distinct attribute-set combinations for one instrument
/// exceeds `max_cardinality`, further new attribute sets fold into the overflow
/// series (`otel.metric.overflow=true`).  The OTel-spec default is 2000.
///
/// Overridden at runtime by `MICROTEL_METRIC_CARDINALITY_LIMIT` (decimal integer).
struct MetricLimitOptions
{
    std::size_t max_cardinality = 2000;
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
///
/// **On failure the batch is dropped, not sent unauthenticated** — an error
/// return and a throw cost the same one batch, counted as
/// `non_retryable_failure` with the callback's message in
/// `HealthSnapshot::last_error_message` (`docs/interfaces.md` §4.9). A throw
/// is caught at the provider boundary and converted to
/// `Error::Kind::InternalFailure`; returning the error is still preferable,
/// since the kind then survives to the health snapshot.
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

    /// @brief Name this provider's profile. Defaults to `kDefaultProfileName`.
    ///
    /// Names identify live providers within the process, and `microtel::
    /// GetProvider(name)` is how one is found again. Each named profile is fully
    /// independent — its own endpoint, protocol, TLS material, sampler,
    /// `Resource`, pipelines, worker threads and I/O thread — and nothing is
    /// shared between them (ICP 0027).
    ///
    /// Compared byte-for-byte; no normalisation, no case folding. `Build()`
    /// fails with `ConfigError::Kind::DuplicateProfileName` if another **live**
    /// provider already carries this name — shutting a provider down does not
    /// release its name, destroying it does — with
    /// `ConfigError::Kind::ProfileLimitExceeded` when the process already holds
    /// the maximum number of live providers, and with
    /// `ConfigError::Kind::InvalidValue` if @p name is empty: an unnamed profile
    /// is `"default"`, not `""`.
    SdkBuilder& WithProfileName(std::string name);

    /// @brief Register a resource detector.
    ///
    /// Call once per detector; registration order is significant. `Build()`
    /// runs each detector exactly once, on the calling thread, and merges their
    /// contributions per `microtel-spec.md` §12.7: detectors first (a later one
    /// overriding an earlier one), then the configuration — `microtel.toml`,
    /// environment and code, resolved key by key in that ascending order
    /// (§12.1). A key set in any configured source therefore always beats the
    /// same key from a detector.
    ///
    /// A detector that returns a `ConfigError` is logged at Warn and skipped.
    /// Setting `sdk.resource_detectors_strict` in `microtel.toml`, or
    /// `MICROTEL_RESOURCE_DETECTORS_STRICT` in the environment, makes the same
    /// failure fail `Build()` instead.
    ///
    /// `microtel::MakeProcessDetector()` and `microtel::MakeHostDetector()` in
    /// `microtel/resource_detectors.hpp` supply the built-in detectors.
    ///
    /// @param detector ownership is moved in; `nullptr` is ignored.
    SdkBuilder& WithResourceDetector(std::unique_ptr<internal::IResourceDetector> detector);

    SdkBuilder& WithSampler(SamplerHandle sampler);
    /// @brief Set the batch processor knobs (span and log pipelines).
    ///
    /// Validated by `Build()` with the same rules `Provider::SetBatchOptions`
    /// applies: a zero `max_queue_size`, a zero `max_export_batch_size`, a
    /// `max_export_batch_size` above `max_queue_size`, or a `schedule_delay`
    /// of zero or less fails `Build()` with `ConfigError::Kind::InvalidValue`.
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

    /// @brief Set the per-instrument cardinality cap.
    ///
    /// Overrides `MICROTEL_METRIC_CARDINALITY_LIMIT`.
    SdkBuilder& WithMetricLimits(MetricLimitOptions opts);

    /// @brief Register one metric view.
    ///
    /// Views are evaluated in registration order. When no view matches an
    /// instrument, a default stream is created with the original instrument
    /// name. Multiple matching views create one stream each (fan-out).
    SdkBuilder& WithView(ViewConfig view);

    /// @brief Enable the concentrator's leaf receiver (ICP 0034,
    ///        `docs/leaf-concentrator-design.md` §4.3). Experimental.
    ///
    /// `Provider::GetLeafReceiver()` then returns a live receiver instead of
    /// the no-op one. The `[concentrator]` TOML table and the
    /// `MICROTEL_CONCENTRATOR_*` variables set the same options; @p opts is
    /// merged over them as the highest source (`docs/configuration.md` §3.14):
    /// every scalar and the resolver come from @p opts, while
    /// `leaf_defaults_resource` merges per key and `leaves` per leaf id.
    ///
    /// `Build()` validates the result and fails with
    /// `ConfigError::Kind::InvalidValue` on a bad limit or duration, a reserved
    /// or `leaf_id_attribute` key in a configured Resource, or a configured
    /// Resource over `max_leaf_resource_bytes`. In a library built without
    /// `MICROTEL_WITH_CONCENTRATOR` (the default), `Build()` fails with
    /// `InvalidValue` on field `concentrator.enabled` if any source enables the
    /// receiver.
    SdkBuilder& WithLeafReceiver(LeafReceiverOptions opts);

    /// @brief Validate configuration and construct a `Provider`.
    ///
    /// Eager validation: TOML parsing, env-var parsing, endpoint URL
    /// validation, TLS material readability, batch-option coherence (see
    /// `WithBatch`). Network reachability is **not**
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
