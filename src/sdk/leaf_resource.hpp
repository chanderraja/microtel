// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/resource.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/// @file
/// The pure parts of the leaf receiver: reading the reserved wire attributes
/// (`docs/leaf-concentrator-design.md` §3.8), the payload checks of §3.4 that
/// need them, and the Resource merge of §4.4 with the budget of §4.5.

namespace microtel::sdk
{

/// The prefix of every reserved wire attribute (§3.8). Keys under it are read
/// and removed before a leaf's Resource is resolved.
inline constexpr std::string_view kLeafReservedPrefix = "microtel.leaf.";

/// The one wire-contract version this concentrator accepts (§3.4).
inline constexpr std::int64_t kLeafProtoVersion = 1;

/// Longest leaf id accepted, in bytes (§3.7, `max_leaf_id_bytes`).
inline constexpr std::size_t kMaxLeafIdBytes = 128;

/// @brief The reserved attributes of one ResourceSpans (§3.8).
struct LeafWireInfo
{
    std::optional<std::int64_t> proto;
    std::optional<std::int64_t> time_mode;
    std::optional<std::int64_t> encode_time;
    std::optional<std::int64_t> sync_age;
    std::optional<std::int64_t> boot_id;
    std::optional<std::int64_t> dropped_spans;
    std::optional<std::int64_t> dropped_items;
    /// A reserved key this concentrator knows carried a non-integer value.
    bool wrong_type = false;
};

/// @brief Read the reserved attributes out of a declared Resource.
[[nodiscard]] LeafWireInfo ReadWireInfo(const std::vector<KeyValue>& resource) noexcept;

/// @brief The time mode a payload declares, if its reserved attributes pass
///        §3.4: a supported `microtel.leaf.proto`, a `time_mode` in range, and
///        the attributes §5 requires for that mode.
/// @return nullopt when the payload is malformed.
[[nodiscard]] std::optional<LeafTimeMode> CheckWireInfo(const LeafWireInfo& info) noexcept;

/// @brief Whether a leaf whose config says @p configured may send a payload in
///        @p declared mode (§5.1). Unset is `auto`: every mode is allowed. A
///        set mode allows itself and `ConcentratorStamped`, which every mode
///        degrades to.
[[nodiscard]] bool TimeModeAllowed(LeafTimeMode declared,
                                   std::optional<LeafTimeMode> configured) noexcept;

/// @brief A hash of the leaf-declared Resource, reserved keys excluded, so a
///        changed declaration is noticed and resolved again (§4.5). The
///        reserved keys change with every payload (`encode_time`) and must
///        not count.
[[nodiscard]] std::uint64_t HashDeclaredResource(const std::vector<KeyValue>& resource) noexcept;

/// @brief What one attribute costs against `max_leaf_resource_bytes`: its key
///        plus its value, a string by its bytes, a numeric or boolean value by
///        its width, an array by the sum of its elements (§4.5).
[[nodiscard]] std::size_t AttributeBytes(const KeyValue& kv) noexcept;

/// @brief The layers of a leaf's Resource, lowest precedence first (§4.4).
struct LeafResourceLayers
{
    /// 1. `leaf_defaults.resource`.
    const std::vector<KeyValue>* defaults = nullptr;
    /// 2. The leaf's own Resource from the payload; reserved keys are skipped.
    const std::vector<KeyValue>* declared = nullptr;
    /// 3. The leaf's configured `resource`, or nullptr for an unconfigured leaf.
    const std::vector<KeyValue>* configured = nullptr;
    /// 4. `leaf_id_attribute`; empty to write no id.
    std::string_view id_key;
    std::string_view leaf_id;
    /// `max_leaf_resource_bytes`.
    std::size_t budget = 0;
};

/// @brief A resolved leaf Resource and what the budget cost.
struct ResolvedLeafResource
{
    std::shared_ptr<const Resource> resource;
    /// Leaf-declared attributes dropped to stay within the budget.
    std::uint64_t attributes_dropped = 0;
};

/// @brief Merge the layers into one Resource (§4.4), with `service.name` set
///        to `unknown_service` if no layer names it.
///
/// Leaf-declared attributes are admitted in payload order while the merged
/// Resource stays within the budget; one that would push it over is dropped
/// and counted (§4.5). Declared keys that a higher layer overrides cost
/// nothing and are never dropped. The configured layers are the operator's own
/// and were checked against the budget at `Build()`.
///
/// @throws std::bad_alloc if the Resource cannot be built.
[[nodiscard]] ResolvedLeafResource ResolveLeafResource(const LeafResourceLayers& layers);

}  // namespace microtel::sdk
