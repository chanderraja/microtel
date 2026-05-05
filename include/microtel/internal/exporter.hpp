// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <cstdint>

namespace microtel::internal
{

/// @brief Outcome of `IExporter::Export`.
enum class ExportResult : std::uint8_t
{
    Success = 0,          ///< accepted into the export pipeline
    Failure = 1,          ///< accepted but classified as non-retryable terminal failure
    Dropped = 2,          ///< queue full or post-shutdown; record dropped
    AlreadyShutDown = 3,  ///< exporter has shut down
};

/// @brief The protocol-agnostic export pipeline.
///
/// Owns the exporter worker thread. Drains batches from the processor,
/// encodes via `IOtlpEncoder`, sends via `IWireCodec`, classifies retries,
/// accounts for drops.
///
/// `Export` is non-blocking: it hands the batch to the worker via the
/// worker's task queue and returns immediately. Successful return does not
/// mean the batch was sent — only that it was accepted into the pipeline.
///
/// @threadsafety Thread-safe.
/// @noexcept All methods.
/// @see docs/interfaces.md §4.4
class IExporter
{
public:
    virtual ~IExporter() noexcept = default;

    [[nodiscard]] virtual ExportResult Export(BatchHandle&& batch) noexcept = 0;

    [[nodiscard]] virtual microtel::Status ForceFlush(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
