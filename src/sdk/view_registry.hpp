// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/view.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace microtel::sdk
{

/// @brief Identifies an instrument at registration time for view matching.
///
/// All fields are non-owning views; callers must ensure the referenced
/// strings outlive the `Match()` call.
struct InstrumentDescriptor
{
    std::string_view name;
    InstrumentKind kind = InstrumentKind::Counter;
    std::string_view meter_name;
};

/// @brief Ordered registry of `ViewConfig` entries.
///
/// `Add()` appends a view; `Match()` returns const pointers to all entries
/// whose selector matches `desc`, in registration order. An empty result
/// means "no view configured — use the default stream."
///
/// @threadsafety Not thread-safe. Populate at SDK-build time (single thread)
/// before any reader threads start.
class ViewRegistry
{
public:
    ViewRegistry() = default;

    /// @brief Append @p view. Registration order is preserved in `Match()` results.
    void Add(ViewConfig view);

    /// @brief Return const pointers to all views whose selector matches @p desc.
    [[nodiscard]] std::vector<const ViewConfig*> Match(const InstrumentDescriptor& desc) const;

    /// @brief True when no views have been registered.
    [[nodiscard]] bool Empty() const noexcept { return m_views.empty(); }

private:
    [[nodiscard]] static bool MatchesName(const std::optional<std::string>& pattern,
                                          std::string_view name) noexcept;

    std::vector<ViewConfig> m_views;
};

}  // namespace microtel::sdk
