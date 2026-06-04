#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Update bench/baseline/results.json from a completed benchmark.yml artifact.
#
# Usage:
#   ci/scripts/baseline-update.sh <github-sha>
#
# Requires: gh (GitHub CLI), jq

set -euo pipefail

SHA="${1:?Usage: $0 <github-sha>}"
REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
ARTIFACT_NAME="bench-results-${SHA}"
DEST="bench/baseline/results.json"

echo "Looking for artifact '${ARTIFACT_NAME}' in ${REPO}..."

# Find the run ID that produced this artifact.
RUN_ID=$(gh api "repos/${REPO}/actions/artifacts" \
    --paginate \
    --jq ".artifacts[] | select(.name == \"${ARTIFACT_NAME}\") | .workflow_run.id" \
    | head -1)

if [ -z "${RUN_ID}" ]; then
    echo "ERROR: No artifact '${ARTIFACT_NAME}' found." >&2
    echo "       Check that the benchmark workflow completed for SHA ${SHA}." >&2
    exit 1
fi

echo "Downloading artifact from run ${RUN_ID}..."
TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

gh run download "${RUN_ID}" --name "${ARTIFACT_NAME}" --dir "${TMP_DIR}"

if [ ! -f "${TMP_DIR}/results.json" ]; then
    echo "ERROR: results.json not found in artifact." >&2
    exit 1
fi

# Validate it looks like a results.json before overwriting.
if ! jq -e '.schema_version and (.suts | length > 0)' "${TMP_DIR}/results.json" > /dev/null 2>&1; then
    echo "ERROR: results.json does not look like a valid benchmark document." >&2
    exit 1
fi

cp "${TMP_DIR}/results.json" "${DEST}"
echo "Updated ${DEST} from SHA ${SHA} (run ${RUN_ID})."
echo ""
echo "Review the diff, then:"
echo "  git add ${DEST}"
echo "  git commit -m \"bench: update perf baseline from run ${SHA}\""
