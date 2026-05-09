// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for EpollReactor. Uses real POSIX pipe(2) and eventfd(2) fds to
// trigger events without any mocking — the reactor is an OS abstraction and
// must be tested against the OS.

#include "transport/epoll_reactor.hpp"

#include "microtel/internal/reactor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <unistd.h>

namespace mti = microtel::internal;
namespace mtt = microtel::transport;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::pair<int, int> MakePipe()
{
    int fds[2] = {-1, -1};
    EXPECT_EQ(::pipe(fds), 0);
    return {fds[0], fds[1]};
}

static mti::TimePointSteady Deadline(std::chrono::milliseconds ms)
{
    return std::chrono::steady_clock::now() + ms;
}

// Wrapper: assert Register succeeds (most tests treat failure as fatal).
static void MustRegister(mtt::EpollReactor& reactor,
                         int fd,
                         mti::EventMask mask,
                         mti::EventCallback cb)
{
    auto r = reactor.Register(fd, mask, std::move(cb));
    ASSERT_TRUE(r.has_value()) << r.error().message;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EpollReactorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto result = mtt::EpollReactor::Create();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        m_reactor = std::move(*result);
    }

    std::unique_ptr<mtt::EpollReactor> m_reactor;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(EpollReactorTest, Create_Succeeds)
{
    EXPECT_NE(m_reactor, nullptr);
}

TEST_F(EpollReactorTest, Register_And_Dispatch_ReadEvent)
{
    auto [read_fd, write_fd] = MakePipe();

    bool fired = false;
    auto result = m_reactor->Register(read_fd,
                                      mti::EventMask::Read,
                                      [&](int /*fd*/, mti::EventMask events)
                                      {
                                          if (mti::HasEvent(events, mti::EventMask::Read))
                                          {
                                              fired = true;
                                          }
                                      });
    ASSERT_TRUE(result.has_value());

    const char byte = 'x';
    ASSERT_EQ(::write(write_fd, &byte, 1), 1);

    const std::size_t dispatched = m_reactor->WaitAndDispatch(Deadline(std::chrono::seconds(1)));
    EXPECT_GE(dispatched, 1U);
    EXPECT_TRUE(fired);

    m_reactor->Unregister(read_fd);
    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(EpollReactorTest, WaitAndDispatch_Returns_Zero_When_No_Events_Before_Deadline)
{
    auto [read_fd, write_fd] = MakePipe();

    MustRegister(*m_reactor, read_fd, mti::EventMask::Read, [](int, mti::EventMask) {});

    const auto before = std::chrono::steady_clock::now();
    const std::size_t count = m_reactor->WaitAndDispatch(Deadline(std::chrono::milliseconds(50)));
    const auto elapsed = std::chrono::steady_clock::now() - before;

    EXPECT_EQ(count, 0U);
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));

    m_reactor->Unregister(read_fd);
    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(EpollReactorTest, Wake_Interrupts_WaitAndDispatch)
{
    std::atomic<bool> dispatched{false};
    std::atomic<std::size_t> return_count{0};

    std::thread io_thread(
        [&]()
        {
            return_count.store(m_reactor->WaitAndDispatch(Deadline(std::chrono::seconds(10))));
            dispatched.store(true);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    m_reactor->Wake();
    io_thread.join();

    EXPECT_TRUE(dispatched.load());
    EXPECT_EQ(return_count.load(), 0U);
}

TEST_F(EpollReactorTest, Unregister_Removes_Fd)
{
    auto [read_fd, write_fd] = MakePipe();

    bool fired = false;
    MustRegister(
        *m_reactor, read_fd, mti::EventMask::Read, [&](int, mti::EventMask) { fired = true; });

    m_reactor->Unregister(read_fd);

    const char byte = 'y';
    ASSERT_EQ(::write(write_fd, &byte, 1), 1);

    m_reactor->WaitAndDispatch(Deadline(std::chrono::milliseconds(30)));
    EXPECT_FALSE(fired);

    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(EpollReactorTest, Modify_Updates_EventMask)
{
    auto [read_fd, write_fd] = MakePipe();

    bool read_fired = false;
    MustRegister(*m_reactor,
                 read_fd,
                 mti::EventMask::Write,
                 [&](int, mti::EventMask events)
                 {
                     if (mti::HasEvent(events, mti::EventMask::Read))
                     {
                         read_fired = true;
                     }
                 });

    const char byte = 'z';
    ASSERT_EQ(::write(write_fd, &byte, 1), 1);

    m_reactor->Modify(read_fd, mti::EventMask::Read);

    const std::size_t count = m_reactor->WaitAndDispatch(Deadline(std::chrono::seconds(1)));
    EXPECT_GE(count, 1U);
    EXPECT_TRUE(read_fired);

    m_reactor->Unregister(read_fd);
    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(EpollReactorTest, MultipleRegistrations_Dispatch_Independently)
{
    auto [r0, w0] = MakePipe();
    auto [r1, w1] = MakePipe();

    int fire_count = 0;
    MustRegister(*m_reactor, r0, mti::EventMask::Read, [&](int, mti::EventMask) { ++fire_count; });
    MustRegister(*m_reactor, r1, mti::EventMask::Read, [&](int, mti::EventMask) { ++fire_count; });

    const char byte = 'a';
    ASSERT_EQ(::write(w0, &byte, 1), 1);
    ASSERT_EQ(::write(w1, &byte, 1), 1);

    m_reactor->WaitAndDispatch(Deadline(std::chrono::seconds(1)));
    EXPECT_EQ(fire_count, 2);

    m_reactor->Unregister(r0);
    m_reactor->Unregister(r1);
    ::close(r0);
    ::close(w0);
    ::close(r1);
    ::close(w1);
}
