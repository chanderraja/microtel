# `examples/`

Standalone, runnable programs that demonstrate the public `microtel::*` API,
plus the container stack they all export to. These exist to be **read and
run** — not to be depended on.

---

## Quickstart

Three commands from a clean checkout to a trace on screen:

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

Now open **<http://localhost:3000>** — no login, the stack runs anonymous for
local use. The home page is the **microtel — recent traces** dashboard; the
trace appears within about ten seconds. Click the row for the flame graph, or
paste the trace ID into **Explore → Tempo**.

Tear down with `examples/stack/down.sh`.

Everything about the stack — pinned image versions, ports, engine detection,
SELinux notes, troubleshooting — is in
[`stack/README.md`](stack/README.md).

---

## The examples

Every example is one directory: a `main.cpp`, a `README.md`, and a
`CMakeLists.txt` with a single `microtel_add_example()` call. Each binary lands
in `build/examples/microtel_example_<name>`, whichever directory defined it.

| Example | What it shows | Status |
|---|---|---|
| [`basic_trace/`](basic_trace/) | The smallest end-to-end flow: build a provider, emit one request trace, flush, read exporter health, shut down. | **Available** |
| `sugar_tour/` | The `microtel::sugar` convenience layer. | Planned — [#279](https://github.com/chanderraja/microtel/issues/279) |
| `context_propagation/` | `Context`, active spans, and scope management in one process. | Planned |
| `distributed_handoff/` | W3C `traceparent` inject/extract across two processes. | Planned |
| `sampler_chains/` | Composing samplers; parent-based and ratio sampling. | Planned |
| `resource_detectors/` | Populating `Resource` from the environment. | Planned |
| `multi_profile/` | More than one exporter profile from one provider. | Planned |
| `hot_reload/` | Reconfiguring a live pipeline. | Planned |
| `health_and_backpressure/` | Reading `HealthSnapshot`, drop counters, queue depth under load. | Planned |
| `auth_bearer/` | Static headers and `WithAuthProvider`. | Planned |
| `tls/` | TLS and mTLS to the collector — and the one configuration in which OTLP/**HTTP** works. | Planned |
| `metrics_exemplars/` | Metrics with exemplars. | Planned, optional (adds Prometheus to the stack) |

The planned set mirrors the v1.1 public surface; see issue
[#279](https://github.com/chanderraja/microtel/issues/279) for the epic.
Metrics and logs examples track when those signals get conformance coverage
(v1.2 and v1.3 — see [`microtel-roadmap.md`](../microtel-roadmap.md)).

---

## Why `http://localhost:4317` and not `:4318`

Examples export **OTLP/gRPC** to the collector's 4317, or OTLP/HTTP over
**TLS**. Plaintext OTLP/HTTP is not an option, and the reason is structural
rather than a missing feature:

microtel's transport is HTTP/2-only, so a plaintext `http://` endpoint means
h2c with prior knowledge. A stock collector's plaintext OTLP/HTTP receiver
serves HTTP/1.1 only, answers the HTTP/2 preface with an HTTP/1.1 response,
and the connection never completes. gRPC is h2c by definition and is
unaffected; over TLS, ALPN negotiates `h2` and OTLP/HTTP works against the same
collector.

microtel says so in three places rather than failing silently — a `Build()`
warning, a `Connect()` error of kind `Protocol`, and
`HealthSnapshot::last_error_message`. Full detail in
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
them. Each example links `microtel_sdk`, which transitively provides the public
headers and every runtime link dependency (nghttp2, OpenSSL, zlib) — exactly
what a consumer gets.

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

Nothing else in the tree changes, which is the point: two examples landing in
parallel touch only that list.

House rules, all visible in `basic_trace/main.cpp`:

- **Print the trace ID.** It is what makes a run verifiable against Tempo, and
  the first thing a reader needs when the dashboard looks empty.
- Default the endpoint to `http://localhost:4317` and accept an override as
  `argv[1]`, so the stack works with no arguments.
- Build under `-Wall -Wextra -Wpedantic -Werror` (the helper adds them). In
  particular, initialise `StartSpanOptions` with **all** fields — a partial
  designated initialiser trips `-Wmissing-field-initializers`.
- Report failure through the API's own channels (`ForceFlush`'s status,
  `GetExporterHealth()`) rather than crashing, so the example is still
  instructive with no collector running.

---

## Scope, and what CI does with these

Examples are **outside the TDD gates**. Neither the diff-coverage gate nor the
test-presence check applies to `examples/**` — they are demonstrations, not
tested code, and requiring a unit test per example would produce tests nobody
reads.

They are not unguarded, though. The `cxx20 / clang` job in
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) configures with
`-DMICROTEL_BUILD_EXAMPLES=ON`, so every example must compile against the
current public API on every PR. That gate is not theoretical: it was added
alongside this README and immediately caught `basic_trace` failing to build,
because `microtel::Status` grew two enumerators in v1.1 and nothing had ever
asked the example to compile again.

Compiling is the whole contract. Nothing runs the examples in CI — that needs
the container stack, which is a local-developer tool.
