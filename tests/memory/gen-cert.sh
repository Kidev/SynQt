#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Generate a throwaway localhost TLS server certificate for the memory suite's web edge
# into $1 (ca.crt, server.crt / server.key). A public-link TLS server cert, NOT a mesh CA;
# it lives under build/ (git-ignored) and is never committed.
#
# The edge tests here run over real TLS on purpose: a per-connection allocation the TLS
# path makes is exactly the kind this suite exists to find, and a plaintext edge would not
# make it. The profiles come from tests/lib/mesh-certs.sh, so there is one shape of edge
# certificate in the tree rather than one per suite.

set -euo pipefail

# shellcheck source=../lib/mesh-certs.sh
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
