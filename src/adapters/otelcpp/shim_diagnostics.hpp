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
/// currently function-local static atomics: one set per process, shared by
/// every provider and option set, never reset (ICP 0033 §7) — is an
/// implementation detail: if a general adapter-diagnostics mechanism is ever built (ICP
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

    /// A `span<const uint8_t>` attribute was omitted because its hex rendering
    /// would exceed `ShimOptions::attribute_value_length_limit`. One per omitted
    /// value.
    std::uint64_t oversized_byte_attributes_omitted = 0;

    /// Largest size, in bytes of the application's byte span (not hex
    /// characters), of any attribute counted in `oversized_byte_attributes_omitted`.
    /// 0 if none has been omitted. Monotonic: it only ever increases.
    std::uint64_t largest_omitted_byte_attribute = 0;
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

[[nodiscard]] inline std::atomic<std::uint64_t>& OversizedByteAttributesOmittedCounter() noexcept
{
    static std::atomic<std::uint64_t> s_counter{0};
    return s_counter;
}

[[nodiscard]] inline std::atomic<std::uint64_t>& LargestOmittedByteAttributeHighWater() noexcept
{
    static std::atomic<std::uint64_t> s_high_water{0};
    return s_high_water;
}

/// @brief Record that a byte-span attribute of @p byte_count bytes was omitted
///        for exceeding `ShimOptions::attribute_value_length_limit`.
///
/// The counter and the high-water mark are updated independently, each
/// relaxed: a concurrent snapshot can see one updated and not yet the other
/// (ICP 0033 §4). The high-water mark is a compare-exchange loop that only
/// ever stores a larger value (`std::atomic::fetch_max` is C++26).
inline void RecordOversizedByteAttributeOmitted(std::uint64_t byte_count) noexcept
{
    OversizedByteAttributesOmittedCounter().fetch_add(1, std::memory_order_relaxed);
    auto& high_water = LargestOmittedByteAttributeHighWater();
    std::uint64_t current = high_water.load(std::memory_order_relaxed);
    // A failed exchange reloads `current`; stop once it is no longer smaller.
    while (current < byte_count)
    {
        if (high_water.compare_exchange_weak(current, byte_count, std::memory_order_relaxed))
        {
            return;
        }
    }
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
        .oversized_byte_attributes_omitted =
            detail::OversizedByteAttributesOmittedCounter().load(std::memory_order_relaxed),
        .largest_omitted_byte_attribute =
            detail::LargestOmittedByteAttributeHighWater().load(std::memory_order_relaxed),
    };
}

}  // namespace microtel::adapters::otelcpp
