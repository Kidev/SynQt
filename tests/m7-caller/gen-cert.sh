#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Generate throwaway certificates for the M7 three-entity authorization test into $1:
#   ca         - the project mesh CA
#   database   - owner entity cert (SAN DNS:database)
#   web        - the edge's mesh identity (SAN DNS:web), a listed consumer of items
#   reporter   - a listed consumer of items, but NOT the edge (tests the in-slot
#                Caller.entity check: connection allowed, insert refused)
#   other      - a CA-signed entity that is NOT a consumer (tests deny-by-default)
#   server.*   - the edge's public TLS server cert (localhost), for the browser wss link,
#                signed by ca: the client pins ca.crt and verifies the edge against it
#
# The profiles come from tests/lib/mesh-certs.sh, which mirrors what `synqt mesh cert`
# issues. TEST certificates only, under build/ (git-ignored), never committed; no
# production mesh CA key is created here (see docs/security.md).

set -euo pipefail

# shellcheck source=../lib/mesh-certs.sh
. "$(cd "$(dirname "$0")/../lib" && pwd)/mesh-certs.sh"

OUT="${1:?usage: gen-cert.sh <output-dir>}"
mkdir -p "$OUT"
cd "$OUT"

if synqt_certs_current .profile ca.crt database.crt web.crt reporter.crt other.crt \
                       server.crt; then
    exit 0
fi

synqt_gen_ca ca
synqt_gen_entity database ca
synqt_gen_entity web ca
synqt_gen_entity reporter ca
synqt_gen_entity other ca

# The edge's public TLS server certificate for the browser link (not a mesh cert).
synqt_gen_edge_cert server ca

chmod 600 ./*.key
synqt_mark_certs .profile
