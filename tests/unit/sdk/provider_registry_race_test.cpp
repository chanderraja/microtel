// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The multi-profile registry under contention — ICP 0027.
//
// Only meaningful under `-DMICROTEL_SANITIZER=tsan`; elsewhere it exercises
// the paths without being able to observe a race. What it puts under load:
//
//   - the slot array itself — registration claiming a slot, deregistration
//     releasing one, and `GetProvider` scanning all of them, all at once;
//   - the duplicate scan and the lowest-index-wins re-scan, driven by two
//     threads that never stop claiming the *same* name;
//   - `SdkProvider`'s own lifecycle — construction, `Shutdown`, destruction —
//     running beside every one of the above.
//
// What it deliberately does **not** do is destroy a *registered* provider
// while other threads are in the registry. ICP 0027 §4 states the contract
// plainly: a slot holds a borrowed pointer, and a lookup that loads a slot
// immediately before `~SdkProvider` clears it holds a pointer into a provider
// that is tearing down. That is the inherent race of a non-owning accessor,
// it is the caller's to synchronise, and a hammer that tripped it on purpose
// would be asserting that the documented contract is wrong rather than that
// the implementation honours it. So every provider that enters the registry
// here outlives every thread that could scan it; the lifecycle thread churns
// providers that never enter it.
//
// Duration. The default keeps the sanitizer CI job a CI job;
// `MICROTEL_HAMMER_SECONDS` extends it for a longer local run:
//
//     MICROTEL_HAMMER_SECONDS=120 ./build-tsan/tests/unit/sdk/provider_registry_race_test

#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/provider_registry.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

constexpr int kDefaultSeconds = 3;
constexpr std::size_t kStableProfiles = 3;
constexpr std::size_t kChurnThreads = 2;
constexpr std::size_t kContendedThreads = 2;
constexpr std::size_t kLookupThreads = 3;
constexpr auto kShutdownTimeout = std::chrono::milliseconds(50);

// Every slot the hammer can hold at once: the stable set, one per churn
// thread, and — transiently, between the CAS claim and the losing re-scan —
// one per contended claimant. Under the capacity, so a refusal in this test
// always means a defect and never a full registry.
static_assert(kStableProfiles + kChurnThreads + kContendedThreads <= mts::kMaxProfiles,
              "the hammer must never be refused for capacity");

std::chrono::seconds HammerBudget()
{
    const char* const raw = std::getenv("MICROTEL_HAMMER_SECONDS");
    if (raw == nullptr)
    {
        return std::chrono::seconds{kDefaultSeconds};
    }
    const long parsed = std::strtol(raw, nullptr, 10);
    if (parsed <= 0)
    {
        return std::chrono::seconds{kDefaultSeconds};
    }
    return std::chrono::seconds{parsed};
}

std::unique_ptr<mts::SdkProvider> MakeProvider(std::string name)
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
        .profile_name = std::move(name),
    });
}

std::string StableName(std::size_t i)
{
    return "stable-" + std::to_string(i);
}

using Deadline = std::chrono::steady_clock::time_point;
using ProviderList = std::vector<std::unique_ptr<mts::SdkProvider>>;

[[nodiscard]] bool Running(Deadline deadline) noexcept
{
    return std::chrono::steady_clock::now() < deadline;
}

/// Scan for names that are registered for the whole run, and one that never is.
void LookupLoop(Deadline deadline, std::atomic<std::size_t>& lookups)
{
    while (Running(deadline))
    {
        for (std::size_t j = 0; j < kStableProfiles; ++j)
        {
            EXPECT_NE(mt::GetProvider(StableName(j)), nullptr);
        }
        EXPECT_EQ(mt::GetProvider("no-such-profile"), nullptr);
        lookups.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Take and release one slot, under a name no other thread claims.
void ChurnLoop(Deadline deadline, mts::SdkProvider* provider, const std::string& name)
{
    while (Running(deadline))
    {
        ASSERT_EQ(mts::RegisterProvider(provider), mts::RegistrationResult::Registered);
        EXPECT_EQ(mt::GetProvider(name), provider);
        mts::DeregisterProvider(provider);
    }
}

/// One name, two claimants, forever: the pair that has to reach the CAS and the
/// re-scan rather than settling it in the sequential duplicate scan.
void ContendLoop(Deadline deadline, mts::SdkProvider* provider, std::atomic<std::size_t>& claims)
{
    while (Running(deadline))
    {
        if (mts::RegisterProvider(provider) == mts::RegistrationResult::Registered)
        {
            EXPECT_NE(mt::GetProvider("contended"), nullptr);
            mts::DeregisterProvider(provider);
            claims.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

/// Provider construction, Shutdown and destruction, beside every registry
/// operation above. These never enter the registry, so nothing else can hold a
/// pointer to them (ICP 0027 §4).
void LifecycleLoop(Deadline deadline, std::atomic<std::size_t>& lifecycles)
{
    while (Running(deadline))
    {
        auto transient = MakeProvider("never-registered");
        EXPECT_NE(transient->Shutdown(kShutdownTimeout), mt::Status::Failed);
        transient.reset();
        lifecycles.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Shutdown on the providers the lookup threads are handing out. Idempotent
/// after the first, which is the point: it keeps running.
void ShutdownLoop(Deadline deadline, const ProviderList& stable)
{
    while (Running(deadline))
    {
        for (const auto& provider : stable)
        {
            (void)provider->ForceFlush(kShutdownTimeout);
            (void)provider->Shutdown(kShutdownTimeout);
        }
    }
}

std::string ChurnName(std::size_t i)
{
    return "churn-" + std::to_string(i);
}

/// @p count providers, none of them registered, all carrying @p name — the
/// shape the contended claimants need.
ProviderList MakeProviders(std::size_t count, const std::string& name)
{
    ProviderList providers;
    providers.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        providers.push_back(MakeProvider(name));
    }
    return providers;
}

/// @p count providers, none of them registered, each with its own name.
ProviderList MakeChurnProviders(std::size_t count)
{
    ProviderList providers;
    providers.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        providers.push_back(MakeProvider(ChurnName(i)));
    }
    return providers;
}

}  // namespace

TEST(ProviderRegistryRaceTest, RegisterLookupDeregisterAndLifecycleRaceCleanly)
{
    const auto deadline = std::chrono::steady_clock::now() + HammerBudget();

    // Every provider the registry can see is constructed here and destroyed
    // after every thread has joined — see the file header.
    ProviderList stable;
    stable.reserve(kStableProfiles);
    for (std::size_t i = 0; i < kStableProfiles; ++i)
    {
        auto provider = MakeProvider(StableName(i));
        ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered);
        stable.push_back(std::move(provider));
    }

    ProviderList churn = MakeChurnProviders(kChurnThreads);
    ProviderList contended = MakeProviders(kContendedThreads, "contended");

    std::atomic<std::size_t> lookups{0};
    std::atomic<std::size_t> claims{0};
    std::atomic<std::size_t> lifecycles{0};
    std::vector<std::thread> threads;
    threads.reserve(kLookupThreads + kChurnThreads + kContendedThreads + 2);

    for (std::size_t i = 0; i < kLookupThreads; ++i)
    {
        threads.emplace_back([&] { LookupLoop(deadline, lookups); });
    }
    for (std::size_t i = 0; i < kChurnThreads; ++i)
    {
        threads.emplace_back([&, i] { ChurnLoop(deadline, churn[i].get(), ChurnName(i)); });
    }
    for (std::size_t i = 0; i < kContendedThreads; ++i)
    {
        threads.emplace_back([&, i] { ContendLoop(deadline, contended[i].get(), claims); });
    }
    threads.emplace_back([&] { LifecycleLoop(deadline, lifecycles); });
    threads.emplace_back([&] { ShutdownLoop(deadline, stable); });

    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_GT(lookups.load(), 0U);
    EXPECT_GT(claims.load(), 0U);
    EXPECT_GT(lifecycles.load(), 0U);

    // The registry is exactly as the stable set left it: the churning threads
    // released every slot they took.
    for (std::size_t i = 0; i < kStableProfiles; ++i)
    {
        EXPECT_EQ(mt::GetProvider(StableName(i)), stable[i].get());
    }
    EXPECT_EQ(mt::GetProvider("contended"), nullptr);
    for (std::size_t i = 0; i < kChurnThreads; ++i)
    {
        EXPECT_EQ(mt::GetProvider("churn-" + std::to_string(i)), nullptr);
    }
}
