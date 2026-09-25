# `src/exporter/`

## What lives here

The protocol-agnostic export pipelines, one per signal. Each exporter owns a
worker thread and a bounded batch queue. `Export` enqueues a batch and returns
at once; the worker drains the queue, encodes each batch, hands the bytes to
the wire codec, and accounts for what failed.

This is the layer between the SDK, which knows nothing about the OTLP wire, and
the wire codecs, which know nothing about OpenTelemetry semantics.

| File | Class | Signal |
|---|---|---|
| [`otlp_exporter.hpp`](otlp_exporter.hpp) | `OtlpExporter` | traces |
| [`otlp_metric_exporter.hpp`](otlp_metric_exporter.hpp) | `OtlpMetricExporter` | metrics |
| [`otlp_log_exporter.hpp`](otlp_log_exporter.hpp) | `OtlpLogExporter` | logs |
| [`retry_policy.hpp`](retry_policy.hpp) | `RetryPolicyConfig`, `ComputeBackoff` | all three |
| [`retry_engine.hpp`](retry_engine.hpp) | `RetryEngine` | all three |

All three exporters retry through one `RetryEngine`. It applies a retry
budget, exponential backoff with jitter and a per-export deadline
(`docs/sequences/retry-after-failure.md`), and records each batch's
final-outcome counter. `Shutdown` wakes a backoff sleep and ends the retry
loop (`docs/sequences/shutdown-drain.md`). Each exporter makes attempt 0
itself as a `SendAll` fan-out, then hands each result to the engine.

## Owner

Track A — Trace SDK.

## What it implements

- `internal::IExporter`, `internal::IMetricExporter` and
  `internal::ILogExporter` (declared in
  [`include/microtel/internal/`](../../include/microtel/internal/)
  `exporter.hpp`, `metric_exporter.hpp` and `log_exporter.hpp`).
- Retry orchestration for every signal, with defaults from the OTLP spec: 5 attempts,
  1 s initial backoff, 32 s ceiling, 1.5× multiplier, ±20 % jitter, a 5-minute
  budget.
- Drop accounting at the export boundary: `retryable_failure_recovered`,
  `retry_budget_exhausted`, `non_retryable_failure`,
  `partial_success_rejection`, and `queue_full` / `post_shutdown` when a batch
  cannot be queued (`error-model.md` §3). `force_flush_timeout` and
  `shutdown_timeout` are recorded one level up, by `SdkProvider` in
  [`src/sdk/`](../sdk/).

## Dependencies and test doubles

- `IOtlpEncoder`, `IMetricEncoder`, `ILogEncoder` (Track F; one `OtlpEncoder`
  implements all three). Mocks: `mock_otlp_encoder.hpp`,
  `mock_metric_encoder.hpp`, `mock_log_encoder.hpp` in
  [`tests/mocks/`](../../tests/mocks/).
- `IWireCodec` (Tracks B/C). Mock `mock_wire_codec.hpp` in
  [`tests/mocks/`](../../tests/mocks/); fake `fake_wire_codec.hpp` in
  [`tests/fakes/`](../../tests/fakes/) where a test needs scripted results.
- `ISteadyClock` (optional; `fake_steady_clock.hpp`) and `IDiagnosticsSink`
  (`fake_diagnostics_sink.hpp`), both in [`tests/fakes/`](../../tests/fakes/).

## Tests

- `tests/unit/exporter/otlp_exporter_test.cpp`: the retry, partial-success and
  non-retryable rows of the `error-model.md` §7 matrix, against
  `MockWireCodec` and `FakeWireCodec`, including the partial-success
  "never retried" rule (LOCKED — `error-model.md` §6).
- `tests/unit/exporter/retry_policy_test.cpp`: backoff and jitter arithmetic.
- `tests/unit/exporter/otlp_metric_exporter_test.cpp` and
  `otlp_log_exporter_test.cpp`: the metric and log pipelines, including the
  same retry rows (recovered, exhausted, non-retryable, `retry_after`, partial
  success, Shutdown during a backoff).
- `tests/integration/sdk/retry_budget_test.cpp`,
  `auth_failure_export_test.cpp` and `exporter_health_test.cpp`: the exporter
  wired into a real SDK pipeline.

## Style notes

- **The exporter does not reinterpret the codec's classification.**
  `WireResult.retryable` and `retry_after` are respected as returned;
  protocol-specific details (`Retry-After` vs `RetryInfo`) stay inside
  the codec (LOCKED — `interfaces.md` §4.3).
- **The exporter never retries a partial-success response.** This is the least
  intuitive rule here, and it has its own sequence diagram in
  `docs/sequences/partial-success.md`.
- **Per-encode arena** (LOCKED — `memory-model.md` §3.1): on retry, the
  exporter calls `IOtlpEncoder::Encode` again rather than reusing the
  failed `EncodedPayload`. The encoder produces fresh bytes each time.
- `OtlpExporter::DrainQueue` releases its queue lock before fanning out to the
  codec, so the worker never holds two non-leaf locks
  (`docs/threading-model.md` §4 rule 2).
