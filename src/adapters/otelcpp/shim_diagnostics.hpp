// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

/// @file
/// Adapter-local diagnostics for events the shim cannot report through
/// `microtel::Provider::GetExporterHealth()`, because they happen above the
/// SDK — where `IDiagnosticsSink` is unreachable (ICP 0016).
///
/// `GetShimDiagnostics()` is the stable public surface. Its backing store —
/// currently a pair of function-local static atomics — is an implementation
/// detail: if a general adapter-diagnostics mechanism is ever built (ICP
/// 0016's "Forward-compatibility"), this function is reimplemented to read
/// from it, and every existing caller sees no change.

namespace microtel::adapters::otelcpp
{

/// @brief Snapshot of adapter-local events the shim has recorded.
///
/// These counters are the shim's own; they do not appear in
/// `microtel::Provider::GetExporterHealth()`.
struct ShimDiagnostics
{
    /// A `uint64_t` counter or histogram measurement above `INT64_MAX` was
    /// omitted — no `int64_t` representation exists and, unlike attributes
    /// (ICP 0015), there is no degraded type to preserve a measurement into.
    std::uint64_t unrepresentable_measurements_omitted = 0;

    /// An application observable-metric callback threw during collection;
    /// the exception was contained at the shim boundary rather than
    /// propagating into microtel's `noexcept` collection path.
    std::uint64_t observer_callback_failures = 0;
};

namespace detail
{

[[nodiscard]] inline std::atomic<std::uint64_t>&
UnrepresentableMeasurementsOmittedCounter() noexcept
{
    static std::atomic<std::uint64_t> s_counter{0};
    return s_counter;
}

[[nodiscard]] inline std::atomic<std::uint64_t>& ObserverCallbackFailuresCounter() noexcept
{
    static std::atomic<std::uint64_t> s_counter{0};
    return s_counter;
}

/// @brief Record that a `uint64_t` measurement above `INT64_MAX` was omitted.
inline void RecordUnrepresentableMeasurementOmitted() noexcept
{
    UnrepresentableMeasurementsOmittedCounter().fetch_add(1, std::memory_order_relaxed);
}

/// @brief Record that an observable-metric callback threw and was contained.
inline void RecordObserverCallbackFailure() noexcept
{
    ObserverCallbackFailuresCounter().fetch_add(1, std::memory_order_relaxed);
}

}  // namespace detail

/// @brief Current counts, as of the call.
/// @threadsafety Thread-safe. @noexcept
[[nodiscard]] inline ShimDiagnostics GetShimDiagnostics() noexcept
{
    return ShimDiagnostics{
        .unrepresentable_measurements_omitted =
            detail::UnrepresentableMeasurementsOmittedCounter().load(std::memory_order_relaxed),
        .observer_callback_failures =
            detail::ObserverCallbackFailuresCounter().load(std::memory_order_relaxed),
    };
}

}  // namespace microtel::adapters::otelcpp
