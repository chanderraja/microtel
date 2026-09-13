// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "nosignal_io.hpp"

#include "common/raii/bio_method.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <sys/socket.h>
#include <sys/types.h>

namespace microtel::transport
{

namespace
{

/// The BIO carries the descriptor in its data pointer rather than in a heap
/// cell: there is nothing to allocate, nothing to free, and nothing for the
/// destroy callback to get wrong.
///
/// @param bio Borrowed.
/// @return The socket this BIO writes, borrowed from whoever opened it.
int BioFd(BIO* bio) noexcept
{
    return static_cast<int>(reinterpret_cast<std::intptr_t>(::BIO_get_data(bio)));
}

/// `bwrite_ex`: one `send` with `MSG_NOSIGNAL`.
///
/// On `EAGAIN` the retry-write flag is what turns the failure into
/// `SSL_ERROR_WANT_WRITE` for the caller; without it OpenSSL would report a
/// fatal I/O error and every non-blocking write would look like a dead peer.
/// Every other failure clears the flags, so `SSL_get_error` reports the fault
/// it really is.
int NoSignalBioWrite(BIO* bio, const char* data, std::size_t len, std::size_t* written) noexcept
{
    ::BIO_clear_retry_flags(bio);
    const ssize_t n = SendNoSignal(BioFd(bio), data, len);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            ::BIO_set_retry_write(bio);
        }
        return 0;
    }
    *written = static_cast<std::size_t>(n);
    return 1;
}

/// `bread_ex`: one `recv`. The read side cannot raise `SIGPIPE`; it is here
/// because a BIO must answer both halves, and because the retry-read flag is
/// what keeps `SSL_ERROR_WANT_READ` working on a non-blocking socket.
///
/// A return of 0 bytes is the peer's orderly close and is reported as failure
/// with no retry flag, which is how OpenSSL's own socket BIO reports EOF.
int NoSignalBioRead(BIO* bio, char* data, std::size_t len, std::size_t* readbytes) noexcept
{
    ::BIO_clear_retry_flags(bio);
    const int fd = BioFd(bio);
    ssize_t n = ::recv(fd, data, len, 0);
    while (n < 0 && errno == EINTR)
    {
        n = ::recv(fd, data, len, 0);
    }
    if (n <= 0)
    {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            ::BIO_set_retry_read(bio);
        }
        return 0;
    }
    *readbytes = static_cast<std::size_t>(n);
    return 1;
}

/// This BIO is unbuffered, so a flush has nothing to do — but it must still
/// report success, because OpenSSL treats a failed flush as a fatal error.
/// Everything else (EOF, pending, duplicate, close-flag) is genuinely
/// unsupported and says so.
long NoSignalBioCtrl(BIO* /*bio*/, int cmd, long /*larg*/, void* /*parg*/) noexcept
{
    return (cmd == BIO_CTRL_FLUSH) ? 1 : 0;
}

/// The BIO is usable the moment it exists; `MakeNoSignalBio` stores the
/// descriptor immediately afterwards and nothing can reach it in between.
int NoSignalBioCreate(BIO* bio) noexcept
{
    ::BIO_set_init(bio, 1);
    ::BIO_set_data(bio, nullptr);
    return 1;
}

/// Freeing the BIO must not close the socket: the descriptor is borrowed and
/// its owner (`Http2Transport::m_socket`, a `UniqueFd`) closes it.
int NoSignalBioDestroy(BIO* bio) noexcept
{
    ::BIO_set_data(bio, nullptr);
    ::BIO_set_init(bio, 0);
    return 1;
}

/// Build the method table. Callers get it through `NoSignalBioMethod`.
common::raii::BioMethod MakeBioMethod()
{
    // NOLINTNEXTLINE(hicpp-signed-bitwise) — BIO type ids are signed constants
    const int type = ::BIO_get_new_index() | BIO_TYPE_SOURCE_SINK;
    common::raii::BioMethod method{::BIO_meth_new(type, "microtel sigpipe-safe socket")};
    if (!method.IsValid())
    {
        return method;
    }
    ::BIO_meth_set_write_ex(method.Get(), NoSignalBioWrite);
    ::BIO_meth_set_read_ex(method.Get(), NoSignalBioRead);
    ::BIO_meth_set_ctrl(method.Get(), NoSignalBioCtrl);
    ::BIO_meth_set_create(method.Get(), NoSignalBioCreate);
    ::BIO_meth_set_destroy(method.Get(), NoSignalBioDestroy);
    return method;
}

/// @return Borrowed; owned by the function-local static below, and valid for
///         the rest of the process. `nullptr` if OpenSSL could not allocate.
///
/// One table per process rather than one per transport: a `BIO_METHOD` is a
/// vtable, fully built before the first caller can observe it and never
/// written to afterwards, so sharing it across transports and threads is safe
/// — and C++ already guarantees the initialisation runs exactly once.
const BIO_METHOD* NoSignalBioMethod() noexcept
{
    static const common::raii::BioMethod kMethod = MakeBioMethod();
    return kMethod.Get();
}

}  // namespace

ssize_t SendNoSignal(int fd, const void* data, std::size_t len) noexcept
{
    ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR)
    {
        n = ::send(fd, data, len, MSG_NOSIGNAL);
    }
    return n;
}

BioPtr MakeNoSignalBio(int fd) noexcept
{
    const BIO_METHOD* const method = NoSignalBioMethod();
    if (method == nullptr)
    {
        return {};
    }
    BioPtr bio{::BIO_new(method)};
    if (bio)
    {
        // The only cell OpenSSL offers a custom BIO is a void*, and a
        // descriptor is what has to go in it; the alternative is heap-
        // allocating an int for the BIO to own, which is more code and one
        // more thing for the destroy callback to get wrong.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        ::BIO_set_data(bio.get(), reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
    }
    return bio;
}

}  // namespace microtel::transport
