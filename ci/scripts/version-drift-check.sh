#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Version-coherence gate. The release version is spelled by hand in several
# places; this fails a PR in which they stop agreeing.
#
# `project(microtel VERSION …)` in the top-level CMakeLists.txt is the
# authority. Every other literal is compared against it:
#
#   include/microtel/version.hpp      kVersionString              (public API)
#   include/microtel/version.hpp      kVersionMajor/Minor/Patch   (public API)
#   src/wire/grpc/grpc_wire_codec.cpp kUserAgent                  (on the wire)
#   tools/preflight/preflight.cpp     kVersion                    (on the wire)
#
# This is a CHECK, not a generator. Deriving version.hpp from PROJECT_VERSION at
# configure time was considered and rejected: it would make a public header a
# build artifact, which the header-only `microtel_headers` target, the M0 header
# check and the install surface all consume as a plain source file. A check costs
# one CI job and changes nothing about what ships. See RELEASING.md.
#
# The gRPC user-agent additionally carries a `static_assert` against
# kVersionString, so that one pair is already caught at compile time. It is
# re-checked here anyway: the static_assert dies with the literal it guards, and
# a gate that lists every location is the one a release engineer can read.
#
# The preflight literal has *no* compile-time guard, and is the reason this gate
# covers more than the CMake/header pair: it read "1.0.0" from M6-D onward while
# the project version was still 0.1.0, and became correct only by coincidence
# when 1.0.0 shipped. It reaches collectors as the `microtel.version` span
# attribute (spec §6.4).
#
# Usage:
#   ci/scripts/version-drift-check.sh [repo-root]   (default: git toplevel)
#   ci/scripts/version-drift-check.sh --self-test   (exercise the gate itself)
#
# Exit codes:
#   0  every literal agrees with PROJECT_VERSION
#   1  drift — at least one literal disagrees
#   2  the gate could not run (file missing, literal no longer matched)

set -euo pipefail

# Number of literals that disagreed with PROJECT_VERSION. Reset by run_check.
FAILURES=0

# A well-formed semantic version, as both CMake and the C++ literals spell it.
readonly VERSION_RE='^[0-9]+\.[0-9]+\.[0-9]+$'

# One sed script per literal. Each is addressed to a `constexpr` line so that
# prose mentioning the identifier in a comment cannot be mistaken for it.
readonly SED_VERSION_STRING='/constexpr/ s/.*kVersionString[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p'
readonly SED_VERSION_MAJOR='/constexpr/ s/.*kVersionMajor[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'
readonly SED_VERSION_MINOR='/constexpr/ s/.*kVersionMinor[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'
readonly SED_VERSION_PATCH='/constexpr/ s/.*kVersionPatch[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p'
readonly SED_GRPC_USER_AGENT='/constexpr/ s/.*kUserAgent[[:space:]]*=[[:space:]]*"microtel-cpp\/\([^"]*\)".*/\1/p'
readonly SED_PREFLIGHT_VERSION='/constexpr/ s/.*kVersion[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p'

readonly PATH_VERSION_HPP='include/microtel/version.hpp'
readonly PATH_GRPC_CODEC='src/wire/grpc/grpc_wire_codec.cpp'
readonly PATH_PREFLIGHT='tools/preflight/preflight.cpp'

# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

# extract_one <file> <sed script> <label>
#
# Prints the single value the sed script captures. Zero matches and more than
# one match are both hard failures (exit 2), not silent passes: a gate that
# matches nothing reports green for the wrong reason, and is exactly how this
# check would rot after an unrelated rename.
extract_one()
{
    local file="$1"
    local script="$2"
    local label="$3"
    local matches
    local count

    if [[ ! -r "$file" ]]; then
        echo "version-drift-check: $label: '$file' is missing or unreadable" >&2
        return 2
    fi

    matches="$(sed -n "$script" "$file")"
    count="$(printf '%s' "$matches" | grep -c . || true)"

    if [[ "$count" -eq 0 ]]; then
        echo "version-drift-check: $label: no literal matched in $file" >&2
        echo "  The declaration moved or was reworded. Update the pattern in this" >&2
        echo "  script — do not delete the check." >&2
        return 2
    fi

    if [[ "$count" -ne 1 ]]; then
        echo "version-drift-check: $label: $count literals matched in $file" >&2
        printf '%s\n' "$matches" | sed 's/^/    /' >&2
        echo "  A version literal must be spelled exactly once per file." >&2
        return 2
    fi

    printf '%s\n' "$matches"
}

# cmake_project_version <CMakeLists.txt>
#
# Prints the VERSION argument of the top-level `project()` call. The scan is
# bounded to that call so the unrelated `cmake_minimum_required(VERSION …)`
# above it cannot be picked up.
cmake_project_version()
{
    local file="$1"
    local value

    if [[ ! -r "$file" ]]; then
        echo "version-drift-check: '$file' is missing or unreadable" >&2
        return 2
    fi

    if ! value="$(awk '
        /^[[:space:]]*project[[:space:]]*\(/ { in_project = 1 }
        in_project && $1 == "VERSION"        { print $2; found = 1; exit }
        in_project && /\)/                   { exit }
        END                                  { if (!found) { exit 1 } }
    ' "$file")"; then
        echo "version-drift-check: no VERSION argument in the project() call of $file" >&2
        return 2
    fi

    if ! printf '%s' "$value" | grep -Eq "$VERSION_RE"; then
        echo "version-drift-check: PROJECT_VERSION '$value' is not MAJOR.MINOR.PATCH" >&2
        return 2
    fi

    printf '%s\n' "$value"
}

# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

# report <status> <location> <found> <expected> <note>
#
# Records the verdict for one literal. Drift increments FAILURES; it never
# aborts, so a single run lists every location that disagrees rather than only
# the first.
report()
{
    local location="$1"
    local found="$2"
    local expected="$3"
    local note="$4"

    if [[ "$found" == "$expected" ]]; then
        printf '  ok     %-34s %-12s %s\n' "$location" "$found" "$note"
        return 0
    fi

    printf '  DRIFT  %-34s %-12s %s\n' "$location" "$found" "$note" >&2
    FAILURES=$((FAILURES + 1))
}

# check_literal <root> <relative path> <sed script> <expected> <note>
check_literal()
{
    local root="$1"
    local rel="$2"
    local script="$3"
    local expected="$4"
    local note="$5"
    local found

    found="$(extract_one "$root/$rel" "$script" "$rel")" || return 2
    report "$rel" "$found" "$expected" "$note"
}

# check_component_triple <root> <expected>
#
# kVersionMajor/Minor/Patch are a second spelling of kVersionString inside the
# same header, so they can drift from it without either drifting from CMake.
check_component_triple()
{
    local root="$1"
    local expected="$2"
    local major minor patch

    major="$(extract_one "$root/$PATH_VERSION_HPP" "$SED_VERSION_MAJOR" "kVersionMajor")" || return 2
    minor="$(extract_one "$root/$PATH_VERSION_HPP" "$SED_VERSION_MINOR" "kVersionMinor")" || return 2
    patch="$(extract_one "$root/$PATH_VERSION_HPP" "$SED_VERSION_PATCH" "kVersionPatch")" || return 2

    report "$PATH_VERSION_HPP" "$major.$minor.$patch" "$expected" "kVersionMajor/Minor/Patch"
}

# ---------------------------------------------------------------------------
# The gate
# ---------------------------------------------------------------------------

# run_check <repo root>
run_check()
{
    local root="$1"
    local expected

    FAILURES=0

    expected="$(cmake_project_version "$root/CMakeLists.txt")" || return 2

    echo "version-drift-check: PROJECT_VERSION = $expected (CMakeLists.txt)"

    check_literal "$root" "$PATH_VERSION_HPP" "$SED_VERSION_STRING" \
        "$expected" "kVersionString" || return 2
    check_component_triple "$root" "$expected" || return 2
    check_literal "$root" "$PATH_GRPC_CODEC" "$SED_GRPC_USER_AGENT" \
        "$expected" "gRPC user-agent (also static_assert'd)" || return 2
    check_literal "$root" "$PATH_PREFLIGHT" "$SED_PREFLIGHT_VERSION" \
        "$expected" "preflight microtel.version attribute" || return 2

    if [[ "$FAILURES" -ne 0 ]]; then
        cat >&2 <<EOF

version-drift-check: FAIL — $FAILURES location(s) disagree with PROJECT_VERSION.

  A release bump edits every location listed above in one commit. The procedure
  is in RELEASING.md. If you are deliberately changing the version, change all
  of them; if you are not, one of them was edited by accident.

EOF
        return 1
    fi

    echo "version-drift-check: clean — every version literal agrees with PROJECT_VERSION"
    return 0
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

# write_fixture <dir> <cmake ver> <hpp string ver> <hpp triple> <ua ver> <preflight ver>
#
# A minimal tree with the same shape as the repository. `<hpp triple>` is spelled
# as "MAJOR MINOR PATCH" so the triple can be drifted independently of
# kVersionString.
write_fixture()
{
    local dir="$1"
    local cmake_ver="$2"
    local hpp_ver="$3"
    local triple="$4"
    local ua_ver="$5"
    local preflight_ver="$6"
    # shellcheck disable=SC2086
    set -- $triple
    local major="$1" minor="$2" patch="$3"

    mkdir -p "$dir/include/microtel" "$dir/src/wire/grpc" "$dir/tools/preflight"

    cat > "$dir/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)

project(microtel
        VERSION $cmake_ver
        LANGUAGES C CXX
        DESCRIPTION "fixture")
EOF

    cat > "$dir/include/microtel/version.hpp" <<EOF
// A release bump edits this header; see RELEASING.md and kVersionString below.
inline constexpr std::uint32_t kVersionMajor = $major;
inline constexpr std::uint32_t kVersionMinor = $minor;
inline constexpr std::uint32_t kVersionPatch = $patch;
inline constexpr std::string_view kVersionString = "$hpp_ver";
EOF

    cat > "$dir/src/wire/grpc/grpc_wire_codec.cpp" <<EOF
constexpr std::string_view kUserAgentPrefix = "microtel-cpp/";
constexpr std::string_view kUserAgent = "microtel-cpp/$ua_ver";
EOF

    cat > "$dir/tools/preflight/preflight.cpp" <<EOF
constexpr std::string_view kVersion = "$preflight_ver";
EOF
}

# expect_status <expected> <description> <fixture dir>
expect_status()
{
    local expected="$1"
    local description="$2"
    local dir="$3"
    local actual=0

    # A subshell so a `return 2` path and the FAILURES counter cannot leak into
    # the next case.
    (run_check "$dir") >/dev/null 2>&1 || actual=$?

    if [[ "$actual" -eq "$expected" ]]; then
        printf '  ok    exit %d  %s\n' "$actual" "$description"
        return 0
    fi

    printf '  FAIL  exit %d, expected %d  %s\n' "$actual" "$expected" "$description" >&2
    return 1
}

self_test()
{
    local tmp
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" EXIT

    echo "version-drift-check: self-test"

    local failed=0

    write_fixture "$tmp/coherent" "1.2.3" "1.2.3" "1 2 3" "1.2.3" "1.2.3"
    expect_status 0 "all five literals agree" "$tmp/coherent" || failed=1

    write_fixture "$tmp/cmake-drift" "1.2.4" "1.2.3" "1 2 3" "1.2.3" "1.2.3"
    expect_status 1 "CMake bumped, headers left behind" "$tmp/cmake-drift" || failed=1

    write_fixture "$tmp/hpp-drift" "1.2.3" "1.2.4" "1 2 3" "1.2.3" "1.2.3"
    expect_status 1 "kVersionString drifts from PROJECT_VERSION" "$tmp/hpp-drift" || failed=1

    write_fixture "$tmp/triple-drift" "1.2.3" "1.2.3" "1 2 4" "1.2.3" "1.2.3"
    expect_status 1 "kVersionMajor/Minor/Patch drift from kVersionString" "$tmp/triple-drift" || failed=1

    write_fixture "$tmp/ua-drift" "1.2.3" "1.2.3" "1 2 3" "0.1.0" "1.2.3"
    expect_status 1 "gRPC user-agent left at an old version" "$tmp/ua-drift" || failed=1

    write_fixture "$tmp/preflight-drift" "1.2.3" "1.2.3" "1 2 3" "1.2.3" "1.0.0"
    expect_status 1 "preflight microtel.version left at an old version" "$tmp/preflight-drift" || failed=1

    write_fixture "$tmp/missing" "1.2.3" "1.2.3" "1 2 3" "1.2.3" "1.2.3"
    : > "$tmp/missing/include/microtel/version.hpp"
    expect_status 2 "literal no longer present — gate fails rather than passes" "$tmp/missing" || failed=1

    write_fixture "$tmp/duplicate" "1.2.3" "1.2.3" "1 2 3" "1.2.3" "1.2.3"
    echo 'constexpr std::string_view kVersion = "9.9.9";' >> "$tmp/duplicate/tools/preflight/preflight.cpp"
    expect_status 2 "literal spelled twice — ambiguous, so the gate fails" "$tmp/duplicate"  || failed=1

    write_fixture "$tmp/no-project" "1.2.3" "1.2.3" "1 2 3" "1.2.3" "1.2.3"
    echo 'cmake_minimum_required(VERSION 3.20)' > "$tmp/no-project/CMakeLists.txt"
    expect_status 2 "no project() VERSION — not mistaken for cmake_minimum_required" "$tmp/no-project" || failed=1

    if [[ "$failed" -ne 0 ]]; then
        echo "version-drift-check: self-test FAILED" >&2
        return 1
    fi

    echo "version-drift-check: self-test passed"
    return 0
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

main()
{
    if [[ "${1:-}" == "--self-test" ]]; then
        self_test
        return
    fi

    local root="${1:-}"
    if [[ -z "$root" ]]; then
        root="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
    fi

    run_check "$root"
}

main "$@"
