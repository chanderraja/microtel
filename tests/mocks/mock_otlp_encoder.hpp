// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/encoded_payload.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::IOtlpEncoder`.
///
/// Returns a freshly-allocated `EncodedPayload` initialised from a
/// configured byte sequence on every call. No protobuf encoding actually
/// happens; the bytes are whatever the test sets.
class MockOtlpEncoder : public internal::IOtlpEncoder
{
public:
    /// @brief Bytes returned from every Encode call. Test owns the buffer
    /// here; Encode copies into a fresh allocation.
    std::vector<std::byte> bytes_to_return {};

    // --- Recording ---
    int encode_call_count = 0;

    // --- IOtlpEncoder ---

    [[nodiscard]] internal::EncodedPayload
        Encode(const internal::BatchHandle& /*batch*/) override
    {
        ++encode_call_count;
        const std::size_t n = bytes_to_return.size();
        auto buf = std::make_unique<std::byte[]>(n == 0 ? 1 : n);
        if (n > 0)
        {
            std::memcpy(buf.get(), bytes_to_return.data(), n);
        }
        return internal::EncodedPayload{std::move(buf), n};
    }
};

}  // namespace microtel::testing
