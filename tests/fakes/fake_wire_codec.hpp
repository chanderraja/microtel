// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/internal/wire_result.hpp"

#include <atomic>
#include <chrono>
#include <deque>

namespace microtel::testing
{

/// @brief Scriptable `IWireCodec` for retry / exporter integration tests.
///
/// Distinct from `MockWireCodec` (which returns one fixed result):
/// `FakeWireCodec` serves scripted `WireResult`s in FIFO order, falling back
/// to `default_result` when the queue is empty. Tests drive scenarios such as
/// retry-then-succeed or retry-until-exhausted by pre-loading the queue.
class FakeWireCodec : public internal::IWireCodec
{
public:
    // --- Recording ---
    // Atomic: OtlpExporter's worker thread calls Send() while the test thread
    // reads the count after ForceFlush, so it is accessed cross-thread.
    std::atomic<int> send_call_count{0};

    // --- Configurable ---
    /// @brief FIFO of scripted results; each Send pops the front.
    std::deque<internal::WireResult> scripted_results;
    /// @brief Returned when scripted_results is empty. Default: failure, non-retryable.
    internal::WireResult default_result{};

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& /*payload*/,
                                            std::chrono::milliseconds /*deadline*/) override
    {
        ++send_call_count;
        if (!scripted_results.empty())
        {
            auto r = std::move(scripted_results.front());
            scripted_results.pop_front();
            return r;
        }
        return default_result;
    }
};

}  // namespace microtel::testing
