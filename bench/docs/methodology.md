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

## Sink modes

### blackhole

The blackhole sink accepts all incoming OTLP spans and discards them
immediately, providing minimal processing overhead.  It exposes `/stats`
(spans received, bytes received) and `/reset` over HTTP on port 19080.

`bytes_received` is available in blackhole mode and is used for the
wire-bytes correctness check (bench-spec §6.2 step 2 / ICP 0006 §7):
after each sample the driver asserts that `sink.bytes_received` equals the
SUT's reported `bytes_sent`.  This check is a **release gate**.

### collector (otel-collector-contrib)

The collector sink runs the OpenTelemetry Collector with an OTLP receiver,
batch processor, and debug exporter.  Span counts are read by scraping the
collector's Prometheus endpoint on port 8888 using a **two-scrape bracket**:
`reset()` records the current values of `otelcol_receiver_accepted_spans`
and `otelcol_receiver_refused_spans`; `stats()` scrapes again and subtracts.

`bytes_received` is `null` in collector mode — the collector does not expose
received-bytes via its Prometheus metrics.  The wire-bytes correctness check
is therefore **skipped** in collector mode.  Span-count checks are performed
in both modes.

Collector mode has higher span-count variance than blackhole mode due to
Prometheus scrape timing and collector-internal batching.

## Bytes sent

`bytes_sent_total` in the per-sample results is always 0 from the SUT side;
it is populated by the blackhole sink's `/stats` endpoint after each run.
In collector mode the field remains 0 and `sink.bytes_received` is null.

## Latency measurement

Per-span latency is the wall-clock time from immediately before `EmitSpan()`
returns to immediately after, measured with `std::chrono::steady_clock`.  This
captures SDK hot-path overhead (span creation, attribute encoding, queue push)
but excludes network RTT and batch flush latency.  It is **not** end-to-end
export latency.
