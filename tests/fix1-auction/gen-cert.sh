#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Generate throwaway certificates for the auction acceptance test into $1:
#   ca         - the project mesh CA
#   books      - owner entity cert (SAN DNS:books), owns the ledger connect point
#   edge       - the edge's mesh identity (SAN DNS:edge), the only entity allowed to write
#   auditor    - a listed consumer of ledger, but NOT the edge (tests the in-slot
#                Caller.entity check. Connection allowed, recordWinner refused)
#   server.*   - the edge's public TLS server cert (localhost), for the browser wss link,
#                signed by ca. The client pins ca.crt and verifies the edge against it
#
# Profiles from tests/lib/mesh-certs.sh. Throwaway, under build/, never committed.

set -euo pipefail

# shellcheck source=../lib/mesh-certs.sh
. "$(cd "$(dirname "$0")/../lib" && pwd)/mesh-certs.sh"

OUT="${1:?usage: gen-cert.sh <output-dir>}"
mkdir -p "$OUT"
cd "$OUT"

if synqt_certs_current .profile ca.crt books.crt edge.crt auditor.crt server.crt; then
    exit 0
fi

synqt_gen_ca ca
synqt_gen_entity books ca
synqt_gen_entity edge ca
synqt_gen_entity auditor ca

# The edge's public TLS server certificate for the browser link (not a mesh cert).
synqt_gen_edge_cert server ca

chmod 600 ./*.key
synqt_mark_certs .profile
