# microtel

> A lightweight OpenTelemetry-compatible trace runtime and OTLP exporter built on nghttp2.
> Metrics and logs are implemented too, but experimental — not conformance-tested ([status](#status)).

[![license](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
[![CI](https://github.com/chanderraja/microtel/actions/workflows/ci.yml/badge.svg)](https://github.com/chanderraja/microtel/actions)

---

## What is microtel?

microtel is a small, focused **OTel SDK and OTLP exporter** for C++ applications that need to participate in OpenTelemetry without paying the cost of the full `opentelemetry-cpp` dependency closure.

It speaks both **OTLP/HTTP-protobuf** and **OTLP/gRPC** on the wire — without linking the gRPC library. The gRPC path is a thin unary-RPC protocol layer over the same `nghttp2` transport used for OTLP/HTTP, so the binary cost is the same regardless of which protocol you pick. Being HTTP/2-only has one consequence worth knowing before you pick: plaintext OTLP/HTTP cannot reach an HTTP/1.1-only receiver such as a stock collector's `:4318` — use `https://`, or OTLP/gRPC ([compatibility matrix](docs/compatibility-matrix.md)).

**Runtime dependency closure:**

| Dependency | Purpose |
|---|---|
| nghttp2 | HTTP/2 transport (both OTLP protocols) |
| OpenSSL | TLS, mTLS |
| upb (vendored) | OTLP protobuf encoding |
| zlib | gzip compression |
| spdlog (optional) | internal log sink |

Not in the closure: gRPC, abseil, c-ares, re2, protobuf-cpp runtime, Bazel.

## Why it exists

`opentelemetry-cpp` with the OTLP/gRPC exporter pulls in multi-MB of transitive dependencies and adds significant build time. For edge, embedded, air-gapped, and CNF deployments, that closure is often a hard adoption blocker.

gRPC at the wire level is a thin protocol on top of HTTP/2 — a 5-byte length-prefix, a handful of specific headers, and an HTTP/2 trailer carrying `grpc-status`. The complexity of the gRPC *library* lives almost entirely in features OTLP doesn't use. Implementing only the wire protocol on top of `nghttp2` keeps wire compatibility while shedding the entire gRPC library closure.

## Status

**v1.0 — traces.**

The release cut and the implementation are not the same line. v1.0 is scoped to
traces: that is the signal the spec's conformance gates, the compatibility
matrix and the support promise cover. Metrics and logs are *implemented* — the
code is in the tree and tested — but they are claimed by later releases, and
until then they carry no compatibility guarantee. `microtel-spec.md` §13 puts it
directly: *"do not read 'v1 is traces only' as a statement about what is
implemented — that phrasing described the release cut, and the implementation
has run ahead of it."*

| Signal | Status |
|---|---|
| Traces | **v1.0** ✅ — Tracer, Span, W3C propagation (traceparent and tracestate), batch processor, OTLP/gRPC + OTLP/HTTP (the latter over TLS; see the [compatibility matrix](docs/compatibility-matrix.md)) |
| Metrics | Implemented ahead of the release cut; claimed in **v1.2** (spec §13) — all 7 instruments, OTLP encoder, periodic reader, cardinality limits, temporality, views, exemplars |
| Logs | Implemented ahead of the release cut; claimed in **v1.3** (spec §13) |

What "claimed in v1.2/v1.3" buys you when it lands: the compatibility-tier
promise in [microtel-roadmap.md](microtel-roadmap.md) §3, conformance coverage,
and the security-support matrix in [SECURITY.md](SECURITY.md). Using the metrics
or logs API before then works; it is just not yet something v1.0 promises not to
break.

## Benchmarks

Hot-loop traces, 10 000 spans/sample × 10 samples, blackhole sink (no network), Podman containers, AMD Ryzen 5 5600G. CPU governor was `powersave` and SMT was on; results with `performance` and SMT off will be lower-variance. Every number below is read off the committed snapshot linked under the table — not a separate run.

| Metric | **microtel** (HTTP) | **microtel** (gRPC) | otelcpp (gRPC) | otelcpp (HTTP) |
|---|---|---|---|---|
| StartSpan p50 | **192 ns** | **192 ns** | 768 ns | 768 ns |
| StartSpan p95 | **384 ns** | **384 ns** | 3 072 ns | 3 072 ns |
| Spans / sec | **1 528 105** | 1 286 592 | 742 689 | 799 785 |
| Flush p50 | 3.0 ms | 4.3 ms | 2.0 ms | 1.9 ms |
| Delivery rate | **100%** | **100%** | 94.0% | 96.6% |
| Wire bytes / span | **62.2** | **62.2** | 68.1 | 68.1 |
| Binary size | **14.2 MB** | **14.2 MB** | 38.5 MB | 16.8 MB |

microtel's StartSpan is **4× faster** than otelcpp, throughput is **~2×** higher, delivery is **100%** (otelcpp drops up to 6% under load), and the binary is **2.7× smaller** than otelcpp-gRPC. (The binary grew 11.5 → 14.2 MB in the v1.0 release round — W3C propagation, response decompression, memory-limit enforcement and GOAWAY handling are new code.)

Full results with interactive plots: [`docs/bench-results/plots.html`](docs/bench-results/plots.html) — a committed snapshot of one run, with its environment, warnings and raw per-sample data ([`results.md`](docs/bench-results/results.md), [`results.json`](docs/bench-results/results.json), [provenance](docs/bench-results/README.md)). `bench/results/` is where a local `./bench.sh` writes, and it is gitignored; the snapshot exists so these numbers are checkable from a clone. Methodology: [`docs/bench-spec.md`](docs/bench-spec.md).

## API

### Traces

```cpp
#include <microtel/tracer.hpp>

auto provider = microtel::SdkBuilder{}
    .WithResource({{"service.name", "my-service"}, {"service.version", "1.2.3"}})
    .WithEndpoint("https://collector.internal:4317")
    .WithProtocol(microtel::Protocol::Grpc)
    .Build();

auto tracer = provider->GetTracer("my.component");
{
    auto span = tracer->StartSpan("handle_request");
    span->SetAttribute("http.route", "/users/:id");
    // ... work ...
}  // RAII close — span exported on scope exit

provider->ForceFlush(std::chrono::seconds(5));
provider->Shutdown(std::chrono::seconds(5));
```

### Metrics

```cpp
#include <microtel/meter.hpp>

auto provider = microtel::SdkBuilder{}
    .WithResource({{"service.name", "my-service"}})
    .WithEndpoint("https://collector.internal:4317")
    .WithPeriodicMetricReader(std::chrono::seconds(60))
    .WithMetricLimits({.max_cardinality = 2000})
    .Build();

auto meter = provider->GetMeter("my.component");
auto requests = meter->CreateCounter<std::int64_t>("requests.total");
auto latency  = meter->CreateHistogram<double>("request.duration", "", "ms");

// hot path — noexcept, no allocation in steady state
requests->Add(1, {{"http.method", "GET"}, {"http.status_code", 200}});
latency->Record(4.2, {{"http.route", "/users/:id"}});
```

## Build

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

**Sanitizer builds:**

```bash
cmake -S . -B build-asan -DMICROTEL_SANITIZER=asan -DMICROTEL_BUILD_TESTS=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan
```

Replace `asan` with `tsan` or `ubsan` as needed.

## Install and consume

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=OFF
cmake --build build -j$(nproc)
cmake --install build --prefix /opt/microtel
```

Then, from another project:

```cmake
find_package(microtel REQUIRED)
target_link_libraries(my_app PRIVATE microtel::microtel)
```

`microtel::microtel` is the only supported target name. The per-layer
components (`microtel::sdk`, `microtel::transport`, …) are exported because a
static link closure needs them, but they are **not** a supported API and may
merge, split or disappear without notice — see
[ICP 0020](docs/icps/0020-install-and-package-config.md) Decision 2. Linking
the aggregate also means CMake derives the link order of the fourteen static
archives for you.

If the prefix is not on the default search path, point cmake at it:
`-DCMAKE_PREFIX_PATH=/opt/microtel`.

**A consumer needs zlib, OpenSSL and libnghttp2 at link time.** They reach
microtel through PRIVATE link interfaces, so they add no include paths or
definitions to your build, but the static archives carry undefined references
to them. `find_package(microtel)` resolves all three itself — nghttp2 through
pkg-config, so `pkg-config` and libnghttp2's `.pc` file must be installed.

**Benchmarks** (requires Podman):

```bash
cd bench && ./bench.sh                        # hot-loop-traces profile
./bench.sh --profile hot-loop-metrics        # metrics hot path
./bench.sh --flamegraph                      # + per-SUT SVG flame graphs
```

## Documentation

- **[microtel-spec.md](microtel-spec.md)** — v1 specification. Source of truth.
- **[microtel-roadmap.md](microtel-roadmap.md)** — roadmap v1.0 → v3.0.
- **[docs/metrics-design.md](docs/metrics-design.md)** — metrics design decisions (M11, signed off).
- **[docs/interfaces.md](docs/interfaces.md)** — locked internal interface contracts.
- **[docs/auth-callback-recipes.md](docs/auth-callback-recipes.md)** — OAuth2 and AWS SigV4 over `WithAuthProvider`, and what the callback cannot do.
- **[docs/](docs/)** — architecture, threading model, memory model, error model, coding standards, sequence diagrams.
- **[CLAUDE.md](CLAUDE.md)** — rules for AI coding agents (and humans) working on this project.
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — contribution process.
- **[SECURITY.md](SECURITY.md)** — vulnerability disclosure policy.
- **[RELEASING.md](RELEASING.md)** — release procedure: the version bump, tagging, and snapshot refreshes.

## License

Apache 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Apache 2.0 aligns with the OpenTelemetry ecosystem, provides an explicit patent grant and retaliation clauses for protocol-implementing code, and reduces friction for enterprise legal review.
