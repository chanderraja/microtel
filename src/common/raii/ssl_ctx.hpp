// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <openssl/ssl.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for an OpenSSL `SSL_CTX*`.
///
/// Default-constructed or after move-from: holds `nullptr` and does not free.
/// Destructor calls `SSL_CTX_free` iff the pointer is non-null.
///
/// @note Per ICP 0003 §3.1, one `SslCtx` instance is owned by each
///       `Http2Transport`; not process-shared.
class SslCtx
{
public:
    SslCtx() noexcept = default;

    explicit SslCtx(SSL_CTX* ctx) noexcept : m_ctx(ctx) {}

    ~SslCtx() noexcept
    {
        Reset();
    }

    SslCtx(const SslCtx&) = delete;
    SslCtx& operator=(const SslCtx&) = delete;

    SslCtx(SslCtx&& other) noexcept : m_ctx(other.m_ctx)
    {
        other.m_ctx = nullptr;
    }

    SslCtx& operator=(SslCtx&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_ctx = other.m_ctx;
            other.m_ctx = nullptr;
        }
        return *this;
    }

    /// @brief Return the raw pointer (borrowed; do not free).
    [[nodiscard]] SSL_CTX* Get() const noexcept
    {
        return m_ctx;
    }

    /// @brief Return true if this holds a non-null pointer.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_ctx != nullptr;
    }

    /// @brief Relinquish ownership and return the raw pointer.
    [[nodiscard]] SSL_CTX* Release() noexcept
    {
        SSL_CTX* p = m_ctx;
        m_ctx = nullptr;
        return p;
    }

    /// @brief Free the context immediately (no-op if already null).
    void Reset() noexcept
    {
        if (m_ctx != nullptr)
        {
            ::SSL_CTX_free(m_ctx);
            m_ctx = nullptr;
        }
    }

private:
    SSL_CTX* m_ctx = nullptr;
};

}  // namespace microtel::common::raii
