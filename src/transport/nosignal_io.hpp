// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <openssl/bio.h>

#include <cstddef>
#include <memory>

#include <sys/types.h>

namespace microtel::transport
{

/// @brief `write(2)` on a socket that can never raise `SIGPIPE`.
///
/// A library must not be able to terminate its host because a network peer
/// went away, and the default disposition of `SIGPIPE` is exactly that: a
/// collector restart, a GOAWAY followed by a close, or a load balancer
/// draining a backend all end with microtel writing to a socket the peer has
/// finished with. `MSG_NOSIGNAL` turns that into an ordinary `EPIPE`
/// (issue #177).
///
/// microtel installs no signal handler and changes no process-wide signal
/// disposition to achieve this — see `docs/threading-model.md` §7.1.
///
/// @param fd   Borrowed; not closed here.
/// @param data Borrowed; read but not retained.
/// @param len  Bytes to write.
/// @return Bytes written, or -1 with `errno` set. `EINTR` is retried
///         internally; `EAGAIN`/`EWOULDBLOCK` are the caller's to handle, as
///         are `EPIPE` and `ECONNRESET` — the two that say the peer is gone.
[[nodiscard]] ssize_t SendNoSignal(int fd, const void* data, std::size_t len) noexcept;

/// @brief Deleter for a `BIO*` this code still owns.
struct BioDeleter
{
    void operator()(BIO* bio) const noexcept
    {
        ::BIO_free(bio);
    }
};

/// @brief Owning handle for a `BIO*`; release it to transfer ownership.
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

/// @brief Create a BIO that reads and writes @p fd with `SIGPIPE` suppressed.
///
/// OpenSSL's stock socket BIO writes with `write(2)`, so `SSL_write` and the
/// handshake writes inside `SSL_connect` carry the fault `SendNoSignal` fixes
/// for plaintext. This is the same fix one layer down: a `BIO_METHOD` whose
/// write callback is `::send(..., MSG_NOSIGNAL)` and whose read callback is
/// `::recv`.
///
/// The BIO sets its retry flags exactly as OpenSSL's own socket BIO does, so
/// a non-blocking socket still produces `SSL_ERROR_WANT_READ` /
/// `SSL_ERROR_WANT_WRITE` and every caller's existing `WANT_*` handling keeps
/// working unchanged.
///
/// @param fd Borrowed for the lifetime of the BIO: the returned BIO reads and
///           writes it but never closes it. The socket's owner (an
///           `common::raii::UniqueFd` in `Http2Transport`) must outlive it.
/// @return An owning BIO, or `nullptr` if OpenSSL could not allocate one.
[[nodiscard]] BioPtr MakeNoSignalBio(int fd) noexcept;

}  // namespace microtel::transport
