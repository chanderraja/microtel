# Benchmark methodology

## Drop counter reporting

### microtel SUT

Drop counters are read from `Provider::GetExporterHealth()` which returns a
`HealthSnapshot` containing one counter per `DropReason` enumerator (20 total
in v1).  `spans_dropped_total` in the result JSON is the sum of all 20 counters.

### otelcpp-grpc and otelcpp-http SUTs

opentelemetry-cpp does not expose drop counters via a stable public API.
`spans_dropped_total` is reported as **0** for these SUTs regardless of actual
drops.  Comparisons of drop rates between microtel and otelcpp SUTs are
therefore not meaningful.  This will be revisited if otelcpp adds a comparable
API.

## Bytes sent

`bytes_sent_total` is read from the **blackhole sink** (via its `/metrics`
control endpoint), not from the SUT.  The value in `BackendStats` returned by
`IBackend::Stats()` is always 0; the driver overwrites it with the sink-side
counter after each run.

## Latency measurement

Per-span latency is the wall-clock time from immediately before `EmitSpan()`
returns to immediately after, measured with `std::chrono::steady_clock`.  This
captures SDK hot-path overhead (span creation, attribute encoding, queue push)
but excludes network RTT and batch flush latency.  It is **not** end-to-end
export latency.
