# M1 Spike — throwaway code

**Status:** active during M1.
**Lifetime:** deleted at the end of M1, or merged into an ICP record if findings are durable. Never moved into `src/`.

## What this is

Per `microtel-spec.md` §13 and `docs/development.md`, M1 (the spike) validates M0's architecture decisions with **throwaway code** before M2 builds the real project skeleton.

The spike's job is to answer:

1. **Does the M0 architecture build?** Concretely: can an nghttp2 client running on this codebase's intended dependency closure (nghttp2 + OpenSSL + zlib + upb) speak OTLP/HTTP-protobuf to a real OpenTelemetry Collector?
2. **Does the gRPC-on-nghttp2 idea work in practice?** Can the same client speak OTLP/gRPC by adding the 5-byte length-prefix framing, the right HTTP/2 headers, and trailer parsing — without linking the gRPC library?
3. **What surfaces in the locked interfaces of `docs/interfaces.md` were wrong?** Anything the spike reveals as wrong is filed as an ICP. The spike code itself is then thrown away.

## What this is NOT

- Not the start of `src/`. Implementation begins in M3 against locked, mocked interfaces.
- Not held to TDD discipline. M1 is research; M3 is implementation.
- Not held to the test-presence CI gate. The header check still applies; this directory is excluded.
- Not a public API. Nothing here is shipped, ABI-stable, or documented for users.

## Layout

```
spike/
├── README.md                       # this file
├── CMakeLists.txt                  # builds the spike binaries (separate target tree)
├── fixtures/                       # shared protoc-encoded payloads (M1 step 1)
│   ├── README.md
│   ├── single_span.textproto       # human-readable source
│   ├── single_span.bin             # encoded ExportTraceServiceRequest
│   └── regenerate.sh               # one-command regen against the pinned schema
├── http_post/                      # OTLP/HTTP-protobuf, single span, end-to-end
│   └── main.cpp
├── grpc_unary/                     # OTLP/gRPC, same shape, with framing + trailer
│   └── main.cpp
└── docker/
    ├── otel-collector.yml          # docker-compose with otel/opentelemetry-collector
    ├── otel-collector-config.yaml  # collector config (TLS on; debug exporter)
    └── generate_certs.sh           # per-author self-signed root CA + server cert
```

## How to run

Prerequisites: `nghttp2`, `openssl`, and `protobuf-compiler` development packages installed; `docker` (or `podman`) for the local collector.

```bash
# 0a. One-time per author: generate self-signed certs for the collector.
#     Certs are gitignored; each author regenerates locally.
spike/docker/generate_certs.sh

# 0b. Optional: regenerate the protoc-encoded fixture if you bumped the
#     pinned opentelemetry-proto tag or edited single_span.textproto.
#     The committed single_span.bin is the contract; this is the regen recipe.
spike/fixtures/regenerate.sh

# 1. Spin up a local collector listening on 4317 (gRPC) and 4318 (HTTP).
docker compose -f spike/docker/otel-collector.yml up -d
# (or: podman compose -f spike/docker/otel-collector.yml up -d)

# 2. Configure and build.
cmake -S . -B build/Spike -DMICROTEL_BUILD_SPIKE=ON
cmake --build build/Spike --target spike_http_post spike_grpc_unary

# 3. Run. Each binary takes the collector endpoint and the trust-anchor cert.
./build/Spike/spike/spike_http_post  https://localhost:4318 spike/docker/certs/ca.crt
./build/Spike/spike/spike_grpc_unary https://localhost:4317 spike/docker/certs/ca.crt

# 4. Tear down.
docker compose -f spike/docker/otel-collector.yml down
```

Each binary prints either `OK: span accepted` or a diagnostic explaining why the request failed. The collector's stdout shows the received span (verbose `debug` exporter is wired in `spike/docker/otel-collector-config.yaml`).

## Per-author findings ledger

Keep a running scratch file `spike/findings.md` (gitignored) as you discover things during the spike. Promote each item to a formal ICP under `docs/icps/` as you go — do not batch them up for the end of M1, when the small details have already faded. The ledger is private to each author; the ICPs are the durable record.

## What we learn from each binary

### `http_post/`

- DNS + TCP + TLS + ALPN `h2` + nghttp2 SETTINGS exchange — does the chain work end-to-end with our intended dependency closure?
- Building the HTTP/2 HEADERS frame (`:method`, `:path`, `:scheme`, `:authority`, `content-type`, `user-agent`) by hand against nghttp2's API.
- Submitting one DATA frame containing the protobuf body (no framing — OTLP/HTTP doesn't use the gRPC 5-byte prefix).
- Receiving the response, distinguishing 2xx / partial-success / non-retryable / retryable.

The fixture is hand-encoded by `protoc --encode` so this binary doesn't depend on upb yet — keeps the spike's surface as small as possible. upb integration lands in M3.

### `grpc_unary/`

- Same connection setup as above on port 4317.
- Adding the gRPC-specific HTTP/2 headers (`te: trailers`, `content-type: application/grpc+proto`, `:path: /opentelemetry.proto.collector.trace.v1.TraceService/Export`).
- Building the **5-byte length-prefixed message** in the DATA frame.
- **Reading the trailer HEADERS frame** for `grpc-status`, including the trailer-only response variant.
- Confirming that nghttp2's callback model surfaces all of the above cleanly without needing the gRPC library.

This is the project's load-bearing claim. If this binary doesn't work, the spec needs revisiting.

## Findings flow back as ICPs

Anything the spike surfaces about M0's locked interfaces becomes an ICP under `docs/icps/`. Examples of what could come back:

- `ITransport::Send` should hand the codec a per-stream object instead of a `RequestHandle` token.
- The `WireResult::response_excerpt` cap is too small for real-world error messages.
- `IReactor`'s callback contract needs an explicit "from-callback" reentrancy rule.

The spike code itself is **deleted** at the end of M1. The interface contracts are the durable record.

## End of M1

When the spike has answered its three questions above:

1. Open ICPs for any locked-interface deltas. Land them.
2. Update `docs/interfaces.md` per the ICPs.
3. Re-run the M0 header check; ensure the contracts still compile clean.
4. Tag `v0.1.1-m1` (or similar — milestone tag policy decided when M1 is being closed).
5. Delete `spike/`. The git history preserves it; nobody depends on it going forward.
6. M2 begins.
