# microtel

> A lightweight OpenTelemetry-compatible trace and metrics runtime built on nghttp2.

[![license](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
[![CI](https://github.com/chanderraja/microtel/actions/workflows/ci.yml/badge.svg)](https://github.com/chanderraja/microtel/actions)

---

## What is microtel?

microtel is a small, focused **OTel SDK and OTLP exporter** for C++ applications that need to participate in OpenTelemetry without paying the cost of the full `opentelemetry-cpp` dependency closure.

It speaks both **OTLP/HTTP-protobuf** and **OTLP/gRPC** on the wire — without linking the gRPC library. The gRPC path is a thin unary-RPC protocol layer over the same `nghttp2` transport used for OTLP/HTTP, so the binary cost is the same regardless of which protocol you pick.

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

**v1.2 in progress — Traces complete, Metrics SDK substantially implemented.**

| Signal | Status |
|---|---|
| Traces | Complete — Tracer, Span, W3C propagation, batch processor, OTLP/HTTP + OTLP/gRPC |
| Metrics | In progress (v1.2) — all 7 instruments, OTLP encoder, periodic reader, cardinality limits, temporality |
| Logs | Planned v1.3 |

Metrics remaining for v1.2: Views, Exemplars, `mt::Timer` sugar. See [microtel-roadmap.md](microtel-roadmap.md).

## Benchmarks

Hot-loop traces, 10 000 spans/sample × 10 samples, blackhole sink (no network), Podman containers, AMD Ryzen 5 5600G. CPU governor was `powersave`; results with `performance` governor will be lower-variance.

| Metric | **microtel** (HTTP) | **microtel** (gRPC) | otelcpp (gRPC) | otelcpp (HTTP) |
|---|---|---|---|---|
| StartSpan p50 | **192 ns** | **192 ns** | 768 ns | 768 ns |
| StartSpan p95 | **384 ns** | **384 ns** | 3 072 ns | 3 072 ns |
| Spans / sec | **1 494 603** | 1 245 356 | 706 764 | 822 506 |
| Flush p50 | 3.7 ms | 4.9 ms | 2.1 ms | 1.9 ms |
| Delivery rate | **100%** | **100%** | 93.4% | 97.1% |
| Wire bytes / span | **62.2** | **62.2** | 68.1 | 68.1 |
| Binary size | **11.3 MB** | **11.3 MB** | 38.5 MB | 16.8 MB |

microtel's StartSpan is **4× faster** than otelcpp, throughput is **~2×** higher, delivery is **100%** (otelcpp drops up to 7% under load), and the binary is **3.4× smaller** than otelcpp-gRPC.

Full results with interactive plots: [`bench/results/plots.html`](bench/results/plots.html). Methodology: [`docs/bench-spec.md`](docs/bench-spec.md).

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
- **[docs/](docs/)** — architecture, threading model, memory model, error model, coding standards, sequence diagrams.
- **[CLAUDE.md](CLAUDE.md)** — rules for AI coding agents (and humans) working on this project.
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — contribution process.
- **[SECURITY.md](SECURITY.md)** — vulnerability disclosure policy.

## License

Apache 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Apache 2.0 aligns with the OpenTelemetry ecosystem, provides an explicit patent grant and retaliation clauses for protocol-implementing code, and reduces friction for enterprise legal review.
