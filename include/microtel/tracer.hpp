// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/span.hpp"

#include <string_view>

namespace microtel
{

/// @brief Issues spans for one instrumentation scope.
///
/// Obtained from `Provider::GetTracer(name, version)`. The tracer holds a
/// non-owning back-reference to the `Provider`; the application must not hold
/// a `Tracer` past the `Provider`'s shutdown / destruction.
///
/// `StartSpan` is the hot-path entry point — `noexcept` and never blocks on
/// I/O. On the unsampled path, `StartSpan` returns a no-op `Span` handle and
/// performs **no allocation** (`docs/memory-model.md` §8.1).
///
/// @threadsafety Thread-safe. Concurrent `StartSpan` calls from any number of
///               threads are safe.
/// @noexcept All hot-path methods.
///
/// @see docs/architecture.md §3.1
/// @see docs/threading-model.md §10
class Tracer
{
public:
    Tracer() noexcept = default;
    virtual ~Tracer() noexcept = default;

    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;
    Tracer(Tracer&&) noexcept = default;
    Tracer& operator=(Tracer&&) noexcept = default;

    /// @brief Begin a new span.
    ///
    /// On the unsampled path, returns a no-op `Span` and performs no
    /// allocation. The returned handle is always valid; the caller may
    /// always call `SetAttribute`, `AddEvent`, `End`, etc.
    ///
    /// If `opts.parent` is unset, the parent is the `active_span_context` of
    /// the calling thread's `CurrentContext()`; if it is set and valid, that
    /// context is the parent; if it is set but **invalid**, the span is an
    /// explicit root with a fresh trace id and the current context is not
    /// consulted ([ICP 0025](../../docs/icps/0025-propagation-core.md) §3).
    ///
    /// @param name borrowed; copied into the span record on the sampled path.
    /// @param opts initial attributes, parent, kind, start time.
    ///
    /// @return non-null span handle. RAII auto-end fires at scope exit if
    ///         `End()` is not called explicitly.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept Always succeeds.
    [[nodiscard]] virtual SpanHandle StartSpan(std::string_view name,
                                               const StartSpanOptions& opts = {}) noexcept = 0;

    /// @brief Start a span and make it the calling thread's current span.
    ///
    /// Resolves the parent exactly as `StartSpan` does, and additionally
    /// installs the new span's context for the returned scope's lifetime. The
    /// returned `ScopedSpan` ends the span and restores the previous context
    /// on destruction, in that order.
    ///
    /// The install happens on the unsampled path too: the handle is then the
    /// no-op singleton, but the installed context carries the real trace id
    /// with the sampled flag cleared, so children of an unsampled span stay in
    /// the same trace (ICP 0025 §3 contract 3).
    ///
    /// **The installed context carries the calling thread's baggage.** Baggage
    /// is per-context, not per-span (ICP 0025 §2), so entering a span scope
    /// changes the active span and nothing else: a read of
    /// `CurrentContext().baggage` from inside the scope, or an
    /// `Inject(CurrentContext().baggage, setter)` on an outgoing call, sees
    /// exactly what the caller installed.
    ///
    /// The scope is **thread-confined** — destroy it on the thread that
    /// created it, and in reverse order relative to any other scope on that
    /// thread. There is no cross-thread inheritance: a worker thread starts
    /// from the root context unless the caller hands `CurrentContext()` across
    /// explicitly and installs it with `ScopedContext`.
    ///
    /// @threadsafety Thread-safe to call; the returned scope is thread-confined.
    /// @noexcept Always succeeds.
    [[nodiscard]] virtual ScopedSpan StartAsCurrentSpan(
        std::string_view name, const StartSpanOptions& opts = {}) noexcept = 0;
};

}  // namespace microtel
