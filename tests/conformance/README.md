# `tests/conformance/`

End-to-end against a real OpenTelemetry Collector. Validates wire-
protocol compliance per spec §2.2 Tier 1.

## What this proves

Tier 1 of the four-tier compatibility model in
`microtel-spec.md` §2.2: payloads emitted by microtel are accepted by
receivers implementing the pinned OTLP specification version, and
microtel correctly parses success / failure / retryable failure /
partial-success responses.

This is the gate for the v1.0 release per spec §13.5:

> OTLP/HTTP trace export passes integration tests against the pinned
> OpenTelemetry Collector matrix.
> OTLP/gRPC trace export passes integration tests against the pinned
> OpenTelemetry Collector matrix.

## Suggested subdirectories

| Subdirectory | Theme |
|---|---|
| `http/` | OTLP/HTTP-protobuf against the collector. |
| `grpc/` | OTLP/gRPC against the collector. |

## Pinned versions

The collector container, the pinned `opentelemetry-proto` version, and
any backend versions are tracked in
[`docs/interop-matrix.md`](../../docs/interop-matrix.md).

## CI

Conformance tests run on every PR per spec §14.2. They require a
container runtime in CI (Docker on GitHub Actions, podman locally).
The collector starts as a service container; tests connect to it over
TLS using a self-signed cert generated at job start.
