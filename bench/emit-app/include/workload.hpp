// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Workload selection: which IBackend emit method one iteration calls.

#pragma once

#include "backend.hpp"

#include <cstdint>
#include <string_view>

namespace bench
{

/// Workload pattern the emit-app runs during a "run" command.
enum class WorkloadMode : std::uint8_t
{
    HotLoop,          ///< EmitSpan() called as fast as possible (or rate-limited)
    RealisticRequest, ///< EmitRequest() — one parent span + two child spans per iteration
    HotLoopMetrics,   ///< EmitRecord() — one Counter::Add() + one Histogram::Record() per iteration
    HotLoopLogs,      ///< EmitLog() — one Logger::Emit() per iteration
    LeafFanin,        ///< EmitLeafPayload() — one LeafReceiver::Ingest() per iteration
};

/// Map an EMIT_WORKLOAD value to its mode. Unknown values fall back to HotLoop.
[[nodiscard]] WorkloadMode ParseWorkloadMode(std::string_view name) noexcept;

/// Run one workload iteration: the IBackend emit call `mode` selects.
void EmitOnce(IBackend& backend, WorkloadMode mode);

}  // namespace bench
