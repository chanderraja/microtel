# proto/ — Vendored OpenTelemetry protocol definitions

A pinned copy of the [`opentelemetry-proto`][upstream] schema that microtel's
OTLP encoder is generated from. The definitions are vendored instead of being
fetched at configure time (spec §9.6), so a clone always has everything needed
to regenerate the encoder.

[upstream]: https://github.com/open-telemetry/opentelemetry-proto

## Pin

| Field            | Value                                      |
|------------------|--------------------------------------------|
| Upstream tag     | `v1.10.0`                                  |
| Upstream commit  | `ca839c51f706f5d53bfb46f06c3e90c3af3a52c6` |
| License          | Apache-2.0 (see `LICENSE`)                 |
| Last refreshed   | 2026-05-05                                 |

## What's vendored (and what's not)

Only the `.proto` files microtel encodes against are vendored. The trace files
encode an `ExportTraceServiceRequest`:

```
opentelemetry/proto/common/v1/common.proto
opentelemetry/proto/resource/v1/resource.proto
opentelemetry/proto/trace/v1/trace.proto
opentelemetry/proto/collector/trace/v1/trace_service.proto
```

The metrics files back the metrics implementation, which is experimental
until v1.3 (see `docs/metrics-design.md` §10):

```
opentelemetry/proto/metrics/v1/metrics.proto
opentelemetry/proto/collector/metrics/v1/metrics_service.proto
```

The logs files back the logs implementation, experimental until v1.2:

```
opentelemetry/proto/logs/v1/logs.proto
opentelemetry/proto/collector/logs/v1/logs_service.proto
```

Upstream's `profiles/` is left out on purpose. It comes in when microtel
supports that signal (roadmap v2.0+).

## Refreshing the pin

Refresh the pin by replacing the vendored files wholesale from an upstream
tag. Never patch them in place. To bump:

1. `git -C /tmp clone --branch <new-tag> https://github.com/open-telemetry/opentelemetry-proto.git`
2. Copy the vendored files above into this directory, preserving paths.
3. Update the table in this README (tag, commit, refresh date).
4. Refresh `LICENSE` from the upstream tree.
5. Regenerate the upb-generated accessors via `ci/scripts/regen-protos.sh`
   (see `gen/README.md` for the toolchain).
6. Run the full test suite. The wire tests catch incompatible field number
   changes.

A bump that changes the semantics of `trace.proto` needs an ICP
(`docs/icps/`), because it changes the wire contract microtel guarantees. The
same will apply to `metrics.proto` and `logs.proto` once those signals are
supported.

## Why a directory and not a submodule

A vendored copy needs no fetch or submodule init after cloning, and a
security review only has to diff this directory. The `.proto` files come to
about 75 KB, too small for a submodule to be worth the trouble.
