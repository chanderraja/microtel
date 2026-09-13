// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <openssl/bio.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for an OpenSSL `BIO_METHOD*`.
///
/// A `BIO_METHOD` is the method table of a BIO type — allocated by
/// `BIO_meth_new`, populated with `BIO_meth_set_*`, and released by
/// `BIO_meth_free`. It is not a BIO: objects created from it with `BIO_new`
/// are owned separately (microtel's are handed to an `SSL`, which frees them).
///
/// Default-constructed or after move-from: holds `nullptr` and does not free.
/// Destructor calls `BIO_meth_free` iff the pointer is non-null.
///
/// @see src/transport/nosignal_io.hpp — microtel's only use, the SIGPIPE-safe
///      socket BIO (issue #177).
class BioMethod
{
public:
    BioMethod() noexcept = default;

    explicit BioMethod(BIO_METHOD* method) noexcept : m_method(method) {}

    ~BioMethod() noexcept
    {
        Reset();
    }

    BioMethod(const BioMethod&) = delete;
    BioMethod& operator=(const BioMethod&) = delete;

    BioMethod(BioMethod&& other) noexcept : m_method(other.m_method)
    {
        other.m_method = nullptr;
    }

    BioMethod& operator=(BioMethod&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_method = other.m_method;
            other.m_method = nullptr;
        }
        return *this;
    }

    /// @brief Return the raw pointer (borrowed; do not free).
    [[nodiscard]] BIO_METHOD* Get() const noexcept
    {
        return m_method;
    }

    /// @brief Return true if this holds a non-null pointer.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_method != nullptr;
    }

    /// @brief Relinquish ownership and return the raw pointer.
    [[nodiscard]] BIO_METHOD* Release() noexcept
    {
        BIO_METHOD* p = m_method;
        m_method = nullptr;
        return p;
    }

    /// @brief Free the method table immediately (no-op if already null).
    void Reset() noexcept
    {
        if (m_method != nullptr)
        {
            ::BIO_meth_free(m_method);
            m_method = nullptr;
        }
    }

private:
    BIO_METHOD* m_method = nullptr;
};

}  // namespace microtel::common::raii
