# microtel CI Architecture

This document describes the CI pipeline structure. The actual workflow files live under `.github/workflows/`. The CI architecture is informed by the requirements in `microtel-spec.md` §14 (Engineering Practices) — the gates listed there are not aspirational; they're the contract.

CI runs on **GitHub Actions** for the OSS path. A self-hosted Jenkins line may be added later for users who fork microtel into private CI environments.

---

## Job overview

### As built (check names as they appear in GitHub)

These are the jobs that exist today. The **check name** column is what branch
protection matches on — see [`branch-protection.md`](branch-protection.md) — and
it is the job's `name:` field, which for matrix jobs is expanded per cell.

| Workflow | Job | Check name(s) | Required? |
|---|---|---|---|
| `ci.yml` | `format-check` | `clang-format` | ✅ |
| `ci.yml` | `tidy-check` | `clang-tidy` | ✅ |
| `ci.yml` | `compile` | `cxx20 / gcc`, `cxx20 / clang`, `cxx23 / gcc`, `cxx23 / clang` | ✅ (cxx20 pair only) |
| `ci.yml` | `sanitizers` | `asan`, `tsan`, `ubsan` | ✅ |
| `ci.yml` | `coverage` | `coverage` | ✅ |
| `ci.yml` | `regen-check` | `regen-check` | ❌ (see below) |
| `ci.yml` | `symbol-scan` | `symbol-scan` | ✅ |
| `ci.yml` | `conformance` | `conformance` | ❌ (see below) |
| `sonarqube.yml` | — | `scan` | ❌ |
| `fuzz.yml`, `soak.yml`, `interop.yml`, `benchmark.yml` | — | scheduled / on-demand | ❌ |

Note that `cxx23 / gcc` and `cxx23 / clang` run on every PR but are **not**
required, so a C++23-only regression can currently merge.

### Design intent (not all implemented)

The table below is the originally designed job set. Several rows —
`build-and-test`, `static-analysis`, `test-presence`, `license-scan`, `release` —
do **not** exist as jobs today; their function is either covered by the as-built
jobs above under different names, or still outstanding. Treat this as the target,
not as a description of the pipeline.

| Job | When | Blocking? | Approx. duration |
|---|---|---|---|
| `build-and-test` | every PR, every push to main | yes | 8–12 min |
| `static-analysis` | every PR, every push to main | yes | 4–6 min |
| `sanitizers` | every PR, every push to main | yes | 15–20 min |
| `coverage` | every PR | yes (diff coverage gate) | 10–15 min |
| `test-presence` | every PR | yes | < 30 sec |
| `license-scan` | every PR, weekly cron | yes (PR), reports (cron) | 3 min |
| `fuzz-smoke` | every PR (60 sec budget per target) | yes | 5 min |
| `fuzz-soak` | nightly cron | reports only | 8 hours |
| `sonarqube` | every PR | yes (no critical/blocker) | 5–10 min |
| `release` | tagged commits | yes | 30+ min |

---

## Per-job detail

### `.github/workflows/build-and-test.yml`

The bread-and-butter check. Builds on a matrix of (compiler × build-type × architecture) and runs every test category.

**Matrix:**
- compiler: `gcc-12`, `gcc-13`, `clang-16`, `clang-17`
- build_type: `Debug`, `Release`, `RelWithDebInfo`
- arch: `x86_64`, `arm64`

**Steps:**
1. Checkout, with submodules.
2. Install deps (`nghttp2`, `openssl`, `zlib`, `cmake`, `ninja`).
3. `cmake --preset ci-${{ matrix.build_type }}-${{ matrix.compiler }}`.
4. `cmake --build build --parallel`.
5. `ctest --test-dir build --output-on-failure`.

**Pass condition:** all matrix cells green.

**As built, for step 5 and the conformance step this section used to list.**
The ctest labels that exist are `unit`, `integration` and `conformance` — set
in [`tests/CMakeLists.txt`](../tests/CMakeLists.txt),
[`tests/integration/CMakeLists.txt`](../tests/integration/CMakeLists.txt) and
[`tests/conformance/CMakeLists.txt`](../tests/conformance/CMakeLists.txt)
respectively. There is no `wire` label: the byte-level wire tests live under
`tests/unit/wire/` (and `tests/unit/wire/grpc/`) and carry `unit`. The
as-built `compile` job runs `ctest` unfiltered rather than by label regex, and
conformance is not a step in any build job — it is the separate `conformance`
job documented below.

### `.github/workflows/static-analysis.yml`

clang-format and clang-tidy enforcement.

**Steps:**
1. Checkout.
2. Install `clang-format-17`, `clang-tidy-17`.
3. Run `clang-format --dry-run --Werror` on `src/`, `include/`, `tests/`. Fails on any drift.
4. Run `clang-tidy` on changed `.cpp` files (using `compile_commands.json` from a CMake build). Fail on any warning.
5. Run `cppcheck` as a secondary check (informational, not blocking).

**Pass condition:** clang-format and clang-tidy clean.

### `.github/workflows/sanitizers.yml`

Three sanitizer build configurations, run separately because they're slow and somewhat incompatible with each other.

**Matrix:**
- sanitizer: `address`, `thread`, `undefined`

**Steps per cell:**
1. Configure with `-DCMAKE_BUILD_TYPE=Debug -DMICROTEL_SANITIZER=${{ matrix.sanitizer }}`.
2. Build.
3. Run unit + integration test labels.
4. Capture sanitizer output; fail on any sanitizer error.

**Pass condition:** all three sanitizer builds green on the same test corpus that `build-and-test` runs.

### `.github/workflows/coverage.yml`

Two coverage measurements: aggregate (must meet the per-area thresholds in spec §14.2) and diff (must cover lines added/modified in this PR).

**Steps:**
1. Build with `--coverage` flag (gcc) or `-fprofile-instr-generate -fcoverage-mapping` (clang).
2. Run unit + integration tests.
3. Generate `lcov` report.
4. Run `diff-cover` against the report, comparing against `origin/main`. Fail if:
   - SDK/encoder paths < 90% line coverage on changed lines
   - Transport/exporter paths < 80% line coverage on changed lines
5. Check aggregate thresholds against the report. Fail if the floor (90% / 85% branch on SDK/encoder; 80% on transport/exporter) is not met.
6. Post the report as a PR comment.

**Pass condition:** both diff coverage and aggregate coverage thresholds met.

### `.github/workflows/test-presence.yml`

Custom check that fails any PR modifying `src/**/*.{cpp,hpp}` without a corresponding modification in `tests/**/*.{cpp,hpp}`.

**Logic:**
```
src_changed = git diff --name-only origin/main HEAD -- 'src/**/*.cpp' 'src/**/*.hpp'
tests_changed = git diff --name-only origin/main HEAD -- 'tests/**/*.cpp' 'tests/**/*.hpp'

if src_changed and not tests_changed:
    if PR has '[refactor]' label or commit message has '[refactor]':
        pass
    elif all changes are formatting/comment-only (verified with structured diff):
        pass
    else:
        fail("src/ changed without tests/. See spec §14.2.")
```

**Pass condition:** test-presence rule satisfied or documented exception applied.

### `regen-check` (job in `.github/workflows/ci.yml`)

Verifies that regenerating the proto accessors produces a zero-diff result against the committed generated code under `gen/`. Prevents drift between `proto/`, the pinned upb version, and the committed generated outputs.

**Steps:**
1. Install pinned `protoc` v29.4 and build `protoc-gen-upb` / `protoc-gen-upb_minitable` from the matching protobuf tag.
2. Run [`ci/scripts/regen-protos.sh`](../ci/scripts/regen-protos.sh) with those binaries.
3. `git diff --exit-code gen/`. Fail if any diff.

**Pass condition:** generated code matches the result of regenerating from pinned sources.

**When it runs:** every PR and every push to `master`. It carries **no path filter** — an earlier version of this document claimed it was skipped unless `proto/`, `third_party/upb/`, or `gen/` changed; that was never implemented.

**Not currently a required status check.** Because it runs unconditionally, requiring it is safe (a path-filtered job could never be required — the check would never report and would block every PR that didn't trigger it). Whether to require it is an open decision; the argument for is that a stale `gen/` means the shipped encoder disagrees with the pinned `opentelemetry-proto`, which is a Tier 1 wire-compatibility break.

### `symbol-scan` (job in `.github/workflows/ci.yml`)

Mechanical enforcement of the dependency closure, in two passes.

**Pass 1 — forbidden namespaces.** No shipped artifact defines **or references**
a symbol from gRPC, abseil, or the protobuf C++ runtime. This is the test behind
CLAUDE.md rule 13 and spec §3 — the closure claim is the project's reason to
exist, so it is verified rather than asserted.

Undefined (`U`) references count as violations alongside defined symbols: a
static archive carrying `U absl::…` makes abseil a link requirement for every
consumer even though the archive contains none of abseil's code.

**Pass 2 — the vendored-upb prefix.** No shipped artifact carries a vendored
upb or utf8_range symbol under its *upstream* name. Per ICP 0020 Decision 4 they
ship renamed to `microtel_*`, so a consumer who also links a real upb cannot get
two definitions of `upb_Arena_Init` and — with static libraries — a silent,
undiagnosable selection between them. The pass fails on any unprefixed `upb_*`,
`_upb_*`, `kUpb_*`, `_kUpb_*`, `kWyhashSalt`, `UPB_linkarr*` or `utf8_range_*`
global.

This pass looks at global linkage only (`nm -g`), which is both the collision
surface and the only deterministic choice: `UPB_INLINE` functions are emitted
out-of-line at `-O0` and inlined away at `-O2`, so an all-linkage scan would
fire on optimization level rather than on a real hazard.

A upb pin bump that introduces a new global fails here rather than shipping it
unprefixed; the fix is to regenerate the list using the recipe in
[`third_party/upb/microtel_upb_rename.h`](../third_party/upb/microtel_upb_rename.h),
never to widen the pattern.

**Steps:**
1. Configure with `-DMICROTEL_BUILD_TESTS=OFF` — the gate must see the shipped
   configuration only, never gtest/gmock or other test-only inputs.
2. Build.
3. Run [`ci/scripts/symbol-scan.sh build`](../ci/scripts/symbol-scan.sh).

**Pass condition:** zero forbidden symbols and zero unprefixed vendored symbols
across every `libmicrotel_*.a` and the `microtel-preflight` binary.

**Deliberate non-violations.** The scan anchors its patterns at the start of the
demangled name, which is what keeps the generated accessors legal: upb emits C
accessors such as `google_protobuf_Timestamp_set_seconds` and
`opentelemetry_proto_trace_v1_Span_set_name`, which are protoc-gen-upb's output
and must not be confused with the `google::protobuf::` C++ runtime. Those
generated names are unchanged by the rename — they are not upb runtime symbols.

Anchoring is also what makes pass 2 self-consistent: a correctly renamed symbol
begins with `microtel_`, so it can never match a pattern anchored on the
upstream name.

**A scan that finds no artifacts fails with exit 2** rather than reporting green,
so a build-layout change cannot silently turn this gate into a no-op.

**Known gap — re-point at the install tree when `install()` lands.** The scan
currently walks the *build* tree. "Shipped" properly means the *install* tree;
the two coincide today only because the project has no `install(TARGETS …
EXPORT …)` rules yet (issue #19). When those land, this job must be re-pointed at
`cmake --install` output — or at minimum extended to cover it — in the same PR.
Otherwise install rules ship and the gate quietly begins checking the wrong set
of artifacts while still reporting green.

### `conformance` (job in `.github/workflows/ci.yml`)

The spec §13.5 Tier 1 gate: a real OpenTelemetry Collector accepts what
microtel emits, and what the collector decodes is what microtel meant. A mock
cannot discharge that claim — it is about a receiver microtel's authors did not
write — so the collector is as much the system under test as microtel is. The
tier itself is documented in
[`tests/conformance/README.md`](../tests/conformance/README.md).

**Steps:**
1. Install clang-18 plus `libssl-dev`, `libnghttp2-dev`, `zlib1g-dev`.
2. Configure with `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_STANDARD=20
   -DMICROTEL_BUILD_TESTS=ON`.
3. Build.
4. Run [`ci/scripts/conformance.sh build`](../ci/scripts/conformance.sh). That
   script generates a throwaway certificate set, starts the collector image
   pinned in [`interop-matrix.md`](interop-matrix.md), waits up to 60 s on its
   `health_check` extension, refuses to continue if no test carries the
   `conformance` label, exports the endpoint / certificate / output-file
   environment contract, and runs `ctest -L conformance`.

Engine selection is the script's: `podman` if present, else `docker` (docker is
what the GitHub-hosted runner has). The job's timeout is 30 minutes because
pulling the collector image dominates; the tests themselves are seconds.

**Pass condition:** every `conformance`-labelled test passes. The script exits
1 when a test fails and 2 when the gate could not run at all — no container
engine, the collector never became healthy, or nothing carried the label — so a
gate that ran nothing cannot report green.

**Why every other job stays green without a collector.** `compile`,
`sanitizers`, `otelcpp-shim` and [`coverage.sh`](../ci/scripts/coverage.sh) all
run `ctest` unfiltered, so the conformance binaries execute there too and skip,
because the `MICROTEL_CONFORMANCE_*` variables are exported only by the runner.
To stop that skip from masquerading as a pass *inside* the gate, the runner also
exports `MICROTEL_CONFORMANCE_REQUIRE`: with it set, an unset variable is a test
failure rather than a skip
([`tests/conformance/support/conformance_env.hpp`](../tests/conformance/support/conformance_env.hpp)).

**Not currently a required status check.** The required list is in
[`branch-protection.md`](branch-protection.md) and `conformance` is not on it,
so a Tier 1 wire regression can merge today. Whether to require it is an open
decision: the argument for is that Tier 1 is the compatibility promise spec
§2.2 makes testable, and the argument against is making every PR depend on
pulling a third-party container image.

### `.github/workflows/license-scan.yml`

License compliance check over vendored and generated code.

**Steps:**
1. Run a license-detection tool (e.g., `scancode-toolkit` or `licensee`) over `third_party/`.
2. Verify each `third_party/<dep>/README.md` records the upstream license and SHA.
3. Generate `THIRD_PARTY_NOTICES.md` from the vendored licenses; fail if it differs from the committed version.

**Pass condition:** all vendored licenses recognized and compatible with Apache 2.0; notices file matches.

### `.github/workflows/fuzz-smoke.yml` and `fuzz-soak.yml`

Fuzz targets: TOML parser, gRPC trailer parser, response decompression, OTLP response parser.

- **Smoke:** 60-second budget per target on every PR. Fail on crashes or sanitizer errors.
- **Soak:** 8-hour overnight cron. Crashes generate issues; corpus is uploaded to artifact storage for replay.

### `.github/workflows/sonarqube.yml`

Runs **SonarQube Cloud** on the project's OSS tier — free for public/open-source repositories with no LOC cap, full feature set including the C++ analyzer (`cfamily`) and inline PR decoration. Triggered on every PR (not a periodic cron).

**Steps:**
1. Checkout with full history (`fetch-depth: 0` — Sonar uses git blame for issue annotation).
2. Set up build wrapper for the C++ analyzer (`build-wrapper-linux-x86-64`).
3. Configure CMake.
4. Build under the build wrapper to capture compile commands.
5. Upload the lcov coverage report from the `coverage` job's artifacts.
6. Run `sonar-scanner` with the project key and organization configured for SonarQube Cloud.
7. Wait for the quality gate result; fail the PR on critical or blocker issues.

**Pass condition:** SonarQube Cloud quality gate passes — no critical or blocker issues introduced by the PR.

**Configuration:**
- `sonar-project.properties` (at the repo root — required by SonarCloud's automatic analysis discovery) — project key, organization, source paths, exclusions, coverage report path.
- `SONAR_TOKEN` GitHub Actions secret — generated in SonarQube Cloud, stored in repo secrets.

**Why SonarQube Cloud OSS tier vs self-hosted Community Build:** the cloud OSS tier is free for public projects and includes the C++ analyzer, branch analysis, and PR decoration — all of which the self-hosted Community Build lacks without paid Developer Edition. For a public OSS project, the cloud tier is the strict superset at zero cost.

### `.github/workflows/release.yml`

Triggered on tags matching `v*.*.*`. Builds and publishes:

- `.deb` packages.
- `.rpm` packages.
- Python wheels via `cibuildwheel` for manylinux_2_28.
- A source tarball with `THIRD_PARTY_NOTICES.md` included.
- A SBOM (CycloneDX or SPDX format).
- GitHub Release with all artifacts attached.

Release builds run all CI gates first as a prerequisite.

---

## Required secrets

The CI configuration expects these GitHub Actions secrets:

| Secret | Purpose |
|---|---|
| `PYPI_API_TOKEN` | publishing Python wheels |
| `SONAR_TOKEN` | required for the SonarQube Cloud scan |
| `CODECOV_TOKEN` | optional; only if using Codecov for coverage upload alongside SonarQube |

---

## Status badges (for README.md)

```markdown
![build](https://github.com/<org>/microtel/actions/workflows/build-and-test.yml/badge.svg)
![sanitizers](https://github.com/<org>/microtel/actions/workflows/sanitizers.yml/badge.svg)
![coverage](https://codecov.io/gh/<org>/microtel/branch/main/graph/badge.svg)
```

---

## Local pre-commit equivalents

Developers can run the equivalent of CI gates locally. Recommended setup:

```bash
# install pre-commit hooks
pre-commit install

# run all checks manually
pre-commit run --all-files

# run the test-presence check on a working PR
ci/scripts/check-test-presence.sh
```

The `ci/scripts/` directory holds the shared scripts called by both the workflow files and the pre-commit hooks, so local and CI behavior stays in sync.

---

## What's NOT in CI

- **GUI / GUI-test runs.** No GUI in microtel.
- **Performance benchmarks.** Live in the separate `microtel-bench` repo (per spec M7); not part of per-PR CI.
- **macOS / Windows builds.** Out of scope for v1; not in the matrix.
- **End-to-end integration with downstream backends** (Datadog, New Relic). The `tests/conformance/` job runs against an OTel Collector only.
