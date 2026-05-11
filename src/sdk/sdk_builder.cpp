// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

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

}  // namespace

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

Expected<std::shared_ptr<Provider>, ConfigError> SdkBuilder::Build()
{
    if (m_impl->consumed)
    {
        return Unexpected{ConfigError{.kind = ConfigError::Kind::BuildAlreadyConsumed,
                                      .field = {},
                                      .message = "SdkBuilder::Build() called more than once"}};
    }
    m_impl->consumed = true;

    // --- Step 1: assemble Config (file → env → code) -----------------------
    config::Config cfg;

    if (m_impl->file_path)
    {
        auto file_cfg = config::LoadToml(*m_impl->file_path);
        if (!file_cfg)
        {
            return Unexpected{file_cfg.error()};
        }
        cfg = std::move(*file_cfg);
    }

    if (auto r = config::OverlayEnv(cfg); !r)
    {
        return Unexpected{r.error()};
    }

    if (m_impl->endpoint)
    {
        cfg.endpoint_url = *m_impl->endpoint;
    }
    if (m_impl->protocol)
    {
        cfg.protocol = *m_impl->protocol;
    }
    if (m_impl->compression_gzip)
    {
        cfg.compression_gzip = *m_impl->compression_gzip;
    }
    if (m_impl->headers)
    {
        cfg.headers = *m_impl->headers;
    }
    if (m_impl->service_name)
    {
        cfg.service_name = *m_impl->service_name;
    }
    if (m_impl->service_version)
    {
        cfg.service_version = *m_impl->service_version;
    }
    if (m_impl->resource_attrs)
    {
        cfg.resource_attrs = *m_impl->resource_attrs;
    }
    if (m_impl->batch)
    {
        cfg.batch = *m_impl->batch;
    }
    if (m_impl->span_limits)
    {
        cfg.span_limits = *m_impl->span_limits;
    }
    if (m_impl->memory_limits)
    {
        cfg.memory_limits = *m_impl->memory_limits;
    }
    if (m_impl->timeouts)
    {
        cfg.timeouts = *m_impl->timeouts;
    }
    if (m_impl->tls)
    {
        cfg.tls = *m_impl->tls;
    }

    // --- Step 2: validate ---------------------------------------------------
    if (auto r = config::Validate(cfg); !r)
    {
        return Unexpected{r.error()};
    }

    // --- Step 3: resource ---------------------------------------------------
    auto resource = BuildResource(cfg);

    // --- Step 4: auth provider ----------------------------------------------
    std::unique_ptr<internal::IAuthProvider> auth;
    if (m_impl->auth_cb)
    {
        auth = std::make_unique<config::CallbackAuthProvider>(std::move(*m_impl->auth_cb),
                                                              m_impl->auth_cache_ttl);
    }

    // --- Step 5: transport --------------------------------------------------
    auto reactor = transport::EpollReactor::Create();
    if (!reactor)
    {
        return Unexpected{
            ConfigError{.kind = ConfigError::Kind::Unspecified,
                        .field = {},
                        .message = "reactor init failed: " + reactor.error().message}};
    }

    auto http2 = transport::Http2Transport::Create(std::move(*reactor));
    if (!http2)
    {
        return Unexpected{
            ConfigError{.kind = ConfigError::Kind::Unspecified,
                        .field = {},
                        .message = "transport init failed: " + http2.error().message}};
    }

    // --- Step 6: encoder ----------------------------------------------------
    auto encoder = std::make_unique<wire::OtlpEncoder>();

    // --- Step 7: wire codec -------------------------------------------------
    const auto host_port = cfg.endpoint.host + ":" + std::to_string(cfg.endpoint.port);
    auto extra_headers = ToHeaderFields(cfg.headers);
    internal::ITransport* const transport_ptr = (*http2).get();

    std::unique_ptr<internal::IWireCodec> codec;
    if (cfg.protocol == Protocol::Grpc)
    {
        codec = std::make_unique<wire::GrpcWireCodec>(transport_ptr,
                                                      wire::GrpcWireCodecConfig{
                                                          .host = host_port,
                                                          .scheme = cfg.endpoint.scheme,
                                                          .extra_headers = std::move(extra_headers),
                                                      },
                                                      auth.get());
    }
    else
    {
        codec = std::make_unique<wire::HttpWireCodec>(transport_ptr,
                                                      wire::HttpWireCodecConfig{
                                                          .host = host_port,
                                                          .scheme = cfg.endpoint.scheme,
                                                          .path = cfg.endpoint.path,
                                                          .extra_headers = std::move(extra_headers),
                                                      },
                                                      auth.get());
    }

    // --- Step 8: exporter ---------------------------------------------------
    exporter::OtlpExporterConfig exporter_cfg{
        .export_deadline = cfg.timeouts.per_export,
    };
    auto exporter_obj =
        std::make_unique<exporter::OtlpExporter>(encoder.get(), codec.get(), exporter_cfg);

    // --- Step 9: processor --------------------------------------------------
    internal::InstrumentationScope scope{
        .name = cfg.service_name,
        .version = cfg.service_version,
    };
    auto processor = std::make_unique<sdk::BatchSpanProcessor>(
        exporter_obj.get(), resource, std::move(scope), cfg.batch);

    // --- Step 10: provider --------------------------------------------------
    auto connect_opts = BuildConnectOptions(cfg);

    return std::make_shared<sdk::SdkProvider>(std::move(encoder),
                                              std::move(auth),
                                              std::move(*http2),
                                              std::move(codec),
                                              std::move(exporter_obj),
                                              std::move(processor),
                                              std::move(resource),
                                              std::move(m_impl->sampler),
                                              cfg.span_limits,
                                              std::move(connect_opts));
}

}  // namespace microtel
