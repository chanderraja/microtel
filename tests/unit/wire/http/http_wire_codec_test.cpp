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
#include "helpers/gunzip.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

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
        .signal_path = {},
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

// ---------------------------------------------------------------------------
// M5-B: Partial-success parsing
// ---------------------------------------------------------------------------

// ExportTraceServiceResponse { partial_success { rejected_spans: 42 } }
// Outer: field 1 (partial_success), wt=2: tag=0x0A, len=2.
// Inner: field 1 (rejected_spans), wt=0: tag=0x08, varint(42)=0x2A.
static std::vector<std::byte> MakePartialSuccessBody(std::uint8_t rejected_varint)
{
    return {
        std::byte{0x0A},             // outer tag: field 1, wt=2
        std::byte{0x02},             // inner length = 2
        std::byte{0x08},             // inner tag: field 1, wt=0
        std::byte{rejected_varint},  // varint value
    };
}

TEST(HttpWireCodecTest, PartialSuccess_PopulatesRejectedSpans)
{
    mtfk::FakeTransport transport;
    auto resp = OkResponse("200");
    resp.response_body = MakePartialSuccessBody(42U);
    transport.default_response = resp;
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(HttpWireCodecTest, PartialSuccess_EmptyBody_ZeroRejected)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("200");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

TEST(HttpWireCodecTest, PartialSuccess_NonSuccessStatus_BodyIgnored)
{
    // 400 is non-retryable failure — body must not populate partial_success_rejected.
    mtfk::FakeTransport transport;
    const mti::TransportResult resp{
        .success = true,
        .response_headers = {{.name = ":status", .value = "400"}},
        .response_trailers = {},
        .response_body = MakePartialSuccessBody(7U),
        .error = {},
    };
    transport.default_response = resp;
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

// ---------------------------------------------------------------------------
// signal_path override (M12 metrics codec wiring)
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_SignalPath_OverridesDefaultTracesPath)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.signal_path = "/v1/metrics";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, ":path"), "/v1/metrics");
}

TEST(HttpWireCodecTest, Send_SignalPath_WithBasePath_UsesSignalPathDirectly)
{
    // signal_path wins even when a base `path` prefix is also set.
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.path = "/prefix";
    cfg.signal_path = "/v1/metrics";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, ":path"), "/v1/metrics");
}

// ---------------------------------------------------------------------------
// Lazy connect (ICP 0017)
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_WhenDisconnected_ConnectsThenSucceeds)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 1U);
}

TEST(HttpWireCodecTest, Send_WhenAlreadyConnected_DoesNotCallConnect)
{
    mtfk::FakeTransport transport;  // default state: Connected
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 0U);
}

TEST(HttpWireCodecTest, Send_WhenDisconnectedAndConnectFails_ReturnsRetryableWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "refused");
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 0U);  // never got to the actual send
}

TEST(HttpWireCodecTest, SendAll_WhenDisconnected_ConnectsOnceThenSendsAll)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(results[0].success);
    EXPECT_TRUE(results[1].success);
    EXPECT_EQ(transport.connect_calls.size(), 1U);  // one prologue check, not per-payload
    EXPECT_EQ(transport.sent_specs.size(), 2U);
}

TEST(HttpWireCodecTest, SendAll_WhenDisconnectedAndConnectFails_EveryPayloadMarkedRetryable)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    // results[i] must line up with the caller's original payloads[i]
    // (OtlpExporter::FanOutAndProcess indexes both by i) — every batch gets
    // its own retryable result even though none of them were actually sent.
    ASSERT_EQ(results.size(), 3U);
    for (const auto& result : results)
    {
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.retryable);
    }
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 0U);
}

// ---------------------------------------------------------------------------
// Request compression (grpc-wire-protocol.md §5.1; configuration.md
// `exporter.compression`)
// ---------------------------------------------------------------------------

static mti::EncodedPayload MakePayloadFrom(const std::string& s)
{
    auto buf = std::make_unique<std::byte[]>(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        buf[i] = static_cast<std::byte>(s[i]);
    }
    return mti::EncodedPayload{std::move(buf), s.size()};
}

TEST(HttpWireCodecTest, Send_CompressionOff_OmitsContentEncoding)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayloadFrom("hello"), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "content-encoding"), "");
    // Body goes out verbatim.
    ASSERT_EQ(transport.sent_payloads[0].size(), 5U);
}

TEST(HttpWireCodecTest, Send_CompressionOn_SetsContentEncodingAndCompressesBody)
{
    const std::string body(2048, 'x');

    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayloadFrom(body), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "content-encoding"), "gzip");

    const auto& sent = transport.sent_payloads[0];
    EXPECT_LT(sent.size(), body.size());

    const auto restored = mtfk::GunzipToString(sent);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored.value_or(""), body);
}

TEST(HttpWireCodecTest, Send_CompressionOn_ContentLengthMatchesCompressedSize)
{
    // content-length describing the uncompressed size is the classic mistake
    // here: the server reads the wrong number of bytes and the stream hangs.
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayloadFrom(std::string(2048, 'y')), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    const auto declared = FindHeader(transport.sent_specs[0].headers, "content-length");
    EXPECT_EQ(declared, std::to_string(transport.sent_payloads[0].size()));
}

TEST(HttpWireCodecTest, SendAll_CompressionOn_CompressesEveryPayloadInOrder)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::HttpWireCodec codec{&transport, cfg};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayloadFrom("first"));
    payloads.push_back(MakePayloadFrom("second"));

    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    ASSERT_EQ(results.size(), 2U);
    ASSERT_EQ(transport.sent_payloads.size(), 2U);
    EXPECT_EQ(mtfk::GunzipToString(transport.sent_payloads[0]).value_or(""), "first");
    EXPECT_EQ(mtfk::GunzipToString(transport.sent_payloads[1]).value_or(""), "second");
}
