# Spike fixtures

Hand-written ExportTraceServiceRequest payloads for the M1 spike binaries.
Encoded once, committed as binary blobs, never regenerated dynamically at run
time.

## Why hand-written, not library-generated

Per `spike/README.md`, the spike's job is to validate that nghttp2 + a real
OpenTelemetry Collector is buildable. Using another library to generate the
fixture would introduce a dependency on that library's correctness — exactly
the variable the spike is trying to isolate. `protoc --encode` against a
hand-written textproto is the smallest tool that produces a known-good
payload.

## Files

| File | Role |
|---|---|
| [`single_span.textproto`](single_span.textproto) | Human-readable source. Edit this. |
| [`single_span.bin`](single_span.bin) | Encoded `ExportTraceServiceRequest`. Both spike binaries read this. |
| [`regenerate.sh`](regenerate.sh) | One-command regen against the pinned `opentelemetry-proto` tag. |

## Pinned schema

`opentelemetry-proto` **v1.10.0** (per `regenerate.sh`'s `OTEL_PROTO_TAG`).
The committed `.bin` is the contract between the spike binaries and the
collector; the textproto and the script are the regen recipe. Bump the pin
when the spike needs newer message fields.

## Regenerating

```bash
# protoc is required on PATH (Fedora: `dnf install protobuf-compiler`).
spike/fixtures/regenerate.sh
```

The script fetches the pinned `opentelemetry-proto` tag into a tmp directory,
runs `protoc --encode`, and round-trips through `protoc --decode` to confirm
the bytes are well-formed. Aborts on any failure.

The textproto values (timestamps, trace_id, span_id) are **deliberately
fixed** so the encoded fixture is byte-stable across regeneration — the
proto3 wire format is deterministic, so the same input always produces the
same bytes regardless of `protoc` version. Verified byte-identical against
both `protoc 3.19.6` (Fedora package) and `protoc 34.1` (upstream release)
during M1 setup.

If you bump the pin and the resulting `.bin` is byte-different, that is
expected — commit both the textproto and the new binary together. CI does
not enforce byte-stability across pin bumps.

## Throwaway

This whole directory is deleted at the end of M1. The spike binaries that
read these fixtures are also deleted. The wire-protocol contract that the
fixtures exercise lives in `docs/grpc-wire-protocol.md` and the pinned proto
schemas land in `proto/` during M2 (vendored properly per spec §9.6).
