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
    Tracer() noexcept                    = default;
    virtual ~Tracer() noexcept           = default;

    Tracer(const Tracer&)                = delete;
    Tracer& operator=(const Tracer&)     = delete;
    Tracer(Tracer&&) noexcept            = default;
    Tracer& operator=(Tracer&&) noexcept = default;

    /// @brief Begin a new span.
    ///
    /// On the unsampled path, returns a no-op `Span` and performs no
    /// allocation. The returned handle is always valid; the caller may
    /// always call `SetAttribute`, `AddEvent`, `End`, etc.
    ///
    /// @param name borrowed; copied into the span record on the sampled path.
    /// @param opts initial attributes, parent, kind, start time.
    ///
    /// @return non-null span handle. RAII auto-end fires at scope exit if
    ///         `End()` is not called explicitly.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept Always succeeds.
    [[nodiscard]] virtual SpanHandle
        StartSpan(std::string_view name, const StartSpanOptions& opts = {}) noexcept = 0;

    /// @brief Convenience: start a span and make it the current span in the
    /// thread-local context.
    ///
    /// The returned handle restores the previous current span on destruction.
    /// (To be added in v1.1 once the `Context` machinery is fully fleshed
    /// out; placeholder declaration for M0 surface review.)
    [[nodiscard]] virtual SpanHandle
        StartAsCurrentSpan(std::string_view name, const StartSpanOptions& opts = {}) noexcept = 0;
};

}  // namespace microtel
