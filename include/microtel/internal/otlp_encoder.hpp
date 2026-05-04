// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/encoded_payload.hpp"

namespace microtel::internal
{

/// @brief Encodes a `BatchHandle` into OTLP protobuf bytes.
///
/// **The only place in the codebase that includes upb headers** (LOCKED —
/// `docs/memory-model.md` §3.1). No upb type appears in this header.
///
/// One `UpbArena` per call; arena destroyed before `Encode` returns.
/// Stateless across calls. Called only by the exporter worker.
///
/// @threadsafety Not thread-safe (single-caller — exporter worker).
/// @see docs/interfaces.md §4.2
class IOtlpEncoder
{
public:
    virtual ~IOtlpEncoder() noexcept = default;

    [[nodiscard]] virtual EncodedPayload Encode(const BatchHandle& batch) = 0;
};

}  // namespace microtel::internal
