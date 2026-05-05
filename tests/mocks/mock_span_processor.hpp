// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel
{
class Span;
class Context;
}  // namespace microtel

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::ISpanProcessor`.
///
/// Used by Tracer unit tests to assert that `OnStart` / `OnEnd` are
/// invoked at the right points in the lifecycle without engaging real
/// batching machinery.
class MockSpanProcessor : public internal::ISpanProcessor
{
public:
    // --- Configurable returns ---
    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;

    // --- Recording ---
    int on_start_call_count = 0;
    int on_end_call_count = 0;
    int force_flush_call_count = 0;
    int shutdown_call_count = 0;

    // --- ISpanProcessor ---

    void OnStart(microtel::Span& /*span*/, const microtel::Context& /*parent*/) noexcept override
    {
        ++on_start_call_count;
    }

    void OnEnd(internal::SpanRecord&& /*record*/) noexcept override
    {
        ++on_end_call_count;
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
