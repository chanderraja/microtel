// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_encoder.hpp"

#include <atomic>
#include <cstddef>
#include <memory>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::IMetricEncoder`.
///
/// Returns a 1-byte-allocated empty `EncodedPayload` on every call. No
/// protobuf encoding happens. Counts calls atomically so the test thread can
/// read after `ForceFlush`.
class MockMetricEncoder : public internal::IMetricEncoder
{
public:
    std::atomic<int> encode_call_count{0};

    [[nodiscard]] internal::EncodedPayload Encode(
        const internal::MetricBatchHandle& /*batch*/) override
    {
        ++encode_call_count;
        auto buf = std::make_unique<std::byte[]>(1);
        return internal::EncodedPayload{std::move(buf), 0};
    }
};

}  // namespace microtel::testing
