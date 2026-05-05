// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace microtel::internal
{

/// @brief Encoded protobuf bytes produced by `IOtlpEncoder::Encode`.
///
/// Move-only owning byte buffer. The encoder allocates the buffer on every
/// call; the upb arena that fed the encoding is destroyed before `Encode`
/// returns. Bytes do not survive a retry — on retry the exporter calls
/// `Encode` again. (LOCKED — `docs/memory-model.md` §3.)
///
/// No upb type appears in this header. Methods are defined inline because
/// the type is small enough to be header-only; tests depend on this so
/// mocks can construct `EncodedPayload` without a separate translation
/// unit.
class EncodedPayload
{
public:
    EncodedPayload() noexcept = default;

    EncodedPayload(std::unique_ptr<std::byte[]> bytes, std::size_t size) noexcept
        : m_bytes(std::move(bytes)), m_size(size)
    {
    }

    EncodedPayload(const EncodedPayload&)            = delete;
    EncodedPayload& operator=(const EncodedPayload&) = delete;
    EncodedPayload(EncodedPayload&&) noexcept        = default;
    EncodedPayload& operator=(EncodedPayload&&) noexcept = default;
    ~EncodedPayload() noexcept                       = default;

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept
    {
        return {m_bytes.get(), m_size};
    }

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return m_size;
    }

    /// @brief Release the underlying buffer; leaves the payload empty.
    [[nodiscard]] std::unique_ptr<std::byte[]> Release() && noexcept
    {
        m_size = 0;
        return std::move(m_bytes);
    }

private:
    std::unique_ptr<std::byte[]> m_bytes;
    std::size_t                  m_size = 0;
};

}  // namespace microtel::internal
