# Interop Matrix

## 1. Purpose

The pinned-version registry behind the conformance gates in
[`microtel-spec.md`](../microtel-spec.md) §13.5. Those gates say trace export
"passes integration tests against the pinned OpenTelemetry Collector matrix" —
this file is what "pinned" means, in one place, so a version bump is a
reviewable diff rather than an archaeology exercise across three trees.

Two readers:

- **The per-PR `conformance` job**, which runs
  [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh) against the
  collector pinned below.
- **The weekly [`interop.yml`](../.github/workflows/interop.yml) workflow**,
  which exercises delivery against collector and Jaeger.

Nothing reads this file mechanically. It records what the pins *are*; the pins
themselves live in the files named in the table.

---

## 2. Pins

| Component | Version | Pinned where | Exercised by |
|---|---|---|---|
| `opentelemetry-proto` | `v1.10.0` | vendored; [`proto/README.md`](../proto/README.md) | wire encoder, `regen-check` |
| `protoc` / upb | `v29.4` | [`gen/`](../gen/) + `regen-check` in [`ci.yml`](../.github/workflows/ci.yml) | generated accessors, zero-diff regen gate |
| `otel/opentelemetry-collector-contrib` | `0.160.0` | [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh), [`bench/sink/collector/Dockerfile`](../bench/sink/collector/Dockerfile), [`bench/versions.lock`](../bench/versions.lock) | `conformance` job, bench collector sink |
| `jaegertracing/all-in-one` | **`latest` — UNPINNED, known gap** | [`interop.yml`](../.github/workflows/interop.yml) | weekly interop only |

Collector image digest (multi-arch index) for `0.160.0`:

```
sha256:799dc6cf12c96192af37b5bdba804da8c10b3bc563b43cb90c3f3c58d9572ad6
```

The script pins by **tag**, not digest: a tag is readable in a diff and the
digest above is the audit trail if a tag is ever re-pushed. Verify with
`podman inspect --format '{{json .RepoDigests}}' otel/opentelemetry-collector-contrib:0.160.0`.

### Jaeger is not pinned

`interop.yml` pulls `jaegertracing/all-in-one:latest`. A weekly job that tracks
a moving tag can fail for reasons that have nothing to do with a microtel
change, and its green is not evidence about any particular Jaeger version.
It runs weekly rather than per-PR, so it blocks nothing — but it should be
pinned. See §5.

---

## 3. Coverage split

The two suites answer different questions and are deliberately not merged.

**Per-PR conformance** — [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh),
`tests/conformance/`. Correctness against a real receiver: does the collector
accept what microtel emits, and is what it decodes what microtel meant?

| Area | OTLP/HTTP | OTLP/gRPC |
|---|---|---|
| Acceptance (connect, export, `ForceFlush`) | ✅ | ✅ |
| Content round-trip (ids, attribute types, events, status) | ✅ | ✅ |
| Multi-batch delivery, exactly-once, large multi-DATA-frame bodies | ✅ | ✅ |
| gzip request compression | ✅ | ✅ (per-message, `grpc-encoding`) |
| TLS, mTLS, CA pinning, SNI override | ✅ | ✅ |
| Bearer-token auth (static header, callback, wrong-credential rejection) | ✅ (401) | ✅ (`grpc-status: 16`) |
| Plaintext transport | ✗ — pinned as a negative test, issue #166 | ✅ — h2c, the counterpoint |

The two suites are deliberate per-file duplicates rather than one parameterised
harness: they diverge on which endpoint is reachable, on what a rejection looks
like on the wire, and on what the failure surface says, and a shared template
would have to be taught all three differences to hide none of them.

Three caveats behind those ticks:

- **`drop_counters` is not yet evidence.** The negative tests assert
  `batches_failed` and the collector's output file, not the `DropReason`
  counters `docs/error-model.md` §7 names, because only two of the 24 counters
  are ever incremented (issue #169). The assertion is parked as
  `DISABLED_WrongTokenIncrementsNonRetryableDropCounter` in both
  `tests/conformance/http/auth_test.cpp` and
  `tests/conformance/grpc/auth_test.cpp` until they are wired.
- **Instrumentation scope is not asserted.** `ScopeSpans.scope` currently
  carries the service name rather than the `GetTracer(name, version)` scope
  (issue #167), so no test here asserts on it — that would enshrine the bug.
  Confirmed identical on both protocols.
- **The gRPC failure surface names the status** (issue #171, fixed). A
  rejected credential reaches `HealthSnapshot::last_error_message` as
  `UNAUTHENTICATED (16): provided authorization does not match expected scheme
  or token` — the status name, its number, and the collector's own
  percent-decoded `grpc-message`. `tests/conformance/grpc/auth_test.cpp`
  asserts the status text and a fragment of the collector's sentence
  separately, so a regression says which half broke. Until this landed the
  field read `"grpc error"` for every non-zero status.

**Encoding is protocol-independent, and that was measured.** The collector's
file-exporter line for the OTLP/HTTP basic-export span and the one for its
OTLP/gRPC twin are byte-identical after normalising trace/span ids, timestamps
and the per-run marker — same key order, same typed value envelopes. The gRPC
suite therefore reuses the HTTP suite's protojson fragment constants verbatim
rather than maintaining a second set.

**Weekly interop** — [`interop.yml`](../.github/workflows/interop.yml).
Behaviour at volume against collector and Jaeger: delivery rate and
throughput. Slow, and not a merge gate.

---

## 4. Known gaps

**OTLP/HTTP over plaintext does not reach a stock collector.** microtel is
HTTP/2-only, so a plaintext endpoint means h2c with prior knowledge. The
collector's plaintext OTLP/HTTP receiver serves HTTP/1.1 only — it does not
wrap its handler in `h2c` — so the HTTP/2 connection preface is answered with
an HTTP/1.1 error and the SETTINGS exchange never completes. `curl
--http2-prior-knowledge` against `:4318` fails identically, which is how the
finding was confirmed against the pinned image.

Consequences:

- The HTTP conformance tests use the **TLS** receiver, where the same collector
  negotiates `h2` through ALPN.
- The gap is pinned deliberately by
  `tests/conformance/http/plaintext_gap_test.cpp`, which asserts that
  `Connect()` to the plaintext receiver **fails**. When microtel gains an
  HTTP/1.1 fallback, or the collector gains h2c, that test fails and is to be
  inverted into a positive delivery test rather than deleted.
- The collector's bearer-auth **http** receiver (`:4348`) carries server TLS
  material for the same reason, even though the point of that receiver is to
  isolate the `Authorization` header. Its **grpc** twin (`:4347`) stays
  plaintext.
- OTLP/**gRPC** over plaintext is unaffected: gRPC is h2c by definition and the
  collector's gRPC receiver speaks it. This is no longer an argument — the
  whole gRPC conformance suite except `tls_test.cpp` runs over `http://` against
  the same collector, so the two directories side by side are the measurement of
  how far the gap reaches.
- `bench/sink/blackhole` already wraps its handler in `h2c.NewHandler` for this
  reason, so the bench harness does not see the gap.

Since this was found, the gap names itself rather than arriving as a generic
nghttp2 failure: `Build()` warns, `Connect()` returns an `Error::Kind::Protocol`
naming the two configurations that do work, and the codecs do not retry it.
[`compatibility-matrix.md`](compatibility-matrix.md) §4 is the operational
version — what to use instead, and what each surface says.

Whether microtel should gain an HTTP/1.1 fallback — the OTLP specification
permits HTTP/1.1 for OTLP/HTTP — is not something this file decides. It is
parked as open roadmap question 6 in
[`microtel-roadmap.md`](../microtel-roadmap.md) §12, and would need an ICP.
Issue #166 tracks it.

---

## 5. Update procedure

A pin bump is one PR:

1. Bump the tag in [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh).
2. Verify the components the config depends on still exist:
   `podman run --rm otel/opentelemetry-collector-contrib:<TAG> components`
   — needs `otlp`, `batch`, `file`, `debug`, `health_check`, `bearertokenauth`.
   Receiver and extension config schemas change between releases; a rejected
   config fails at startup, which the health wait reports as a timeout.
3. Run `ci/scripts/conformance.sh <build-dir>` locally and confirm green.
4. Update this file (tag **and** digest) plus
   [`bench/sink/collector/Dockerfile`](../bench/sink/collector/Dockerfile) and
   [`bench/versions.lock`](../bench/versions.lock) in the same PR — three
   places, one commit, or they drift.

---

## 6. Deferred

| Item | Target |
|---|---|
| Metrics conformance against the collector | v1.2 |
| Restart-recovery scenario (collector bounced mid-export) | post-v1.0 |
| Pinning `jaegertracing/all-in-one` | post-v1.0 |
