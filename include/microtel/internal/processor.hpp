// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel
{
class Span;
class Context;
}  // namespace microtel

namespace microtel::internal
{

/// @brief Consumes completed spans and routes them onward.
///
/// v1 ships `BatchSpanProcessor` (queue + worker + batch) and
/// `SimpleSpanProcessor` (synchronous, for tests).
///
/// `OnStart` and `OnEnd` are callable from any caller thread; thread-safe;
/// `noexcept`. `OnEnd` is called exactly once per `Span`, on the caller thread
/// that ended the span. The `scope` identifies the instrumentation scope of the
/// `Tracer` that started the span — the `name` / `version` pair passed to
/// `Provider::GetTracer`. It is borrowed for the duration of the call; a
/// processor that outlives the call copies it. The processor groups records by
/// `(Resource, InstrumentationScope)` when it forms a `BatchHandle`
/// (ICP 0023).
///
/// @threadsafety Thread-safe.
/// @noexcept All methods.
/// @see docs/interfaces.md §4.6
class ISpanProcessor
{
public:
    virtual ~ISpanProcessor() noexcept = default;

    virtual void OnStart(microtel::Span& span, const microtel::Context& parent) noexcept = 0;

    virtual void OnEnd(SpanRecord&& record, const InstrumentationScope& scope) noexcept = 0;

    [[nodiscard]] virtual microtel::Status ForceFlush(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
