# proto/ — Vendored OpenTelemetry protocol definitions

This directory contains a pinned copy of the [`opentelemetry-proto`][upstream]
schema used by microtel's OTLP encoder. Per spec §9.6 the wire definitions are
vendored — not fetched at configure time — so a clone of microtel always
contains everything needed to regenerate the encoder.

[upstream]: https://github.com/open-telemetry/opentelemetry-proto

## Pin

| Field            | Value                                      |
|------------------|--------------------------------------------|
| Upstream tag     | `v1.10.0`                                  |
| Upstream commit  | `ca839c51f706f5d53bfb46f06c3e90c3af3a52c6` |
| License          | Apache-2.0 (see `LICENSE`)                 |
| Last refreshed   | 2026-05-05                                 |

## What's vendored (and what's not)

We vendor only the `.proto` files microtel encodes against. The **trace** files
(the v1 signal) encode an `ExportTraceServiceRequest`:

```
opentelemetry/proto/common/v1/common.proto
opentelemetry/proto/resource/v1/resource.proto
opentelemetry/proto/trace/v1/trace.proto
opentelemetry/proto/collector/trace/v1/trace_service.proto
```

The **metrics** files are vendored ahead of the M12 metrics implementation
(v1.2; see `docs/metrics-design.md` §10):

```
opentelemetry/proto/metrics/v1/metrics.proto
opentelemetry/proto/collector/metrics/v1/metrics_service.proto
```

> **Not yet wired into codegen.** `gen/` and `ci/scripts/regen-protos.sh` still
> generate only the trace accessors, so `regen-check` stays green. The follow-up
> M12 codegen PR adds these two files to the regen list and commits the
> generated `*.upb.*` accessors (it needs the protoc v29.4 + upb-plugin
> toolchain documented in `gen/README.md`).

`logs/` and `profiles/` from upstream remain intentionally excluded; they land
when those signals land in microtel (roadmap v1.3+).

## Refreshing the pin

The pin is refreshed by replacing the vendored files above wholesale from a
matching upstream tag — never patched in-place. To bump:

1. `git -C /tmp clone --branch <new-tag> https://github.com/open-telemetry/opentelemetry-proto.git`
2. Copy the vendored files above into this directory, preserving paths.
3. Update the table in this README (tag, commit, refresh date).
4. Refresh `LICENSE` from the upstream tree.
5. Regenerate the upb-generated accessors via `ci/scripts/regen-protos.sh`
   (see `gen/README.md` for the toolchain).
6. Run the full test suite — wire tests will catch any incompatible field
   number changes immediately.

A bump that touches `trace.proto` (or, once wired, `metrics.proto`) semantics
goes through the ICP process (`docs/icps/`), since it changes the on-the-wire
contract microtel guarantees.

## Why a directory and not a submodule

A vendored copy has no clone-time fetch, no submodule init dance for users,
and a clear blast radius for security review (just diff this directory).
The footprint is ~75 KB total — well below the threshold where a submodule
would be worth the operational cost.
