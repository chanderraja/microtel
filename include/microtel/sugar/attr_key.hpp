// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/span.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace microtel::sugar
{

/// @brief A pre-bound attribute key: one place where a key name is spelled.
///
/// Intended use is a `constexpr` object at namespace scope, which is
/// constant-initialised — no static-initialisation order to reason about and
/// no allocation anywhere in the type:
///
/// ```cpp
/// namespace mt = microtel::sugar;               // consumer-side alias
/// constexpr mt::AttrKey kHttpMethod{"http.method"};
///
/// kHttpMethod.Set(span, std::string{"GET"});    // on a live span
/// auto scope = mt::Span(tracer, "req", {kHttpMethod(std::string{"GET"})});
/// ```
///
/// What it buys: a distinct type, so a key cannot be passed where a value is
/// expected, and a `std::string_view` whose length is fixed at compile time —
/// no `strlen` at each of N call sites where the compiler cannot see the
/// literal.
///
/// What it deliberately does **not** buy in v1.1: pre-encoded wire bytes.
/// Carrying protobuf field framing here would be an ABI change on
/// `attribute.hpp` and would put wire encoding in a public header, against
/// CLAUDE.md rule 13. `microtel-roadmap.md` §4 v1.5 schedules that
/// optimisation; shipping the plain binder now is what makes it
/// source-compatible for callers when it lands.
///
/// Rule of zero: all five special members are implicit.
///
/// @threadsafety Thread-safe (immutable, and it owns nothing).
///
/// @see docs/icps/0028-sugar-surface.md §2
class AttrKey
{
public:
    /// @brief Bind a key name.
    ///
    /// @param key **borrowed** — must outlive this object and every
    ///            `KeyValue` built from it. A string literal is the intended
    ///            argument.
    constexpr explicit AttrKey(std::string_view key) noexcept : m_key(key) {}

    /// @brief The bound key. Borrowed from whatever was passed to the
    ///        constructor.
    [[nodiscard]] constexpr std::string_view Key() const noexcept
    {
        return m_key;
    }

    /// @brief `span.SetAttribute(Key(), std::move(value))`.
    ///
    /// @param span  borrowed; not retained.
    /// @param value moved into the span record on the sampled path.
    ///
    /// @noexcept Inherits `Span::SetAttribute`'s contract: on any internal
    ///           failure the attribute is dropped and counted.
    void Set(::microtel::Span& span, ::microtel::AttributeValue value) const noexcept
    {
        span.SetAttribute(m_key, std::move(value));
    }

    /// @brief Build a `KeyValue` for the `AttributeSpan` paths — event
    ///        attributes, `StartSpanOptions::attributes`, `sugar::Span`.
    ///
    /// Allocates iff the key exceeds `std::string`'s small-buffer capacity;
    /// the copy is `KeyValue::key`'s, which is owned by declaration in
    /// `attribute.hpp` and is not something `AttrKey` can remove.
    [[nodiscard]] ::microtel::KeyValue operator()(::microtel::AttributeValue value) const
    {
        return ::microtel::KeyValue{.key = std::string{m_key}, .value = std::move(value)};
    }

private:
    std::string_view m_key;
};

}  // namespace microtel::sugar
