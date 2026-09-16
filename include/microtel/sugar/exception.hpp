// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <typeinfo>

namespace microtel::sugar
{

/// @brief OTel semantic-convention name of the exception event.
constexpr std::string_view kExceptionEventName{"exception"};

/// @brief OTel semantic-convention key for the exception's type name.
constexpr std::string_view kExceptionTypeKey{"exception.type"};

/// @brief OTel semantic-convention key for the exception's message.
constexpr std::string_view kExceptionMessageKey{"exception.message"};

/// @brief Set `Error` status and add the OTel `exception` event.
///
/// Two calls on the public `Span`, in this order: `SetStatus(Error, message)`
/// — which cannot fail — then `AddEvent("exception", …)` carrying
/// `exception.type` and `exception.message`.
///
/// `exception.stacktrace` and `exception.escaped` are deliberately omitted:
/// microtel captures no stack traces, and whether an exception escaped the
/// span's scope is not knowable from inside this helper.
///
/// Setting `Error` status is a deliberate divergence from
/// opentelemetry-cpp's similarly-named `Span::RecordException`, which does
/// not. `microtel-roadmap.md` §5 v1.1 specifies both halves, and
/// `microtel-spec.md` §18.1 excludes sugar from conformance testing, so this
/// helper is not measured against the OTel API surface.
///
/// @param span    borrowed; not retained.
/// @param type    the exception's type name; borrowed, copied into the event.
/// @param message the exception's message; borrowed, copied into the event.
///
/// @noexcept The status is set first and cannot fail. If building the two
///           event attributes throws `std::bad_alloc`, the event is dropped
///           and the call returns with the status already set.
///
/// @see docs/icps/0028-sugar-surface.md §1
inline void RecordException(::microtel::Span& span,
                            std::string_view type,
                            std::string_view message) noexcept
{
    span.SetStatus(::microtel::StatusCode::Error, message);

    constexpr std::size_t kAttributeCount = 2;
    try
    {
        const std::array<::microtel::KeyValue, kAttributeCount> attributes{
            ::microtel::KeyValue{.key = std::string{kExceptionTypeKey}, .value = std::string{type}},
            ::microtel::KeyValue{.key = std::string{kExceptionMessageKey},
                                 .value = std::string{message}}};
        span.AddEvent(kExceptionEventName,
                      ::microtel::AttributeSpan{attributes.data(), attributes.size()});
    }
    catch (const std::bad_alloc&)
    {
        // The two attribute strings are the only allocation on this path, so
        // there is no event left to add. The status set above stands.
        return;
    }
}

#if defined(__cpp_rtti)

/// @brief Set `Error` status and add the OTel `exception` event for @p e.
///
/// `exception.type` is `typeid(e).name()` — the implementation-mangled name,
/// **not** demangled; `exception.message` and the status description are
/// `e.what()`.
///
/// The name is left mangled deliberately: demangling needs
/// `abi::__cxa_demangle`, which allocates on an error path and is
/// ABI-specific, while the mangled form is stable and greppable. A
/// collector-side processor — or the (type, message) overload above — gives a
/// pretty name where one is wanted.
///
/// This overload costs `typeid`, so it is declared only when RTTI is enabled.
/// A consumer compiling with `-fno-rtti` uses the (type, message) overload,
/// which needs neither.
///
/// @param span borrowed; not retained.
/// @param e    borrowed; not retained.
///
/// @noexcept As the overload above.
///
/// @see docs/icps/0028-sugar-surface.md §1
inline void RecordException(::microtel::Span& span, const std::exception& e) noexcept
{
    RecordException(span, std::string_view{typeid(e).name()}, std::string_view{e.what()});
}

#endif  // __cpp_rtti

}  // namespace microtel::sugar
