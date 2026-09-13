// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for response decompression-bomb protection.
//
// The invariant, from grpc-wire-protocol.md §5.2: decompressed output is
// always bounded by max_decompressed_bytes regardless of the compression ratio
// of the input. A harness that only checked for crashes would miss the failure
// that matters here — a bomb that inflates past the ceiling without the
// process falling over — so the bound is asserted explicitly.
//
// Two entry points over the same bytes:
//   1. GzipDecompress directly, which is where the bound lives.
//   2. GrpcWireCodec's response path, which is where the bytes arrive in
//      production: the 5-byte prefix, the CF flag, and the declared length are
//      all attacker-controlled there.
//
// Input layout:
//   byte 0     : output-ceiling selector (scaled to 0..16320 bytes)
//   bytes 1..n : candidate gzip stream / candidate response body

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "wire/grpc/grpc_wire_codec.hpp"
#include "wire/gzip.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace
{

/// Scales byte 0 into a ceiling small enough that an unbounded implementation
/// blows past it almost immediately, and varied enough to hit the boundary
/// cases (0, exactly-at, one-over).
constexpr std::size_t kCeilingScale = 64U;

// Minimal synchronous transport — resolves immediately with a preset result.
// Mirrors grpc_codec_fuzz.cpp; the codec is the system under test, not the I/O.
class FuzzTransport final : public microtel::internal::ITransport
{
public:
    explicit FuzzTransport(microtel::internal::TransportResult result) noexcept
        : m_result(std::move(result))
    {
    }

    microtel::Expected<void, microtel::Error> Connect(
        const microtel::internal::ConnectOptions& /*opts*/) override
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

    void Cancel(const microtel::internal::RequestHandle& /*handle*/) noexcept override {}

    microtel::ConnectionState GetState() const noexcept override
    {
        return microtel::ConnectionState::Connected;
    }

    microtel::Status Close(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        return microtel::Status::Completed;
    }

private:
    microtel::internal::TransportResult m_result;
};

/// Asserts the documented bound. `abort` rather than a returned error: this is
/// the one outcome the harness exists to catch, and libFuzzer must see it.
void RequireWithinCeiling(std::size_t produced, std::size_t ceiling)
{
    if (produced > ceiling)
    {
        std::abort();
    }
}

void FuzzGzipDecompress(std::span<const std::byte> body, std::size_t ceiling)
{
    const auto result = microtel::wire::GzipDecompress(body, ceiling);
    if (result)
    {
        RequireWithinCeiling(result->size(), ceiling);
    }
}

void FuzzCodecResponsePath(std::vector<std::byte> body, std::size_t ceiling)
{
    microtel::internal::TransportResult result{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
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
            .service_path = {},
            .compression_gzip = false,
            .max_decompressed_bytes = static_cast<std::uint32_t>(ceiling),
        },
    };

    auto buf = std::make_unique<std::byte[]>(1);
    buf[0] = std::byte{0};
    microtel::internal::EncodedPayload payload{std::move(buf), 1};

    (void)codec.Send(std::move(payload), std::chrono::seconds(5));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
    {
        return 0;
    }

    const std::size_t ceiling = static_cast<std::size_t>(data[0]) * kCeilingScale;

    std::vector<std::byte> body;
    body.reserve(size - 1U);
    for (size_t i = 1; i < size; ++i)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        body.push_back(static_cast<std::byte>(data[i]));
    }

    FuzzGzipDecompress(body, ceiling);
    FuzzCodecResponsePath(std::move(body), ceiling);
    return 0;
}
