// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkProvider::GetLogger — M14 L5 (ICP 0012, docs/logs-design.md §8).
//
// Contract under test:
//  - GetLogger() returns a non-null shared_ptr<Logger>.
//  - Same (name, version) → same pointer (identity cache); different → different.
//  - With no log exporter configured, GetLogger returns a no-op logger and
//    Emit does not crash / exports nothing.
//  - Emitting through a logger reaches the configured exporter after ForceFlush.
//  - ForceFlush / Shutdown flush / shut down the log exporter.

#include "microtel/internal/sampler.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_log_exporter.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mtm = microtel::testing;

using namespace std::chrono_literals;

namespace
{

std::unique_ptr<mts::SdkProvider> MakeProvider(std::unique_ptr<mti::ILogExporter> log_exp)
{
    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::make_unique<mtm::MockExporter>(),
        .processor = std::make_unique<mtm::MockSpanProcessor>(),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .log_exporter = std::move(log_exp),
    });
}

std::size_t TotalRecords(const mtm::FakeLogExporter& exp)
{
    std::size_t total = 0;
    for (const auto& handle : exp.exported)
    {
        total += handle.Records().size();
    }
    return total;
}

TEST(SdkProviderLoggerTest, GetLoggerReturnsNonNull)
{
    auto provider = MakeProvider(std::make_unique<mtm::FakeLogExporter>());
    EXPECT_NE(provider->GetLogger("my.lib"), nullptr);
}

TEST(SdkProviderLoggerTest, SameScopeReturnsSameInstance)
{
    auto provider = MakeProvider(std::make_unique<mtm::FakeLogExporter>());
    auto a = provider->GetLogger("my.lib", "1.0");
    auto b = provider->GetLogger("my.lib", "1.0");
    EXPECT_EQ(a.get(), b.get());
}

TEST(SdkProviderLoggerTest, DifferentScopeReturnsDifferentInstance)
{
    auto provider = MakeProvider(std::make_unique<mtm::FakeLogExporter>());
    EXPECT_NE(provider->GetLogger("lib.a").get(), provider->GetLogger("lib.b").get());
    EXPECT_NE(provider->GetLogger("lib", "1.0").get(), provider->GetLogger("lib", "2.0").get());
}

TEST(SdkProviderLoggerTest, NoExporterReturnsNoOpLogger)
{
    auto provider = MakeProvider(nullptr);  // no log exporter configured
    auto logger = provider->GetLogger("my.lib");
    ASSERT_NE(logger, nullptr);
    // No-op: emitting must not crash and there is no pipeline to export to.
    logger->Emit(mt::LogRecord{});
    EXPECT_EQ(provider->ForceFlush(500ms), mt::Status::Completed);
}

TEST(SdkProviderLoggerTest, EmitReachesExporterAfterForceFlush)
{
    auto exp = std::make_unique<mtm::FakeLogExporter>();
    const auto* exp_ptr = exp.get();
    auto provider = MakeProvider(std::move(exp));

    auto logger = provider->GetLogger("my.lib");
    logger->Emit(mt::LogRecord{});
    logger->Emit(mt::LogRecord{});

    EXPECT_EQ(provider->ForceFlush(2000ms), mt::Status::Completed);
    EXPECT_EQ(TotalRecords(*exp_ptr), 2U);
}

TEST(SdkProviderLoggerTest, ForceFlushFlushesLogExporter)
{
    auto exp = std::make_unique<mtm::FakeLogExporter>();
    const auto* exp_ptr = exp.get();
    auto provider = MakeProvider(std::move(exp));

    (void)provider->GetLogger("my.lib");  // build the pipeline
    EXPECT_EQ(provider->ForceFlush(500ms), mt::Status::Completed);
    EXPECT_GE(exp_ptr->force_flush_call_count, 1);
}

TEST(SdkProviderLoggerTest, ShutdownShutsDownLogExporter)
{
    auto exp = std::make_unique<mtm::FakeLogExporter>();
    const auto* exp_ptr = exp.get();
    auto provider = MakeProvider(std::move(exp));

    (void)provider->GetLogger("my.lib");
    EXPECT_EQ(provider->Shutdown(500ms), mt::Status::Completed);
    EXPECT_GE(exp_ptr->shutdown_call_count, 1);
}

}  // namespace
