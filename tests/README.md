# `tests/`

All tests for microtel live here. The structure mirrors the test
taxonomy from `microtel-spec.md` §14.2 / `docs/coding-standards.md` §11.

## Subdirectory map

| Directory | What |
|---|---|
| [`unit/`](unit/) | gtest unit tests, mirrors `src/` — one file per type or behaviour. < 1 ms each. |
| [`integration/`](integration/) | Multi-component flows; real components wired together against fakes (and sometimes a containerised collector). |
| [`conformance/`](conformance/) | End-to-end against a real OpenTelemetry Collector. Validates wire-protocol compliance per spec §2.2 Tier 1. |
| [`wire/`](wire/) | Protocol byte-level fixtures — verifies the encoder and HTTP codec against captured upstream payloads. |
| [`grpc-wire/`](grpc-wire/) | Same idea for the gRPC codec; the corpus from `docs/grpc-wire-protocol.md` §7. |
| [`fuzz/`](fuzz/) | libFuzzer harnesses (TOML parser, gRPC trailer parser, response decompression, OTLP response parser). Required for v1.0 release. |
| [`mocks/`](mocks/) | Dumb mocks per interface — return what they're configured to return, no logic. |
| [`fakes/`](fakes/) | Fakes with logic — fake clock, fake transport, fake reactor, fake server. |

## Conventions

- **Mocks are dumb.** They return what they're configured to return, no
  logic. If you need logic, write a fake. "Smart mocks" are a code
  smell (`CLAUDE.md` rule 4, spec §14.2).
- **Fakes have logic.** A fake clock advances on demand; a fake reactor
  scripts events; a fake transport replays scripted server behaviour.
- **One mock or fake per locked interface** in
  [`docs/interfaces.md`](../docs/interfaces.md) §4. The location of
  each is recorded in that document's per-interface "Mock and fake"
  section.
- **Tests follow coding-standards.md** with these relaxations
  (`coding-standards.md` §11):
  - Test functions may be longer (75-line limit relaxes to 200).
  - Magic numbers in test data are acceptable.
  - Mock/fake classes may use `friend` declarations to access internals.
  - A `_test_only` suffix on a class member opts it out of certain
    rules where the construct exists for testability.
- **Each test file starts with the SPDX header** per
  `coding-standards.md` §8.4.
- **Required test categories per coverage gate** (`coding-standards.md`
  §14.2 / spec §14.2):
  - **Diff coverage** ≥ 90% on SDK + encoder, ≥ 80% on transport +
    exporter — every line added or modified in a PR is exercised by a
    test in the same PR.
  - **Test-presence gate** — any change to `src/**/*.{cpp,hpp}` requires
    a corresponding change to `tests/**/*.{cpp,hpp}` (refactor-only PRs
    use the `[refactor]` label).

## Build

Tests are gated behind `MICROTEL_BUILD_TESTS=ON` (default ON; `OFF`
skips the whole test tree for cross-compilation or constrained builds
per spec §14.2). Wired into the top-level CMakeLists in M2 chunk 2.

## Test data

Test fixtures (canonical encoded payloads, captured collector
responses, malformed-server traces) live alongside the tests that use
them — `tests/wire/encoder/fixtures/`, `tests/grpc-wire/corpus/`. The
fixtures are committed; CI does not regenerate them.
