# `src/wire/encoder/`

## Purpose

Encodes `BatchHandle` → OTLP protobuf bytes via upb.

**This is the only directory in the codebase that includes upb headers
or references upb symbols** (LOCKED — `memory-model.md` §3.1, ICP 0001).
No upb type appears in any header outside this directory; the
`OtlpEncoder` C++ wrapper is the contract surface.

## Owner

Track F — OTLP encoder. Foundational track (finishes before A/B/C/E
unblock per `docs/development.md` §2).

## Implements

- `internal::IOtlpEncoder` (declared in [`include/microtel/internal/otlp_encoder.hpp`](../../../include/microtel/internal/otlp_encoder.hpp))
- The `EncodedPayload` value type (declared in [`include/microtel/internal/encoded_payload.hpp`](../../../include/microtel/internal/encoded_payload.hpp); produced by `Encode`)
- The per-call `UpbArena` RAII wrapper (lives here, not in `src/common/raii/`,
  because nothing outside `src/wire/encoder/` may reference it)

## Depends on

- upb (vendored at [`third_party/upb/`](../../../third_party/upb/), pinned commit per spec §9.1)
- Generated upb C accessors at [`gen/`](../../../gen/) — committed; CI
  verifies regeneration zero-diff per spec §9.3.
- `Resource`, `BatchHandle`, `SpanRecord` (plain C++ values; no upb
  leakage at the boundary)

## Test entry points

- `tests/unit/wire/encoder/` — encode every span shape (status codes,
  attribute types, events, links).
- `tests/wire/encoder/` — byte-level fixtures verified against canonical
  upstream encoders.
- CI regen-determinism gate: `make regen-protos` against the pinned
  upb + `opentelemetry-proto` versions must produce zero diff (spec §9.3,
  release gate per spec §13.5).

## Style notes

- **upb arena lifetime is per-`Encode()` call** (LOCKED). Constructed at
  entry, destroyed before `Encode` returns. No arena outlives a single
  call; no upb pointer survives the call.
- **`EncodedPayload` is a `std::unique_ptr<std::byte[]>` + `size_t`
  pair** (LOCKED — `memory-model.md` §3.2, ICP 0003). The encoder
  allocates a fresh byte buffer each call and transfers ownership; the
  arena is destroyed independently.
- **Containment rule:** `#include` of any upb header outside this
  directory is a CI failure. The custom check that enforces this is
  added in M2 chunk 5 (CI scaffolding).
- **Stateless across calls.** The encoder is a pure function. Multiple
  encoders may exist; v1 has one shared encoder per `Provider`.
