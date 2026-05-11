// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for M6-B auth provider implementations:
//   - StaticHeadersAuthProvider
//   - CallbackAuthProvider (TTL cache)

#include "common/config/auth_providers.hpp"

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/sdk_builder.hpp"

#include "fakes/fake_steady_clock.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

namespace mc = microtel::config;
namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;

using TimePoint = mti::TimePointSteady;

// ---------------------------------------------------------------------------
// StaticHeadersAuthProvider
// ---------------------------------------------------------------------------

TEST(StaticAuthProviderTest, NonEmptyToken_ReturnsToken)
{
    mc::StaticHeadersAuthProvider provider{"Bearer tok123"};
    const auto result = provider.GetAuthorization(TimePoint{});
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(**result, "Bearer tok123");
}

TEST(StaticAuthProviderTest, EmptyToken_ReturnsNullopt)
{
    mc::StaticHeadersAuthProvider provider{""};
    const auto result = provider.GetAuthorization(TimePoint{});
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(StaticAuthProviderTest, ConsistentAcrossMultipleCalls)
{
    mc::StaticHeadersAuthProvider provider{"Basic dXNlcjpwYXNz"};
    for (int i = 0; i < 5; ++i)
    {
        const auto result = provider.GetAuthorization(TimePoint{});
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(**result, "Basic dXNlcjpwYXNz");
    }
}

// ---------------------------------------------------------------------------
// CallbackAuthProvider — callback invocation
// ---------------------------------------------------------------------------

TEST(CallbackAuthProviderTest, FirstCall_InvokesCallback)
{
    int call_count = 0;
    mc::CallbackAuthProvider provider{[&]() -> mt::Expected<std::string, mt::Error>
                                      {
                                          ++call_count;
                                          return std::string{"Bearer fresh"};
                                      },
                                      std::chrono::seconds(60)};

    mtfk::FakeSteadyClock clock;
    const auto result = provider.GetAuthorization(clock.Now());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(**result, "Bearer fresh");
    EXPECT_EQ(call_count, 1);
}

TEST(CallbackAuthProviderTest, SecondCallWithinTtl_ReturnsCached_NoExtraInvocation)
{
    int call_count = 0;
    mc::CallbackAuthProvider provider{[&]() -> mt::Expected<std::string, mt::Error>
                                      {
                                          ++call_count;
                                          return std::string{"Bearer tok"};
                                      },
                                      std::chrono::seconds(60)};

    mtfk::FakeSteadyClock clock;
    (void)provider.GetAuthorization(clock.Now());
    ASSERT_EQ(call_count, 1);

    clock.Advance(std::chrono::seconds(30));  // still within TTL
    (void)provider.GetAuthorization(clock.Now());
    EXPECT_EQ(call_count, 1);  // no second invocation
}

TEST(CallbackAuthProviderTest, SecondCallWithinTtl_ReturnsSameCachedValue)
{
    mc::CallbackAuthProvider provider{[]() -> mt::Expected<std::string, mt::Error>
                                      { return std::string{"Bearer cached"}; },
                                      std::chrono::seconds(60)};

    mtfk::FakeSteadyClock clock;
    (void)provider.GetAuthorization(clock.Now());
    clock.Advance(std::chrono::seconds(1));
    const auto result = provider.GetAuthorization(clock.Now());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(**result, "Bearer cached");
}

TEST(CallbackAuthProviderTest, CallAfterTtlExpiry_InvokesCallbackAgain)
{
    int call_count = 0;
    mc::CallbackAuthProvider provider{[&]() -> mt::Expected<std::string, mt::Error>
                                      {
                                          ++call_count;
                                          return std::string{"Bearer tok"};
                                      },
                                      std::chrono::seconds(60)};

    mtfk::FakeSteadyClock clock;
    (void)provider.GetAuthorization(clock.Now());
    ASSERT_EQ(call_count, 1);

    clock.Advance(std::chrono::seconds(61));  // TTL exceeded
    (void)provider.GetAuthorization(clock.Now());
    EXPECT_EQ(call_count, 2);
}

TEST(CallbackAuthProviderTest, ZeroTtl_InvokesCallbackEveryTime)
{
    int call_count = 0;
    mc::CallbackAuthProvider provider{[&]() -> mt::Expected<std::string, mt::Error>
                                      {
                                          ++call_count;
                                          return std::string{"Bearer tok"};
                                      },
                                      std::chrono::milliseconds(0)};

    mtfk::FakeSteadyClock clock;
    (void)provider.GetAuthorization(clock.Now());
    (void)provider.GetAuthorization(clock.Now());
    (void)provider.GetAuthorization(clock.Now());
    EXPECT_EQ(call_count, 3);
}

// ---------------------------------------------------------------------------
// CallbackAuthProvider — error handling
// ---------------------------------------------------------------------------

TEST(CallbackAuthProviderTest, CallbackError_PropagatesError)
{
    mc::CallbackAuthProvider provider{
        []() -> mt::Expected<std::string, mt::Error>
        {
            return mt::Unexpected{
                mt::Error{.kind = mt::Error::Kind::Network, .message = "token fetch failed"}};
        },
        std::chrono::seconds(60)};

    mtfk::FakeSteadyClock clock;
    const auto result = provider.GetAuthorization(clock.Now());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::Error::Kind::Network);
}

TEST(CallbackAuthProviderTest, CallbackError_DoesNotCache_NextCallRetries)
{
    int call_count = 0;
    bool fail = true;
    mc::CallbackAuthProvider provider{
        [&]() -> mt::Expected<std::string, mt::Error>
        {
            ++call_count;
            if (fail)
            {
                return mt::Unexpected{
                    mt::Error{.kind = mt::Error::Kind::Network, .message = "transient failure"}};
            }
            return std::string{"Bearer recovered"};
        },
        std::chrono::seconds(60)};

    mtfk::FakeSteadyClock clock;
    (void)provider.GetAuthorization(clock.Now());  // fails, call_count = 1
    ASSERT_EQ(call_count, 1);

    fail = false;
    const auto result = provider.GetAuthorization(clock.Now());  // retries, call_count = 2
    ASSERT_EQ(call_count, 2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(**result, "Bearer recovered");
}

TEST(CallbackAuthProviderTest, AfterError_ThenSuccess_CachesSuccessValue)
{
    int call_count = 0;
    bool fail = true;
    mc::CallbackAuthProvider provider{[&]() -> mt::Expected<std::string, mt::Error>
                                      {
                                          ++call_count;
                                          if (fail)
                                          {
                                              return mt::Unexpected{mt::Error{}};
                                          }
                                          return std::string{"Bearer ok"};
                                      },
                                      std::chrono::seconds(60)};

    mtfk::FakeSteadyClock clock;
    (void)provider.GetAuthorization(clock.Now());  // error path
    fail = false;
    (void)provider.GetAuthorization(clock.Now());  // success — now cached

    clock.Advance(std::chrono::seconds(1));        // still within TTL
    (void)provider.GetAuthorization(clock.Now());  // should hit cache
    EXPECT_EQ(call_count, 2);                      // error + first success only; third is cached
}
