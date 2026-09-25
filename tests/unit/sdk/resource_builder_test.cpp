// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// v1.1 — the §12.7 resource composition: built-in defaults, then detectors in
// registration order, then the resolved config (env and user). Later wins.
//
// This is the unit that owns the precedence *order*; `Resource::Merge` owns the
// key-level rule and is tested in tests/unit/api/resource_merge_test.cpp.

#include "sdk/resource_builder.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/resource.hpp"

#include "common/config/config.hpp"
#include "common/internal_log.hpp"
#include "fakes/fake_resource_detector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

namespace ms = microtel::sdk;
namespace mt = microtel;

using DetectorList = std::vector<std::unique_ptr<mt::internal::IResourceDetector>>;

[[nodiscard]] std::optional<mt::AttributeValue> Lookup(const mt::Resource& res,
                                                       std::string_view key)
{
    for (const auto& kv : res.Attributes())
    {
        if (kv.key == key)
        {
            return kv.value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool Has(const mt::Resource& res, std::string_view key)
{
    return Lookup(res, key).has_value();
}

[[nodiscard]] std::string Str(const mt::Resource& res, std::string_view key)
{
    const auto value = Lookup(res, key);
    if (!value.has_value())
    {
        ADD_FAILURE() << "missing resource attribute: " << key;
        return {};
    }
    return std::get<std::string>(*value);
}

/// @brief A config as `Validate()` leaves it: service name already resolved.
[[nodiscard]] microtel::config::Config ConfigWithServiceName(std::string name)
{
    microtel::config::Config cfg;
    cfg.service_name_defaulted = name.empty();
    cfg.service_name = cfg.service_name_defaulted ? "unknown_service" : std::move(name);
    return cfg;
}

/// @brief A detector that reports `attrs`, owned by the returned list entry.
[[nodiscard]] std::unique_ptr<mt::testing::FakeResourceDetector> MakeFake(
    std::string_view name, std::vector<mt::KeyValue> attrs)
{
    auto fake = std::make_unique<mt::testing::FakeResourceDetector>();
    fake->name = name;
    fake->resource_to_return = mt::Resource{std::move(attrs)};
    return fake;
}

/// @brief A detector that fails with `message`.
[[nodiscard]] std::unique_ptr<mt::testing::FakeResourceDetector> MakeFailingFake(
    std::string_view name, std::string message)
{
    auto fake = std::make_unique<mt::testing::FakeResourceDetector>();
    fake->name = name;
    fake->failure = mt::ConfigError{.kind = mt::ConfigError::Kind::Unspecified,
                                    .field = "resource.detectors.fake",
                                    .message = std::move(message)};
    return fake;
}

/// @brief RAII capture of microtel's internal log lines.
class LogCapture
{
public:
    LogCapture()
    {
        microtel::SetLogSink([this](microtel::LogLevel level, std::string_view message)
                             { m_lines.emplace_back(level, std::string{message}); });
    }

    ~LogCapture()
    {
        microtel::ResetLogSink();
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    [[nodiscard]] bool Contains(microtel::LogLevel level, std::string_view needle) const
    {
        return std::ranges::any_of(m_lines,
                                   [level, needle](const auto& line) {
                                       return line.first == level &&
                                              line.second.find(needle) != std::string::npos;
                                   });
    }

    /// @brief Every captured line at `level` that contains `needle`.
    [[nodiscard]] std::vector<std::string> Lines(microtel::LogLevel level,
                                                 std::string_view needle) const
    {
        std::vector<std::string> out;
        for (const auto& [lvl, text] : m_lines)
        {
            if (lvl == level && text.find(needle) != std::string::npos)
            {
                out.push_back(text);
            }
        }
        return out;
    }

private:
    std::vector<std::pair<microtel::LogLevel, std::string>> m_lines;
};

/// @brief Restores the process-wide internal log level on scope exit.
class MinLogLevelGuard
{
public:
    explicit MinLogLevelGuard(microtel::LogLevel level) : m_saved(microtel::internal::MinLogLevel())
    {
        (void)microtel::internal::SetMinLogLevel(level);
    }

    ~MinLogLevelGuard()
    {
        (void)microtel::internal::SetMinLogLevel(m_saved);
    }

    MinLogLevelGuard(const MinLogLevelGuard&) = delete;
    MinLogLevelGuard& operator=(const MinLogLevelGuard&) = delete;
    MinLogLevelGuard(MinLogLevelGuard&&) = delete;
    MinLogLevelGuard& operator=(MinLogLevelGuard&&) = delete;

private:
    microtel::LogLevel m_saved;
};

constexpr std::string_view kResolvedNeedle = "resolved resource";

/// @brief The one resolved-Resource Info line captured, or "" after a failure.
[[nodiscard]] std::string OnlyResolvedLine(const LogCapture& logs)
{
    const auto lines = logs.Lines(microtel::LogLevel::Info, kResolvedNeedle);
    EXPECT_EQ(lines.size(), 1U);
    return lines.empty() ? std::string{} : lines.front();
}

}  // namespace

// ---------------------------------------------------------------------------
// No detectors — the v1.0 behaviour, unchanged
// ---------------------------------------------------------------------------

TEST(ResourceBuilderTest, NoDetectors_EmitsServiceIdentityAndConfigAttributes)
{
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.service_version = "1.2.3";
    cfg.resource_attrs = {{.key = "deployment.environment", .value = std::string{"prod"}}};

    const DetectorList detectors;
    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "service.name"), "checkout");
    EXPECT_EQ(Str(*res, "service.version"), "1.2.3");
    EXPECT_EQ(Str(*res, "deployment.environment"), "prod");
}

TEST(ResourceBuilderTest, NoDetectors_EmptyServiceVersion_IsOmitted)
{
    const microtel::config::Config cfg = ConfigWithServiceName("checkout");

    const DetectorList detectors;
    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_FALSE(Has(*res, "service.version"));
}

// ---------------------------------------------------------------------------
// §12.7 precedence — detectors, then env, then user
// ---------------------------------------------------------------------------

TEST(ResourceBuilderTest, DetectorAttributes_ReachTheResource)
{
    const microtel::config::Config cfg = ConfigWithServiceName("checkout");

    DetectorList detectors;
    detectors.push_back(MakeFake("host", {{.key = "host.name", .value = std::string{"node-7"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.name"), "node-7");
    EXPECT_EQ(Str(*res, "service.name"), "checkout");
}

TEST(ResourceBuilderTest, ConfigAttribute_OverridesDetectorAttribute)
{
    // The user-supplied layer is last, so it wins. This is the whole point of
    // the ordering: a detector guesses, an operator decides.
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_attrs = {{.key = "host.name", .value = std::string{"from-config"}}};

    DetectorList detectors;
    detectors.push_back(
        MakeFake("host", {{.key = "host.name", .value = std::string{"from-detector"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.name"), "from-config");
}

TEST(ResourceBuilderTest, LaterDetector_OverridesEarlierDetector)
{
    const microtel::config::Config cfg = ConfigWithServiceName("checkout");

    DetectorList detectors;
    detectors.push_back(MakeFake("first", {{.key = "host.id", .value = std::string{"first"}}}));
    detectors.push_back(MakeFake("second", {{.key = "host.id", .value = std::string{"second"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.id"), "second");
}

TEST(ResourceBuilderTest, DetectorServiceName_BeatsTheUnknownServicePlaceholder)
{
    // `unknown_service` is a built-in default — the lowest tier in §12.1 — so a
    // detector that knows better must win. Without the provenance flag the
    // placeholder would shadow every detector that sets service.name.
    const microtel::config::Config cfg = ConfigWithServiceName("");
    ASSERT_TRUE(cfg.service_name_defaulted);

    DetectorList detectors;
    detectors.push_back(
        MakeFake("service", {{.key = "service.name", .value = std::string{"detected-svc"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "service.name"), "detected-svc");
}

TEST(ResourceBuilderTest, NoDetectorServiceName_KeepsTheUnknownServicePlaceholder)
{
    const microtel::config::Config cfg = ConfigWithServiceName("");

    DetectorList detectors;
    detectors.push_back(MakeFake("host", {{.key = "host.name", .value = std::string{"node-7"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "service.name"), "unknown_service");
}

TEST(ResourceBuilderTest, ConfiguredServiceName_BeatsDetectorServiceName)
{
    const microtel::config::Config cfg = ConfigWithServiceName("checkout");

    DetectorList detectors;
    detectors.push_back(
        MakeFake("service", {{.key = "service.name", .value = std::string{"detected-svc"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "service.name"), "checkout");
}

TEST(ResourceBuilderTest, EachDetectorIsCalledExactlyOnce)
{
    // The interface documents detection as one-shot (docs/interfaces.md §4.10).
    const microtel::config::Config cfg = ConfigWithServiceName("checkout");

    auto owned = MakeFake("host", {{.key = "host.name", .value = std::string{"node-7"}}});
    auto* const observer = owned.get();

    DetectorList detectors;
    detectors.push_back(std::move(owned));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(observer->detect_call_count, 1);
}

// ---------------------------------------------------------------------------
// Strict / lenient detector policy
// ---------------------------------------------------------------------------

TEST(ResourceBuilderTest, Lenient_FailingDetector_IsSkippedAndTheRestSurvive)
{
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_detectors_strict = false;

    DetectorList detectors;
    detectors.push_back(MakeFailingFake("broken", "no /proc here"));
    detectors.push_back(MakeFake("host", {{.key = "host.name", .value = std::string{"node-7"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.name"), "node-7");
    EXPECT_EQ(Str(*res, "service.name"), "checkout");
}

TEST(ResourceBuilderTest, Lenient_FailingDetector_LogsAWarningNamingTheDetector)
{
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_detectors_strict = false;

    DetectorList detectors;
    detectors.push_back(MakeFailingFake("broken", "no /proc here"));

    const LogCapture logs;
    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_TRUE(logs.Contains(microtel::LogLevel::Warn, "broken"));
    EXPECT_TRUE(logs.Contains(microtel::LogLevel::Warn, "no /proc here"));
}

TEST(ResourceBuilderTest, Strict_FailingDetector_ReturnsTheDetectorsConfigError)
{
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_detectors_strict = true;

    DetectorList detectors;
    detectors.push_back(MakeFailingFake("broken", "no /proc here"));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message.find("broken"), std::string::npos);
    EXPECT_NE(res.error().message.find("no /proc here"), std::string::npos);
}

TEST(ResourceBuilderTest, Strict_FailingDetector_StopsBeforeLaterDetectorsRun)
{
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_detectors_strict = true;

    auto later = MakeFake("host", {{.key = "host.name", .value = std::string{"node-7"}}});
    auto* const observer = later.get();

    DetectorList detectors;
    detectors.push_back(MakeFailingFake("broken", "no /proc here"));
    detectors.push_back(std::move(later));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(observer->detect_call_count, 0);
}

TEST(ResourceBuilderTest, Strict_AllDetectorsSucceed_BuildsNormally)
{
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_detectors_strict = true;

    DetectorList detectors;
    detectors.push_back(MakeFake("host", {{.key = "host.name", .value = std::string{"node-7"}}}));

    const auto res = ms::BuildResource(cfg, detectors);

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.name"), "node-7");
}

// ---------------------------------------------------------------------------
// Issue #284 — spec §12.7 "the resolved Resource is logged at init"
// ---------------------------------------------------------------------------

TEST(ResourceBuilderTest, ResolvedResource_LoggedOnceAtInfoPerBuild)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_attrs.push_back({.key = "deployment.environment", .value = std::string{"prod"}});
    const DetectorList detectors;

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    const std::string line = OnlyResolvedLine(logs);
    EXPECT_NE(line.find("service.name=\"checkout\""), std::string::npos) << line;
    EXPECT_NE(line.find("deployment.environment=\"prod\""), std::string::npos) << line;

    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    EXPECT_EQ(logs.Lines(microtel::LogLevel::Info, kResolvedNeedle).size(), 2U);
}

TEST(ResourceBuilderTest, ResolvedResource_FilteredOutAtWarn)
{
    const MinLogLevelGuard level{microtel::LogLevel::Warn};
    const microtel::config::Config cfg = ConfigWithServiceName("checkout");
    const DetectorList detectors;

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    EXPECT_TRUE(logs.Lines(microtel::LogLevel::Info, kResolvedNeedle).empty());
}

TEST(ResourceBuilderTest, ResolvedResource_KeysAreSorted)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_attrs.push_back({.key = "zeta.attr", .value = std::int64_t{7}});

    DetectorList detectors;
    detectors.push_back(MakeFake(
        "fake",
        {{.key = "mid.attr", .value = true}, {.key = "alpha.attr", .value = std::string{"a"}}}));

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    const std::string line = OnlyResolvedLine(logs);

    const auto alpha = line.find("alpha.attr=\"a\"");
    const auto mid = line.find("mid.attr=true");
    const auto service = line.find("service.name=");
    const auto zeta = line.find("zeta.attr=7");
    ASSERT_NE(alpha, std::string::npos) << line;
    ASSERT_NE(mid, std::string::npos) << line;
    ASSERT_NE(service, std::string::npos) << line;
    ASSERT_NE(zeta, std::string::npos) << line;
    EXPECT_LT(alpha, mid);
    EXPECT_LT(mid, service);
    EXPECT_LT(service, zeta);
}

TEST(ResourceBuilderTest, ResolvedResource_RedactsSecretLookingValues)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_attrs.push_back({.key = "auth.Token", .value = std::string{"s3cr3t-token"}});
    cfg.resource_attrs.push_back({.key = "db.password", .value = std::string{"hunter2"}});
    cfg.resource_attrs.push_back(
        {.key = "authorization", .value = std::vector<std::string>{"Bearer abc"}});
    const DetectorList detectors;

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    const std::string line = OnlyResolvedLine(logs);

    EXPECT_EQ(line.find("s3cr3t-token"), std::string::npos) << line;
    EXPECT_EQ(line.find("hunter2"), std::string::npos) << line;
    EXPECT_EQ(line.find("Bearer abc"), std::string::npos) << line;
    EXPECT_NE(line.find("auth.Token=<redacted>"), std::string::npos) << line;
    EXPECT_NE(line.find("db.password=<redacted>"), std::string::npos) << line;
    EXPECT_NE(line.find("service.name=\"checkout\""), std::string::npos)
        << "an ordinary key must not be redacted: " << line;
}

TEST(ResourceBuilderTest, ResolvedResource_TruncatesPastTheAttributeCap)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    constexpr std::size_t kExtra = 3;
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    for (std::size_t i = 0; i < ms::kMaxLoggedResourceAttributes + kExtra; ++i)
    {
        // Zero-padded so the sorted order is the numeric order.
        std::string key =
            "attr." + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i);
        cfg.resource_attrs.push_back({.key = std::move(key), .value = std::int64_t{1}});
    }
    const DetectorList detectors;

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    const std::string line = OnlyResolvedLine(logs);

    // Cap + extra from config, plus service.name: kExtra + 1 left over.
    EXPECT_NE(line.find("...and " + std::to_string(kExtra + 1) + " more"), std::string::npos)
        << line;
    EXPECT_NE(line.find("attr.000=1"), std::string::npos) << line;
    EXPECT_EQ(line.find("service.name="), std::string::npos)
        << "service.name sorts last here and falls past the cap: " << line;
}

TEST(ResourceBuilderTest, ResolvedResource_CapsALongValue)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    constexpr std::size_t kLong = 4 * ms::kMaxLoggedResourceValueChars;
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_attrs.push_back({.key = "big", .value = std::string(kLong, 'x')});
    const DetectorList detectors;

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    const std::string line = OnlyResolvedLine(logs);

    EXPECT_EQ(line.find(std::string(ms::kMaxLoggedResourceValueChars + 1, 'x')), std::string::npos)
        << line;
    EXPECT_NE(line.find("big=\"" + std::string(ms::kMaxLoggedResourceValueChars - 1, 'x')),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("..."), std::string::npos) << line;
}

TEST(ResourceBuilderTest, ResolvedResource_NamesTheProfile)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    const microtel::config::Config cfg = ConfigWithServiceName("checkout");
    const DetectorList detectors;

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors, "payments").has_value());
    EXPECT_NE(OnlyResolvedLine(logs).find("profile \"payments\""), std::string::npos);
}

TEST(ResourceBuilderTest, ResolvedResource_StrictFailure_LogsNothing)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_detectors_strict = true;
    DetectorList detectors;
    detectors.push_back(MakeFailingFake("broken", "no /proc here"));

    const LogCapture logs;
    ASSERT_FALSE(ms::BuildResource(cfg, detectors).has_value());
    EXPECT_TRUE(logs.Lines(microtel::LogLevel::Info, kResolvedNeedle).empty());
}

TEST(ResourceBuilderTest, ResolvedResource_RendersEveryValueType)
{
    const MinLogLevelGuard level{microtel::LogLevel::Info};
    microtel::config::Config cfg = ConfigWithServiceName("checkout");
    cfg.resource_attrs.push_back({.key = "v.bool", .value = false});
    cfg.resource_attrs.push_back({.key = "v.double", .value = 1.5});
    cfg.resource_attrs.push_back({.key = "v.bools", .value = std::vector<bool>{true, false}});
    cfg.resource_attrs.push_back({.key = "v.ints", .value = std::vector<std::int64_t>{1, -2}});
    cfg.resource_attrs.push_back({.key = "v.doubles", .value = std::vector<double>{0.25}});
    cfg.resource_attrs.push_back({.key = "v.strings", .value = std::vector<std::string>{"a", "b"}});
    const DetectorList detectors;

    const LogCapture logs;
    ASSERT_TRUE(ms::BuildResource(cfg, detectors).has_value());
    const std::string line = OnlyResolvedLine(logs);

    for (const std::string_view expected : {"v.bool=false",
                                            "v.double=1.5",
                                            "v.bools=[true, false]",
                                            "v.ints=[1, -2]",
                                            "v.doubles=[0.25]",
                                            R"(v.strings=["a", "b"])",
                                            "(7 attributes):"})
    {
        EXPECT_NE(line.find(expected), std::string::npos) << expected << " in: " << line;
    }
}
