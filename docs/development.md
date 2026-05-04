# microtel Development — Track-to-Directory Atlas

**Status:** M0 deliverable. Answers "which track owns this code?" for contributors and AI agents.
**Companion:** `architecture.md` §7 (track ↔ component map), `microtel-spec.md` §13.1 and §13.3.

This document is an index, not a design doc. The substantive content lives in the per-track READMEs under each `src/<dir>/README.md`, which land in M2 (Skeleton & contracts). Until M2, this atlas is the only map.

---

## 1. Purpose

When you start a session — human or agent — and need to do non-trivial work on microtel, this document tells you:

- Which **track** owns the code you're about to touch.
- Which **directories** make up that track.
- Which **interfaces** the track provides and which it consumes (via mocks during track work).
- Which **tests** exercise the track.
- Which **other documents** to read before editing.

If your change spans tracks, this is the document that tells you so — and prompts you to think about whether your change is one PR or two.

After M2, each `src/<dir>/README.md` carries the directory-local detail; this document remains the cross-cutting map.

---

## 2. The six tracks

Defined in spec §13.1.

| Track | Theme | Foundational? |
|---|---|---|
| **A** | Trace SDK (API + SDK + exporter orchestration) | no |
| **B** | OTLP/HTTP wire codec | no |
| **C** | OTLP/gRPC wire codec | no |
| **D** | Transport (nghttp2 + OpenSSL + reactor) | **yes** |
| **E** | Configuration (TOML + env + auth) | no |
| **F** | OTLP encoder (upb-backed) | **yes** |

**Foundational tracks (D, F) finish first.** They produce the `ITransport` and `IOtlpEncoder` interfaces and the value types (`EncodedPayload`, `WireResult`, `ConnectionState`) that A/B/C consume. Once mocks of the foundational interfaces exist (M2), tracks A/B/C/E proceed concurrently.

---

## 3. Track A — Trace SDK

**Theme:** the `Tracer` / `Span` / `Provider` user-facing surface, the four samplers, the `BatchSpanProcessor`, the exporter orchestration loop.

**Directories owned:**
- `src/api/` — `Tracer`, `Span`, propagator inject/extract.
- `src/sdk/` — `Resource`, samplers, `BatchSpanProcessor`, `SimpleSpanProcessor`, `Provider`, `SdkBuilder` implementation.
- `src/exporter/` — protocol-agnostic export pipeline: drains the processor, calls the encoder, calls the wire codec, classifies retries, accounts for drops.
- `include/microtel/` — public API headers (M0 deliverable, drafted per `interfaces.md`).
- Public-facing examples in `examples/` that demonstrate the API.

**Provides:**
- `ISpanProcessor`, `ISampler`, `IResourceDetector`, `IExporter` (`interfaces.md` §4.4–§4.6, §4.10).
- The public API surface (`microtel::Tracer`, `microtel::Span`, `microtel::Provider`, `microtel::SdkBuilder`).

**Consumes (via mocks during track work):**
- `IOtlpEncoder` (Track F) — mock in `tests/mocks/mock_otlp_encoder.hpp`.
- `IWireCodec` (Tracks B/C) — mock in `tests/mocks/mock_wire_codec.hpp`.
- `IClock`, `ISteadyClock` — fake in `tests/fakes/fake_clock.hpp`.
- `IDiagnosticsSink` — fake in `tests/fakes/fake_diagnostics_sink.hpp`.
- `Config` value (Track E) — built directly in tests; no interface needed at consumption.

**Tests:**
- `tests/unit/api/`, `tests/unit/sdk/`, `tests/unit/exporter/`.
- `tests/integration/` flows that drive the SDK end-to-end against fakes.
- `tests/conformance/` end-to-end against a real OpenTelemetry Collector.

**Read before editing:**
- `microtel-spec.md` §6 (API surface), §8 (SDK features).
- `architecture.md` §3.1–§3.3.
- `threading-model.md` §2.1, §2.2 (caller-thread and worker-thread contracts).
- `memory-model.md` §8.1 (zero-allocation unsampled-span rule).
- `interfaces.md` §4.4–§4.6.

---

## 4. Track B — OTLP/HTTP wire codec

**Theme:** the OTLP/HTTP-protobuf codec implementation of `IWireCodec`.

**Directories owned:**
- `src/wire/http/` — HTTP-codec source.

**Provides:**
- The HTTP implementation of `IWireCodec` (`interfaces.md` §4.3).

**Consumes (via mocks during track work):**
- `ITransport` (Track D) — mock or fake in `tests/mocks/mock_transport.hpp` / `tests/fakes/fake_transport.hpp`.
- `IAuthProvider` (Track E) — fake in `tests/fakes/fake_auth_provider.hpp`.
- `IDiagnosticsSink` — fake.
- `EncodedPayload` value type (Track F) — used as input only.

**Tests:**
- `tests/unit/wire/http/`.
- `tests/wire/http/` — byte-level codec validation.
- `tests/conformance/` (shared with Track C).

**Read before editing:**
- `microtel-spec.md` §7.1, §7.3.
- `error-model.md` §7.1 (HTTP retry classification matrix).
- `interfaces.md` §4.3.

---

## 5. Track C — OTLP/gRPC wire codec

**Theme:** the OTLP/gRPC codec implementation of `IWireCodec`. Built directly on nghttp2; no gRPC library.

**Directories owned:**
- `src/wire/grpc/` — gRPC-codec source.

**Provides:**
- The gRPC implementation of `IWireCodec`.

**Consumes:** same as Track B, plus the vendored `google.rpc.Status` and `RetryInfo` proto generated code under `gen/google/rpc/`.

**Tests:**
- `tests/unit/wire/grpc/`.
- `tests/grpc-wire/` — byte-level edge-case corpus (see `grpc-wire-protocol.md` §7).
- `tests/conformance/`.
- `tests/fuzz/grpc_codec_fuzz.cpp`.

**Read before editing:**
- `microtel-spec.md` §7.2, §7.3.
- `error-model.md` §7.2 (gRPC retry classification matrix).
- **`grpc-wire-protocol.md` in full.**
- `interfaces.md` §4.3.

---

## 6. Track D — Transport (foundational)

**Theme:** the nghttp2 + OpenSSL + reactor connection. The seam where HTTP/3 could drop in later.

**Directories owned:**
- `src/transport/` — `ITransport` implementation, the I/O loop, per-stream state.
- `src/common/raii/` — `Socket`, `SslCtx`, `SslSession`, `Nghttp2Session`. (Shared resource — RAII wrappers serve every track but are implemented and tested under D.)

**Provides:**
- `ITransport` (`interfaces.md` §4.1).
- `IReactor` (`interfaces.md` §4.8).
- The five RAII wrappers in `src/common/raii/`.

**Consumes:**
- OpenSSL (system library).
- nghttp2 (system library; minimum version pinned per spec §9.1).
- `IDiagnosticsSink` (fake during track work).

**Tests:**
- `tests/unit/transport/`.
- `tests/unit/common/raii/`.
- `tests/integration/transport_loopback/` — runs against a small in-process server.
- `tests/fuzz/` — response-size and trailer-parser fuzzers (Track C uses these too).

**Read before editing:**
- `microtel-spec.md` §5.2, §5.3.
- `architecture.md` §3.6.
- `threading-model.md` §2.3, §3.2, §3.3, §5.
- `memory-model.md` §4.2 (RAII wrapper contract).
- `interfaces.md` §4.1, §4.8.

---

## 7. Track E — Configuration

**Theme:** parse `microtel.toml`, resolve env-var overlays, validate, freeze, hand back to `SdkBuilder`. Plus the auth-provider implementations.

**Directories owned:**
- `src/common/config/` — TOML parser usage, env-var reading, validation, the resolved `Config` value type.
- `src/common/auth/` — `StaticHeadersAuthProvider`, `CallbackAuthProvider` (subdirectory created in M3 when first auth code lands; until then auth code may live alongside config).

**Provides:**
- The frozen, validated `Config` value consumed by every other component.
- `IAuthProvider` and its two concrete implementations (`interfaces.md` §4.9).

**Consumes:**
- A TOML parser (vendored or in-tree; selection at M2).
- `std::getenv` for environment reads.

**Tests:**
- `tests/unit/common/config/`.
- `tests/unit/common/auth/`.
- `tests/fuzz/toml_fuzz.cpp` (M9 hardening).

**Read before editing:**
- `microtel-spec.md` §12.
- **`configuration.md` in full** — every new setting takes a row.
- `error-model.md` §8 (init-failure taxonomy).
- `interfaces.md` §4.9.

---

## 8. Track F — OTLP encoder (foundational)

**Theme:** wrap upb. Produce protobuf bytes from a `BatchHandle`. The single file in the codebase that includes upb headers (LOCKED — `memory-model.md` §3.1).

**Directories owned:**
- `src/wire/encoder/` — `IOtlpEncoder` implementation. Sole consumer of upb headers.
- `gen/` — generated upb C accessors for OpenTelemetry protos and `google.rpc.{Status,RetryInfo}`. Committed; regeneration verified by CI.
- `proto/` — vendored `opentelemetry-proto` and the small subset of `googleapis` (Status, RetryInfo). Pinned tags.
- `third_party/upb/` — vendored upb at a pinned commit.

**Provides:**
- `IOtlpEncoder` (`interfaces.md` §4.2).
- `EncodedPayload` (`memory-model.md` §3.2).

**Consumes:**
- upb (vendored).
- The generated proto code under `gen/`.

**Tests:**
- `tests/unit/wire/encoder/`.
- `tests/wire/encoder/` — encoded-byte fixtures verified against canonical upstream encoders.
- A regen-determinism test in CI: `make regen-protos` against the pinned upb + opentelemetry-proto must produce a zero-diff result (spec §9.3).

**Read before editing:**
- `microtel-spec.md` §7.3 (shared encoder for both wire protocols).
- `memory-model.md` §3 (per-encode arena, `EncodedPayload`, the encoder containment rule).
- `interfaces.md` §4.2.

---

## 9. Cross-track work — when your PR spans tracks

If your PR touches files in two or more track directories, stop and check:

1. **Is this really one change?** A bug fix that happens to require touching two tracks (e.g., a missing field in `WireResult`) is one change. A combined "add a new sampler and update the gRPC codec" is two and should be split.
2. **Does it touch a locked interface?** A breaking change to any `interfaces.md` entry needs an ICP merged first (`docs/icps/README.md`). The implementing PR follows.
3. **Does it touch a contract document?** Changes to `architecture.md` §3, `threading-model.md`, `memory-model.md`, or `error-model.md` require an ICP per `docs/icps/README.md` §"When an ICP is required."
4. **Are the right reviewers tagged?** `CODEOWNERS` routes per directory. A cross-track PR adds two or more reviewers automatically; do not bypass that.

When in doubt: **ask before writing.** Cross-track refactors that look small often grow.

---

## 10. Starting a session — checklist

When you (or an AI agent) start a session intending to do non-trivial work:

1. **Run `git status` and `git log --oneline -20`.** Get the current state.
2. **Read `CLAUDE.md`.** Durable rules.
3. **Determine the milestone phase** from `CLAUDE.md` "Current phase: check before you act."
4. **Identify the track** for the work, using §3–§8 above.
5. **Read the entries flagged "Read before editing"** for that track.
6. **Read the per-directory `README.md`** for any directory you're going to edit. (M2+ only.)
7. **If the work touches an interface**, re-read its entry in `interfaces.md`.
8. **If the work changes a contract**, draft the ICP first.

For trivial work (typo fix, comment update, single-line bug fix in one file), skip directly to the change. The checklist exists for substantive work.

---

## 11. CODEOWNERS

`CODEOWNERS` at the repo root is the authoritative routing table for review requests. Per spec §13.3:

- Each `src/<dir>/` has an owner (one or more GitHub handles, plus organisation teams once a maintainer model is established).
- `docs/` and `microtel-spec.md` and `microtel-roadmap.md` have a separate set of owners.
- `third_party/upb/` and `gen/` have specific owners because vendored-dependency updates have their own review checklist (spec §9.6).

A solo project before v1.0 has every owner pointing to the project lead. The structure exists so that as the project grows, ownership can fan out without changing the routing mechanism.

A CI check that fails PRs touching files outside the author's claimed track is **deferred** until collision incidents prove the lighter tools insufficient (spec §13.3). For now, CODEOWNERS plus this atlas plus per-directory READMEs are sufficient.
