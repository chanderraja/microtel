// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers RegisterGlobally: the one-call startup path that registers all
// three otel-cpp global providers (trace, metrics, logs) over a single
// microtel::Provider, and UnregisterGlobally, which restores the noop
// defaults.

#include "adapters/otelcpp/global_registration.hpp"
#include "adapters/otelcpp/shim_options.hpp"
#include "fakes/fake_provider.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

TEST_F(GlobalRegistrationTest, SecondRegistrationWinsOnlyForObjectsObtainedAfterIt)
{
    // ICP 0033 §6: options are copied into each object at creation. A second
    // RegisterGlobally governs what is obtained through the globals after it;
    // a tracer, meter or logger obtained before it keeps the first options.
    using microtel::adapters::otelcpp::ShimOptions;
    auto provider = std::make_shared<microtel::testing::FakeProvider>();
    const std::uint8_t three_bytes[] = {0x01, 0x02, 0x03};
    const opentelemetry::common::AttributeValue blob{
        opentelemetry::nostd::span<const std::uint8_t>{three_bytes, 3}};

    microtel::adapters::otelcpp::RegisterGlobally(
        provider, ShimOptions{.attribute_value_length_limit = std::uint32_t{4}});
    auto old_tracer = otel_trace::Provider::GetTracerProvider()->GetTracer("app");
    auto old_meter = otel_metrics::Provider::GetMeterProvider()->GetMeter("app");
    auto old_logger = otel_logs::Provider::GetLoggerProvider()->GetLogger("app");

    microtel::adapters::otelcpp::RegisterGlobally(
        provider, ShimOptions{.attribute_value_length_limit = std::nullopt});
    auto new_tracer = otel_trace::Provider::GetTracerProvider()->GetTracer("app");
    auto new_meter = otel_metrics::Provider::GetMeterProvider()->GetMeter("app");
    auto new_logger = otel_logs::Provider::GetLoggerProvider()->GetLogger("app");

    auto old_span = old_tracer->StartSpan("old");
    old_span->SetAttribute("blob", blob);
    auto new_span = new_tracer->StartSpan("new");
    new_span->SetAttribute("blob", blob);

    old_meter->CreateDoubleCounter("old")->Add(1.0, {{"blob", blob}});
    new_meter->CreateDoubleCounter("new")->Add(1.0, {{"blob", blob}});

    auto old_record = old_logger->CreateLogRecord();
    old_record->SetAttribute("blob", blob);
    old_logger->EmitLogRecord(std::move(old_record));
    auto new_record = new_logger->CreateLogRecord();
    new_record->SetAttribute("blob", blob);
    new_logger->EmitLogRecord(std::move(new_record));

    ASSERT_EQ(provider->tracer->spans.size(), 2U);
    EXPECT_TRUE(provider->tracer->spans[0]->attributes.empty());
    EXPECT_EQ(provider->tracer->spans[1]->attributes.size(), 1U);

    ASSERT_EQ(provider->meter->counters_double.size(), 2U);
    ASSERT_EQ(provider->meter->counters_double[0]->calls.size(), 1U);
    EXPECT_TRUE(provider->meter->counters_double[0]->calls[0].attributes.empty());
    ASSERT_EQ(provider->meter->counters_double[1]->calls.size(), 1U);
    EXPECT_EQ(provider->meter->counters_double[1]->calls[0].attributes.size(), 1U);

    ASSERT_EQ(provider->logger->emitted.size(), 2U);
    EXPECT_TRUE(provider->logger->emitted[0].attributes.empty());
    EXPECT_EQ(provider->logger->emitted[1].attributes.size(), 1U);
}

}  // namespace
