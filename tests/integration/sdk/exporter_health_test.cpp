// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Proves the diagnostics sink actually reaches the export pipeline through a
// real SdkBuilder::Build().
//
// Two independent bugs used to make this impossible: OtlpExporter stored its
// IDiagnosticsSink* as [[maybe_unused]] and never called it, and SdkBuilder
// never passed one in at all. Every counter behind GetExporterHealth() read
// zero no matter what the pipeline did, so an operator debugging a failing
// export got silence. Unit coverage of the recording logic lives in
// tests/unit/exporter/otlp_exporter_test.cpp; this test exists to catch the
// *wiring* regressing, which a unit test with a hand-injected sink cannot see.
//
// Integration tier because it opens a real socket and takes seconds.

#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <utility>

namespace
{

// Port 1 is reliably closed for a non-root process, so the lazy connect
// (ICP 0017) fails rather than hanging on an unroutable address.
constexpr const char* kClosedPortEndpoint = "http://127.0.0.1:1";

TEST(ExporterHealthIntegrationTest, FailedExportIsVisibleInHealthSnapshot)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint(kClosedPortEndpoint)
                      .WithTimeouts(microtel::TimeoutOptions{
                          .connect = std::chrono::milliseconds(200),
                          .tls_handshake = std::chrono::milliseconds(200),
                          .per_export = std::chrono::milliseconds(200),
                          // Near-zero budget: stop after the first retry rather
                          // than sleeping through the full backoff schedule.
                          .retry_budget = std::chrono::milliseconds(1),
                          .flush = std::chrono::seconds(30),
                          .shutdown = std::chrono::seconds(10),
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // Deliberately no Connect() — the export path connects lazily (ICP 0017).
    {
        auto tracer = provider->GetTracer("diag-wiring", "1.0");
        auto span = tracer->StartSpan("doomed-export");
        span->End();
    }
    ASSERT_EQ(provider->ForceFlush(std::chrono::seconds(30)), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GT(health.batches_failed, 0U)
        << "an export to a closed port must be visible in GetExporterHealth()";
    EXPECT_FALSE(health.last_error_message.empty())
        << "an operator reading health must be told why the export failed";
    EXPECT_EQ(health.batches_sent, 0U);
    EXPECT_EQ(health.queue_depth_now, 0U) << "queue must drain, not stay at high-water mark";
}

}  // namespace
