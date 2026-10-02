// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/config_validator.hpp"

#include "microtel/error.hpp"
#include "microtel/protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

/// OTel resource semantic conventions: `service.name` is required, and this is
/// the placeholder a producer uses when it has not been told one.
constexpr std::string_view kUnknownService = "unknown_service";

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
/// `grpc://` and `grpcs://` are microtel shorthand for OTLP/gRPC (`docs/configuration.md` §3.3).
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
/// unit (see src/common/config/CMakeLists.txt). `docs/configuration.md` §3.5 makes the refusal an
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

// ---------------------------------------------------------------------------
// Static request header names (ICP 0038)
// ---------------------------------------------------------------------------

/// Forbidden in any HTTP/2 request (RFC 9113 §8.2.2). `te: trailers` is the
/// one legal `te`; the gRPC codec sends it and it means nothing on HTTP.
constexpr std::array<std::string_view, 6> kConnectionSpecificHeaders{
    "connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade", "te"};

/// Set by the gRPC codec (`docs/grpc-wire-protocol.md` §2.1).
constexpr std::array<std::string_view, 4> kGrpcCodecHeaders{
    "content-type", "user-agent", "grpc-encoding", "grpc-accept-encoding"};

/// Set by the HTTP codec.
constexpr std::array<std::string_view, 4> kHttpCodecHeaders{
    "content-type", "content-length", "content-encoding", "accept-encoding"};

/// Not sent on gRPC, whose body length varies per request; a fixed value
/// would contradict every body but one (ICP 0038 amendment).
constexpr std::string_view kContentLengthHeader = "content-length";
constexpr std::string_view kHostHeader = "host";
constexpr std::string_view kAuthorizationHeader = "authorization";
constexpr std::string_view kHeadersFieldPrefix = "exporter.headers.";
/// RFC 9110 §5.6.2 `tchar`, less ALPHA and DIGIT.
constexpr std::string_view kTokenPunctuation = "!#$%&'*+-.^_`|~";

[[nodiscard]] bool IsTokenChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           kTokenPunctuation.find(c) != std::string_view::npos;
}

/// RFC 9110 §5.1 field names are tokens. A pseudo-header's leading `:` is not
/// a token character, so this also rejects every pseudo-header.
[[nodiscard]] bool IsToken(std::string_view name) noexcept
{
    return !name.empty() && std::ranges::all_of(name, IsTokenChar);
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a,
                              b,
                              [](char x, char y)
                              {
                                  return std::tolower(static_cast<unsigned char>(x)) ==
                                         std::tolower(static_cast<unsigned char>(y));
                              });
}

template <std::size_t N>
[[nodiscard]] bool IsOneOf(std::string_view name,
                           const std::array<std::string_view, N>& set) noexcept
{
    return std::ranges::any_of(set,
                               [name](std::string_view s) { return EqualsIgnoreCase(name, s); });
}

/// The protocol-specific half of `HeaderNameFault`.
[[nodiscard]] std::optional<std::string_view> ProtocolHeaderFault(std::string_view name,
                                                                  Protocol protocol) noexcept
{
    if (protocol == Protocol::Http)
    {
        if (IsOneOf(name, kHttpCodecHeaders))
        {
            return "is set by microtel for this protocol";
        }
        return std::nullopt;
    }
    if (IsOneOf(name, kGrpcCodecHeaders))
    {
        return "is set by microtel for this protocol";
    }
    if (EqualsIgnoreCase(name, kContentLengthHeader))
    {
        return "cannot be fixed on gRPC, where every request body has its own length";
    }
    return std::nullopt;
}

/// @return Why @p name cannot be a static header under @p protocol, or
///         `std::nullopt` if it can.
[[nodiscard]] std::optional<std::string_view> HeaderNameFault(std::string_view name,
                                                              Protocol protocol) noexcept
{
    if (!IsToken(name))
    {
        return "is not a valid header name (RFC 9110 token); pseudo-headers are set by microtel";
    }
    if (IsOneOf(name, kConnectionSpecificHeaders))
    {
        return "is connection-specific, which HTTP/2 forbids in a request (RFC 9113 §8.2.2)";
    }
    if (EqualsIgnoreCase(name, kHostHeader))
    {
        return "is set by microtel from the endpoint, as :authority";
    }
    return ProtocolHeaderFault(name, protocol);
}

/// The error for header @p name; the value is never included (it may be a
/// secret).
[[nodiscard]] ConfigError HeaderError(const std::string& name, std::string_view reason)
{
    return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                       .field = std::string{kHeadersFieldPrefix} + name,
                       .message = "header \"" + name + "\" " + std::string{reason}};
}

[[nodiscard]] microtel::Expected<void, ConfigError> ValidateHeaderNames(const Config& cfg)
{
    for (const auto& header : cfg.headers)
    {
        if (const auto fault = HeaderNameFault(header.key, cfg.protocol))
        {
            return microtel::make_unexpected(HeaderError(header.key, *fault));
        }
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::optional<BatchOptionsFault> CheckBatchOptions(const BatchOptions& opts) noexcept
{
    if (opts.max_queue_size == 0U)
    {
        return BatchOptionsFault{.field = "sdk.max_queue_size",
                                 .message = "max_queue_size must be greater than zero"};
    }
    if (opts.max_export_batch_size == 0U)
    {
        return BatchOptionsFault{.field = "sdk.max_export_batch_size",
                                 .message = "max_export_batch_size must be greater than zero"};
    }
    if (opts.max_export_batch_size > opts.max_queue_size)
    {
        return BatchOptionsFault{.field = "sdk.max_export_batch_size",
                                 .message = "max_export_batch_size must not exceed max_queue_size"};
    }
    if (opts.schedule_delay.count() <= 0)
    {
        return BatchOptionsFault{.field = "sdk.schedule_delay_ms",
                                 .message = "schedule_delay must be greater than zero"};
    }
    return std::nullopt;
}

microtel::Expected<void, ConfigError> CheckNoStaticAuthorization(
    const std::vector<KeyValue>& headers)
{
    for (const auto& header : headers)
    {
        if (EqualsIgnoreCase(header.key, kAuthorizationHeader))
        {
            return microtel::make_unexpected(HeaderError(
                header.key, "is set by the WithAuthProvider callback; set one or the other"));
        }
    }
    return {};
}

microtel::Expected<void, ConfigError> Validate(Config& cfg)
{
    // --- Protocol (docs/configuration.md §3.3) ---
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

    // --- gRPC path rejection (docs/configuration.md §3.3) ---
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

    // --- Static header names (ICP 0038): after the protocol is resolved,
    // because the names the codec sets depend on it ---
    if (auto headers_ok = ValidateHeaderNames(cfg); !headers_ok)
    {
        return microtel::make_unexpected(headers_ok.error());
    }

    // --- Batch coherence (the same rules SetBatchOptions applies, #267) ---
    if (const auto fault = CheckBatchOptions(cfg.batch))
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::InvalidValue,
                                                     .field = std::string{fault->field},
                                                     .message = std::string{fault->message}});
    }

    // --- Service identity ---
    // `service.name` is required by the OTel resource semantic conventions;
    // resolving the placeholder here rather than at resource-assembly time
    // keeps one owner for the default and leaves `Config` a complete record of
    // what the pipeline will actually report.
    // `service_name_defaulted` records which of the two it is, because the
    // resource merge order (docs/configuration.md §3.2) puts a built-in default below a detector's
    // contribution and a configured name above it.
    cfg.service_name_defaulted = cfg.service_name.empty();
    if (cfg.service_name_defaulted)
    {
        cfg.service_name = std::string{kUnknownService};
    }

    return {};
}

}  // namespace microtel::config
