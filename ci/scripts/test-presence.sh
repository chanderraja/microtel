#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# TDD test-presence gate. Fails a pull request that changes production sources
# under `src/` without changing anything under `tests/` in the same PR.
#
# This is the mechanical backing for CLAUDE.md rule 3 and spec §14.2: "any
# change to `src/**/*.{cpp,hpp}` requires a corresponding change to
# `tests/**/*.{cpp,hpp}`". It is a presence check, not a coverage check — it
# proves a test file moved, not that the new line is covered. The diff-coverage
# gate in the `coverage` job is what proves the latter. The two are deliberately
# separate: diff coverage needs a full coverage build (10-15 min), this needs a
# git diff (< 5 sec), and a missing-tests PR should be told so immediately.
#
# Documented exceptions (CLAUDE.md rule 3, CONTRIBUTING.md §"Submitting code
# changes"), and how each is honored here:
#
#   * Pure refactors     — the PR carries the `[refactor]` label. Honored.
#   * Code deletions     — deleted files are excluded from the changed-source
#                          set (`--diff-filter=d`). Removing code does not
#                          require adding a test. Honored.
#   * Comment/format-only — NOT detected automatically. Deciding whether a hunk
#                          is semantically empty needs a structured diff, and a
#                          wrong answer here silently disables the gate. Use the
#                          `[refactor]` label as the manual override instead;
#                          the label is visible on the PR and in its history,
#                          where a silent heuristic would not be.
#
# Usage:  ci/scripts/test-presence.sh [base-ref]     (default: origin/master)
#
# Environment overrides:
#   MICROTEL_TEST_PRESENCE_BASE  base ref/SHA (same as the positional argument)
#   MICROTEL_PR_LABELS           comma- and/or newline-separated PR label names.
#                                CI sets this from the `pull_request` event
#                                context; setting it by hand is also how a local
#                                dry-run simulates a labelled PR.
#   GITHUB_EVENT_PATH            fallback label source — the `pull_request`
#                                event JSON, parsed with jq when available.
#   JQ                           jq binary (default: jq)
#
# Exit codes:
#   0  the rule is satisfied, or a documented exception applies
#   1  src/ changed with no corresponding tests/ change
#   2  the gate could not run (not a git repo, base ref unreachable)

set -euo pipefail

JQ="${JQ:-jq}"

BASE_REF="${1:-${MICROTEL_TEST_PRESENCE_BASE:-origin/master}}"

# The label that waives the rule. Spelled with the brackets, exactly as
# CLAUDE.md and CONTRIBUTING.md name it.
readonly REFACTOR_LABEL="[refactor]"

# Number of changed paths echoed per category before the list is truncated.
readonly MAX_LISTED_PATHS=20

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "test-presence: not a git repository" >&2
    exit 2
fi

if ! git rev-parse --verify --quiet "$BASE_REF^{commit}" >/dev/null; then
    echo "test-presence: base ref '$BASE_REF' is unreachable." >&2
    echo "  In CI this means the checkout was too shallow — the job needs" >&2
    echo "  fetch-depth: 0 (or an explicit fetch of the base branch)." >&2
    exit 2
fi

if ! MERGE_BASE="$(git merge-base "$BASE_REF" HEAD)"; then
    echo "test-presence: no merge base between '$BASE_REF' and HEAD" >&2
    exit 2
fi

echo "test-presence: base = $BASE_REF ($MERGE_BASE)"

# ---------------------------------------------------------------------------
# Changed paths
# ---------------------------------------------------------------------------

# `--diff-filter=d` drops deletions: removing code is a documented exception.
# The extension filter is a grep rather than a git pathspec because git's
# default pathspec matching lets `*` cross `/`, which makes `src/**/*.cpp` and
# `src/*.cpp` mean the same thing and neither one obviously correct.
changed_sources()
{
    local tree="$1"
    git diff --name-only --diff-filter=d "$MERGE_BASE..HEAD" -- "$tree" \
        | grep -E '\.(cpp|hpp)$' || true
}

SRC_CHANGED="$(changed_sources src)"
TESTS_CHANGED="$(changed_sources tests)"

# Truncation is done inside awk rather than with `head`, which would exit early
# and, under `pipefail`, SIGPIPE its producer.
list_paths()
{
    local label="$1"
    local paths="$2"

    if [[ -z "$paths" ]]; then
        echo "  $label: (none)"
        return
    fi

    echo "  $label:"
    printf '%s\n' "$paths" | awk -v limit="$MAX_LISTED_PATHS" '
        NR <= limit { print "    " $0 }
        END { if (NR > limit) { print "    ... and " NR - limit " more" } }'
}

list_paths "changed src/ sources" "$SRC_CHANGED"
list_paths "changed tests/ sources" "$TESTS_CHANGED"

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------

if [[ -z "$SRC_CHANGED" ]]; then
    echo "test-presence: PASS — no src/**/*.{cpp,hpp} changes in this diff."
    exit 0
fi

if [[ -n "$TESTS_CHANGED" ]]; then
    echo "test-presence: PASS — src/ changes are accompanied by tests/ changes."
    exit 0
fi

# ---------------------------------------------------------------------------
# Escape hatch: the `[refactor]` label
# ---------------------------------------------------------------------------

pr_labels()
{
    if [[ -n "${MICROTEL_PR_LABELS:-}" ]]; then
        printf '%s\n' "${MICROTEL_PR_LABELS//,/$'\n'}"
        return
    fi

    if [[ -n "${GITHUB_EVENT_PATH:-}" && -r "${GITHUB_EVENT_PATH}" ]] \
        && command -v "$JQ" >/dev/null 2>&1; then
        "$JQ" -r '.pull_request.labels[]?.name // empty' "$GITHUB_EVENT_PATH"
    fi
}

has_refactor_label()
{
    local label
    while read -r label; do
        # Trim surrounding whitespace, then compare case-insensitively.
        label="$(printf '%s' "$label" | tr -d '[:space:]')"
        if [[ "${label,,}" == "${REFACTOR_LABEL,,}" ]]; then
            return 0
        fi
    done < <(pr_labels)
    return 1
}

if has_refactor_label; then
    echo "test-presence: PASS — PR carries the '$REFACTOR_LABEL' label."
    echo "  The label waives the rule for pure refactors. A reviewer is"
    echo "  expected to confirm the diff really is behaviour-preserving;"
    echo "  mislabelling a real change is a CLAUDE.md violation."
    exit 0
fi

cat >&2 <<EOF

test-presence: FAIL

  This PR changes src/**/*.{cpp,hpp} but changes nothing under
  tests/**/*.{cpp,hpp}. Per spec §14.2 and CLAUDE.md rule 3, production
  changes ship with the tests that exercise them, in the same PR.

  To clear this gate, either:
    * add or update the test that covers the change (the intended path), or
    * apply the '$REFACTOR_LABEL' label if the change is genuinely
      behaviour-preserving — a pure refactor, or a comment/formatting-only
      edit. The label is the manual override for both; this gate does not
      try to detect comment-only diffs on its own.

EOF
exit 1
