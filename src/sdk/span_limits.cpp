// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/span_limits.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace microtel::sdk
{
namespace
{

/// @brief Longest prefix of @p s that is at most @p limit bytes and does not
///        split a UTF-8 code point.
/// @pre `limit < s.size()` — the caller has already decided to truncate.
[[nodiscard]] std::size_t Utf8SafePrefix(std::string_view s, std::size_t limit) noexcept
{
    constexpr auto kContinuationMask = static_cast<unsigned char>(0xC0U);
    constexpr auto kContinuationBits = static_cast<unsigned char>(0x80U);

    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & kContinuationMask) == kContinuationBits)
    {
        --cut;
    }
    return cut;
}

void Count(internal::IDiagnosticsSink* diag, DropReason reason, std::uint64_t n) noexcept
{
    if (n > 0 && diag != nullptr)
    {
        diag->RecordDrop(reason, n);
    }
}

/// Keep the first @p limit elements of @p items; return how many went.
template <typename T>
[[nodiscard]] std::uint64_t KeepFirst(std::vector<T>& items, std::size_t limit) noexcept
{
    if (items.size() <= limit)
    {
        return 0;
    }
    const std::size_t surplus = items.size() - limit;
    items.erase(std::next(items.begin(), static_cast<std::ptrdiff_t>(limit)), items.end());
    return surplus;
}

/// Clip the string values of @p attributes; return how many were clipped.
[[nodiscard]] std::uint64_t TruncateAll(std::vector<KeyValue>& attributes,
                                        std::size_t limit) noexcept
{
    std::uint64_t truncated = 0;
    for (auto& kv : attributes)
    {
        truncated += TruncateStrings(kv.value, limit);
    }
    return truncated;
}

}  // namespace

std::uint64_t TruncateStrings(AttributeValue& value, std::size_t limit) noexcept
{
    if (auto* const s = std::get_if<std::string>(&value); s != nullptr)
    {
        if (s->size() <= limit)
        {
            return 0;
        }
        s->resize(Utf8SafePrefix(*s, limit));
        return 1;
    }

    auto* const arr = std::get_if<std::vector<std::string>>(&value);
    if (arr == nullptr)
    {
        return 0;
    }
    std::uint64_t truncated = 0;
    for (auto& element : *arr)
    {
        if (element.size() > limit)
        {
            element.resize(Utf8SafePrefix(element, limit));
            ++truncated;
        }
    }
    return truncated;
}

void ApplySpanLimits(internal::SpanRecord& record,
                     const SpanLimitOptions& limits,
                     internal::IDiagnosticsSink* diag) noexcept
{
    const std::size_t value_limit = limits.attribute_value_length_limit;

    Count(diag,
          DropReason::SpanAttributeLimit,
          KeepFirst(record.attributes, limits.attribute_count_limit));
    std::uint64_t truncated = TruncateAll(record.attributes, value_limit);

    Count(diag, DropReason::SpanEventLimit, KeepFirst(record.events, limits.event_count_limit));
    std::uint64_t event_attrs = 0;
    for (auto& event : record.events)
    {
        event_attrs += KeepFirst(event.attributes, limits.event_attribute_count_limit);
        truncated += TruncateAll(event.attributes, value_limit);
    }
    Count(diag, DropReason::EventAttributeLimit, event_attrs);

    Count(diag, DropReason::SpanLinkLimit, KeepFirst(record.links, limits.link_count_limit));
    std::uint64_t link_attrs = 0;
    for (auto& link : record.links)
    {
        link_attrs += KeepFirst(link.attributes, limits.link_attribute_count_limit);
        truncated += TruncateAll(link.attributes, value_limit);
    }
    Count(diag, DropReason::LinkAttributeLimit, link_attrs);

    Count(diag, DropReason::AttributeValueTruncated, truncated);
}

}  // namespace microtel::sdk
