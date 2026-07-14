// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/view_registry.hpp"

#include <string_view>
#include <utility>

namespace microtel::sdk
{

void ViewRegistry::Add(ViewConfig view)
{
    m_views.push_back(std::move(view));
}

bool ViewRegistry::MatchesName(const std::optional<std::string>& pattern,
                               std::string_view name) noexcept
{
    if (!pattern.has_value())
    {
        return true;
    }

    const std::string_view p{*pattern};

    if (p.ends_with('*'))
    {
        return name.starts_with(p.substr(0, p.size() - 1));
    }

    return name == p;
}

std::vector<const ViewConfig*> ViewRegistry::Match(const InstrumentDescriptor& desc) const
{
    std::vector<const ViewConfig*> result;

    for (const auto& view : m_views)
    {
        const auto& sel = view.selector;

        if (!MatchesName(sel.name, desc.name))
        {
            continue;
        }
        if (sel.kind.has_value() && *sel.kind != desc.kind)
        {
            continue;
        }
        if (sel.meter_name.has_value() && *sel.meter_name != desc.meter_name)
        {
            continue;
        }

        result.push_back(&view);
    }

    return result;
}

}  // namespace microtel::sdk
