// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"
#include "microtel/sdk_builder.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microtel::config
{

/// @brief Parse a `microtel::LogLevel` from its configuration spelling.
///
/// One table shared by the `[logging] level` TOML key and `MICROTEL_LOG_LEVEL`
/// so the two spellings can never drift apart (ICP 0026 §6).
///
/// @param text the configured value, lowercase.
/// @return the level, or `std::nullopt` if `text` names none.
[[nodiscard]] inline std::optional<microtel::LogLevel> ParseLogLevel(std::string_view text) noexcept
{
    if (text == "trace")
    {
        return microtel::LogLevel::Trace;
    }
    if (text == "debug")
    {
        return microtel::LogLevel::Debug;
    }
    if (text == "info")
    {
        return microtel::LogLevel::Info;
    }
    if (text == "warn")
    {
        return microtel::LogLevel::Warn;
    }
    if (text == "error")
    {
        return microtel::LogLevel::Error;
    }
    return std::nullopt;
}


/// @brief Controls treatment of unknown TOML keys (per spec §12 strict mode).
enum class UnknownKeyMode : std::uint8_t
{
    Error = 0,   ///< unknown key is a fatal error (default)
    Warn = 1,    ///< unknown key emits a diagnostic warning
    Ignore = 2,  ///< unknown key is silently ignored
};

/// @brief Parsed components of the endpoint URL.
struct ParsedEndpoint
{
    std::string scheme;    ///< "https" or "http"
    std::string host;      ///< hostname or IP address
    std::uint16_t port{};  ///< port number (0 means use default for protocol)
    std::string path;      ///< base path, no trailing slash (empty if absent)
};

/// @brief Frozen resolved configuration produced by the config layer.
///
/// Populated in three passes — TOML file, env-var overlay, code overrides —
/// then validated and frozen by `Validate()`. Never mutated after that.
///
/// @see src/common/config/README.md
struct Config
{
    UnknownKeyMode unknown_key_mode{UnknownKeyMode::Error};

    // Endpoint / transport
    std::string endpoint_url;  ///< raw URL string (validated and parsed)
    ParsedEndpoint endpoint;   ///< parsed URL components (filled by Validate)
    Protocol protocol{Protocol::Http};

    /// @brief True when `protocol` was named by the user rather than defaulted.
    ///
    /// Set by the TOML `exporter.protocol` key, `OTEL_EXPORTER_OTLP_PROTOCOL`,
    /// and `SdkBuilder::WithProtocol`. `Validate()` reads it to tell "the user
    /// asked for OTLP/HTTP" apart from "nobody said", which is what decides
    /// whether a `grpc://` endpoint scheme may select `Protocol::Grpc` or
    /// conflicts with an explicit choice (issue #203).
    bool protocol_explicit{false};
    bool compression_gzip{false};
    std::vector<KeyValue> headers;  ///< extra request headers from [exporter.headers]

    // Service / resource
    std::string service_name;
    std::string service_version;
    std::vector<KeyValue> resource_attrs;

    /// @brief True when `service_name` holds the `unknown_service` placeholder
    ///        `Validate()` supplies rather than a value someone configured.
    ///
    /// The placeholder is a built-in default, the lowest tier in §12.1, and so
    /// sits *below* a resource detector's contribution in the §12.7 merge
    /// order. The resource builder cannot tell the two apart from the string
    /// alone — a user is free to configure `unknown_service` themselves — so
    /// `Validate()` records which one it is.
    bool service_name_defaulted{false};

    /// @brief Strict resource-detector policy: a failing detector fails `Build()`.
    ///
    /// Default (`false`) is lenient: a detector returning a `ConfigError` is
    /// logged at Warn and skipped, and the rest of the pipeline is built. Set
    /// via `sdk.resource_detectors_strict` in TOML or
    /// `MICROTEL_RESOURCE_DETECTORS_STRICT` in the environment.
    ///
    /// @see docs/interfaces.md §4.10, docs/configuration.md §3.2
    bool resource_detectors_strict{false};

    // Nested option structs (defined in microtel/sdk_builder.hpp)
    TlsOptions tls;
    BatchOptions batch;
    TimeoutOptions timeouts;
    SpanLimitOptions span_limits;
    MemoryLimitOptions memory_limits;

    // Metrics pipeline
    /// @brief Background export interval for `PeriodicExportingMetricReader`.
    ///
    /// Honors `OTEL_METRIC_EXPORT_INTERVAL` (integer milliseconds).
    /// OTel spec default: 60 000 ms.
    std::chrono::milliseconds metric_interval{60'000};

    /// @brief Aggregation temporality preference for the metrics pipeline.
    ///
    /// Honors `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`
    /// ("cumulative" | "delta" | "lowmemory"). OTel default: cumulative.
    microtel::TemporalityPreference metric_temporality{microtel::TemporalityPreference::Cumulative};

    // Internal diagnostic logging
    /// @brief Minimum severity for microtel's own internal logs.
    ///
    /// From the `[logging] level` TOML key or `MICROTEL_LOG_LEVEL`
    /// (`trace` | `debug` | `info` | `warn` | `error`). `Build()` seeds the
    /// process-global filter from it; `Provider::SetLogLevel` retunes it
    /// afterwards. Default `Info` changes nothing observable — every
    /// production emission site logs at `Warn`.
    ///
    /// @see docs/configuration.md §3.11, ICP 0026 §6, issue #190
    microtel::LogLevel log_level{microtel::LogLevel::Info};
};

}  // namespace microtel::config
