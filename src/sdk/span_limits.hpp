// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/sdk_builder.hpp"

#include <cstddef>
#include <cstdint>

namespace microtel::sdk
{

/// @brief Truncate every string in @p value to @p limit bytes, backing up to a
///        UTF-8 code point boundary.
///
/// `attribute_value_length_limit` is a **byte** budget, and an OTLP attribute
/// is a proto3 `string` that must hold valid UTF-8: a cut through a multi-byte
/// sequence would turn one truncated attribute into a whole batch the
/// collector refuses to parse. Backing up loses at most three more bytes.
///
/// Only the two string alternatives of `AttributeValue` can exceed a length
/// budget; the fixed-width ones pass through untouched. Shrinking a
/// `std::string` never allocates, so this is safe in a `noexcept` path.
///
/// @return How many strings lost bytes: 0 or 1 for a scalar, one per clipped
///         element for a string array. Each is one `AttributeValueTruncated`.
[[nodiscard]] std::uint64_t TruncateStrings(AttributeValue& value, std::size_t limit) noexcept;

/// @brief Apply the Provider's `SpanLimitOptions` to a span that was built
///        elsewhere — a decoded leaf span (`docs/leaf-concentrator-design.md`
///        §3.6 step 2) — the way `SdkSpan` applies them as a span is built.
///
/// Surplus attributes, events and links are dropped from the end and counted
/// (`span_attribute_limit`, `span_event_limit`, `span_link_limit`,
/// `event_attribute_limit`, `link_attribute_limit`); long string values are
/// truncated and counted (`attribute_value_truncated`). Only shrinks the
/// record, so it never allocates.
///
/// @param record the span to trim, in place.
/// @param limits the Provider's limits.
/// @param diag   borrowed sink for the counts, or nullptr for none.
void ApplySpanLimits(internal::SpanRecord& record,
                     const SpanLimitOptions& limits,
                     internal::IDiagnosticsSink* diag) noexcept;

}  // namespace microtel::sdk
