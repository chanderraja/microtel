// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "workload.hpp"

#include "backend.hpp"

#include <string_view>

namespace bench
{

WorkloadMode ParseWorkloadMode(std::string_view name) noexcept
{
    if (name == "realistic_request")
    {
        return WorkloadMode::RealisticRequest;
    }
    if (name == "hot_loop_metrics")
    {
        return WorkloadMode::HotLoopMetrics;
    }
    if (name == "hot_loop_logs")
    {
        return WorkloadMode::HotLoopLogs;
    }
    if (name == "leaf_fanin")
    {
        return WorkloadMode::LeafFanin;
    }
    return WorkloadMode::HotLoop;
}

void EmitOnce(IBackend& backend, WorkloadMode mode)
{
    switch (mode)
    {
    case WorkloadMode::RealisticRequest:
        backend.EmitRequest();
        break;
    case WorkloadMode::HotLoopMetrics:
        backend.EmitRecord();
        break;
    case WorkloadMode::HotLoopLogs:
        backend.EmitLog();
        break;
    case WorkloadMode::LeafFanin:
        backend.EmitLeafPayload();
        break;
    case WorkloadMode::HotLoop:
        backend.EmitSpan();
        break;
    }
}

}  // namespace bench
