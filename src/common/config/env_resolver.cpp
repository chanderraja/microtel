// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/env_resolver.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/protocol.hpp"

#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace microtel::config
{

namespace
{

/// Return the value of an env var, or empty string if unset / empty.
[[nodiscard]] std::string GetEnv(const char* name) noexcept
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read-only at config load time
    const char* const val = ::getenv(name);
    return (val != nullptr) ? std::string{val} : std::string{};
}

/// Parse an integer millisecond value from a string.
[[nodiscard]] microtel::Expected<std::chrono::milliseconds, ConfigError> ParseMs(
    std::string_view sv, const char* var_name)
{
    std::int64_t ms = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), ms);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                        .field = var_name,
                        .message = std::string{var_name} + ": expected integer milliseconds"});
    }
    return std::chrono::milliseconds{ms};
}

/// Parse a "key=value,key2=value2" list into KeyValue pairs.
///
/// Each token must contain exactly one '='. Empty tokens are skipped.
[[nodiscard]] microtel::Expected<std::vector<KeyValue>, ConfigError> ParseKeyValueList(
    std::string_view sv, const char* var_name)
{
    std::vector<KeyValue> result;
    while (!sv.empty())
    {
        const auto comma = sv.find(',');
        const std::string_view token = sv.substr(0, comma);
        if (comma == std::string_view::npos)
        {
            sv = {};
        }
        else
        {
            sv = sv.substr(comma + 1);
        }
        if (token.empty())
        {
            continue;
        }
        const auto eq = token.find('=');
        if (eq == std::string_view::npos)
        {
            return microtel::make_unexpected(ConfigError{
                .kind = ConfigError::Kind::EnvParseFailure,
                .field = var_name,
                .message = std::string{var_name} +
                           ": malformed key=value pair (missing '='): " + std::string{token}});
        }
        result.push_back(
            {.key = std::string{token.substr(0, eq)}, .value = std::string{token.substr(eq + 1)}});
    }
    return result;
}

/// Parse OTEL_EXPORTER_OTLP_PROTOCOL → Protocol.
[[nodiscard]] microtel::Expected<Protocol, ConfigError> ParseProtocol(std::string_view sv)
{
    if (sv == "grpc")
    {
        return Protocol::Grpc;
    }
    if (sv == "http/protobuf" || sv == "http")
    {
        return Protocol::Http;
    }
    return microtel::make_unexpected(
        ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                    .field = "OTEL_EXPORTER_OTLP_PROTOCOL",
                    .message = "expected \"grpc\" or \"http/protobuf\""});
}

}  // namespace

microtel::Expected<void, ConfigError> OverlayEnv(Config& cfg)
{
    // OTEL_EXPORTER_OTLP_ENDPOINT
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_ENDPOINT"); !v.empty())
    {
        cfg.endpoint_url = v;
    }

    // OTEL_EXPORTER_OTLP_PROTOCOL
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_PROTOCOL"); !v.empty())
    {
        auto proto = ParseProtocol(v);
        if (!proto)
        {
            return microtel::make_unexpected(proto.error());
        }
        cfg.protocol = *proto;
    }

    // OTEL_EXPORTER_OTLP_HEADERS
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_HEADERS"); !v.empty())
    {
        auto headers = ParseKeyValueList(v, "OTEL_EXPORTER_OTLP_HEADERS");
        if (!headers)
        {
            return microtel::make_unexpected(headers.error());
        }
        cfg.headers = std::move(*headers);
    }

    // OTEL_EXPORTER_OTLP_TIMEOUT (integer milliseconds)
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_TIMEOUT"); !v.empty())
    {
        auto ms = ParseMs(v, "OTEL_EXPORTER_OTLP_TIMEOUT");
        if (!ms)
        {
            return microtel::make_unexpected(ms.error());
        }
        cfg.timeouts.per_export = *ms;
    }

    // OTEL_EXPORTER_OTLP_COMPRESSION
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_COMPRESSION"); !v.empty())
    {
        cfg.compression_gzip = (v == "gzip");
    }

    // OTEL_EXPORTER_OTLP_CERTIFICATE → ca_bundle path
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_CERTIFICATE"); !v.empty())
    {
        cfg.tls.ca_bundle = v;
    }

    // OTEL_SERVICE_NAME
    if (const auto v = GetEnv("OTEL_SERVICE_NAME"); !v.empty())
    {
        cfg.service_name = v;
    }

    // OTEL_RESOURCE_ATTRIBUTES
    if (const auto v = GetEnv("OTEL_RESOURCE_ATTRIBUTES"); !v.empty())
    {
        auto attrs = ParseKeyValueList(v, "OTEL_RESOURCE_ATTRIBUTES");
        if (!attrs)
        {
            return microtel::make_unexpected(attrs.error());
        }
        cfg.resource_attrs = std::move(*attrs);
    }

    return {};
}

}  // namespace microtel::config
