// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers LoggerShim (GetName, CreateLogRecord/EmitLogRecord round trip) and
// LoggerProviderShim (name-defaults-to-logger_name per the reference SDK,
// schema_url/attributes dropped, global registration end-to-end).

#include "adapters/otelcpp/logger_shim.hpp"
#include "adapters/otelcpp/shim_options.hpp"
#include "fakes/fake_provider.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include <opentelemetry/logs/provider.h>

namespace
{

using microtel::adapters::otelcpp::LoggerProviderShim;
using microtel::adapters::otelcpp::LoggerShim;
namespace otel_logs = opentelemetry::logs;

TEST(OtelCppLoggerShim, GetNameReturnsTheResolvedScopeName)
{
    auto fake = std::make_shared<microtel::testing::FakeLogger>();
    LoggerShim shim{"my.scope", fake};

    EXPECT_EQ(shim.GetName(), "my.scope");
}

TEST(OtelCppLoggerShim, CreateThenEmitForwardsIntoMicrotelLogger)
{
    auto fake = std::make_shared<microtel::testing::FakeLogger>();
    LoggerShim shim{"my.scope", fake};

    auto record = shim.CreateLogRecord();
    record->SetSeverity(otel_logs::Severity::kError);
    record->SetBody(opentelemetry::common::AttributeValue{"disk full"});
    shim.EmitLogRecord(std::move(record));

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(fake->emitted[0].severity_number, microtel::SeverityNumber::Error);
    EXPECT_EQ(std::get<std::string>(fake->emitted[0].body), "disk full");
}

TEST(OtelCppLoggerShim, EmitNullLogRecordIsANoOp)
{
    auto fake = std::make_shared<microtel::testing::FakeLogger>();
    LoggerShim shim{"my.scope", fake};

    shim.EmitLogRecord(opentelemetry::nostd::unique_ptr<otel_logs::LogRecord>{});

    EXPECT_TRUE(fake->emitted.empty());
}

TEST(OtelCppLoggerShim, CreatedRecordsInheritTheLoggerOptions)
{
    auto fake = std::make_shared<microtel::testing::FakeLogger>();
    LoggerShim shim{
        "my.scope",
        fake,
        microtel::adapters::otelcpp::ShimOptions{.attribute_value_length_limit = std::uint32_t{4}}};
    const std::uint8_t over[] = {0xab, 0xcd, 0xef};

    auto record = shim.CreateLogRecord();
    record->SetAttribute("blob",
                         opentelemetry::common::AttributeValue{
                             opentelemetry::nostd::span<const std::uint8_t>{over, 3}});
    record->SetAttribute("host", opentelemetry::common::AttributeValue{"db"});
    shim.EmitLogRecord(std::move(record));

    ASSERT_EQ(fake->emitted.size(), 1U);
    ASSERT_EQ(fake->emitted[0].attributes.size(), 1U);
    EXPECT_EQ(fake->emitted[0].attributes[0].key, "host");
}

// ── LoggerProviderShim ────────────────────────────────────────────────────────

TEST(OtelCppLoggerProviderShim, NameDefaultsToLoggerNameWhenUnset)
{
    // Matches the reference SDK (logger_provider.cc): `if (name.empty())
    // name = logger_name;` — the common single-argument call site
    // `GetLogger("my.lib")` must produce a logger scoped "my.lib", not "".
    auto provider = std::make_shared<microtel::testing::FakeProvider>();
    LoggerProviderShim shim{provider};

    auto logger = shim.GetLogger("my.lib");

    ASSERT_EQ(provider->logger_requests.size(), 1U);
    EXPECT_EQ(provider->logger_requests[0].name, "my.lib");
    EXPECT_EQ(logger->GetName(), "my.lib");
}

TEST(OtelCppLoggerProviderShim, ExplicitNameOverridesLoggerName)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();
    LoggerProviderShim shim{provider};

    auto logger = shim.GetLogger("legacy-id", "my.lib", "2.0");

    ASSERT_EQ(provider->logger_requests.size(), 1U);
    EXPECT_EQ(provider->logger_requests[0].name, "my.lib");
    EXPECT_EQ(provider->logger_requests[0].version, "2.0");
}

TEST(OtelCppLoggerProviderShim, GlobalRegistrationRoutesOtelApiCallsToMicrotel)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();

    otel_logs::Provider::SetLoggerProvider(
        microtel::adapters::otelcpp::MakeLoggerProvider(provider));

    // Pure otel-cpp application code from here on.
    auto logger = otel_logs::Provider::GetLoggerProvider()->GetLogger("app.logs");
    auto record = logger->CreateLogRecord();
    record->SetBody(opentelemetry::common::AttributeValue{"started"});
    logger->EmitLogRecord(std::move(record));

    ASSERT_EQ(provider->logger_requests.size(), 1U);
    EXPECT_EQ(provider->logger_requests[0].name, "app.logs");
    ASSERT_EQ(provider->logger->emitted.size(), 1U);
    EXPECT_EQ(std::get<std::string>(provider->logger->emitted[0].body), "started");

    // Restore the noop default so no other test observes this global.
    otel_logs::Provider::SetLoggerProvider(
        opentelemetry::nostd::shared_ptr<otel_logs::LoggerProvider>{
            std::make_shared<otel_logs::NoopLoggerProvider>()});
}

}  // namespace
