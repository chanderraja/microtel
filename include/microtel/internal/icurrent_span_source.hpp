// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/trace.hpp"

namespace microtel::internal
{

/// @brief Seam that lets metric instruments query the currently active span
/// without a hard dependency on the trace SDK context propagator.
///
/// The implementation is provided by the SDK integration layer (v1.1); for
/// v1.2 builds, pass `nullptr` to disable exemplar capture.  Thread-safe:
/// `GetCurrentSpan` must be callable from any thread that records measurements.
///
/// Non-copyable, non-movable (interface type — owned by the integration layer).
class ICurrentSpanSource
{
public:
    ICurrentSpanSource() noexcept = default;
    ICurrentSpanSource(const ICurrentSpanSource&) = delete;
    ICurrentSpanSource& operator=(const ICurrentSpanSource&) = delete;
    ICurrentSpanSource(ICurrentSpanSource&&) = delete;
    ICurrentSpanSource& operator=(ICurrentSpanSource&&) = delete;
    virtual ~ICurrentSpanSource() noexcept = default;

    /// @brief Returns the `SpanContext` of the currently active span on the
    /// calling thread, or an invalid context if no sampled span is active.
    [[nodiscard]] virtual SpanContext GetCurrentSpan() const = 0;
};

}  // namespace microtel::internal
