// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <nghttp2/nghttp2.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for an `nghttp2_session*`.
///
/// Default-constructed or after move-from: holds `nullptr` and does not free.
/// Destructor calls `nghttp2_session_del` iff the pointer is non-null.
class Nghttp2Session
{
public:
    Nghttp2Session() noexcept = default;

    explicit Nghttp2Session(nghttp2_session* session) noexcept : m_session(session) {}

    ~Nghttp2Session() noexcept
    {
        Reset();
    }

    Nghttp2Session(const Nghttp2Session&) = delete;
    Nghttp2Session& operator=(const Nghttp2Session&) = delete;

    Nghttp2Session(Nghttp2Session&& other) noexcept : m_session(other.m_session)
    {
        other.m_session = nullptr;
    }

    Nghttp2Session& operator=(Nghttp2Session&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_session = other.m_session;
            other.m_session = nullptr;
        }
        return *this;
    }

    /// @brief Return the raw pointer (borrowed; do not free).
    [[nodiscard]] nghttp2_session* Get() const noexcept
    {
        return m_session;
    }

    /// @brief Return true if this holds a non-null pointer.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_session != nullptr;
    }

    /// @brief Relinquish ownership and return the raw pointer.
    [[nodiscard]] nghttp2_session* Release() noexcept
    {
        nghttp2_session* p = m_session;
        m_session = nullptr;
        return p;
    }

    /// @brief Destroy the session immediately (no-op if already null).
    void Reset() noexcept
    {
        if (m_session != nullptr)
        {
            ::nghttp2_session_del(m_session);
            m_session = nullptr;
        }
    }

private:
    nghttp2_session* m_session = nullptr;
};

}  // namespace microtel::common::raii
