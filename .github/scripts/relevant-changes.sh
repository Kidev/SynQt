#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The SynQt Authors
# SPDX-License-Identifier: Apache-2.0

# Decide whether a run has to do its expensive work, and say so on stdout as `relevant=`.
#
# Usage: relevant-changes.sh <pattern>...
# where a pattern is a path prefix ("src/", "cmake/") or an exact file. The answer is
# `relevant=true` whenever the run cannot prove none of them was touched.

set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <path prefix>..." >&2
    exit 2
fi

say() {
    echo "relevant=$1"
    exit 0
}

case "${GITHUB_EVENT_NAME:-}" in
    workflow_dispatch|schedule) say true ;;
esac

if [ "${GITHUB_EVENT_NAME:-}" = "pull_request" ]; then
    # The merge base, not the recorded base sha, which a merge queue or a rebase can leave unreachable.
    base_ref="origin/${GITHUB_BASE_REF:?}"
    git fetch --quiet --depth=0 origin "${GITHUB_BASE_REF}" 2>/dev/null || true
    base="$(git merge-base "${base_ref}" HEAD 2>/dev/null || true)"
else
    base="${GITHUB_EVENT_BEFORE:-}"
fi

# No usable starting point (a new branch, a shallow clone, a rewritten history), build.
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
