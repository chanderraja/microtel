// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file expected.hpp
/// @brief Compatibility shim that aliases an `expected<T, E>` value type into
/// the `microtel` namespace.
///
/// `std::expected` is a C++23 feature. microtel's compiler floor is C++20
/// (per `microtel-spec.md` §1, to preserve the RHEL 8 + devtoolset-11
/// target). Until the project moves to C++23, the public API and internal
/// interfaces use `microtel::Expected<T, E>`, which:
///
/// - Aliases to `std::expected<T, E>` when compiled under C++23 with a
///   standard library that ships `<expected>`.
/// - Aliases to `tl::expected<T, E>` (vendored under
///   `third_party/tl-expected/`) on C++20.
///
/// The two aliases share an API. User code that consumes microtel only
/// touches `microtel::Expected` and `microtel::Unexpected`; the alias
/// resolution is invisible.
///
/// @see docs/icps/0002-vendor-tl-expected.md

#include <type_traits>

#if __cplusplus >= 202302L && __has_include(<expected>)

#include <expected>

namespace microtel
{

/// @brief `std::expected<T, E>` alias when C++23 `<expected>` is
/// available; otherwise `tl::expected<T, E>` from the vendored polyfill.
template <typename T, typename E>
using Expected = std::expected<T, E>;

/// @brief `std::unexpected<E>` alias matching `Expected`.
template <typename E>
using Unexpected = std::unexpected<E>;

/// @brief Constructs an `Unexpected<E>` without requiring CTAD on an alias
/// template (alias-template CTAD is C++23; this header targets C++20).
template <typename E>
constexpr auto make_unexpected(E&& e) -> Unexpected<std::decay_t<E>>
{
    return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

}  // namespace microtel

#else

#include "tl/expected.hpp"

namespace microtel
{

template <typename T, typename E>
using Expected = tl::expected<T, E>;

template <typename E>
using Unexpected = tl::unexpected<E>;

/// @brief Constructs an `Unexpected<E>` without requiring CTAD on an alias
/// template (alias-template CTAD is C++23; this header targets C++20).
template <typename E>
constexpr auto make_unexpected(E&& e) -> Unexpected<std::decay_t<E>>
{
    return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

}  // namespace microtel

#endif
