# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Benchmark harness build — included when MICROTEL_BUILD_BENCH=ON.
# Requires Docker (or Podman) on the host; does not link into the main
# microtel library. See docs/bench-spec.md for the full spec.
#
# B0 scope: driver scaffolding, env fingerprint, blackhole sink,
# microtel + otel-cpp-gRPC SUTs, hot-loop-traces profile, JSON output.
# Later milestones (B1+) add remaining profiles, Markdown report, etc.

cmake_minimum_required(VERSION 3.20)

if(MICROTEL_BUILD_BENCH)
    add_subdirectory(emit-app)
endif()
