#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Generate a throwaway localhost TLS server certificate for the M6 client test into $1
# (ca.crt, server.crt / server.key). This is a public-link TLS server cert (like a real
# edge's), NOT a mesh CA; it lives under build/ (git-ignored) and is never committed.
#
# The profiles come from tests/lib/mesh-certs.sh. The client pins ca.crt and verifies the
# edge against it, which is the shape a real deployment has; the certificate it verifies
# is a CA-issued leaf, not its own trust anchor.

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
