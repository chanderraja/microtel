# ICP 0031: move leaf / concentrator, with both encoder backends, into v1.3

**Status:** Draft.
**Affected interfaces / docs:**
- `microtel-spec.md` §18.3 and §18.4
- `microtel-roadmap.md`:
  - §3 tier table
  - §4: v1.3, v2.0 and v2.1
  - §6, §7, §8 (the v1.x anti-goal row), §9
  - §11: one new decision-log row
- `docs/interfaces.md` (the `IReceiver` note)
- `docs/architecture.md` §6
- `docs/coding-standards.md` (new C section)
- `CLAUDE.md`:
  - rule 12: nanopb joins the closure, for the leaf artifact only
  - rule 13: the upb-confinement rule gains a second directory
- `THIRD_PARTY_NOTICES.md`, plus new `third_party/nanopb/` and generated
  sources under `gen/`
- `CMakeLists.txt` (new leaf target, later)
- `.github/workflows/ci.yml` (a Cortex-M cross-compile job, later)

No existing interface changes. Everything this ICP introduces is additive.

**Affected tracks:** SDK, wire / encoder, build / packaging, CI.

## Summary

Ship the leaf / concentrator architecture from spec §18.4 in v1.3 as an
experimental feature, next to logs, instead of waiting for v2.0. The leaf
ships with both encoder backends, upb and nanopb, so it reaches MCU-class
devices from its first release instead of waiting for v2.1.

## Motivation

The leaf and concentrator are microtel's clearest reason to exist for IoT and
embedded fleets. Most of those devices can't run an OpenTelemetry SDK at all,
never mind a gRPC stack. Leaving the architecture until v2.0 puts it behind
three minor releases whose main themes are largely built already:

- metrics and logs already ship as experimental
- the v1.4 conformance push
- the v1.5 performance work

Metrics and logs mostly need stabilising, which leaves room in v1.3.

Spec §18.4 put this in v2.0 because it assumed the `Receiver` and
Resource-enrichment hooks would be internal interfaces *made* public, which is
a breaking change. They don't have to be. Neither exists in code today
(`docs/interfaces.md` says the `IReceiver` seam "is not realised in v1"), so
both can be introduced as new public API. Adding API isn't breaking and needs
no major version.

## Decisions

### 1. Scope moves, design does not

Everything in spec §18.4 moves to v1.3, including what it leaves out. That
covers:

- the pure-C leaf
- the concentrator role
- the three time-handling modes
- the "Out of scope" list:
  - RTOS ports
  - reliable delivery on the leaf-to-concentrator link
  - time sync beyond the three modes
  - leaf-side sampling

This ICP changes when the work ships, not what it is. The one addition is
the nanopb backend, pulled forward from v2.1 (Decision 3).

### 2. v1.3 becomes "Logs + leaf / concentrator"

The two halves of v1.3 are independent:

- **Logs** are promoted to supported when their gates pass: #303 conformance,
  plus #222 retry.
- **Leaf / concentrator** ships marked experimental, like metrics and logs did.

If the leaf isn't ready, logs don't wait for it. The leaf moves to the next
1.x minor, and that move needs no further ICP.

v2.0 keeps its slot, but its meaning changes. It becomes the release where the
leaf and receiver APIs go **stable**. Any breaking changes that experience
with the experimental API calls for are collected there.

v2.1 loses the nanopb backend to v1.3. It keeps the rest of its theme:
- static memory pools with no malloc anywhere in the leaf
- the < 15 KB flash / < 2 KB RAM Cortex-M0+ budget as a published, gated
  number
- reference ports (STM32 HAL, Zephyr, FreeRTOS examples)

v2.2 is unchanged.

### 3. Leaf: C, two encoder backends, its own artifact

The leaf is written in C, as spec §18.4 says. It has one public API and two
encoder backends, chosen at build time with `MICROTEL_LEAF_ENCODER=upb|nanopb`:

- **upb** reuses the vendored encoder. It targets Linux-on-ARM, OpenWrt-class
  and Cortex-A/R devices.
- **nanopb** targets Cortex-M: no MMU, no heap allocator, sub-256 KB flash.
  That is where most IoT devices are.

Both backends produce the same OTLP bytes. The leaf's tests run against both,
and a byte-for-byte comparison of their output for the same input is part of
the wire test suite.

- It builds as a separate static library behind `MICROTEL_BUILD_LEAF`
  (default OFF) and is exported as `microtel::leaf`.
- It is not part of the `microtel::microtel` aggregate.
- Its link closure is the chosen encoder plus libc only: no nghttp2, OpenSSL,
  zlib or C++ runtime.
- It carries the same `microtel_` symbol prefix on vendored upb, and a
  matching `microtel_pb_*` prefix on nanopb (Decision 4).
- Its source lives in its own top-level directory. `CLAUDE.md` currently
  confines upb to `src/wire/encoder/`; the leaf directory becomes the second,
  and only other, place allowed to include upb headers.
- It gets its own `symbol-scan` pass, which also fails on any C++ runtime
  symbol.

The leaf API follows the constraints spec §18.4 already set: opaque buffer
types and builder functions, with no `upb_*` or `pb_*` symbols in its
headers. The leaf's source is split so that only one backend file per encoder
includes that encoder's headers. The OTLP
wire format is the contract between leaf and concentrator.

Because the leaf is C, the RAII rules in `CLAUDE.md` (rules 5–11) can't apply
to it. `docs/coding-standards.md` gains a C section covering these rules:

- **Ownership:** every allocation is owned by the caller's arena or buffer.
- **Cleanup:** paired init/free functions.
- **Portability:** no VLAs and no compiler extensions.
- **Limits:** the SonarQube limits still apply.

### 4. nanopb as a vendored dependency of the leaf only

Rule 12 fixes microtel's runtime dependency closure, so adding nanopb needs
this ICP. The terms:

- **Scope.** nanopb is linked only into `microtel::leaf` built with
  `MICROTEL_LEAF_ENCODER=nanopb`. It never enters `microtel::microtel`, the
  concentrator, or any other shipped archive. `symbol-scan` fails any
  non-leaf archive that references a `pb_*` or `microtel_pb_*` symbol.
- **Vendored and pinned.** Like upb, nanopb is vendored at a pinned release
  under `third_party/nanopb/`, with its zlib licence added to
  `THIRD_PARTY_NOTICES.md`. That licence is compatible with Apache 2.0.
- **Renamed.** Its globally visible symbols get a `microtel_pb_` prefix
  through a force-included rename header, for the same reason as upb in
  ICP 0020 Decision 4: a firmware image that already links its own nanopb
  must not get two definitions.
- **Generated code is committed.** The nanopb `.pb.c`/`.pb.h` files for the
  OTLP protos are generated once and committed under `gen/`, next to the upb
  output, by an extension of `ci/scripts/regen-protos.sh`. The generator
  (Python plus protoc) is a developer-time tool only and is never needed to
  build microtel.

`CLAUDE.md` rule 12 is amended to read: the runtime closure is nghttp2,
OpenSSL, upb, zlib and optional spdlog, **plus nanopb for the leaf artifact
only**.

### 5. Concentrator: decode into the normal pipeline

The concentrator decodes each leaf payload and feeds it into the same
processing path as in-process telemetry:

- Resource enrichment from config (`device-id → service.*`)
- timestamp correction per the leaf's time mode
- sampling
- batching
- export over OTLP/HTTP or OTLP/gRPC

This is the path spec §18.4 already guarantees. Pass-through forwarding of the
raw bytes is **not** part of this ICP.

- **Input:** bytes come in through a public ingest call that the application
  makes. microtel opens no inbound socket, and the application's transport
  (UART, CAN, BLE, UDP, a message queue) stays the application's business.
  This avoids the attack surface that made ICP 0024 defer the control-plane
  socket.
- **Where decoding lives:** decoding uses upb, so it sits in `src/wire/encoder/`
  under the rule 13 confinement. That directory becomes the upb codec in both
  directions.
- **Resource enrichment:** late Resource enrichment (spec §18.4 seam 1) is built
  here for the first time.

### 6. A design doc comes first

`docs/leaf-concentrator-design.md` must be signed off before any code, the
same way `docs/metrics-design.md` preceded metrics. It settles:

- the leaf C API, and how each backend keeps to it
- the ingest call's signature and error model
- per-leaf identity and config
- the three time modes in detail
- size limits
- what an ingest failure counts as in `DropReason`

This ICP deliberately does not fix those signatures.

## Ship gates for the experimental leaf in v1.3

1. The design doc is signed off.
2. The ingest path has a fuzz target in the standing fuzz job. It parses
   untrusted bytes.
3. An end-to-end test runs leaf, then an in-memory transport, then the
   concentrator, then a real collector, over both protocols.
4. Flash and RAM footprints are measured and published for both backends:
   upb on a Cortex-A / Linux-on-ARM target, and nanopb on a Cortex-M target.
   A CI job cross-compiles the nanopb leaf with `arm-none-eabi-gcc` and
   reports its size on every PR. The < 30 KB (upb) and < 15 KB (nanopb) flash
   figures stay targets in v1.3; v2.1 turns the nanopb one into a gate.
5. The end-to-end test in gate 3 runs once per backend.
6. An example under `examples/leaf/` uses a plain UDP or pipe transport.

## Migration

None for existing users; everything is additive. After this ICP is accepted,
a follow-up docs PR makes these edits:

- **Spec §18.3 and §18.4:** move the section into v1.3, and move the nanopb
  backend out of the v2.1 text.
- **Roadmap:**
  - move the v2.0 content under v1.3
  - restate v2.0 as the stabilisation release
  - remove the nanopb backend from v2.1, leaving pools, budgets and ports
  - drop "leaf/embedded" from the v1.x anti-goals in §8
  - update the §6 footprint table and the §7 adoption story
  - append a §11 decision-log row
- **`docs/interfaces.md` and `docs/architecture.md`:** point at the design doc
  instead of "out of v1 scope".

## Rationale & alternatives

- **v1.4, shifting the later themes back.** This carries less risk to v1.3. It
  was rejected to get the leaf to embedded users sooner. Decision 2's
  "logs don't wait" rule limits the risk instead.
- **upb only in v1.3, nanopb in v2.1.** This was the first draft of this
  ICP, and it matched the spec. It was rejected because upb is too large for
  true microcontrollers. A upb-only leaf would mostly help Linux-class and
  Cortex-A/R devices, and many of those could run full microtel instead. The
  cost of the change is a second encoder and a new vendored dependency in
  v1.3, and more testing, since every leaf test runs twice.
- **nanopb only.** One backend is simpler, and nanopb also builds for Linux.
  It was rejected because upb is already vendored, tested and renamed in
  microtel, and the concentrator decodes with upb anyway. Keeping upb as a
  backend means the larger embedded targets share one encoder with the rest of
  the project.
- **Pass-through forwarding.** Sending each leaf payload on unchanged as the
  gRPC request body would be cheaper, but gives up enrichment, sampling,
  batching and timestamp correction. Those are what make a concentrator more
  than a relay. It could come later as an opt-in fast path.
- **Keeping v2.0.** This is the status quo. It was rejected for the reason in
  Motivation: nothing in the design needs a major version to ship.

## Open questions for the design doc

- Which backend a leaf build uses by default when `MICROTEL_LEAF_ENCODER` is
  not set.

- Whether the concentrator gets a `MICROTEL_WITH_CONCENTRATOR` option under
  ICP 0030's mechanism, and its default.
- Limits on per-leaf Resource cardinality, so a large fleet can't exhaust
  concentrator memory.
- Whether a leaf that sends metrics must use delta temporality, since the
  concentrator can't reconstruct cumulative sums across leaf restarts.
