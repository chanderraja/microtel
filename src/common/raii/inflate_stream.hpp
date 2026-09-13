// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <utility>

#include <zlib.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for a zlib inflate stream.
///
/// The decompression sibling of `DeflateStream`, with the same shape: default
/// -constructed or after move-from the object is uninitialised and the
/// destructor does nothing; `Init` acquires the stream's internal state and
/// the destructor calls `inflateEnd` iff `Init` succeeded. Response
/// decompression has more early-return paths than compression does — a
/// corrupt stream, a cap overflow — and every one of them releases the zlib
/// allocation without the caller writing anything.
///
/// `Reset` rather than `Release` is the release verb here, matching
/// `DeflateStream`: the `z_stream` is an embedded member, so there is no
/// handle to hand back to a caller.
class InflateStream
{
public:
    InflateStream() noexcept = default;

    ~InflateStream() noexcept
    {
        Reset();
    }

    InflateStream(const InflateStream&) = delete;
    InflateStream& operator=(const InflateStream&) = delete;

    InflateStream(InflateStream&& other) noexcept
        : m_stream(other.m_stream), m_initialized(other.m_initialized)
    {
        other.m_stream = z_stream{};
        other.m_initialized = false;
    }

    InflateStream& operator=(InflateStream&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_stream = other.m_stream;
            m_initialized = other.m_initialized;
            other.m_stream = z_stream{};
            other.m_initialized = false;
        }
        return *this;
    }

    /// @brief Initialise the stream via `inflateInit2`.
    /// @param window_bits window size; add 16 to require the gzip wrapper, or
    ///        32 to auto-detect zlib vs gzip.
    /// @return true on `Z_OK`. Calling twice without an intervening `Reset`
    ///         returns false rather than leaking the first state.
    [[nodiscard]] bool Init(int window_bits) noexcept
    {
        if (m_initialized)
        {
            return false;
        }
        m_stream = z_stream{};
        m_initialized = (inflateInit2(&m_stream, window_bits) == Z_OK);
        return m_initialized;
    }

    /// @brief Borrowed pointer to the stream, or nullptr when uninitialised.
    /// @note Non-owning; valid until this object is destroyed or moved from.
    [[nodiscard]] z_stream* Get() noexcept
    {
        return m_initialized ? &m_stream : nullptr;
    }

    /// @brief Releases the zlib state if held. Idempotent.
    void Reset() noexcept
    {
        if (m_initialized)
        {
            static_cast<void>(inflateEnd(&m_stream));
            m_initialized = false;
        }
    }

private:
    z_stream m_stream{};
    bool m_initialized{false};
};

}  // namespace microtel::common::raii
