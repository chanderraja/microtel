# `tests/`

All of microtel's tests. The layout follows the test taxonomy in
`microtel-spec.md` §14.2 and `docs/coding-standards.md` §11.

## Subdirectory map

| Directory | What |
|---|---|
| [`unit/`](unit/) | gtest unit tests, mirroring `src/`, one file per type or behaviour. Under 1 ms each. |
| [`integration/`](integration/) | Real components wired together, with fakes or an in-process server at the system boundary. |
| [`conformance/`](conformance/) | End-to-end against a real OpenTelemetry Collector. Checks wire-protocol compliance (spec §2.2, Tier 1). |
| [`wire/`](wire/) | Index of the byte-level encoder and OTLP/HTTP codec coverage. Holds no tests itself; the tests live under `unit/wire/`. |
| [`grpc-wire/`](grpc-wire/) | The same index for the gRPC response corpus in `docs/grpc-wire-protocol.md` §7. |
| [`fuzz/`](fuzz/) | libFuzzer harnesses: TOML parser, gRPC response parser, response decompression, OTLP response parser, and the v1.1 baggage and `Provider`-setter harnesses. |
| [`consumer/`](consumer/) | The external `find_package(microtel)` project from ICP 0020 Decision 6. It is **not part of this build**: `ci/scripts/consumer-smoke.sh` drives it against a real install prefix. It doubles as the canonical consumer example. |
| [`mocks/`](mocks/) | Dumb mocks, one per interface. They return what they're configured to return and hold no logic. |
| [`fakes/`](fakes/) | Test doubles with logic: fake clocks, transport, reactor, wire codec, and the public-API fakes the otelcpp shim tests use. |
| `helpers/` | Shared test-only utilities. Currently `gunzip.hpp`, an independent inflate the gzip round-trip tests check production against. |

## Conventions

- Mocks are dumb. They return what they're configured to return and do
  nothing else. If you need logic, write a fake. A "smart mock" is a
  code smell (`CLAUDE.md` rule 4, spec §14.2).
- Fakes have logic. A fake clock advances on demand, a fake reactor
  scripts events, and a fake transport serves scripted results.
- Each locked interface in [`docs/interfaces.md`](../docs/interfaces.md)
  §4 has a mock or a fake. Which one, and where it lives, is recorded in
  that interface's "Mock and fake" section.
- Tests follow `coding-standards.md`, with the relaxations in its §11:
  - Test functions may be longer (the 75-line limit relaxes to 200).
  - Magic numbers in test data are fine.
  - Mock and fake classes may use `friend` declarations to reach
    internals.
  - A `_test_only` suffix on a class member opts it out of certain rules
    where the construct exists for testability.
- Every test file starts with the SPDX header (`coding-standards.md`
  §8.4).
- Two coverage gates apply to every PR (`coding-standards.md` §14.2,
  spec §14.2):
  - Diff coverage: at least 90% on SDK and encoder code and 80% on
    transport and exporter code. Every line a PR adds or modifies must
    be exercised by a test in the same PR.
  - Test presence: a change to `src/**/*.{cpp,hpp}` needs a matching
    change to `tests/**/*.{cpp,hpp}`. Refactor-only PRs use the
    `[refactor]` label.

## Build

The tree is gated behind `MICROTEL_BUILD_TESTS`, which defaults to `ON`.
Set it to `OFF` for cross-compilation or constrained builds (spec
§14.2). Tests are registered with ctest under the labels `unit`,
`integration` and `conformance`, so `ctest -L unit` runs one tier.
The fuzz harnesses build separately, under `MICROTEL_BUILD_FUZZ=ON`
(see [`fuzz/`](fuzz/)).

## Test data

Most fixtures are built in the test source, byte for byte, rather than
loaded from disk: encoded payloads, collector responses and malformed
server replies are all expressed in C++. The exceptions are the fuzz
seed and crash inputs under `tests/fuzz/corpus/` and
`tests/fuzz/crashes/`, and the collector config under
`tests/conformance/collector/`. All of it is committed; CI regenerates
none of it.
