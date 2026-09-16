// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// src/api/ — microtel::Baggage, declared in include/microtel/baggage.hpp.
// ICP 0025 §2, packet 2.3c.
//
// The entry representation lives here and nowhere else. `baggage.hpp` only
// forward-declares `internal::BaggageImpl`, so no other translation unit can
// depend on the layout and a later change to it is not an ABI event.
//
// Ownership: the list is immutable once built and held by
// `std::shared_ptr<const BaggageImpl>`. That is the rule-8 justification — a
// `unique_ptr` would force `Context`'s copy constructor to deep-copy the list,
// and a `Context` is copied into every `ScopedContext` and every
// `ISpanProcessor::OnStart`, where an allocating copy would break the
// `noexcept` contract ICP 0025 §2 depends on. Nothing ever mutates through the
// pointer; `Set` and `Erase` build a new list. The empty baggage is a null
// pointer, so a `Context` that carries none allocates nothing
// (`memory-model.md` §8.1).
//
// Dependency-free by design: the public header plus the standard library.

#include "microtel/baggage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::internal
{

/// @brief The immutable `baggage` list-member list.
struct BaggageImpl
{
    /// @brief One `key=value` list-member and its opaque property tail.
    struct Entry
    {
        /// RFC 7230 token, verbatim — never percent-decoded.
        std::string key;
        /// Percent-**decoded** value; may hold any octet.
        std::string value;
        /// Canonical `;`-joined property tail without its leading `;`, or
        /// empty. Carried opaquely: microtel neither reads nor writes it.
        std::string properties;
        /// Serialised length of this list-member, computed once at build time
        /// so the limit checks never have to re-encode the value.
        std::size_t member_bytes = 0;
    };

    std::vector<Entry> entries;
};

}  // namespace microtel::internal

namespace microtel
{

namespace
{

using Impl = internal::BaggageImpl;
using Entry = Impl::Entry;

constexpr char kListSeparator = ',';
constexpr char kKeyValueSeparator = '=';
constexpr char kPropertySeparator = ';';
constexpr char kEscapePrefix = '%';

// `baggage-octet = %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E` — printable
// US-ASCII without SP, DQUOTE, comma, semicolon and backslash.
constexpr unsigned char kFirstOctet = 0x21U;
constexpr unsigned char kLastOctet = 0x7EU;

constexpr std::string_view kUpperHexDigits = "0123456789ABCDEF";
constexpr unsigned int kNibbleShift = 4U;
constexpr unsigned int kLowNibbleMask = 0x0FU;
constexpr unsigned int kDecimalDigitCount = 10U;

/// @brief Bytes a percent-escape occupies: `%` and two hex digits.
constexpr std::size_t kEscapeChars = 3U;
/// @brief Bytes the `=` between a key and its value occupies.
constexpr std::size_t kKeyValueSeparatorChars = 1U;
/// @brief Bytes the `,` between two list-members occupies.
constexpr std::size_t kListSeparatorChars = 1U;
/// @brief Bytes the `;` before a property tail occupies.
constexpr std::size_t kPropertySeparatorChars = 1U;

[[nodiscard]] constexpr bool IsDigit(char c) noexcept
{
    return (c >= '0') && (c <= '9');
}

[[nodiscard]] constexpr bool IsAlpha(char c) noexcept
{
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'));
}

/// @brief `tchar`, RFC 7230 §3.2.6. Note that `%` is one of them, which is why
/// keys are never percent-decoded.
[[nodiscard]] constexpr bool IsTokenChar(char c) noexcept
{
    constexpr std::string_view kTokenSpecials = "!#$%&'*+-.^_`|~";
    return IsAlpha(c) || IsDigit(c) || (kTokenSpecials.find(c) != std::string_view::npos);
}

/// @brief `key = token`, which is `1*tchar`.
[[nodiscard]] bool IsToken(std::string_view text) noexcept
{
    return !text.empty() && std::ranges::all_of(text, IsTokenChar);
}

/// @brief `baggage-octet`.
[[nodiscard]] constexpr bool IsBaggageOctet(char c) noexcept
{
    const auto octet = static_cast<unsigned char>(c);
    if ((octet < kFirstOctet) || (octet > kLastOctet))
    {
        return false;
    }
    return (c != '"') && (c != kListSeparator) && (c != kPropertySeparator) && (c != '\\');
}

/// @brief True if @p text is a legal `value` as it appears on the wire.
[[nodiscard]] bool IsWireValue(std::string_view text) noexcept
{
    return std::ranges::all_of(text, IsBaggageOctet);
}

/// @brief `OWS = *( SP / HTAB )`, which surrounds every delimiter.
[[nodiscard]] constexpr bool IsOptionalWhitespace(char c) noexcept
{
    return (c == ' ') || (c == '\t');
}

/// @brief Strips leading and trailing `OWS`.
[[nodiscard]] std::string_view TrimOptionalWhitespace(std::string_view text) noexcept
{
    while (!text.empty() && IsOptionalWhitespace(text.front()))
    {
        text.remove_prefix(1U);
    }
    while (!text.empty() && IsOptionalWhitespace(text.back()))
    {
        text.remove_suffix(1U);
    }
    return text;
}

[[nodiscard]] constexpr bool IsHexDigit(char c) noexcept
{
    const bool is_lower = (c >= 'a') && (c <= 'f');
    const bool is_upper = (c >= 'A') && (c <= 'F');
    return IsDigit(c) || is_lower || is_upper;
}

/// @brief Numeric value of a character satisfying `IsHexDigit`.
[[nodiscard]] constexpr unsigned int HexDigitValue(char c) noexcept
{
    if (IsDigit(c))
    {
        return static_cast<unsigned int>(c - '0');
    }
    const bool is_lower = (c >= 'a') && (c <= 'f');
    const char base = is_lower ? 'a' : 'A';
    return static_cast<unsigned int>(c - base) + kDecimalDigitCount;
}

/// @brief Percent-decodes @p raw into @p out.
///
/// @return false if @p raw holds an octet outside `baggage-octet` or an escape
///         that is truncated or not hexadecimal. @p out is then meaningless.
[[nodiscard]] bool DecodeValue(std::string_view raw, std::string& out)
{
    out.reserve(raw.size());
    std::size_t at = 0;
    while (at < raw.size())
    {
        const char c = raw[at];
        if (!IsBaggageOctet(c))
        {
            return false;
        }
        if (c != kEscapePrefix)
        {
            out.push_back(c);
            ++at;
            continue;
        }
        if ((at + kEscapeChars) > raw.size())
        {
            return false;
        }
        const char high = raw[at + 1U];
        const char low = raw[at + 2U];
        if (!IsHexDigit(high) || !IsHexDigit(low))
        {
            return false;
        }
        out.push_back(
            static_cast<char>((HexDigitValue(high) << kNibbleShift) | HexDigitValue(low)));
        at += kEscapeChars;
    }
    return true;
}

/// @brief True if @p c cannot appear literally in a serialised value.
///
/// `%` is a `baggage-octet` but the specification requires it to be escaped, so
/// that an escape is never ambiguous.
[[nodiscard]] constexpr bool NeedsEscape(char c) noexcept
{
    return !IsBaggageOctet(c) || (c == kEscapePrefix);
}

/// @brief Serialised length of @p decoded once percent-encoded.
[[nodiscard]] std::size_t EncodedSize(std::string_view decoded) noexcept
{
    std::size_t size = 0;
    for (const char c : decoded)
    {
        size += NeedsEscape(c) ? kEscapeChars : 1U;
    }
    return size;
}

/// @brief Appends @p decoded to @p out, percent-encoding what must be.
void AppendEncoded(std::string_view decoded, std::string& out)
{
    for (const char c : decoded)
    {
        if (!NeedsEscape(c))
        {
            out.push_back(c);
            continue;
        }
        const auto octet = static_cast<unsigned int>(static_cast<unsigned char>(c));
        out.push_back(kEscapePrefix);
        out.push_back(kUpperHexDigits[(octet >> kNibbleShift) & kLowNibbleMask]);
        out.push_back(kUpperHexDigits[octet & kLowNibbleMask]);
    }
}

/// @brief Appends one canonicalised `property` to @p out, `;`-separated.
///
/// @return false if @p property violates `property = key OWS "=" OWS value /
///         key OWS`, which makes the whole list-member malformed.
[[nodiscard]] bool AppendProperty(std::string_view property, std::string& out)
{
    const std::size_t equals = property.find(kKeyValueSeparator);
    const std::string_view name = TrimOptionalWhitespace(property.substr(0U, equals));
    if (!IsToken(name))
    {
        return false;
    }

    std::string_view value;
    if (equals != std::string_view::npos)
    {
        value = TrimOptionalWhitespace(property.substr(equals + 1U));
        if (!IsWireValue(value))
        {
            return false;
        }
    }

    if (!out.empty())
    {
        out.push_back(kPropertySeparator);
    }
    out.append(name);
    if (equals != std::string_view::npos)
    {
        out.push_back(kKeyValueSeparator);
        out.append(value);
    }
    return true;
}

/// @brief Canonicalises the `;`-separated property tail of a list-member.
///
/// @param tail The text after the first `;`, which the grammar requires to
///             hold at least one property.
/// @return false if any property is malformed.
[[nodiscard]] bool ParseProperties(std::string_view tail, std::string& out)
{
    while (true)
    {
        const std::size_t semicolon = tail.find(kPropertySeparator);
        if (!AppendProperty(TrimOptionalWhitespace(tail.substr(0U, semicolon)), out))
        {
            return false;
        }
        if (semicolon == std::string_view::npos)
        {
            return true;
        }
        tail = tail.substr(semicolon + 1U);
    }
}

/// @brief Builds an entry and computes its serialised length once, so the
/// limit checks never have to re-encode the value.
[[nodiscard]] Entry MakeEntry(std::string_view key, std::string value, std::string properties)
{
    const std::size_t property_bytes =
        properties.empty() ? 0U : (kPropertySeparatorChars + properties.size());
    const std::size_t member_bytes =
        key.size() + kKeyValueSeparatorChars + EncodedSize(value) + property_bytes;
    return Entry{
        .key = std::string(key),
        .value = std::move(value),
        .properties = std::move(properties),
        .member_bytes = member_bytes,
    };
}

/// @brief Parses one already-trimmed list-member.
///
/// @return The entry, or `std::nullopt` if the member violates the grammar.
[[nodiscard]] std::optional<Entry> ParseMember(std::string_view member)
{
    const std::size_t semicolon = member.find(kPropertySeparator);
    const std::string_view head = member.substr(0U, semicolon);

    // `=` is itself a `baggage-octet` and never a `tchar`, so the first one
    // always separates the key from the value.
    const std::size_t equals = head.find(kKeyValueSeparator);
    if (equals == std::string_view::npos)
    {
        return std::nullopt;
    }

    const std::string_view key = TrimOptionalWhitespace(head.substr(0U, equals));
    if (!IsToken(key))
    {
        return std::nullopt;
    }

    std::string value;
    if (!DecodeValue(TrimOptionalWhitespace(head.substr(equals + 1U)), value))
    {
        return std::nullopt;
    }

    std::string properties;
    if ((semicolon != std::string_view::npos) &&
        !ParseProperties(member.substr(semicolon + 1U), properties))
    {
        return std::nullopt;
    }

    return MakeEntry(key, std::move(value), std::move(properties));
}

/// @brief True if @p entries already carries @p key.
[[nodiscard]] bool Contains(const std::vector<Entry>& entries, std::string_view key) noexcept
{
    return std::ranges::any_of(entries, [key](const Entry& entry) { return entry.key == key; });
}

/// @brief Serialised length of @p entries plus one more member of
/// @p member_bytes, separators included.
[[nodiscard]] std::size_t TotalWith(std::size_t total,
                                    std::size_t count,
                                    std::size_t member_bytes) noexcept
{
    const std::size_t separator = (count == 0U) ? 0U : kListSeparatorChars;
    return total + separator + member_bytes;
}

/// @brief Serialised length of a whole entry list, separators included.
[[nodiscard]] std::size_t SerialisedSize(const std::vector<Entry>& entries) noexcept
{
    if (entries.empty())
    {
        return 0U;
    }
    std::size_t total = (entries.size() - 1U) * kListSeparatorChars;
    for (const Entry& entry : entries)
    {
        total += entry.member_bytes;
    }
    return total;
}

/// @brief What the limits say about one candidate member.
enum class Admission : std::uint8_t
{
    Accepted,  ///< Append it.
    Skipped,   ///< Drop it; later members may still be admissible.
    Stopped,   ///< Drop it and everything after it.
};

/// @brief Applies the three W3C Baggage limits to one candidate.
///
/// A member over `kMaxEntryBytes` costs only itself, but a full list or a full
/// header ends the parse: the specification's guidance is to drop list-members
/// until the size and count conditions hold, and dropping from the end is both
/// the cheapest reading and the one that keeps the surviving prefix stable.
[[nodiscard]] Admission Admit(const Entry& candidate,
                              const std::vector<Entry>& entries,
                              std::size_t total) noexcept
{
    if (candidate.member_bytes > Baggage::kMaxEntryBytes)
    {
        return Admission::Skipped;
    }
    if (Contains(entries, candidate.key))
    {
        return Admission::Skipped;
    }
    if (entries.size() >= Baggage::kMaxEntries)
    {
        return Admission::Stopped;
    }
    if (TotalWith(total, entries.size(), candidate.member_bytes) > Baggage::kMaxTotalBytes)
    {
        return Admission::Stopped;
    }
    return Admission::Accepted;
}

/// @brief Appends the surviving members of @p header to @p entries.
void ParseMembers(std::string_view header, std::vector<Entry>& entries)
{
    std::size_t total = 0;
    while (!header.empty())
    {
        const std::size_t comma = header.find(kListSeparator);
        const std::string_view member = TrimOptionalWhitespace(header.substr(0U, comma));
        header = (comma == std::string_view::npos) ? std::string_view{} : header.substr(comma + 1U);

        std::optional<Entry> entry = member.empty() ? std::nullopt : ParseMember(member);
        if (!entry.has_value())
        {
            continue;
        }

        const Admission verdict = Admit(*entry, entries, total);
        if (verdict == Admission::Stopped)
        {
            return;
        }
        if (verdict == Admission::Skipped)
        {
            continue;
        }
        total = TotalWith(total, entries.size(), entry->member_bytes);
        entries.push_back(std::move(*entry));
    }
}

/// @brief Wraps a freshly built entry list, collapsing an empty one to the
/// null-pointer empty state.
[[nodiscard]] std::shared_ptr<const Impl> Seal(std::vector<Entry>&& entries)
{
    if (entries.empty())
    {
        return nullptr;
    }
    auto impl = std::make_shared<Impl>();
    impl->entries = std::move(entries);
    return impl;
}

}  // namespace

Baggage Baggage::FromHeader(std::string_view header)
{
    std::vector<Entry> entries;
    ParseMembers(header, entries);

    Baggage bag;
    bag.m_impl = Seal(std::move(entries));
    return bag;
}

std::string Baggage::ToHeader() const
{
    if (m_impl == nullptr)
    {
        return {};
    }

    std::string header;
    header.reserve(SerialisedSize(m_impl->entries));
    for (const Entry& entry : m_impl->entries)
    {
        if (!header.empty())
        {
            header.push_back(kListSeparator);
        }
        header.append(entry.key);
        header.push_back(kKeyValueSeparator);
        AppendEncoded(entry.value, header);
        if (!entry.properties.empty())
        {
            header.push_back(kPropertySeparator);
            header.append(entry.properties);
        }
    }
    return header;
}

std::optional<std::string_view> Baggage::Get(std::string_view key) const noexcept
{
    if (m_impl == nullptr)
    {
        return std::nullopt;
    }
    for (const Entry& entry : m_impl->entries)
    {
        if (entry.key == key)
        {
            return std::string_view(entry.value);
        }
    }
    return std::nullopt;
}

Baggage Baggage::Set(std::string_view key, std::string_view value) const
{
    const bool replaces_existing = Get(key).has_value();
    if (!IsToken(key) || (!replaces_existing && (Size() >= kMaxEntries)))
    {
        return *this;
    }

    // A replacement drops the entry's properties: the public surface can
    // neither read nor write them, so carrying invisible metadata forward under
    // a caller's new value would be a surprise.
    Entry candidate = MakeEntry(key, std::string(value), std::string{});
    if (candidate.member_bytes > kMaxEntryBytes)
    {
        return *this;
    }

    std::vector<Entry> entries;
    if (m_impl != nullptr)
    {
        entries = m_impl->entries;
    }
    entries.reserve(entries.size() + 1U);

    const auto existing = std::ranges::find(entries, candidate.key, &Entry::key);
    if (existing != entries.end())
    {
        *existing = std::move(candidate);
    }
    else
    {
        entries.push_back(std::move(candidate));
    }

    if (SerialisedSize(entries) > kMaxTotalBytes)
    {
        return *this;
    }

    Baggage bag;
    bag.m_impl = Seal(std::move(entries));
    return bag;
}

Baggage Baggage::Erase(std::string_view key) const
{
    if (!Get(key).has_value())
    {
        return *this;
    }

    std::vector<Entry> entries;
    entries.reserve(Size() - 1U);
    for (const Entry& entry : m_impl->entries)
    {
        if (entry.key != key)
        {
            entries.push_back(entry);
        }
    }

    Baggage bag;
    bag.m_impl = Seal(std::move(entries));
    return bag;
}

std::size_t Baggage::Size() const noexcept
{
    return (m_impl == nullptr) ? 0U : m_impl->entries.size();
}

bool Baggage::Empty() const noexcept
{
    return Size() == 0U;
}

}  // namespace microtel
