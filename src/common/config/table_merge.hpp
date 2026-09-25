// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include <vector>

namespace microtel::config
{

/// @brief Merge a higher-precedence `[resource]` table into a lower one, per key.
///
/// Each key of a table-valued setting is its own setting (`docs/configuration.md`
/// §1, issue #257). A key in `overriding` replaces that key in `base`, in
/// place; every key `overriding` does not name survives. Keys compare exactly:
/// OTel attribute keys are case-sensitive. This is `Resource::Merge`, so the
/// config layers resolve with the same rule as the §12.7 detector layers.
///
/// @param base the lower-precedence table; updated in place.
/// @param overriding the higher-precedence table.
void MergeResourceAttrs(std::vector<KeyValue>& base, const std::vector<KeyValue>& overriding);

/// @brief Merge a higher-precedence `[exporter.headers]` table into a lower one, per key.
///
/// As `MergeResourceAttrs`, except that header names compare ASCII
/// case-insensitively (RFC 9110 §5.1): `authorization` overrides
/// `Authorization`. The overriding entry's spelling and value replace the
/// first matching entry in `base`, and any further case variants of the same
/// name in `base` are removed, so one header is never sent twice.
///
/// @param base the lower-precedence table; updated in place.
/// @param overriding the higher-precedence table.
void MergeHeaders(std::vector<KeyValue>& base, const std::vector<KeyValue>& overriding);

}  // namespace microtel::config
