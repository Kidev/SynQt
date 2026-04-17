#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Run a command against a private, throwaway Secret Service: its own session bus, keyring
# directory and empty-password login keyring.
#
#   tests/lib/keyring-session.sh <command> [args...]
#
# When either tool is missing, it runs the command directly and says so; the store tests
# then skip.

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

# The trailing newline is required: the daemon reads the password as a line.
dbus-run-session -- bash -c '
    set -u
    eval "$(printf "\n" | gnome-keyring-daemon --unlock --components=secrets)"
    export GNOME_KEYRING_CONTROL
    "$@"
' bash "$@"
