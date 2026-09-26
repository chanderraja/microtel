// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"

#include <vector>

namespace microtel::internal
{

/// @brief Optional companion to `IExporter`: accept every `BatchHandle` of one
///        processor drain in a single call.
///
/// A drain that holds spans from several Resources or scopes yields several
/// `BatchHandle`s. Handed over one `Export` call at a time, the exporter's
/// worker can wake between two of them and send the first alone, so whether
/// one drain leaves as one request would depend on thread timing. Handed over
/// together, they are queued under one lock and the worker sees them all
/// (`docs/leaf-concentrator-design.md` §3.6.1).
///
/// Not a change to `IExporter`, whose contract is untouched: a processor that
/// is given no group exporter falls back to one `Export` per handle.
///
/// @threadsafety Thread-safe.
/// @see docs/interfaces.md §4.4
class IBatchGroupExporter
{
public:
    IBatchGroupExporter() noexcept = default;
    virtual ~IBatchGroupExporter() noexcept = default;

    IBatchGroupExporter(const IBatchGroupExporter&) = delete;
    IBatchGroupExporter& operator=(const IBatchGroupExporter&) = delete;
    IBatchGroupExporter(IBatchGroupExporter&&) = delete;
    IBatchGroupExporter& operator=(IBatchGroupExporter&&) = delete;

    /// @brief Queue every handle in @p batches, in order, as `Export` would
    ///        queue each one, and wake the worker once.
    ///
    /// Each handle is accepted or refused on its own terms, and a refused one
    /// is counted exactly as `Export` counts it.
    virtual void ExportGroup(std::vector<BatchHandle>&& batches) noexcept = 0;
};

}  // namespace microtel::internal
