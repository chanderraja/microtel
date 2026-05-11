// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/auth_providers.hpp"

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/clock.hpp"

#include <mutex>
#include <optional>
#include <string>

namespace microtel::config
{

// ---------------------------------------------------------------------------
// StaticHeadersAuthProvider
// ---------------------------------------------------------------------------

StaticHeadersAuthProvider::StaticHeadersAuthProvider(std::string token) noexcept
    : m_token{std::move(token)}
{
}

microtel::Expected<std::optional<std::string>, microtel::Error>
StaticHeadersAuthProvider::GetAuthorization(internal::TimePointSteady /*now*/)
{
    if (m_token.empty())
    {
        return std::optional<std::string>{std::nullopt};
    }
    return std::optional<std::string>{m_token};
}

// ---------------------------------------------------------------------------
// CallbackAuthProvider
// ---------------------------------------------------------------------------

CallbackAuthProvider::CallbackAuthProvider(AuthCallback cb,
                                           std::chrono::milliseconds cache_ttl) noexcept
    : m_cb{std::move(cb)}, m_ttl{cache_ttl}
{
}

microtel::Expected<std::optional<std::string>, microtel::Error>
CallbackAuthProvider::GetAuthorization(internal::TimePointSteady now)
{
    const std::scoped_lock lock{m_mu};

    if (m_has_cached && (now - m_last_refresh) < m_ttl)
    {
        return std::optional<std::string>{m_cached};
    }

    auto result = m_cb();
    if (!result)
    {
        return microtel::Unexpected{result.error()};
    }

    m_cached = std::move(*result);
    m_last_refresh = now;
    m_has_cached = true;

    return std::optional<std::string>{m_cached};
}

}  // namespace microtel::config
