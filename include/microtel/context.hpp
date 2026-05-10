// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/trace.hpp"

namespace microtel
{

/// @brief Propagation context passed to `ISpanProcessor::OnStart`.
///
/// v1 carries only the active `SpanContext` from the caller thread.
/// Full context propagation machinery (baggage, thread-local slot, W3C
/// propagators) is deferred to v1.1.
class Context
{
public:
    Context() noexcept = default;
    explicit Context(SpanContext active) noexcept : active_span_context(std::move(active)) {}

    SpanContext active_span_context;
};

}  // namespace microtel
