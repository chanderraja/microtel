// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for M6-C: SdkBuilder::Build() and SdkProvider lifecycle.

#include "microtel/sdk_builder.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/sampler.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Build — validation / consumed guard
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, Build_MinimalConfig_Succeeds)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(*result, nullptr);
}

TEST(SdkBuilderTest, Build_GrpcProtocol_Succeeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4317")
                      .WithProtocol(microtel::Protocol::Grpc)
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, Build_AllOptions_Succeeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithProtocol(microtel::Protocol::Http)
                      .WithCompressionGzip(false)
                      .WithServiceName("svc")
                      .WithServiceVersion("2.0")
                      .WithBatch({.max_queue_size = 512, .max_export_batch_size = 64})
                      .WithSampler(microtel::MakeAlwaysOnSampler())
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, Build_CalledTwice_SecondCallReturnsConsumedError)
{
    microtel::SdkBuilder builder;
    builder.WithEndpoint("https://localhost:4318");
    (void)builder.Build();
    const auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::BuildAlreadyConsumed);
}

TEST(SdkBuilderTest, Build_EmptyEndpoint_ReturnsEndpointMalformed)
{
    const auto result = microtel::SdkBuilder().Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::EndpointMalformed);
}

TEST(SdkBuilderTest, Build_InvalidEndpointUrl_ReturnsEndpointMalformed)
{
    const auto result = microtel::SdkBuilder().WithEndpoint("not-a-url").Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::EndpointMalformed);
}

TEST(SdkBuilderTest, Build_GrpcWithPath_ReturnsProtocolMismatch)
{
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4317/v1/traces")
                            .WithProtocol(microtel::Protocol::Grpc)
                            .Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::ProtocolMismatch);
}

// ---------------------------------------------------------------------------
// Provider lifecycle
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, Provider_GetTracer_ReturnsNonNull)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto tracer = (*result)->GetTracer("test.lib", "1.0");
    EXPECT_NE(tracer, nullptr);
}

TEST(SdkBuilderTest, Provider_GetTracer_EmptyVersion_ReturnsNonNull)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto tracer = (*result)->GetTracer("test.lib");
    EXPECT_NE(tracer, nullptr);
}

TEST(SdkBuilderTest, Provider_ForceFlush_EmptyQueue_ReturnsCompleted)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto status = (*result)->ForceFlush(std::chrono::milliseconds(500));
    EXPECT_EQ(status, microtel::Status::Completed);
}

TEST(SdkBuilderTest, Provider_Shutdown_ReturnsCompleted)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto status = (*result)->Shutdown(std::chrono::milliseconds(500));
    EXPECT_EQ(status, microtel::Status::Completed);
}

TEST(SdkBuilderTest, Provider_Shutdown_Idempotent_SecondCallReturnsAlreadyShutDown)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    (void)(*result)->Shutdown(std::chrono::milliseconds(500));
    const auto status = (*result)->Shutdown(std::chrono::milliseconds(500));
    EXPECT_EQ(status, microtel::Status::AlreadyShutDown);
}

TEST(SdkBuilderTest, Provider_GetExporterHealth_ReturnsSnapshot)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    // Just verify it doesn't crash and returns a snapshot with a valid state.
    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(health.batches_sent, 0U);
}

// ---------------------------------------------------------------------------
// WithMetricInterval (M12)
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, WithMetricInterval_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricInterval(std::chrono::seconds(5))
                      .Build();
    ASSERT_TRUE(result.has_value());
}

// ---------------------------------------------------------------------------
// WithMetricTemporality (M12 — increment 21)
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, WithMetricTemporality_Delta_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricTemporality(microtel::TemporalityPreference::Delta)
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, WithMetricTemporality_LowMemory_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricTemporality(microtel::TemporalityPreference::LowMemory)
                      .Build();
    ASSERT_TRUE(result.has_value());
}

// ---------------------------------------------------------------------------
// WithMetricLimits (M12 increment 27)
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, WithMetricLimits_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricLimits(microtel::MetricLimitOptions{.max_cardinality = 5})
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, EnvVarOverridesDefaultCardinality)
{
    // RAII guard: ensure the env var is removed even if the test fails early.
    struct EnvGuard
    {
        EnvGuard() = default;
        EnvGuard(const EnvGuard&) = delete;
        EnvGuard& operator=(const EnvGuard&) = delete;
        EnvGuard(EnvGuard&&) = delete;
        EnvGuard& operator=(EnvGuard&&) = delete;
        ~EnvGuard() noexcept
        {
            (void)unsetenv("MICROTEL_METRIC_CARDINALITY_LIMIT");
        }
    } const guard;

    (void)setenv("MICROTEL_METRIC_CARDINALITY_LIMIT", "3", 1);

    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());

    auto meter = (*result)->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("c", "", "");

    // 4 distinct attribute sets with cap=3: the 4th must overflow.
    for (std::size_t i = 0; i < 4; ++i)
    {
        const microtel::KeyValue kv{.key = "i", .value = static_cast<std::int64_t>(i)};
        counter->Add(1, {&kv, 1});
    }

    constexpr auto kIdx = static_cast<std::size_t>(microtel::DropReason::CardinalityOverflow);
    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(health.drop_counters[kIdx], 1U);
}

TEST(SdkBuilderTest, WithMetricLimitsOverridesEnvVar)
{
    // WithMetricLimits has higher precedence than the env var.
    struct EnvGuard
    {
        EnvGuard() = default;
        EnvGuard(const EnvGuard&) = delete;
        EnvGuard& operator=(const EnvGuard&) = delete;
        EnvGuard(EnvGuard&&) = delete;
        EnvGuard& operator=(EnvGuard&&) = delete;
        ~EnvGuard() noexcept
        {
            (void)unsetenv("MICROTEL_METRIC_CARDINALITY_LIMIT");
        }
    } const guard;

    (void)setenv("MICROTEL_METRIC_CARDINALITY_LIMIT", "100", 1);

    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricLimits(microtel::MetricLimitOptions{.max_cardinality = 3})
                      .Build();
    ASSERT_TRUE(result.has_value());

    auto meter = (*result)->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("c", "", "");

    // cap is 3 (from WithMetricLimits, not 100 from env); 4th set overflows.
    for (std::size_t i = 0; i < 4; ++i)
    {
        const microtel::KeyValue kv{.key = "i", .value = static_cast<std::int64_t>(i)};
        counter->Add(1, {&kv, 1});
    }

    constexpr auto kIdx = static_cast<std::size_t>(microtel::DropReason::CardinalityOverflow);
    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(health.drop_counters[kIdx], 1U);
}

// ---------------------------------------------------------------------------
// End-to-end drop accounting — issue #169. Each of these reaches the counter
// the way an application does: through the public builder, tracer and
// GetExporterHealth(), with none of the SDK's seams mocked out.
// ---------------------------------------------------------------------------

namespace
{

std::uint64_t DropCount(const microtel::HealthSnapshot& health, microtel::DropReason reason)
{
    return health.drop_counters.at(static_cast<std::size_t>(reason));
}

/// Short deadlines so a provider pointed at a dead endpoint tears down in
/// milliseconds instead of burning the default 10s export deadline.
constexpr microtel::TimeoutOptions kFailFastTimeouts{
    .connect = std::chrono::milliseconds(200),
    .tls_handshake = std::chrono::milliseconds(200),
    .per_export = std::chrono::milliseconds(200),
    .retry_budget = std::chrono::milliseconds(200),
    .flush = std::chrono::milliseconds(500),
    .shutdown = std::chrono::milliseconds(500),
};

}  // namespace

TEST(SdkBuilderTest, EndToEnd_SpanLimitAndPostShutdownDropsReachGetExporterHealth)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("http://127.0.0.1:1")  // nothing listening; never connects
                      .WithTimeouts(kFailFastTimeouts)
                      .WithSpanLimits(microtel::SpanLimitOptions{
                          .attribute_count_limit = 1,
                          .event_count_limit = 1,
                          .link_count_limit = 1,
                      })
                      .Build();
    ASSERT_TRUE(result.has_value());

    const auto tracer = (*result)->GetTracer("test.lib", "1.0");
    // Shut the pipeline down first, so the finished span is refused at the
    // processor and never reaches the wire. The record-shaping counters are
    // recorded in the Span API, well before the processor sees the record, so
    // this exercises the full path without the test waiting out a retry loop
    // against a dead endpoint.
    ASSERT_NE((*result)->Shutdown(std::chrono::seconds(2)), microtel::Status::Failed);

    {
        auto span = tracer->StartSpan("op");
        span->SetAttribute("a", std::int64_t{1});
        span->SetAttribute("b", std::int64_t{2});  // over the limit
        span->AddEvent("e1");
        span->AddEvent("e2");  // over the limit
        span->AddLink(span->GetContext());
        span->AddLink(span->GetContext());  // over the limit
        span->End();
    }

    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(DropCount(health, microtel::DropReason::SpanAttributeLimit), 1U);
    EXPECT_EQ(DropCount(health, microtel::DropReason::SpanEventLimit), 1U);
    EXPECT_EQ(DropCount(health, microtel::DropReason::SpanLinkLimit), 1U);
    EXPECT_EQ(DropCount(health, microtel::DropReason::PostShutdown), 1U);
}

TEST(SdkBuilderTest, EndToEnd_ConnectFailureReachesGetExporterHealth)
{
    // Port 1 is reserved and never listening, so the connect fails fast
    // without depending on a collector being absent from a common port.
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("http://127.0.0.1:1")
                      .WithTimeouts(kFailFastTimeouts)
                      .Build();
    ASSERT_TRUE(result.has_value());

    ASSERT_FALSE((*result)->Connect().has_value());

    EXPECT_EQ(DropCount((*result)->GetExporterHealth(), microtel::DropReason::ConnectFailure), 1U);
}

// ---------------------------------------------------------------------------
// Build()-time warnings
//
// Neither configuration below is rejected: an h2c-capable proxy and the bench
// harness's own blackhole sink both make plaintext OTLP/HTTP legitimate, and
// `insecure = true` is permitted by spec §12.3. Both are, however, very likely
// to be a mistake, and `config::Validate` returns `Expected<void, ConfigError>`
// — it can reject but it cannot warn. These are the first production callers
// of the internal log channel.
// ---------------------------------------------------------------------------

namespace
{

/// Captures microtel's internal log emissions for the duration of a scope, and
/// restores the default routing on the way out.
class LogCapture
{
public:
    LogCapture()
    {
        microtel::SetLogSink([this](microtel::LogLevel level, std::string_view message)
                             { m_entries.emplace_back(level, std::string{message}); });
    }

    ~LogCapture()
    {
        microtel::ResetLogSink();
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    [[nodiscard]] bool WarnedAbout(std::string_view needle) const
    {
        return std::ranges::any_of(m_entries,
                                   [needle](const auto& entry)
                                   {
                                       return entry.first == microtel::LogLevel::Warn &&
                                              entry.second.find(needle) != std::string::npos;
                                   });
    }

private:
    std::vector<std::pair<microtel::LogLevel, std::string>> m_entries;
};

constexpr std::string_view kPlaintextNeedle = "plaintext OTLP/HTTP";
constexpr std::string_view kInsecureNeedle = "insecure";

}  // namespace

TEST(SdkBuilderTest, Build_PlaintextHttpEndpoint_WarnsWithoutRejecting)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("http://localhost:4318")
                            .WithProtocol(microtel::Protocol::Http)
                            .Build();

    ASSERT_TRUE(result.has_value()) << "h2c-capable receivers exist; this must not be rejected";
    EXPECT_TRUE(capture.WarnedAbout(kPlaintextNeedle));
    EXPECT_TRUE(capture.WarnedAbout("compatibility-matrix"))
        << "the warning must point somewhere the reader can act on";
}

TEST(SdkBuilderTest, Build_PlaintextGrpcEndpoint_DoesNotWarn)
{
    // gRPC is h2c by definition and the collector's gRPC receiver speaks it —
    // the whole gRPC conformance suite runs over `http://`.
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("http://localhost:4317")
                            .WithProtocol(microtel::Protocol::Grpc)
                            .Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(capture.WarnedAbout(kPlaintextNeedle));
}

TEST(SdkBuilderTest, Build_HttpsEndpoint_DoesNotWarnAboutPlaintext)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithProtocol(microtel::Protocol::Http)
                            .Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(capture.WarnedAbout(kPlaintextNeedle));
}

TEST(SdkBuilderTest, Build_InsecureTls_Warns)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithTls(microtel::TlsOptions{.insecure = true,
                                                          .ca_bundle = {},
                                                          .client_cert = {},
                                                          .client_key = {},
                                                          .sni_override = {}})
                            .Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(capture.WarnedAbout(kInsecureNeedle))
        << "spec §12.3 promises a prominent runtime warning for insecure = true";
}

TEST(SdkBuilderTest, Build_VerifiedTls_DoesNotWarnAboutInsecure)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(capture.WarnedAbout(kInsecureNeedle));
}
