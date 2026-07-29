#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Throwaway TLS material for the inbound API suite: a CA and a server certificate for
# localhost and 127.0.0.1, written under the build tree and never committed.

set -euo pipefail

. "$(cd "$(dirname "$0")/../lib" && pwd)/mesh-certs.sh"

OUT="${1:?usage: gen-cert.sh <output-dir>}"
mkdir -p "$OUT"
cd "$OUT"

if synqt_certs_current .profile ca.crt server.crt; then
    exit 0
fi

synqt_gen_ca ca
synqt_gen_edge_cert server ca

chmod 600 ./*.key
synqt_mark_certs .profile
