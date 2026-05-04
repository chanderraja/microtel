// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace microtel::internal
{

/// @brief Encoded protobuf bytes produced by `IOtlpEncoder::Encode`.
///
/// Move-only owning byte buffer. The encoder allocates the buffer on every
/// call; the upb arena that fed the encoding is destroyed before `Encode`
/// returns. Bytes do not survive a retry — on retry the exporter calls
/// `Encode` again. (LOCKED — `docs/memory-model.md` §3.)
///
/// No upb type appears in this header.
class EncodedPayload
{
public:
    EncodedPayload() noexcept = default;

    EncodedPayload(std::unique_ptr<std::byte[]> bytes, std::size_t size) noexcept;

    EncodedPayload(const EncodedPayload&)            = delete;
    EncodedPayload& operator=(const EncodedPayload&) = delete;
    EncodedPayload(EncodedPayload&&) noexcept        = default;
    EncodedPayload& operator=(EncodedPayload&&) noexcept = default;
    ~EncodedPayload() noexcept                       = default;

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
    [[nodiscard]] std::size_t                Size() const noexcept;

    /// @brief Release the underlying buffer; leaves the payload empty.
    [[nodiscard]] std::unique_ptr<std::byte[]> Release() && noexcept;

private:
    std::unique_ptr<std::byte[]> m_bytes;
    std::size_t                  m_size = 0;
};

}  // namespace microtel::internal
