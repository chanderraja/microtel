// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Umbrella header for the `microtel::sugar` ergonomic layer.
///
/// Header-only, and it calls nothing but `include/microtel/*.hpp`: no `src/`,
/// no `include/microtel/internal/`, and in particular never
/// `internal::SpanDeleter`. Its RAII is `microtel::ScopedSpan` and
/// `microtel::SpanHandle`; sugar defines no RAII type of its own, so the
/// end-then-restore ordering lives in exactly one place (ICP 0025 §3).
/// Including it adds nothing to a consumer's link closure.
///
/// The namespace is `microtel::sugar`. `mt` is a **consumer-side** alias that
/// no microtel header declares — a library that squats a two-character global
/// name has taken something it cannot give back:
///
/// ```cpp
/// #include <microtel/sugar.hpp>
///
/// namespace mt = microtel::sugar;   // in your code, never in ours
///
/// void HandleRequest(microtel::Tracer& tracer)
/// {
///     MICROTEL_TRACE_FUNCTION(tracer);
///     const auto scope = mt::Span(tracer, "checkout", {{"cart.id", "c-42"}});
/// }
/// ```
///
/// Note that `microtel::sugar::Span` is a *function*, so inside the namespace
/// the core types are spelled fully-qualified (`::microtel::Span`). Consumers
/// are unaffected: `mt::Span(...)` is the factory, `microtel::Span` the class.
///
/// @see docs/icps/0028-sugar-surface.md

#include "microtel/sugar/attr_key.hpp"
#include "microtel/sugar/exception.hpp"
#include "microtel/sugar/span.hpp"
