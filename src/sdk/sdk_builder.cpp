// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// GCC 15 false-positive: variant copy-assign inlined into <variant> internals
// triggers -Wfree-nonheap-object. Not present on g++-13 (CI) or clang.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

#include "microtel/sdk_builder.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"

#include "common/config/auth_providers.hpp"
#include "common/config/config.hpp"
#include "common/config/config_validator.hpp"
#include "common/config/env_resolver.hpp"
#include "common/config/toml_loader.hpp"
#include "common/internal_log.hpp"
#include "exporter/otlp_exporter.hpp"
#include "exporter/otlp_log_exporter.hpp"
#include "exporter/otlp_metric_exporter.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/metric_attribute_set.hpp"
#include "sdk/resource_builder.hpp"
#include "sdk/sdk_provider.hpp"
#include "sdk/view_registry.hpp"
#include "transport/epoll_reactor.hpp"
#include "transport/http2_transport.hpp"
#include "wire/encoder/otlp_encoder.hpp"
#include "wire/grpc/grpc_wire_codec.hpp"
#include "wire/http/http_wire_codec.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microtel
{

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct SdkBuilder::Impl
{
    std::optional<std::filesystem::path> file_path;

    std::optional<std::string> endpoint;
    std::optional<Protocol> protocol;
    std::optional<bool> compression_gzip;
    std::optional<std::vector<KeyValue>> headers;
    std::optional<std::string> service_name;
    std::optional<std::string> service_version;
    std::optional<std::vector<KeyValue>> resource_attrs;
    /// Registration-ordered; `Build` runs each exactly once (interfaces.md §4.10).
    std::vector<std::unique_ptr<internal::IResourceDetector>> resource_detectors;
    SamplerHandle sampler;
    std::optional<BatchOptions> batch;
    std::optional<SpanLimitOptions> span_limits;
    std::optional<MemoryLimitOptions> memory_limits;
    std::optional<TimeoutOptions> timeouts;
    std::optional<TlsOptions> tls;
    std::optional<AuthCallback> auth_cb;
    std::chrono::milliseconds auth_cache_ttl{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> metric_interval;
    std::optional<TemporalityPreference> metric_temporality;
    std::optional<MetricLimitOptions> metric_limits;
    std::vector<ViewConfig> views;

    bool consumed = false;

    [[nodiscard]] Expected<config::Config, ConfigError> LoadConfig() const;
    void ApplyExporterOverrides(config::Config& cfg) const;
    void ApplyResourceOverrides(config::Config& cfg) const;
};

// ---------------------------------------------------------------------------
// SdkBuilder — constructor / destructor
// ---------------------------------------------------------------------------

SdkBuilder::SdkBuilder() noexcept : m_impl(std::make_unique<Impl>())
{
    m_impl->sampler = MakeAlwaysOnSampler();
}

SdkBuilder::~SdkBuilder() noexcept = default;

// ---------------------------------------------------------------------------
// SdkBuilder — WithXxx methods
// ---------------------------------------------------------------------------

SdkBuilder& SdkBuilder::FromFile(std::filesystem::path path)
{
    m_impl->file_path = std::move(path);
    return *this;
}

SdkBuilder& SdkBuilder::WithEndpoint(std::string endpoint)
{
    m_impl->endpoint = std::move(endpoint);
    return *this;
}

SdkBuilder& SdkBuilder::WithProtocol(Protocol p)
{
    m_impl->protocol = p;
    return *this;
}

SdkBuilder& SdkBuilder::WithCompressionGzip(bool on)
{
    m_impl->compression_gzip = on;
    return *this;
}

SdkBuilder& SdkBuilder::WithHeaders(std::vector<KeyValue> headers)
{
    m_impl->headers = std::move(headers);
    return *this;
}

SdkBuilder& SdkBuilder::WithServiceName(std::string name)
{
    m_impl->service_name = std::move(name);
    return *this;
}

SdkBuilder& SdkBuilder::WithServiceVersion(std::string version)
{
    m_impl->service_version = std::move(version);
    return *this;
}

SdkBuilder& SdkBuilder::WithResource(std::vector<KeyValue> attrs)
{
    m_impl->resource_attrs = std::move(attrs);
    return *this;
}

SdkBuilder& SdkBuilder::WithResourceDetector(std::unique_ptr<internal::IResourceDetector> detector)
{
    // A null detector is dropped rather than stored: `BuildResource` documents
    // its span as non-null, and a moved-from unique_ptr reaching Build() as a
    // crash would be a poor trade for the check this costs.
    if (detector != nullptr)
    {
        m_impl->resource_detectors.push_back(std::move(detector));
    }
    return *this;
}

SdkBuilder& SdkBuilder::WithSampler(SamplerHandle sampler)
{
    m_impl->sampler = std::move(sampler);
    return *this;
}

SdkBuilder& SdkBuilder::WithBatch(BatchOptions opts)
{
    m_impl->batch = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithSpanLimits(SpanLimitOptions opts)
{
    m_impl->span_limits = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithMemoryLimits(MemoryLimitOptions opts)
{
    m_impl->memory_limits = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithTimeouts(TimeoutOptions opts)
{
    m_impl->timeouts = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithTls(TlsOptions opts)
{
    m_impl->tls = std::move(opts);
    return *this;
}

SdkBuilder& SdkBuilder::WithAuthProvider(AuthCallback cb, std::chrono::milliseconds cache_ttl)
{
    m_impl->auth_cb = std::move(cb);
    m_impl->auth_cache_ttl = cache_ttl;
    return *this;
}

SdkBuilder& SdkBuilder::WithMetricInterval(std::chrono::milliseconds interval)
{
    m_impl->metric_interval = interval;
    return *this;
}

SdkBuilder& SdkBuilder::WithMetricTemporality(TemporalityPreference pref)
{
    m_impl->metric_temporality = pref;
    return *this;
}

SdkBuilder& SdkBuilder::WithMetricLimits(MetricLimitOptions opts)
{
    m_impl->metric_limits = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithView(ViewConfig view)
{
    m_impl->views.push_back(std::move(view));
    return *this;
}

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------

namespace
{

/// @brief Emit the warnings for configurations that are legal but very likely
///        wrong.
///
/// Neither case is rejected here. Plaintext OTLP/HTTP is legitimate in front of
/// an h2c-capable proxy (and the bench harness's own sink), and a default build
/// permits `insecure = true` outright per spec §12.3 — the hard ban belongs to
/// `MICROTEL_FORBID_INSECURE_TLS=ON`, which `config::Validate` enforces before
/// this ever runs. Both are, however, overwhelmingly likely to be a mistake,
/// and `config::Validate` returns `Expected<void, ConfigError>`: it can reject
/// a configuration but it cannot warn about one.
///
/// @param cfg Borrowed; read only. Both fields are post-resolution:
///            `cfg.endpoint.scheme` has collapsed `grpc://` to `http`, and
///            `cfg.protocol` has already taken `Grpc` from that same scheme,
///            so the plaintext warning below sees a gRPC endpoint as gRPC.
void WarnOnRiskyConfig(const config::Config& cfg) noexcept
{
    // h2c with prior knowledge: microtel has no HTTP/1.1 mode, and a stock
    // OpenTelemetry Collector's plaintext OTLP/HTTP receiver has no h2c. See
    // issue #166. OTLP/gRPC over the same scheme is unaffected — gRPC is h2c
    // by definition — so the protocol is part of the condition.
    if (cfg.protocol == Protocol::Http && cfg.endpoint.scheme == "http")
    {
        internal::LogImpl(
            LogLevel::Warn,
            "plaintext OTLP/HTTP (http:// with protocol=http) is HTTP/2 with prior knowledge "
            "and cannot reach an HTTP/1.1-only OTLP receiver such as a stock OpenTelemetry "
            "Collector - use https:// or OTLP/gRPC; see docs/compatibility-matrix.md");
    }

    if (cfg.tls.insecure)
    {
        internal::LogImpl(LogLevel::Warn,
                          "tls.insecure = true - TLS certificate verification is "
                          "disabled and any certificate will be accepted, including an "
                          "attacker's. Not for production; see docs/compatibility-matrix.md");
    }
}

/// @brief Parse `MICROTEL_METRIC_CARDINALITY_LIMIT` from the environment.
///
/// Returns the parsed value if the variable is set and contains a positive
/// decimal integer; returns `std::nullopt` for an unset, empty, zero, or
/// malformed value.
[[nodiscard]] std::optional<std::size_t> ParseEnvCardinality() noexcept
{
    const char* const raw = std::getenv("MICROTEL_METRIC_CARDINALITY_LIMIT");
    if (raw == nullptr)
    {
        return std::nullopt;
    }
    try
    {
        const std::string str{raw};
        std::size_t pos{};
        const auto val = std::stoull(str, &pos);
        if (pos != str.size() || val == 0)
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>(val);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

[[nodiscard]] internal::ConnectOptions BuildConnectOptions(const config::Config& cfg)
{
    return internal::ConnectOptions{
        .endpoint = cfg.endpoint.scheme + "://" + cfg.endpoint.host + ":" +
                    std::to_string(cfg.endpoint.port),
        .protocol = cfg.protocol,
        .connect_timeout = cfg.timeouts.connect,
        .tls_handshake_timeout = cfg.timeouts.tls_handshake,
        .insecure = cfg.tls.insecure,
        .ca_bundle = cfg.tls.ca_bundle,
        .client_cert = cfg.tls.client_cert,
        .client_key = cfg.tls.client_key,
        .sni_override = cfg.tls.sni_override,
        .max_response_bytes = cfg.memory_limits.max_response_bytes,
        .max_trailer_bytes = cfg.memory_limits.max_trailer_bytes,
    };
}

[[nodiscard]] std::vector<internal::HeaderField> ToHeaderFields(const std::vector<KeyValue>& kvs)
{
    std::vector<internal::HeaderField> out;
    out.reserve(kvs.size());
    for (const auto& kv : kvs)
    {
        const auto* sv = std::get_if<std::string>(&kv.value);
        if (sv != nullptr)
        {
            out.push_back(internal::HeaderField{.name = kv.key, .value = *sv});
        }
    }
    return out;
}

[[nodiscard]] Expected<std::unique_ptr<internal::ITransport>, ConfigError> CreateTransport()
{
    auto reactor = transport::EpollReactor::Create();
    if (!reactor)
    {
        return make_unexpected(
            ConfigError{.kind = ConfigError::Kind::Unspecified,
                        .field = {},
                        .message = "reactor init failed: " + reactor.error().message});
    }
    auto http2 = transport::Http2Transport::Create(std::move(*reactor));
    if (!http2)
    {
        return make_unexpected(
            ConfigError{.kind = ConfigError::Kind::Unspecified,
                        .field = {},
                        .message = "transport init failed: " + http2.error().message});
    }
    return std::move(*http2);
}

constexpr std::string_view kHttpMetricsPath = "/v1/metrics";
constexpr std::string_view kGrpcMetricsPath =
    "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export";
constexpr std::string_view kHttpLogsPath = "/v1/logs";
constexpr std::string_view kGrpcLogsPath =
    "/opentelemetry.proto.collector.logs.v1.LogsService/Export";

/// @brief Construct the wire codec for the configured protocol.
///
/// @param diag the diagnostics sink, which the codecs need rather than merely
///        accept: connect_failure, malformed_response and
///        decompression_too_large are all recorded inside the codec, and
///        passing nullptr here made every one of them invisible to
///        `GetExporterHealth()`.
[[nodiscard]] std::unique_ptr<internal::IWireCodec> BuildWireCodec(
    internal::ITransport* transport,
    const config::Config& cfg,
    std::vector<internal::HeaderField> extra_headers,
    internal::IAuthProvider* auth,
    internal::IDiagnosticsSink* diag,
    std::string_view signal_path = {})
{
    const auto host_port = cfg.endpoint.host + ":" + std::to_string(cfg.endpoint.port);
    if (cfg.protocol == Protocol::Grpc)
    {
        return std::make_unique<wire::GrpcWireCodec>(
            transport,
            wire::GrpcWireCodecConfig{
                .host = host_port,
                .scheme = cfg.endpoint.scheme,
                .extra_headers = std::move(extra_headers),
                .service_path = std::string{signal_path},
                .compression_gzip = cfg.compression_gzip,
                .max_decompressed_bytes = cfg.memory_limits.max_decompressed_bytes,
            },
            auth,
            diag,
            /*clock=*/nullptr,
            BuildConnectOptions(cfg));
    }
    return std::make_unique<wire::HttpWireCodec>(
        transport,
        wire::HttpWireCodecConfig{
            .host = host_port,
            .scheme = cfg.endpoint.scheme,
            .path = cfg.endpoint.path,
            .signal_path = std::string{signal_path},
            .extra_headers = std::move(extra_headers),
            .compression_gzip = cfg.compression_gzip,
            .max_decompressed_bytes = cfg.memory_limits.max_decompressed_bytes,
        },
        auth,
        diag,
        /*clock=*/nullptr,
        BuildConnectOptions(cfg));
}

[[nodiscard]] sdk::ViewRegistry BuildViewRegistry(std::vector<ViewConfig>& views)
{
    sdk::ViewRegistry reg;
    for (auto& v : views)
    {
        reg.Add(std::move(v));
    }
    return reg;
}

/// @brief Resolve the effective per-instrument cardinality cap.
///
/// Precedence (lowest to highest): SDK default → env var
/// `MICROTEL_METRIC_CARDINALITY_LIMIT` → `WithMetricLimits()`.
[[nodiscard]] std::size_t ResolveMaxCardinality(
    const std::optional<MetricLimitOptions>& explicit_limits) noexcept
{
    std::size_t cap = sdk::kDefaultMaxCardinality;
    const auto env_card = ParseEnvCardinality();
    if (env_card.has_value())
    {
        cap = *env_card;
    }
    if (explicit_limits.has_value())
    {
        cap = explicit_limits->max_cardinality;
    }
    return cap;
}

struct ExporterPack
{
    std::unique_ptr<internal::IWireCodec> codec;
    std::unique_ptr<internal::IWireCodec> metric_codec;
    std::unique_ptr<internal::IWireCodec> log_codec;
    std::unique_ptr<internal::IExporter> exporter;
    std::unique_ptr<internal::IMetricExporter> metric_exporter;
    std::unique_ptr<internal::ILogExporter> log_exporter;
};

/// @brief Build the auth provider, or nullptr when no callback was supplied.
[[nodiscard]] std::unique_ptr<internal::IAuthProvider> BuildAuthProvider(
    std::optional<AuthCallback>& cb, std::chrono::milliseconds cache_ttl)
{
    if (!cb)
    {
        return nullptr;
    }
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    return std::make_unique<config::CallbackAuthProvider>(std::move(*cb), cache_ttl);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

[[nodiscard]] ExporterPack BuildExporters(wire::OtlpEncoder* encoder,
                                          internal::ITransport* transport,
                                          internal::IAuthProvider* auth,
                                          const config::Config& cfg,
                                          internal::IDiagnosticsSink* diag)
{
    auto codec = BuildWireCodec(transport, cfg, ToHeaderFields(cfg.headers), auth, diag);
    const std::string_view metric_path =
        cfg.protocol == Protocol::Grpc ? kGrpcMetricsPath : kHttpMetricsPath;
    auto metric_codec =
        BuildWireCodec(transport, cfg, ToHeaderFields(cfg.headers), auth, diag, metric_path);
    const std::string_view log_path =
        cfg.protocol == Protocol::Grpc ? kGrpcLogsPath : kHttpLogsPath;
    auto log_codec =
        BuildWireCodec(transport, cfg, ToHeaderFields(cfg.headers), auth, diag, log_path);

    // `retry_budget` is the only retry axis TimeoutOptions exposes; the rest of
    // RetryPolicyConfig (attempts, backoff shape, jitter) has no config surface
    // and keeps its OTLP-recommended in-class defaults.
    const exporter::OtlpExporterConfig ex_cfg{
        .export_deadline = cfg.timeouts.per_export,
        .retry_policy = {.retry_budget = cfg.timeouts.retry_budget},
    };
    auto trace_exp = std::make_unique<exporter::OtlpExporter>(encoder, codec.get(), ex_cfg, diag);
    // One sink across all three signals: batches_sent / batches_failed are
    // therefore cross-signal aggregates (see docs/error-model.md §3).
    auto metric_exp = std::make_unique<exporter::OtlpMetricExporter>(
        encoder,
        metric_codec.get(),
        exporter::OtlpMetricExporterConfig{.export_deadline = cfg.timeouts.per_export},
        diag);
    auto log_exp = std::make_unique<exporter::OtlpLogExporter>(
        encoder,
        log_codec.get(),
        exporter::OtlpLogExporterConfig{.export_deadline = cfg.timeouts.per_export},
        diag);

    return ExporterPack{
        .codec = std::move(codec),
        .metric_codec = std::move(metric_codec),
        .log_codec = std::move(log_codec),
        .exporter = std::move(trace_exp),
        .metric_exporter = std::move(metric_exp),
        .log_exporter = std::move(log_exp),
    };
}

/// @brief Build the span processor from the resolved configuration.
///
/// No scope is passed: the processor stamps each batch with the scope that
/// arrived with the span, i.e. the one `GetTracer` was called with (ICP 0023).
/// The service identity reaches the wire through the Resource.
///
/// @param exporter non-owning; must outlive the processor.
/// @param resource shared with every batch the processor emits.
/// @param cfg borrowed; read for the batch options and the two memory limits
///        the processor enforces (`max_record_bytes`, `max_total_queue_bytes`).
/// @param diag non-owning diagnostics sink.
[[nodiscard]] std::unique_ptr<sdk::BatchSpanProcessor> BuildSpanProcessor(
    internal::IExporter* exporter,
    std::shared_ptr<const Resource> resource,
    const config::Config& cfg,
    internal::IDiagnosticsSink* diag)
{
    return std::make_unique<sdk::BatchSpanProcessor>(exporter,
                                                     std::move(resource),
                                                     cfg.batch,
                                                     cfg.memory_limits.max_record_bytes,
                                                     cfg.memory_limits.max_total_queue_bytes,
                                                     diag);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl — config loading and code overrides
// ---------------------------------------------------------------------------

Expected<config::Config, ConfigError> SdkBuilder::Impl::LoadConfig() const
{
    config::Config cfg;
    if (file_path)
    {
        auto file_cfg = config::LoadToml(*file_path);
        if (!file_cfg)
        {
            return make_unexpected(file_cfg.error());
        }
        cfg = std::move(*file_cfg);
    }
    if (auto r = config::OverlayEnv(cfg); !r)
    {
        return make_unexpected(r.error());
    }
    ApplyExporterOverrides(cfg);
    ApplyResourceOverrides(cfg);
    if (auto r = config::Validate(cfg); !r)
    {
        return make_unexpected(r.error());
    }
    return cfg;
}

void SdkBuilder::Impl::ApplyExporterOverrides(config::Config& cfg) const
{
    if (endpoint)
    {
        cfg.endpoint_url = *endpoint;
    }
    if (protocol)
    {
        cfg.protocol = *protocol;
        cfg.protocol_explicit = true;
    }
    if (compression_gzip)
    {
        cfg.compression_gzip = *compression_gzip;
    }
    if (headers)
    {
        cfg.headers = *headers;
    }
    if (tls)
    {
        cfg.tls = *tls;
    }
    if (metric_interval)
    {
        cfg.metric_interval = *metric_interval;
    }
    if (metric_temporality)
    {
        cfg.metric_temporality = *metric_temporality;
    }
}

void SdkBuilder::Impl::ApplyResourceOverrides(config::Config& cfg) const
{
    if (service_name)
    {
        cfg.service_name = *service_name;
    }
    if (service_version)
    {
        cfg.service_version = *service_version;
    }
    if (resource_attrs)
    {
        cfg.resource_attrs = *resource_attrs;
    }
    if (batch)
    {
        cfg.batch = *batch;
    }
    if (span_limits)
    {
        cfg.span_limits = *span_limits;
    }
    if (memory_limits)
    {
        cfg.memory_limits = *memory_limits;
    }
    if (timeouts)
    {
        cfg.timeouts = *timeouts;
    }
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

Expected<std::shared_ptr<Provider>, ConfigError> SdkBuilder::Build()
{
    if (m_impl->consumed)
    {
        return make_unexpected(ConfigError{.kind = ConfigError::Kind::BuildAlreadyConsumed,
                                           .field = {},
                                           .message = "SdkBuilder::Build() called more than once"});
    }
    m_impl->consumed = true;

    // --- Steps 1–2: assemble and validate Config (file → env → code) -------
    auto cfg_result = m_impl->LoadConfig();
    if (!cfg_result)
    {
        return make_unexpected(cfg_result.error());
    }
    const config::Config cfg = std::move(*cfg_result);
    // Seeded before the first thing that logs. The knob is process-global
    // (ICP 0026 §6), so this is where `logging.level` / `MICROTEL_LOG_LEVEL`
    // reach the filter; `Provider::SetLogLevel` retunes it afterwards. The
    // return is discarded rather than checked because `config::ParseLogLevel`
    // only ever produces declared enumerators.
    (void)internal::SetMinLogLevel(cfg.log_level);
    WarnOnRiskyConfig(cfg);

    // --- Step 3: resource (spec §12.7 — defaults, detectors, env, user) -----
    auto resource_result = sdk::BuildResource(cfg, m_impl->resource_detectors);
    if (!resource_result)
    {
        return make_unexpected(resource_result.error());
    }
    auto resource = std::make_shared<const Resource>(std::move(*resource_result));

    // --- Step 4: auth provider ----------------------------------------------
    auto auth = BuildAuthProvider(m_impl->auth_cb, m_impl->auth_cache_ttl);

    // --- Steps 5–6: transport -----------------------------------------------
    auto transport_result = CreateTransport();
    if (!transport_result)
    {
        return make_unexpected(transport_result.error());
    }
    auto transport = std::move(*transport_result);

    // --- Steps 7–9: encoder + codecs + exporters ----------------------------
    auto encoder = std::make_unique<wire::OtlpEncoder>();
    // Created before the exporters because they borrow it; ownership moves
    // into the Provider below, which declares it first and so destroys it last.
    auto diagnostics = std::make_unique<sdk::DiagnosticsCounters>();
    auto exporters =
        BuildExporters(encoder.get(), transport.get(), auth.get(), cfg, diagnostics.get());

    // --- Step 10: processor -------------------------------------------------
    auto processor = BuildSpanProcessor(exporters.exporter.get(), resource, cfg, diagnostics.get());
    // Borrowed here, where the concrete type is still known: the Deps boundary
    // below erases it to ISpanProcessor, and Provider::SetBatchOptions needs
    // the batching type back (ICP 0026 §4).
    auto* const batch_span_processor = processor.get();

    // --- Step 11: resolve cardinality cap and build view registry ------------
    const std::size_t max_cardinality = ResolveMaxCardinality(m_impl->metric_limits);
    return std::make_shared<sdk::SdkProvider>(sdk::SdkProviderArgs{
        .diagnostics = std::move(diagnostics),
        .encoder = std::move(encoder),
        .auth = std::move(auth),
        .transport = std::move(transport),
        .codec = std::move(exporters.codec),
        .exporter = std::move(exporters.exporter),
        .processor = std::move(processor),
        .batch_span_processor = batch_span_processor,
        .resource = std::move(resource),
        .sampler = std::move(m_impl->sampler),
        .span_limits = cfg.span_limits,
        .connect_opts = BuildConnectOptions(cfg),
        .metric_codec = std::move(exporters.metric_codec),
        .metric_exporter = std::move(exporters.metric_exporter),
        .metric_interval = cfg.metric_interval,
        .metric_temporality = cfg.metric_temporality,
        .metric_max_cardinality = max_cardinality,
        .view_registry = BuildViewRegistry(m_impl->views),
        .log_codec = std::move(exporters.log_codec),
        .log_exporter = std::move(exporters.log_exporter),
        .log_batch_opts = cfg.batch,
    });
}

}  // namespace microtel
