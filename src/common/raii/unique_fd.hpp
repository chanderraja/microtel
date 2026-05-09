// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <utility>

#include <unistd.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for a POSIX file descriptor.
///
/// Default-constructed or after move-from: holds `kInvalid` (-1) and
/// does not close. Destructor calls `::close` iff the fd is valid.
///
/// @note Wraps any fd returned by open(2), pipe(2), epoll_create1(2),
///       eventfd(2), socket(2), etc.
class UniqueFd
{
public:
    static constexpr int kInvalid = -1;

    UniqueFd() noexcept = default;

    explicit UniqueFd(int fd) noexcept : m_fd(fd) {}

    ~UniqueFd() noexcept
    {
        Close();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : m_fd(other.m_fd)
    {
        other.m_fd = kInvalid;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_fd = other.m_fd;
            other.m_fd = kInvalid;
        }
        return *this;
    }

    /// @brief Return the raw fd (borrowed reference; do not close).
    [[nodiscard]] int Get() const noexcept
    {
        return m_fd;
    }

    /// @brief Return true if this holds a valid fd.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_fd != kInvalid;
    }

    /// @brief Relinquish ownership and return the raw fd.
    [[nodiscard]] int Release() noexcept
    {
        const int fd = m_fd;
        m_fd = kInvalid;
        return fd;
    }

    /// @brief Close immediately (no-op if already invalid).
    void Close() noexcept
    {
        if (m_fd != kInvalid)
        {
            ::close(m_fd);
            m_fd = kInvalid;
        }
    }

private:
    int m_fd = kInvalid;
};

}  // namespace microtel::common::raii
