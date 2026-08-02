#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
#
# The local test network: names, loopback addresses and a development web CA.
#
# Several things this project has to prove are browser policy questions rather than Qt
# questions (what a split-origin session cookie survives, whether a bundle served from
# one origin can reach an edge on another), and they can only be asked of a browser that
# believes it is talking to two different sites. This is the shared plumbing for that, in
# one place, so a harness that needs it does not grow its own.
#
#   tests/local-network/local-network.sh status     what is in place right now
#   tests/local-network/local-network.sh certs      issue the CA and the server cert
#   tests/local-network/local-network.sh up         aliases + hosts entries + certs (sudo)
#   tests/local-network/local-network.sh down       remove hosts entries and aliases (sudo)
#   tests/local-network/local-network.sh trust      add the CA to the system store (sudo)
#   tests/local-network/local-network.sh untrust    remove it again (sudo)
#   eval "$(tests/local-network/local-network.sh env)"
#
# `certs` and `env` need no privileges; everything else edits the machine and says so.
# See README.md for what each layer buys and which parts have actually been run.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SITES_CONF="$HERE/sites.conf"

# Outside the repository on purpose: this directory holds a certificate authority private
# key, and a key inside a checkout is a key one `git add -A` away from being published.
WORK="${SYNQT_LOCAL_NETWORK_DIR:-$HOME/.cache/synqt-local-network}"

# The marker that makes the hosts edit reversible without a hand-written sed: every line
# this script adds carries it, and `down` removes exactly the lines that carry it.
MARKER="# synqt-local-network"
HOSTS_FILE="${SYNQT_HOSTS_FILE:-/etc/hosts}"

CA_DAYS=825          # the maximum a modern browser will accept for a server certificate
LEAF_DAYS=825

# the site table

site_names() {
    awk '!/^[[:space:]]*#/ && NF { print $1 }' "$SITES_CONF"
}

site_addresses() {
    awk '!/^[[:space:]]*#/ && NF { print $2 }' "$SITES_CONF" | sort -u
}

site_lines() {
    awk -v marker="$MARKER" '!/^[[:space:]]*#/ && NF { print $2 "\t" $1 "  " marker }' \
        "$SITES_CONF"
}

# certificates

issue_certs() {
    mkdir -p "$WORK"
    chmod 700 "$WORK"

    if [ ! -f "$WORK/ca.pem" ]; then
        # Two calls, never `req -x509 -addext`: that form has produced certificates
        # carrying an extension twice, and macOS Secure Transport rejects such an anchor
        # outright as malformed, which reads as "TLS is broken" rather than "the CA is".
        # `x509 -req -extfile` is the authoritative form on every openssl and LibreSSL.
        cat > "$WORK/ca.ext" <<'EOF'
basicConstraints = critical, CA:TRUE, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
EOF
        openssl req -new -newkey rsa:2048 -nodes \
            -keyout "$WORK/ca-key.pem" -out "$WORK/ca.csr" \
            -subj "/CN=SynQt local network development CA" 2>/dev/null
        openssl x509 -req -in "$WORK/ca.csr" -signkey "$WORK/ca-key.pem" \
            -days "$CA_DAYS" -extfile "$WORK/ca.ext" -out "$WORK/ca.pem" 2>/dev/null
        chmod 600 "$WORK/ca-key.pem"
        echo "issued a development web CA at $WORK/ca.pem"
    fi

    # One server certificate covering every site, reissued whenever the table changes, so
    # adding a name to sites.conf is the only step needed to serve it.
    local names present
    names="$(site_names | sed 's/^/DNS:/' | paste -sd, -)"
    present=""
    if [ -f "$WORK/cert.pem" ]; then
        present="$(openssl x509 -in "$WORK/cert.pem" -noout -ext subjectAltName 2>/dev/null \
            | tail -n +2 | tr -d ' \n')"
    fi
    if [ "$present" != "$names" ]; then
        cat > "$WORK/leaf.ext" <<EOF
subjectAltName = $names
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
EOF
        openssl req -new -newkey rsa:2048 -nodes \
            -keyout "$WORK/key.pem" -out "$WORK/leaf.csr" \
            -subj "/CN=$(site_names | head -1)" 2>/dev/null
        openssl x509 -req -in "$WORK/leaf.csr" -CA "$WORK/ca.pem" -CAkey "$WORK/ca-key.pem" \
            -CAcreateserial -days "$LEAF_DAYS" -extfile "$WORK/leaf.ext" \
            -out "$WORK/cert.pem" 2>/dev/null
        chmod 600 "$WORK/key.pem"
        echo "issued $WORK/cert.pem for $names"
    fi
}

# names

# Elevate only where the target actually needs it. /etc/hosts does; the scratch file
# SYNQT_HOSTS_FILE points at in a test does not, and requiring a password to exercise the
# logic is how the logic goes untested.
elevation_for() {
    if [ -w "$1" ]; then
        echo ""
    else
        echo "sudo"
    fi
}

add_hosts() {
    if grep -qF "$MARKER" "$HOSTS_FILE" 2>/dev/null; then
        echo "hosts entries are already present"
        return
    fi
    # /etc/hosts is the only mechanism that reaches every engine. Chromium has
    # --host-resolver-rules and Firefox has network.dns.localDomains, but WebKit has
    # neither, and WebKit is Safari's engine: the one browser whose cookie policy this
    # project cannot afford to guess at.
    site_lines | $(elevation_for "$HOSTS_FILE") tee -a "$HOSTS_FILE" > /dev/null
    echo "mapped $(site_names | paste -sd' ' -) in $HOSTS_FILE"
}

remove_hosts() {
    if ! grep -qF "$MARKER" "$HOSTS_FILE" 2>/dev/null; then
        echo "no hosts entries to remove"
        return
    fi
    # Filtered into a temporary file and copied back, rather than edited in place: an
    # in-place rewrite of /etc/hosts that is interrupted leaves the machine with no
    # loopback name at all.
    local filtered
    filtered="$(mktemp)"
    grep -vF "$MARKER" "$HOSTS_FILE" > "$filtered"
    $(elevation_for "$HOSTS_FILE") cp "$filtered" "$HOSTS_FILE"
    rm -f "$filtered"
    echo "removed the entries from $HOSTS_FILE"
}

# addresses

add_aliases() {
    # Linux treats the whole of 127.0.0.0/8 as local with no configuration, so this is a
    # no-op there and is written for macOS, where every address past 127.0.0.1 has to be
    # added to lo0 by hand and is forgotten on reboot.
    if [ "$(uname -s)" != "Darwin" ]; then
        echo "no aliases needed on $(uname -s): all of 127.0.0.0/8 is already local"
        return
    fi
    local address
    for address in $(site_addresses); do
        if [ "$address" = "127.0.0.1" ]; then
            continue
        fi
        if ifconfig lo0 | grep -qF "inet $address "; then
            echo "lo0 already carries $address"
            continue
        fi
        sudo ifconfig lo0 alias "$address" up
        echo "added $address to lo0"
    done
}

remove_aliases() {
    if [ "$(uname -s)" != "Darwin" ]; then
        echo "no aliases to remove on $(uname -s)"
        return
    fi
    local address
    for address in $(site_addresses); do
        if [ "$address" = "127.0.0.1" ]; then
            continue
        fi
        if ifconfig lo0 | grep -qF "inet $address "; then
            sudo ifconfig lo0 -alias "$address"
            echo "removed $address from lo0"
        fi
    done
}

# trust

trust_ca() {
    issue_certs
    case "$(uname -s)" in
    Linux)
        sudo cp "$WORK/ca.pem" /usr/local/share/ca-certificates/synqt-local-network.crt
        sudo update-ca-certificates > /dev/null
        echo "trusted the CA system-wide (Debian/Ubuntu layout)"
        ;;
    Darwin)
        sudo security add-trusted-cert -d -r trustRoot \
            -k /Library/Keychains/System.keychain "$WORK/ca.pem"
        echo "trusted the CA in the System keychain"
        ;;
    *)
        echo "no system trust step for $(uname -s); import $WORK/ca.pem by hand" >&2
        return 1
        ;;
    esac
    # Firefox keeps its own store and reads no system anchor on Linux, so a system trust
    # step leaves it out. Playwright drives a fresh profile per run and takes
    # `ignoreHTTPSErrors` instead, which is why the harnesses here do not need this; a
    # hand-driven Firefox does.
    echo "note: Firefox has its own certificate store. For a hand-driven Firefox, import"
    echo "      $WORK/ca.pem under Settings, Privacy and Security, Certificates."
}

untrust_ca() {
    case "$(uname -s)" in
    Linux)
        sudo rm -f /usr/local/share/ca-certificates/synqt-local-network.crt
        sudo update-ca-certificates --fresh > /dev/null
        echo "removed the CA from the system store"
        ;;
    Darwin)
        sudo security delete-certificate -c "SynQt local network development CA" \
            /Library/Keychains/System.keychain
        echo "removed the CA from the System keychain"
        ;;
    *)
        echo "no system trust step for $(uname -s)" >&2
        return 1
        ;;
    esac
}

# reporting

status() {
    echo "sites (from $(basename "$SITES_CONF")):"
    local name address resolved
    while read -r name address _; do
        # Resolved through Python rather than getent or dscacheutil: those are the glibc
        # and the macOS answer to the same question, and a status command that dies on the
        # host it was not written for is worse than useless.
        resolved="$(python3 -c 'import socket, sys
try:
    print(socket.gethostbyname(sys.argv[1]))
except OSError:
    pass' "$name" 2>/dev/null || true)"
        if [ -n "$resolved" ]; then
            echo "  $name -> $resolved (wanted $address)"
        else
            echo "  $name -> does not resolve (run: $0 hosts)"
        fi
    done < <(awk '!/^[[:space:]]*#/ && NF' "$SITES_CONF")

    echo "certificates:"
    if [ -f "$WORK/cert.pem" ]; then
        echo "  $WORK/cert.pem"
        openssl x509 -in "$WORK/cert.pem" -noout -enddate -ext subjectAltName \
            | sed 's/^/    /'
    else
        echo "  none issued yet (run: $0 certs)"
    fi
}

print_env() {
    # Consumed with `eval`, so every harness reads the same paths rather than rebuilding
    # them from its own idea of where the work directory is.
    echo "export SYNQT_LOCAL_NETWORK_DIR='$WORK'"
    echo "export SYNQT_LOCAL_NETWORK_CA='$WORK/ca.pem'"
    echo "export SYNQT_LOCAL_NETWORK_CERT='$WORK/cert.pem'"
    echo "export SYNQT_LOCAL_NETWORK_KEY='$WORK/key.pem'"
}

case "${1:-status}" in
status)     status ;;
certs)      issue_certs ;;
hosts)      add_hosts ;;
unhosts)    remove_hosts ;;
aliases)    add_aliases ;;
unaliases)  remove_aliases ;;
trust)      trust_ca ;;
untrust)    untrust_ca ;;
up)         add_aliases; add_hosts; issue_certs ;;
down)       remove_hosts; remove_aliases ;;
env)        print_env ;;
*)
    echo "usage: $0 {status|certs|hosts|unhosts|aliases|unaliases|trust|untrust|up|down|env}" >&2
    exit 2 ;;
esac
