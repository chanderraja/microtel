// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// docs/threading-model.md §7 (LOCKED) says a pthread_atfork handler runs in
// the child and marks every live Provider dead, so that "any future API call
// observes a closed state". No handler was ever registered — the rule was
// normative in two documents and implemented nowhere.
//
// "Every live Provider" is what `docs/sequences/fork-survival.md` has said
// since M0, and until ICP 0027 the handler could only reach one: a single
// `g_live_provider` slot that the second provider's constructor overwrote,
// leaving the first live, unmarked, and reachable from the child. The
// multi-provider test below is that scenario; it failed with exit code 31 —
// "the first provider is still live in the child" — before the registry
// landed.
//
// These tests actually fork. Anything less would assert that the code compiles
// rather than that the handler runs.

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/provider_registry.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

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
constexpr int kChildFirstProviderLive = 31;
constexpr int kChildSecondProviderLive = 32;
constexpr int kChildSlotNotCleared = 33;
constexpr int kChildRebuildFailed = 34;

// Nothing here connects; the endpoint only has to parse.
constexpr const char* kTestEndpoint = "https://localhost:4318";

std::unique_ptr<mts::SdkProvider> MakeForkTestProvider(std::string profile_name)
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
        .profile_name = std::move(profile_name),
    });
}

/// The same provider, in the registry the fork handler walks.
///
/// Registration is `SdkBuilder::Build`'s job since ICP 0027 — the constructor
/// cannot do it, because it can fail and the constructor is `noexcept` — so a
/// test that builds its provider from mocks registers it itself. The slot is
/// released by `~SdkProvider`, before it shuts anything down.
std::unique_ptr<mts::SdkProvider> MakeRegisteredForkTestProvider(std::string profile_name)
{
    auto provider = MakeForkTestProvider(std::move(profile_name));
    EXPECT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered);
    return provider;
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
    auto provider = MakeRegisteredForkTestProvider("fork-single");
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

// Several live providers, one fork. `docs/sequences/fork-survival.md`
// annotation 3 has specified this since M0 — "the child handler immediately
// CAS-flips **every** live Provider" — and the single-slot handler could not
// do it: the second provider displaced the first, and a child touching the
// first could still reach `BatchSpanProcessor::OnEnd` and its mutex.
TEST(ForkSafetyTest, ChildSeesEveryLiveProviderMarked)
{
    auto first = MakeRegisteredForkTestProvider("fork-alpha");
    auto second = MakeRegisteredForkTestProvider("fork-beta");
    // Both pipelines are genuinely live in the parent, so the child's verdict
    // is attributable to the handler rather than to an inert provider.
    ASSERT_NE(first->GetLogger("parent", "1.0"), first->GetLogger("parent-2", "1.0"));
    ASSERT_NE(second->GetLogger("parent", "1.0"), second->GetLogger("parent-2", "1.0"));

    const int rc = RunInChild(
        [&first, &second]
        {
            if (first->GetLogger("child", "1.0") != first->GetLogger("child-2", "1.0"))
            {
                return kChildFirstProviderLive;
            }
            if (second->GetLogger("child", "1.0") != second->GetLogger("child-2", "1.0"))
            {
                return kChildSecondProviderLive;
            }
            return kChildOk;
        });

    EXPECT_EQ(rc, kChildOk);
}

// The other half of ICP 0027 §3: the handler clears the slots as well as
// marking them, so the child's registry is as empty as a fresh process's and
// the child can re-`Build()` (fork-survival option A) under the very names the
// parent was using. Keeping the stale entries would have failed that re-init
// with a duplicate-name ConfigError.
TEST(ForkSafetyTest, ChildRebuildsUnderTheParentsProfileNames)
{
    auto first = MakeRegisteredForkTestProvider("fork-rebuild-a");
    auto second = MakeRegisteredForkTestProvider("fork-rebuild-b");
    ASSERT_EQ(mt::GetProvider("fork-rebuild-a"), first.get());
    ASSERT_EQ(mt::GetProvider("fork-rebuild-b"), second.get());

    const int rc = RunInChild(
        []
        {
            if (mt::GetProvider("fork-rebuild-a") != nullptr ||
                mt::GetProvider("fork-rebuild-b") != nullptr)
            {
                return kChildSlotNotCleared;
            }
            auto rebuilt_a = mt::SdkBuilder()
                                 .WithEndpoint(kTestEndpoint)
                                 .WithProfileName("fork-rebuild-a")
                                 .Build();
            auto rebuilt_b = mt::SdkBuilder()
                                 .WithEndpoint(kTestEndpoint)
                                 .WithProfileName("fork-rebuild-b")
                                 .Build();
            if (!rebuilt_a.has_value() || !rebuilt_b.has_value())
            {
                return kChildRebuildFailed;
            }
            return (mt::GetProvider("fork-rebuild-a") == rebuilt_a->get()) ? kChildOk
                                                                           : kChildRebuildFailed;
        });

    EXPECT_EQ(rc, kChildOk);

    // The parent's registry is untouched: a fork marks the child's copy.
    EXPECT_EQ(mt::GetProvider("fork-rebuild-a"), first.get());
    EXPECT_EQ(mt::GetProvider("fork-rebuild-b"), second.get());
}

// The parent must be unaffected: fork marks the child's copy, not ours.
TEST(ForkSafetyTest, ParentRemainsUsableAfterFork)
{
    auto provider = MakeRegisteredForkTestProvider("fork-parent");

    const int rc = RunInChild([] { return kChildOk; });
    ASSERT_EQ(rc, kChildOk);

    auto a = provider->GetLogger("parent-after", "1.0");
    auto b = provider->GetLogger("parent-after-2", "1.0");
    EXPECT_NE(a, b) << "parent's provider was marked shut down by its own fork";
    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(200)), mt::Status::Completed);
}
