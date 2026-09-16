// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/context.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace microtel
{
class Span;
}  // namespace microtel

namespace microtel::testing
{

/// @brief Fake `ISpanProcessor` that records every received `SpanRecord`.
///
/// Distinct from `MockSpanProcessor` (which only counts calls): tests can
/// inspect the actual records — names, attributes, status — to verify
/// that spans flowed correctly through the SDK without engaging real
/// batching machinery.
class FakeSpanProcessor : public internal::ISpanProcessor
{
public:
    std::vector<internal::SpanRecord> received_spans;

    /// Scope of the tracer each span arrived with, index-aligned with
    /// @ref received_spans — both are appended by the same `OnEnd` call.
    std::vector<internal::InstrumentationScope> received_scopes;

    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;

    /// The `Context` each `OnStart` arrived with, in call order. Kept by value
    /// because a `Context` copy is `noexcept` and allocation-free — its two
    /// growable members are both `shared_ptr` refcounts (ICP 0025 §§1-2) — so
    /// recording one does not change what the SDK under test does.
    std::vector<microtel::Context> started_contexts;

    int on_start_call_count = 0;
    int force_flush_call_count = 0;
    int shutdown_call_count = 0;

    void OnStart(microtel::Span& /*span*/, const microtel::Context& parent) noexcept override
    {
        ++on_start_call_count;
        started_contexts.push_back(parent);
    }

    void OnEnd(internal::SpanRecord&& record,
               const internal::InstrumentationScope& scope) noexcept override
    {
        received_spans.push_back(std::move(record));
        received_scopes.push_back(scope);
    }

    [[nodiscard]] microtel::Status ForceFlush(
        std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++force_flush_call_count;
        return force_flush_result;
    }

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++shutdown_call_count;
        return shutdown_result;
    }
};

}  // namespace microtel::testing
