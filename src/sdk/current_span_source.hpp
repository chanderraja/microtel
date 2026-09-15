// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/context.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/trace.hpp"

namespace microtel::sdk
{

/// @brief `ICurrentSpanSource` backed by the API's thread-local context slot.
///
/// The seam metrics exemplars and log trace-correlation have always been
/// declared against; `CurrentContext()` is the thing that finally answers it
/// ([ICP 0025](../../docs/icps/0025-propagation-core.md) §3, "The exemplar
/// payoff"). Owned by `SdkProvider`, borrowed by every `SdkMeter` and
/// `SdkLogger` it creates.
///
/// Stateless, so `GetCurrentSpan` costs a thread-local read and two flag
/// checks — no lock, no allocation.
///
/// @threadsafety Thread-safe. Each caller reads only its own thread's slot.
class CurrentSpanSource final : public internal::ICurrentSpanSource
{
public:
    CurrentSpanSource() noexcept = default;

    /// @brief The calling thread's current span, or an invalid `SpanContext`.
    ///
    /// Returns the invalid context unless a span is both **valid and
    /// sampled** — which is exactly the `trace_based` exemplar filter of
    /// `docs/metrics-design.md` §7, and exactly what
    /// `internal::ICurrentSpanSource` documents.
    [[nodiscard]] SpanContext GetCurrentSpan() const override
    {
        const SpanContext& ctx = CurrentContext().active_span_context;
        if (ctx.IsValid() && ctx.trace_flags.IsSampled())
        {
            return ctx;
        }
        return SpanContext{};
    }
};

}  // namespace microtel::sdk
