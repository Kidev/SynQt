#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The SynQt Authors
# SPDX-License-Identifier: Apache-2.0

# Decide whether a run has to do its expensive work, and say so on stdout as `relevant=`.
#
# This exists because of one GitHub rule: a workflow skipped by a `paths:` filter on its
# trigger never reports a check at all, so a required status check on that workflow leaves
# every unrelated pull request pending forever. A job skipped by an `if:` condition, on the
# other hand, reports a conclusion of "skipped", and a skipped check satisfies a required
# check. So the path decision has to move off the trigger and into a job.
#
# Usage: relevant-changes.sh <pattern>...
# where a pattern is a path prefix ("src/", "cmake/") or an exact file. The answer is
# `relevant=true` whenever the run cannot prove none of them was touched, which is the safe
# direction: a run that builds when it did not have to costs minutes, and one that skips when
# it should not have costs a merged regression.

set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <path prefix>..." >&2
    exit 2
fi

say() {
    echo "relevant=$1"
    exit 0
}

# A manual run is asking for the work to happen; there is no diff to consult.
case "${GITHUB_EVENT_NAME:-}" in
    workflow_dispatch|schedule) say true ;;
esac

if [ "${GITHUB_EVENT_NAME:-}" = "pull_request" ]; then
    # The base branch, not the base sha: a merge queue or a rebase can leave the recorded sha
    # unreachable, and the merge base of the branch is what the pull request actually changes.
    base_ref="origin/${GITHUB_BASE_REF:?}"
    git fetch --quiet --depth=0 origin "${GITHUB_BASE_REF}" 2>/dev/null || true
    base="$(git merge-base "${base_ref}" HEAD 2>/dev/null || true)"
else
    base="${GITHUB_EVENT_BEFORE:-}"
fi

# No usable starting point: a new branch (the null sha), a shallow clone that does not reach
# back that far, or a rewritten history. Nothing can be ruled out, so build.
if [ -z "${base}" ] ||
   [ "${base}" = "0000000000000000000000000000000000000000" ] ||
   ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
    say true
fi

changed="$(git diff --name-only "${base}" HEAD)"
if [ -z "${changed}" ]; then
    say false
fi

while IFS= read -r file; do
    for pattern in "$@"; do
        case "${pattern}" in
            */) [ "${file#"${pattern}"}" != "${file}" ] && say true ;;
            *)  [ "${file}" = "${pattern}" ] && say true ;;
        esac
    done
done <<< "${changed}"

say false
