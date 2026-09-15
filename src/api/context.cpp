// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "microtel/context.hpp"

#include <utility>

namespace microtel
{

namespace
{

/// The one current-context slot per thread.
///
/// Kept in this translation unit alone — `CurrentContext()` is deliberately
/// not inline — so a process that links `microtel_api` once cannot end up with
/// two slots per thread (ICP 0025 §3). A `Context` value: no thread, fd, or
/// lock, which is what makes `threading-model.md` §7 (fork) indifferent to it.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
thread_local Context tl_current;

}  // namespace

const Context& CurrentContext() noexcept
{
    return tl_current;
}

ScopedContext::ScopedContext(Context ctx) noexcept
    : m_previous(std::move(tl_current)), m_armed(true)
{
    tl_current = std::move(ctx);
}

ScopedContext::ScopedContext(ScopedContext&& other) noexcept
    : m_previous(std::move(other.m_previous)), m_armed(std::exchange(other.m_armed, false))
{
}

ScopedContext::~ScopedContext() noexcept
{
    if (m_armed)
    {
        tl_current = std::move(m_previous);
    }
}

}  // namespace microtel
