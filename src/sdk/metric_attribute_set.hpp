// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace microtel::sdk
{

/// @brief Owned, order-insensitive attribute set — the key the metric
/// aggregation map is keyed on (M12, `docs/metrics-design.md` §9).
///
/// Built from a borrowed `AttributeSpan` by copying the pairs, sorting them by
/// key (so equality is order-insensitive), and precomputing a hash that is
/// commutative across pairs. `HashOf` / `MatchesSpan` let the aggregation map
/// probe with a borrowed span without first materialising an `AttributeSet`.
///
/// Rule of zero: value-semantic members; copy/move/destroy are implicit.
///
/// @threadsafety Const methods are thread-safe; an instance is otherwise
///               externally synchronised (it lives under the per-instrument
///               lock in the aggregation store).
class AttributeSet
{
public:
    AttributeSet() noexcept = default;

    /// @brief Copy + normalise a borrowed attribute span into an owned key.
    explicit AttributeSet(AttributeSpan attrs);

    [[nodiscard]] std::size_t Hash() const noexcept
    {
        return m_hash;
    }

    [[nodiscard]] std::span<const KeyValue> Pairs() const noexcept
    {
        return {m_pairs.data(), m_pairs.size()};
    }

    /// @note Not `noexcept`: string/variant comparison is not formally
    ///       no-throw (it does not throw in practice).
    [[nodiscard]] bool operator==(const AttributeSet& other) const;

    /// @brief Hash of a borrowed span, equal to `AttributeSet(attrs).Hash()`
    /// for the same (unordered) attributes — enables heterogeneous lookup.
    [[nodiscard]] static std::size_t HashOf(AttributeSpan attrs);

    /// @brief True if this set holds exactly the attributes in `attrs`
    /// (order-insensitive). Used to confirm a hash-bucket hit during lookup.
    [[nodiscard]] bool MatchesSpan(AttributeSpan attrs) const;

private:
    std::vector<KeyValue> m_pairs;  ///< sorted by key
    std::size_t m_hash = 0;
};

/// @brief Hash functor for unordered containers keyed on `AttributeSet`.
struct AttributeSetHash
{
    [[nodiscard]] std::size_t operator()(const AttributeSet& set) const noexcept
    {
        return set.Hash();
    }
};

/// @brief Default maximum number of distinct attribute sets per instrument,
/// matching the OpenTelemetry SDK recommendation.
constexpr std::size_t kDefaultMaxCardinality = 2000;

/// @brief Returns the stable "overflow series" key used when an instrument's
/// cardinality limit is exceeded (OTel attribute: `otel.metric.overflow=true`).
/// @note The returned reference is valid for the lifetime of the process.
[[nodiscard]] const AttributeSet& OverflowAttributeSet();

}  // namespace microtel::sdk
