// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace microtel
{

namespace internal
{

/// @brief The immutable entry list behind `Baggage`.
///
/// Forward-declared on purpose: the representation is defined only in
/// `src/api/baggage.cpp`, so it is not part of microtel's ABI and can change
/// without an ABI event ([ICP 0025](../../docs/icps/0025-propagation-core.md) §2).
struct BaggageImpl;

}  // namespace internal

/// @brief W3C Baggage — the application-level key/value pairs a request
/// carries alongside its trace.
///
/// Parses and serialises the `baggage` HTTP header defined by
/// [W3C Baggage](https://www.w3.org/TR/baggage/):
///
/// ```
/// baggage-string = list-member 0*179( OWS "," OWS list-member )
/// list-member    = key OWS "=" OWS value *( OWS ";" OWS property )
/// property       = key OWS "=" OWS value / key OWS
/// key            = token                       ; RFC 7230 §3.2.6
/// value          = *baggage-octet
/// baggage-octet  = %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
/// ```
///
/// ### Where baggage lives, and why it is not on `SpanContext`
///
/// A `Baggage` rides `microtel::Context`, **never** `SpanContext`. It is
/// per-context, not per-span — a request carries baggage whether or not a span
/// is active, and baggage set inside a span must outlive that span within the
/// enclosing scope. Parking it on `SpanContext` would also put a second
/// growable member inside `Span::GetContext() const noexcept`, which returns
/// by value (hard rule 14). ICP 0025 §2 records that decision so it is not
/// re-litigated.
///
/// ### Storage
///
/// The entry list is **immutable and shared**: a copy is a refcount bump, so
/// every special member is `noexcept` and a `Context` copy neither allocates
/// nor throws. `Set` and `Erase` are therefore copy-on-write — they build a new
/// list and return a new `Baggage`, leaving the receiver and every other copy
/// untouched. The empty baggage holds a null pointer and allocates nothing.
///
/// ### Values are held decoded
///
/// `Get` returns the **decoded** value and `Set` takes one: percent-encoding is
/// a wire concern that `FromHeader` undoes and `ToHeader` reapplies. Keys are
/// RFC 7230 tokens, are never percent-decoded, and `%` is itself a token
/// character — so the key `%41` is three characters, not an escaped `A`.
///
/// ### Properties
///
/// The optional `;`-separated metadata tail of a list-member is preserved
/// **opaquely**: microtel carries it across a hop unchanged (modulo `OWS`
/// normalisation) but offers no way to read or write it, because ICP 0025 §2
/// locked a key/value surface. `Set` on an existing key replaces the whole
/// list-member, properties included.
///
/// @threadsafety Thread-safe for concurrent reads; an instance is an immutable
///               value.
///
/// @see docs/icps/0025-propagation-core.md §2
class Baggage
{
public:
    /// @brief Maximum number of list-members, from the grammar's `0*179`
    /// repetition of the `"," list-member` tail.
    static constexpr std::size_t kMaxEntries = 180;

    /// @brief Maximum serialised length of a single list-member.
    static constexpr std::size_t kMaxEntryBytes = 4096;

    /// @brief Maximum serialised length of the whole `baggage` header value,
    /// separators included — the specification's "size 8192 bytes or less".
    static constexpr std::size_t kMaxTotalBytes = 8192;

    Baggage() noexcept = default;

    /// @brief Parse a `baggage` header value.
    ///
    /// Parsing is **per list-member**, unlike `TraceState::FromHeader`, which
    /// discards the whole header on the first bad member. W3C Trace Context
    /// §4.3 explicitly permits that for `tracestate`; W3C Baggage says no such
    /// thing, and its only normative drop guidance is about individual
    /// list-members. So a malformed member is dropped and its neighbours
    /// survive. A member is malformed if its key is not a token, its value
    /// holds an octet outside `baggage-octet`, a percent-escape is truncated or
    /// not hexadecimal, or a property violates the `property` production.
    ///
    /// Limits are applied against the **canonical serialised** form of each
    /// member — what microtel would put back on the wire — so anything that
    /// fits once fits forever and `FromHeader(b.ToHeader())` reproduces `b`:
    ///
    ///  - a member longer than `kMaxEntryBytes` is **skipped**, and parsing
    ///    continues with the next one;
    ///  - reaching `kMaxEntries`, or a total that would exceed
    ///    `kMaxTotalBytes`, **stops** the parse, dropping the remaining
    ///    members from the end.
    ///
    /// A duplicate key keeps its first occurrence: the `Get` surface cannot
    /// represent two entries under one key, so the later one is dropped rather
    /// than left invisible in the serialised form.
    ///
    /// Returns the empty baggage if no member survives.
    [[nodiscard]] static Baggage FromHeader(std::string_view header);

    /// @brief Serialise to a `baggage` HTTP header value.
    ///
    /// Members are emitted in order, comma-separated, with no optional
    /// whitespace; values are percent-encoded. The empty baggage serialises to
    /// the empty string, which is what lets a propagator omit the header.
    [[nodiscard]] std::string ToHeader() const;

    /// @brief The decoded value stored under @p key, if any.
    ///
    /// The returned view is **borrowed** from this `Baggage`'s shared entry
    /// list and stays valid while any copy of this baggage lives.
    [[nodiscard]] std::optional<std::string_view> Get(std::string_view key) const noexcept;

    /// @brief Copy-on-write insert-or-update; this baggage is left unchanged.
    ///
    /// An existing key keeps its position and loses its properties — the
    /// caller set a plain value, and carrying invisible metadata forward under
    /// it would be a surprise. A new key is appended, which preserves the
    /// upstream order the specification asks mutators to keep.
    ///
    /// The mutation is refused — the return is an unchanged copy — if @p key is
    /// not a token, if the resulting member would exceed `kMaxEntryBytes` once
    /// @p value is percent-encoded, if the resulting header would exceed
    /// `kMaxTotalBytes`, or if @p key is new and the baggage already holds
    /// `kMaxEntries` entries. There is no error channel on this surface, and
    /// evicting another system's entry to make room would lose state microtel
    /// was asked to carry.
    ///
    /// @p value may hold any octets; anything outside `baggage-octet` is
    /// percent-encoded by `ToHeader` rather than rejected here.
    [[nodiscard]] Baggage Set(std::string_view key, std::string_view value) const;

    /// @brief Copy-on-write removal; this baggage is left unchanged.
    ///
    /// Returns an unchanged copy if @p key is absent. The order of the
    /// surviving entries is preserved.
    [[nodiscard]] Baggage Erase(std::string_view key) const;

    /// @brief Number of list-members.
    [[nodiscard]] std::size_t Size() const noexcept;

    /// @brief True if no list-members.
    [[nodiscard]] bool Empty() const noexcept;

private:
    /// @brief The shared entry list; null is the empty baggage.
    std::shared_ptr<const internal::BaggageImpl> m_impl;
};

// `Baggage` is a member of `Context`, which is copied into every
// `ScopedContext` and handed to every `ISpanProcessor::OnStart`. An allocating
// copy here would make `Context`'s `noexcept` copy a lie — this is the guard
// ICP 0025 §2 asks the implementing packet to plant.
static_assert(std::is_nothrow_copy_constructible_v<Baggage>,
              "Baggage's copy must not allocate — see ICP 0025 §2");
static_assert(std::is_nothrow_copy_assignable_v<Baggage>);
static_assert(std::is_nothrow_move_constructible_v<Baggage>);
static_assert(std::is_nothrow_move_assignable_v<Baggage>);

}  // namespace microtel
