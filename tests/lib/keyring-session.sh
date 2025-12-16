#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Run a command against a real Secret Service, on a machine that has none logged in.
#
#   tests/lib/keyring-session.sh <command> [args...]
#
# The Linux half of docs/desktop.md#storing-the-session is libsecret talking to
# org.freedesktop.secrets, and neither a CI runner nor a container has that: no session bus,
# no unlocked collection, so every store test skips and the backend that ships to visitors is
# never exercised anywhere. This is what gives it one, and it is the same command
# in CI and on a workstation.
#
# Everything it makes is private and thrown away. Its own session bus, its own keyring
# directory, and a login keyring created there with an empty password. That is not only
# hygiene, it is what makes the run reproducible. A developer's real keyring is already
# unlocked and already has a default collection, so a test that used it would pass here and
# fail on a fresh machine for reasons nobody could see, and a suite that writes a credential
# has no business writing it into the keyring somebody keeps their life in.
#
# When either tool is missing it runs the command directly and says so. The store is then
# unavailable, the tests that need one skip, and that is the right outcome for such a
# machine rather than a failure. It is exactly what a visitor there gets, which is a sign-in
# per launch.

set -u

if [ "$#" -eq 0 ]; then
    echo "usage: keyring-session.sh <command> [args...]" >&2
    exit 2
fi

if ! command -v dbus-run-session >/dev/null 2>&1 \
        || ! command -v gnome-keyring-daemon >/dev/null 2>&1; then
    echo "-- no private keyring available (dbus-run-session or gnome-keyring-daemon is" \
         "missing); running without one, so store tests will skip"
    exec "$@"
fi

root="$(mktemp -d "${TMPDIR:-/tmp}/synqt-keyring-XXXXXX")"
trap 'rm -rf "$root"' EXIT

export XDG_DATA_HOME="$root/data"
export XDG_CONFIG_HOME="$root/config"
export XDG_CACHE_HOME="$root/cache"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

# The trailing newline is load-bearing. The daemon reads the password as a line, so an empty
# write with no newline leaves it waiting on stdin, and the run ends with a keyring service
# answering on the bus and no collection in it -- which is worse than no service at all,
# because then the store reports itself available and fails every write.
dbus-run-session -- bash -c '
    set -u
    eval "$(printf "\n" | gnome-keyring-daemon --unlock --components=secrets)"
    export GNOME_KEYRING_CONTROL
    "$@"
' bash "$@"
