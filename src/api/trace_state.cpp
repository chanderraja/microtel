// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// src/api/ — microtel::TraceState, declared in include/microtel/trace.hpp.
// Issue #208 / ICP 0025 packet 2.3a.
//
// The entry representation lives here and nowhere else. `trace.hpp` only
// forward-declares `internal::TraceStateImpl`, so no other translation unit
// can depend on the layout and a later change to it is not a second ABI
// event (ICP 0025 §1).
//
// Ownership: the list is immutable once built and held by
// `std::shared_ptr<const TraceStateImpl>`. That is the rule-8 justification —
// a `unique_ptr` would force `SpanContext`'s copy constructor to deep-copy the
// list, putting an allocation (and a throw) inside
// `Span::GetContext() const noexcept`. Nothing ever mutates through the
// pointer; `Set` and `Erase` build a new list. The empty state is a null
// pointer, so the `SpanContext` of an unsampled span still allocates nothing
// (`memory-model.md` §8.1).
//
// Dependency-free by design: the public header plus the standard library.

#include "microtel/trace.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::internal
{

/// @brief The immutable `tracestate` entry list.
struct TraceStateImpl
{
    /// @brief One `key=value` list member.
    struct Entry
    {
        std::string key;
        std::string value;
    };

    std::vector<Entry> entries;
};

}  // namespace microtel::internal

namespace microtel
{

namespace
{

using Impl = internal::TraceStateImpl;
using Entry = Impl::Entry;

constexpr char kListSeparator = ',';
constexpr char kKeyValueSeparator = '=';
constexpr char kTenantSeparator = '@';

// W3C Trace Context §3.3, `key`:
//
//   key              = simple-key / multi-tenant-key
//   simple-key       = lcalpha 0*255( lcalpha / DIGIT / "_" / "-" / "*" / "/" )
//   multi-tenant-key = tenant-id "@" system-id
//   tenant-id        = ( lcalpha / DIGIT ) 0*240( lcalpha / DIGIT / "_" / "-" / "*" / "/" )
//   system-id        = lcalpha 0*13( lcalpha / DIGIT / "_" / "-" / "*" / "/" )
constexpr std::size_t kMaxSimpleKeyChars = 256U;
constexpr std::size_t kMaxTenantIdChars = 241U;
constexpr std::size_t kMaxSystemIdChars = 14U;

// W3C Trace Context §3.3, `value`:
//
//   value    = 0*255(chr) nblk-chr
//   nblk-chr = %x21-2B / %x2D-3C / %x3E-7E
//   chr      = %x20 / nblk-chr
constexpr std::size_t kMaxValueChars = 256U;
constexpr char kValueLowestChar = ' ';   ///< %x20, a `chr` but not an `nblk-chr`
constexpr char kValueHighestChar = '~';  ///< %x7E

/// @brief `lcalpha = %x61-7A`.
[[nodiscard]] constexpr bool IsLowerAlpha(char c) noexcept
{
    return (c >= 'a') && (c <= 'z');
}

[[nodiscard]] constexpr bool IsDigit(char c) noexcept
{
    return (c >= '0') && (c <= '9');
}

/// @brief A character legal anywhere after the first in any key component.
[[nodiscard]] constexpr bool IsKeyTailChar(char c) noexcept
{
    return IsLowerAlpha(c) || IsDigit(c) || (c == '_') || (c == '-') || (c == '*') || (c == '/');
}

/// @brief `OWS = *( SP / HTAB )`, which surrounds the list separator.
[[nodiscard]] constexpr bool IsOptionalWhitespace(char c) noexcept
{
    return (c == ' ') || (c == '\t');
}

/// @brief True if every character after the first satisfies `IsKeyTailChar`.
/// @pre @p component is non-empty.
[[nodiscard]] bool HasValidKeyTail(std::string_view component) noexcept
{
    return std::ranges::all_of(component.substr(1U), IsKeyTailChar);
}

/// @brief `simple-key`, and also `system-id` at its own length limit.
[[nodiscard]] bool IsLowerAlphaLedComponent(std::string_view component, std::size_t max_chars)
{
    if (component.empty() || (component.size() > max_chars))
    {
        return false;
    }
    return IsLowerAlpha(component.front()) && HasValidKeyTail(component);
}

/// @brief `tenant-id` — the only component that may open with a DIGIT.
[[nodiscard]] bool IsTenantId(std::string_view component)
{
    if (component.empty() || (component.size() > kMaxTenantIdChars))
    {
        return false;
    }
    const char first = component.front();
    return (IsLowerAlpha(first) || IsDigit(first)) && HasValidKeyTail(component);
}

/// @brief `key = simple-key / multi-tenant-key`.
[[nodiscard]] bool IsValidKey(std::string_view key)
{
    const std::size_t at = key.find(kTenantSeparator);
    if (at == std::string_view::npos)
    {
        return IsLowerAlphaLedComponent(key, kMaxSimpleKeyChars);
    }
    // A second `@` lands in the system-id, where it is not a legal character,
    // so splitting on the first one rejects `a@b@c` without a special case.
    return IsTenantId(key.substr(0U, at)) &&
           IsLowerAlphaLedComponent(key.substr(at + 1U), kMaxSystemIdChars);
}

/// @brief `chr = %x20 / nblk-chr` — printable ASCII minus `,` and `=`.
[[nodiscard]] constexpr bool IsValueChar(char c) noexcept
{
    const bool in_printable_range = (c >= kValueLowestChar) && (c <= kValueHighestChar);
    return in_printable_range && (c != kListSeparator) && (c != kKeyValueSeparator);
}

/// @brief `value = 0*255(chr) nblk-chr`.
[[nodiscard]] bool IsValidValue(std::string_view value)
{
    if (value.empty() || (value.size() > kMaxValueChars))
    {
        return false;
    }
    if (value.back() == kValueLowestChar)  // must end in an `nblk-chr`
    {
        return false;
    }
    return std::ranges::all_of(value, IsValueChar);
}

/// @brief Strips leading and trailing `OWS` from a list member.
[[nodiscard]] std::string_view TrimOptionalWhitespace(std::string_view member) noexcept
{
    while (!member.empty() && IsOptionalWhitespace(member.front()))
    {
        member.remove_prefix(1U);
    }
    while (!member.empty() && IsOptionalWhitespace(member.back()))
    {
        member.remove_suffix(1U);
    }
    return member;
}

/// @brief True if @p entries already carries @p key.
[[nodiscard]] bool Contains(const std::vector<Entry>& entries, std::string_view key) noexcept
{
    return std::ranges::any_of(entries, [key](const Entry& entry) { return entry.key == key; });
}

/// @brief Appends every entry of @p source whose key differs from @p key.
///
/// @param source Borrowed entry list, or nullptr for the empty state.
void AppendExcept(const Impl* source, std::string_view key, std::vector<Entry>& destination)
{
    if (source == nullptr)
    {
        return;
    }
    for (const Entry& entry : source->entries)
    {
        if (entry.key != key)
        {
            destination.push_back(entry);
        }
    }
}

/// @brief Splits one already-trimmed, non-empty list member.
///
/// @return The entry, or `std::nullopt` if the member violates the grammar.
[[nodiscard]] std::optional<Entry> ParseMember(std::string_view member)
{
    const std::size_t equals = member.find(kKeyValueSeparator);
    if (equals == std::string_view::npos)
    {
        return std::nullopt;
    }

    const std::string_view key = member.substr(0U, equals);
    const std::string_view value = member.substr(equals + 1U);
    if (!IsValidKey(key) || !IsValidValue(value))
    {
        return std::nullopt;
    }
    return Entry{.key = std::string(key), .value = std::string(value)};
}

/// @brief Appends the members of @p header to @p entries.
///
/// @return false on the first violation — a malformed member, a duplicate key,
///         or a `kMaxEntries + 1`-th member. @p entries is then meaningless
///         and the whole header is discarded (W3C §4.3).
[[nodiscard]] bool ParseMembers(std::string_view header, std::vector<Entry>& entries)
{
    while (!header.empty())
    {
        const std::size_t comma = header.find(kListSeparator);
        const std::string_view member = TrimOptionalWhitespace(header.substr(0U, comma));
        header = (comma == std::string_view::npos) ? std::string_view{} : header.substr(comma + 1U);

        if (member.empty())  // `list-member = ... / OWS`
        {
            continue;
        }
        if (entries.size() == TraceState::kMaxEntries)
        {
            return false;
        }

        std::optional<Entry> entry = ParseMember(member);
        if (!entry.has_value() || Contains(entries, entry->key))
        {
            return false;
        }
        entries.push_back(std::move(*entry));
    }
    return true;
}

}  // namespace

TraceState TraceState::FromHeader(std::string_view header)
{
    auto impl = std::make_shared<Impl>();
    if (!ParseMembers(header, impl->entries) || impl->entries.empty())
    {
        return {};
    }

    TraceState state;
    state.m_entries = std::move(impl);
    return state;
}

std::string TraceState::ToHeader() const
{
    if (m_entries == nullptr)
    {
        return {};
    }

    std::string header;
    for (const Entry& entry : m_entries->entries)
    {
        if (!header.empty())
        {
            header.push_back(kListSeparator);
        }
        header.append(entry.key);
        header.push_back(kKeyValueSeparator);
        header.append(entry.value);
    }
    return header;
}

std::size_t TraceState::Size() const noexcept
{
    return (m_entries == nullptr) ? 0U : m_entries->entries.size();
}

bool TraceState::Empty() const noexcept
{
    return Size() == 0U;
}

std::optional<std::string_view> TraceState::Get(std::string_view key) const noexcept
{
    if (m_entries == nullptr)
    {
        return std::nullopt;
    }
    for (const Entry& entry : m_entries->entries)
    {
        if (entry.key == key)
        {
            return std::string_view(entry.value);
        }
    }
    return std::nullopt;
}

TraceState TraceState::Set(std::string_view key, std::string_view value) const
{
    const bool replaces_existing = Get(key).has_value();
    const bool would_overflow = !replaces_existing && (Size() >= kMaxEntries);
    if (!IsValidKey(key) || !IsValidValue(value) || would_overflow)
    {
        return *this;
    }

    auto impl = std::make_shared<Impl>();
    impl->entries.reserve(Size() + 1U);
    // W3C §3.3.1: the new or modified member leads the list.
    impl->entries.push_back(Entry{.key = std::string(key), .value = std::string(value)});
    AppendExcept(m_entries.get(), key, impl->entries);

    TraceState state;
    state.m_entries = std::move(impl);
    return state;
}

TraceState TraceState::Erase(std::string_view key) const
{
    if (!Get(key).has_value())
    {
        return *this;
    }
    if (Size() == 1U)
    {
        return {};  // the empty state is always the null pointer
    }

    auto impl = std::make_shared<Impl>();
    impl->entries.reserve(Size() - 1U);
    AppendExcept(m_entries.get(), key, impl->entries);

    TraceState state;
    state.m_entries = std::move(impl);
    return state;
}

}  // namespace microtel
