// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/toml_loader.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/sdk_builder.hpp"

// NOLINTNEXTLINE(misc-include-cleaner) — toml++ single-header
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <toml++/toml.hpp>

namespace microtel::config
{

namespace
{

// ---------------------------------------------------------------------------
// Named constants
// ---------------------------------------------------------------------------

constexpr std::string_view kValGrpc = "grpc";
constexpr std::string_view kValHttp = "http";
constexpr std::string_view kValGzip = "gzip";
constexpr std::string_view kValNewest = "newest";
constexpr std::string_view kValOldest = "oldest";
constexpr std::string_view kValError = "error";
constexpr std::string_view kValWarn = "warn";
constexpr std::string_view kValIgnore = "ignore";

// ---------------------------------------------------------------------------
// Unknown-key helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string FindUnknownKey(const toml::table& tbl,
                                         std::initializer_list<std::string_view> known)
{
    for (const auto& [key, val] : tbl)
    {
        const auto* const it = std::ranges::find(known, std::string_view{key});
        if (it == known.end())
        {
            return std::string{key};
        }
    }
    return {};
}

[[nodiscard]] std::optional<ConfigError> CheckUnknown(const toml::table& tbl,
                                                      std::string_view section,
                                                      std::initializer_list<std::string_view> known,
                                                      UnknownKeyMode mode)
{
    if (mode == UnknownKeyMode::Ignore)
    {
        return std::nullopt;
    }
    const std::string key = FindUnknownKey(tbl, known);
    if (key.empty())
    {
        return std::nullopt;
    }
    if (mode == UnknownKeyMode::Warn)
    {
        return std::nullopt;  // warn path: caller logs; no error returned
    }
    const std::string field = section.empty() ? key : (std::string{section} + "." + key);
    return ConfigError{.kind = ConfigError::Kind::UnknownKey,
                       .field = field,
                       .message = "Unknown configuration key: " + field};
}

// ---------------------------------------------------------------------------
// Section parsers — each returns the first error or nullopt
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<ConfigError> ParseConfigSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["config"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    // unknown_keys is the only field; mode not yet applied (it IS the mode).
    if (const auto v = (*sec)["unknown_keys"].value<std::string>())
    {
        if (*v == kValError)
        {
            cfg.unknown_key_mode = UnknownKeyMode::Error;
        }
        else if (*v == kValWarn)
        {
            cfg.unknown_key_mode = UnknownKeyMode::Warn;
        }
        else if (*v == kValIgnore)
        {
            cfg.unknown_key_mode = UnknownKeyMode::Ignore;
        }
        else
        {
            return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                               .field = "config.unknown_keys",
                               .message = R"(must be "error", "warn", or "ignore")"};
        }
    }
    return CheckUnknown(*sec, "config", {"unknown_keys"}, cfg.unknown_key_mode);
}

[[nodiscard]] std::optional<ConfigError> ParseExporterSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["exporter"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    if (auto err = CheckUnknown(*sec,
                                "exporter",
                                {"endpoint", "protocol", "compression", "headers"},
                                cfg.unknown_key_mode))
    {
        return err;
    }
    if (const auto v = (*sec)["endpoint"].value<std::string>())
    {
        cfg.endpoint_url = *v;
    }
    if (const auto v = (*sec)["protocol"].value<std::string>())
    {
        if (*v == kValGrpc)
        {
            cfg.protocol = Protocol::Grpc;
        }
        else if (*v == kValHttp)
        {
            cfg.protocol = Protocol::Http;
        }
        else
        {
            return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                               .field = "exporter.protocol",
                               .message = R"(must be "http" or "grpc")"};
        }
    }
    if (const auto v = (*sec)["compression"].value<std::string>())
    {
        cfg.compression_gzip = (*v == kValGzip);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ParseExporterHeaders(const toml::table& root, Config& cfg)
{
    const auto* exporter = root["exporter"].as_table();
    if (exporter == nullptr)
    {
        return std::nullopt;
    }
    const auto* headers = (*exporter)["headers"].as_table();
    if (headers == nullptr)
    {
        return std::nullopt;
    }
    cfg.headers.clear();
    for (const auto& [key, val] : *headers)
    {
        if (const auto sv = val.value<std::string>())
        {
            cfg.headers.push_back({.key = std::string{key}, .value = std::string{*sv}});
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ParseServiceSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["service"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    if (auto err = CheckUnknown(*sec, "service", {"name", "version"}, cfg.unknown_key_mode))
    {
        return err;
    }
    if (const auto v = (*sec)["name"].value<std::string>())
    {
        cfg.service_name = *v;
    }
    if (const auto v = (*sec)["version"].value<std::string>())
    {
        cfg.service_version = *v;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ParseResourceSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["resource"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    cfg.resource_attrs.clear();
    for (const auto& [key, val] : *sec)
    {
        if (const auto sv = val.value<std::string>())
        {
            cfg.resource_attrs.push_back({.key = std::string{key}, .value = std::string{*sv}});
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ParseTlsSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["tls"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    if (auto err =
            CheckUnknown(*sec,
                         "tls",
                         {"insecure", "ca_bundle", "client_cert", "client_key", "sni_override"},
                         cfg.unknown_key_mode))
    {
        return err;
    }
    if (const auto v = (*sec)["insecure"].value<bool>())
    {
        cfg.tls.insecure = *v;
    }
    if (const auto v = (*sec)["ca_bundle"].value<std::string>())
    {
        cfg.tls.ca_bundle = std::filesystem::path{*v};
    }
    if (const auto v = (*sec)["client_cert"].value<std::string>())
    {
        cfg.tls.client_cert = std::filesystem::path{*v};
    }
    if (const auto v = (*sec)["client_key"].value<std::string>())
    {
        cfg.tls.client_key = std::filesystem::path{*v};
    }
    if (const auto v = (*sec)["sni_override"].value<std::string>())
    {
        cfg.tls.sni_override = *v;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ParseSdkSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["sdk"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    if (auto err = CheckUnknown(
            *sec,
            "sdk",
            {"max_queue_size", "max_export_batch_size", "schedule_delay_ms", "drop_policy"},
            cfg.unknown_key_mode))
    {
        return err;
    }
    if (const auto v = (*sec)["max_queue_size"].value<std::uint32_t>())
    {
        cfg.batch.max_queue_size = *v;
    }
    if (const auto v = (*sec)["max_export_batch_size"].value<std::uint32_t>())
    {
        cfg.batch.max_export_batch_size = *v;
    }
    if (const auto v = (*sec)["schedule_delay_ms"].value<std::int64_t>())
    {
        cfg.batch.schedule_delay = std::chrono::milliseconds{*v};
    }
    if (const auto v = (*sec)["drop_policy"].value<std::string>())
    {
        if (*v == kValNewest)
        {
            cfg.batch.drop_policy = DropPolicy::DropNewest;
        }
        else if (*v == kValOldest)
        {
            cfg.batch.drop_policy = DropPolicy::DropOldest;
        }
        else
        {
            return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                               .field = "sdk.drop_policy",
                               .message = R"(must be "newest" or "oldest")"};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ParseTimeoutsSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["timeouts"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    if (auto err = CheckUnknown(
            *sec,
            "timeouts",
            {"connect_ms", "tls_ms", "per_export_ms", "retry_budget_ms", "flush_ms", "shutdown_ms"},
            cfg.unknown_key_mode))
    {
        return err;
    }
    if (const auto v = (*sec)["connect_ms"].value<std::int64_t>())
    {
        cfg.timeouts.connect = std::chrono::milliseconds{*v};
    }
    if (const auto v = (*sec)["tls_ms"].value<std::int64_t>())
    {
        cfg.timeouts.tls_handshake = std::chrono::milliseconds{*v};
    }
    if (const auto v = (*sec)["per_export_ms"].value<std::int64_t>())
    {
        cfg.timeouts.per_export = std::chrono::milliseconds{*v};
    }
    if (const auto v = (*sec)["retry_budget_ms"].value<std::int64_t>())
    {
        cfg.timeouts.retry_budget = std::chrono::milliseconds{*v};
    }
    if (const auto v = (*sec)["flush_ms"].value<std::int64_t>())
    {
        cfg.timeouts.flush = std::chrono::milliseconds{*v};
    }
    if (const auto v = (*sec)["shutdown_ms"].value<std::int64_t>())
    {
        cfg.timeouts.shutdown = std::chrono::milliseconds{*v};
    }
    return std::nullopt;
}

/// Shared core: parse a pre-built toml::table into a Config.
[[nodiscard]] microtel::Expected<Config, ConfigError> ParseTable(const toml::table& root)
{
    Config cfg;

    // [config] is parsed first to establish unknown_key_mode.
    if (auto err = ParseConfigSection(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    // Top-level section check with the now-resolved mode.
    if (auto err =
            CheckUnknown(root,
                         "",
                         {"config", "exporter", "service", "resource", "tls", "sdk", "timeouts"},
                         cfg.unknown_key_mode))
    {
        return microtel::make_unexpected(*err);
    }

    if (auto err = ParseExporterSection(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    if (auto err = ParseExporterHeaders(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    if (auto err = ParseServiceSection(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    if (auto err = ParseResourceSection(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    if (auto err = ParseTlsSection(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    if (auto err = ParseSdkSection(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    if (auto err = ParseTimeoutsSection(root, cfg))
    {
        return microtel::make_unexpected(*err);
    }
    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

microtel::Expected<Config, ConfigError> ParseTomlString(std::string_view content)
{
    try
    {
        const toml::table root = toml::parse(content);
        return ParseTable(root);
    }
    catch (const toml::parse_error& e)
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::FileParseFailure,
                                                     .field = {},
                                                     .message = std::string{e.description()}});
    }
}

microtel::Expected<Config, ConfigError> LoadToml(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::FileNotFound,
                        .field = {},
                        .message = "Config file not found: " + path.string()});
    }
    try
    {
        const toml::table root = toml::parse_file(path.string());
        return ParseTable(root);
    }
    catch (const toml::parse_error& e)
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::FileParseFailure,
                                                     .field = {},
                                                     .message = std::string{e.description()}});
    }
}

}  // namespace microtel::config
