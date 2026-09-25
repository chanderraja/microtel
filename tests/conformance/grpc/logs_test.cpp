// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// OTLP/gRPC logs (`opentelemetry.proto.collector.logs.v1.LogsService/Export`)
// against a real collector.
//
// The scenarios live in conformance/support/log_conformance.hpp and are shared
// with the OTLP/HTTP suite; this file only decides how the Provider reaches the
// collector. Like grpc/basic_export_test.cpp it uses the plaintext receiver:
// gRPC is h2c by definition, so the quick-start configuration works here.
//
// Assertions read the collector's logs.jsonl, which is downstream of its
// protobuf decode — the same readback the trace suite does against
// traces.jsonl.

#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include "conformance/support/conformance_env.hpp"
#include "conformance/support/log_conformance.hpp"
#include "conformance/support/provider_builder.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace
{

namespace logs = microtel::testing::logs;

constexpr const char* kEndpointEnv = "MICROTEL_CONFORMANCE_GRPC_ENDPOINT";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kScopeName = "microtel.conformance.grpc.logs";

/// @brief Where a test sends and where it reads back.
struct Target
{
    std::string endpoint;
    std::string output_file;
};

/// @brief Resolves the endpoint and logs output file, or reports why not.
/// @return false when the caller must `GTEST_SKIP()`.
bool ResolveTarget(Target& target)
{
    return microtel::testing::ConformanceEnabled(kEndpointEnv, target.endpoint) &&
           microtel::testing::ConformanceEnabled(logs::kLogsOutputFileEnv, target.output_file);
}

/// @brief Builds a provider aimed at the collector's plaintext OTLP/gRPC receiver.
///
/// @param builder  caller-owned; `SdkBuilder` cannot be returned by value.
/// @param endpoint resolved from @ref kEndpointEnv.
/// @param gzip     request per-message `grpc-encoding: gzip`.
/// @return the provider, or null after recording a failure.
std::shared_ptr<microtel::Provider> BuildProvider(microtel::SdkBuilder& builder,
                                                  const std::string& endpoint,
                                                  bool gzip = false)
{
    auto result =
        microtel::testing::ConfigureConformanceBuilder(builder, endpoint, microtel::Protocol::Grpc)
            .WithServiceName(logs::kServiceName)
            .WithCompressionGzip(gzip)
            .Build();
    if (!result.has_value())
    {
        ADD_FAILURE() << result.error().message;
        return nullptr;
    }
    return std::move(*result);
}

TEST(GrpcLogsConformance, RecordRoundTrip)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    logs::RunRecordRoundTrip(*provider, target.output_file, kScopeName);
}

TEST(GrpcLogsConformance, EverySeverity)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    logs::RunEverySeverity(*provider, target.output_file, kScopeName);
}

TEST(GrpcLogsConformance, BodyTypes)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    logs::RunBodyTypes(*provider, target.output_file, kScopeName);
}

TEST(GrpcLogsConformance, ObservedTimeBackfilled)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    logs::RunObservedTimeBackfill(*provider, target.output_file, kScopeName);
}

TEST(GrpcLogsConformance, TraceCorrelation)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    logs::RunTraceCorrelation(*provider, target.output_file, kScopeName);
}

TEST(GrpcLogsConformance, GzipAccepted)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint, /*gzip=*/true);
    ASSERT_NE(provider, nullptr);
    logs::RunGzipDelivery(*provider, target.output_file, kScopeName);
}

}  // namespace
