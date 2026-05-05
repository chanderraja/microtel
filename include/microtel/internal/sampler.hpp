// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/trace.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace microtel::internal
{

/// @brief Sampling decision returned by `ISampler::ShouldSample`.
enum class SamplingDecision : std::uint8_t
{
    Drop = 0,             ///< drop the span; no record kept
    RecordOnly = 1,       ///< record locally but do not export
    RecordAndSample = 2,  ///< record and export
};

/// @brief Inputs to `ISampler::ShouldSample`.
struct SamplingContext
{
    SpanContext parent;
    SpanKind span_kind = SpanKind::Internal;
    std::string_view span_name;
    AttributeSpan initial_attributes;
    /// Links in the to-be-created span; empty in the common case.
    std::span<const class SpanLink> links;
    TraceId trace_id;  ///< the future TraceId of the span if newly rooted
};

/// @brief Outputs from `ISampler::ShouldSample`.
struct SamplingResult
{
    SamplingDecision decision = SamplingDecision::Drop;
    std::vector<KeyValue> additional_attributes;  ///< appended to span if sampled
    std::optional<TraceState> trace_state;        ///< overrides parent state if set
};

/// @brief Pluggable sampling decision.
///
/// The four built-in implementations realise this interface: `AlwaysOn`,
/// `AlwaysOff`, `TraceIdRatio`, `ParentBased`.
///
/// `ShouldSample` is called from the **caller thread** on the hot path; it
/// must be `noexcept`, thread-safe, and **must not allocate on the hot path
/// in the common case**. (LOCKED — `docs/memory-model.md` §8.1.)
///
/// @threadsafety Thread-safe.
/// @noexcept ShouldSample.
/// @see docs/interfaces.md §4.5
class ISampler
{
public:
    virtual ~ISampler() noexcept = default;

    [[nodiscard]] virtual SamplingResult ShouldSample(
        const SamplingContext& ctx) const noexcept = 0;

    [[nodiscard]] virtual std::string_view Description() const noexcept = 0;
};

}  // namespace microtel::internal
