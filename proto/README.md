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

v1 of microtel is **traces-only**. To keep the surface area small we vendor
only the four `.proto` files required to encode an
`ExportTraceServiceRequest`:

```
opentelemetry/proto/common/v1/common.proto
opentelemetry/proto/resource/v1/resource.proto
opentelemetry/proto/trace/v1/trace.proto
opentelemetry/proto/collector/trace/v1/trace_service.proto
```

`logs/`, `metrics/`, and `profiles/` from upstream are intentionally excluded.
They land when the corresponding signal lands in microtel (roadmap v1.1+).

## Refreshing the pin

The pin is refreshed by replacing the four files above wholesale from a
matching upstream tag — never patched in-place. To bump:

1. `git -C /tmp clone --branch <new-tag> https://github.com/open-telemetry/opentelemetry-proto.git`
2. Copy the four files above into this directory, preserving paths.
3. Update the table in this README (tag, commit, refresh date).
4. Refresh `LICENSE` from the upstream tree.
5. Regenerate the upb-generated accessors (M3-F2 will document this command).
6. Run the full test suite — wire tests will catch any incompatible field
   number changes immediately.

A bump that touches `trace.proto` semantics goes through the ICP process
(`docs/icps/`), since it changes the on-the-wire contract microtel
guarantees.

## Why a directory and not a submodule

A vendored copy has no clone-time fetch, no submodule init dance for users,
and a clear blast radius for security review (just diff this directory).
The footprint is ~40 KB total — well below the threshold where a submodule
would be worth the operational cost.
