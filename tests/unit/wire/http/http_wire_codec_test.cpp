// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for HttpWireCodec — every row of the HTTP status-code matrix
// from error-model.md §7.1, plus header construction and auth injection.
// Uses FakeTransport (synchronous, no real socket).

#include "wire/http/http_wire_codec.hpp"

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_result.hpp"

#include "fakes/fake_auth_provider.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;
namespace mtw = microtel::wire;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::EncodedPayload MakePayload(std::size_t n = 4)
{
    auto buf = std::make_unique<std::byte[]>(n);
    return mti::EncodedPayload{std::move(buf), n};
}

static mti::TransportResult OkResponse(const std::string& status = "200")
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = status}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
}

static mtw::HttpWireCodecConfig MakeConfig()
{
    return mtw::HttpWireCodecConfig{
        .host = "localhost:4318",
        .scheme = "http",
        .path = "",
        .extra_headers = {},
    };
}

static std::string FindHeader(const std::vector<mti::HeaderField>& headers, const std::string& name)
{
    for (const auto& h : headers)
    {
        if (h.name == name)
        {
            return h.value;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// 2xx success
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_200_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("200");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_206_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("206");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
}

// ---------------------------------------------------------------------------
// Retryable 4xx/5xx
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_429_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("429");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(HttpWireCodecTest, Send_429_WithRetryAfterHeader_PropagatesDelay)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "429"},
                             {.name = "retry-after", .value = "7"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(7000));
}

TEST(HttpWireCodecTest, Send_502_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("502");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(HttpWireCodecTest, Send_503_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("503");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(HttpWireCodecTest, Send_503_WithRetryAfterHeader_PropagatesDelay)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "503"},
                             {.name = "retry-after", .value = "30"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(30000));
}

TEST(HttpWireCodecTest, Send_504_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("504");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

// ---------------------------------------------------------------------------
// Non-retryable failures
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_404_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("404");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_415_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("415");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_Other4xx_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("401");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_500_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("500");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// Transport-layer failure
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_TransportFailure_ReturnsError)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection reset"},
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.has_value());
}

// ---------------------------------------------------------------------------
// Request header construction
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_BuildsCorrectPseudoHeaders)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    const auto& headers = transport.sent_specs[0].headers;

    EXPECT_EQ(FindHeader(headers, ":method"), "POST");
    EXPECT_EQ(FindHeader(headers, ":scheme"), "http");
    EXPECT_EQ(FindHeader(headers, ":path"), "/v1/traces");
    EXPECT_EQ(FindHeader(headers, ":authority"), "localhost:4318");
    EXPECT_EQ(FindHeader(headers, "content-type"), "application/x-protobuf");
}

TEST(HttpWireCodecTest, Send_EmptyPath_ResolvesToV1Traces)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.path = "";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    const auto& headers = transport.sent_specs[0].headers;
    for (const auto& h : headers)
    {
        if (h.name == ":path")
        {
            EXPECT_EQ(h.value, "/v1/traces");
            return;
        }
    }
    FAIL() << ":path header not found";
}

TEST(HttpWireCodecTest, Send_CustomBasePath_AppendsV1Traces)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.path = "/prefix";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    for (const auto& h : transport.sent_specs[0].headers)
    {
        if (h.name == ":path")
        {
            EXPECT_EQ(h.value, "/prefix/v1/traces");
            return;
        }
    }
    FAIL() << ":path header not found";
}

TEST(HttpWireCodecTest, Send_ExtraHeaders_IncludedInRequest)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.extra_headers.push_back({.name = "x-custom", .value = "value"});
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    bool found = false;
    for (const auto& h : transport.sent_specs[0].headers)
    {
        if (h.name == "x-custom" && h.value == "value")
        {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// Auth provider
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_WithAuth_AddsAuthorizationHeader)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtfk::FakeAuthProvider auth;
    auth.static_value = std::optional<std::string>{"Bearer tok"};
    mtfk::FakeSteadyClock clock;
    mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    bool found = false;
    for (const auto& h : transport.sent_specs[0].headers)
    {
        if (h.name == "authorization" && h.value == "Bearer tok")
        {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(HttpWireCodecTest, Send_WithAuth_NoValue_NoAuthorizationHeader)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtfk::FakeAuthProvider auth;
    auth.static_value = std::optional<std::string>{};  // nullopt inside the Expected
    mtfk::FakeSteadyClock clock;
    mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    for (const auto& h : transport.sent_specs[0].headers)
    {
        EXPECT_NE(h.name, "authorization");
    }
}
