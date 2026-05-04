// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace microtel
{

/// @brief OpenTelemetry attribute value — one of the OTLP-supported scalar
/// types or a homogeneous array thereof.
///
/// Per the OTel spec the supported types are: bool, int64, double, string,
/// plus arrays of each. `std::variant` keeps the type discrimination explicit
/// and copyable; the array variants are owned vectors.
using AttributeValue = std::variant<
    bool,
    std::int64_t,
    double,
    std::string,
    std::vector<bool>,
    std::vector<std::int64_t>,
    std::vector<double>,
    std::vector<std::string>>;

/// @brief Single attribute key-value pair.
///
/// `key` is a borrowed view by convention on hot paths (the API copies into the
/// span record only if the span is sampled). `value` is owned.
struct KeyValue
{
    std::string    key;
    AttributeValue value;
};

/// @brief Borrowed view over a sequence of attributes.
///
/// Used on the hot path where the caller passes a collection without
/// transferring ownership. The implementation copies per-attribute into the
/// span record only on the sampled path.
using AttributeSpan = std::span<const KeyValue>;

}  // namespace microtel
