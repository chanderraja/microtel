# microtel

An OpenTelemetry-compatible trace runtime and OTLP exporter for C++20. It
speaks OTLP/gRPC and OTLP/HTTP-protobuf without linking gRPC, abseil or the
protobuf C++ runtime.

[![license](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
[![CI](https://github.com/chanderraja/microtel/actions/workflows/ci.yml/badge.svg)](https://github.com/chanderraja/microtel/actions)

microtel covers the part of the OpenTelemetry tracing SDK that applications
use day to day: tracers, spans, context, W3C propagation, samplers, a batch
processor and resources, plus an OTLP exporter. Both OTLP protocols run over a
single nghttp2 HTTP/2 transport. The gRPC side is a small unary-RPC layer on
that transport, so picking gRPC over HTTP costs nothing extra in binary size.

## Why it exists

`opentelemetry-cpp` with the OTLP/gRPC exporter brings in gRPC, abseil,
protobuf, c-ares and re2, which is megabytes of transitive dependencies and a
long build. For edge, embedded, air-gapped and CNF deployments that is often
enough to rule it out.

On the wire, gRPC is a thin layer over HTTP/2: a 5-byte length prefix, a few
headers, and a trailer carrying `grpc-status`. OTLP needs nothing else from
the gRPC library, so microtel implements just the wire protocol.

The runtime dependency closure is:

| Dependency | Purpose |
|---|---|
| nghttp2 | HTTP/2 transport for both OTLP protocols |
| OpenSSL | TLS and mTLS |
| zlib | gzip request compression and response decompression |
| upb (vendored, symbols renamed to `microtel_upb_*`) | OTLP protobuf encoding |
| spdlog (optional) | internal diagnostic log sink |

A CI symbol scan fails any PR whose shipped archives define or reference an
`absl::`, `grpc` or `google::protobuf::` symbol. Adding a runtime dependency
requires an [ICP](docs/icps/).

## Status

The current release is **v1.1.0**, and the project follows SemVer.
[SECURITY.md](SECURITY.md) lists which versions get fixes.

| Area | Status |
|---|---|
| Traces | Supported. `Tracer` and `Span`; `StartAsCurrentSpan` with a thread-local context; W3C `traceparent`, `tracestate` and `baggage` inject/extract; head samplers (always on/off, trace-ID ratio, parent-based) and composable rule chains; batch span processor; process and host resource detectors; `HealthSnapshot` drop and queue counters. |
| Operations | Supported since v1.1. Runtime setters on `Provider` (`SetBatchOptions`, `SetSamplerRatio`, `SetMetricInterval`, `SetLogLevel`); several named providers in one process via `GetProvider(name)`; static headers and a `WithAuthProvider` callback; TLS, custom CA and mTLS; gzip; the `microtel::sugar` convenience layer. |
| Metrics | Implemented but experimental: all seven instruments, periodic reader, temporality, cardinality limits, views and exemplars. Scheduled to become supported in v1.2. Until then there is no compatibility guarantee and no conformance coverage. |
| Logs | Implemented but experimental. Scheduled for v1.3, with the same caveat. |
| opentelemetry-cpp API shim | Experimental, source-only and off by default. Routes existing `opentelemetry-cpp` API call sites to microtel; see [migration-from-otel-cpp.md](docs/migration-from-otel-cpp.md). |

Not supported: plaintext OTLP/HTTP to an HTTP/1.1-only receiver (see
[Protocols and endpoints](#protocols-and-endpoints)), HTTP proxies, TLS below
1.2, and Windows. The [compatibility matrix](docs/compatibility-matrix.md) has
the full list, and [microtel-roadmap.md](microtel-roadmap.md) has what's next.

## Getting started

microtel runs on Linux only (the I/O thread uses `epoll`); CI builds it as
C++20 and C++23 with GCC 13 and Clang 18 on Ubuntu x86_64. You need a C++20
compiler, CMake 3.20 or newer, `pkg-config`, and the OpenSSL, nghttp2 and zlib
development packages:

```bash
# Debian / Ubuntu
sudo apt-get install -y cmake ninja-build pkg-config libssl-dev libnghttp2-dev zlib1g-dev
# Fedora / RHEL
sudo dnf install -y cmake ninja-build pkgconf-pkg-config openssl-devel libnghttp2-devel zlib-devel
```

Configuring fetches toml++ (header-only), spdlog (unless
`-DMICROTEL_USE_SPDLOG=OFF`) and, when tests are enabled, GoogleTest. None of
them end up in the installed package's link closure. Build and install:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMICROTEL_BUILD_TESTS=OFF
cmake --build build -j"$(nproc)"
cmake --install build --prefix /opt/microtel
```

That installs the headers, the static archives, the CMake package and the
`microtel-preflight` tool. Consume it with `find_package`, adding
`-DCMAKE_PREFIX_PATH=/opt/microtel` if the prefix isn't on CMake's search path:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(microtel 1.1 REQUIRED CONFIG)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE microtel::microtel)
```

Link against `microtel::microtel` only. The per-layer targets
(`microtel::sdk`, `microtel::transport` and so on) are exported because a
static link needs them, but they can change without notice
([ICP 0020](docs/icps/0020-install-and-package-config.md)). Your link line
also needs zlib, OpenSSL and libnghttp2; `find_package(microtel)` locates
them (nghttp2 through pkg-config) as private dependencies, so they add no
include paths or definitions to your targets.

A minimal program that sends one span to a local collector:

```cpp
#include <microtel/provider.hpp>
#include <microtel/sdk_builder.hpp>
#include <microtel/span.hpp>
#include <microtel/tracer.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main()
{
    // Build() returns microtel::Expected<std::shared_ptr<Provider>, ConfigError>.
    auto built = microtel::SdkBuilder{}
                     .WithEndpoint("http://localhost:4317")  // OTLP/gRPC, plaintext h2c
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("checkout")
                     .WithServiceVersion("1.4.2")
                     .Build();
    if (!built)
    {
        std::cerr << "microtel: " << built.error().message << '\n';
        return 1;
    }
    const auto provider = *built;
    const auto tracer = provider->GetTracer("checkout.http", "1.4.2");

    {
        const auto span = tracer->StartSpan("GET /cart");
        span->SetAttribute("http.request.method", std::string{"GET"});
        span->SetAttribute("http.response.status_code", std::int64_t{200});
        span->SetStatus(microtel::StatusCode::Ok);
    }  // The span ends here and the batch processor exports it in the background.

    const microtel::Status flushed = provider->ForceFlush(std::chrono::seconds{5});
    const microtel::Status shut = provider->Shutdown(std::chrono::seconds{5});
    return (flushed == microtel::Status::Completed && shut == microtel::Status::Completed) ? 0 : 2;
}
```

The API does not throw. `StartSpan`, `SetAttribute`, `AddEvent` and `End` are
`noexcept`; on failure they drop the data and count the drop in
`Provider::GetExporterHealth()`. Initialization returns `microtel::Expected`
(`std::expected` on C++23, a vendored `tl::expected` on C++20), and
`ForceFlush`/`Shutdown` return a `microtel::Status` of `Completed`,
`TimedOut`, `AlreadyShutDown` or `Failed`.

Nothing touches the network until the first export. Call
`provider->Connect()` if you want a bad endpoint reported at startup. A span
without an explicit `StartSpanOptions{.parent = ...}` takes its parent from
the current context, which `StartAsCurrentSpan` sets for the lifetime of the
returned scope.

Metrics use the same provider. Attributes are a
`std::span<const microtel::KeyValue>`, and a braced list won't convert to one
in C++20, so keep them in a named container:

```cpp
#include <microtel/meter.hpp>
#include <array>

// SdkBuilder can also take .WithMetricInterval(std::chrono::seconds{60})
// and .WithMetricLimits({.max_cardinality = 2000}).
const auto meter = provider->GetMeter("checkout.http", "1.4.2");
const auto requests = meter->CreateCounter<std::int64_t>("http.server.requests");
const auto latency = meter->CreateHistogram<double>("http.server.duration", "", "ms");

const std::array<microtel::KeyValue, 2> attrs{{
    {.key = "http.request.method", .value = std::string{"GET"}},
    {.key = "http.response.status_code", .value = std::int64_t{200}},
}};
requests->Add(1, attrs);
latency->Record(4.2, attrs);
```

## Examples

[`examples/`](examples/) has eleven runnable programs and a collector, Tempo
and Grafana stack that runs under Docker or Podman:

```bash
examples/stack/up.sh
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON && cmake --build build
./build/examples/microtel_example_basic_trace   # prints the trace ID; view it at http://localhost:3000
```

| Example | Shows |
|---|---|
| [`basic_trace`](examples/basic_trace/) | Provider setup, one request trace, flush, exporter health, shutdown |
| [`context_propagation`](examples/context_propagation/) | `StartAsCurrentSpan` and the thread-local context |
| [`distributed_handoff`](examples/distributed_handoff/) | `traceparent`, `tracestate` and `baggage` across two processes |
| [`sampler_chains`](examples/sampler_chains/) | Composing rule samplers |
| [`sugar_tour`](examples/sugar_tour/) | `MICROTEL_TRACE_FUNCTION`, scoped spans, `Traced`, `RecordException`, `AttrKey` |
| [`resource_detectors`](examples/resource_detectors/) | `process.*` and `host.*` detection and precedence |
| [`multi_profile`](examples/multi_profile/) | Two named providers in one process |
| [`hot_reload`](examples/hot_reload/) | The four runtime setters |
| [`health_and_backpressure`](examples/health_and_backpressure/) | `HealthSnapshot` under load and with the collector down |
| [`auth_bearer`](examples/auth_bearer/) | Static headers and `WithAuthProvider` |
| [`tls`](examples/tls/) | TLS, custom CA, mTLS, and OTLP/HTTP over TLS |

## Protocols and endpoints

| Endpoint | Speaks | Works with a stock OpenTelemetry Collector? |
|---|---|---|
| `grpc://host:4317`, or `http://host:4317` with `Protocol::Grpc` | OTLP/gRPC, plaintext h2c | Yes |
| `grpcs://host:4317`, or `https://host:4317` with `Protocol::Grpc` | OTLP/gRPC over TLS | Yes |
| `https://host:4318` (default protocol) | OTLP/HTTP over TLS (ALPN `h2`) | Yes |
| `http://host:4318` (default protocol) | OTLP/HTTP, plaintext h2c | No: the collector's plaintext `:4318` receiver is HTTP/1.1 only |

The transport only speaks HTTP/2, so plaintext OTLP/HTTP needs a receiver that
accepts h2c. microtel reports the mismatch as a `Build()` warning, a
`Connect()` error, and in `HealthSnapshot::last_error_message`. See section 4
of the [compatibility matrix](docs/compatibility-matrix.md).

## Configuration

Each setting is resolved separately, highest precedence first: code
(`SdkBuilder::With*`), environment variables, a TOML file passed to
`SdkBuilder::FromFile(path)`, then built-in defaults. A value set in code can't
be overridden from the environment. microtel never searches for a config file
on its own. The environment variables you are most likely to need:

| Variable | Sets |
|---|---|
| `OTEL_EXPORTER_OTLP_ENDPOINT` | Collector endpoint (required unless set in code or TOML) |
| `OTEL_EXPORTER_OTLP_PROTOCOL` | `http` (default) or `grpc` |
| `OTEL_EXPORTER_OTLP_HEADERS` | Static headers, `k=v,k=v` |
| `OTEL_EXPORTER_OTLP_COMPRESSION` | `gzip` |
| `OTEL_EXPORTER_OTLP_TIMEOUT` | Per-export timeout in ms |
| `OTEL_EXPORTER_OTLP_CERTIFICATE` | CA bundle path |
| `OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES` | Resource identity |
| `MICROTEL_LOG_LEVEL` | Level of microtel's own diagnostic log |

Some `OTEL_*` variables are ignored, including the per-signal endpoints,
`OTEL_TRACES_SAMPLER` and `OTEL_BSP_*`. [docs/configuration.md](docs/configuration.md)
has every setting with its TOML key and default.

`microtel-preflight --preflight={connect|export} [config.toml]` resolves a
configuration the same way the SDK does and then attempts a real connection
or export, which is a quick way to check a deployment before it goes live.

## Build options

| Option | Default | Effect |
|---|---|---|
| `MICROTEL_BUILD_TESTS` | `ON` | Test tree (fetches GoogleTest). Set `OFF` for install-only or cross builds. |
| `MICROTEL_BUILD_EXAMPLES` | `OFF` | The programs under [`examples/`](examples/). |
| `MICROTEL_BUILD_OTELCPP_SHIM` | `OFF` | Experimental opentelemetry-cpp API shim, source-only ([ICP 0014](docs/icps/0014-otelcpp-shim-and-rule-13.md)). |
| `MICROTEL_USE_SPDLOG` | `ON` | spdlog for internal diagnostics. `OFF` uses a minimal stderr logger. |
| `MICROTEL_FORBID_INSECURE_TLS` | `OFF` | Makes `tls.insecure = true` a `Build()` error instead of a warning. |
| `MICROTEL_SANITIZER` | empty | `asan`, `tsan` or `ubsan`. |

`MICROTEL_BUILD_HEADER_CHECK`, `MICROTEL_BUILD_FUZZ`, `MICROTEL_BUILD_BENCH`
and `MICROTEL_COVERAGE` are for development and CI; see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Performance

Hot-loop traces, 10 000 spans per sample × 10 samples, blackhole sink (no
network), Podman containers on one host (AMD Ryzen 5 5600G, `powersave`
governor, SMT on). The numbers come from the committed snapshot in
[docs/bench-results/](docs/bench-results/README.md).

| Metric | microtel (HTTP) | microtel (gRPC) | otelcpp (gRPC) | otelcpp (HTTP) |
|---|---|---|---|---|
| StartSpan p50 | **229 ns** | **216 ns** | 808 ns | 812 ns |
| StartSpan p95 | **486 ns** | **474 ns** | 3 103 ns | 2 039 ns |
| Spans / sec | **1 466 287** | 1 288 159 | 786 904 | 863 814 |
| Flush p50 | 2.8 ms | 4.0 ms | 1.7 ms | 1.6 ms |
| Delivery rate | **100%** | **100%** | 94.8% | 97.3% |
| Wire bytes / span | **62.2** | **62.2** | 68.1 | 68.1 |
| Benchmark binary size | **15.4 MB** | **15.4 MB** | 38.5 MB | 16.8 MB |

Some figures differ from the v1.0 README because the harness changed; the
snapshot notes explain why. The methodology is in
[docs/bench-spec.md](docs/bench-spec.md), and [plots.html](docs/bench-results/plots.html) has interactive plots and the raw
per-sample data. To reproduce with Docker or Podman:

```bash
cd bench && ./bench.sh              # hot-loop-traces profile
./bench.sh --profile hot-loop-metrics
./bench.sh --flamegraph             # adds per-SUT SVG flame graphs
```

## Documentation

For users: [configuration](docs/configuration.md),
[compatibility matrix](docs/compatibility-matrix.md),
[interop matrix](docs/interop-matrix.md) (tested collectors and backends),
[auth callback recipes](docs/auth-callback-recipes.md) (OAuth2 and AWS SigV4),
[migrating from opentelemetry-cpp](docs/migration-from-otel-cpp.md), and the
[error](docs/error-model.md), [threading](docs/threading-model.md) and
[memory](docs/memory-model.md) models.

For contributors: the [specification](microtel-spec.md), the
[roadmap](microtel-roadmap.md), [architecture](docs/architecture.md), the
locked [interface contracts](docs/interfaces.md),
[metrics design](docs/metrics-design.md), the [ICPs](docs/icps/) that record
design decisions, and the rest of [docs/](docs/) (coding standards, sequence
diagrams).

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first; it covers the TDD gates and
the interface-change process. [docs/development.md](docs/development.md) maps
each source directory to its owner. AI coding agents should also read
[CLAUDE.md](CLAUDE.md). To build and test from a checkout:

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md).
The release procedure is in [RELEASING.md](RELEASING.md).

## License

Apache 2.0, the same as the OpenTelemetry ecosystem. It carries an explicit
patent grant, which matters for protocol-implementing code. See
[LICENSE](LICENSE), [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
