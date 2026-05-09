// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "epoll_reactor.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/reactor.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>

#include <sys/epoll.h>
#include <sys/eventfd.h>

namespace microtel::transport
{

namespace
{

// Map IReactor EventMask bits to epoll event bits.
std::uint32_t ToEpollEvents(internal::EventMask mask) noexcept
{
    std::uint32_t ev = 0;
    if (HasEvent(mask, internal::EventMask::Read))
    {
        ev |= EPOLLIN;
    }
    if (HasEvent(mask, internal::EventMask::Write))
    {
        ev |= EPOLLOUT;
    }
    if (HasEvent(mask, internal::EventMask::Error))
    {
        ev |= EPOLLERR | EPOLLHUP;
    }
    return ev;
}

// Map epoll event bits back to IReactor EventMask.
internal::EventMask FromEpollEvents(std::uint32_t ev) noexcept
{
    internal::EventMask mask = internal::EventMask::None;
    if ((ev & EPOLLIN) != 0U)
    {
        mask = mask | internal::EventMask::Read;
    }
    if ((ev & EPOLLOUT) != 0U)
    {
        mask = mask | internal::EventMask::Write;
    }
    if ((ev & (EPOLLERR | EPOLLHUP)) != 0U)
    {
        mask = mask | internal::EventMask::Error;
    }
    return mask;
}

microtel::Error OsError(const char* msg) noexcept
{
    return {.kind = microtel::Error::Kind::Network, .message = msg, .os_errno = errno};
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

EpollReactor::EpollReactor(common::raii::UniqueFd epoll_fd, common::raii::UniqueFd wake_fd) noexcept
    : m_epoll_fd(std::move(epoll_fd)), m_wake_fd(std::move(wake_fd))
{
}

// static
microtel::Expected<std::unique_ptr<EpollReactor>, microtel::Error> EpollReactor::Create() noexcept
{
    common::raii::UniqueFd epoll_fd{::epoll_create1(EPOLL_CLOEXEC)};
    if (!epoll_fd.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{OsError("epoll_create1 failed")};
    }

    common::raii::UniqueFd wake_fd{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    if (!wake_fd.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{OsError("eventfd failed")};
    }

    // Register the wakefd with the epoll instance for read events.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wake_fd.Get();
    if (::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_ADD, wake_fd.Get(), &ev) != 0)
    {
        return microtel::Unexpected<microtel::Error>{OsError("epoll_ctl ADD wakefd failed")};
    }

    try
    {
        // Private constructor; make_unique can't reach it.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return std::unique_ptr<EpollReactor>(
            new EpollReactor(std::move(epoll_fd), std::move(wake_fd)));
    }
    catch (const std::bad_alloc&)
    {
        return microtel::Unexpected<microtel::Error>{
            microtel::Error{.kind = microtel::Error::Kind::InternalFailure, .message = "OOM"}};
    }
}

// ---------------------------------------------------------------------------
// IReactor implementation
// ---------------------------------------------------------------------------

microtel::Expected<void, microtel::Error> EpollReactor::Register(int fd,
                                                                 internal::EventMask mask,
                                                                 internal::EventCallback cb)
{
    epoll_event ev{};
    ev.events = ToEpollEvents(mask);
    ev.data.fd = fd;

    if (::epoll_ctl(m_epoll_fd.Get(), EPOLL_CTL_ADD, fd, &ev) != 0)
    {
        return microtel::Unexpected<microtel::Error>{OsError("epoll_ctl ADD failed")};
    }

    {
        const std::scoped_lock lk{m_mu};
        m_callbacks.emplace(fd, std::move(cb));
    }
    return {};
}

void EpollReactor::Modify(int fd, internal::EventMask mask)
{
    epoll_event ev{};
    ev.events = ToEpollEvents(mask);
    ev.data.fd = fd;
    ::epoll_ctl(m_epoll_fd.Get(), EPOLL_CTL_MOD, fd, &ev);
}

void EpollReactor::Unregister(int fd) noexcept
{
    ::epoll_ctl(m_epoll_fd.Get(), EPOLL_CTL_DEL, fd, nullptr);
    const std::scoped_lock lk{m_mu};
    m_callbacks.erase(fd);
}

bool EpollReactor::DispatchOneEvent(int fd, std::uint32_t epoll_events) noexcept
{
    if (fd == m_wake_fd.Get())
    {
        // Drain the eventfd counter so the next Wait doesn't immediately return.
        std::uint64_t dummy = 0;
        [[maybe_unused]] const ssize_t n = ::read(m_wake_fd.Get(), &dummy, sizeof(dummy));
        return false;
    }

    internal::EventCallback cb;
    {
        const std::scoped_lock lk{m_mu};
        const auto it = m_callbacks.find(fd);
        if (it == m_callbacks.end())
        {
            return false;
        }
        cb = it->second;
    }
    cb(fd, FromEpollEvents(epoll_events));
    return true;
}

std::size_t EpollReactor::WaitAndDispatch(internal::TimePointSteady deadline)
{
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const int timeout_ms = remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays) — required by epoll_wait API
    epoll_event events[kMaxEvents];
    const int nfds = ::epoll_wait(m_epoll_fd.Get(), events, kMaxEvents, timeout_ms);
    if (nfds <= 0)
    {
        return 0;
    }

    std::size_t dispatched = 0;
    const std::span<const epoll_event> ready{events, static_cast<std::size_t>(nfds)};
    for (const auto& ev : ready)
    {
        if (DispatchOneEvent(ev.data.fd, ev.events))
        {
            ++dispatched;
        }
    }
    return dispatched;
}

void EpollReactor::Wake() noexcept
{
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(m_wake_fd.Get(), &one, sizeof(one));
}

}  // namespace microtel::transport
