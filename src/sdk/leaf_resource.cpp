// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/leaf_resource.hpp"

#include "microtel/attribute.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/resource.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace microtel::sdk
{
namespace
{

constexpr std::string_view kProtoKey = "microtel.leaf.proto";
constexpr std::string_view kTimeModeKey = "microtel.leaf.time_mode";
constexpr std::string_view kEncodeTimeKey = "microtel.leaf.encode_time";
constexpr std::string_view kSyncAgeKey = "microtel.leaf.sync_age";
constexpr std::string_view kBootIdKey = "microtel.leaf.boot_id";
constexpr std::string_view kDroppedSpansKey = "microtel.leaf.dropped_spans";
constexpr std::string_view kDroppedItemsKey = "microtel.leaf.dropped_items";

constexpr std::string_view kServiceNameKey = "service.name";
/// What the OTel SDK specification requires for a missing service name.
constexpr std::string_view kUnknownService = "unknown_service";

/// Width charged for one boolean, integer or double (§4.5).
constexpr std::size_t kScalarBytes = 8;

// FNV-1a, 64-bit.
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] bool IsReserved(std::string_view key) noexcept
{
    return key.starts_with(kLeafReservedPrefix);
}

/// The slot of @p info a reserved key fills, or nullptr for a key this
/// concentrator does not know (ignored, and still stripped).
[[nodiscard]] std::optional<std::int64_t>* SlotFor(LeafWireInfo& info,
                                                   std::string_view key) noexcept
{
    const std::pair<std::string_view, std::optional<std::int64_t>*> slots[] = {
        {kProtoKey, &info.proto},
        {kTimeModeKey, &info.time_mode},
        {kEncodeTimeKey, &info.encode_time},
        {kSyncAgeKey, &info.sync_age},
        {kBootIdKey, &info.boot_id},
        {kDroppedSpansKey, &info.dropped_spans},
        {kDroppedItemsKey, &info.dropped_items},
    };
    for (const auto& [name, slot] : slots)
    {
        if (name == key)
        {
            return slot;
        }
    }
    return nullptr;
}

class Fnv
{
public:
    void Add(const void* data, std::size_t size) noexcept
    {
        for (const unsigned char byte : std::span{static_cast<const unsigned char*>(data), size})
        {
            m_hash = (m_hash ^ byte) * kFnvPrime;
        }
    }

    void Add(std::string_view s) noexcept
    {
        const std::size_t size = s.size();
        Add(&size, sizeof(size));
        Add(s.data(), s.size());
    }

    template <typename T>
        requires std::is_arithmetic_v<T>
    void Add(T v) noexcept
    {
        Add(&v, sizeof(v));
    }

    [[nodiscard]] std::uint64_t Value() const noexcept
    {
        return m_hash;
    }

private:
    std::uint64_t m_hash = kFnvOffset;
};

void HashElements(Fnv& fnv, const std::vector<std::string>& values) noexcept
{
    fnv.Add(values.size());
    for (const auto& v : values)
    {
        fnv.Add(std::string_view{v});
    }
}

template <typename T>
void HashElements(Fnv& fnv, const std::vector<T>& values) noexcept
{
    fnv.Add(values.size());
    for (const T v : values)
    {
        fnv.Add(v);
    }
}

void HashValue(Fnv& fnv, const AttributeValue& value) noexcept
{
    fnv.Add(value.index());
    if (const auto* const s = std::get_if<std::string>(&value); s != nullptr)
    {
        fnv.Add(std::string_view{*s});
    }
    else if (const auto* const b = std::get_if<bool>(&value); b != nullptr)
    {
        fnv.Add(*b);
    }
    else if (const auto* const i = std::get_if<std::int64_t>(&value); i != nullptr)
    {
        fnv.Add(*i);
    }
    else if (const auto* const d = std::get_if<double>(&value); d != nullptr)
    {
        fnv.Add(*d);
    }
    else if (const auto* const strings = std::get_if<std::vector<std::string>>(&value);
             strings != nullptr)
    {
        HashElements(fnv, *strings);
    }
    else if (const auto* const bools = std::get_if<std::vector<bool>>(&value); bools != nullptr)
    {
        HashElements(fnv, *bools);
    }
    else if (const auto* const ints = std::get_if<std::vector<std::int64_t>>(&value);
             ints != nullptr)
    {
        HashElements(fnv, *ints);
    }
    else if (const auto* const doubles = std::get_if<std::vector<double>>(&value);
             doubles != nullptr)
    {
        HashElements(fnv, *doubles);
    }
}

[[nodiscard]] std::size_t ValueBytes(const AttributeValue& value) noexcept
{
    if (const auto* const s = std::get_if<std::string>(&value); s != nullptr)
    {
        return s->size();
    }
    if (const auto* const strings = std::get_if<std::vector<std::string>>(&value);
        strings != nullptr)
    {
        std::size_t total = 0;
        for (const auto& element : *strings)
        {
            total += element.size();
        }
        return total;
    }
    if (const auto* const bools = std::get_if<std::vector<bool>>(&value); bools != nullptr)
    {
        return bools->size() * kScalarBytes;
    }
    if (const auto* const ints = std::get_if<std::vector<std::int64_t>>(&value); ints != nullptr)
    {
        return ints->size() * kScalarBytes;
    }
    if (const auto* const doubles = std::get_if<std::vector<double>>(&value); doubles != nullptr)
    {
        return doubles->size() * kScalarBytes;
    }
    return kScalarBytes;
}

/// Running byte cost of a Resource under construction, key by key.
class Budget
{
public:
    /// What the total would be with @p key set to a value of @p value_bytes.
    [[nodiscard]] std::size_t TotalWith(std::string_view key, std::size_t value_bytes) const
    {
        const auto it = m_values.find(key);
        return it == m_values.end() ? m_total + key.size() + value_bytes
                                    : m_total - it->second + value_bytes;
    }

    void Set(std::string_view key, std::size_t value_bytes)
    {
        m_total = TotalWith(key, value_bytes);
        m_values[key] = value_bytes;
    }

    [[nodiscard]] bool Has(std::string_view key) const
    {
        return m_values.contains(key);
    }

private:
    std::unordered_map<std::string_view, std::size_t> m_values;
    std::size_t m_total = 0;
};

[[nodiscard]] Resource Layer(std::string_view key, std::string_view value)
{
    return Resource{std::vector<KeyValue>{{.key = std::string{key}, .value = std::string{value}}}};
}

[[nodiscard]] bool Names(const Resource& resource, std::string_view key) noexcept
{
    const auto& attrs = resource.Attributes();
    return std::ranges::find(attrs, key, &KeyValue::key) != attrs.end();
}

}  // namespace

LeafWireInfo ReadWireInfo(const std::vector<KeyValue>& resource) noexcept
{
    LeafWireInfo info;
    for (const auto& kv : resource)
    {
        if (!IsReserved(kv.key))
        {
            continue;
        }
        auto* const slot = SlotFor(info, kv.key);
        if (slot == nullptr)
        {
            continue;
        }
        const auto* const v = std::get_if<std::int64_t>(&kv.value);
        if (v == nullptr)
        {
            info.wrong_type = true;
            continue;
        }
        *slot = *v;
    }
    return info;
}

std::optional<LeafTimeMode> CheckWireInfo(const LeafWireInfo& info) noexcept
{
    if (info.wrong_type || info.proto != kLeafProtoVersion || !info.time_mode.has_value())
    {
        return std::nullopt;
    }
    switch (*info.time_mode)
    {
        case static_cast<std::int64_t>(LeafTimeMode::ConcentratorStamped):
            // A leaf with no clock sends no encode time (§5.2).
            return LeafTimeMode::ConcentratorStamped;
        case static_cast<std::int64_t>(LeafTimeMode::SyncRelative):
            if (!info.encode_time.has_value() || !info.sync_age.has_value())
            {
                return std::nullopt;
            }
            return LeafTimeMode::SyncRelative;
        case static_cast<std::int64_t>(LeafTimeMode::BootRelative):
            if (!info.encode_time.has_value() || !info.boot_id.has_value())
            {
                return std::nullopt;
            }
            return LeafTimeMode::BootRelative;
        default:
            return std::nullopt;
    }
}

bool TimeModeAllowed(LeafTimeMode declared, std::optional<LeafTimeMode> configured) noexcept
{
    return !configured.has_value() || declared == *configured ||
           declared == LeafTimeMode::ConcentratorStamped;
}

std::uint64_t HashDeclaredResource(const std::vector<KeyValue>& resource) noexcept
{
    Fnv fnv;
    for (const auto& kv : resource)
    {
        if (IsReserved(kv.key))
        {
            continue;
        }
        fnv.Add(std::string_view{kv.key});
        HashValue(fnv, kv.value);
    }
    return fnv.Value();
}

std::size_t AttributeBytes(const KeyValue& kv) noexcept
{
    return kv.key.size() + ValueBytes(kv.value);
}

namespace
{

const std::vector<KeyValue>& OrNone(const std::vector<KeyValue>* layer) noexcept
{
    static const std::vector<KeyValue> kNone;
    return layer != nullptr ? *layer : kNone;
}

/// Charge every layer but the leaf's own, which are fixed; return the keys a
/// layer above the leaf's pins. A declared value for a pinned key costs
/// nothing, because it never reaches the merged Resource.
std::unordered_set<std::string_view> ChargeFixedLayers(const LeafResourceLayers& layers,
                                                       Budget& budget)
{
    std::unordered_set<std::string_view> pinned;
    for (const auto& kv : OrNone(layers.defaults))
    {
        budget.Set(kv.key, ValueBytes(kv.value));
    }
    for (const auto& kv : OrNone(layers.configured))
    {
        budget.Set(kv.key, ValueBytes(kv.value));
        pinned.insert(kv.key);
    }
    if (!layers.id_key.empty())
    {
        budget.Set(layers.id_key, layers.leaf_id.size());
        pinned.insert(layers.id_key);
    }
    if (!budget.Has(kServiceNameKey))
    {
        budget.Set(kServiceNameKey, kUnknownService.size());
    }
    return pinned;
}

/// The leaf-declared attributes that fit, in payload order; @p dropped counts
/// the ones that did not.
std::vector<KeyValue> AdmitDeclared(const LeafResourceLayers& layers,
                                    const std::unordered_set<std::string_view>& pinned,
                                    Budget& budget,
                                    std::uint64_t& dropped)
{
    std::vector<KeyValue> admitted;
    for (const auto& kv : OrNone(layers.declared))
    {
        if (IsReserved(kv.key) || pinned.contains(kv.key))
        {
            continue;
        }
        const std::size_t value_bytes = ValueBytes(kv.value);
        if (budget.TotalWith(kv.key, value_bytes) > layers.budget)
        {
            ++dropped;
            continue;
        }
        budget.Set(kv.key, value_bytes);
        admitted.push_back(kv);
    }
    return admitted;
}

}  // namespace

std::uint64_t MergeResolverResource(std::vector<KeyValue>& configured,
                                    const std::vector<KeyValue>& answer,
                                    const LeafResourceLayers& fixed)
{
    // The budget holds views of the keys it has charged, so it charges a copy
    // that does not move while `configured` grows.
    const std::vector<KeyValue> base = configured;
    LeafResourceLayers layers = fixed;
    layers.declared = nullptr;
    layers.configured = &base;
    Budget budget;
    (void)ChargeFixedLayers(layers, budget);
    std::uint64_t dropped = 0;
    for (const auto& kv : answer)
    {
        if (IsReserved(kv.key) || (!fixed.id_key.empty() && kv.key == fixed.id_key))
        {
            continue;
        }
        const std::size_t value_bytes = ValueBytes(kv.value);
        if (budget.TotalWith(kv.key, value_bytes) > fixed.budget)
        {
            ++dropped;
            continue;
        }
        const auto it = std::ranges::find(configured, kv.key, &KeyValue::key);
        if (it == configured.end())
        {
            configured.push_back(kv);
        }
        else
        {
            it->value = kv.value;
        }
        budget.Set(kv.key, value_bytes);
    }
    return dropped;
}

ResolvedLeafResource ResolveLeafResource(const LeafResourceLayers& layers)
{
    Budget budget;
    const auto pinned = ChargeFixedLayers(layers, budget);
    ResolvedLeafResource out;
    std::vector<KeyValue> admitted = AdmitDeclared(layers, pinned, budget, out.attributes_dropped);

    Resource merged =
        Resource::Merge(Resource{OrNone(layers.defaults)}, Resource{std::move(admitted)});
    merged = Resource::Merge(merged, Resource{OrNone(layers.configured)});
    if (!layers.id_key.empty())
    {
        merged = Resource::Merge(merged, Layer(layers.id_key, layers.leaf_id));
    }
    if (!Names(merged, kServiceNameKey))
    {
        merged = Resource::Merge(merged, Layer(kServiceNameKey, kUnknownService));
    }
    out.resource = std::make_shared<const Resource>(std::move(merged));
    return out;
}

}  // namespace microtel::sdk
