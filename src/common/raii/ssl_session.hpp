// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <openssl/ssl.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for an OpenSSL `SSL*` (TLS session).
///
/// Default-constructed or after move-from: holds `nullptr` and does not free.
/// Destructor calls `SSL_free` iff the pointer is non-null.
class SslSession
{
public:
    SslSession() noexcept = default;

    explicit SslSession(SSL* ssl) noexcept : m_ssl(ssl) {}

    ~SslSession() noexcept
    {
        Reset();
    }

    SslSession(const SslSession&) = delete;
    SslSession& operator=(const SslSession&) = delete;

    SslSession(SslSession&& other) noexcept : m_ssl(other.m_ssl)
    {
        other.m_ssl = nullptr;
    }

    SslSession& operator=(SslSession&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_ssl = other.m_ssl;
            other.m_ssl = nullptr;
        }
        return *this;
    }

    /// @brief Return the raw pointer (borrowed; do not free).
    [[nodiscard]] SSL* Get() const noexcept
    {
        return m_ssl;
    }

    /// @brief Return true if this holds a non-null pointer.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_ssl != nullptr;
    }

    /// @brief Relinquish ownership and return the raw pointer.
    [[nodiscard]] SSL* Release() noexcept
    {
        SSL* p = m_ssl;
        m_ssl = nullptr;
        return p;
    }

    /// @brief Free the session immediately (no-op if already null).
    void Reset() noexcept
    {
        if (m_ssl != nullptr)
        {
            ::SSL_free(m_ssl);
            m_ssl = nullptr;
        }
    }

private:
    SSL* m_ssl = nullptr;
};

}  // namespace microtel::common::raii
