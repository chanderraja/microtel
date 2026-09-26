// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/toml_loader.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/sdk_builder.hpp"

#include "common/config/concentrator_config.hpp"

// NOLINTNEXTLINE(misc-include-cleaner) — toml++ single-header
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
        cfg.protocol_explicit = true;
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
    if (auto err = CheckUnknown(*sec,
                                "sdk",
                                {"max_queue_size",
                                 "max_export_batch_size",
                                 "schedule_delay_ms",
                                 "drop_policy",
                                 "resource_detectors_strict"},
                                cfg.unknown_key_mode))
    {
        return err;
    }
    if (const auto v = (*sec)["resource_detectors_strict"].value<bool>())
    {
        cfg.resource_detectors_strict = *v;
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

/// `[logging]` carries one key: `level`. `sink` and `file` remain
/// unimplemented — they are the sink's business rather than the level's — so
/// they are unknown keys here, not silently ignored ones (ICP 0026 Migration,
/// correction #196).
[[nodiscard]] std::optional<ConfigError> ParseLoggingSection(const toml::table& root, Config& cfg)
{
    const auto* sec = root["logging"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    if (auto err = CheckUnknown(*sec, "logging", {"level"}, cfg.unknown_key_mode))
    {
        return err;
    }
    const auto v = (*sec)["level"].value<std::string>();
    if (!v)
    {
        return std::nullopt;
    }
    const auto parsed = ParseLogLevel(*v);
    if (!parsed)
    {
        return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                           .field = "logging.level",
                           .message = R"(must be "trace", "debug", "info", "warn" or "error")"};
    }
    cfg.log_level = *parsed;
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

// ---------------------------------------------------------------------------
// [concentrator] (docs/leaf-concentrator-design.md §4.2)
// ---------------------------------------------------------------------------
//
// Unlike the older sections, a key present with the wrong type is an error
// here rather than silently skipped: every key names a limit or a policy, and
// a typo that left the default in place would go unnoticed.

[[nodiscard]] ConfigError InvalidAt(std::string_view section,
                                    std::string_view key,
                                    std::string_view message)
{
    std::string field{section};
    field += '.';
    field += key;
    return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                       .field = field,
                       .message = field + ": " + std::string{message}};
}

/// Where a `[concentrator]` value is read from and what it is called.
struct ConcentratorKey
{
    const toml::table* table = nullptr;
    std::string_view section;
    std::string_view key;
};

[[nodiscard]] std::optional<ConfigError> ReadBool(const ConcentratorKey& at, bool& out)
{
    const toml::node* const node = at.table->get(at.key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const b = node->as_boolean();
    if (b == nullptr)
    {
        return InvalidAt(at.section, at.key, "must be true or false");
    }
    out = b->get();
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ReadCount(const ConcentratorKey& at, std::uint32_t& out)
{
    const toml::node* const node = at.table->get(at.key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const i = node->as_integer();
    if (i == nullptr || !std::in_range<std::uint32_t>(i->get()))
    {
        return InvalidAt(at.section, at.key, "must be an integer from 0 to 4294967295");
    }
    out = static_cast<std::uint32_t>(i->get());
    return std::nullopt;
}

/// A byte size: an integer number of bytes, or a string such as "64KiB".
[[nodiscard]] std::optional<ConfigError> ReadByteSize(const ConcentratorKey& at, std::uint32_t& out)
{
    const toml::node* const node = at.table->get(at.key);
    if (node == nullptr || node->is_integer())
    {
        return ReadCount(at, out);
    }
    const auto* const s = node->as_string();
    const auto parsed = s == nullptr ? std::nullopt : ParseByteSize(s->get());
    if (!parsed.has_value())
    {
        return InvalidAt(at.section,
                         at.key,
                         R"(must be a byte count or a string such as "64KiB" (B, KiB, MiB))");
    }
    out = *parsed;
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ReadDuration(const ConcentratorKey& at,
                                                      std::chrono::seconds& out)
{
    const toml::node* const node = at.table->get(at.key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const s = node->as_string();
    const auto parsed = s == nullptr ? std::nullopt : ParseDuration(s->get());
    if (!parsed.has_value())
    {
        return InvalidAt(at.section, at.key, R"(must be a string such as "30s", "5m" or "1h")");
    }
    out = *parsed;
    return std::nullopt;
}

/// A string key whose text @p parse turns into a value, or refuses.
template <typename T, typename Parse>
[[nodiscard]] std::optional<ConfigError> ReadEnum(const ConcentratorKey& at,
                                                  Parse parse,
                                                  std::string_view expected,
                                                  T& out)
{
    const toml::node* const node = at.table->get(at.key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const s = node->as_string();
    auto parsed = s == nullptr ? std::nullopt : parse(s->get());
    if (!parsed.has_value())
    {
        return InvalidAt(at.section, at.key, expected);
    }
    out = *parsed;
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ReadString(const ConcentratorKey& at, std::string& out)
{
    const toml::node* const node = at.table->get(at.key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const s = node->as_string();
    if (s == nullptr)
    {
        return InvalidAt(at.section, at.key, "must be a string");
    }
    out = s->get();
    return std::nullopt;
}

/// One Resource value: a string, integer, float or boolean.
[[nodiscard]] std::optional<AttributeValue> ResourceValue(const toml::node& node)
{
    if (const auto* const s = node.as_string())
    {
        return AttributeValue{s->get()};
    }
    if (const auto* const i = node.as_integer())
    {
        return AttributeValue{i->get()};
    }
    if (const auto* const d = node.as_floating_point())
    {
        return AttributeValue{d->get()};
    }
    if (const auto* const b = node.as_boolean())
    {
        return AttributeValue{b->get()};
    }
    return std::nullopt;
}

/// A `resource` sub-table of `[concentrator]`, as attributes in file order.
[[nodiscard]] std::optional<ConfigError> ReadResource(const ConcentratorKey& at,
                                                      std::vector<KeyValue>& out)
{
    const toml::node* const node = at.table->get(at.key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const tbl = node->as_table();
    if (tbl == nullptr)
    {
        return InvalidAt(at.section, at.key, "must be a table");
    }
    std::vector<KeyValue> attrs;
    for (const auto& [key, value] : *tbl)
    {
        auto v = ResourceValue(value);
        if (!v.has_value())
        {
            return InvalidAt(at.section,
                             at.key,
                             "'" + std::string{key.str()} +
                                 "' must be a string, integer, float or boolean (quote "
                                 "dotted keys: \"service.name\" = ...)");
        }
        attrs.push_back(KeyValue{.key = std::string{key.str()}, .value = std::move(*v)});
    }
    out = std::move(attrs);
    return std::nullopt;
}

constexpr std::string_view kConcentrator = "concentrator";

[[nodiscard]] std::optional<ConfigError> ReadConcentratorLimits(const toml::table& sec,
                                                                LeafReceiverOptions& o)
{
    const auto at = [&sec](std::string_view key)
    { return ConcentratorKey{.table = &sec, .section = kConcentrator, .key = key}; };
    std::optional<ConfigError> err = ReadBool(at("enabled"), o.enabled);
    if (!err)
    {
        err = ReadByteSize(at("max_payload_bytes"), o.max_payload_bytes);
    }
    if (!err)
    {
        err = ReadCount(at("max_spans_per_payload"), o.max_spans_per_payload);
    }
    if (!err)
    {
        err = ReadCount(at("max_leaves"), o.max_leaves);
    }
    if (!err)
    {
        err = ReadByteSize(at("max_leaf_resource_bytes"), o.max_leaf_resource_bytes);
    }
    if (!err)
    {
        err = ReadDuration(at("leaf_idle_timeout"), o.leaf_idle_timeout);
    }
    return err;
}

[[nodiscard]] std::optional<ConfigError> ReadConcentratorPolicies(const toml::table& sec,
                                                                  LeafReceiverOptions& o)
{
    const auto at = [&sec](std::string_view key)
    { return ConcentratorKey{.table = &sec, .section = kConcentrator, .key = key}; };
    std::optional<ConfigError> err = ReadEnum(at("unknown_leaf"),
                                              ParseUnknownLeafPolicy,
                                              R"(must be "accept" or "reject")",
                                              o.unknown_leaf);
    if (!err)
    {
        err = ReadString(at("leaf_id_attribute"), o.leaf_id_attribute);
    }
    if (!err)
    {
        err = ReadEnum(
            at("default_time_mode"),
            ParseDefaultTimeMode,
            R"(must be "auto", "concentrator_stamped", "sync_relative" or "boot_relative")",
            o.default_time_mode);
    }
    if (!err)
    {
        err = ReadDuration(at("max_sync_age"), o.max_sync_age);
    }
    if (!err)
    {
        err = ReadDuration(at("max_clock_skew"), o.max_clock_skew);
    }
    if (!err)
    {
        err = ReadDuration(at("boot_anchor_window"), o.boot_anchor_window);
    }
    return err;
}

[[nodiscard]] std::optional<ConfigError> ReadLeafDefaults(const toml::table& sec,
                                                          LeafReceiverOptions& o,
                                                          UnknownKeyMode mode)
{
    const toml::node* const node = sec.get("leaf_defaults");
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const tbl = node->as_table();
    if (tbl == nullptr)
    {
        return InvalidAt(kConcentrator, "leaf_defaults", "must be a table");
    }
    constexpr std::string_view kSection = "concentrator.leaf_defaults";
    if (auto err = CheckUnknown(*tbl, kSection, {"resource"}, mode))
    {
        return err;
    }
    return ReadResource(ConcentratorKey{.table = tbl, .section = kSection, .key = "resource"},
                        o.leaf_defaults_resource);
}

[[nodiscard]] std::optional<ConfigError> ReadLeaf(const toml::node& node,
                                                  const std::string& section,
                                                  LeafConfig& leaf,
                                                  UnknownKeyMode mode)
{
    const auto* const tbl = node.as_table();
    if (tbl == nullptr)
    {
        return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                           .field = section,
                           .message = section + ": must be a table"};
    }
    if (auto err = CheckUnknown(*tbl, section, {"time_mode", "resource"}, mode))
    {
        return err;
    }
    const auto at = [tbl, &section](std::string_view key)
    { return ConcentratorKey{.table = tbl, .section = section, .key = key}; };
    LeafTimeMode time_mode{};
    if (auto err = ReadEnum(at("time_mode"),
                            ParseLeafTimeMode,
                            R"(must be "concentrator_stamped", "sync_relative" or "boot_relative")",
                            time_mode))
    {
        return err;
    }
    if (tbl->contains("time_mode"))
    {
        leaf.time_mode = time_mode;
    }
    return ReadResource(at("resource"), leaf.resource);
}

[[nodiscard]] std::optional<ConfigError> ReadLeaves(const toml::table& sec,
                                                    LeafReceiverOptions& o,
                                                    UnknownKeyMode mode)
{
    const toml::node* const node = sec.get("leaves");
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* const tbl = node->as_table();
    if (tbl == nullptr)
    {
        return InvalidAt(kConcentrator, "leaves", "must be a table");
    }
    for (const auto& [id, value] : *tbl)
    {
        LeafConfig leaf;
        const std::string section = "concentrator.leaves." + std::string{id.str()};
        if (auto err = ReadLeaf(value, section, leaf, mode))
        {
            return err;
        }
        o.leaves.emplace_back(std::string{id.str()}, std::move(leaf));
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> ParseConcentratorSection(const toml::table& root,
                                                                  Config& cfg)
{
    const auto* sec = root["concentrator"].as_table();
    if (sec == nullptr)
    {
        return std::nullopt;
    }
    if (auto err = CheckUnknown(*sec,
                                kConcentrator,
                                {"enabled",
                                 "max_payload_bytes",
                                 "max_spans_per_payload",
                                 "max_leaves",
                                 "max_leaf_resource_bytes",
                                 "leaf_idle_timeout",
                                 "unknown_leaf",
                                 "leaf_id_attribute",
                                 "default_time_mode",
                                 "max_sync_age",
                                 "max_clock_skew",
                                 "boot_anchor_window",
                                 "leaf_defaults",
                                 "leaves"},
                                cfg.unknown_key_mode))
    {
        return err;
    }
    if (auto err = ReadConcentratorLimits(*sec, cfg.concentrator))
    {
        return err;
    }
    if (auto err = ReadConcentratorPolicies(*sec, cfg.concentrator))
    {
        return err;
    }
    if (auto err = ReadLeafDefaults(*sec, cfg.concentrator, cfg.unknown_key_mode))
    {
        return err;
    }
    return ReadLeaves(*sec, cfg.concentrator, cfg.unknown_key_mode);
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
    if (auto err = CheckUnknown(root,
                                "",
                                {"config",
                                 "exporter",
                                 "service",
                                 "resource",
                                 "tls",
                                 "sdk",
                                 "timeouts",
                                 "logging",
                                 "concentrator"},
                                cfg.unknown_key_mode))
    {
        return microtel::make_unexpected(*err);
    }

    // Every other section, in this order; the first error wins.
    using SectionParser = std::optional<ConfigError> (*)(const toml::table&, Config&);
    constexpr std::array<SectionParser, 9> kSections{
        ParseExporterSection,
        ParseExporterHeaders,
        ParseServiceSection,
        ParseResourceSection,
        ParseTlsSection,
        ParseSdkSection,
        ParseLoggingSection,
        ParseTimeoutsSection,
        ParseConcentratorSection,
    };
    for (const SectionParser parse : kSections)
    {
        if (auto err = parse(root, cfg))
        {
            return microtel::make_unexpected(*err);
        }
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
