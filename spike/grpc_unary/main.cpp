// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// M1 spike — OTLP/gRPC unary on nghttp2, no gRPC library.
//
// THROWAWAY CODE. Deleted at the end of M1. See spike/README.md.
//
// What this binary should do (M1 author fills in):
//
//   1. Parse argv[1] as the collector endpoint URL (e.g., https://localhost:4317).
//   2. Same TCP + TLS + ALPN h2 + nghttp2 setup as spike_http_post.
//   3. Build a HEADERS frame:
//        :method  POST
//        :scheme  https
//        :path    /opentelemetry.proto.collector.trace.v1.TraceService/Export
//        :authority <host:port>
//        te                 trailers
//        content-type       application/grpc+proto
//        user-agent         microtel-cpp-spike/0.1
//   4. Submit one DATA frame whose body is:
//        [1 byte CF=0x00][4 bytes BE length][protobuf bytes]
//      where the protobuf bytes are the protoc-encoded
//      ExportTraceServiceRequest at MICROTEL_SPIKE_FIXTURE.
//   5. Drive the loop until both DATA and the trailer HEADERS frame arrive.
//      Specifically test:
//        - Whole-message-in-one-frame happy path.
//        - Trailer-only response (collector under load may emit this).
//        - grpc-status interpretation per docs/grpc-wire-protocol.md §2.4.
//   6. Print the gRPC status, message, and any inline RetryInfo if present.
//
// This binary is the load-bearing claim of the project: gRPC-on-nghttp2
// is feasible without linking the gRPC library. Failure here triggers a
// spec-level revisit, not just an ICP.

#include <cstdio>

int main(int argc, char* argv[])
{
    (void) argc;
    (void) argv;
    std::fprintf(stderr,
        "spike_grpc_unary: not yet implemented (M1 deliverable).\n"
        "See spike/README.md and spike/grpc_unary/main.cpp for the work plan.\n");
    return 0;
}
