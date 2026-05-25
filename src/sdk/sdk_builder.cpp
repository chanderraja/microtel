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
#include "microtel/protocol.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"

#include "common/config/auth_providers.hpp"
#include "common/config/config.hpp"
#include "common/config/config_validator.hpp"
#include "common/config/env_resolver.hpp"
#include "common/config/toml_loader.hpp"
#include "exporter/otlp_exporter.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/sdk_provider.hpp"
#include "transport/epoll_reactor.hpp"
#include "transport/http2_transport.hpp"
#include "wire/encoder/otlp_encoder.hpp"
#include "wire/grpc/grpc_wire_codec.hpp"
#include "wire/http/http_wire_codec.hpp"

#include <chrono>
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
    SamplerHandle sampler;
    std::optional<BatchOptions> batch;
    std::optional<SpanLimitOptions> span_limits;
    std::optional<MemoryLimitOptions> memory_limits;
    std::optional<TimeoutOptions> timeouts;
    std::optional<TlsOptions> tls;
    std::optional<AuthCallback> auth_cb;
    std::chrono::milliseconds auth_cache_ttl{std::chrono::seconds(60)};

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

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------

namespace
{

[[nodiscard]] std::shared_ptr<const Resource> BuildResource(const config::Config& cfg)
{
    std::vector<KeyValue> attrs;
    if (!cfg.service_name.empty())
    {
        attrs.push_back({.key = "service.name", .value = cfg.service_name});
    }
    if (!cfg.service_version.empty())
    {
        attrs.push_back({.key = "service.version", .value = cfg.service_version});
    }
    for (const auto& kv : cfg.resource_attrs)
    {
        attrs.push_back(kv);
    }
    return std::make_shared<const Resource>(std::move(attrs));
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

[[nodiscard]] std::unique_ptr<internal::IWireCodec> BuildWireCodec(
    internal::ITransport* transport,
    const config::Config& cfg,
    std::vector<internal::HeaderField> extra_headers,
    internal::IAuthProvider* auth)
{
    const auto host_port = cfg.endpoint.host + ":" + std::to_string(cfg.endpoint.port);
    if (cfg.protocol == Protocol::Grpc)
    {
        return std::make_unique<wire::GrpcWireCodec>(transport,
                                                     wire::GrpcWireCodecConfig{
                                                         .host = host_port,
                                                         .scheme = cfg.endpoint.scheme,
                                                         .extra_headers = std::move(extra_headers),
                                                     },
                                                     auth);
    }
    return std::make_unique<wire::HttpWireCodec>(transport,
                                                 wire::HttpWireCodecConfig{
                                                     .host = host_port,
                                                     .scheme = cfg.endpoint.scheme,
                                                     .path = cfg.endpoint.path,
                                                     .extra_headers = std::move(extra_headers),
                                                 },
                                                 auth);
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

    // --- Step 3: resource ---------------------------------------------------
    auto resource = BuildResource(cfg);

    // --- Step 4: auth provider ----------------------------------------------
    std::unique_ptr<internal::IAuthProvider> auth;
    if (m_impl->auth_cb)
    {
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        auth = std::make_unique<config::CallbackAuthProvider>(std::move(*m_impl->auth_cb),
                                                              m_impl->auth_cache_ttl);
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    // --- Steps 5–6: transport -----------------------------------------------
    auto transport_result = CreateTransport();
    if (!transport_result)
    {
        return make_unexpected(transport_result.error());
    }
    auto transport = std::move(*transport_result);

    // --- Step 7: encoder ----------------------------------------------------
    auto encoder = std::make_unique<wire::OtlpEncoder>();

    // --- Step 8: wire codec -------------------------------------------------
    auto codec = BuildWireCodec(transport.get(), cfg, ToHeaderFields(cfg.headers), auth.get());

    // --- Step 9: exporter ---------------------------------------------------
    const exporter::OtlpExporterConfig exporter_cfg{
        .export_deadline = cfg.timeouts.per_export,
    };
    auto exporter_obj =
        std::make_unique<exporter::OtlpExporter>(encoder.get(), codec.get(), exporter_cfg);

    // --- Step 10: processor -------------------------------------------------
    internal::InstrumentationScope scope{
        .name = cfg.service_name,
        .version = cfg.service_version,
    };
    auto processor = std::make_unique<sdk::BatchSpanProcessor>(
        exporter_obj.get(), resource, std::move(scope), cfg.batch);

    // --- Step 11: provider --------------------------------------------------
    return std::make_shared<sdk::SdkProvider>(sdk::SdkProviderArgs{
        .encoder = std::move(encoder),
        .auth = std::move(auth),
        .transport = std::move(transport),
        .codec = std::move(codec),
        .exporter = std::move(exporter_obj),
        .processor = std::move(processor),
        .resource = std::move(resource),
        .sampler = std::move(m_impl->sampler),
        .span_limits = cfg.span_limits,
        .connect_opts = BuildConnectOptions(cfg),
    });
}

}  // namespace microtel
