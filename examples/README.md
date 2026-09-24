# `examples/`

Standalone programs that use the public `microtel::*` API, plus the container
stack they all export to. They are meant to be read and run. Nothing should
depend on them.

---

## Quickstart

Three commands take you from a clean checkout to a trace on screen:

```bash
# 1. Start the collector + Tempo + Grafana stack (docker or podman, either).
examples/stack/up.sh

# 2. Build the examples. They are gated behind MICROTEL_BUILD_EXAMPLES=ON.
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build

# 3. Run one. It prints the trace ID it emitted.
./build/examples/microtel_example_basic_trace
```

```
trace_id: 532267361510131fac037e635454ce60
ForceFlush: Completed
batches_sent=1 batches_failed=0 queue_depth=0
Shutdown: Completed
```

Then open <http://localhost:3000>. There's no login; the stack runs anonymous
for local use. The home page is the **microtel — recent traces** dashboard, and
the trace shows up there within about ten seconds. Click the row for the flame
graph, or paste the trace ID into Explore → Tempo.

`examples/stack/down.sh` tears it all down.

Pinned image versions, ports, engine detection, SELinux notes and
troubleshooting for the stack are in [`stack/README.md`](stack/README.md).

---

## The examples

Each example is one directory holding a `main.cpp`, a `README.md`, and a
`CMakeLists.txt` with a single `microtel_add_example()` call. Every binary lands
in `build/examples/microtel_example_<name>`, whichever directory defined it.

| Example | What it shows | Status |
|---|---|---|
| [`basic_trace/`](basic_trace/) | The smallest end-to-end flow: build a provider, emit one request trace, flush, read exporter health, shut down. | Available |
| [`sugar_tour/`](sugar_tour/) | The `microtel::sugar` convenience layer (`MICROTEL_TRACE_FUNCTION`, `mt::Span`, `mt::Traced`, `mt::RecordException`, `mt::AttrKey`) in one order pipeline. | Available |
| [`context_propagation/`](context_propagation/) | `StartAsCurrentSpan` and the thread-local context slot: implicit parenting with no arguments passed, and what does *not* cross a thread boundary. | Available |
| [`distributed_handoff/`](distributed_handoff/) | Two binaries. W3C `traceparent` / `tracestate` / `baggage` inject and extract across a process boundary, joined into one trace. | Available |
| [`sampler_chains/`](sampler_chains/) | Composing head samplers into a first-match rule chain, and the same rules under all-must-agree. Prints per-case sampled counts. | Available |
| [`resource_detectors/`](resource_detectors/) | The built-in `process.*` and `host.*` detectors, and the detector → env → user precedence that decides a contested key. | Available |
| [`multi_profile/`](multi_profile/) | Two independent named providers in one process, found by name with `microtel::GetProvider`. | Available |
| [`hot_reload/`](hot_reload/) | The four ICP 0026 setters against a live pipeline (sampler ratio, batch options, log level), including the calls that are refused. | Available |
| [`health_and_backpressure/`](health_and_backpressure/) | Reading `HealthSnapshot`: drop counters and queue depth under load, then a collector that is not there. | Available |
| [`auth_bearer/`](auth_bearer/) | Static headers and `WithAuthProvider`, against a collector that checks the token. Opt-in overlay. | Available |
| [`tls/`](tls/) | TLS, a custom CA and mTLS, and the one configuration in which OTLP/HTTP works. Opt-in overlay. | Available |
| `metrics_exemplars/` | Metrics with exemplars. | Planned, optional (adds Prometheus to the stack) |

The planned set mirrors the v1.1 public surface; issue
[#279](https://github.com/chanderraja/microtel/issues/279) is the epic.
Metrics and logs examples will follow when those signals get conformance
coverage in v1.2 and v1.3 (see [`microtel-roadmap.md`](../microtel-roadmap.md)).

### Opt-in overlays

Two examples need a receiver the shared stack deliberately leaves out: one that
checks a bearer token, and one that serves TLS. Each brings its own collector
as a separate compose project on separate ports, started and torn down by its
own scripts:

```bash
examples/auth_bearer/up-auth.sh   # bearer-guarded OTLP/gRPC on :5317
examples/tls/up-tls.sh            # TLS on :5327 (gRPC), :5328 (HTTP), :5337 (mTLS)
```

Nothing in [`stack/`](stack/) changes while they run. The shared collector
keeps 4317/4318, every other example keeps working, and each overlay forwards
what it accepts to the shared collector so its traces still reach Grafana.
Start the shared stack first if you want to see them there.

---

## Why `http://localhost:4317` and not `:4318`

The examples export OTLP/gRPC to the collector's 4317, or OTLP/HTTP over TLS.
Plaintext OTLP/HTTP doesn't work, and adding a feature wouldn't fix it; the
cause is in how the two ends speak HTTP.

microtel's transport is HTTP/2-only, so a plaintext `http://` endpoint means
h2c with prior knowledge. A stock collector's plaintext OTLP/HTTP receiver
serves HTTP/1.1 only. It answers the HTTP/2 preface with an HTTP/1.1 response,
and the connection never completes. gRPC is h2c by definition and is
unaffected. Over TLS, ALPN negotiates `h2` and OTLP/HTTP works against the same
collector.

microtel reports this in three places instead of failing silently: a `Build()`
warning, a `Connect()` error of kind `Protocol`, and
`HealthSnapshot::last_error_message`. The full story is in
[`docs/compatibility-matrix.md`](../docs/compatibility-matrix.md) §4 and issue
#166.

---

## Building

```bash
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build                                   # all examples
cmake --build build --target microtel_example_basic_trace   # just one
```

`MICROTEL_BUILD_EXAMPLES` defaults to `OFF`, so a normal build never pays for
them. Each example links `microtel_sdk`, which brings in the public headers and
every runtime link dependency (nghttp2, OpenSSL, zlib). That's the same thing a
consumer gets.

### Adding an example

1. `mkdir examples/<name>/` with `main.cpp` and `README.md`.
2. `examples/<name>/CMakeLists.txt`, one line:

   ```cmake
   microtel_add_example(<name>)
   ```

   `SOURCES` overrides the default of `main.cpp`, and is how one directory
   builds two binaries:

   ```cmake
   microtel_add_example(handoff_sender   SOURCES sender.cpp)
   microtel_add_example(handoff_receiver SOURCES receiver.cpp)
   ```

3. One `add_subdirectory(<name>)` line in
   [`CMakeLists.txt`](CMakeLists.txt), and a row in the table above.

Nothing else in the tree changes, so two examples landing in parallel only
ever touch that list.

House rules, all visible in `basic_trace/main.cpp`:

- Print the trace ID. It's what makes a run verifiable against Tempo, and the
  first thing a reader needs when the dashboard looks empty.
- Default the endpoint to `http://localhost:4317` and accept an override as
  `argv[1]`, so the stack works with no arguments.
- Build under `-Wall -Wextra -Wpedantic -Werror` (the helper adds them). In
  particular, initialise `StartSpanOptions` with all of its fields; a partial
  designated initialiser trips `-Wmissing-field-initializers`.
- Report failure through the API's own channels (`ForceFlush`'s status,
  `GetExporterHealth()`) instead of crashing, so the example still teaches
  something when no collector is running.

---

## Scope, and what CI does with these

Examples sit outside the TDD gates. Neither the diff-coverage gate nor the
test-presence check applies to `examples/**`. They're demonstrations, and a
unit test per example would be a test nobody reads.

CI still keeps them honest. The `cxx20 / clang` job in
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) configures with
`-DMICROTEL_BUILD_EXAMPLES=ON`, so every example has to compile against the
current public API on every PR. The gate paid for itself straight away: when it
was added (alongside this README) it caught `basic_trace` failing to build,
because `microtel::Status` had grown two enumerators in v1.1 and nothing had
asked the example to compile since.

Compiling is the whole contract. CI never runs the examples, because that
needs the container stack, which is a local-developer tool.
