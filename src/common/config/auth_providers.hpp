// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/sdk_builder.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace microtel::config
{

/// @brief `IAuthProvider` that always returns the same static Authorization value.
///
/// Zero allocation per call. An empty `token` maps to `nullopt` — no header sent.
///
/// @threadsafety Thread-safe.
class StaticHeadersAuthProvider final : public internal::IAuthProvider
{
public:
    explicit StaticHeadersAuthProvider(std::string token) noexcept;

    ~StaticHeadersAuthProvider() noexcept override = default;
    StaticHeadersAuthProvider(const StaticHeadersAuthProvider&) = delete;
    StaticHeadersAuthProvider& operator=(const StaticHeadersAuthProvider&) = delete;
    StaticHeadersAuthProvider(StaticHeadersAuthProvider&&) = delete;
    StaticHeadersAuthProvider& operator=(StaticHeadersAuthProvider&&) = delete;

    [[nodiscard]] microtel::Expected<std::optional<std::string>, microtel::Error> GetAuthorization(
        internal::TimePointSteady now) override;

private:
    std::string m_token;
};

/// @brief `IAuthProvider` that calls a user callback with a configurable TTL cache.
///
/// On cache miss (or first call, or after TTL expiry), the callback is invoked.
/// On error the error is returned and the cache is NOT updated; the next call
/// retries the callback.
///
/// The user-supplied callback may be invoked on the exporter worker thread;
/// it must be thread-safe. It may throw: the throw is caught here and
/// converted to `Error::Kind::InternalFailure`, which drops the batch being
/// built and nothing else (`docs/interfaces.md` §4.9).
///
/// @threadsafety Thread-safe.
class CallbackAuthProvider final : public internal::IAuthProvider
{
public:
    CallbackAuthProvider(AuthCallback cb, std::chrono::milliseconds cache_ttl) noexcept;

    ~CallbackAuthProvider() noexcept override = default;
    CallbackAuthProvider(const CallbackAuthProvider&) = delete;
    CallbackAuthProvider& operator=(const CallbackAuthProvider&) = delete;
    CallbackAuthProvider(CallbackAuthProvider&&) = delete;
    CallbackAuthProvider& operator=(CallbackAuthProvider&&) = delete;

    [[nodiscard]] microtel::Expected<std::optional<std::string>, microtel::Error> GetAuthorization(
        internal::TimePointSteady now) override;

private:
    /// @brief Invokes the user callback, converting anything it throws to an
    /// `Error::Kind::InternalFailure` (LOCKED — `docs/interfaces.md` §4.9).
    /// Nothing thrown by user code leaves this function.
    [[nodiscard]] microtel::Expected<std::string, microtel::Error> InvokeCallback() const;

    AuthCallback m_cb;
    std::chrono::milliseconds m_ttl;

    mutable std::mutex m_mu;
    std::optional<std::string> m_cached;
    internal::TimePointSteady m_last_refresh{};
    bool m_has_cached{false};
};

}  // namespace microtel::config
