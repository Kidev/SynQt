#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
#
# The local test network: names, loopback addresses and a development web CA, for tests a
# browser must see as two different sites.
#
#   tests/local-network/local-network.sh status     what is in place right now
#   tests/local-network/local-network.sh certs      issue the CA and the server cert
#   tests/local-network/local-network.sh up         aliases + hosts entries + certs (sudo)
#   tests/local-network/local-network.sh down       remove hosts entries and aliases (sudo)
#   tests/local-network/local-network.sh trust      add the CA to the system store (sudo)
#   tests/local-network/local-network.sh untrust    remove it again (sudo)
#   eval "$(tests/local-network/local-network.sh env)"
#
# `certs` and `env` need no privileges. See README.md.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SITES_CONF="$HERE/sites.conf"

# Outside the repository: this directory holds a CA private key.
WORK="${SYNQT_LOCAL_NETWORK_DIR:-$HOME/.cache/synqt-local-network}"

# Every hosts line this script adds carries this marker; `down` removes exactly those.
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
        # Two calls, never `req -x509 -addext`, which can emit an extension twice.
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

    # One server certificate for every site, reissued whenever sites.conf changes.
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

# Elevate only for /etc/hosts, not for SYNQT_HOSTS_FILE in a test.
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
    # /etc/hosts is the only mechanism WebKit honours.
    site_lines | $(elevation_for "$HOSTS_FILE") tee -a "$HOSTS_FILE" > /dev/null
    echo "mapped $(site_names | paste -sd' ' -) in $HOSTS_FILE"
}

remove_hosts() {
    if ! grep -qF "$MARKER" "$HOSTS_FILE" 2>/dev/null; then
        echo "no hosts entries to remove"
        return
    fi
    # Filtered into a temporary file and copied back, never edited in place.
    local filtered
    filtered="$(mktemp)"
    grep -vF "$MARKER" "$HOSTS_FILE" > "$filtered"
    $(elevation_for "$HOSTS_FILE") cp "$filtered" "$HOSTS_FILE"
    rm -f "$filtered"
    echo "removed the entries from $HOSTS_FILE"
}

# addresses

add_aliases() {
    # A no-op on Linux; macOS needs each address past 127.0.0.1 added to lo0.
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
    # Firefox ignores the system store on Linux; Playwright uses ignoreHTTPSErrors instead.
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
        # Resolved through Python, which works on every host.
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
    # Consumed with `eval`, so every harness reads the same paths.
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
