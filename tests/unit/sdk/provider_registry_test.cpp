// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The multi-profile registry — ICP 0027.
//
// Two surfaces, one data structure. Below the line, `RegisterProvider` /
// `DeregisterProvider` / `FindProvider` over a fixed array of atomic slots.
// Above it, `SdkBuilder::WithProfileName` and the free function
// `microtel::GetProvider`, which is all a consumer ever sees.
//
// The registry-level tests build their providers directly from mocks rather
// than through `SdkBuilder::Build()`, because filling `kMaxProfiles` slots
// through `Build()` would spawn five worker threads per profile to test an
// array of pointers. The builder-level tests below use the real `Build()`
// exactly where the contract is about `Build()`: the two `ConfigError`
// enumerators, and the registration that happens after construction.
//
// The one thing neither half can assert is the inherent race of a non-owning
// accessor: a `GetProvider` that loads a slot immediately before the
// destructor clears it holds a pointer to a provider that is tearing down.
// ICP 0027 §4 documents that as the caller's to synchronise, and this file
// keeps to the documented usage — look a profile up, hold the pointer, destroy
// providers at teardown.

#include "sdk/provider_registry.hpp"

#include "microtel/error.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

constexpr auto kTimeout = std::chrono::milliseconds(200);
constexpr const char* kTestEndpoint = "https://localhost:4318";

/// A provider with mock pipelines, named @p name, not yet registered.
///
/// Registration is deliberately a separate step: it is `SdkBuilder::Build`'s
/// job in production (ICP 0027 §2), not the constructor's, and a test that
/// wants an unregistered provider is exactly how that is proved.
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

/// A provider with the default profile name — what a v1.0 program builds.
std::unique_ptr<mts::SdkProvider> MakeDefaultProvider()
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
    });
}

/// Registered providers, deregistered in reverse order when the vector dies.
using Registered = std::vector<std::unique_ptr<mts::SdkProvider>>;

}  // namespace

// ---------------------------------------------------------------------------
// The slots
// ---------------------------------------------------------------------------

TEST(ProviderRegistryTest, RegisteredProviderIsFoundByName)
{
    auto provider = MakeProvider("alpha");
    ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered);

    EXPECT_EQ(mt::GetProvider("alpha"), provider.get());
    EXPECT_EQ(mt::GetProvider("beta"), nullptr);
}

// A provider that was constructed but never registered is invisible: the
// constructor does not touch the registry, because registration can fail and
// the constructor is noexcept (ICP 0027 §2).
TEST(ProviderRegistryTest, ConstructionAloneDoesNotRegister)
{
    auto provider = MakeProvider("unregistered");
    EXPECT_EQ(mt::GetProvider("unregistered"), nullptr);
}

TEST(ProviderRegistryTest, UnnamedProviderCarriesTheDefaultProfileName)
{
    auto provider = MakeDefaultProvider();
    ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered);

    EXPECT_EQ(provider->ProfileName(), mt::kDefaultProfileName);
    // The defaulted argument is the default-profile accessor.
    EXPECT_EQ(mt::GetProvider(), provider.get());
    EXPECT_EQ(mt::GetProvider(mt::kDefaultProfileName), provider.get());
}

TEST(ProviderRegistryTest, DuplicateNameIsRefusedAndTheFirstProviderKeepsTheName)
{
    auto first = MakeProvider("audit");
    ASSERT_EQ(mts::RegisterProvider(first.get()), mts::RegistrationResult::Registered);

    auto second = MakeProvider("audit");
    EXPECT_EQ(mts::RegisterProvider(second.get()), mts::RegistrationResult::DuplicateName);
    EXPECT_EQ(mt::GetProvider("audit"), first.get());
}

// Byte-for-byte comparison, no normalisation and no case folding — the
// `WithProfileName` contract.
TEST(ProviderRegistryTest, NamesAreComparedByteForByte)
{
    auto lower = MakeProvider("audit");
    ASSERT_EQ(mts::RegisterProvider(lower.get()), mts::RegistrationResult::Registered);

    auto upper = MakeProvider("AUDIT");
    EXPECT_EQ(mts::RegisterProvider(upper.get()), mts::RegistrationResult::Registered);
    EXPECT_EQ(mt::GetProvider("AUDIT"), upper.get());
    EXPECT_EQ(mt::GetProvider("audit"), lower.get());
}

TEST(ProviderRegistryTest, CapacityIsExhaustedLoudlyAndNeverLastWins)
{
    Registered live;
    for (std::size_t i = 0; i < mts::kMaxProfiles; ++i)
    {
        auto provider = MakeProvider("profile-" + std::to_string(i));
        ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered) << i;
        live.push_back(std::move(provider));
    }

    auto overflow = MakeProvider("one-too-many");
    EXPECT_EQ(mts::RegisterProvider(overflow.get()), mts::RegistrationResult::CapacityExhausted);
    EXPECT_EQ(mt::GetProvider("one-too-many"), nullptr);
    // Nothing was displaced to make room.
    EXPECT_EQ(mt::GetProvider("profile-0"), live.front().get());
}

TEST(ProviderRegistryTest, DestroyingAProviderFreesItsSlotAndItsName)
{
    Registered live;
    for (std::size_t i = 0; i < mts::kMaxProfiles; ++i)
    {
        auto provider = MakeProvider("profile-" + std::to_string(i));
        ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered) << i;
        live.push_back(std::move(provider));
    }

    live.front().reset();
    EXPECT_EQ(mt::GetProvider("profile-0"), nullptr);

    // The freed slot takes a new profile, and the freed name is reusable.
    auto replacement = MakeProvider("profile-0");
    EXPECT_EQ(mts::RegisterProvider(replacement.get()), mts::RegistrationResult::Registered);
    EXPECT_EQ(mt::GetProvider("profile-0"), replacement.get());
}

// Deregistering an unregistered provider is a no-op rather than a corruption:
// `~SdkProvider` runs for providers that never reached the registry (a
// duplicate-name `Build`, or a direct construction).
TEST(ProviderRegistryTest, DeregisteringAnUnregisteredProviderLeavesTheSlotsAlone)
{
    auto registered = MakeProvider("kept");
    ASSERT_EQ(mts::RegisterProvider(registered.get()), mts::RegistrationResult::Registered);

    auto never_registered = MakeProvider("kept");
    mts::DeregisterProvider(never_registered.get());

    EXPECT_EQ(mt::GetProvider("kept"), registered.get());
}

// ---------------------------------------------------------------------------
// Lifetime: shut down is not gone
// ---------------------------------------------------------------------------

TEST(ProviderRegistryTest, LookupSurvivesShutdown)
{
    auto provider = MakeProvider("draining");
    ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered);

    ASSERT_EQ(provider->Shutdown(kTimeout), mt::Status::Completed);

    // `Shutdown` does not deregister: the name is freed by destruction, not by
    // shutdown, and the pointer stays valid and answers AlreadyShutDown.
    mt::Provider* const found = mt::GetProvider("draining");
    ASSERT_EQ(found, provider.get());
    EXPECT_EQ(found->Shutdown(kTimeout), mt::Status::AlreadyShutDown);
}

TEST(ProviderRegistryTest, LookupAfterDestructionReturnsNull)
{
    {
        auto provider = MakeProvider("transient");
        ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered);
        ASSERT_NE(mt::GetProvider("transient"), nullptr);
    }
    EXPECT_EQ(mt::GetProvider("transient"), nullptr);
}

// ---------------------------------------------------------------------------
// Concurrency: one name, many claimants
// ---------------------------------------------------------------------------

// The CAS claim plus the lowest-index-wins re-scan. Every thread registers a
// *distinct provider object* under the *same name*, all of them past the
// sequential duplicate scan at once, which is the only way to reach the
// re-scan at all.
TEST(ProviderRegistryTest, ConcurrentRegistrationOfOneNameYieldsExactlyOneWinner)
{
    constexpr std::size_t kThreads = 6;
    static_assert(kThreads <= mts::kMaxProfiles,
                  "every claimant must be able to take a slot, or capacity — not the name — "
                  "would be what refuses the losers");

    Registered providers;
    providers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
    {
        providers.push_back(MakeProvider("race"));
    }

    std::atomic<bool> go{false};
    std::atomic<std::size_t> winners{0};
    std::atomic<std::size_t> duplicates{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(
            [&, i]
            {
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                const mts::RegistrationResult result = mts::RegisterProvider(providers[i].get());
                if (result == mts::RegistrationResult::Registered)
                {
                    winners.fetch_add(1, std::memory_order_relaxed);
                }
                else if (result == mts::RegistrationResult::DuplicateName)
                {
                    duplicates.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(winners.load(), 1U);
    EXPECT_EQ(duplicates.load(), kThreads - 1);

    // The winner is the one the registry hands out, and it is one of ours.
    mt::Provider* const found = mt::GetProvider("race");
    ASSERT_NE(found, nullptr);
    std::size_t matches = 0;
    for (const auto& p : providers)
    {
        matches += static_cast<std::size_t>(found == p.get());
    }
    EXPECT_EQ(matches, 1U);

    // Exactly one slot is occupied: every loser backed its claim out, so the
    // registry has capacity for the remaining profiles.
    for (std::size_t i = 0; i + 1 < mts::kMaxProfiles; ++i)
    {
        auto filler = MakeProvider("filler-" + std::to_string(i));
        EXPECT_EQ(mts::RegisterProvider(filler.get()), mts::RegistrationResult::Registered) << i;
        providers.push_back(std::move(filler));
    }
}

// ---------------------------------------------------------------------------
// The builder surface
// ---------------------------------------------------------------------------

TEST(MultiProfileBuildTest, DefaultProfileSemanticsAreUnchanged)
{
    auto built = mt::SdkBuilder().WithEndpoint(kTestEndpoint).Build();
    ASSERT_TRUE(built.has_value());
    const std::shared_ptr<mt::Provider> provider = std::move(*built);

    EXPECT_EQ(mt::GetProvider(), provider.get());
}

TEST(MultiProfileBuildTest, TwoNamedProfilesAreIndependentlyReachable)
{
    auto audit = mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("audit").Build();
    ASSERT_TRUE(audit.has_value());
    auto ops = mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("ops").Build();
    ASSERT_TRUE(ops.has_value());

    EXPECT_EQ(mt::GetProvider("audit"), audit->get());
    EXPECT_EQ(mt::GetProvider("ops"), ops->get());
    EXPECT_NE(mt::GetProvider("audit"), mt::GetProvider("ops"));
    // Neither took the default profile, so an unnamed lookup finds nothing.
    EXPECT_EQ(mt::GetProvider(), nullptr);
}

TEST(MultiProfileBuildTest, DuplicateProfileNameFailsTheBuild)
{
    auto first = mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("audit").Build();
    ASSERT_TRUE(first.has_value());

    const auto second =
        mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("audit").Build();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, mt::ConfigError::Kind::DuplicateProfileName);
    // Never last-wins: the first provider still owns the name.
    EXPECT_EQ(mt::GetProvider("audit"), first->get());
}

// The same rule for the profile nobody names.
TEST(MultiProfileBuildTest, TwoDefaultProfilesCollide)
{
    auto first = mt::SdkBuilder().WithEndpoint(kTestEndpoint).Build();
    ASSERT_TRUE(first.has_value());

    const auto second = mt::SdkBuilder().WithEndpoint(kTestEndpoint).Build();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, mt::ConfigError::Kind::DuplicateProfileName);
}

// Shutdown does not free the name; destruction does.
TEST(MultiProfileBuildTest, ANameIsReusableOnlyAfterTheProviderIsDestroyed)
{
    {
        auto first = mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("cycle").Build();
        ASSERT_TRUE(first.has_value());
        ASSERT_EQ((*first)->Shutdown(kTimeout), mt::Status::Completed);

        const auto during =
            mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("cycle").Build();
        ASSERT_FALSE(during.has_value());
        EXPECT_EQ(during.error().kind, mt::ConfigError::Kind::DuplicateProfileName);
    }

    auto after = mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("cycle").Build();
    EXPECT_TRUE(after.has_value());
}

TEST(MultiProfileBuildTest, EmptyProfileNameIsRejected)
{
    const auto built = mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("").Build();
    ASSERT_FALSE(built.has_value());
    EXPECT_EQ(built.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(built.error().field, "sdk.profile_name");
}

// The capacity error reaches `Build()`'s caller, and it carries the number a
// reader needs — `kMaxProfiles` is internal, so the message is the only place
// a consumer can learn it.
TEST(MultiProfileBuildTest, BuildBeyondCapacityFailsWithProfileLimitExceeded)
{
    Registered live;
    for (std::size_t i = 0; i < mts::kMaxProfiles; ++i)
    {
        auto provider = MakeProvider("taken-" + std::to_string(i));
        ASSERT_EQ(mts::RegisterProvider(provider.get()), mts::RegistrationResult::Registered) << i;
        live.push_back(std::move(provider));
    }

    const auto built = mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("late").Build();
    ASSERT_FALSE(built.has_value());
    EXPECT_EQ(built.error().kind, mt::ConfigError::Kind::ProfileLimitExceeded);
    EXPECT_NE(built.error().message.find(std::to_string(mts::kMaxProfiles)), std::string::npos)
        << built.error().message;
}

// Two concurrent `Build()`s of one name: one provider, one ConfigError. The
// sequential pre-check cannot decide this — both calls pass it — so this is
// the claim-and-re-scan path end to end, through the public surface.
TEST(MultiProfileBuildTest, ConcurrentBuildsOfOneNameYieldOneProviderAndOneError)
{
    constexpr std::size_t kThreads = 4;
    std::atomic<bool> go{false};
    std::atomic<std::size_t> succeeded{0};
    std::atomic<std::size_t> duplicates{0};
    std::vector<std::shared_ptr<mt::Provider>> built(kThreads);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(
            [&, i]
            {
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                auto result =
                    mt::SdkBuilder().WithEndpoint(kTestEndpoint).WithProfileName("shared").Build();
                if (result.has_value())
                {
                    built[i] = std::move(*result);
                    succeeded.fetch_add(1, std::memory_order_relaxed);
                }
                else if (result.error().kind == mt::ConfigError::Kind::DuplicateProfileName)
                {
                    duplicates.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(succeeded.load(), 1U);
    EXPECT_EQ(duplicates.load(), kThreads - 1);
    EXPECT_NE(mt::GetProvider("shared"), nullptr);
}
