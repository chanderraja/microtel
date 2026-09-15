// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/trace.hpp"

#include <type_traits>
#include <utility>

namespace microtel
{

/// @brief The unit of propagation — what crosses a process boundary, a thread
/// hand-off, or an `ISpanProcessor::OnStart` call.
///
/// v1.1 carries the active `SpanContext`. A `Baggage` slot lands beside it in
/// packet 2.3c ([ICP 0025](../../docs/icps/0025-propagation-core.md) §2); the
/// two named, typed slots are the whole of what microtel propagates.
///
/// Copying a `Context` is `noexcept` and allocation-free: `SpanContext`'s
/// only growable member, `TraceState`, holds its entries behind a
/// `shared_ptr`, so a copy is a refcount bump (ICP 0025 §1).
///
/// @threadsafety Thread-safe. A `Context` is a value; distinct copies are
///               independent and nothing mutates through a shared pointer.
///
/// @see docs/threading-model.md §10
class Context
{
public:
    Context() noexcept = default;
    explicit Context(SpanContext active) noexcept : active_span_context(std::move(active)) {}

    /// @brief The span a child started from this context parents to.
    ///
    /// Invalid (all-zero ids) when no span is active.
    SpanContext active_span_context;
};

static_assert(std::is_nothrow_copy_constructible_v<Context>,
              "Context is copied into every ScopedContext — see ICP 0025 §2");

/// @brief The calling thread's current context.
///
/// Never null: a thread that has installed nothing sees a default-constructed
/// `Context` whose `active_span_context` is invalid.
///
/// Deliberately **not** inline — the `thread_local` behind it lives in exactly
/// one translation unit (`src/api/context.cpp`, in `microtel_api`), so a
/// process that links `microtel_api` once has exactly one slot per thread.
///
/// The returned reference is borrowed from that thread-local slot and is
/// invalidated by the next `ScopedContext` construction or destruction on the
/// calling thread; copy it if you need to keep it.
///
/// @threadsafety Thread-safe. Each thread reads and writes only its own slot;
///               there is no cross-thread inheritance (ICP 0025 §3 contract 4).
///               A newly created thread starts from the root context.
[[nodiscard]] const Context& CurrentContext() noexcept;

/// @brief Installs a `Context` as the calling thread's current one and
/// restores the displaced one on destruction.
///
/// The per-thread "stack" **is** the C++ stack: the slot holds a single
/// `Context` by value and each live scope holds the value it displaced. There
/// is no container, no heap, and no depth limit — installing a context costs a
/// refcount bump and a thread-local store.
///
/// The cost of that shape is that **restore is positional: scopes must be
/// destroyed in reverse order of creation on a thread.** Destroying out of
/// order writes back a stale context. That is a programming error, not a
/// diagnosed condition; microtel does not detect it. Ordinary block scope and
/// ordinary member lifetimes give the right order for free.
///
/// Move-constructible so a factory can return one; a moved-from scope is
/// disarmed and restores nothing. Not copyable and not move-assignable —
/// either would let a restore land out of order.
///
/// @threadsafety **Thread-confined.** A `ScopedContext` must be constructed
///               and destroyed on the same thread, and must not be shared
///               between threads.
///
/// @see docs/threading-model.md §10
/// @see docs/icps/0025-propagation-core.md §3
class ScopedContext
{
public:
    /// @brief An inert scope: installs nothing and restores nothing.
    ScopedContext() noexcept = default;

    /// @brief Installs @p ctx for the calling thread until this object dies.
    explicit ScopedContext(Context ctx) noexcept;

    ~ScopedContext() noexcept;

    ScopedContext(ScopedContext&& other) noexcept;
    ScopedContext& operator=(ScopedContext&&) = delete;
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    /// The context this scope displaced; written back on destruction.
    Context m_previous;
    /// False for an inert or moved-from scope — it restores nothing.
    bool m_armed = false;
};

}  // namespace microtel
