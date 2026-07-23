#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Generate the throwaway public TLS server certificate for the FIX-3 stall acceptance test
# into $1 (ca.crt, server.crt / server.key, CN=localhost). The stall test drives the edge
# directly with its Pages connect point; no mesh entity certificates are needed here, since
# the acceptance test does not stand up the mesh. The CA is the anchor the client pins to
# verify the edge.
#
# The profiles come from tests/lib/mesh-certs.sh. TEST certificates only, under build/
# (git-ignored), never committed.

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
