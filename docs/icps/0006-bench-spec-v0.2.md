# ICP 0006: Align bench-spec.md with v1 scope (v0.1 → v0.2)

**Status:** Accepted
**Affected interfaces / docs:** `docs/bench-spec.md` (all sections)
**Affected tracks:** None (docs / infra only)
**Spec / roadmap impact:** None. All changes are internal to the bench
harness specification; no production interfaces touched.

## Summary

`docs/bench-spec.md` was drafted at v0.1 before the v1 spec stabilised
at v0.13. Ten alignment gaps accumulated. This ICP closes them in a
single pass, advancing bench-spec to v0.2.

## Changes

### 1. v1 scope: traces only

`microtel` v1 is traces-only. The `realistic-request` profile description
mentioned "one log record, one histogram observation, one counter
increment" — none of which exist in v1. Remove them from the profile
description; add a note that those slots expand in v1.2 (metrics) and
v1.3 (logs).

### 2. Four SUTs, not three

The v1 spec requires validating both microtel wire protocols
(OTLP/HTTP-protobuf and OTLP/gRPC). The original bench-spec had one
microtel SUT (HTTP+pb). Add **SUT-B: microtel (gRPC)**, making the SUT
matrix:
- SUT-A: microtel (HTTP+protobuf)
- SUT-B: microtel (gRPC)
- SUT-C: otel-cpp + gRPC exporter
- SUT-D: otel-cpp + HTTP exporter

Update the architecture diagram, §3.1 SUT description, §9 layout
(`sut/microtel-grpc/`), §10 B0 milestone scope, and the §6.2 TL;DR
table example.

### 3. Sampled vs unsampled measurement

Hot-path latency numbers are meaningless without knowing the sampler
state. The `hot-loop-traces` profile gains a `sampler` field
(default: `always_on`; also runs `never` and `traceidratio(0.1)`).
The §5 hot-path metrics rows gain "(sampled/unsampled)" qualifiers.

### 4. Drop counters by reason

The spec defines `IDiagnosticsSink::RecordDrop` with a per-reason enum.
Aggregate drop count ("Items dropped (queue overflow)") does not map to
the actual counters. Split into four rows:
- queue overflow
- span limit exceeded
- record too large
- partial-success rejection from collector

### 5. Binary-size profile: component-separated outputs

Spec §10.5 sets targets per component. Update §4.6 to produce
component-separated measurements: exporter library (stripped), SDK
library (stripped), full dep-closure size — not just a single binary-size
figure.

### 6. v1.0 release-gate minimum

M7 needs a hard completion bar. Document it explicitly: `hot-loop-traces`,
`binary-size`, and `realistic-request` are the v1.0 release-gate minimum.
`cold-start` is a stretch goal. `bursty` and `large-attributes` defer
to v1.0.1.

### 7. Wire-bytes-identical correctness check

OTLP wire encoding must be identical across microtel-HTTP and microtel-gRPC
for the same payload (after framing is removed). The harness must treat
a wire-bytes divergence as a test failure before accepting any other
metric as valid. Add this as step 0 in §6.2 results.md and as a
`--sanity-check` gate in the driver.

### 8. Blackhole-sink wire coverage scope

The blackhole sink covers the happy path only. Trailer-only responses,
split frames, and GOAWAY during export require the otel-collector sink
or a purpose-built fault-injection sink (out of scope for v1 bench).
Document this as a scope statement, not a gap.

### 9. Environment fingerprint additions

Add to the driver's captured fingerprint:
- microcode mitigation status (`/sys/devices/system/cpu/vulnerabilities/`)
- kernel `mitigations=` boot parameter (`/proc/cmdline`)

These affect timing results on spectre/meltdown-patched kernels and
must be present in every report for results to be reproducible.

### 10. §10 B0 SUT list corrected

B0 listed "microtel + otel-cpp-gRPC SUTs" (one microtel SUT). Updated
to "microtel-HTTP + microtel-gRPC + otel-cpp-gRPC SUTs" (two microtel
SUTs from day one).

## §12 Open Question Resolutions

| # | Question | Decision |
|---|---|---|
| Q1 | Default sink | **blackhole.** Lower noise for primary comparison. `--sink=collector` for end-to-end realism. As proposed. |
| Q2 | otel-cpp+nghttp2 hybrid | **Deferred to v2.** Useful isolation experiment but adds SUT maintenance cost before v1.0. |
| Q3 | Languages beyond C++ | **Deferred.** Document in `bench/docs/methodology.md` that v1 is C++-comparison-focused. Multi-language profiles may land in v2. |
| Q4 | Reference machine | **c6i.xlarge (or equivalent 4-vCPU compute-optimized cloud VM), pinned AMI/kernel.** Weekly cron. GitHub-hosted runners are too noisy. Document in methodology. |
| Q5 | Workload code license | **Apache 2.0. Confirmed.** |

## Sign-off

| Reviewer | Date | Status |
|---|---|---|
| Chander Raja | 2026-05-11 | Accepted |
