// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/table_merge.hpp"

#include "microtel/attribute.hpp"
#include "microtel/resource.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::config
{

namespace
{

/// ASCII case-insensitive equality — HTTP field names (RFC 9110 §5.1).
[[nodiscard]] bool HeaderNameEquals(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a,
                              b,
                              [](char x, char y)
                              {
                                  return std::tolower(static_cast<unsigned char>(x)) ==
                                         std::tolower(static_cast<unsigned char>(y));
                              });
}

/// Put `kv` in `table` in place of its first case-insensitive match (or at the
/// end if none), and drop every later case variant of the same name.
void OverrideHeader(std::vector<KeyValue>& table, const KeyValue& kv)
{
    const auto same = [&kv](const KeyValue& e) { return HeaderNameEquals(e.key, kv.key); };
    const auto first = std::ranges::find_if(table, same);
    if (first == table.end())
    {
        table.push_back(kv);
        return;
    }
    *first = kv;
    const auto rest = std::ranges::remove_if(first + 1, table.end(), same);
    table.erase(rest.begin(), rest.end());
}

}  // namespace

void MergeResourceAttrs(std::vector<KeyValue>& base, const std::vector<KeyValue>& overriding)
{
    base = Resource::Merge(Resource{std::move(base)}, Resource{overriding}).Attributes();
}

void MergeHeaders(std::vector<KeyValue>& base, const std::vector<KeyValue>& overriding)
{
    for (const auto& kv : overriding)
    {
        OverrideHeader(base, kv);
    }
}

}  // namespace microtel::config
