// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/reactor.hpp"

#include "common/raii/unique_fd.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>

namespace microtel::transport
{

/// @brief epoll(7) + eventfd(2) implementation of `IReactor`.
///
/// One epoll instance and one eventfd (for `Wake()`).  All methods except
/// `Wake()` must be called from the I/O thread. (LOCKED — interfaces.md §4.8.)
///
/// @threadsafety `Wake()` is thread-safe. All other methods are I/O-thread-only.
class EpollReactor final : public internal::IReactor
{
public:
    /// @brief Factory — returns an error if epoll_create1(2) or eventfd(2) fails.
    [[nodiscard]] static microtel::Expected<std::unique_ptr<EpollReactor>, microtel::Error>
    Create() noexcept;

    ~EpollReactor() noexcept override = default;

    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;
    EpollReactor(EpollReactor&&) = delete;
    EpollReactor& operator=(EpollReactor&&) = delete;

    [[nodiscard]] microtel::Expected<void, microtel::Error> Register(
        int fd, internal::EventMask mask, internal::EventCallback cb) override;

    void Modify(int fd, internal::EventMask mask) override;

    void Unregister(int fd) noexcept override;

    std::size_t WaitAndDispatch(internal::TimePointSteady deadline) override;

    void Wake() noexcept override;

private:
    EpollReactor(common::raii::UniqueFd epoll_fd, common::raii::UniqueFd wake_fd) noexcept;

    static constexpr int kMaxEvents = 64;

    common::raii::UniqueFd m_epoll_fd;
    common::raii::UniqueFd m_wake_fd;
    std::unordered_map<int, internal::EventCallback> m_callbacks;
};

}  // namespace microtel::transport
