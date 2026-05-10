// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for Http2Transport state machine and I/O thread lifecycle.
// Uses FakeReactor so no real socket, TLS, or nghttp2 session is needed.
// Full Connect / Send behaviour is integration-tested in M3-D4+.

#include "transport/http2_transport.hpp"

#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_reactor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace mt = microtel;
namespace mtt = microtel::transport;
namespace mtfk = microtel::testing;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::unique_ptr<mtt::Http2Transport> MakeTransport()
{
    auto reactor = std::make_unique<mtfk::FakeReactor>();
    auto result = mtt::Http2Transport::Create(std::move(reactor));
    EXPECT_TRUE(result.has_value());
    return std::move(*result);
}

namespace mti = microtel::internal;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(Http2TransportTest, Create_Succeeds)
{
    auto reactor = std::make_unique<mtfk::FakeReactor>();
    const auto result = mtt::Http2Transport::Create(std::move(reactor));
    EXPECT_TRUE(result.has_value());
}

TEST(Http2TransportTest, InitialState_IsDisconnected)
{
    const auto t = MakeTransport();
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Disconnected);
}

TEST(Http2TransportTest, Close_WhenDisconnected_ReturnsCompleted)
{
    auto t = MakeTransport();
    const mt::Status s = t->Close(std::chrono::milliseconds(500));
    EXPECT_EQ(s, mt::Status::Completed);
}

TEST(Http2TransportTest, Close_SetsStateToClosed)
{
    auto t = MakeTransport();
    (void)t->Close(std::chrono::milliseconds(500));
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Closed);
}

TEST(Http2TransportTest, Close_Twice_ReturnsAlreadyShutDown)
{
    auto t = MakeTransport();
    (void)t->Close(std::chrono::milliseconds(500));
    const mt::Status s = t->Close(std::chrono::milliseconds(500));
    EXPECT_EQ(s, mt::Status::AlreadyShutDown);
}

TEST(Http2TransportTest, Destructor_JoinsIoThread)
{
    // Destructor must not block indefinitely or crash; the I/O thread joins.
    auto t = MakeTransport();
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Disconnected);
    // t goes out of scope here; destructor calls Close internally.
}

TEST(Http2TransportTest, Connect_EmptyEndpoint_ReturnsError)
{
    auto t = MakeTransport();
    const auto result = t->Connect(mti::ConnectOptions{});
    EXPECT_FALSE(result.has_value());
}

TEST(Http2TransportTest, Connect_WhenClosed_ReturnsError)
{
    auto t = MakeTransport();
    (void)t->Close(std::chrono::milliseconds(500));
    mti::ConnectOptions opts;
    opts.endpoint = "https://localhost:4317";
    const auto result = t->Connect(opts);
    EXPECT_FALSE(result.has_value());
}

TEST(Http2TransportTest, Send_WhenDisconnected_FutureHasError)
{
    auto t = MakeTransport();
    mti::RequestSpec spec;
    spec.headers.push_back({":method", "POST"});

    auto handle = t->Send(std::move(spec));
    ASSERT_TRUE(handle.Future().valid());

    const auto result = handle.Future().get();
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.has_value());
}

TEST(Http2TransportTest, Send_WhenClosed_FutureHasError)
{
    auto t = MakeTransport();
    (void)t->Close(std::chrono::milliseconds(500));

    mti::RequestSpec spec;
    spec.headers.push_back({":method", "POST"});

    auto handle = t->Send(std::move(spec));
    ASSERT_TRUE(handle.Future().valid());

    const auto result = handle.Future().get();
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.has_value());
}

TEST(Http2TransportTest, Send_WhenDisconnected_HandleIdIsZero)
{
    auto t = MakeTransport();
    mti::RequestSpec spec;

    const auto handle = t->Send(std::move(spec));
    EXPECT_EQ(handle.Id(), 0U);
}

TEST(Http2TransportTest, Cancel_DefaultHandle_DoesNotCrash)
{
    auto t = MakeTransport();
    const mti::RequestHandle handle;  // default-constructed, id = 0
    EXPECT_NO_THROW(t->Cancel(handle));
}
