// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "upb/mem/arena.h"

#include <utility>

/// @file
/// Move-only RAII wrapper for a upb arena.
///
/// **Why this lives in `src/wire/encoder/` and not `src/common/raii/`:**
/// including it is including `upb/mem/arena.h`, and the encoder is the only
/// place in the codebase permitted to see upb headers (CLAUDE.md's upb
/// containment rule, `memory-model.md` §3.1). Putting it under
/// `src/common/raii/` would leak upb into a directory every component
/// includes. `src/wire/encoder/README.md` already said it belongs here;
/// `docs/coding-standards.md` and `docs/architecture.md` list it under
/// `src/common/raii/`, and they are the ones that are wrong.

namespace microtel::wire
{

/// @brief Owns a `upb_Arena*` and frees it on destruction.
///
/// Default-constructed or after move-from: holds `nullptr` and does not free.
///
/// @note This type exists to make the "arena destroyed before `Encode`
///       returns" guarantee (`memory-model.md` §3.1, LOCKED) hold on the
///       *exception* path as well as the return path. `Encode` is documented
///       as able to throw `std::bad_alloc` while allocating the output buffer,
///       and that allocation sits between arena creation and the manual free
///       it replaced — so a throw leaked the arena.
class UpbArena
{
public:
    /// @brief Allocate a new arena. Holds `nullptr` if upb could not.
    UpbArena() noexcept : m_arena(upb_Arena_New()) {}

    ~UpbArena() noexcept
    {
        Free();
    }

    UpbArena(const UpbArena&) = delete;
    UpbArena& operator=(const UpbArena&) = delete;

    UpbArena(UpbArena&& other) noexcept : m_arena(other.m_arena)
    {
        other.m_arena = nullptr;
    }

    UpbArena& operator=(UpbArena&& other) noexcept
    {
        if (this != &other)
        {
            Free();
            m_arena = other.m_arena;
            other.m_arena = nullptr;
        }
        return *this;
    }

    /// @brief Return the raw arena (borrowed; do not free).
    [[nodiscard]] upb_Arena* Get() const noexcept
    {
        return m_arena;
    }

    /// @brief True when this holds an arena.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_arena != nullptr;
    }

    /// @brief Relinquish ownership; the caller becomes responsible for
    ///        `upb_Arena_Free`.
    [[nodiscard]] upb_Arena* Release() noexcept
    {
        return std::exchange(m_arena, nullptr);
    }

private:
    void Free() noexcept
    {
        if (m_arena != nullptr)
        {
            upb_Arena_Free(m_arena);
            m_arena = nullptr;
        }
    }

    upb_Arena* m_arena = nullptr;
};

}  // namespace microtel::wire
