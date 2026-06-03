// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_result.hpp"

#include <chrono>
#include <vector>

namespace microtel::internal
{

/// @brief OTLP wire codec — one of two implementations.
///
/// Two implementations: HTTP-protobuf (`src/wire/http/`) and gRPC-on-nghttp2
/// (`src/wire/grpc/`). One interface (LOCKED — ICP 0001).
///
/// The codec — not the exporter — owns retry classification. The exporter
/// respects `WireResult::retryable` and `WireResult::retry_after` without
/// reinterpretation.
///
/// `Send` is called only by the exporter worker; the codec is **not**
/// thread-safe. Concurrent `Send` calls are a contract violation.
///
/// @threadsafety Not thread-safe (single-caller — exporter worker).
/// @see docs/interfaces.md §4.3
/// @see docs/grpc-wire-protocol.md
class IWireCodec
{
public:
    virtual ~IWireCodec() noexcept = default;

    [[nodiscard]] virtual WireResult Send(EncodedPayload&& payload,
                                          std::chrono::milliseconds deadline) = 0;

    /// @brief Submit N payloads and wait for all responses.
    ///
    /// Default implementation calls `Send` in a loop — correct for all codecs
    /// that do not override. `HttpWireCodec` overrides to submit all requests
    /// as concurrent HTTP/2 streams before waiting, collapsing N round trips
    /// into one.
    ///
    /// Results are returned in the same order as `payloads`.
    [[nodiscard]] virtual std::vector<WireResult> SendAll(std::vector<EncodedPayload> payloads,
                                                          std::chrono::milliseconds deadline)
    {
        std::vector<WireResult> results;
        results.reserve(payloads.size());
        for (auto& p : payloads)
        {
            results.push_back(Send(std::move(p), deadline));
        }
        return results;
    }
};

}  // namespace microtel::internal
