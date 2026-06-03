// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for the gRPC response parser in GrpcWireCodec.
//
// Exercises: grpc-status header parsing, grpc-status-details-bin base64
// decoding, RetryInfo proto parsing (ReadVarint / ReadLenDelim / SkipField /
// ParseDurationMs / ParseRetryInfoMs), and ClassifyResponse dispatch across
// all gRPC status codes.
//
// Input layout:
//   byte 0              : grpc-status code (mod 17 → 0..16)
//   bytes 1..size/2     : raw grpc-status-details-bin header value
//                         (exercises base64 decoder + proto parser on
//                         arbitrary bytes, including invalid base64)
//   bytes size/2..end   : response body (exercises partial-success path)

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "wire/grpc/grpc_wire_codec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace
{

// Minimal synchronous transport — resolves immediately with a preset result.
class FuzzTransport final : public microtel::internal::ITransport
{
public:
    explicit FuzzTransport(microtel::internal::TransportResult result) noexcept
        : m_result(std::move(result))
    {
    }

    microtel::Expected<void, microtel::Error> Connect(
        const microtel::internal::ConnectOptions&) override
    {
        return {};
    }

    microtel::internal::RequestHandle Send(
        microtel::internal::RequestSpec /*spec*/) noexcept override
    {
        std::promise<microtel::internal::TransportResult> p;
        p.set_value(m_result);
        return microtel::internal::RequestHandle{0, p.get_future()};
    }

    void Cancel(const microtel::internal::RequestHandle&) noexcept override {}

    microtel::ConnectionState GetState() const noexcept override
    {
        return microtel::ConnectionState::Connected;
    }

    microtel::Status Close(std::chrono::milliseconds) noexcept override
    {
        return microtel::Status::Completed;
    }

private:
    microtel::internal::TransportResult m_result;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
    {
        return 0;
    }

    const size_t split = size / 2;
    const int status_code = data[0] % 17;
    const std::string details_bin(reinterpret_cast<const char*>(data + 1),
                                  split > 0 ? split - 1 : 0);
    std::vector<std::byte> body;
    body.reserve(size - split);
    for (size_t i = split; i < size; ++i)
    {
        body.push_back(static_cast<std::byte>(data[i]));
    }

    microtel::internal::TransportResult result{
        .success = true,
        .response_headers =
            {
                {.name = "content-type", .value = "application/grpc"},
            },
        .response_trailers =
            {
                {.name = "grpc-status", .value = std::to_string(status_code)},
                {.name = "grpc-status-details-bin", .value = details_bin},
            },
        .response_body = std::move(body),
        .error = {},
    };

    FuzzTransport transport{std::move(result)};
    microtel::wire::GrpcWireCodec codec{
        &transport,
        microtel::wire::GrpcWireCodecConfig{
            .host = "fuzz",
            .scheme = "http",
            .extra_headers = {},
        },
    };

    auto buf = std::make_unique<std::byte[]>(1);
    buf[0] = std::byte{0};
    microtel::internal::EncodedPayload payload{std::move(buf), 1};

    (void)codec.Send(std::move(payload), std::chrono::seconds(5));
    return 0;
}
