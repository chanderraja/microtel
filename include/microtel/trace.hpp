// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace microtel
{

namespace internal
{

/// @brief The immutable entry list behind `TraceState`.
///
/// Forward-declared on purpose: the representation is defined only in
/// `src/api/trace_state.cpp`, so it is not part of microtel's ABI and can
/// change without a second ABI event ([ICP 0025](../../docs/icps/0025-propagation-core.md) §1).
struct TraceStateImpl;

}  // namespace internal

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
/// Stores at most `kMaxEntries` entries (the W3C maximum) and enforces the
/// W3C Trace Context §3.3 key and value grammars on both parsing and
/// mutation. Order is semantic: the left-most entry belongs to the most
/// recently mutating system, so nothing here sorts or reorders except where
/// the specification says to.
///
/// The entry list is **immutable and shared** — a copy is a refcount bump, so
/// every special member is `noexcept` and `Span::GetContext() const noexcept`
/// can keep returning a `SpanContext` by value (hard rule 14). `Set` and
/// `Erase` are therefore copy-on-write: they build a new list and return a new
/// `TraceState`, leaving the receiver and every other copy untouched. The
/// empty state holds a null pointer and allocates nothing.
///
/// @threadsafety Thread-safe for concurrent reads; an instance is an
///               immutable value.
class TraceState
{
public:
    /// @brief W3C maximum number of list members in a `tracestate` header.
    static constexpr std::size_t kMaxEntries = 32;

    TraceState() noexcept = default;

    /// @brief Parse a tracestate header value.
    ///
    /// Returns an empty `TraceState` on parse failure; failures are silently
    /// elided per the W3C "be liberal in what you accept" guidance. microtel
    /// takes W3C Trace Context §4.3's whole-header option — a single
    /// malformed member, a duplicate key, or a 33rd member discards the
    /// entire header rather than leaving a caller to guess which half
    /// survived.
    [[nodiscard]] static TraceState FromHeader(std::string_view header);

    /// @brief Serialise to a `tracestate` HTTP header value.
    ///
    /// Entries are emitted in order, comma-separated, with no optional
    /// whitespace. An empty state serialises to the empty string, which is
    /// what lets a propagator omit the header entirely.
    [[nodiscard]] std::string ToHeader() const;

    /// @brief Number of entries.
    [[nodiscard]] std::size_t Size() const noexcept;

    /// @brief True if no entries.
    [[nodiscard]] bool Empty() const noexcept;

    /// @brief The value stored under @p key, if any.
    ///
    /// The returned view is **borrowed** from this `TraceState`'s shared entry
    /// list and stays valid while any copy of this state lives.
    [[nodiscard]] std::optional<std::string_view> Get(std::string_view key) const noexcept;

    /// @brief Copy-on-write insert-or-update; this state is left unchanged.
    ///
    /// The entry is placed at the **front** of the returned list per W3C
    /// Trace Context §3.3.1 — a new pair "SHOULD be added to the beginning",
    /// and a modified key "should be moved to the beginning (left) of the
    /// list". The relative order of the untouched entries is preserved.
    ///
    /// The mutation is refused — the return is an unchanged copy — if @p key
    /// or @p value violates the W3C grammar, or if @p key is new and the state
    /// already holds `kMaxEntries` entries. There is no error channel on this
    /// surface, and evicting another system's entry to make room would lose
    /// state microtel was asked to carry.
    [[nodiscard]] TraceState Set(std::string_view key, std::string_view value) const;

    /// @brief Copy-on-write removal; this state is left unchanged.
    ///
    /// Returns an unchanged copy if @p key is absent. The order of the
    /// surviving entries is preserved.
    [[nodiscard]] TraceState Erase(std::string_view key) const;

private:
    /// @brief The shared entry list; null is the empty state.
    std::shared_ptr<const internal::TraceStateImpl> m_entries;
};

/// @brief W3C trace flags. Single bit in v1 — sampled.
class TraceFlags
{
public:
    static constexpr std::uint8_t kSampled = 0x01;

    TraceFlags() noexcept = default;
    explicit TraceFlags(std::uint8_t bits) noexcept : m_bits(bits) {}

    [[nodiscard]] bool IsSampled() const noexcept
    {
        return (m_bits & kSampled) != 0;
    }
    [[nodiscard]] std::uint8_t AsByte() const noexcept
    {
        return m_bits;
    }

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
    [[nodiscard]] bool IsValid() const noexcept
    {
        return trace_id.IsValid() && span_id.IsValid();
    }
};

// `Span::GetContext() const noexcept` returns a `SpanContext` **by value**
// (hard rule 14), so nothing inside one may have an allocating copy. This is
// the whole reason `TraceState` holds its entries behind a `shared_ptr`
// instead of in a `std::vector` member — see ICP 0025 §1, which asks the
// implementing packet to plant exactly this guard.
static_assert(std::is_nothrow_copy_constructible_v<TraceState>,
              "TraceState's copy must not allocate — see ICP 0025 §1");
static_assert(std::is_nothrow_copy_constructible_v<SpanContext>,
              "SpanContext is returned by value from Span::GetContext() const noexcept");
static_assert(std::is_nothrow_move_constructible_v<SpanContext>);
static_assert(std::is_nothrow_copy_assignable_v<SpanContext>);

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
