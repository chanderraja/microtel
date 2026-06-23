// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/internal/wire_result.hpp"

#include <atomic>
#include <chrono>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::IWireCodec`.
///
/// Returns a configured `WireResult` on every Send call. The most-used mock
/// in the project — every exporter unit test wires this rather than a real
/// codec.
class MockWireCodec : public internal::IWireCodec
{
public:
    /// @brief Result returned from every Send call. The default-constructed
    /// `WireResult` has `success=false` and `retryable=false` (per its
    /// in-class initialisers); tests opt in to success by flipping the flag.
    internal::WireResult result_to_return{};

    // --- Recording ---
    // Atomic: OtlpExporter's worker thread calls Send() (via SendAll) while the
    // test thread reads the count after ForceFlush, so it is accessed cross-thread.
    std::atomic<int> send_call_count{0};

    // --- IWireCodec ---

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& /*payload*/,
                                            std::chrono::milliseconds /*deadline*/) override
    {
        ++send_call_count;
        return result_to_return;
    }
};

}  // namespace microtel::testing
