#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Decides whether a pull request changes anything the build, tests or static
# analysis can see. The `changes` job in .github/workflows/ci.yml runs it and
# the heavy jobs (compile, sanitizers, clang-tidy, coverage, conformance, …)
# skip their work when it prints `code=false`, so a docs-only PR finishes in
# minutes instead of an hour. Their required checks still report: a skipped
# job counts as passing, and the matrix jobs run with every step skipped.
#
# Docs-only means every changed path is one of:
#   *.md            Markdown anywhere, except under .github/
#   docs/**         prose, images, bench results
#   tools/diagrams/** the scripts that draw docs/images/*.svg
#
# Anything else, any path under .github/, an empty diff, a non-PR event or a
# git failure prints `code=true`: when in doubt, run everything.
#
# Usage:
#   ci/scripts/classify-change.sh <base-sha> <head-sha>   (PR)
#   ci/scripts/classify-change.sh                         (push etc.: code=true)
#   ci/scripts/classify-change.sh --self-test
#
# Prints `code=true|false` on stdout and, when GITHUB_OUTPUT is set, appends it
# there too. Exit status is 0 unless the arguments are malformed.

set -euo pipefail

# is_docs_path <path> → 0 if the path is documentation only.
is_docs_path()
{
    local path="$1"
    case "$path" in
        .github/*) return 1 ;;
        docs/* | tools/diagrams/* | *.md) return 0 ;;
        *) return 1 ;;
    esac
}

# classify <newline-separated paths> → prints true (code) or false (docs only).
classify()
{
    local paths="$1"
    local path

    if [[ -z "$paths" ]]; then
        echo true
        return
    fi
    while IFS= read -r path; do
        [[ -z "$path" ]] && continue
        if ! is_docs_path "$path"; then
            echo true
            return
        fi
    done <<< "$paths"
    echo false
}

emit()
{
    local value="$1"
    echo "code=$value"
    if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
        echo "code=$value" >> "$GITHUB_OUTPUT"
    fi
}

self_test()
{
    local failed=0
    check()
    {
        local expected="$1" description="$2" paths="$3" got
        got="$(classify "$paths")"
        if [[ "$got" == "$expected" ]]; then
            printf '  ok    %-5s %s\n' "$got" "$description"
        else
            printf '  FAIL  got %s, want %s: %s\n' "$got" "$expected" "$description"
            failed=1
        fi
    }
    echo "classify-change: self-test"
    check false "README only" "README.md"
    check false "docs, an SVG and a nested README" $'docs/troubleshooting.md\ndocs/images/a.svg\nexamples/logs/README.md'
    check false "diagram generator" "tools/diagrams/usage.py"
    check true  "a source file" "src/sdk/sdk_tracer.cpp"
    check true  "docs plus one CMakeLists.txt" $'docs/README.md\nsrc/transport/CMakeLists.txt'
    check true  "a workflow" ".github/workflows/ci.yml"
    check true  "Markdown under .github" ".github/PULL_REQUEST_TEMPLATE.md"
    check true  "a CI script" "ci/scripts/symbol-scan.sh"
    check true  "an example's code" "examples/logs/main.cpp"
    check true  "a collector config" "examples/stack/collector-config.yaml"
    check true  "empty diff" ""
    if [[ "$failed" -ne 0 ]]; then
        echo "classify-change: self-test FAILED" >&2
        return 1
    fi
    echo "classify-change: self-test passed"
}

main()
{
    if [[ "${1:-}" == "--self-test" ]]; then
        self_test
        return
    fi
    if [[ $# -eq 0 ]]; then
        emit true
        return
    fi
    if [[ $# -ne 2 ]]; then
        echo "usage: $0 [<base-sha> <head-sha> | --self-test]" >&2
        return 2
    fi

    local paths
    if ! paths="$(git diff --name-only "$1...$2" 2>/dev/null)"; then
        echo "classify-change: git diff $1...$2 failed; running everything" >&2
        emit true
        return
    fi
    echo "classify-change: $(printf '%s\n' "$paths" | grep -c . || true) changed path(s)" >&2
    emit "$(classify "$paths")"
}

main "$@"
