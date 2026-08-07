// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/log_batch.hpp"

namespace microtel::internal
{

/// @brief Encodes a `LogBatchHandle` into OTLP logs protobuf bytes
/// (`ExportLogsServiceRequest`).
///
/// Implemented by the upb `OtlpEncoder` extension — the only place that
/// includes upb headers (LOCKED). No upb type appears in this header.
/// One arena per call; stateless across calls.
///
/// @threadsafety Not thread-safe (single-caller — the log exporter worker).
/// @see docs/logs-design.md §encoder
class ILogEncoder
{
public:
    virtual ~ILogEncoder() noexcept = default;

    [[nodiscard]] virtual EncodedPayload Encode(const LogBatchHandle& batch) = 0;
};

}  // namespace microtel::internal
