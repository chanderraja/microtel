// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/clock.hpp"

#include <optional>
#include <string>

namespace microtel::internal
{

/// @brief Supplies the `Authorization` header value for each export batch.
///
/// v1 implementations: `StaticHeadersAuthProvider` (returns a constant) and
/// `CallbackAuthProvider` (calls user code with a TTL cache).
///
/// `GetAuthorization` is callable from the exporter worker. The user-supplied
/// callback wrapped by `CallbackAuthProvider` may be invoked on the exporter
/// worker thread; this is documented in the public `WithAuthProvider` API
/// (LOCKED — `docs/interfaces.md` §4.9).
///
/// @threadsafety Thread-safe (in practice: single-caller — exporter worker).
/// @see docs/interfaces.md §4.9
class IAuthProvider
{
public:
    virtual ~IAuthProvider() noexcept = default;

    /// @brief Returns the value of the `Authorization` header for the next
    /// batch. Empty optional means: do not send the header at all.
    ///
    /// @param now monotonic time used for cache TTL arithmetic.
    [[nodiscard]] virtual microtel::Expected<std::optional<std::string>, microtel::Error>
        GetAuthorization(TimePointSteady now) = 0;
};

}  // namespace microtel::internal
