// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <new>
#include <utility>

#include <zlib.h>

namespace microtel::common::raii
{

/// @brief Move-only RAII wrapper for a zlib inflate stream.
///
/// The decompression sibling of `DeflateStream`. Default-constructed or after
/// move-from the object is uninitialised and the destructor does nothing;
/// `Init` acquires the stream's internal state and the destructor calls
/// `inflateEnd` iff `Init` succeeded, so the zlib allocation is released on
/// every path — a corrupt stream, a cap overflow, an early return from a
/// failed `inflate`.
///
/// **The `z_stream` is heap-allocated, not a by-value member.** zlib's internal
/// state keeps a back-pointer to the `z_stream` it was initialised against, and
/// every later call validates it (`inflateStateCheck`). A by-value member would
/// change address on a move, after which `inflateEnd` returns `Z_STREAM_ERROR`
/// and frees nothing — a 42 KiB leak per moved stream, which is exactly what
/// LeakSanitizer reported against the first version of this header. Holding the
/// `z_stream` behind a `unique_ptr` keeps its address stable, so a move
/// transfers a pointer and zlib never notices.
///
/// `Reset` rather than `Release` is the release verb here, matching
/// `DeflateStream`: there is no handle to hand back to a caller.
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
        : m_stream(std::move(other.m_stream)), m_initialized(other.m_initialized)
    {
        other.m_initialized = false;
    }

    InflateStream& operator=(InflateStream&& other) noexcept
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

    /// @brief Initialise the stream via `inflateInit2`.
    /// @param window_bits window size; add 16 to require the gzip wrapper, or
    ///        32 to auto-detect zlib vs gzip.
    /// @return true on `Z_OK`. Calling twice without an intervening `Reset`
    ///         returns false rather than leaking the first state, and an
    ///         allocation failure is reported the same way rather than thrown.
    [[nodiscard]] bool Init(int window_bits) noexcept
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
        m_initialized = (inflateInit2(m_stream.get(), window_bits) == Z_OK);
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
            static_cast<void>(inflateEnd(m_stream.get()));
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
