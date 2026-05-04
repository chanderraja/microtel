# microtel

> A lightweight OpenTelemetry-compatible trace runtime and OTLP exporter built on nghttp2.

[![status](https://img.shields.io/badge/status-pre--1.0--draft-orange)](microtel-spec.md)
[![license](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
![build](https://img.shields.io/badge/build-not%20yet-lightgrey)
![tests](https://img.shields.io/badge/tests-not%20yet-lightgrey)

> ⚠️ **Pre-1.0 development.** This project is currently in the architecture and design phase (milestone M0). The v1 specification is locked; implementation has not yet begun. **No working binaries or wheels exist yet.** See the [roadmap](microtel-roadmap.md) for the path to v1.0.
>
> 🚫 **Not accepting external implementation PRs until v1.0 ships.** The internal architecture is still being established and accepting outside contributions now would slow the design phase. **Issues, spec feedback, and architecture discussion are welcome and valuable** — see [CONTRIBUTING.md](CONTRIBUTING.md) for what's actionable. This restriction lifts when v1.0 ships.

---

## What is microtel?

microtel is a small, focused **trace runtime and OTLP exporter** for C++ applications that need to participate in OpenTelemetry without paying the cost of the full `opentelemetry-cpp` dependency closure.

It speaks both **OTLP/HTTP-protobuf** and **OTLP/gRPC** on the wire — but doesn't link the gRPC library. The gRPC path is implemented as a small unary-RPC protocol layer over the same HTTP/2 transport (`nghttp2`) used for OTLP/HTTP, so the runtime closure is the same regardless of which protocol you pick.

**Target dependency closure for the v1 core:**

- nghttp2
- OpenSSL
- upb (vendored, pinned)
- zlib
- spdlog (optional, header-only)

**Not in the closure:** gRPC, abseil, c-ares, re2, protobuf-cpp runtime, Bazel.

## Why it exists

`opentelemetry-cpp` paired with the OTLP/gRPC exporter pulls in multi-MB of transitive dependencies (gRPC, abseil, c-ares, re2, protobuf, OpenSSL, zlib) and adds significant build time. For embedded, edge, air-gapped, and CNF deployments, that closure can be a hard adoption blocker.

The architectural insight: gRPC at the wire level is a thin protocol on top of HTTP/2 — a 5-byte length-prefix per message, a handful of specific headers, and an HTTP/2 trailer carrying `grpc-status`. The complexity that makes the gRPC *library* heavy lives almost entirely in features OTLP doesn't use. Implementing only the gRPC wire protocol on top of `nghttp2` keeps wire compatibility while shedding the entire gRPC library closure.

microtel implements the parts of OTLP that real applications actually use, with a much smaller closure and lower runtime overhead, while staying wire-compatible with conformant OTLP receivers.

## v1 scope (planned)

**v1 ships with:**

- Trace SDK (`Tracer`, `Span`, W3C Context propagation, basic samplers, batch processor)
- OTLP/HTTP-protobuf exporter
- OTLP/gRPC exporter (nghttp2-native, no gRPC library)
- Production correctness — partial-success parsing, retry/backoff, GOAWAY/RST_STREAM handling, deterministic shutdown, fork-safety
- Static config + OTel env-var fallback
- TLS, mTLS, proxy, basic auth (static + callback)
- Python bindings (traces only)
- Experimental compatibility shims for `opentelemetry-cpp` and `opentelemetry-python`

**v1 does NOT include:**

- Metrics — coming in v1.2 after a dedicated design phase
- Logs — coming in v1.3
- Control plane / hot reload — v1.1
- Sugar APIs — v1.1
- Auto-instrumentation — v1.4
- Windows support — out of scope for now

See [microtel-roadmap.md](microtel-roadmap.md) for the full multi-year picture from v1.0 through v3.0 (full SDK conformance) and the v2.0 leaf/concentrator architecture for embedded fleets.

## API preview

API surface from the v1 spec; not yet runnable.

```cpp
#include <microtel/tracer.hpp>

auto provider = microtel::SdkBuilder()
    .WithResource({{"service.name", "my-service"}, {"service.version", "1.2.3"}})
    .WithEndpoint("https://collector.internal:4317")
    .WithProtocol(microtel::Protocol::Grpc)
    .Build();

auto tracer = provider->GetTracer("my.component");
{
    auto span = tracer->StartSpan("handle_request");
    span->SetAttribute("http.route", "/users/:id");
    // ... work ...
}  // RAII close

provider->ForceFlush(std::chrono::seconds(5));
provider->Shutdown(std::chrono::seconds(5));
```

## Documentation

- **[microtel-spec.md](microtel-spec.md)** — the v1 specification. Source of truth.
- **[microtel-roadmap.md](microtel-roadmap.md)** — multi-year roadmap from v1.0 to v3.0.
- **[CLAUDE.md](CLAUDE.md)** — durable rules for AI coding agents (and humans) working on this project.
- **[docs/](docs/)** — architecture, threading model, memory model, error model, interfaces, coding standards, sequence diagrams.
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — contribution process.
- **[SECURITY.md](SECURITY.md)** — vulnerability disclosure policy.

## Build

Build instructions will be locked in by milestone M2. Until then, this section is a placeholder.

```bash
cmake --preset Release
cmake --build build/Release
ctest --test-dir build/Release
```

## License

Apache 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

The choice of Apache 2.0 over MIT is deliberate — it aligns with the OpenTelemetry ecosystem, provides explicit patent grant and retaliation clauses for protocol-implementing code, and reduces friction for enterprise legal review.

## Status

Current phase: **M0 — Architecture & Design**.

Track progress in [microtel-spec.md §13](microtel-spec.md#13-roadmap) and the [project roadmap](microtel-roadmap.md). Issues and pull requests are welcome, but the project structure is still being established — see [CONTRIBUTING.md](CONTRIBUTING.md) for what's actionable now.
