# ICP 0005: Move benchmark harness into the main repo (bench/)

**Status:** Draft
**Affected interfaces / docs:** `microtel-spec.md` §13 (M7 row),
`microtel-roadmap.md` (M7 framing, §9 benchmark trajectory),
`docs/bench-spec.md` (all location references), `CMakeLists.txt`,
`.github/workflows/` (new `benchmark.yml`)
**Affected tracks:** None (docs / infra only)
**Spec / roadmap impact:** Supersedes the implicit separate-repo assumption
in the M7 row of `microtel-spec.md` §13 and the `microtel-bench` companion
framing throughout `microtel-roadmap.md`.

## Summary

Benchmarks live in `microtel/bench/` behind `MICROTEL_BUILD_BENCH=ON`,
not in a separate `microtel-bench` repository.

## Motivation

The M7 milestone row in the spec and several passages in the roadmap assume
that benchmarks will live in a companion repo called `microtel-bench`. This
assumption was a drafting convenience, not a deliberate architecture
decision. Before scaffolding anything for M7, it is worth evaluating it
explicitly.

At the current project stage — pre-1.0, interfaces still settling, a small
team — the separate-repo shape creates real coordination overhead:

- Every interface refinement may require a coordinated PR across two repos
  to keep the benchmark SUT and the library in sync.
- Reproducibility requires cross-repo commit pairs; `git bisect` and CI
  artifact linkage become non-trivial.
- The "clean separation" benefit is only meaningful once the benchmark tool
  is a general-purpose OTLP-exporter comparison framework usable by other
  projects. That is a v2+ concern.

The "heavy dependencies" and "slow CI" concerns (Docker images, otel-cpp
comparison builds) are addressed by the opt-in CMake flag and a separate
cron workflow, not by a separate repository.

## Proposed change

### 1. `microtel-spec.md` §13 M7 row

Drop the parenthetical `(separate microtel-bench repo)`. The M7 deliverable
description becomes:

> Benchmark harness in `bench/` measuring all hot-path / exporter / footprint
> metrics defined in §10. Validates the size and CPU claims that justify the
> project's existence. Opt-in: `cmake -DMICROTEL_BUILD_BENCH=ON`.

### 2. `microtel-roadmap.md`

- M7 framing section: replace all `microtel-bench repo` references with
  `bench/ directory`.
- §9 cross-cutting threads → "Performance benchmarks" paragraph: update
  the per-version trajectory to reflect the in-repo location.

### 3. `docs/bench-spec.md`

- Retitle from `microtel-bench: Benchmark Harness Spec` to
  `microtel bench/: Benchmark Harness Spec`.
- Update the repo-layout tree (`microtel-bench/ …`) to reflect `bench/`
  inside the main repo.
- Update all prose references from "the `microtel-bench` repo" to
  "the `bench/` directory".

### 4. Root `CMakeLists.txt`

Add opt-in flag, mirroring `MICROTEL_BUILD_TESTS`:

```cmake
option(MICROTEL_BUILD_BENCH "Build the benchmark harness (requires Docker)" OFF)

if(MICROTEL_BUILD_BENCH)
    add_subdirectory(bench)
endif()
```

### 5. `.github/workflows/benchmark.yml` (new file)

A scheduled + manually-triggerable workflow. Runs on `cron` (weekly or
nightly), not on every PR. Does not block merge. Reports results as
workflow artifacts and, optionally, a GitHub Actions summary.

## Migration

After this ICP is accepted and implemented:

- Any reference to `microtel-bench` in documentation, comments, or agent
  instructions should be replaced with `bench/` or `microtel/bench/`.
- `CLAUDE.md` requires no change (no current bench references).
- Contributors scaffolding M7 work `mkdir bench/` in the main repo, not
  `git clone microtel-bench`.

## Rationale and alternatives

### Alternative A: Keep separate repo (spec as written)

**Pros:** Clean boundary once the harness matures into a general-purpose
comparison tool; no Docker build complexity leaks into the main repo CI.

**Cons:** Cross-repo coordination cost is front-loaded onto a pre-1.0
project where interfaces are still settling. The "clean boundary" benefit
arrives at a time (v2+) when extraction is still easy; extraction from
in-repo is low-cost, the reverse merge is not.

**Verdict:** Deferred to the reversibility note below; not selected now.

### Alternative B: In-repo, always built (`MICROTEL_BUILD_BENCH` always ON)

**Pros:** Simpler CMake.

**Cons:** Forces Docker as a dev dependency on everyone. Unacceptable.

**Verdict:** Rejected.

### Selected: In-repo, opt-in (this ICP)

Same pattern as `MICROTEL_BUILD_TESTS`. Benchmarks are part of the tree,
visible in PRs, atomically versioned with the library, but never imposed
on contributors who just want to build and test the library.

**Reversibility:** If the harness later grows into a general-purpose
OTLP-exporter comparison tool that other projects consume, the `bench/`
directory can be extracted into its own repo at that point without loss of
history (`git filter-repo --subdirectory-filter bench/`). The reverse
direction — merging a separate repo's history — is more disruptive.

## Sign-off

| Reviewer | Date | Status |
|---|---|---|
| | | Pending |
