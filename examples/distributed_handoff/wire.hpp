// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// wire.hpp — the dumb half of distributed_handoff.
//
// A localhost TCP socket and a text protocol that is HTTP's start-line-plus-
// headers shape and nothing else:
//
//     PLACE /orders/ord-1 CRLF
//     traceparent: 00-<32 hex>-<16 hex>-01 CRLF
//     baggage: tenant=acme CRLF
//     CRLF
//
// Everything here is deliberately boring and lives apart from sender.cpp and
// receiver.cpp so that those two files contain only the telemetry. Nothing in
// this file includes a microtel header: the propagators are carrier-agnostic —
// they speak through `HeaderGetter` / `HeaderSetter` callbacks — so the carrier
// never has to know what is being carried. Swap this for HTTP/2, gRPC
// metadata, a Kafka record, or a JSON envelope and the two `main`s do not
// change.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace handoff
{

/// @brief Loopback port the two halves meet on. Override with `argv[2]`.
constexpr std::uint16_t kDefaultPort{9099};

/// @brief The headers of one message.
///
/// `std::less<>` is the transparent comparator, so `find(std::string_view)`
/// does not build a `std::string` — which is what lets the `HeaderGetter`
/// lambda in receiver.cpp be a one-liner. A real HTTP carrier would also
/// case-fold the names; this one matches them exactly, because both halves of
/// the example write them the way the propagators do (lowercase).
using Headers = std::map<std::string, std::string, std::less<>>;

/// @brief A move-only owner for a file descriptor.
///
/// CLAUDE.md rule 5: every resource lives inside a type whose destructor
/// releases it. The project's own `UniqueFd` lives in `src/common/raii/` and is
/// not public API, so an example that is meant to be copied into a consumer's
/// tree carries its own five lines rather than reaching into `src/`.
class Fd
{
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : m_fd(fd) {}

    ~Fd() noexcept
    {
        Reset();
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}

    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }

    [[nodiscard]] bool IsOpen() const noexcept
    {
        return m_fd >= 0;
    }

    /// @brief Borrowed; this object still owns and still closes it.
    [[nodiscard]] int Get() const noexcept
    {
        return m_fd;
    }

    void Reset() noexcept
    {
        if (m_fd >= 0)
        {
            ::close(m_fd);
            m_fd = -1;
        }
    }

private:
    int m_fd{-1};
};

namespace detail
{

inline ::sockaddr_in LoopbackAddress(std::uint16_t port) noexcept
{
    ::sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    return addr;
}

inline bool WriteAll(const Fd& fd, std::string_view data) noexcept
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const ::ssize_t written =
            ::send(fd.Get(), data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

inline void ParseHeaderLine(std::string_view line, Headers& headers)
{
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos)
    {
        return;
    }
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
    {
        value.remove_prefix(1);
    }
    headers.emplace(std::string{line.substr(0, colon)}, std::string{value});
}

}  // namespace detail

/// @brief Connect to 127.0.0.1:@p port, retrying while nothing is listening.
///
/// The retry exists so "start the receiver, then the sender" is forgiving
/// about how fast you type. Returns a closed `Fd` if the receiver never
/// appears.
[[nodiscard]] inline Fd DialLocalhost(std::uint16_t port)
{
    constexpr int kAttempts{50};
    constexpr std::chrono::milliseconds kRetryDelay{100};

    const ::sockaddr_in addr = detail::LoopbackAddress(port);

    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        Fd sock{::socket(AF_INET, SOCK_STREAM, 0)};
        if (sock.IsOpen() &&
            ::connect(sock.Get(), reinterpret_cast<const ::sockaddr*>(&addr), sizeof(addr)) == 0)
        {
            return sock;
        }
        std::this_thread::sleep_for(kRetryDelay);
    }
    return Fd{};
}

/// @brief Bind @p port, accept exactly one connection, and return it.
///
/// One connection because the example's job is one hand-off: it accepts, does
/// the work, flushes its trace, and exits. A server would loop here.
[[nodiscard]] inline Fd ListenAndAcceptOne(std::uint16_t port)
{
    Fd listener{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!listener.IsOpen())
    {
        return Fd{};
    }

    constexpr int kOn{1};
    if (::setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, &kOn, sizeof(kOn)) != 0)
    {
        std::cerr << "handoff: SO_REUSEADDR failed; a recently used port may refuse to bind\n";
    }

    const ::sockaddr_in addr = detail::LoopbackAddress(port);
    if (::bind(listener.Get(), reinterpret_cast<const ::sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        return Fd{};
    }

    constexpr int kBacklog{1};
    if (::listen(listener.Get(), kBacklog) != 0)
    {
        return Fd{};
    }

    return Fd{::accept(listener.Get(), nullptr, nullptr)};
}

/// @brief Send a start line and headers, terminated by a blank line.
inline bool WriteMessage(const Fd& fd, std::string_view start_line, const Headers& headers)
{
    std::string out{start_line};
    out += "\r\n";
    for (const auto& [name, value] : headers)
    {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "\r\n";
    return detail::WriteAll(fd, out);
}

/// @brief Read one message; fills @p headers and returns the start line.
///
/// Returns `std::nullopt` if the peer closed before a blank line arrived.
[[nodiscard]] inline std::optional<std::string> ReadMessage(const Fd& fd, Headers& headers)
{
    constexpr std::size_t kChunkBytes{1024};
    constexpr std::string_view kTerminator{"\r\n\r\n"};
    constexpr std::string_view kEol{"\r\n"};

    std::string buffer;
    std::array<char, kChunkBytes> chunk{};
    while (buffer.find(kTerminator) == std::string::npos)
    {
        const ::ssize_t got = ::recv(fd.Get(), chunk.data(), chunk.size(), 0);
        if (got <= 0)
        {
            return std::nullopt;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(got));
    }

    std::optional<std::string> start_line;
    std::size_t pos = 0;
    for (std::size_t eol = buffer.find(kEol, pos); eol != std::string::npos;
         eol = buffer.find(kEol, pos))
    {
        const std::string_view line{buffer.data() + pos, eol - pos};
        pos = eol + kEol.size();
        if (line.empty())
        {
            break;
        }
        if (!start_line.has_value())
        {
            start_line = std::string{line};
            continue;
        }
        detail::ParseHeaderLine(line, headers);
    }
    return start_line;
}

}  // namespace handoff
