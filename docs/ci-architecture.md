# microtel CI Architecture

This document describes the CI pipeline structure. The actual workflow files live under `.github/workflows/`. The CI architecture is informed by the requirements in `microtel-spec.md` §14 (Engineering Practices) — the gates listed there are not aspirational; they're the contract.

CI runs on **GitHub Actions** for the OSS path. A self-hosted Jenkins line may be added later for users who fork microtel into private CI environments.

---

## Job overview

| Job | When | Blocking? | Approx. duration |
|---|---|---|---|
| `build-and-test` | every PR, every push to main | yes | 8–12 min |
| `static-analysis` | every PR, every push to main | yes | 4–6 min |
| `sanitizers` | every PR, every push to main | yes | 15–20 min |
| `coverage` | every PR | yes (diff coverage gate) | 10–15 min |
| `test-presence` | every PR | yes | < 30 sec |
| `proto-regen-check` | every PR touching `third_party/upb/` or `proto/` | yes | 2 min |
| `symbol-scan` | every PR | yes | 3–5 min |
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
5. `ctest --test-dir build --output-on-failure --label-regex 'unit|integration|wire'`.
6. `ctest --test-dir build --output-on-failure --label-regex 'conformance'` — runs against a collector container started in-job.

**Pass condition:** all matrix cells green.

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

### `.github/workflows/proto-regen-check.yml`

Verifies that running `make regen-protos` produces a zero-diff result against the committed generated code under `gen/`. Prevents drift between `proto/`, the pinned upb commit, and the committed generated outputs.

**Steps:**
1. Checkout with submodules.
2. Run `make regen-protos`.
3. `git diff --exit-code gen/`. Fail if any diff.

**Pass condition:** generated code matches the result of regenerating from pinned sources.

**When it runs:** on PRs that touch `proto/`, `third_party/upb/`, `gen/`, or any `*.upb*` files. Skipped otherwise to save CI time.

### `symbol-scan` (job in `.github/workflows/ci.yml`)

Mechanical enforcement of the dependency closure: asserts that no shipped
artifact defines **or references** a symbol from gRPC, abseil, or the protobuf
C++ runtime. This is the test behind CLAUDE.md rule 13 and spec §3 — the closure
claim is the project's reason to exist, so it is verified rather than asserted.

Undefined (`U`) references count as violations alongside defined symbols: a
static archive carrying `U absl::…` makes abseil a link requirement for every
consumer even though the archive contains none of abseil's code.

**Steps:**
1. Configure with `-DMICROTEL_BUILD_TESTS=OFF` — the gate must see the shipped
   configuration only, never gtest/gmock or other test-only inputs.
2. Build.
3. Run [`ci/scripts/symbol-scan.sh build`](../ci/scripts/symbol-scan.sh).

**Pass condition:** zero forbidden symbols across every `libmicrotel_*.a` and the
`microtel-preflight` binary.

**Deliberate non-violations.** The scan anchors its patterns at the start of the
demangled name, which is what keeps the vendored dependencies legal: upb emits C
accessors such as `google_protobuf_Timestamp_set_seconds`, which are upb's own
generated code and must not be confused with the `google::protobuf::` C++
runtime. `upb_*` and `utf8_range_*` are likewise permitted members of the
closure.

**A scan that finds no artifacts fails with exit 2** rather than reporting green,
so a build-layout change cannot silently turn this gate into a no-op.

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
