// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The spec §12.7 resource composition. Kept out of sdk_builder.cpp so that the
// ordering — the part with actual semantics — is unit-testable without
// standing up a whole Provider.

#include "sdk/resource_builder.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/resource.hpp"

#include "common/config/config.hpp"
#include "common/internal_log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace microtel::sdk
{

namespace
{

/// @brief Layer 1 — the built-in defaults.
///
/// Only `service.name`, and only when nothing configured one. It lives here
/// rather than alongside the configured attributes because a placeholder must
/// lose to a detector that actually knows the service's name.
[[nodiscard]] Resource DefaultsLayer(const config::Config& cfg)
{
    if (!cfg.service_name_defaulted)
    {
        return Resource{};
    }
    return Resource{{{.key = "service.name", .value = cfg.service_name}}};
}

/// @brief Layers 3 and 4 — the environment and the user, already collapsed.
///
/// `config::OverlayEnv` and `SdkBuilder::Impl::ApplyResourceOverrides` have
/// resolved file, env and code into `cfg` before this runs, in that order, so
/// the two tiers arrive as one layer that sits above every detector.
[[nodiscard]] Resource ConfigLayer(const config::Config& cfg)
{
    std::vector<KeyValue> attrs;
    if (!cfg.service_name_defaulted)
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
    return Resource{std::move(attrs)};
}

/// @brief Describe a detector failure for a log line or an error message.
[[nodiscard]] std::string DescribeFailure(const internal::IResourceDetector& detector,
                                          const ConfigError& error)
{
    return "resource detector \"" + std::string{detector.Name()} + "\" failed: " + error.message;
}

// --- The resolved-Resource log line (spec §12.7, issue #284) ---------------

constexpr std::string_view kRedacted = "<redacted>";
constexpr std::string_view kEllipsis = "...";

/// Key fragments whose values the log line never prints. There is no general
/// redaction code in microtel yet (docs/configuration.md §5); this applies the
/// spec §12.6 rule — `Authorization`, client secrets, token-provider outputs —
/// by key, matched case-insensitively anywhere in the attribute key.
constexpr std::array<std::string_view, 8> kSecretKeyFragments = {
    "authorization", "secret", "token", "password", "passwd", "credential", "api_key", "apikey"};

[[nodiscard]] bool IsSecretKey(std::string_view key)
{
    std::string lowered{key};
    std::ranges::transform(lowered,
                           lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::any_of(kSecretKeyFragments,
                               [&lowered](std::string_view fragment)
                               { return lowered.find(fragment) != std::string::npos; });
}

[[nodiscard]] std::string Render(bool value)
{
    return value ? "true" : "false";
}

[[nodiscard]] std::string Render(std::int64_t value)
{
    return std::to_string(value);
}

[[nodiscard]] std::string Render(double value)
{
    return std::format("{}", value);
}

constexpr unsigned char kFirstPrintable = 0x20;
constexpr unsigned char kDelete = 0x7F;
constexpr std::size_t kSimpleEscapeLen = 2;  // backslash + one char
constexpr std::size_t kHexEscapeLen = 4;     // backslash + 'x' + two hex digits

/// @brief Append `c` to `out`, escaped if it is a backslash, a double quote
/// or a control character. Bytes >= 0x80 (UTF-8) pass through unchanged.
void AppendEscaped(std::string& out, char c)
{
    switch (c)
    {
        case '\\':
            out += "\\\\";
            return;
        case '"':
            out += "\\\"";
            return;
        case '\n':
            out += "\\n";
            return;
        case '\r':
            out += "\\r";
            return;
        case '\t':
            out += "\\t";
            return;
        default:
            break;
    }
    const auto byte = static_cast<unsigned char>(c);
    if (byte < kFirstPrintable || byte == kDelete)
    {
        out += std::format("\\x{:02X}", byte);
        return;
    }
    out += c;
}

/// @brief `text` with backslash, double quote and control characters escaped,
/// so that no key or value can split the log line or blur its `key="value"`
/// boundaries (issue #315).
[[nodiscard]] std::string Escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        AppendEscaped(out, c);
    }
    return out;
}

/// @brief The longest prefix of an escaped `rendered` that fits in `cap`
/// characters without cutting an escape sequence in half. `Escape` turns
/// every literal backslash into `\\`, so a backslash always starts one.
[[nodiscard]] std::size_t SafeCutPoint(std::string_view rendered, std::size_t cap)
{
    std::size_t pos = 0;
    while (pos < rendered.size())
    {
        const std::string_view rest = rendered.substr(pos);
        std::size_t len = 1;
        if (rest.starts_with("\\x"))
        {
            len = kHexEscapeLen;
        }
        else if (rest.starts_with('\\'))
        {
            len = kSimpleEscapeLen;
        }
        if (pos + len > cap)
        {
            break;
        }
        pos += len;
    }
    return pos;
}

[[nodiscard]] std::string Render(const std::string& value)
{
    return "\"" + Escape(value) + "\"";
}

template <typename T>
[[nodiscard]] std::string Render(const std::vector<T>& values)
{
    std::string out = "[";
    std::string_view separator;
    for (const auto& value : values)
    {
        out += separator;
        out += Render(T{value});
        separator = ", ";
    }
    return out + "]";
}

/// @brief One value as the log line shows it: redacted, or rendered and capped.
[[nodiscard]] std::string LoggedValue(const KeyValue& kv)
{
    if (IsSecretKey(kv.key))
    {
        return std::string{kRedacted};
    }
    std::string rendered = std::visit([](const auto& held) { return Render(held); }, kv.value);
    if (rendered.size() > kMaxLoggedResourceValueChars)
    {
        rendered.resize(SafeCutPoint(rendered, kMaxLoggedResourceValueChars));
        rendered += kEllipsis;
    }
    return rendered;
}

/// @brief Log the composed Resource once at Info: sorted, redacted, bounded.
void LogResolvedResource(const Resource& resource, std::string_view profile_name)
{
    if (internal::MinLogLevel() > LogLevel::Info)
    {
        return;
    }
    std::vector<const KeyValue*> sorted;
    sorted.reserve(resource.Attributes().size());
    for (const auto& kv : resource.Attributes())
    {
        sorted.push_back(&kv);
    }
    std::ranges::sort(sorted, {}, [](const KeyValue* kv) -> const std::string& { return kv->key; });

    std::string line = "resolved resource (";
    if (!profile_name.empty())
    {
        line += "profile \"" + std::string{profile_name} + "\", ";
    }
    line += std::to_string(sorted.size()) + " attributes):";
    const std::size_t shown = std::min(sorted.size(), kMaxLoggedResourceAttributes);
    std::string_view separator = " ";
    for (const KeyValue* kv : std::span{sorted}.first(shown))
    {
        line += separator;
        line += Escape(kv->key) + "=" + LoggedValue(*kv);
        separator = ", ";
    }
    if (sorted.size() > shown)
    {
        line += ", " + std::string{kEllipsis} + "and " + std::to_string(sorted.size() - shown) +
                " more";
    }
    internal::LogImpl(LogLevel::Info, line);
}

}  // namespace

Expected<Resource, ConfigError> BuildResource(
    const config::Config& cfg,
    std::span<const std::unique_ptr<internal::IResourceDetector>> detectors,
    std::string_view profile_name)
{
    Resource detected;
    for (const auto& detector : detectors)
    {
        auto contribution = detector->Detect();
        if (contribution.has_value())
        {
            detected = Resource::Merge(detected, *contribution);
            continue;
        }
        if (cfg.resource_detectors_strict)
        {
            return make_unexpected(
                ConfigError{.kind = contribution.error().kind,
                            .field = contribution.error().field,
                            .message = DescribeFailure(*detector, contribution.error())});
        }
        internal::LogImpl(LogLevel::Warn,
                          DescribeFailure(*detector, contribution.error()) +
                              " - skipping its contribution (set "
                              "sdk.resource_detectors_strict to fail Build instead)");
    }

    Resource resolved =
        Resource::Merge(Resource::Merge(DefaultsLayer(cfg), detected), ConfigLayer(cfg));
    LogResolvedResource(resolved, profile_name);
    return resolved;
}

}  // namespace microtel::sdk
