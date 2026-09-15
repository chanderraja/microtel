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
| `ci.yml` | `test-presence` | `test-presence` | ❌ (see below) |
| `ci.yml` | `regen-check` | `regen-check` | ❌ (see below) |
| `ci.yml` | `symbol-scan` | `symbol-scan` | ✅ |
| `ci.yml` | `version-drift-check` | `version-drift-check` | ✅ |
| `ci.yml` | `conformance` | `conformance` | ❌ (see below) |
| `sonarqube.yml` | — | `scan` | ❌ |
| `fuzz.yml`, `soak.yml`, `interop.yml`, `benchmark.yml` | — | scheduled / on-demand | ❌ |

Note that `cxx23 / gcc` and `cxx23 / clang` run on every PR but are **not**
required, so a C++23-only regression can currently merge.

### Design intent (not all implemented)

The table below is the originally designed job set. Several rows —
`build-and-test`, `static-analysis`, `license-scan`, `release` — do **not** exist
as jobs today; their function is either covered by the as-built jobs above under
different names, or still outstanding. Treat this as the target, not as a
description of the pipeline.

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

### `coverage` (job in `.github/workflows/ci.yml`)

Two independent coverage measurements, both against the same filtered lcov tracefile: **aggregate** (the whole tree must meet the spec §14.2 floors) and **diff** (the lines this PR touched must be covered).

**Steps:**
1. Checkout at `fetch-depth: 0` — the diff gate needs `origin/master` as a base.
2. [`ci/scripts/coverage.sh build`](../ci/scripts/coverage.sh) configures with `-DMICROTEL_COVERAGE=ON` (clang `-fprofile-instr-generate -fcoverage-mapping`), builds, runs `ctest` with `LLVM_PROFILE_FILE` pointed at a per-process `.profraw`, merges them with `llvm-profdata`, and exports a filtered lcov tracefile with `llvm-cov export -format=lcov`. `llvm-profdata` must be exactly the compiler's major version and `llvm-cov` at least 21; the script checks both and refuses to run otherwise.
3. The same script then enforces the aggregate floors, and **fails the job** below them. It reduces the tracefile to one row per file (`build/coverage.per-file.tsv`, uploaded with the report) and sums those rows into two groups. The group mapping is stated in the script's header; briefly:

   | Group | Paths | Floor |
   |---|---|---|
   | `sdk-encoder` | `include/microtel/**`, `src/api/`, `src/sdk/`, `src/common/`, `src/wire/encoder/` | ≥ 90% line, ≥ 85% branch |
   | `transport-exporter` | `src/transport/`, `src/exporter/`, `src/adapters/`, rest of `src/wire/`, `tools/` | ≥ 80% line |

   Nothing that survives the tracefile filter is exempt. A path matching neither rule is gated as `sdk-encoder` — the stricter floor — and named in the output, so a new directory cannot dodge the gate by going unmentioned.
4. `diff-cover` (pip, PR events only) against the same tracefile with `--compare-branch origin/<base>` and `--fail-under=80`.

**Branch coverage is enforced** (issue #198). It was not, under gcov: gcov records an edge for the unwind path out of every potentially-throwing call, so `include/microtel/meter.hpp` measured 100% line and 50% branch with its "uncovered" branches on lines holding no conditional at all, and whole-tree branch coverage read 57.8% against 91.0% line. Clang's source-based coverage attaches counters to source *regions* the front end knows about — the same `meter.hpp` now reports **0 branch records**, because it contains no conditional — so the §14.2 branch floor measures program logic and fails the job like the line floors do. The 85% threshold was never lowered while it was unenforceable, and it is not raised now. Spec §13.5 gate 11 is discharged for line and branch.

**Why clang-only.** The gate's numbers are the compiler's numbers, so the compiler is part of the gate. `coverage.sh` refuses to run under gcc rather than silently measuring something else, and checks its llvm tools rather than surfacing a version skew as a corrupt-looking file three steps later.

**Why the exporter is newer than the compiler** (issue #236). The build uses Ubuntu's `clang-18`, the same compiler as the `compile` matrix; the export uses `llvm-cov` **21** from `apt.llvm.org`, and `coverage.sh` refuses to run below that. Up to `llvm-cov` 20, `export` emits one set of branch records per *template instantiation* while emitting line records already merged across them — so the gate would compare a merged line percentage against a per-instantiation branch percentage, which §14.2 states as if the two were commensurable. `llvm-cov` 21 applies the same merge to both. Bisected on one `clang-19` object and profile, a function template with one `if` instantiated twice: `llvm-cov` 19 and 20 emit 4 `BRDA` records, 21 and 22 emit 2. On this tree that is `sdk-encoder` reading 81.48% (1219/1496) versus 85.59% (1099/1284) branch, from identical line coverage of 4757/5189. Newer llvm tools read older coverage-mapping and profile formats, so the built code is unchanged.

`llvm-profdata`, by contrast, must match the compiler **exactly**: it reads the *raw* profiles the instrumented binaries write, and that format is locked to the compiler's release in both directions — `llvm-profdata-21` rejects `clang-18`'s raw version 9 with "raw profile version mismatch … expected version = 10" and then "no profile can be merged". So the job installs `clang-18`, `llvm-18` and `llvm-21`, and pairs `clang-18` + `llvm-profdata-18` + `llvm-cov-21`. `llvm-cov` reads the *indexed* profile and the coverage mapping, both of which a newer reader accepts.

The LLVM apt repository is added to the two jobs that run `coverage.sh` — `coverage` here and `scan` in `sonarqube.yml` — and deliberately to no others; `clang-format` and `clang-tidy` stay on their pinned Ubuntu 18 packages. The tradeoff is that `apt.llvm.org` becomes a network dependency of a required check: if it flakes, re-run the job.

**Why `--fail-under=80` when §14.2 names two diff thresholds.** `diff-cover` takes a single threshold and does not partition by path. 80 is the floor that holds everywhere; the 90 for SDK/encoder is carried by the aggregate gate in step 3, which *is* measured per group. A PR that drags `sdk-encoder` below 90 fails step 3 whatever step 4 reports.

`diff-cover` is a pip package installed in the job and used only at test time. It is not part of microtel's runtime dependency closure (CLAUDE.md rule 12).

**Pass condition:** both enforced aggregate floors met, and diff coverage ≥ 80% on the changed lines. A PR whose diff touches no covered source (a docs- or CI-only PR, for example) reports "no lines with coverage information in this diff" and passes.

### `test-presence` (job in `.github/workflows/ci.yml`)

[`ci/scripts/test-presence.sh`](../ci/scripts/test-presence.sh) fails any PR modifying `src/**/*.{cpp,hpp}` without a corresponding modification to `tests/**/*.{cpp,hpp}`. It is a presence check, not a coverage check — the diff-coverage gate in `coverage` proves the new lines are exercised; this proves a test file moved at all, and says so in seconds rather than after a 10-minute instrumented build.

**Logic:**
```
merge_base   = git merge-base origin/<base> HEAD
src_changed   = git diff --name-only --diff-filter=d merge_base..HEAD -- src   | grep '\.(cpp|hpp)$'
tests_changed = git diff --name-only --diff-filter=d merge_base..HEAD -- tests | grep '\.(cpp|hpp)$'

if src_changed and not tests_changed:
    if '[refactor]' in PR labels:  pass
    else:                          fail
```

Labels reach the script through `MICROTEL_PR_LABELS`, which the workflow fills from `github.event.pull_request.labels.*.name`; the script also falls back to parsing `GITHUB_EVENT_PATH` with `jq`, and honours `MICROTEL_PR_LABELS` directly for local dry-runs.

**Exceptions**, per CLAUDE.md rule 3:
- *Code deletions* — handled by `--diff-filter=d`, which drops deleted files from the changed-source set.
- *Pure refactors* — the `[refactor]` PR label.
- *Comment/formatting-only changes* — **not** detected automatically. Classifying a hunk as semantically empty needs a structured diff, and a wrong answer silently disables the gate. The `[refactor]` label is the manual override for this case too; a label is visible on the PR where a heuristic would not be.

**Not currently a required status check.** It runs on `pull_request` only (it needs a base to diff against), so requiring it would block pushes to `master`, and it is new enough to want a few PRs of observation first.

**Exit codes:** 0 satisfied or waived, 1 rule violated, 2 the gate could not run (base ref unreachable — in CI that means the checkout was too shallow).

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
3. `cmake --install build --prefix install-tree`.
4. Run [`ci/scripts/symbol-scan.sh --prefix install-tree`](../ci/scripts/symbol-scan.sh).

**It scans the install tree, not the build tree** (ICP 0020 Decision 5). Once
`cmake --install` exists, "shipped" means what `cmake --install` produces;
scanning the build directory would verify the closure claim against artifacts
that are not the ones users receive. The narrowing is real and intended —
`libmicrotel_preflight_lib.a` is in the build tree and is deliberately not
installed, so it is no longer scanned. The script keeps its build-directory
form (`symbol-scan.sh [build-dir]`) for the quicker local loop.

**Pass condition:** zero forbidden symbols and zero unprefixed vendored symbols
across every installed `libmicrotel_*.a` and the installed `microtel-preflight`
binary.

### `version-drift-check` (job in `.github/workflows/ci.yml`)

[`ci/scripts/version-drift-check.sh`](../ci/scripts/version-drift-check.sh)
compares every hand-written version literal against `project(microtel VERSION …)`
in the top-level `CMakeLists.txt`, which is the authority. The literals are
`kVersionString` and the `kVersionMajor`/`Minor`/`Patch` triple in
`include/microtel/version.hpp`, the gRPC `kUserAgent` in
`src/wire/grpc/grpc_wire_codec.cpp`, and `kVersion` in
`tools/preflight/preflight.cpp`. The last two reach collectors — as the
`user-agent` export header (spec §7.2) and the `microtel.version` span attribute
(spec §6.4) — which is why drift is a wire-visible bug and not bookkeeping.

**It is a check, not a generator.** Deriving `version.hpp` from `PROJECT_VERSION`
at configure time would turn a public header into a build artifact that the
header-only `microtel_headers` target, the M0 header check, and the install
surface all read as plain source. [`RELEASING.md`](../RELEASING.md) records that
decision, the full bump procedure, and why `master` carries no `-dev` suffix.

Zero matches and duplicate matches are both hard failures (exit 2), not passes:
a pattern that stops matching after an unrelated rename would otherwise turn the
gate into a no-op that still reports green — the same principle as `symbol-scan`
failing when it finds no artifacts.

**Steps:**
1. `ci/scripts/version-drift-check.sh` — the gate.
2. `ci/scripts/version-drift-check.sh --self-test` — the gate checking itself
   against synthetic fixture trees: one per drift shape, plus the missing- and
   duplicate-literal cases, asserting the exit code for each.

The job installs no toolchain and builds nothing; it reads source text and
answers in seconds.

**Pass condition:** every literal equals `PROJECT_VERSION`, and the self-test's
nine cases all produce their expected exit codes.

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

**When `SONAR_TOKEN` is absent the job exits 0 but is no longer silent.** Requiring the secret would block every PR on a maintainer-only setup step, so the job stays green — but a green check that means "nothing ran" is worse than no check, because it reads as "analysed and clean". The skip now emits a `::warning::` annotation and writes a **NO SONAR SCAN RAN** heading to `$GITHUB_STEP_SUMMARY` stating that spec §13.5 gate 13 is UNMEASURED, with the maintainer steps to fix it. The scanned path writes its own counterpart heading, so the two outcomes are distinguishable from the summary alone.

The workflow also carries a `workflow_dispatch` trigger, so a scan can be fired from the Actions tab the moment the secret lands, rather than waiting for the next merge to `master`.

`ci/scripts/coverage.sh` runs here with `MICROTEL_COVERAGE_ENFORCE=0`: this job wants the tracefile, not a second opinion on the §14.2 floors. The `coverage` job owns that gate, and letting a shortfall abort this job too would suppress the Sonar scan exactly when the code most needs looking at.

**Coverage import (issue #211).** The tracefile is *not* handed to the C++ analyzer. It is converted by `ci/scripts/lcov-to-sonar.py` into SonarQube's [generic test coverage XML](https://docs.sonarsource.com/sonarqube-cloud/enriching/test-coverage/generic-test-data/) and imported via `sonar.coverageReportPaths`. The previous configuration passed the lcov `.info` to `sonar.cfamily.llvm-cov.reportPath`, which names an *llvm-cov* report: the sensor parsed the file in ~81 ms, imported nothing, and warned about nothing, so the whole project read 0.0% coverage against `coverage.sh`'s measured ~91% and the default gate's `new_coverage ≥ 80` condition failed on every PR. Compounding it, the tracefile carries absolute `SF:` paths that do not match the repo-relative keys SonarQube indexes files under; the converter rebases them. It emits **lines only**. That used to be because gcov's branch data was an artefact; since issue #198 the `BRDA` records are real, and what remains is that importing conditions moves SonarQube's own `coverage` measure and its new-code gate — a separate decision from the CI gate, left open in the converter's header. `--include src --include include` mirrors `sonar.sources`. The converter exits non-zero rather than writing an empty report, so the silent-zero failure mode cannot recur unnoticed.

**Configuration:**
- `sonar-project.properties` (at the repo root — required by SonarCloud's automatic analysis discovery) — project key, organization, source paths, exclusions, coverage report path.
- `SONAR_TOKEN` GitHub Actions secret — generated in SonarQube Cloud, stored in repo secrets.

**Exclusions (audited).** `sonar.exclusions` covers `gen/**`, `**/third_party/**`, `**/_deps/**`, `proto/**`, `build*/**`, `spike/**`, the generated upb outputs (`**/*.upb.c`, `**/*.upb.h`, `**/*.upb_minitable.*`), and `**/_generated/**`. The build pattern is `build*/**` rather than `build/**` + `build-*/**` so that every build directory the repo's scripts and docs create — `build/`, `build/coverage`, `build-asan/`, `builds/` — is covered; a build tree that escapes it would be scanned as source, and the FetchContent output under `_deps/` is precisely the vendored code these exclusions exist to keep out. `tests/**` is excluded from **coverage** analysis (`sonar.coverage.exclusions`) but deliberately still *analysed* for issues. The most load-bearing patterns are restated inline in the workflow's `args:` as a safety net against a future edit that breaks the `.properties` file.

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

# test-presence, against whatever your branch will be PR'd into
ci/scripts/test-presence.sh origin/master

# ... simulating the [refactor] label
MICROTEL_PR_LABELS='[refactor]' ci/scripts/test-presence.sh origin/master

# coverage build + the aggregate §14.2 gate (10-15 min). Clang only, and
# llvm-profdata / llvm-cov must be at least the compiler's major version.
CC=clang CXX=clang++ ci/scripts/coverage.sh build/coverage

# ... reporting the numbers without failing on a shortfall
CC=clang CXX=clang++ MICROTEL_COVERAGE_ENFORCE=0 \
    ci/scripts/coverage.sh build/coverage

# diff coverage, the way the coverage job runs it
pip install diff-cover
diff-cover build/coverage/coverage.filtered.info \
    --compare-branch origin/master --src-roots . --fail-under=80
```

The `ci/scripts/` directory holds the shared scripts called by both the workflow files and the pre-commit hooks, so local and CI behavior stays in sync.

---

## What's NOT in CI

- **GUI / GUI-test runs.** No GUI in microtel.
- **Performance benchmarks.** Live in the separate `microtel-bench` repo (per spec M7); not part of per-PR CI.
- **macOS / Windows builds.** Out of scope for v1; not in the matrix.
- **End-to-end integration with downstream backends** (Datadog, New Relic). The `tests/conformance/` job runs against an OTel Collector only.
