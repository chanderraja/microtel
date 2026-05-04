# ICP 0001: M0 deliverables clarification

**Status:** Accepted
**Affected interfaces / docs:** `CLAUDE.md`, `microtel-spec.md` §14.1, `docs/repository-layout.md` §1 and §2
**Affected tracks:** none (docs only; pre-M0-close)

## Summary

Clarify that the M0 deliverable set covers (a) public C++ headers in `include/microtel/` alongside the internal interface stubs in `include/microtel/internal/`; (b) `docs/configuration.md` and `docs/development.md`; and (c) eight sequence diagrams in `docs/sequences/` rather than five.

## Motivation

Three artifacts disagreed on M0 scope:

- `CLAUDE.md` (Hard rules, rule 1) emphasized only `include/microtel/internal/` for header stubs, leaving the public API headers ambiguous.
- `microtel-spec.md` §14.1 listed six sub-bullets of M0 deliverables but did not include `docs/configuration.md` or `docs/development.md`, even though both are referenced as durable artifacts elsewhere in the spec (§12.1 and §13.3 respectively).
- `docs/repository-layout.md` §1 listed both files in the docs/ tree without `# M0 deliverable` annotation, and §2's "What exists at end of M0" tree omitted them.

The public-API shape (`Tracer`, `Span`, `Provider`, `SdkBuilder`, `Resource`, `StartSpanOptions`, `BatchOptions`, status / error types) is itself an architectural commitment. Deferring it to M2 turns M2 into "decide the public API and build the skeleton" instead of just "build the skeleton against locked interfaces" — undermining M0's purpose.

`docs/configuration.md` documents per-setting precedence (spec §12.1) and is needed on day one as the canonical home for "which env var beats which TOML key." `docs/development.md` is the track-to-directory atlas referenced by spec §13.3; the tracks are defined as of M0 even if their directories are empty.

The original five sequence diagrams (connection establishment, retry-after-failure, GOAWAY handling, shutdown drain, fork survival) miss three flows where production correctness is at stake and the right behavior is non-obvious from prose: backpressure-and-drop, partial-success, and gRPC trailer-only / multi-frame parsing.

## Proposed change

### `CLAUDE.md`, Hard rules > Phase discipline > rule 1

Replace:

> No source code in M0. Only documents in `docs/` and compilable header stubs in `include/microtel/internal/`. Headers contain pure-virtual / abstract declarations with full Doxygen comments — no implementations. Implementation begins in M3.

With:

> No source code in M0. Only documents in `docs/` and compilable header stubs in `include/microtel/` (public API) and `include/microtel/internal/` (internal interfaces). Public headers carry full Doxygen and method signatures with no bodies; internal interface headers are pure-virtual / abstract with full Doxygen. Both compile under an `INTERFACE` CMake target with `-Werror`. No method implementations. Implementation begins in M3.

### `microtel-spec.md` §14.1

Append to the M0 deliverables list:

- `include/microtel/*.hpp` — public API headers with full Doxygen, method signatures, no bodies. Compile under an `INTERFACE` target.
- `docs/configuration.md` — per-setting precedence table (spec §12.1) and resolution rules. New settings appended as they land in M3+.
- `docs/development.md` — track-to-directory atlas (spec §13.1, §13.3).

Update the sequence-diagrams sub-bullet to list eight flows: connection establishment, retry-after-failure, GOAWAY handling, shutdown drain, fork survival, **backpressure-and-drop**, **partial-success handling**, **gRPC trailer-only and multi-frame parsing**.

### `docs/repository-layout.md`

§1 docs/ tree: tag `configuration.md` and `development.md` as `# M0 deliverable`. Replace the placeholder five-file `sequences/` listing with the locked-in eight.

§2 "What exists at end of M0" tree: add `configuration.md`, `development.md`, and the three additional sequence files (`backpressure-and-drop.md`, `partial-success.md`, `grpc-trailer-only-and-multi-frame.md`).

## Migration

None. M0 has not closed; no contracts have been signed off. This ICP is captured as a heads-up document and to establish the ICP pattern as the first numbered entry under `docs/icps/`.

Future contributors and agents reading the spec, CLAUDE.md, or repository-layout.md will see a consistent M0 scope.

## Rationale & alternatives

**Alternative for public headers:** defer to M2. Rejected because the public API is an architectural commitment, not a skeleton concern. Locking it in M0 lets M0 reviewer sign-off cover the surface that users see, not just the surface contributors see.

**Alternative for `docs/configuration.md` / `docs/development.md`:** defer to M3 / M6 when the things they document are partly built. Rejected because both are precedence/atlas documents whose structure is known from the spec; appending entries as features land is cheaper than starting them late.

**Alternative for sequence-diagram count:** keep at five and add the other three as needed. Rejected because the three additions are exactly the flows where implementations get written against assumed shapes — diagrams in M0 are how that's prevented. Eight is still small enough that each diagram is a half-page artifact.
