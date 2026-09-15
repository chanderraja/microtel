// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/auth_providers.hpp"

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/clock.hpp"

#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace microtel::config
{

namespace
{
/// Prefix on every message built from a callback that threw, so an operator
/// reading `last_error_message` sees the callback named and not just its text.
constexpr std::string_view kThrewPrefix = "auth callback threw: ";
}  // namespace

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

microtel::Expected<std::string, microtel::Error> CallbackAuthProvider::InvokeCallback() const
{
    // The boundary `interfaces.md` §4.9 promises: the user callback may throw,
    // and the throw becomes an `InternalFailure` here rather than unwinding
    // through the wire codec into the exporter worker — which catches
    // `std::exception` around a whole drain and would lose every batch in it,
    // not the one whose header was being built (issue #251).
    try
    {
        return m_cb();
    }
    catch (const std::exception& e)
    {
        return microtel::make_unexpected(
            microtel::Error{.kind = microtel::Error::Kind::InternalFailure,
                            .message = std::string{kThrewPrefix} + e.what(),
                            .os_errno = 0});
    }
    // Not belt-and-braces: the exporter worker's handler is
    // `catch (const std::exception&)` and `WorkerLoop` is `noexcept`, so a
    // non-std throw that got that far would be std::terminate rather than one
    // dropped batch.
    catch (...)
    {
        return microtel::make_unexpected(
            microtel::Error{.kind = microtel::Error::Kind::InternalFailure,
                            .message = std::string{kThrewPrefix} + "non-std exception",
                            .os_errno = 0});
    }
}

microtel::Expected<std::optional<std::string>, microtel::Error>
CallbackAuthProvider::GetAuthorization(internal::TimePointSteady now)
{
    const std::scoped_lock lock{m_mu};

    if (m_has_cached && (now - m_last_refresh) < m_ttl)
    {
        return std::optional<std::string>{m_cached};
    }

    auto result = InvokeCallback();
    if (!result)
    {
        return microtel::make_unexpected(result.error());
    }

    m_cached = std::move(*result);
    m_last_refresh = now;
    m_has_cached = true;

    return std::optional<std::string>{m_cached};
}

}  // namespace microtel::config
