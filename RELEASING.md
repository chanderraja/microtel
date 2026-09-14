# Releasing microtel

The release procedure as actually practiced, written down from the v1.0.0
release. It is short on purpose: microtel has no release automation, so every
step below is something a person does and something a person can forget.

The mechanical half is [`ci/scripts/version-drift-check.sh`](ci/scripts/version-drift-check.sh),
run on every PR by the `version-drift-check` job. It catches exactly one class of
mistake — a version bumped in some places and not others — which is the mistake
this project has actually made.

---

## 1. Bump the version

`project(microtel VERSION …)` in the top-level `CMakeLists.txt` is the
authority. Four other literals are hand-written and must be edited in the same
commit:

| Location | Literal | Reaches |
|---|---|---|
| [`CMakeLists.txt`](CMakeLists.txt) | `project(microtel VERSION …)` | `PROJECT_VERSION`, and through it `microtelConfigVersion.cmake` — what `find_package(microtel …)` matches against |
| [`include/microtel/version.hpp`](include/microtel/version.hpp) | `kVersionString` | public API |
| [`include/microtel/version.hpp`](include/microtel/version.hpp) | `kVersionMajor` / `kVersionMinor` / `kVersionPatch` | public API |
| [`src/wire/grpc/grpc_wire_codec.cpp`](src/wire/grpc/grpc_wire_codec.cpp) | `kUserAgent` (`"microtel-cpp/<version>"`) | **the wire** — the `user-agent` header on every gRPC export (spec §7.2) |
| [`tools/preflight/preflight.cpp`](tools/preflight/preflight.cpp) | `kVersion` | **the wire** — the `microtel.version` span attribute and the preflight tracer's version (spec §6.4) |

Two of those five have a history worth knowing:

- The gRPC user-agent read `microtel-cpp/0.1.0` from M4 until the 1.0.0 release,
  while `version.hpp` said something else. It now carries a `static_assert`
  against `kVersionString`, so that one pair fails to compile if it drifts.
- The preflight literal has **no** compile-time guard. It has read `"1.0.0"`
  since M6-D — including the whole stretch when the project version was `0.1.0`
  — and became correct by coincidence rather than by being bumped. It was not
  part of the documented bump until this file existed.

### Why generate nothing

Deriving `version.hpp` from `PROJECT_VERSION` at configure time was considered
and rejected. It would turn a public header into a build artifact, and three
consumers read it as a plain source file: the header-only `microtel_headers`
target, the M0 header check, and the install surface (`include/` is installed
verbatim). Changing that to save one hand-edit per release is a bad trade. A
check costs one CI job and changes nothing about what ships.

### Why `master` has no `-dev` suffix

Between releases, `master` stays at the version that was last released — today,
`1.0.0`. It is **not** bumped to `1.1.0-dev` or similar.

The reason is that the version is not only metadata: it goes out on the wire.
`kUserAgent` becomes the gRPC `user-agent` header on every export, and `kVersion`
becomes the `microtel.version` span attribute that preflight writes. A `-dev`
suffix on `master` would mean every span exported from a `master` build — every
developer run, every CI run, every downstream consumer pinning a commit rather
than a tag — arrives at the collector labelled `microtel-cpp/1.1.0-dev`.
Collector-side rules that match on the user-agent would see a version that was
never released, and operators reading `microtel.version` in their traces would
see a version they cannot look up.

The cost of this choice is that a commit on `master` is not distinguishable from
the release tag by version string alone. That is the intended trade: the git SHA
is the right identifier for an unreleased build, and the bug-report template
([`.github/ISSUE_TEMPLATE/bug_report.md`](.github/ISSUE_TEMPLATE/bug_report.md))
already asks for "microtel version (or commit SHA)" for exactly this reason.

The version therefore moves exactly once per release, in the release commit.

### Verify

```bash
ci/scripts/version-drift-check.sh
```

Every literal must agree with `PROJECT_VERSION`. The same script run with
`--self-test` exercises the gate itself against synthetic fixtures; CI runs both.

---

## 2. Package-config compatibility mode

`write_basic_package_version_file(… COMPATIBILITY …)` at the foot of
`CMakeLists.txt` is `SameMajorVersion` from 1.0.0 onward: semver says a
`find_package(microtel 1.0)` requirement is satisfied by any later 1.y. It was
`ExactVersion` below 1.0, where minor bumps are breaking by convention.

Revisit this line only at a **major** bump. A minor or patch release does not
touch it.

---

## 3. Update `SECURITY.md`

[`SECURITY.md`](SECURITY.md) carries a supported-versions matrix. Add or amend
the row for the new release, and demote whatever it supersedes, following the
policy stated below the table (latest minor of the latest major fully supported;
previous major gets security fixes for 18 months per `microtel-roadmap.md` §2).

The v1.0.0 release replaced a "pre-1.0, no version is stable" paragraph with the
table now there. A 1.1.0 release adds a `1.1.x` row and moves `1.0.x` to
whatever the policy says it becomes.

---

## 4. Commit, merge, tag

`master` is protected — a direct push is rejected because a pushed commit has no
passing status checks and `enforce_admins` is on (see
[`docs/branch-protection.md`](docs/branch-protection.md)). So the release commit
goes through a PR like anything else:

```bash
git switch master && git pull
git switch -c chore/vX.Y.Z
# edit the five literals + SECURITY.md
ci/scripts/version-drift-check.sh
git commit -am "chore: vX.Y.Z"
gh pr create --title "chore: vX.Y.Z" --body "…"
# merge once CI is green
```

Then tag the **merge commit on `master`**, not the branch head:

```bash
git switch master && git pull
git tag -a vX.Y.Z -m "microtel vX.Y.Z"
git push origin vX.Y.Z
gh release create vX.Y.Z --title "microtel vX.Y.Z" --notes "…"
```

Tag names are `vX.Y.Z`. Pre-1.0 tags carried a milestone suffix
(`v0.2.0-m2`); that convention ended at 1.0.0.

---

## 5. Refresh the snapshots

Two committed files are point-in-time snapshots of generated output. Neither is
required for the release to be usable, and neither should be folded into the
release PR — both are noisy diffs that would bury the version change.

### Benchmark baseline

[`bench/baseline/results.json`](bench/baseline/results.json) is what the weekly
`benchmark.yml` regression check compares against. Refresh it after a release so
the next cycle's 5% gate measures against the released numbers:

1. Trigger `benchmark.yml` on the reference runner.
2. `ci/scripts/baseline-update.sh <sha>` — it downloads the
   `bench-results-<sha>` artifact, validates the JSON, and overwrites the file.
3. Commit on its own branch, PR, merge.

See [`bench/baseline/README.md`](bench/baseline/README.md) for what is actually
gated today (drop rate; the latency medians are still placeholder zeros).

### graphify snapshot

[`docs/graph-report.md`](docs/graph-report.md) is a committed copy of the
knowledge-graph report. Per `CLAUDE.md`:

```bash
graphify update . --force        # --force: the rebuild has fewer nodes
cp graphify-out/GRAPH_REPORT.md docs/graph-report.md
git switch -c docs/graph-snapshot-vX-Y-Z
git commit -am "docs(graph): refresh snapshot at vX.Y.Z"
gh pr create …                   # merge once green; master is protected
```

`.graphifyignore` already excludes `docs/graph-report.md` so the snapshot does
not self-index.

---

## Checklist

```
[ ] CMakeLists.txt project(… VERSION …)
[ ] include/microtel/version.hpp  kVersionString
[ ] include/microtel/version.hpp  kVersionMajor / kVersionMinor / kVersionPatch
[ ] src/wire/grpc/grpc_wire_codec.cpp  kUserAgent
[ ] tools/preflight/preflight.cpp  kVersion
[ ] ci/scripts/version-drift-check.sh passes locally
[ ] COMPATIBILITY mode still right (major bumps only)
[ ] SECURITY.md supported-versions row
[ ] release PR merged with CI green
[ ] annotated tag vX.Y.Z on the master merge commit, pushed
[ ] gh release created
[ ] bench/baseline/results.json refreshed (own PR)
[ ] docs/graph-report.md refreshed (own docs/ PR)
```
