#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Generate throwaway certificates for the mesh-transport benchmark into $1:
#   ca     - the project private CA
#   alpha  - owner entity cert (SAN DNS:alpha), signed by ca
#   beta   - consumer entity cert (SAN DNS:beta), signed by ca
#
# TEST certificates only. They live under build/ (git-ignored) and are never committed; no
# production mesh CA key is created here (see docs/security.md).

set -euo pipefail

OUT="${1:?usage: gen-certs.sh <output-dir>}"
mkdir -p "$OUT"
cd "$OUT"

# Idempotent: skip if a still-valid CA already exists.
if [ -f ca.crt ] && openssl x509 -checkend 3600 -noout -in ca.crt >/dev/null 2>&1; then
    exit 0
fi

DAYS=5

gen_ca() { # name
    openssl genrsa -out "$1.key" 2048 >/dev/null 2>&1
    openssl req -x509 -new -nodes -key "$1.key" -sha256 -days "$DAYS" \
        -subj "/CN=$1" -out "$1.crt" >/dev/null 2>&1
}

gen_entity() { # name signing-ca
    openssl genrsa -out "$1.key" 2048 >/dev/null 2>&1
    openssl req -new -key "$1.key" -subj "/CN=$1" -out "$1.csr" >/dev/null 2>&1
    openssl x509 -req -in "$1.csr" -CA "$2.crt" -CAkey "$2.key" -CAcreateserial \
        -days "$DAYS" -sha256 \
        -extfile <(printf "subjectAltName=DNS:%s" "$1") \
        -out "$1.crt" >/dev/null 2>&1
    rm -f "$1.csr"
}

gen_ca ca
gen_entity alpha ca
gen_entity beta ca

chmod 600 ./*.key
