// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/leaf_receiver.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

/// @file
/// The value syntax of the concentrator's settings, shared by the
/// `[concentrator]` TOML table and the `MICROTEL_CONCENTRATOR_*` variables
/// (`docs/leaf-concentrator-design.md` §4.2–§4.3, `docs/configuration.md`
/// §3.14), and the per-key merge of `SdkBuilder::WithLeafReceiver` over them.

namespace microtel::config
{

/// @brief The concentrator's settings before any source has spoken: the
///        `LeafReceiverOptions` defaults with `enabled = false`, the TOML and
///        environment default (§4.2). Inline, because every `Config` is built
///        with it, including in tests that link no more of the config layer
///        than the validator.
[[nodiscard]] inline LeafReceiverOptions DefaultConcentratorOptions()
{
    LeafReceiverOptions options;
    options.enabled = false;
    return options;
}

/// @brief Parse a byte size: decimal digits with an optional `B`, `KiB` or
///        `MiB` suffix (`"65536"`, `"64KiB"`, `"1MiB"`).
/// @return the size, or nullopt if @p text is not one or is over `UINT32_MAX`.
[[nodiscard]] std::optional<std::uint32_t> ParseByteSize(std::string_view text) noexcept;

/// @brief Parse a duration: decimal digits with an `s`, `m` or `h` suffix
///        (`"30s"`, `"5m"`, `"1h"`). A bare number is refused, so a value can
///        never be read in the wrong unit.
/// @return the duration, or nullopt if @p text is not one.
[[nodiscard]] std::optional<std::chrono::seconds> ParseDuration(std::string_view text) noexcept;

/// @brief Parse `unknown_leaf`: `"accept"` or `"reject"`.
[[nodiscard]] std::optional<UnknownLeafPolicy> ParseUnknownLeafPolicy(
    std::string_view text) noexcept;

/// @brief Parse one time mode: `"concentrator_stamped"`, `"sync_relative"` or
///        `"boot_relative"`.
[[nodiscard]] std::optional<LeafTimeMode> ParseLeafTimeMode(std::string_view text) noexcept;

/// @brief Parse `default_time_mode`: `"auto"` (an empty inner optional) or one
///        of the modes `ParseLeafTimeMode` accepts.
/// @return nullopt if @p text is neither.
[[nodiscard]] std::optional<std::optional<LeafTimeMode>> ParseDefaultTimeMode(
    std::string_view text) noexcept;

/// @brief Merge the options passed to `SdkBuilder::WithLeafReceiver` over the
///        file-and-environment ones (§4.3, `docs/configuration.md` §1).
///
/// Code is the highest source. Every scalar and the resolver come from
/// @p code, since a `LeafReceiverOptions` value cannot say which of its
/// fields were set on purpose (as with `WithBatch`). The tables merge per
/// key: `leaf_defaults_resource` with `MergeResourceAttrs`; `leaves` per leaf
/// id, and within one leaf its `resource` per key and its `time_mode` when
/// @p code sets one.
///
/// @param base the resolved file-and-environment options; updated in place.
/// @param code the options from `WithLeafReceiver`.
void MergeLeafReceiverOptions(LeafReceiverOptions& base, const LeafReceiverOptions& code);

}  // namespace microtel::config
