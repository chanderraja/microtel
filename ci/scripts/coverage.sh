#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Aggregate coverage gate. Configures + builds + runs the test suite under
# clang source-based coverage, exports an lcov tracefile with llvm-cov, and
# fails if any coverage group is below its spec §14.2 floor.
#
# This is spec §13.5 release gate 11 ("test coverage thresholds met"). The
# per-PR diff-coverage half of §14.2 is a separate CI step (`diff-cover`
# against the same filtered tracefile); this script owns the aggregate half.
#
# ---------------------------------------------------------------------------
# Why clang source-based coverage (issue #198)
# ---------------------------------------------------------------------------
#
# This gate used to build with gcc `--coverage` and capture with `lcov`. Its
# line numbers were sound, but its branch numbers were not: gcov records an
# edge for the unwind path out of every potentially-throwing call, so
# include/microtel/meter.hpp measured 100% line and 50% branch with its
# "uncovered" branches sitting on lines like
#
#     return DoCreateCounterI64(std::move(name), std::move(description), ...);
#
# which hold no conditional at all. Whole-tree branch coverage read 57.8%
# against 91.0% line for that reason alone, and the §14.2 branch floor had to
# ship measured-but-not-enforced.
#
# Clang's source-based coverage attaches counters to source *regions* the front
# end knows about. A branch record corresponds to a conditional someone wrote,
# so the branch floor is now enforced alongside the line floors. The cost is
# that this gate is clang-only: the compiler is checked below rather than left
# to produce a confusing failure three steps later.
#
# ---------------------------------------------------------------------------
# Why the exporter must be llvm-cov 21 or newer (issue #236)
# ---------------------------------------------------------------------------
#
# DO NOT "simplify" LLVM_COV back to the compiler's own version. It changes the
# number the gate reads, and not for the better.
#
# Up to llvm-cov 20, `export` emits one set of branch records per *template
# instantiation*, while emitting **line** records already summed across
# instantiations. The gate then compares a merged line percentage against a
# per-instantiation branch percentage, which spec §14.2 states as if the two
# were commensurable. Where an instantiation is never entered, the older
# exporter emits `BRDA:…,-` records that are pure denominator.
#
# llvm-cov 21 applies the same instantiation merge to branches that every
# version already applies to lines. Bisected on one clang-19 object and
# profile, exported four ways — a function template with one `if`, instantiated
# twice, both arms taken in both instantiations:
#
#     llvm-cov 19 -> 4 BRDA records on the templated `if`  (per-instantiation)
#     llvm-cov 20 -> 4 BRDA records                        (per-instantiation)
#     llvm-cov 21 -> 2 BRDA records                        (merged)
#     llvm-cov 22 -> 2 BRDA records                        (merged)
#
# Same bytes in, different aggregation out. On this tree it is the difference
# between sdk-encoder reading 81.48% (1219/1496) and 85.59% (1099/1284) branch,
# from *identical* line coverage of 4757/5189. The gate's job is to compare
# like with like, so the minimum below is a correctness requirement, not a
# preference. The compiler stays at whatever CI pins (clang-18); newer llvm
# tools read older coverage-mapping and profile formats, so nothing about the
# built code changes.
#
# ---------------------------------------------------------------------------
# Group mapping
# ---------------------------------------------------------------------------
#
# §14.2 states the thresholds against two named areas — "SDK and encoder code"
# and "transport and exporter paths" — without enumerating directories. The
# mapping below is this script's reading of that wording. It is stated here
# rather than buried in the code because the mapping, not the percentage, is
# where a coverage gate is usually gamed.
#
#   sdk-encoder          ≥ 90% line, ≥ 85% branch
#     include/microtel/**    public API + internal interface headers: inline
#                            code on the SDK's own surface
#     src/api/**             API-layer implementation
#     src/sdk/**             the SDK proper
#     src/common/**          config loading, env resolution, validation, the
#                            log sink, and the RAII handle wrappers. All of it
#                            is in-process logic reachable from a unit test
#                            with no socket, so it is held to the SDK floor
#                            rather than the transport one.
#     src/wire/encoder/**    the upb OTLP encoder — the "encoder" of §14.2
#     leaf/**                the C leaf: span building and its OTLP encoder
#                            backends (docs/leaf-concentrator-design.md);
#                            measured when MICROTEL_BUILD_LEAF is on
#
#   transport-exporter   ≥ 80% line
#     src/transport/**       epoll reactor, HTTP/2 + TLS transport
#     src/exporter/**        OTLP exporters and retry policy
#     src/wire/** (rest)     http/, grpc/, otlp_response, gzip — wire framing
#                            and response parsing on the export path
#     src/adapters/**        optional third-party adapters
#     tools/**               microtel-preflight, which drives the export path
#                            end to end and is shipped alongside the library
#
# A path matching neither list lands in sdk-encoder — the stricter of the two —
# and is named in the report as unclassified. A new directory therefore cannot
# dodge the gate by not being mentioned; it gets the strict floor until someone
# classifies it deliberately.
#
# Nothing in the filtered tracefile is exempt. There is no "ungated" bucket.
#
# ---------------------------------------------------------------------------
#
# Usage:  ci/scripts/coverage.sh [build-dir]     (default: build/coverage)
#
# Environment overrides:
#   CC / CXX                   compiler to build with; must be clang
#   LLVM_PROFDATA / LLVM_COV   llvm tools. llvm-profdata must be exactly $CXX's
#                              major version; llvm-cov must be at least that
#                              and at least 21. Unset, they are looked up by
#                              name. See the version note above.
#   MICROTEL_COVERAGE_ENFORCE  1 (default) fails on a shortfall; 0 reports it
#                              and exits 0, for local use and for the Sonar
#                              job, which wants the tracefile rather than a
#                              second opinion on the floors
#   COVERAGE_EXTRA_CMAKE_ARGS  extra configure arguments, word-split — CI uses
#                              it to turn on the optional glog / log4cxx
#                              bridges, whose headers live under
#                              include/microtel/ and so are gated here
#
# Exit codes:
#   0  every threshold met (or MICROTEL_COVERAGE_ENFORCE=0)
#   1  a group is below its floor
#   2  the gate could not run (wrong compiler, missing or too-old llvm tool,
#      no profile data, or a tracefile with no records)

set -euo pipefail

BUILD_DIR="${1:-build/coverage}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# spec §14.2. Percentages, as integers.
readonly SDK_LINE_MIN=90
readonly SDK_BRANCH_MIN=85
readonly TRANSPORT_LINE_MIN=80

# How many of the lowest-covered files to name per group when reporting.
readonly WORST_FILE_COUNT=8

# Files with fewer instrumented lines than this are too small for their
# percentage to be informative, so they are not listed as "worst" offenders.
# They still count in full toward the group totals.
readonly WORST_FILE_MIN_LINES=20

# Code microtel does not own, dropped from the gated tracefile. This is not a
# place to put a microtel file whose coverage is inconvenient: every path that
# survives it is gated by one group or the other.
readonly IGNORE_REGEX='(^/usr/|/third_party/|/gen/|/_deps/|/tests/|/ci/)'

# First llvm-cov that merges branch regions across template instantiations, so
# that the branch percentage is drawn from the same population as the line
# percentage. See the header — this is the #236 decision, not a preference.
readonly MIN_COV_MAJOR=21

ENFORCE="${MICROTEL_COVERAGE_ENFORCE:-1}"

CXX_BIN="${CXX:-clang++}"

# ---------------------------------------------------------------------------
# Toolchain resolution
# ---------------------------------------------------------------------------
#
# Two different version rules, because two different formats are in play.
#
#   llvm-profdata  must match the compiler exactly. It reads the *raw* profiles
#                  the instrumented binaries write, and that format is locked to
#                  the compiler's release in both directions: clang-18 writes
#                  raw version 9, and llvm-profdata-21 refuses it with "raw
#                  profile version mismatch … expected version = 10", then
#                  "no profile can be merged".
#
#   llvm-cov       must be at least the compiler's version and at least
#                  MIN_COV_MAJOR. It reads the *indexed* profile llvm-profdata
#                  wrote plus the coverage mapping in the objects, and both of
#                  those a newer reader accepts.
#
# So the pairing CI uses — clang-18 + llvm-profdata-18 + llvm-cov-21 — is not an
# oversight. Each half is pinned for its own reason, and both are checked below
# rather than left to surface as a corrupt-looking file three steps later.

die()
{
    echo "coverage: $*" >&2
    exit 2
}

# Major version of an LLVM-family binary, or empty if it is not one — including
# when it is missing or cannot load its shared libraries. The `|| true` matters:
# under `pipefail` this pipeline's status is the tool's, and without it a broken
# tool would abort the script through `set -e` with a bare exit code instead of
# reaching the diagnostics below.
llvm_major()
{
    { "$1" --version 2>/dev/null || true; } \
        | sed -n 's/.*[Vv]ersion \([0-9][0-9]*\)\..*/\1/p' \
        | head -1
}

if ! "$CXX_BIN" --version 2>/dev/null | head -1 | grep -qi clang; then
    die "CXX='$CXX_BIN' is not clang. This gate measures coverage with clang" \
        "source-based instrumentation (issue #198); set CC/CXX to clang."
fi

CLANG_MAJOR="$(llvm_major "$CXX_BIN")"
[[ -n "$CLANG_MAJOR" ]] || die "cannot read a version out of '$CXX_BIN'."

# Try the caller's preferred major first, then the compiler's, then the bare
# name, so a host with several LLVM releases installed does not silently reach
# for the wrong one.
resolve_llvm_tool()
{
    local tool="$1"
    local preferred="$2"
    local candidate
    for candidate in "$tool-$preferred" "$tool-$CLANG_MAJOR" "$tool"; do
        if command -v "$candidate" > /dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

PROFDATA_BIN="${LLVM_PROFDATA:-$(resolve_llvm_tool llvm-profdata "$CLANG_MAJOR" || true)}"
COV_BIN="${LLVM_COV:-$(resolve_llvm_tool llvm-cov "$MIN_COV_MAJOR" || true)}"

[[ -n "$PROFDATA_BIN" ]] || die "no llvm-profdata-$CLANG_MAJOR or llvm-profdata on PATH."
[[ -n "$COV_BIN" ]] || die "no llvm-cov-$MIN_COV_MAJOR or llvm-cov on PATH."

PROFDATA_MAJOR="$(llvm_major "$PROFDATA_BIN")"
COV_MAJOR="$(llvm_major "$COV_BIN")"
[[ -n "$PROFDATA_MAJOR" ]] || die "cannot read a version out of '$PROFDATA_BIN'."
[[ -n "$COV_MAJOR" ]] || die "cannot read a version out of '$COV_BIN'."

# Raw profiles are version-locked to the compiler, newer readers included.
if (( PROFDATA_MAJOR != CLANG_MAJOR )); then
    die "'$PROFDATA_BIN' is llvm-profdata $PROFDATA_MAJOR but '$CXX_BIN' is" \
        "clang $CLANG_MAJOR. The raw profile format is tied to the compiler's" \
        "release in both directions; use llvm-profdata-$CLANG_MAJOR."
fi

if (( COV_MAJOR < CLANG_MAJOR )); then
    die "'$COV_BIN' is llvm-cov $COV_MAJOR but '$CXX_BIN' is clang" \
        "$CLANG_MAJOR; it cannot read the coverage mapping clang will write."
fi

# The exporter carries its own, higher floor. Below it, branch records are
# emitted per template instantiation while line records are not, and the gate
# would be comparing two different populations (issue #236).
if (( COV_MAJOR < MIN_COV_MAJOR )); then
    die "'$COV_BIN' is llvm-cov $COV_MAJOR; this gate needs" \
        "$MIN_COV_MAJOR or newer, which merges branch regions across template" \
        "instantiations the way line regions already are. See this script's" \
        "header and issue #236 before changing this."
fi

echo "coverage: build dir = $BUILD_DIR"
echo "coverage: clang $CLANG_MAJOR, $PROFDATA_BIN, $COV_BIN (llvm-cov $COV_MAJOR)"

# ---------------------------------------------------------------------------
# Build + run
# ---------------------------------------------------------------------------

# Word-splitting COVERAGE_EXTRA_CMAKE_ARGS is the point: it carries several -D flags.
# shellcheck disable=SC2086
cmake -S . -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_STANDARD=20 \
    -DMICROTEL_BUILD_HEADER_CHECK=ON \
    -DMICROTEL_BUILD_TESTS=ON \
    -DMICROTEL_COVERAGE=ON \
    ${COVERAGE_EXTRA_CMAKE_ARGS:-}

cmake --build "$BUILD_DIR" -j

# One raw profile per process. %p keeps concurrent ctest workers from writing
# over each other and %m keeps two different binaries apart.
#
# Absolute, and it has to be: ctest runs each test with its own directory in
# the build tree as the working directory, so a relative LLVM_PROFILE_FILE is
# resolved once per test against a different directory. The profile runtime
# creates the missing parents and writes there without complaining, so the
# symptom is not an error — it is an empty $BUILD_DIR/profraw and a gate that
# would have measured nothing.
PROFRAW_DIR="$(cd "$BUILD_DIR" && pwd)/profraw"
rm -rf "$PROFRAW_DIR"
mkdir -p "$PROFRAW_DIR"

LLVM_PROFILE_FILE="$PROFRAW_DIR/%p-%m.profraw" \
    ctest --test-dir "$BUILD_DIR" --output-on-failure

# ---------------------------------------------------------------------------
# Merge + export
# ---------------------------------------------------------------------------

PROFDATA_FILE="$BUILD_DIR/coverage.profdata"
LCOV_FILE="$BUILD_DIR/coverage.info"
LCOV_FILTERED="$BUILD_DIR/coverage.filtered.info"
PER_FILE="$BUILD_DIR/coverage.per-file.tsv"

mapfile -t PROFRAWS < <(find "$PROFRAW_DIR" -type f -name '*.profraw' | sort)
if (( ${#PROFRAWS[@]} == 0 )); then
    die "no .profraw files under '$PROFRAW_DIR' — the tests ran without" \
        "instrumentation, or LLVM_PROFILE_FILE did not reach them."
fi
echo "coverage: merging ${#PROFRAWS[@]} raw profile(s)"

"$PROFDATA_BIN" merge -sparse -o "$PROFDATA_FILE" "${PROFRAWS[@]}"

# Every object carrying a coverage mapping is handed to llvm-cov, not just the
# test binaries. A translation unit compiled into a library that no test links
# has genuinely-zero coverage; if it were left out of the export instead it
# would vanish from the denominator and *raise* the measured percentage. This
# is the property `lcov --capture --directory` had for free by reading every
# .gcno in the build tree, and it is the one a coverage gate cannot lose.
#
# Object files rather than archives or executables: each TU produces exactly
# one, so no function record is presented to llvm-cov twice.
#
# llvm-cov prints "N functions have mismatched data" against this object set.
# Those are inline functions emitted into several translation units with
# different structural hashes, not lost coverage: the object-file set, the
# test-executable set and the two together all export the same file count and
# the same totals.
mapfile -t OBJECTS < <(
    find "$BUILD_DIR" -type f -name '*.o' -not -path '*/_deps/*' | sort
)
if (( ${#OBJECTS[@]} == 0 )); then
    die "no object files under '$BUILD_DIR' — nothing to export coverage for."
fi
echo "coverage: exporting from ${#OBJECTS[@]} object file(s)"

# llvm-cov takes the first object positionally and the rest behind -object.
COV_OBJECT_ARGS=("${OBJECTS[0]}")
for object in "${OBJECTS[@]:1}"; do
    COV_OBJECT_ARGS+=(-object "$object")
done

"$COV_BIN" export \
    -format=lcov \
    -instr-profile "$PROFDATA_FILE" \
    "${COV_OBJECT_ARGS[@]}" \
    > "$LCOV_FILE"

"$COV_BIN" export \
    -format=lcov \
    -instr-profile "$PROFDATA_FILE" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    "${COV_OBJECT_ARGS[@]}" \
    > "$LCOV_FILTERED"

# ---------------------------------------------------------------------------
# Aggregate threshold gate
# ---------------------------------------------------------------------------

if [[ ! -s "$LCOV_FILTERED" ]]; then
    echo "coverage: '$LCOV_FILTERED' is missing or empty — nothing to gate." >&2
    exit 2
fi

# Pass 1: reduce each tracefile record to one row
#
#   <group>  <repo-relative path>  <lines found> <lines hit> <br found> <br hit>
#
# Counting DA:/BRDA: records directly rather than trusting the LF:/LH:/BRF:/BRH:
# summary lines keeps this independent of which tool wrote the file. A BRDA
# taken-count of "-" means the enclosing block never executed, which is an
# untaken branch.
#
# Everything here is POSIX awk: CI runners resolve `awk` to mawk, which has no
# gawk array extensions.
awk -v root="$REPO_ROOT/" '
    # Anchored on "start of string or a slash" rather than "start of string"
    # so classification survives a tracefile whose SF: paths did not get the
    # repo-root prefix stripped (a symlinked checkout, a different builder).
    # The filter above has already removed everything microtel does not own, so
    # a mid-path match cannot pull in a vendored directory.
    function group_of(p)
    {
        if (p ~ /(^|\/)src\/wire\/encoder\//) { return "sdk-encoder" }
        if (p ~ /(^|\/)leaf\//) { return "sdk-encoder" }
        if (p ~ /(^|\/)src\/(api|sdk|common)\//) { return "sdk-encoder" }
        if (p ~ /(^|\/)include\/microtel\//) { return "sdk-encoder" }
        if (p ~ /(^|\/)src\/(transport|exporter|adapters|wire)\//)
        {
            return "transport-exporter"
        }
        if (p ~ /(^|\/)tools\//) { return "transport-exporter" }
        return "unclassified"
    }

    BEGIN { OFS = "\t" }

    /^SF:/ {
        path = substr($0, 4)
        if (index(path, root) == 1)
        {
            path = substr(path, length(root) + 1)
        }
        lf = 0; lh = 0; brf = 0; brh = 0
        next
    }

    /^DA:/ {
        split(substr($0, 4), d, ",")
        lf++
        if (d[2] + 0 > 0) { lh++ }
        next
    }

    /^BRDA:/ {
        split(substr($0, 6), b, ",")
        brf++
        if (b[4] != "-" && b[4] + 0 > 0) { brh++ }
        next
    }

    /^end_of_record/ { print group_of(path), path, lf, lh, brf, brh }
' "$LCOV_FILTERED" > "$PER_FILE"

if [[ ! -s "$PER_FILE" ]]; then
    echo "coverage: tracefile '$LCOV_FILTERED' contains no records." >&2
    exit 2
fi

# Lowest-covered files in a group, largest shortfall first. Sorting in `sort`
# rather than awk keeps pass 2 portable.
#
# The last stage is an awk that reads its whole input rather than a `head` that
# exits early: under `pipefail`, an early exit can SIGPIPE `sort` and make this
# informational helper abort the script.
worst_files()
{
    local group="$1"

    awk \
        -F'\t' \
        -v group="$group" \
        -v min_lines="$WORST_FILE_MIN_LINES" '
        $1 == group && $3 >= min_lines {
            printf "%09.4f\t%6.2f%%  %5d/%-5d  %s\n",
                $4 / $3, 100 * $4 / $3, $4, $3, $2
        }' "$PER_FILE" \
        | sort \
        | awk -F'\t' -v limit="$WORST_FILE_COUNT" '
            NR <= limit { print "      " $2 }
            END { if (NR == 0) { print "      (nothing to report)" } }'
}

# Pass 2: group totals, the report table, and the verdict. awk's exit status
# is the gate's verdict — 0 pass, 1 shortfall.
#
# `unclassified` rows are folded into sdk-encoder: a path matching no rule gets
# the stricter floor rather than a free pass. They are also listed by name, so
# a new directory shows up as something to classify rather than staying silent.
set +e
awk \
    -F'\t' \
    -v sdk_line_min="$SDK_LINE_MIN" \
    -v sdk_branch_min="$SDK_BRANCH_MIN" \
    -v transport_line_min="$TRANSPORT_LINE_MIN" \
    -v enforce="$ENFORCE" '
    # A group with nothing instrumented reads as 100%: vacuously covered. It
    # cannot be a way to pass with no data — unclassified paths fold into
    # sdk-encoder, and an empty tracefile exits 2 before this runs — and the
    # printed (0/0) makes the emptiness visible either way.
    function pct(hit, found)
    {
        return found > 0 ? 100 * hit / found : 100
    }

    # Percentages are compared with a half-hundredth of slack so that a group
    # printed as "90.00%" is never failed by a rounding artefact.
    function meets(value, floor_pct)
    {
        return value + 0.005 >= floor_pct
    }

    function verdict(value, floor_pct)
    {
        return meets(value, floor_pct) ? "PASS" : "FAIL"
    }

    function report_line(group, floor_pct,    p)
    {
        p = pct(lh[group], lf[group])
        printf "    line    %6.2f%%  (%5d/%5d)   floor %d%%   %s\n",
            p, lh[group], lf[group], floor_pct, verdict(p, floor_pct)
        if (!meets(p, floor_pct)) { failures = failures " " group "/line" }
    }

    function report_branch(group, floor_pct,    p)
    {
        p = pct(brh[group], brf[group])
        if (floor_pct == 0)
        {
            printf "    branch  %6.2f%%  (%5d/%5d)   no floor in §14.2\n",
                p, brh[group], brf[group]
            return
        }
        printf "    branch  %6.2f%%  (%5d/%5d)   floor %d%%   %s\n",
            p, brh[group], brf[group], floor_pct, verdict(p, floor_pct)
        if (!meets(p, floor_pct)) { failures = failures " " group "/branch" }
    }

    {
        group = ($1 == "unclassified") ? "sdk-encoder" : $1
        if ($1 == "unclassified") { unclassified_count++ }
        lf[group] += $3; lh[group] += $4; brf[group] += $5; brh[group] += $6
        lf["ALL"] += $3; lh["ALL"] += $4; brf["ALL"] += $5; brh["ALL"] += $6
    }

    END {
        print ""
        print "=== Aggregate coverage vs spec §14.2 ==="

        print ""
        print "  sdk-encoder  (include/microtel, src/api, src/sdk, src/common, src/wire/encoder, leaf)"
        report_line("sdk-encoder", sdk_line_min)
        report_branch("sdk-encoder", sdk_branch_min)

        print ""
        print "  transport-exporter  (src/transport, src/exporter, src/adapters, rest of src/wire, tools)"
        report_line("transport-exporter", transport_line_min)
        report_branch("transport-exporter", 0)

        print ""
        print "  ALL  (every microtel-owned file surviving the filter)"
        printf "    line    %6.2f%%  (%5d/%5d)\n",
            pct(lh["ALL"], lf["ALL"]), lh["ALL"], lf["ALL"]
        printf "    branch  %6.2f%%  (%5d/%5d)\n",
            pct(brh["ALL"], brf["ALL"]), brh["ALL"], brf["ALL"]

        if (unclassified_count > 0)
        {
            printf "\n  %d path(s) matched no group rule and were gated as sdk-encoder.\n",
                unclassified_count
        }

        if (failures == "")
        {
            print ""
            print "coverage: PASS — every threshold met."
            exit 0
        }

        print ""
        print "coverage: SHORTFALL —" failures
        if (enforce + 0 != 1)
        {
            print "coverage: MICROTEL_COVERAGE_ENFORCE=0, reporting only."
            exit 0
        }
        print "coverage: raise coverage on the files listed above. Do not lower a"
        print "coverage: threshold and do not widen the ignore filter."
        exit 1
    }
' "$PER_FILE"
GATE_STATUS=$?
set -e

echo
echo "=== Lowest line coverage, sdk-encoder ==="
worst_files sdk-encoder
echo
echo "=== Lowest line coverage, transport-exporter ==="
worst_files transport-exporter

UNCLASSIFIED="$(awk -F'\t' '$1 == "unclassified" { print "    " $2 }' "$PER_FILE")"
if [[ -n "$UNCLASSIFIED" ]]; then
    echo
    echo "=== Unclassified paths (gated as sdk-encoder) ==="
    printf '%s\n' "$UNCLASSIFIED"
    echo "  Give them a group in this script's header mapping."
fi

echo
echo "Full report: $LCOV_FILE"
echo "Filtered:    $LCOV_FILTERED"
echo "Per file:    $PER_FILE"

exit "$GATE_STATUS"
