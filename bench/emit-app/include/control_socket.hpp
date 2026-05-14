// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TCP control socket (port 9090).
// The Python driver connects, writes JSON commands, reads JSON responses.
// Protocol: newline-delimited JSON (ndjson) — one JSON object per line.

#pragma once

#include "backend.hpp"
#include "histogram.hpp"

#include <cstdint>
#include <string>

namespace bench
{

struct RunResult
{
    uint64_t spans_emitted{0};
    DroppedCounts spans_dropped{};
    uint64_t bytes_sent{0};
    uint64_t duration_ns{0};
    uint64_t latency_p50_ns{0};
    uint64_t latency_p95_ns{0};
    uint64_t latency_p99_ns{0};
    uint64_t latency_min_ns{0};
    uint64_t latency_max_ns{0};
};

/// Listen on TCP port 9090, accept one connection, then loop:
///   - read a JSON command line from the driver
///   - dispatch to the appropriate handler
///   - write a JSON response line back
///
/// Commands:
///   {"cmd":"run","spans":N,"rate_hz":R}   → runs workload, replies with RunResult JSON
///   {"cmd":"quit"}                         → replies {"ok":true} and returns
///
/// Blocks until "quit" is received or the connection is closed.
void RunControlLoop(int port, class IBackend& backend, Histogram& latency_hist);

/// Serialize a RunResult to a single-line JSON string (no trailing newline).
[[nodiscard]] std::string SerializeRunResult(const RunResult& r);

}  // namespace bench
