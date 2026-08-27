// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers RegisterGlobally: the one-call startup path that registers all
// three otel-cpp global providers (trace, metrics, logs) over a single
// microtel::Provider, and UnregisterGlobally, which restores the noop
// defaults.

#include "adapters/otelcpp/global_registration.hpp"
#include "fakes/fake_provider.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include <opentelemetry/logs/provider.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/trace/provider.h>

namespace
{

namespace otel_trace = opentelemetry::trace;
namespace otel_metrics = opentelemetry::metrics;
namespace otel_logs = opentelemetry::logs;

/// Every test restores the noop defaults on exit so global state never
/// leaks between tests, regardless of what RegisterGlobally leaves behind.
class GlobalRegistrationTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        microtel::adapters::otelcpp::UnregisterGlobally();
    }
};

TEST_F(GlobalRegistrationTest, RegistersAllThreeSignalsOverOneProvider)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();

    microtel::adapters::otelcpp::RegisterGlobally(provider);

    // Pure otel-cpp application code from here on, across all three signals.
    auto tracer = otel_trace::Provider::GetTracerProvider()->GetTracer("app");
    auto span = tracer->StartSpan("op");
    span->End();

    auto meter = otel_metrics::Provider::GetMeterProvider()->GetMeter("app");
    auto counter = meter->CreateUInt64Counter("hits");
    counter->Add(1U);

    auto logger = otel_logs::Provider::GetLoggerProvider()->GetLogger("app");
    auto record = logger->CreateLogRecord();
    record->SetBody(opentelemetry::common::AttributeValue{"started"});
    logger->EmitLogRecord(std::move(record));

    EXPECT_EQ(provider->tracer_requests.size(), 1U);
    ASSERT_EQ(provider->tracer->spans.size(), 1U);
    EXPECT_EQ(provider->tracer->spans[0]->end_calls.size(), 1U);

    EXPECT_EQ(provider->meter_requests.size(), 1U);
    ASSERT_EQ(provider->meter->counters_i64.size(), 1U);
    EXPECT_EQ(provider->meter->counters_i64[0]->calls.size(), 1U);

    EXPECT_EQ(provider->logger_requests.size(), 1U);
    EXPECT_EQ(provider->logger->emitted.size(), 1U);
}

TEST_F(GlobalRegistrationTest, UnregisterGloballyRestoresNoopDefaults)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();
    microtel::adapters::otelcpp::RegisterGlobally(provider);

    microtel::adapters::otelcpp::UnregisterGlobally();

    // A noop tracer must not touch the microtel provider at all.
    auto tracer = otel_trace::Provider::GetTracerProvider()->GetTracer("app");
    auto span = tracer->StartSpan("op");
    span->End();

    EXPECT_TRUE(provider->tracer_requests.empty());
}

}  // namespace
