// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"
#include "microtel/tracer.hpp"

#include <concepts>
#include <initializer_list>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

namespace microtel::sugar
{

/// @brief Start a span named for the enclosing function and make it current.
///
/// The name is `loc.function_name()`. `loc` defaults to
/// `std::source_location::current()`, which as a default argument is
/// evaluated at the **call site** — so the name is the calling function's,
/// not this one's. That string has static storage duration, so passing it as
/// a `std::string_view` to `StartAsCurrentSpan` (which copies it into the
/// record on the sampled path) is safe.
///
/// @param tracer borrowed; not retained. The returned scope outlives no
///               tracer it did not already outlive.
/// @param kind   span kind; `Internal` by default.
/// @param loc    leave unset — the default captures the call site.
///
/// @return a scope that ends the span and restores the caller's context on
///         destruction, in that order. **Thread-confined** (ICP 0025 §3).
///
/// @noexcept Always succeeds; `StartAsCurrentSpan` is `noexcept`.
///
/// @see docs/icps/0028-sugar-surface.md §1
[[nodiscard]] inline ::microtel::ScopedSpan TraceFunction(
    ::microtel::Tracer& tracer,
    ::microtel::SpanKind kind = ::microtel::SpanKind::Internal,
    std::source_location loc = std::source_location::current()) noexcept
{
    return tracer.StartAsCurrentSpan(
        loc.function_name(), {.kind = kind, .parent = {}, .start_time = {}, .attributes = {}});
}

/// @brief RAII scoped span with inline attributes.
///
/// `attributes` is viewed, not owned: the `std::initializer_list` backing
/// array lives to the end of the enclosing full-expression, and
/// `StartAsCurrentSpan` copies the attributes into the span record inside
/// this call. The returned scope holds no reference to them.
///
/// ```cpp
/// namespace mt = microtel::sugar;               // consumer-side alias
/// const auto scope = mt::Span(*tracer, "checkout", {{"cart.id", "c-42"}});
/// ```
///
/// @param tracer     borrowed; not retained.
/// @param name       borrowed; copied into the record on the sampled path.
/// @param attributes borrowed for the duration of this call only.
/// @param kind       span kind; `Internal` by default.
///
/// @return a scope as `TraceFunction`'s. **Thread-confined**.
///
/// @noexcept Always succeeds.
///
/// @see docs/icps/0028-sugar-surface.md §1
[[nodiscard]] inline ::microtel::ScopedSpan Span(
    ::microtel::Tracer& tracer,
    std::string_view name,
    std::initializer_list<::microtel::KeyValue> attributes = {},
    ::microtel::SpanKind kind = ::microtel::SpanKind::Internal) noexcept
{
    return tracer.StartAsCurrentSpan(
        name,
        {.kind = kind,
         .parent = {},
         .start_time = {},
         .attributes = ::microtel::AttributeSpan{attributes.begin(), attributes.size()}});
}

/// @brief Run @p fn inside a scoped span; returns whatever @p fn returns.
///
/// The return type is forwarded exactly — a value stays a value, a reference
/// stays a reference, `void` stays `void` — and the span is ended after the
/// return value has been initialised.
///
/// If @p fn throws, the span is ended by `ScopedSpan`'s destructor during
/// unwinding and the exception propagates **unrecorded**. Recording it is
/// `mt::TryCatch`, `microtel-roadmap.md` §5 v1.4; a `Traced` that
/// swallowed-and-rethrew would make that helper redundant and this one
/// surprising.
///
/// @param tracer borrowed; not retained.
/// @param name   span name; borrowed.
/// @param fn     invoked exactly once, with the new span current.
///
/// @noexcept Conditionally — `noexcept` iff invoking @p fn is, so a
///           `noexcept` caller keeps its guarantee.
///
/// @see docs/icps/0028-sugar-surface.md §1
template <std::invocable Fn>
inline decltype(auto) Traced(::microtel::Tracer& tracer,
                             std::string_view name,
                             Fn&& fn) noexcept(std::is_nothrow_invocable_v<Fn&&>)
{
    const ::microtel::ScopedSpan scope = Span(tracer, name);
    return std::forward<Fn>(fn)();
}

}  // namespace microtel::sugar

/// @brief Internal token-paste helper for `MICROTEL_TRACE_FUNCTION`. The two
///        levels are what make `__LINE__` expand before it is pasted.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MICROTEL_SUGAR_CAT_(a, b) a##b

/// @brief Second level of the paste. See `MICROTEL_SUGAR_CAT_`.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MICROTEL_SUGAR_CAT(a, b) MICROTEL_SUGAR_CAT_(a, b)

/// @brief Declare a scoped span named for the enclosing function.
///
/// A macro rather than a function because only a declaration can create the
/// variable whose lifetime is the scope; a function cannot. It takes the
/// tracer because microtel has no global provider, and adding one for a
/// convenience macro's benefit would put a mutable global with
/// static-destruction-order problems under it (ICP 0028, Discrepancies §1).
///
/// The name is `__LINE__`-uniqued, so two uses in one scope are two distinct
/// scopes. The declared object is `const` deliberately: a scope that cannot
/// be moved out of cannot be destroyed out of order, which is the one
/// programming error ICP 0025 §3 documents and does not check.
///
/// ```cpp
/// void HandleRequest(microtel::Tracer& tracer)
/// {
///     MICROTEL_TRACE_FUNCTION(tracer);
///     …
/// }
/// ```
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MICROTEL_TRACE_FUNCTION(tracer)                                                            \
    const ::microtel::ScopedSpan MICROTEL_SUGAR_CAT(microtel_fn_scope_, __LINE__) =                \
        ::microtel::sugar::TraceFunction(tracer)
