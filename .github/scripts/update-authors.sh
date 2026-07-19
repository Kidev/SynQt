#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The SynQt Authors
# SPDX-License-Identifier: Apache-2.0

# Regenerate AUTHORS from the commit history.
#
# The file is derived, never hand maintained, so it cannot drift from who actually
# contributed. Every author of a commit reachable from HEAD is listed, plus everyone named
# in a Co-authored-by trailer, since a pair-programmed or co-authored commit has only one
# git author but more than one author in the sense that matters here.
#
# Identities are keyed on the email, not the name: the same person committing as "jane" and
# "Jane Doe" is one entry, and the display name kept is the one from their most recent
# commit. Names are then sorted case-insensitively, so the list does not order itself by
# whoever happened to capitalize their name.
#
# Excluded: GitHub's noreply bot addresses and the accounts that automation commits under.
# They are not authors, and this script's own commits would otherwise add one.
#
# Writes AUTHORS in place and exits 0 whether or not anything changed. The caller decides
# what to do about that (see .github/workflows/authors.yml).

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Bot and automation identities, matched against the email. Anchored where possible so a
# real address that merely contains one of these strings is not dropped.
is_bot() {
    case "${1,,}" in
        *"[bot]@users.noreply.github.com") return 0 ;;
        actions@github.com|*"@users.noreply.github.com.invalid") return 0 ;;
        noreply@github.com|github-actions*) return 0 ;;
        *) return 1 ;;
    esac
}

# Collect "name <email>" from commit authors and from Co-authored-by trailers. Oldest first,
# so a later commit overwrites the display name and the newest spelling of a name wins.
collect() {
    git log --reverse --format='%aN <%aE>'
    git log --reverse --format='%(trailers:key=Co-authored-by,valueonly)' \
        | sed '/^[[:space:]]*$/d'
}

declare -A by_email=()

while IFS= read -r line; do
    # "Some Name <addr@example.org>": split on the last "<".
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
refreshed automatically whenever a pull request lands on main. Do not edit it by hand;
contribute and it will list you.

Copyright in each contribution stays with its author. Source files carry the collective
notice "The SynQt Authors", which is this list. See CONTRIBUTING.md and CLA.md.

EOF
    printf '%s\n' "${by_email[@]}" | sort -f
} > AUTHORS

printf 'AUTHORS: %d author(s)\n' "${#by_email[@]}"
