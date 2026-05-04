// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/provider.hpp"   // DropReason, ConnectionState, HealthSnapshot

#include <cstdint>

namespace microtel::internal
{

/// @brief Single sink for drop counters, batch counters, last-error data,
/// and connection state. Backing store for `Provider::GetExporterHealth()`.
///
/// All methods are thread-safe; the sink is the leaf-lock of the system
/// (`docs/threading-model.md` §4). Counters are `std::atomic<uint64_t>`.
///
/// @threadsafety Thread-safe.
/// @noexcept All methods.
/// @see docs/interfaces.md §4.11
class IDiagnosticsSink
{
public:
    virtual ~IDiagnosticsSink() noexcept = default;

    virtual void RecordDrop(DropReason reason, std::uint64_t n = 1) noexcept = 0;
    virtual void RecordBatchSent() noexcept                                  = 0;
    virtual void RecordBatchFailed(const Error& err) noexcept                = 0;
    virtual void SetQueueDepth(std::uint64_t depth) noexcept                 = 0;
    virtual void SetConnectionState(ConnectionState state) noexcept          = 0;
    [[nodiscard]] virtual HealthSnapshot Snapshot() const noexcept           = 0;
};

}  // namespace microtel::internal
