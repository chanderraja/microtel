// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for GrpcWireCodec — every row of the gRPC status-code matrix
// from error-model.md §7.2, plus header construction, gRPC framing,
// trailer-only detection, and RetryInfo decoding.
// Uses FakeTransport (synchronous, no real socket).

#include "wire/grpc/grpc_wire_codec.hpp"

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_result.hpp"

#include "fakes/fake_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
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

static mtw::GrpcWireCodecConfig MakeConfig()
{
    return mtw::GrpcWireCodecConfig{
        .host = "localhost:4317",
        .scheme = "https",
        .extra_headers = {},
        .service_path = {},
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

// Minimal base64 URL-safe encoder (no padding) — test-only fixture builder.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
static std::string Base64UrlEncode(const std::vector<std::uint8_t>& data)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    std::size_t i = 0;
    while (i + 2 < data.size())
    {
        const auto d0 = static_cast<unsigned>(data[i]);
        const auto d1 = static_cast<unsigned>(data[i + 1U]);
        const auto d2 = static_cast<unsigned>(data[i + 2U]);
        result += kAlphabet[(d0 >> 2U) & 0x3FU];
        result += kAlphabet[((d0 & 0x3U) << 4U) | (d1 >> 4U)];
        result += kAlphabet[((d1 & 0xFU) << 2U) | (d2 >> 6U)];
        result += kAlphabet[d2 & 0x3FU];
        i += 3;
    }
    if (i < data.size())
    {
        const auto d0 = static_cast<unsigned>(data[i]);
        result += kAlphabet[(d0 >> 2U) & 0x3FU];
        if (i + 1 < data.size())
        {
            const auto d1 = static_cast<unsigned>(data[i + 1U]);
            result += kAlphabet[((d0 & 0x3U) << 4U) | (d1 >> 4U)];
            result += kAlphabet[(d1 & 0xFU) << 2U];
        }
        else
        {
            result += kAlphabet[(d0 & 0x3U) << 4U];
        }
    }
    return result;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

static void EncodeVarint(std::uint64_t v, std::vector<std::uint8_t>& out)
{
    while (v >= 0x80U)
    {
        out.push_back(static_cast<std::uint8_t>((v & 0x7FU) | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

// Builds a grpc-status-details-bin value encoding:
//   google.rpc.Status { details: [ Any { type_url: <RetryInfo>, value: RetryInfo{delay=seconds} } ]
//   }
static std::string MakeRetryInfoDetailsBin(std::int64_t seconds)
{
    // Duration{seconds}
    std::vector<std::uint8_t> duration;
    duration.push_back(0x08U);  // field 1 (seconds), wire type 0
    EncodeVarint(static_cast<std::uint64_t>(seconds), duration);

    // RetryInfo{retry_delay: duration}
    std::vector<std::uint8_t> retry_info;
    retry_info.push_back(0x0AU);  // field 1 (retry_delay), wire type 2
    retry_info.push_back(static_cast<std::uint8_t>(duration.size()));
    retry_info.insert(retry_info.end(), duration.begin(), duration.end());

    // Any{type_url, value: retry_info}
    static constexpr std::string_view kTypeUrl = "type.googleapis.com/google.rpc.RetryInfo";
    std::vector<std::uint8_t> any_msg;
    any_msg.push_back(0x0AU);  // field 1 (type_url), wire type 2
    any_msg.push_back(static_cast<std::uint8_t>(kTypeUrl.size()));
    for (char c : kTypeUrl)
    {
        any_msg.push_back(static_cast<std::uint8_t>(c));
    }
    any_msg.push_back(0x12U);  // field 2 (value), wire type 2
    any_msg.push_back(static_cast<std::uint8_t>(retry_info.size()));
    any_msg.insert(any_msg.end(), retry_info.begin(), retry_info.end());

    // Status{details: [any]}
    std::vector<std::uint8_t> status;
    status.push_back(0x1AU);  // field 3 (details), wire type 2
    status.push_back(static_cast<std::uint8_t>(any_msg.size()));
    status.insert(status.end(), any_msg.begin(), any_msg.end());

    return Base64UrlEncode(status);
}

static mti::TransportResult GrpcSuccessResponse()
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
}

static mti::TransportResult GrpcStatusResponse(const std::string& code)
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = code}},
        .response_body = {},
        .error = {},
    };
}

static mti::TransportResult TrailerOnlyResponse(const std::string& code)
{
    // grpc-status in the initial HEADERS frame (END_STREAM on first HEADERS)
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"},
                             {.name = "grpc-status", .value = code}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
}

// ---------------------------------------------------------------------------
// gRPC success — grpc-status: 0
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_GrpcStatus0_InTrailers_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_TrailerOnly_GrpcStatus0_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = TrailerOnlyResponse("0");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// Retryable gRPC status codes
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_Cancelled_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("1");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_DeadlineExceeded_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("4");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_Unavailable_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("14");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_DataLoss_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("15");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_TrailerOnly_Unavailable_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = TrailerOnlyResponse("14");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

// ---------------------------------------------------------------------------
// Non-retryable gRPC status codes
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_InvalidArgument_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("3");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_Internal_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("13");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_Unimplemented_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("12");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// RESOURCE_EXHAUSTED (8) — the RetryInfo gate
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_ResourceExhausted_WithRetryInfo_RetryableWithDelay)
{
    constexpr std::int64_t kDelaySec = 5;
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "8"},
                              {.name = "grpc-status-details-bin",
                               .value = MakeRetryInfoDetailsBin(kDelaySec)}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(kDelaySec * 1000));
}

TEST(GrpcWireCodecTest, Send_ResourceExhausted_WithoutRetryInfo_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("8");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_FALSE(result.retry_after.has_value());
}

TEST(GrpcWireCodecTest, Send_ResourceExhausted_RetryInfo_InInitialHeaders_Retryable)
{
    // Trailer-only: grpc-status-details-bin in the initial HEADERS frame
    constexpr std::int64_t kDelaySec = 3;
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"},
                             {.name = "grpc-status", .value = "8"},
                             {.name = "grpc-status-details-bin",
                              .value = MakeRetryInfoDetailsBin(kDelaySec)}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(kDelaySec * 1000));
}

// ---------------------------------------------------------------------------
// Missing grpc-status — HTTP :status fallback (§4.2)
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_MissingGrpcStatus_Http429_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "429"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_MissingGrpcStatus_Http503_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "503"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_MissingGrpcStatus_Http200_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// Transport-level error
// ---------------------------------------------------------------------------

// A transport-level failure is exactly what the retry engine exists for:
// collector restarts, rolling deploys, load balancers draining. This used to
// return retryable=false, so a gRPC deployment dropped the batch on the first
// connection reset while an HTTP deployment retried the identical failure
// (`http_wire_codec.cpp`, "transport-level failure: connection reset, etc.").
TEST(GrpcWireCodecTest, Send_TransportError_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection reset"},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "connection reset");
}

// The codec used to disagree with itself: a connection that failed while being
// established was retryable (ICP 0017's EnsureConnected), but one that failed
// after being established was not. Same Error::Kind, same recovery, opposite
// verdicts.
TEST(GrpcWireCodecTest, Send_ConnectFailureAndMidStreamFailure_AgreeOnRetryability)
{
    mtfk::FakeTransport connect_fails;
    connect_fails.state = mt::ConnectionState::Disconnected;
    connect_fails.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::GrpcWireCodec connect_codec{&connect_fails, MakeConfig()};
    const auto connect_result = connect_codec.Send(MakePayload(), std::chrono::milliseconds(500));

    mtfk::FakeTransport send_fails;
    send_fails.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection reset"},
    };
    mtw::GrpcWireCodec send_codec{&send_fails, MakeConfig()};
    const auto send_result = send_codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_EQ(connect_result.retryable, send_result.retryable);
    EXPECT_TRUE(send_result.retryable);
}

// ---------------------------------------------------------------------------
// Header construction
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_BuildsRequiredGrpcHeaders)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    const auto& headers = transport.sent_specs.front().headers;
    EXPECT_EQ(FindHeader(headers, ":method"), "POST");
    EXPECT_EQ(FindHeader(headers, ":scheme"), "https");
    EXPECT_EQ(FindHeader(headers, ":authority"), "localhost:4317");
    EXPECT_EQ(FindHeader(headers, ":path"),
              "/opentelemetry.proto.collector.trace.v1.TraceService/Export");
    EXPECT_EQ(FindHeader(headers, "te"), "trailers");
    EXPECT_EQ(FindHeader(headers, "content-type"), "application/grpc+proto");
    EXPECT_FALSE(FindHeader(headers, "user-agent").empty());
}

// ---------------------------------------------------------------------------
// gRPC framing — 5-byte length prefix
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_GrpcFrame_HasCorrectPrefix)
{
    constexpr std::size_t kPayloadSize = 7;
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(kPayloadSize), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_payloads.size(), 1U);
    const auto& payload = transport.sent_payloads.front();
    ASSERT_GE(payload.size(), 5U);

    // Byte 0: compression flag (0 = uncompressed)
    EXPECT_EQ(std::to_integer<int>(payload[0]), 0);
    // Bytes 1-4: big-endian uint32 message length
    const std::uint32_t length = (std::to_integer<std::uint32_t>(payload[1]) << 24U) |
                                 (std::to_integer<std::uint32_t>(payload[2]) << 16U) |
                                 (std::to_integer<std::uint32_t>(payload[3]) << 8U) |
                                 std::to_integer<std::uint32_t>(payload[4]);
    EXPECT_EQ(length, kPayloadSize);
    EXPECT_EQ(payload.size(), 5U + kPayloadSize);
}

// ---------------------------------------------------------------------------
// M5-B: Partial-success parsing
// ---------------------------------------------------------------------------

// Build a gRPC DATA frame (5-byte prefix + proto) from raw proto bytes.
// Compression flag = 0x00, length as big-endian uint32.
static std::vector<std::byte> GrpcFrame(std::span<const std::uint8_t> proto_bytes)
{
    const auto n = static_cast<std::uint32_t>(proto_bytes.size());
    std::vector<std::byte> frame;
    frame.reserve(5U + proto_bytes.size());
    frame.push_back(std::byte{0x00U});
    frame.push_back(std::byte{static_cast<std::uint8_t>((n >> 24U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>((n >> 16U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>((n >> 8U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>(n & 0xFFU)});
    for (const auto b : proto_bytes)
    {
        frame.push_back(static_cast<std::byte>(b));
    }
    return frame;
}

// ExportTraceServiceResponse { partial_success { rejected_spans: 42 } }
// [0x0A, 0x02, 0x08, 0x2A]
static constexpr std::array<std::uint8_t, 4> kRejected42Proto{0x0A, 0x02, 0x08, 0x2A};

TEST(GrpcWireCodecTest, PartialSuccess_PopulatesRejectedSpans)
{
    mtfk::FakeTransport transport;
    auto resp = GrpcSuccessResponse();
    resp.response_body = GrpcFrame(kRejected42Proto);
    transport.default_response = resp;
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(GrpcWireCodecTest, PartialSuccess_EmptyBody_ZeroRejected)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

TEST(GrpcWireCodecTest, PartialSuccess_NonZeroGrpcStatus_BodyIgnored)
{
    // grpc-status=8 (RESOURCE_EXHAUSTED without RetryInfo) → failure, no partial success.
    mtfk::FakeTransport transport;
    const mti::TransportResult resp{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "8"}},
        .response_body = GrpcFrame(kRejected42Proto),
        .error = {},
    };
    transport.default_response = resp;
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

// ---------------------------------------------------------------------------
// service_path override (M12 metrics codec wiring)
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_ServicePath_OverridesDefaultTracesPath)
{
    mtfk::FakeTransport transport;
    const mti::TransportResult resp{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
    transport.default_response = resp;

    mtw::GrpcWireCodecConfig cfg = MakeConfig();
    cfg.service_path = "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export";
    mtw::GrpcWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, ":path"),
              "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export");
}

// ---------------------------------------------------------------------------
// Lazy connect (ICP 0017)
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_WhenDisconnected_ConnectsThenSucceeds)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 1U);
}

TEST(GrpcWireCodecTest, Send_WhenAlreadyConnected_DoesNotCallConnect)
{
    mtfk::FakeTransport transport;  // default state: Connected
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 0U);
}

TEST(GrpcWireCodecTest, Send_WhenDisconnectedAndConnectFails_ReturnsRetryableWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "refused");
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 0U);  // never got to the actual send
}

// ── Abandoned promise: the mid-connection drop hang (ICP 0018) ───────────────
//
// The transport's drop path left every in-flight promise unfulfilled, and this
// codec waited on it with an unbounded Future().get(). A dropped connection
// therefore wedged a gRPC exporter thread permanently — the process had to be
// restarted. HttpWireCodec was never affected because it always waited with a
// deadline.
//
// The drop path now fulfils (see Http2Transport::AbandonInFlight); this test
// covers the codec half, so that a future bug which abandons a promise costs
// one deadline instead of the thread.

TEST(GrpcWireCodecTest, Send_PromiseNeverFulfilled_TimesOutInsteadOfHanging)
{
    mtfk::FakeTransport transport;
    transport.abandon_promises = true;
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto start = std::chrono::steady_clock::now();
    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(50));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable) << "a timed-out request must be retried, not dropped";
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded above
    EXPECT_EQ(result.error->message, "request deadline exceeded");
    // Returned near the deadline rather than blocking indefinitely.
    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_EQ(transport.cancel_call_count, 1) << "the abandoned request must be cancelled";
}
