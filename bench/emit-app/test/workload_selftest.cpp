// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test for EMIT_WORKLOAD parsing and per-iteration dispatch (issue #305).
// Plain asserts, no gtest, for the same reason as histogram_selftest.cpp:
// only in-tree builds (MICROTEL_BUILD_BENCH=ON) wire it up.

#undef NDEBUG
#include "backend.hpp"
#include "workload.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace
{

/// Counts which IBackend emit method each iteration reached.
class CountingBackend final : public bench::IBackend
{
public:
    void Init(const bench::BackendOptions& /*opts*/) override {}
    void EmitSpan() override { ++spans; }
    void EmitRequest() override { ++requests; }
    void EmitRecord() override { ++records; }
    void EmitLog() override { ++logs; }
    void EmitLeafPayload() override { ++leaf_payloads; }
    void Shutdown() override {}
    [[nodiscard]] bench::BackendStats Stats() const override { return {}; }

    int spans{0};
    int requests{0};
    int records{0};
    int logs{0};
    int leaf_payloads{0};
};

void TestParseWorkloadMode()
{
    using bench::WorkloadMode;
    assert(bench::ParseWorkloadMode("hot_loop") == WorkloadMode::HotLoop);
    assert(bench::ParseWorkloadMode("realistic_request") == WorkloadMode::RealisticRequest);
    assert(bench::ParseWorkloadMode("hot_loop_metrics") == WorkloadMode::HotLoopMetrics);
    assert(bench::ParseWorkloadMode("hot_loop_logs") == WorkloadMode::HotLoopLogs);
    assert(bench::ParseWorkloadMode("leaf_fanin") == WorkloadMode::LeafFanin);
    // Unknown values keep the historical fallback: the span hot loop.
    assert(bench::ParseWorkloadMode("nonsense") == WorkloadMode::HotLoop);
}

void TestHotLoopLogsEmitsOneLogRecord()
{
    CountingBackend b;
    bench::EmitOnce(b, bench::WorkloadMode::HotLoopLogs);
    assert(b.logs == 1);
    assert(b.spans == 0 && b.requests == 0 && b.records == 0);
}

void TestLeafFaninIngestsOnePayload()
{
    CountingBackend b;
    bench::EmitOnce(b, bench::WorkloadMode::LeafFanin);
    assert(b.leaf_payloads == 1);
    assert(b.spans == 0 && b.requests == 0 && b.records == 0 && b.logs == 0);
}

void TestOtherModesDispatchUnchanged()
{
    CountingBackend b;
    bench::EmitOnce(b, bench::WorkloadMode::HotLoop);
    bench::EmitOnce(b, bench::WorkloadMode::RealisticRequest);
    bench::EmitOnce(b, bench::WorkloadMode::HotLoopMetrics);
    assert(b.spans == 1 && b.requests == 1 && b.records == 1);
    assert(b.logs == 0 && b.leaf_payloads == 0);
}

}  // namespace

int main()
{
    TestParseWorkloadMode();
    TestHotLoopLogsEmitsOneLogRecord();
    TestLeafFaninIngestsOnePayload();
    TestOtherModesDispatchUnchanged();
    std::puts("workload_selftest: all passed");
    return 0;
}
