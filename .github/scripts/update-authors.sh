#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The SynQt Authors
# SPDX-License-Identifier: Apache-2.0

# Regenerate AUTHORS from the commit history. Every commit author reachable from HEAD plus
# every Co-authored-by trailer, keyed on the email, with the display name from the most
# recent commit, sorted case-insensitively. Bot and automation identities are excluded.
# Writes AUTHORS in place and exits 0 whether or not anything changed.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

is_bot() {
    case "${1,,}" in
        *"[bot]@users.noreply.github.com") return 0 ;;
        actions@github.com|*"@users.noreply.github.com.invalid") return 0 ;;
        noreply@github.com|github-actions*) return 0 ;;
        *) return 1 ;;
    esac
}

# Oldest first, so the newest spelling of a name wins.
collect() {
    git log --reverse --format='%aN <%aE>'
    git log --reverse --format='%(trailers:key=Co-authored-by,valueonly)' \
        | sed '/^[[:space:]]*$/d'
}

declare -A by_email=()

while IFS= read -r line; do
    name="${line%% <*}"
    email="${line##*<}"
    email="${email%>}"
    [ -n "$email" ] || continue
    [ -n "$name" ] || continue
    if is_bot "$email"; then
        continue
    fi
    by_email["${email,,}"]="$name <$email>"
done < <(collect)

{
    cat <<'EOF'
The SynQt Authors
=================

Everyone who has contributed to SynQt, in alphabetical order.

This file is generated from the commit history by .github/scripts/update-authors.sh and
refreshed automatically whenever a pull request lands on main. Do not edit it by hand.
Contribute and it will list you.

Copyright in each contribution stays with its author. Source files carry the collective
notice "The SynQt Authors", which is this list. See CONTRIBUTING.md and CLA.md.

EOF
    printf '%s\n' "${by_email[@]}" | sort -f
} > AUTHORS

printf 'AUTHORS: %d author(s)\n' "${#by_email[@]}"
