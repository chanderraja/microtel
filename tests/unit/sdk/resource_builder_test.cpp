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
#include "fakes/fake_resource_detector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

private:
    std::vector<std::pair<microtel::LogLevel, std::string>> m_lines;
};

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
