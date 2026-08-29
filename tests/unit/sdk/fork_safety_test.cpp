// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// docs/threading-model.md §7 (LOCKED) says a pthread_atfork handler runs in
// the child and marks every live Provider dead, so that "any future API call
// observes a closed state". No handler was ever registered — the rule was
// normative in two documents and implemented nowhere.
//
// These tests actually fork. Anything less would assert that the code compiles
// rather than that the handler runs.

#include "microtel/internal/sampler.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include <sys/wait.h>
#include <unistd.h>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

// Exit codes the child uses to report what it observed. Distinct values so a
// failure says which expectation broke, not merely that one did.
constexpr int kChildOk = 0;
constexpr int kChildLoggerNotNoop = 21;

std::unique_ptr<mts::SdkProvider> MakeForkTestProvider()
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
        // A log exporter is required, or GetLogger returns the noop logger
        // unconditionally and the child's check would pass without the fix.
        .log_exporter = std::make_unique<mtm::MockLogExporter>(),
    });
}

/// Run @p child_body in a forked child; return its exit status.
int RunInChild(const std::function<int()>& child_body)
{
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        ::_exit(child_body());
    }
    EXPECT_GT(pid, 0) << "fork failed";
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// Before the handler existed, the child inherited a provider that still looked
// live and would happily build a BatchLogRecordProcessor — spawning a worker
// thread in a child whose other threads do not exist.
TEST(ForkSafetyTest, ChildSeesTheProviderAsShutDown)
{
    auto provider = MakeForkTestProvider();
    // Prove the parent's pipeline is genuinely live, so the child's result is
    // attributable to the fork handler rather than to an inert provider.
    ASSERT_NE(provider->GetLogger("parent", "1.0"), provider->GetLogger("noop-check", "1.0"));

    const int rc = RunInChild(
        [&provider]
        {
            auto logger = provider->GetLogger("child", "1.0");
            // Post-fork the provider is marked shut down, so every GetLogger
            // returns the one shared noop instance.
            auto again = provider->GetLogger("child-2", "1.0");
            return (logger == again) ? kChildOk : kChildLoggerNotNoop;
        });

    EXPECT_EQ(rc, kChildOk);
}

// The parent must be unaffected: fork marks the child's copy, not ours.
TEST(ForkSafetyTest, ParentRemainsUsableAfterFork)
{
    auto provider = MakeForkTestProvider();

    const int rc = RunInChild([] { return kChildOk; });
    ASSERT_EQ(rc, kChildOk);

    auto a = provider->GetLogger("parent-after", "1.0");
    auto b = provider->GetLogger("parent-after-2", "1.0");
    EXPECT_NE(a, b) << "parent's provider was marked shut down by its own fork";
    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(200)), mt::Status::Completed);
}
