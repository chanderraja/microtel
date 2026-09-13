// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/config_validator.hpp"

#include "microtel/error.hpp"
#include "microtel/protocol.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace microtel::config
{

namespace
{

// ---------------------------------------------------------------------------
// Named constants
// ---------------------------------------------------------------------------

constexpr std::string_view kSchemeHttps = "https";
constexpr std::string_view kSchemeHttp = "http";
constexpr std::string_view kSchemeGrpc = "grpc";
constexpr std::string_view kSchemeGrpcs = "grpcs";
constexpr std::string_view kSchemeSep = "://";

constexpr std::uint16_t kDefaultPortGrpc = 4317;
constexpr std::uint16_t kDefaultPortHttp = 4318;
constexpr std::uint16_t kPortMax = 65535;

// ---------------------------------------------------------------------------
// URL parsing helpers
// ---------------------------------------------------------------------------

/// Extract `scheme` from "scheme://rest". Returns empty on failure.
[[nodiscard]] std::string_view ExtractScheme(std::string_view url)
{
    const auto sep = url.find(kSchemeSep);
    if (sep == std::string_view::npos)
    {
        return {};
    }
    return url.substr(0, sep);
}

/// Remove "scheme://" prefix. `url` must already have the prefix.
[[nodiscard]] std::string_view StripScheme(std::string_view url)
{
    const auto sep = url.find(kSchemeSep);
    return url.substr(sep + kSchemeSep.size());
}

/// Split "host:port/path" → host, port string, path. Port is empty if absent.
struct AuthorityPath
{
    std::string_view host;
    std::string_view port_str;
    std::string_view path;
};

[[nodiscard]] AuthorityPath SplitAuthorityPath(std::string_view authority_and_path)
{
    AuthorityPath result;

    // Separate path from authority.
    const auto slash = authority_and_path.find('/');
    const std::string_view authority = (slash == std::string_view::npos)
                                           ? authority_and_path
                                           : authority_and_path.substr(0, slash);
    result.path =
        (slash == std::string_view::npos) ? std::string_view{} : authority_and_path.substr(slash);

    // Separate host from port.
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos)
    {
        result.host = authority;
    }
    else
    {
        result.host = authority.substr(0, colon);
        result.port_str = authority.substr(colon + 1);
    }
    return result;
}

/// Parse the port component of an endpoint URL.
[[nodiscard]] microtel::Expected<std::uint16_t, ConfigError> ParsePort(std::string_view port_str,
                                                                       Protocol protocol)
{
    if (port_str.empty())
    {
        return (protocol == Protocol::Grpc) ? kDefaultPortGrpc : kDefaultPortHttp;
    }
    std::uint32_t parsed = 0;
    const auto [ptr, ec] =
        std::from_chars(port_str.data(), port_str.data() + port_str.size(), parsed);
    if (ec != std::errc{} || ptr != port_str.data() + port_str.size() || parsed > kPortMax ||
        parsed == 0)
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                                                     .field = "exporter.endpoint",
                                                     .message = "invalid port in endpoint URL"});
    }
    return static_cast<std::uint16_t>(parsed);
}

/// Parse an endpoint URL into ParsedEndpoint components.
/// Does not validate file-system resources.
[[nodiscard]] microtel::Expected<ParsedEndpoint, ConfigError> ParseEndpointUrl(
    const std::string& url, Protocol protocol)
{
    if (url.empty())
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                                                     .field = "exporter.endpoint",
                                                     .message = "endpoint URL is empty"});
    }

    const std::string_view scheme = ExtractScheme(url);
    if (scheme.empty())
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                        .field = "exporter.endpoint",
                        .message = "endpoint URL missing scheme (expected https:// or http://)"});
    }

    // Normalise scheme: grpc → https, grpcs → https, http → http, https → https.
    std::string resolved_scheme;
    if (scheme == kSchemeHttps || scheme == kSchemeGrpcs)
    {
        resolved_scheme = std::string{kSchemeHttps};
    }
    else if (scheme == kSchemeHttp || scheme == kSchemeGrpc)
    {
        resolved_scheme = std::string{kSchemeHttp};
    }
    else
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                        .field = "exporter.endpoint",
                        .message = "unsupported scheme: " + std::string{scheme}});
    }

    const auto ap = SplitAuthorityPath(StripScheme(url));

    if (ap.host.empty())
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                                                     .field = "exporter.endpoint",
                                                     .message = "endpoint URL has empty host"});
    }

    // Parse optional port.
    auto port_result = ParsePort(ap.port_str, protocol);
    if (!port_result)
    {
        return microtel::make_unexpected(port_result.error());
    }
    const std::uint16_t port = *port_result;

    // Strip trailing slash from path so it's a clean base.
    std::string path{ap.path};
    if (path == "/")
    {
        path.clear();
    }

    return ParsedEndpoint{
        .scheme = resolved_scheme,
        .host = std::string{ap.host},
        .port = port,
        .path = path,
    };
}

/// @brief Resolve the effective protocol against the endpoint scheme.
///
/// `grpc://` and `grpcs://` are microtel shorthand for OTLP/gRPC (spec §12.2).
/// The shorthand selects the protocol when the user has not named one; when the
/// user has named `http`, the two disagree and the configuration is rejected
/// rather than silently resolved in either direction — the same rule, and the
/// same `ProtocolMismatch` kind, that the gRPC-path check below applies.
///
/// `https://` and `http://` say nothing about the protocol: `https://` plus an
/// explicit `protocol` is the canonical spelling of a gRPC endpoint, and
/// plaintext h2c gRPC over `http://` is legitimate. Only the two gRPC-named
/// schemes carry an opinion, so only they are consulted here.
///
/// A URL with no recognisable scheme falls through unchanged; `ParseEndpointUrl`
/// is what reports it, and it reports it as `EndpointMalformed`.
[[nodiscard]] microtel::Expected<Protocol, ConfigError> ResolveProtocol(const Config& cfg)
{
    const std::string_view scheme = ExtractScheme(cfg.endpoint_url);
    if (scheme != kSchemeGrpc && scheme != kSchemeGrpcs)
    {
        return cfg.protocol;
    }
    if (cfg.protocol_explicit && cfg.protocol != Protocol::Grpc)
    {
        return microtel::make_unexpected(ConfigError{
            .kind = ConfigError::Kind::ProtocolMismatch,
            .field = "exporter.protocol",
            .message = "endpoint scheme \"" + std::string{scheme} +
                       "://\" selects OTLP/gRPC but protocol is set to \"http\"; use an "
                       "http:// or https:// endpoint, or drop the explicit protocol"});
    }
    return Protocol::Grpc;
}

/// Check that a path-string refers to a readable file.
[[nodiscard]] bool IsReadable(const std::filesystem::path& p)
{
    if (p.empty())
    {
        return true;  // absent means "not configured", which is fine
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) && !ec;
}

/// @brief Reject `insecure = true` when the build forbids it.
///
/// `MICROTEL_FORBID_INSECURE_TLS=ON` compiles the macro into this translation
/// unit (see src/common/config/CMakeLists.txt). Spec §12.3 makes the refusal an
/// initialisation failure, not a warning: a default build only warns, and
/// `SdkBuilder`'s `WarnOnRiskyConfig` owns that half.
///
/// This is the only place in the tree that tests the macro. Everything else
/// calls this function unconditionally, so the OFF build differs from the ON
/// build by the contents of one function body and nothing else.
[[nodiscard]] microtel::Expected<void, ConfigError> CheckInsecureAllowed(
    [[maybe_unused]] const Config& cfg)
{
#ifdef MICROTEL_FORBID_INSECURE_TLS
    if (cfg.tls.insecure)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InsecureDisallowed,
                        .field = "tls.insecure",
                        .message = "tls.insecure = true is refused: this build was compiled "
                                   "with MICROTEL_FORBID_INSECURE_TLS=ON"});
    }
#endif
    return {};
}

/// @brief Check the configured TLS material is coherent and readable.
///
/// Split out of `Validate` to keep both function bodies inside the cognitive
/// complexity budget; it carries no state and is called exactly once.
[[nodiscard]] microtel::Expected<void, ConfigError> ValidateTlsMaterial(const Config& cfg)
{
    if (!IsReadable(cfg.tls.ca_bundle))
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::TlsMaterialUnreadable,
                        .field = "tls.ca_bundle",
                        .message = "CA bundle not readable: " + cfg.tls.ca_bundle.string()});
    }

    const bool has_cert = !cfg.tls.client_cert.empty();
    const bool has_key = !cfg.tls.client_key.empty();

    if (has_cert && !has_key)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InvalidValue,
                        .field = "tls.client_key",
                        .message = "client_cert is set but client_key is missing"});
    }
    if (has_key && !has_cert)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InvalidValue,
                        .field = "tls.client_cert",
                        .message = "client_key is set but client_cert is missing"});
    }
    if (has_cert && !IsReadable(cfg.tls.client_cert))
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::TlsMaterialUnreadable,
                        .field = "tls.client_cert",
                        .message = "client cert not readable: " + cfg.tls.client_cert.string()});
    }
    if (has_key && !IsReadable(cfg.tls.client_key))
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::TlsMaterialUnreadable,
                        .field = "tls.client_key",
                        .message = "client key not readable: " + cfg.tls.client_key.string()});
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

microtel::Expected<void, ConfigError> Validate(Config& cfg)
{
    // --- Protocol (spec §12.2) ---
    // Before the endpoint is parsed: the resolved protocol is what picks the
    // default port, so `grpc://collector` means 4317 and not 4318.
    auto protocol = ResolveProtocol(cfg);
    if (!protocol)
    {
        return microtel::make_unexpected(protocol.error());
    }
    cfg.protocol = *protocol;

    // --- Endpoint URL ---
    auto endpoint = ParseEndpointUrl(cfg.endpoint_url, cfg.protocol);
    if (!endpoint)
    {
        return microtel::make_unexpected(endpoint.error());
    }

    // --- gRPC path rejection (spec §12.2) ---
    if (cfg.protocol == Protocol::Grpc && !endpoint->path.empty())
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::ProtocolMismatch,
                        .field = "exporter.endpoint",
                        .message = "gRPC endpoint URLs must not include a path"});
    }

    cfg.endpoint = std::move(*endpoint);

    // --- TLS ---
    if (auto insecure_ok = CheckInsecureAllowed(cfg); !insecure_ok)
    {
        return microtel::make_unexpected(insecure_ok.error());
    }
    if (auto tls_ok = ValidateTlsMaterial(cfg); !tls_ok)
    {
        return microtel::make_unexpected(tls_ok.error());
    }

    // --- Batch coherence ---
    if (cfg.batch.max_export_batch_size > cfg.batch.max_queue_size)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InvalidValue,
                        .field = "sdk.max_export_batch_size",
                        .message = "max_export_batch_size must not exceed max_queue_size"});
    }

    return {};
}

}  // namespace microtel::config
