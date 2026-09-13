// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <new>
#include <utility>

#include <zlib.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for a zlib deflate stream.
///
/// Default-constructed or after move-from: uninitialised; the destructor does
/// nothing. `Init` acquires the stream's internal state; the destructor calls
/// `deflateEnd` iff `Init` succeeded, so the zlib allocation is released on
/// every path including an early return from a failed `deflate`.
///
/// **The `z_stream` is heap-allocated, not a by-value member.** zlib's internal
/// `deflate_state` keeps a back-pointer to the `z_stream` it was initialised
/// against, and every later call validates it (`deflateStateCheck`). A
/// by-value member would change address on a move, after which `deflateEnd`
/// returns `Z_STREAM_ERROR` and frees nothing — the leak LeakSanitizer
/// measured at 42 KiB per moved stream on the inflate sibling, and issue #186
/// on this one. Holding the `z_stream` behind a `unique_ptr` keeps its address
/// stable, so a move transfers a pointer and zlib never notices.
///
/// Deliberately the same shape as `InflateStream`: the two are meant to be
/// read as a pair.
class DeflateStream
{
public:
    DeflateStream() noexcept = default;

    ~DeflateStream() noexcept
    {
        Reset();
    }

    DeflateStream(const DeflateStream&) = delete;
    DeflateStream& operator=(const DeflateStream&) = delete;

    DeflateStream(DeflateStream&& other) noexcept
        : m_stream(std::move(other.m_stream)), m_initialized(other.m_initialized)
    {
        other.m_initialized = false;
    }

    DeflateStream& operator=(DeflateStream&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_stream = std::move(other.m_stream);
            m_initialized = other.m_initialized;
            other.m_initialized = false;
        }
        return *this;
    }

    /// @brief Initialise the stream via `deflateInit2`.
    /// @param level zlib compression level.
    /// @param window_bits window size; add 16 to select the gzip wrapper.
    /// @param mem_level zlib internal-state memory level.
    /// @return true on `Z_OK`. Calling twice without an intervening `Reset`
    ///         returns false rather than leaking the first state, and an
    ///         allocation failure is reported the same way rather than thrown.
    [[nodiscard]] bool Init(int level, int window_bits, int mem_level) noexcept
    {
        if (m_initialized)
        {
            return false;
        }
        try
        {
            m_stream = std::make_unique<z_stream>();
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        const int rc = deflateInit2(
            m_stream.get(), level, Z_DEFLATED, window_bits, mem_level, Z_DEFAULT_STRATEGY);
        m_initialized = (rc == Z_OK);
        if (!m_initialized)
        {
            m_stream.reset();
        }
        return m_initialized;
    }

    /// @brief Borrowed pointer to the stream, or nullptr when uninitialised.
    /// @note Non-owning; valid until this object is destroyed or moved from.
    [[nodiscard]] z_stream* Get() noexcept
    {
        return m_initialized ? m_stream.get() : nullptr;
    }

    /// @brief Releases the zlib state if held. Idempotent.
    void Reset() noexcept
    {
        if (m_initialized)
        {
            static_cast<void>(deflateEnd(m_stream.get()));
            m_initialized = false;
        }
        m_stream.reset();
    }

private:
    /// Heap-allocated for address stability — see the class comment.
    std::unique_ptr<z_stream> m_stream;
    bool m_initialized{false};
};

}  // namespace microtel::common::raii
