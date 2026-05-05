// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/clock.hpp"

#include <deque>
#include <optional>
#include <string>

namespace microtel::testing
{

/// @brief Fake `IAuthProvider` with TTL-cache and scripted-response support.
///
/// Two modes: a static value (set `static_value` and leave
/// `scripted_responses` empty), or a FIFO of scripted responses (each
/// `GetAuthorization` call dequeues one). When the scripted queue is empty,
/// `static_value` is returned.
class FakeAuthProvider : public internal::IAuthProvider
{
public:
    /// @brief Returned when `scripted_responses` is empty.
    /// `nullopt` means "no Authorization header"; an empty optional<string>
    /// means the same. Configure as needed.
    microtel::Expected<std::optional<std::string>, microtel::Error> static_value{
        std::optional<std::string>{}};

    /// @brief FIFO of one-shot responses. Each `GetAuthorization` call
    /// pops the front; falls back to `static_value` when empty.
    std::deque<microtel::Expected<std::optional<std::string>, microtel::Error>> scripted_responses;

    // --- Recording ---
    int call_count = 0;
    std::vector<internal::TimePointSteady> seen_times;

    [[nodiscard]] microtel::Expected<std::optional<std::string>, microtel::Error> GetAuthorization(
        internal::TimePointSteady now) override
    {
        ++call_count;
        seen_times.push_back(now);
        if (!scripted_responses.empty())
        {
            auto value = std::move(scripted_responses.front());
            scripted_responses.pop_front();
            return value;
        }
        return static_value;
    }
};

}  // namespace microtel::testing
