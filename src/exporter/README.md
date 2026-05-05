# `src/exporter/`

## Purpose

The protocol-agnostic export pipeline. Drains the `BatchSpanProcessor`
queue, builds batches, encodes via `IOtlpEncoder`, hands payloads to
`IWireCodec::Send`, classifies retries, applies backoff with jitter,
accounts for drops.

This is the "translation layer" between the SDK (which knows nothing
about OTLP wire) and the wire codecs (which know nothing about
OpenTelemetry semantics).

## Owner

Track A — Trace SDK.

## Implements

- `internal::IExporter` (declared in [`include/microtel/internal/exporter.hpp`](../../include/microtel/internal/exporter.hpp))
- The retry orchestration: budget, exponential backoff with jitter,
  per-export deadline (per `docs/sequences/retry-after-failure.md`)
- Drop accounting at the export-pipeline boundary (counters
  `retryable_failure_recovered`, `retry_budget_exhausted`,
  `force_flush_timeout`, `shutdown_timeout` per `error-model.md` §3)

## Depends on

- `IOtlpEncoder` (Track F; mock at [`tests/mocks/mock_otlp_encoder.hpp`](../../tests/mocks/))
- `IWireCodec`  (Tracks B/C; mock at [`tests/mocks/mock_wire_codec.hpp`](../../tests/mocks/))
- `IClock` / `ISteadyClock` (fakes at [`tests/fakes/`](../../tests/fakes/))
- `IDiagnosticsSink` (fake at [`tests/fakes/`](../../tests/fakes/))

## Test entry points

- `tests/unit/exporter/` — drives every retry/partial-success/non-
  retryable matrix row from `error-model.md` §7 against `MockWireCodec`.
- `tests/unit/exporter/partial_success/` — pinned coverage for the
  partial-success "never retried" rule (LOCKED — `error-model.md` §6).
- `tests/integration/export_pipeline/` — wires real exporter +
  encoder + fake transport.

## Style notes

- **The exporter does not reinterpret the codec's classification.**
  `WireResult.retryable` and `retry_after` are respected as returned;
  protocol-specific details (`Retry-After` vs `RetryInfo`) stay inside
  the codec (LOCKED — `interfaces.md` §4.3).
- **The exporter never retries a partial-success response.** This is
  the most counterintuitive rule; it has its own sequence diagram in
  `docs/sequences/partial-success.md`.
- **Per-encode arena** (LOCKED — `memory-model.md` §3.1): on retry, the
  exporter calls `IOtlpEncoder::Encode` again rather than reusing the
  failed `EncodedPayload`. The encoder produces fresh bytes each time.
