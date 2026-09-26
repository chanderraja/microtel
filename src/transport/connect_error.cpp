// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "transport/connect_error.hpp"

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

namespace microtel::transport
{

int MergeConnectErrno(int previous, int current) noexcept
{
    if (current == ECONNREFUSED && previous != 0)
    {
        return previous;
    }
    return current;
}

microtel::Error ConnectFailureError(int err)
{
    // std::generic_category().message() rather than strerror(): the latter is
    // not thread-safe, and Connect can run on any application thread.
    const std::string text = std::generic_category().message(err);
    std::string message;
    if (err == 0 || err == ECONNREFUSED)
    {
        message = "connection refused";
    }
    else if (err == ECONNRESET || err == EPIPE)
    {
        message = "peer closed the connection during TCP connect: " + text;
    }
    else
    {
        message = "connect failed: " + text;
    }
    return microtel::Error{
        .kind = microtel::Error::Kind::Network, .message = std::move(message), .os_errno = err};
}

}  // namespace microtel::transport
