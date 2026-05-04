// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// M1 spike — OTLP/HTTP-protobuf single-span end-to-end.
//
// THROWAWAY CODE. Deleted at the end of M1. See spike/README.md.
//
// What this binary should do (M1 author fills in):
//
//   1. Parse argv[1] as the collector endpoint URL (e.g., https://localhost:4318).
//   2. Open a TCP socket, drive an OpenSSL handshake with ALPN h2.
//   3. Hand the SSL session to nghttp2 via the standard read/write callbacks.
//   4. Build a HEADERS frame:
//        :method  POST
//        :scheme  https
//        :path    /v1/traces
//        :authority <host:port>
//        content-type   application/x-protobuf
//        user-agent     microtel-cpp-spike/0.1
//   5. Submit one DATA frame whose body is the protoc-encoded
//      ExportTraceServiceRequest at MICROTEL_SPIKE_FIXTURE.
//   6. Drive the loop until response headers + body arrive.
//   7. Print the response status and (if 2xx with a body) decode partial-success.
//
// Findings flow back as ICPs against docs/interfaces.md §4.1 (ITransport)
// and docs/grpc-wire-protocol.md.

#include <cstdio>

int main(int argc, char* argv[])
{
    (void) argc;
    (void) argv;
    std::fprintf(stderr,
        "spike_http_post: not yet implemented (M1 deliverable).\n"
        "See spike/README.md and spike/http_post/main.cpp for the work plan.\n");
    return 0;
}
