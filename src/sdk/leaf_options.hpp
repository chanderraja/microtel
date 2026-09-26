// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/leaf_receiver.hpp"

namespace microtel::sdk
{

/// @brief Check `LeafReceiverOptions` the way `SdkBuilder::Build()` must
///        before it builds a receiver (`docs/leaf-concentrator-design.md`
///        §4.2–§4.5).
///
/// Refused with `ConfigError::Kind::InvalidValue`, `field` naming the setting
/// by its `[concentrator]` TOML path:
/// - a zero `max_payload_bytes`, `max_spans_per_payload` or `max_leaves`;
/// - a leaf id that is empty or longer than 128 bytes;
/// - a reserved `microtel.leaf.*` key in any configured Resource (§4.2);
/// - the `leaf_id_attribute` key in any configured Resource, which could
///   never take effect (§4.4);
/// - a configured Resource — the defaults, or the defaults merged with one
///   leaf's own — over `max_leaf_resource_bytes` (§4.5).
///
/// Options with `enabled = false` are not checked: nothing is built from them.
[[nodiscard]] Expected<void, ConfigError> ValidateLeafReceiverOptions(
    const LeafReceiverOptions& options);

}  // namespace microtel::sdk
