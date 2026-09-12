// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Environment plumbing for the conformance tier.
//
// Conformance tests need a live collector, which a developer running plain
// `ctest` does not have. The tier therefore reads its endpoints from the
// environment and skips when they are absent — but a skip that CI cannot tell
// apart from a pass is worthless, so ci/scripts/conformance.sh also exports
// MICROTEL_CONFORMANCE_REQUIRE. With it set, a missing variable is broken
// plumbing and fails loudly instead of skipping.

#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace microtel::testing
{

/// @brief Environment variable that turns a skip into a failure.
///
/// Exported by `ci/scripts/conformance.sh` once the collector is healthy.
inline constexpr const char* kConformanceRequireEnv = "MICROTEL_CONFORMANCE_REQUIRE";

/// @brief Reads an environment variable.
/// @param name variable name; borrowed.
/// @return its value, or `nullopt` if unset.
inline std::optional<std::string> GetEnv(const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read before any thread starts.
    const char* const value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string(value);
}

/// @brief A span name no other run will produce.
///
/// The collector's output file accumulates across tests and across runs, so a
/// round-trip assertion needs a needle unique to this span. 64 bits of
/// `std::random_device` is ample and needs no seeding ceremony.
inline std::string UniqueMarker()
{
    constexpr std::string_view kHexDigits = "0123456789abcdef";
    constexpr std::size_t kHexCharCount = 16;
    constexpr unsigned int kNibbleMask = 0xFU;

    std::random_device device;
    std::string marker = "conformance.";
    marker.reserve(marker.size() + kHexCharCount);
    for (std::size_t i = 0; i < kHexCharCount; ++i)
    {
        marker.push_back(kHexDigits[device() & kNibbleMask]);
    }
    return marker;
}

/// @brief The tier's skip contract: resolve one conformance environment
/// variable, or explain why the test cannot run.
///
/// A function rather than a macro so the call site stays lintable and the test
/// body stays flat:
///
/// ```
/// std::string endpoint;
/// if (!ConformanceEnabled(kHttpEndpointEnv, endpoint))
/// {
///     GTEST_SKIP() << "collector not configured";
/// }
/// ```
///
/// @param env_name  variable to read; borrowed.
/// @param out_value receives the value when this returns true; untouched otherwise.
/// @return true when the test may proceed. When false, the caller must
///         `GTEST_SKIP()` — and a failure has already been recorded if
///         `MICROTEL_CONFORMANCE_REQUIRE` was set.
inline bool ConformanceEnabled(const char* env_name, std::string& out_value)
{
    std::optional<std::string> value = GetEnv(env_name);
    if (value.has_value())
    {
        out_value = *std::move(value);
        return true;
    }
    if (GetEnv(kConformanceRequireEnv).has_value())
    {
        ADD_FAILURE() << env_name << " is unset while " << kConformanceRequireEnv
                      << " is set — the conformance runner failed to export it";
    }
    return false;
}

}  // namespace microtel::testing
