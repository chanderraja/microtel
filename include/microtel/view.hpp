// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace microtel
{

/// @brief Identifies the semantic kind of a metric instrument.
///
/// Used in `InstrumentSelector` to filter views by instrument type.
enum class InstrumentKind : std::uint8_t
{
    Counter,
    UpDownCounter,
    Gauge,
    Histogram,
    ExponentialHistogram,
    ObservableCounter,
    ObservableUpDownCounter,
    ObservableGauge,
};

/// @brief Describes which instruments a `ViewConfig` applies to.
///
/// All populated fields must match simultaneously. An empty selector (all
/// fields nullopt) matches any instrument. `name` supports exact match or
/// a single trailing `*` wildcard (e.g. `"http.*"` matches any name
/// starting with `"http."`).
struct InstrumentSelector
{
    std::optional<std::string> name;
    std::optional<InstrumentKind> kind;
    std::optional<std::string> meter_name;
};

/// @brief Transforms applied to the streams matched by an `InstrumentSelector`.
///
/// `name` renames the output metric (nullopt = keep the instrument name).
/// `attribute_allowlist` limits which attribute keys are recorded (nullopt =
/// pass all attributes through). `drop = true` suppresses the instrument
/// entirely — no data is collected or exported.
struct ViewTransform
{
    std::optional<std::string> name;
    std::optional<std::vector<std::string>> attribute_allowlist;
    bool drop = false;
};

/// @brief Pairs a selector with a transform to configure one metric view.
///
/// Register via `SdkBuilder::WithView()`. Views are evaluated in registration
/// order; all matching views produce independent streams (fan-out).
struct ViewConfig
{
    InstrumentSelector selector;
    ViewTransform transform;
};

}  // namespace microtel
