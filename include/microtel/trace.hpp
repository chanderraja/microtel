// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace microtel
{

/// @brief 128-bit W3C TraceId.
class TraceId
{
public:
    static constexpr std::size_t kSizeBytes = 16;
    using Bytes = std::array<std::uint8_t, kSizeBytes>;

    TraceId() noexcept = default;

    /// @brief Construct from raw 16-byte buffer.
    explicit TraceId(const Bytes& bytes) noexcept : m_bytes(bytes) {}

    /// @brief Returns true if any byte is non-zero (W3C "valid trace id"
    /// rule: not all zeros).
    [[nodiscard]] bool IsValid() const noexcept
    {
        for (auto b : m_bytes)
        {
            if (b != 0)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const Bytes& AsBytes() const noexcept
    {
        return m_bytes;
    }

    /// @brief Lower-case hex encoding (32 chars, no separators).
    [[nodiscard]] std::string ToHex() const;

private:
    Bytes m_bytes{};
};

/// @brief 64-bit W3C SpanId.
class SpanId
{
public:
    static constexpr std::size_t kSizeBytes = 8;
    using Bytes = std::array<std::uint8_t, kSizeBytes>;

    SpanId() noexcept = default;

    explicit SpanId(const Bytes& bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] bool IsValid() const noexcept
    {
        for (auto b : m_bytes)
        {
            if (b != 0)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const Bytes& AsBytes() const noexcept
    {
        return m_bytes;
    }

    /// @brief Lower-case hex encoding (16 chars, no separators).
    [[nodiscard]] std::string ToHex() const;

private:
    Bytes m_bytes{};
};

/// @brief W3C `tracestate` representation — a small ordered list of
/// vendor-specific entries.
///
/// v1 stores at most 32 entries (W3C maximum). Mutation methods enforce the
/// limit and the "valid key/value charset" rules from RFC 9.
class TraceState
{
public:
    TraceState() noexcept = default;

    /// @brief Parse a tracestate header value.
    ///
    /// Returns an empty `TraceState` on parse failure; failures are silently
    /// elided per the W3C "be liberal in what you accept" guidance.
    [[nodiscard]] static TraceState FromHeader(std::string_view header);

    /// @brief Serialise to a `tracestate` HTTP header value.
    [[nodiscard]] std::string ToHeader() const;

    /// @brief Number of entries.
    [[nodiscard]] std::size_t Size() const noexcept;

    /// @brief True if no entries.
    [[nodiscard]] bool Empty() const noexcept;
};

/// @brief W3C trace flags. Single bit in v1 — sampled.
class TraceFlags
{
public:
    static constexpr std::uint8_t kSampled = 0x01;

    TraceFlags() noexcept = default;
    explicit TraceFlags(std::uint8_t bits) noexcept;

    [[nodiscard]] bool IsSampled() const noexcept;
    [[nodiscard]] std::uint8_t AsByte() const noexcept;

private:
    std::uint8_t m_bits = 0;
};

/// @brief Identifying state of a span — shared across the in-process span and
/// any propagation surface.
struct SpanContext
{
    TraceId trace_id;
    SpanId span_id;
    TraceFlags trace_flags;
    TraceState trace_state;
    bool remote = false;  ///< true if extracted from a propagator

    /// @brief A span context with a non-zero TraceId and SpanId.
    [[nodiscard]] bool IsValid() const noexcept;
};

/// @brief OTel span kind.
enum class SpanKind : std::uint8_t
{
    Internal = 0,
    Server = 1,
    Client = 2,
    Producer = 3,
    Consumer = 4,
};

/// @brief OTel span status code.
enum class StatusCode : std::uint8_t
{
    Unset = 0,
    Ok = 1,
    Error = 2,
};

}  // namespace microtel
