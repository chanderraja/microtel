// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_result.hpp"

#include <chrono>

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
};

}  // namespace microtel::internal
