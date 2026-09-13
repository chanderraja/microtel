// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit test: the transport's SIGPIPE-safe socket I/O (issue #177).
//
// A socketpair is the whole apparatus. It gives deterministic control of the
// three conditions that matter and that a loopback TCP test can only race for:
// a peer that has gone, a send buffer that is full, and a read with nothing to
// read. Every assertion about a signal is made with SIGPIPE still at its
// default disposition, which is checked rather than assumed — a test process
// that had quietly ignored the signal would prove nothing at all.

#include "transport/nosignal_io.hpp"

#include "common/raii/unique_fd.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mtt = microtel::transport;

namespace
{

constexpr std::string_view kHello{"hello"};
/// Small enough that a handful of writes fills it, large enough that the
/// kernel's own minimum does not swallow the request entirely.
constexpr int kTinySendBuffer = 4096;
/// Bound on the fill loop: enough to overrun any plausible buffer, finite so a
/// kernel that never blocks fails the test instead of hanging it.
constexpr int kFillAttempts = 4096;

/// A connected pair of non-blocking sockets, both ends RAII-owned.
struct SocketPair
{
    microtel::common::raii::UniqueFd local;
    microtel::common::raii::UniqueFd peer;
};

[[nodiscard]] SocketPair MakeSocketPair()
{
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    SocketPair pair{.local = microtel::common::raii::UniqueFd{std::get<0>(fds)},
                    .peer = microtel::common::raii::UniqueFd{std::get<1>(fds)}};
    for (const int fd : fds)
    {
        const int flags = ::fcntl(fd, F_GETFL);
        // NOLINTNEXTLINE(hicpp-signed-bitwise)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return pair;
}

/// Shrink the send buffer so the fill loops below terminate quickly.
void ShrinkSendBuffer(int fd)
{
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &kTinySendBuffer, sizeof(kTinySendBuffer));
}

/// @return true if SIGPIPE would still terminate this process.
[[nodiscard]] bool SigPipeIsFatal()
{
    struct sigaction current
    {
    };
    EXPECT_EQ(::sigaction(SIGPIPE, nullptr, &current), 0);
    return current.sa_handler == SIG_DFL;
}

/// Write until the socket refuses more, so the next write is a guaranteed
/// EAGAIN. @return true if the buffer was filled within the attempt bound.
[[nodiscard]] bool FillSendBuffer(int fd)
{
    const std::array<char, 1024> block{};
    for (int i = 0; i < kFillAttempts; ++i)
    {
        if (mtt::SendNoSignal(fd, block.data(), block.size()) < 0)
        {
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// SendNoSignal
// ---------------------------------------------------------------------------

TEST(NoSignalIoTest, SendNoSignalDeliversToThePeer)
{
    const SocketPair pair = MakeSocketPair();

    ASSERT_EQ(mtt::SendNoSignal(pair.local.Get(), kHello.data(), kHello.size()),
              static_cast<ssize_t>(kHello.size()));

    std::array<char, 16> buf{};
    ASSERT_EQ(::read(pair.peer.Get(), buf.data(), buf.size()), static_cast<ssize_t>(kHello.size()));
    EXPECT_EQ(std::string_view(buf.data(), kHello.size()), kHello);
}

TEST(NoSignalIoTest, SendNoSignalReportsWouldBlockOnAFullBuffer)
{
    const SocketPair pair = MakeSocketPair();
    ShrinkSendBuffer(pair.local.Get());

    ASSERT_TRUE(FillSendBuffer(pair.local.Get())) << "the send buffer never filled";
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK) << "errno was " << errno;
}

// The regression test for issue #177. Linux answers the first write on a dead
// connection with EPIPE and raises SIGPIPE with it; the second and every later
// one does the same. With the default disposition that terminates the process,
// which is why a library must never write without MSG_NOSIGNAL.
TEST(NoSignalIoTest, SendNoSignalReportsEpipeInsteadOfKillingTheProcess)
{
    SocketPair pair = MakeSocketPair();
    pair.peer.Close();

    ASSERT_TRUE(SigPipeIsFatal()) << "the disposition was changed; this test would prove nothing";

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const ssize_t written = mtt::SendNoSignal(pair.local.Get(), kHello.data(), kHello.size());
        EXPECT_EQ(written, -1) << "attempt " << attempt;
        EXPECT_EQ(errno, EPIPE) << "attempt " << attempt;
    }

    // Reaching this line is the assertion: an unsuppressed SIGPIPE would have
    // ended the process inside the loop.
    EXPECT_TRUE(SigPipeIsFatal()) << "nothing may quietly ignore SIGPIPE on the host's behalf";
}

// ---------------------------------------------------------------------------
// The custom BIO
// ---------------------------------------------------------------------------

TEST(NoSignalBioTest, WritesAndReadsThroughTheSocket)
{
    const SocketPair pair = MakeSocketPair();
    const mtt::BioPtr bio = mtt::MakeNoSignalBio(pair.local.Get());
    ASSERT_TRUE(bio);

    ASSERT_EQ(::BIO_write(bio.get(), kHello.data(), static_cast<int>(kHello.size())),
              static_cast<int>(kHello.size()));
    std::array<char, 16> seen{};
    ASSERT_EQ(::read(pair.peer.Get(), seen.data(), seen.size()),
              static_cast<ssize_t>(kHello.size()));
    EXPECT_EQ(std::string_view(seen.data(), kHello.size()), kHello);

    ASSERT_EQ(::write(pair.peer.Get(), kHello.data(), kHello.size()),
              static_cast<ssize_t>(kHello.size()));
    std::array<char, 16> read_back{};
    EXPECT_EQ(::BIO_read(bio.get(), read_back.data(), static_cast<int>(read_back.size())),
              static_cast<int>(kHello.size()));
    EXPECT_EQ(std::string_view(read_back.data(), kHello.size()), kHello);
}

// The retry flags are the contract with OpenSSL: without them a full buffer
// reads as a fatal I/O error instead of SSL_ERROR_WANT_WRITE, and every
// non-blocking write on a busy connection would tear the session down.
TEST(NoSignalBioTest, SetsRetryWriteWhenTheBufferIsFull)
{
    const SocketPair pair = MakeSocketPair();
    ShrinkSendBuffer(pair.local.Get());
    const mtt::BioPtr bio = mtt::MakeNoSignalBio(pair.local.Get());
    ASSERT_TRUE(bio);

    ASSERT_TRUE(FillSendBuffer(pair.local.Get())) << "the send buffer never filled";

    const std::array<char, 1024> block{};
    EXPECT_LE(::BIO_write(bio.get(), block.data(), static_cast<int>(block.size())), 0);
    EXPECT_TRUE(::BIO_should_retry(bio.get()));
    EXPECT_TRUE(::BIO_should_write(bio.get()));
}

TEST(NoSignalBioTest, SetsRetryReadWhenNothingIsAvailable)
{
    const SocketPair pair = MakeSocketPair();
    const mtt::BioPtr bio = mtt::MakeNoSignalBio(pair.local.Get());
    ASSERT_TRUE(bio);

    std::array<char, 16> buf{};
    EXPECT_LE(::BIO_read(bio.get(), buf.data(), static_cast<int>(buf.size())), 0);
    EXPECT_TRUE(::BIO_should_retry(bio.get()));
    EXPECT_TRUE(::BIO_should_read(bio.get()));
}

// The TLS half of the regression test: OpenSSL's stock socket BIO writes with
// write(2), so this is the path SSL_write and SSL_connect take.
TEST(NoSignalBioTest, FailsWithoutRetryWhenThePeerHasGone)
{
    SocketPair pair = MakeSocketPair();
    const mtt::BioPtr bio = mtt::MakeNoSignalBio(pair.local.Get());
    ASSERT_TRUE(bio);
    pair.peer.Close();

    ASSERT_TRUE(SigPipeIsFatal()) << "the disposition was changed; this test would prove nothing";

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        EXPECT_LE(::BIO_write(bio.get(), kHello.data(), static_cast<int>(kHello.size())), 0)
            << "attempt " << attempt;
        EXPECT_FALSE(::BIO_should_retry(bio.get()))
            << "a dead peer is not a retryable condition; attempt " << attempt;
    }
}

TEST(NoSignalBioTest, ReportsPeerEofWithoutRetry)
{
    const SocketPair pair = MakeSocketPair();
    const mtt::BioPtr bio = mtt::MakeNoSignalBio(pair.local.Get());
    ASSERT_TRUE(bio);
    ASSERT_EQ(::shutdown(pair.peer.Get(), SHUT_WR), 0);

    std::array<char, 16> buf{};
    EXPECT_LE(::BIO_read(bio.get(), buf.data(), static_cast<int>(buf.size())), 0);
    EXPECT_FALSE(::BIO_should_retry(bio.get())) << "an orderly close is not a retryable condition";
}

// OpenSSL flushes before it expects bytes on the wire and treats a failed
// flush as fatal; this BIO is unbuffered, so the flush is a no-op that must
// still succeed. Anything else is genuinely unsupported and says so.
TEST(NoSignalBioTest, FlushSucceedsAndUnsupportedControlsDoNot)
{
    const SocketPair pair = MakeSocketPair();
    const mtt::BioPtr bio = mtt::MakeNoSignalBio(pair.local.Get());
    ASSERT_TRUE(bio);

    EXPECT_EQ(::BIO_ctrl(bio.get(), BIO_CTRL_FLUSH, 0, nullptr), 1);
    EXPECT_EQ(::BIO_ctrl(bio.get(), BIO_CTRL_PENDING, 0, nullptr), 0);
    EXPECT_EQ(::BIO_ctrl(bio.get(), BIO_CTRL_EOF, 0, nullptr), 0);
}

// The descriptor is borrowed: Http2Transport's UniqueFd owns the socket, and
// SSL_free frees the BIO whenever a connection is torn down or retried. A
// destroy callback that closed the fd would close a descriptor its owner still
// holds — and, once the number is reused, somebody else's.
TEST(NoSignalBioTest, FreeingTheBioLeavesTheSocketOpen)
{
    const SocketPair pair = MakeSocketPair();
    {
        const mtt::BioPtr bio = mtt::MakeNoSignalBio(pair.local.Get());
        ASSERT_TRUE(bio);
    }

    EXPECT_EQ(mtt::SendNoSignal(pair.local.Get(), kHello.data(), kHello.size()),
              static_cast<ssize_t>(kHello.size()))
        << "the socket must still be open after the BIO is freed";
}
